#include "PUMEngine.hh"
#include "MLCPUMManager.hh"

#include "debug/LLCStreamPUM.hh"

#define DEBUG_TYPE LLCStreamPUM
#include "../../stream_log.hh"

PUMEngine::PUMEngine(LLCStreamEngine *_se)
    : se(_se), controller(_se->controller) {}

void PUMEngine::receiveKick(const RequestMsg &msg) {
  assert(this->pumManager && "Not configured yet.");
  if (this->acked) {
    // We are waiting for the kick after sync.
    this->synced();
  } else {
    // We just recieved the first kick after configuration.
    if (this->nextCmdIdx != 0) {
      LLC_SE_PANIC("[PUM] RecvConfig with NextCmdIdx %d != 0.", nextCmdIdx);
    }
    this->receivedConfig = true;
    this->startedRound++;
    this->kickNextCommand();
  }
}

bool PUMEngine::hasCompletedRound(int64_t pumContextId, int rounds) const {
  assert(this->pumContextId == pumContextId);
  return this->completedRound >= rounds;
}

bool PUMEngine::hasStartedRound(int64_t pumContextId, int rounds) const {
  assert(this->pumContextId == pumContextId);
  return this->startedRound >= rounds;
}

void PUMEngine::setPUMManager(MLCPUMManager *pumManager) {
  // We need to know the PUMManager for PUMPrefetchStream.
  if (this->pumManager) {
    assert(this->pumManager == pumManager);
  }
  this->pumManager = pumManager;
}

void PUMEngine::configure(MLCPUMManager *pumManager, int64_t pumContextId,
                          const PUMCommandVecT &commands) {

  // Initialize HWConfig.
  if (!this->hwConfig) {
    this->hwConfig =
        m5::make_unique<PUMHWConfiguration>(StreamNUCAMap::getPUMHWConfig());
  }

  if (pumContextId != this->pumContextId) {
    // Only clear this when we are haveing a new context.
    this->completedRound = -1;
    this->startedRound = -1;
  }
  LLC_SE_DPRINTF("[PUMEngine] Configured CompletedRound %d StartedRound %d.\n",
                 this->completedRound, this->startedRound);

  if ((this->commands.empty() && this->nextCmdIdx != 0) ||
      (!this->commands.empty() &&
       this->nextCmdIdx + 1 != this->commands.size())) {
    // Ignore the last sync command, which will never complete.
    LLC_SE_PANIC("Not done with previous commands. NextCmdIdx %d Commands %d.",
                 this->nextCmdIdx, this->commands.size());
  }

  this->setPUMManager(pumManager);
  this->pumContextId = pumContextId;
  this->nextCmdIdx = 0;
  this->sentPUMDataPkts = 0;
  this->recvDataPkts = 0;
  this->sentInterBankPacketMap.clear();
  this->recvPUMDataPktMap.clear();
  this->recvStreamDataPktMap.clear();
  this->acked = false;
  this->commands.clear();
  this->receivedConfig = false;

  /**
   * Filter out unrelated commands and merge continuous one.
   */
  auto myBankIdx = this->getBankIdx();
  for (int i = 0; i < commands.size(); ++i) {
    const auto &command = commands[i];
    if (command.type == "sync") {
      // Sync command is always related.
      this->commands.push_back(command);
      continue;
    }
    assert(command.llcSplitTileCmds.dimension > 0);
    if (command.llcSplitTileCmds.getBankSubRegionCount(myBankIdx) == 0) {
      continue;
    }
    auto c = commands[i];
    for (auto j = 0; j < c.llcSplitTileCmds.NumBanks; ++j) {
      if (j != myBankIdx) {
        c.llcSplitTileCmds.clearBankSubRegion(j);
      }
    }
    this->commands.push_back(c);
  }

  if (Debug::LLCStreamPUM) {
    LLC_SE_DPRINTF("[PUMEngine]   Configured with CMD %lu.\n",
                   this->commands.size());
    for (int i = 0; i < this->commands.size(); ++i) {
      LLC_SE_DPRINTF("[PUMEngine]   CMD %ld %s.", i,
                     this->commands.at(i).to_string(myBankIdx));
    }
  }
}

void PUMEngine::kickNextCommand() {

  auto myBankIdx = this->getBankIdx();
  Cycles latency(1);

  /**
   * As an optimization, we schedule future commands if they are using different
   * arrays. NOTE: This may assume too much schedule flexibility.
   */
  std::unordered_set<int64_t> scheduledArrays;
  auto firstSchedCmdIdx = this->nextCmdIdx;
  while (this->nextCmdIdx < this->commands.size()) {
    const auto &command = this->commands.at(this->nextCmdIdx);

    if (command.type == "sync") {
      // Sync command is handled in wakeup.
      break;
    }

    if (this->nextCmdIdx > firstSchedCmdIdx) {
      const auto &firstSchedCmd = this->commands.at(firstSchedCmdIdx);
      if (firstSchedCmd.type != command.type ||
          firstSchedCmd.opClass != command.opClass) {
        // Cannot schedule different type commands.
        break;
      }
    }

    auto numCmds = command.llcSplitTileCmds.getBankSubRegionCount(myBankIdx);
    assert(numCmds > 0 && "Empty LLC command.\n");

    std::unordered_set<int64_t> usedArrays;
    for (int j = 0; j < numCmds; ++j) {
      auto mask = command.llcSplitTileCmds.getAffinePattern(myBankIdx, j);
      for (auto arrayIdx : mask.generate_all_values()) {
        usedArrays.insert(arrayIdx);
      }
    }
    bool conflicted = false;
    for (auto arrayIdx : usedArrays) {
      if (scheduledArrays.count(arrayIdx)) {
        LLC_SE_DPRINTF("  Conflict Array %lld NextCmd %s", arrayIdx, command);
        conflicted = true;
        break;
      }
    }
    if (conflicted) {
      break;
    }

    LLC_SE_DPRINTF("[Kick] NextCmd %s", command.to_string(myBankIdx));
    this->pumManager->reportProgress(this->pumContextId);

    /**
     * Estimate the latency of each command.
     */
    scheduledArrays.insert(usedArrays.begin(), usedArrays.end());
    auto cmdLat = this->estimateCommandLatency(command);

    /**
     * Record the number of bitline ops we have done for compute cmd.
     */
    if (command.type == "cmp" && command.opClass != No_OpClass) {
      auto bitlineOps =
          scheduledArrays.size() * command.bitline_mask.getTotalTrip();
      this->controller->m_statPUMComputeCmds++;
      this->controller->m_statPUMComputeOps += bitlineOps;
    }

    this->nextCmdIdx++;
    LLC_SE_DPRINTF("  CMD Latency %lld NextCmdIdx %ld.\n", cmdLat,
                   this->nextCmdIdx);
    if (cmdLat > latency) {
      latency = cmdLat;
    }
  }

  auto commandType = this->commands.at(firstSchedCmdIdx).type;
  if (commandType == "cmp") {
    this->controller->m_statPUMComputeCycles += latency;
  } else if (commandType == "intra-array" || commandType == "inter-array") {
    this->controller->m_statPUMDataMoveCycles += latency;
  }

  LLC_SE_DPRINTF("Schedule Next Tick after Latency %ld.\n", latency);
  this->nextCmdReadyCycle = this->controller->curCycle() + latency;
  this->se->scheduleEvent(latency);
}

void PUMEngine::sendPUMDataToLLC(const DynStreamSliceId &sliceId,
                                 const NetDest &recvBanks, int bytes,
                                 bool isPUMPrefetch) {
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = 0;
  msg->m_Type = CoherenceRequestType_STREAM_PUM_DATA;
  msg->m_Requestors.add(this->controller->getMachineID());
  msg->m_Destination = recvBanks;
  msg->m_MessageSize = this->controller->getMessageSizeType(bytes);
  if (sliceId.isValid()) {
    // This is a PUMData from normal stream.
    msg->m_sliceIds.add(sliceId);
    LLC_SLICE_DPRINTF(sliceId, "[PUMEngine] Send PUMData -> %s.\n", recvBanks);
  } else {
    LLC_SE_DPRINTF("[PUMEngine] Send Inter-Bank Data -> %s.\n", recvBanks);
  }
  if (isPUMPrefetch) {
    assert(sliceId.isValid() && "Should come from PUMPrefetchStream.");
    msg->m_isPUMPrefetch = true;
  }

  this->se->streamIndirectIssueMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(Cycles(1)));
}

Cycles PUMEngine::estimateCommandLatency(const PUMCommand &command) {

  if (command.type == "intra-array") {
    /**
     * Intra array is easy:
     * 1. If enabled parallel intra-array shift, just charge one cycle for each
     * wordline.
     * 2. Otherwise, charge one cycle for each bitline shifted.
     *
     */
    if (this->controller->myParams
            ->stream_pum_enable_parallel_intra_array_shift) {
      return Cycles(command.wordline_bits);
    } else {
      return Cycles(command.wordline_bits * std::abs(command.bitline_dist));
    }
  }

  if (command.type == "inter-array") {
    /**
     * Inter array is complicated:
     * For each level:
     * 1. Get the number of bitlines per array.
     * 2. Get the number of arrays need to be transfered in that level.
     * 3. Estimate the latency.
     *
     * Special case is the last level, which is inter-llc-bank traffic.
     * We need to construct packet and send out the fake data.
     * Then we record how many packets are sent out to monitor when they all
     * arrived.
     */
    auto myBankIdx = this->getBankIdx();

    auto numBankSubRegionCount =
        command.llcSplitTileCmds.getBankSubRegionCount(myBankIdx);
    assert(numBankSubRegionCount > 0 && "Empty LLC Inter-Array command.");
    /**
     * We should really handle each TileMask separately. However, here I just
     * sum all of them.
     */
    int64_t totalTiles = 0;
    for (auto i = 0; i < numBankSubRegionCount; ++i) {
      const auto &tileMask =
          command.llcSplitTileCmds.getAffinePattern(myBankIdx, 0);
      totalTiles += tileMask.getTotalTrip();
    }

    auto llcTreeLeafBandwidthBits = this->hwConfig->tree_leaf_bw_bytes * 8;
    auto bitlinesPerArray = command.bitline_mask.getTotalTrip();
    auto latencyPerWordline =
        (bitlinesPerArray + llcTreeLeafBandwidthBits - 1) /
        llcTreeLeafBandwidthBits;
    auto latencyPerArray = latencyPerWordline * command.wordline_bits;

    std::vector<std::pair<int, int>> interBankBitlineTraffic;

    auto accumulatedLatency = 0;
    auto numLevels = command.inter_array_splits.size();
    auto numSubTreeNodes = hwConfig->tree_degree;
    for (int level = 0; level < numLevels;
         ++level, numSubTreeNodes *= hwConfig->tree_degree) {
      int64_t levelArrays = 0;
      for (const auto &splitPattern : command.inter_array_splits[level]) {
        // TODO: Intersect with LLC array masks.
        if (level + 1 == numLevels) {
          /**
           * This is the last inter-bank level.
           * Notice that PUMEngine is placed at bank level, here we only
           * care about the first trip.
           */
          auto numInterBankTiles =
              std::min(splitPattern.getTrips().front(), totalTiles);
          levelArrays += numInterBankTiles;

          auto srcArrayIdx = this->hwConfig->get_array_per_bank() * myBankIdx +
                             splitPattern.start;
          auto dstArrayIdx = srcArrayIdx + command.tile_dist;
          if (dstArrayIdx < 0) {
            dstArrayIdx += this->hwConfig->get_total_arrays();
          } else if (dstArrayIdx >= this->hwConfig->get_total_arrays()) {
            dstArrayIdx = dstArrayIdx % this->hwConfig->get_total_arrays();
          }
          auto dstBankIdx =
              this->hwConfig->get_bank_idx_from_array_idx(dstArrayIdx);

          auto numInterBankBitlines = numInterBankTiles * bitlinesPerArray;
          interBankBitlineTraffic.emplace_back(dstBankIdx,
                                               numInterBankBitlines);

          LLC_SE_DPRINTF("Bank %d -> %d Array %d -> %d Bitlines %d.\n",
                         myBankIdx, dstBankIdx, srcArrayIdx, dstArrayIdx,
                         numInterBankBitlines);

        } else {
          /**
           * Still intra-bank level.
           * 1. If enabled parallel inter-array shift, we charge how many arrays
           * shifted in each sub-tree.
           * 2. Otherwise, we need to perform shift in each sub-tree
           * sequentially.
           */
          auto shiftedArrays =
              std::min(splitPattern.getTotalTrip(), totalTiles);
          if (this->controller->myParams
                  ->stream_pum_enable_parallel_inter_array_shift) {
            levelArrays += shiftedArrays;
          } else {
            if (level + 2 == numLevels) {
              // This is the inter-way level. Can always shift in parallel.
              levelArrays += shiftedArrays;
            } else {
              // Intra-way inter-array level.
              auto numLeafNodes = this->hwConfig->array_per_way;
              assert(numSubTreeNodes <= numLeafNodes);
              assert(numLeafNodes % numSubTreeNodes == 0);
              auto levelSubTrees = numLeafNodes / numSubTreeNodes;
              levelArrays += shiftedArrays * levelSubTrees;
            }
          }
        }
      }
      accumulatedLatency += levelArrays * latencyPerArray;
      LLC_SE_DPRINTF("InterArray Level %d Arrays %d AccLat +%d -> %d.\n", level,
                     levelArrays, levelArrays * latencyPerArray,
                     accumulatedLatency);
    }

    // Packetize the last inter-bank level (assume 64B data packet).
    const auto packetDataBits = 512;
    for (const auto &entry : interBankBitlineTraffic) {
      auto dstBankIdx = entry.first;
      auto bitlines = entry.second;
      auto totalBits = bitlines * command.wordline_bits;

      NetDest dstBanks;
      if (command.hasReuse()) {
        /**
         * Hack: when there is reuse, just send to all the DstBank.
         * TODO: Properly handle this.
         */
        assert(!command.llcSplitDstTileCmds[myBankIdx].empty());

        const auto &dstSplitBanks = command.llcSplitDstTileCmds[myBankIdx][0];

        for (auto dstBankIdx = 0; dstBankIdx < dstSplitBanks.size();
             ++dstBankIdx) {
          if (!dstSplitBanks.at(dstBankIdx)) {
            continue;
          }
          MachineID dstMachineId(MachineType_L2Cache, dstBankIdx);
          dstBanks.add(dstMachineId);
        }

      } else {
        MachineID dstMachineId(MachineType_L2Cache, dstBankIdx);
        dstBanks.add(dstMachineId);
      }

      for (auto i = 0; i < totalBits; i += packetDataBits) {

        auto bits = packetDataBits;
        if (i + bits > totalBits) {
          bits = totalBits - bits;
        }

        this->sendPUMDataToLLC(DynStreamSliceId(), dstBanks, (bits + 7) / 8);

        for (const auto &dstNodeId : dstBanks.getAllDest()) {
          this->sentInterBankPacketMap.emplace(dstNodeId, 0).first->second++;
        }
        this->sentPUMDataPkts += dstBanks.count();
      }
    }

    return Cycles(accumulatedLatency);
  }

  if (command.type == "cmp") {

    bool forceInt = this->controller->myParams->stream_pum_force_integer;

    auto wordlineBits = command.wordline_bits;
    auto wordlineBitsSquare = wordlineBits * wordlineBits;
    int computeLatency = wordlineBits;
    switch (command.opClass) {
    default:
      panic("Unknown PUM OpClass %s.", Enums::OpClassStrings[command.opClass]);
      break;
    case No_OpClass:
    case SimdMiscOp:
      computeLatency = 1;
      break;
    case FloatMemReadOp:
      // Assume one cycle to read 1 bit of constant value.
      computeLatency = wordlineBits;
      break;
    case SimdCmpOp:
    case IntAluOp:
      computeLatency = wordlineBits;
      break;
    case IntMultOp:
      computeLatency = wordlineBitsSquare / 2;
      break;
    case FloatAddOp:
    case SimdFloatAddOp: {
      computeLatency = forceInt ? wordlineBits : wordlineBitsSquare;
      break;
    }
    case FloatMultOp:
    case SimdFloatMultOp: {
      computeLatency = forceInt ? wordlineBitsSquare / 2 : wordlineBitsSquare;
      break;
    }
    case SimdFloatDivOp: {
      computeLatency = wordlineBitsSquare;
      break;
    }
    case SimdFloatCmpOp:
      computeLatency = forceInt ? wordlineBits : wordlineBitsSquare;
      break;
    }
    return Cycles(computeLatency);
  }

  panic("Unknown PUMCommand %s.", command.type);
}

void PUMEngine::tick() {

  if (this->commands.empty() || !this->receivedConfig) {
    return;
  }

  if (this->controller->curCycle() < this->nextCmdReadyCycle) {
    return;
  }

  if (this->commands[this->nextCmdIdx].type == "sync") {
    // We are waiting for the sync.
    if (!this->acked) {
      if (this->nextCmdIdx + 1 == this->commands.size()) {
        // This is the last sync. We are done.
        LLC_SE_DPRINTF("[Sync] Completed Round %d.\n",
                       this->completedRound + 1);
        this->completedRound++;
      }
      LLC_SE_DPRINTF("[Sync] SentPackets %d.\n", this->sentPUMDataPkts);
      this->sendSyncToLLCs(this->sentInterBankPacketMap, DynStreamSliceId());
      auto sentPackets = this->sentPUMDataPkts;
      this->sentPUMDataPkts = 0;
      this->sentInterBankPacketMap.clear();
      this->acked = true;
      this->sendSyncToMLC(sentPackets);
    }
    return;
  }

  this->kickNextCommand();
}

void PUMEngine::synced() {
  assert(this->nextCmdIdx < this->commands.size());
  const auto &c = this->commands[this->nextCmdIdx];
  assert(c.type == "sync");
  assert(this->acked);
  this->acked = false;
  this->nextCmdIdx++;
  this->kickNextCommand();
}

void PUMEngine::receiveData(const RequestMsg &msg) {

  /**
   * So far if this is from PUMPrefetchStream, we simply discard it.
   */

  assert(this->pumManager);
  if (msg.m_isPUMPrefetch) {
    assert(msg.m_sliceIds.isValid());
    this->pumManager->receivePrefetchPacket(1);
    return;
  }
  this->pumManager->reportProgress(this->pumContextId);
  if (msg.m_sliceIds.isValid()) {
    this->receiveDataFromStream(msg);
  } else {
    this->receiveDataFromPUM(msg);
  }
}

void PUMEngine::receiveDataFromPUM(const RequestMsg &msg) {

  auto sender = msg.m_Requestors.singleElement();
  auto senderNodeId = sender.getRawNodeID();
  auto iter = this->recvPUMDataPktMap
                  .emplace(std::piecewise_construct,
                           std::forward_as_tuple(senderNodeId),
                           std::forward_as_tuple(0, -1))
                  .first;
  if (msg.m_isPUM) {
    // This is the sync message.
    auto sentPackets = msg.m_Len;
    LLC_SE_DPRINTF("[Sync] Recv Done %d from %s Current %d.\n", sentPackets,
                   sender, iter->second.second);
    if (iter->second.second == -1) {
      iter->second.second = sentPackets;
    } else {
      iter->second.second += sentPackets;
    }
  } else {
    // This is normal data message.
    iter->second.first++;
    this->recvDataPkts++;
  }
  if (iter->second.first == iter->second.second) {
    auto recvPackets = iter->second.first;
    // Clear the entry.
    this->recvPUMDataPktMap.erase(iter);
    /**
     * At this point, we know we have received all messages from this sender.
     * However, we don't know if we will still receive more data from other
     * banks. So here I can only report Done for packets from this sender.
     */
    LLC_SE_DPRINTF(
        "[Sync] Sent Done %d from %s to MLC TotalRecvPkt %d RemainEntry %d.\n",
        recvPackets, sender, this->recvDataPkts,
        this->recvPUMDataPktMap.size());
    for (const auto &entry : this->recvPUMDataPktMap) {
      auto sender = MachineID::getMachineIDFromRawNodeID(entry.first);
      LLC_SE_DPRINTF("[Sync] Remain Entry from %s %d %d.\n", sender,
                     entry.second.first, entry.second.second);
    }
    this->sendDoneToMLC(recvPackets);
  }
}

void PUMEngine::receiveDataFromStream(const RequestMsg &msg) {

  const auto &sliceId = msg.m_sliceIds.singleSliceId();
  auto sender = sliceId.getDynStrandId();
  auto iter =
      this->recvStreamDataPktMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(sender),
                   std::forward_as_tuple(0, -1))
          .first;
  if (msg.m_isPUM) {
    // This is the sync message.
    auto sentPackets = msg.m_Len;
    LLC_SE_DPRINTF("[Sync] Recv Done %d from %s Current %d.\n", sentPackets,
                   sliceId, iter->second.second);
    if (iter->second.second == -1) {
      iter->second.second = sentPackets;
    } else {
      iter->second.second += sentPackets;
    }
  } else {
    // This is normal data message.
    LLC_SE_DPRINTF("[Sync] Recv Data %d from %s Current %d.\n",
                   iter->second.first, sliceId, iter->second.second);
    iter->second.first++;
    this->recvDataPkts++;
  }
  if (iter->second.first == iter->second.second) {
    auto recvPackets = iter->second.first;
    // Clear the entry.
    this->recvStreamDataPktMap.erase(iter);
    /**
     * At this point, we know we have received all messages from this sender.
     * However, we don't know if we will still receive more data from other
     * banks. So here I can only report Done for packets from this sender.
     */
    LLC_SE_DPRINTF(
        "[Sync] Sent Done %d from %s to MLC TotalRecvPkt %d RemainEntry %d.\n",
        recvPackets, sliceId, this->recvDataPkts,
        this->recvStreamDataPktMap.size());
    for (const auto &entry : this->recvStreamDataPktMap) {
      auto sender = entry.first;
      LLC_SE_DPRINTF("[Sync] Remain Entry from %s %d %d.\n", sender,
                     entry.second.first, entry.second.second);
    }
    this->sendDoneToMLC(recvPackets);
  }
}

void PUMEngine::sendDoneToMLC(int recvPackets) {
  this->sendAckToMLC(CoherenceResponseType_STREAM_DONE, recvPackets);
}

void PUMEngine::sendSyncToMLC(int sentPackets) {

  /**
   * This is represented as a StreamAck message.
   */
  LLC_SE_DPRINTF("[Sync] Sent Sync %d to MLC.\n", sentPackets);
  this->sendAckToMLC(CoherenceResponseType_STREAM_ACK, sentPackets);
}

void PUMEngine::sendAckToMLC(CoherenceResponseType type, int ackCount) {

  assert(this->pumManager);
  auto msg = std::make_shared<ResponseMsg>(this->controller->clockEdge());
  msg->m_addr = 0;
  msg->m_Type = type;
  msg->m_Sender = this->controller->getMachineID();
  msg->m_MessageSize = MessageSizeType_Control;
  msg->m_isPUM = true;
  msg->m_AckCount = ackCount;
  msg->m_Destination.add(this->pumManager->getMachineID());

  auto mlcMachineId = msg->m_Destination.singleElement();

  if (this->controller->isStreamIdeaAckEnabled()) {
    auto mlcController =
        AbstractStreamAwareController::getController(mlcMachineId);
    auto mlcSE = mlcController->getMLCStreamEngine();
    // StreamAck is also disguised as StreamData.
    mlcSE->receiveStreamData(*msg);
  } else {
    // Charge some latency.
    Cycles latency(1);
    this->se->streamResponseMsgBuffer->enqueue(
        msg, this->controller->clockEdge(),
        this->controller->cyclesToTicks(latency));
  }
}

void PUMEngine::sendSyncToLLCs(const SentPktMapT &sentMap,
                               const DynStreamSliceId &sliceId) {
  for (const auto &entry : sentMap) {
    auto nodeId = entry.first;
    auto packets = entry.second;
    auto machineId = MachineID::getMachineIDFromRawNodeID(nodeId);
    LLC_SE_DPRINTF("[Sync] Sent Packets %d to %s.\n", packets, machineId);
    // Send a done msg to the destination bank.
    this->sendSyncToLLC(machineId, packets, sliceId);
  }
}

void PUMEngine::sendSyncToLLC(MachineID recvBank, int sentPackets,
                              const DynStreamSliceId &sliceId) {

  /**
   * This is represented as a StreamAck message.
   */
  assert(this->pumManager);
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = 0;
  msg->m_Type = CoherenceRequestType_STREAM_PUM_DATA;
  msg->m_Requestors.add(this->controller->getMachineID());
  msg->m_MessageSize = MessageSizeType_Control;
  msg->m_isPUM = true;
  msg->m_Len = sentPackets; // Reuse the Len field.
  msg->m_Destination.add(recvBank);
  if (sliceId.isValid()) {
    msg->m_sliceIds.add(sliceId);
  }

  // Charge some latency.
  Cycles latency(1);
  this->se->streamIndirectIssueMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(Cycles(1)));
}
