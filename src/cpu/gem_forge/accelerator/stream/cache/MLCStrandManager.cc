#include "MLCStrandManager.hh"
#include "LLCStreamEngine.hh"

#include "mem/ruby/protocol/RequestMsg.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStrandSplit.hh"
#include "debug/MLCRubyStreamBase.hh"
#include "debug/MLCRubyStreamLife.hh"

#define DEBUG_TYPE MLCRubyStreamBase
#include "../stream_log.hh"

#define MLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(MLCRubyStream, "[MLC_SE%d]: " format,                                \
          this->controller->getMachineID().num, ##args)

MLCStrandManager::MLCStrandManager(MLCStreamEngine *_mlcSE)
    : mlcSE(_mlcSE), controller(_mlcSE->controller) {}

MLCStrandManager::~MLCStrandManager() {
  for (auto &idStream : this->strandMap) {
    delete idStream.second;
    idStream.second = nullptr;
  }
  this->strandMap.clear();
}

void MLCStrandManager::receiveStreamConfigure(PacketPtr pkt) {

  auto configs = *(pkt->getPtr<CacheStreamConfigureVec *>());

  if (this->canSplitIntoStrands(*configs)) {
    this->splitIntoStrands(*configs);
  }

  mlcSE->computeReuseInformation(*configs);
  for (auto config : *configs) {
    this->configureStream(config, pkt->req->masterId());
  }

  // We initalize all LLCDynStreams here (see LLCDynStream.hh)
  LLCDynStream::allocateLLCStreams(this->controller, *configs);

  // Release the configure vec.
  delete configs;
  delete pkt;
}

bool MLCStrandManager::canSplitIntoStrands(
    const CacheStreamConfigureVec &configs) const {
  if (!this->controller->myParams->enable_stream_strand) {
    return false;
  }

  /**
   * We can split streams into strands iff.
   * 1. With known trip count (no StreamLoopBound).
   * 2. There is no indirect streams.
   * 3. Simple linear continuous streams.
   * 4. Float plan is just the LLC.
   * TODO: Handle reduction and tiled patterns.
   */
  for (const auto &config : configs) {
    if (!config->hasTotalTripCount()) {
      MLC_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                     "[Strand] No TripCount.\n");
      return false;
    }
    for (const auto &dep : config->depEdges) {
      if (dep.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {
        MLC_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                       "[Strand] Has IndirectS %s.\n", dep.data->dynamicId);
        return false;
      }
    }
    auto linearAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
        config->addrGenCallback);
    if (!linearAddrGen) {
      MLC_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                     "[Strand] Not LinearAddrGen.\n");
      return false;
    }
    if (!linearAddrGen->isContinuous(config->addrGenFormalParams,
                                     config->elementSize)) {
      MLC_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                     "[Strand] Not Continuous.\n");
      return false;
    }
    if (config->floatPlan.isFloatedToMem()) {
      MLC_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                     "[Strand] Float to Mem.\n");
      return false;
    }
    if (config->floatPlan.getFirstFloatElementIdx() != 0) {
      MLC_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                     "[Strand] Delayed Float.\n");
      return false;
    }
  }
  return true;
}

void MLCStrandManager::splitIntoStrands(CacheStreamConfigureVec &configs) {
  // Make a copy of the orginal stream configs.
  auto streamConfigs = configs;
  configs.clear();

  // For now just split by interleave = 1kB / 64B = 16, totalStrands = 64.
  auto initOffset = 0;
  auto interleave = 16;
  auto totalStrands = 64;
  StrandSplitInfo splitInfo(initOffset, interleave, totalStrands);

  // Split and insert into configs.
  for (auto &config : streamConfigs) {
    auto strandConfigs = config->splitIntoStrands(splitInfo);
    configs.insert(configs.end(), strandConfigs.begin(), strandConfigs.end());
  }
}

void MLCStrandManager::configureStream(CacheStreamConfigureDataPtr configs,
                                       MasterID masterId) {
  MLC_S_DPRINTF_(MLCRubyStreamLife, configs->dynamicId,
                 "Received StreamConfigure, TotalTripCount %lu.\n",
                 configs->totalTripCount);
  /**
   * Do not release the pkt and streamConfigureData as they should be forwarded
   * to the LLC bank and released there. However, we do need to fix up the
   * initPAddr to our LLC bank if case it is not valid.
   * ! This has to be done before initializing the MLCDynStream so that it
   * ! knows the initial llc bank.
   */
  if (!configs->initPAddrValid) {
    configs->initPAddr = this->controller->getAddressToOurLLC();
    configs->initPAddrValid = true;
  }

  /**
   * ! We initialize the indirect stream first so that
   * ! the direct stream's constructor can start notify it about base stream
   * data.
   */
  std::vector<MLCDynIndirectStream *> indirectStreams;
  for (const auto &edge : configs->depEdges) {
    if (edge.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {
      const auto &indirectStreamConfig = edge.data;
      // Let's create an indirect stream.
      auto indirectStream = new MLCDynIndirectStream(
          indirectStreamConfig, this->controller,
          mlcSE->responseToUpperMsgBuffer, mlcSE->requestToLLCMsgBuffer,
          configs->dynamicId /* Root dynamic stream id. */);
      this->strandMap.emplace(indirectStream->getDynStrandId(), indirectStream);
      indirectStreams.push_back(indirectStream);

      for (const auto &ISDepEdge : indirectStreamConfig->depEdges) {
        if (ISDepEdge.type != CacheStreamConfigureData::DepEdge::UsedBy) {
          continue;
        }
        /**
         * So far we don't support Two-Level Indirect LLCStream, except:
         * 1. IndirectRedcutionStream.
         * 2. Two-Level IndirectStoreComputeStream.
         */
        auto ISDepS = ISDepEdge.data->stream;
        if (ISDepS->isReduction() || ISDepS->isStoreComputeStream()) {
          auto IIS = new MLCDynIndirectStream(
              ISDepEdge.data, this->controller, mlcSE->responseToUpperMsgBuffer,
              mlcSE->requestToLLCMsgBuffer,
              configs->dynamicId /* Root dynamic stream id. */);
          this->strandMap.emplace(IIS->getDynStrandId(), IIS);

          indirectStreams.push_back(IIS);
          continue;
        }
        panic("Two-Level Indirect LLCStream is not supported: %s.",
              ISDepEdge.data->dynamicId);
      }
    }
  }
  // Create the direct stream.
  auto directStream = new MLCDynDirectStream(
      configs, this->controller, mlcSE->responseToUpperMsgBuffer,
      mlcSE->requestToLLCMsgBuffer, indirectStreams);
  this->strandMap.emplace(directStream->getDynStrandId(), directStream);

  /**
   * If there is reuse for this stream, we cut the stream's totalTripCount.
   * ! This can only be done after initializing MLC streams, as only LLC streams
   * ! should be cut.
   */
  {
    auto reuseIter = mlcSE->reverseReuseInfoMap.find(configs->dynamicId);
    if (reuseIter != mlcSE->reverseReuseInfoMap.end()) {
      auto cutElementIdx = reuseIter->second.targetCutElementIdx;
      auto cutLineVAddr = reuseIter->second.targetCutLineVAddr;
      if (configs->totalTripCount == -1 ||
          configs->totalTripCount > cutElementIdx) {
        configs->totalTripCount = cutElementIdx;
        configs->hasBeenCuttedByMLC = true;
        directStream->setLLCCutLineVAddr(cutLineVAddr);
        assert(configs->depEdges.empty() &&
               "Reuse stream with indirect stream is not supported.");
      }
    }
  }

  // Configure Remote SE.
  this->sendConfigToRemoteSE(configs, masterId);
}

void MLCStrandManager::sendConfigToRemoteSE(
    CacheStreamConfigureDataPtr streamConfigureData, MasterID masterId) {

  /**
   * Set the RemoteSE to LLC SE or Mem SE, depending on the FloatPlan on the
   * FirstFloatElemIdx.
   */
  auto firstFloatElemIdx =
      streamConfigureData->floatPlan.getFirstFloatElementIdx();
  auto firstFloatElemMachineTypee =
      streamConfigureData->floatPlan.getMachineTypeAtElem(firstFloatElemIdx);

  auto initPAddrLine = makeLineAddress(streamConfigureData->initPAddr);
  auto remoteSEMachineID = this->controller->mapAddressToLLCOrMem(
      initPAddrLine, firstFloatElemMachineTypee);

  // Create a new packet.
  RequestPtr req = std::make_shared<Request>(
      streamConfigureData->initPAddr, sizeof(streamConfigureData), 0, masterId);
  PacketPtr pkt = new Packet(req, MemCmd::StreamConfigReq);
  uint8_t *pktData = reinterpret_cast<uint8_t *>(
      new CacheStreamConfigureDataPtr(streamConfigureData));
  pkt->dataDynamic(pktData);
  // Enqueue a configure packet to the target LLC bank.
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = initPAddrLine;
  msg->m_Type = CoherenceRequestType_STREAM_CONFIG;
  msg->m_Requestors.add(this->controller->getMachineID());
  msg->m_Destination.add(remoteSEMachineID);
  msg->m_pkt = pkt;

  /**
   * If we enable PartialConfig, we assume the static parameters are
   * already configured at RemoteSE, and thus we only need to send out
   * dynamic parameters. Here we assume it can be represented as a
   * control message.
   */

  if (this->controller->myParams->enable_stream_partial_config) {
    msg->m_MessageSize = MessageSizeType_Control;
  } else {
    msg->m_MessageSize = MessageSizeType_Data;
  }

  Cycles latency(1); // Just use 1 cycle latency here.

  MLC_S_DPRINTF(streamConfigureData->dynamicId,
                "Send Config to RemoteSE at %s.\n", remoteSEMachineID);

  mlcSE->requestToLLCMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(latency));
}

void MLCStrandManager::receiveStreamEnd(PacketPtr pkt) {
  auto endIds = *(pkt->getPtr<std::vector<DynStreamId> *>());
  for (const auto &endId : *endIds) {
    this->endStream(endId, pkt->req->masterId());
  }
  // Release the vector and packet.
  delete endIds;
  delete pkt;
}

void MLCStrandManager::endStream(const DynStreamId &endId, MasterID masterId) {
  MLC_S_DPRINTF_(MLCRubyStreamLife, endId, "Received StreamEnd.\n");

  /**
   * Find all root strands and record the PAddr and MachineType to multicast the
   * StreamEnd message.
   */
  std::vector<std::pair<DynStrandId, std::pair<Addr, MachineType>>>
      rootStrandTailPAddrMachineTypeVec;
  for (const auto &entry : this->strandMap) {
    const auto &strandId = entry.first;
    if (strandId.dynStreamId == endId) {
      auto dynS = entry.second;
      rootStrandTailPAddrMachineTypeVec.emplace_back(
          strandId, dynS->getRemoteTailPAddrAndMachineType());
    }
  }
  assert(!rootStrandTailPAddrMachineTypeVec.empty() &&
         "Failed to find the ending root stream.");

  // End all streams with the correct root stream id (indirect streams).
  for (auto streamIter = this->strandMap.begin(),
            streamEnd = this->strandMap.end();
       streamIter != streamEnd;) {
    auto stream = streamIter->second;
    if (stream->getRootDynStreamId() == endId) {
      /**
       * ? Can we release right now?
       * We need to make sure all the seen request is responded (with dummy
       * data).
       * TODO: In the future, if the core doesn't require to send the request,
       * TODO: we are fine to simply release the stream.
       */
      mlcSE->endedStreamDynamicIds.insert(stream->getDynStreamId());
      stream->endStream();
      delete stream;
      streamIter->second = nullptr;
      streamIter = this->strandMap.erase(streamIter);
    } else {
      ++streamIter;
    }
  }

  // Clear the reuse information.
  if (mlcSE->reuseInfoMap.count(endId)) {
    mlcSE->reverseReuseInfoMap.erase(
        mlcSE->reuseInfoMap.at(endId).targetStreamId);
    mlcSE->reuseInfoMap.erase(endId);
  }

  // For each remote root strand, send out a StreamEnd packet.
  for (const auto &entry : rootStrandTailPAddrMachineTypeVec) {

    const auto &strandId = entry.first;
    auto rootLLCStreamPAddr = entry.second.first;
    auto rootStreamOffloadedMachineType = entry.second.second;

    auto rootLLCStreamPAddrLine = makeLineAddress(rootLLCStreamPAddr);
    auto rootStreamOffloadedBank = this->controller->mapAddressToLLCOrMem(
        rootLLCStreamPAddrLine, rootStreamOffloadedMachineType);
    auto copyStrandId = new DynStrandId(strandId);
    RequestPtr req = std::make_shared<Request>(
        rootLLCStreamPAddrLine, sizeof(copyStrandId), 0, masterId);
    PacketPtr pkt = new Packet(req, MemCmd::StreamEndReq);
    uint8_t *pktData = new uint8_t[req->getSize()];
    *(reinterpret_cast<uint64_t *>(pktData)) =
        reinterpret_cast<uint64_t>(copyStrandId);
    pkt->dataDynamic(pktData);

    if (this->controller->myParams->enable_stream_idea_end) {
      auto remoteController =
          AbstractStreamAwareController::getController(rootStreamOffloadedBank);
      auto remoteSE = remoteController->getLLCStreamEngine();
      // StreamAck is also disguised as StreamData.
      remoteSE->receiveStreamEnd(pkt);
      MLC_S_DPRINTF(strandId, "Send ideal StreamEnd to %s.\n",
                    rootStreamOffloadedBank);

    } else {
      // Enqueue a end packet to the target LLC bank.
      auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
      msg->m_addr = rootLLCStreamPAddrLine;
      msg->m_Type = CoherenceRequestType_STREAM_END;
      msg->m_Requestors.add(this->controller->getMachineID());
      msg->m_Destination.add(rootStreamOffloadedBank);
      msg->m_MessageSize = MessageSizeType_Control;
      msg->m_pkt = pkt;

      Cycles latency(1); // Just use 1 cycle latency here.

      mlcSE->requestToLLCMsgBuffer->enqueue(
          msg, this->controller->clockEdge(),
          this->controller->cyclesToTicks(latency));
    }
  }
}

StreamEngine *MLCStrandManager::getCoreSE() const {
  if (!this->strandMap.empty()) {
    return this->strandMap.begin()->second->getStaticStream()->se;
  } else {
    return nullptr;
  }
}

MLCDynStream *MLCStrandManager::getStreamFromStrandId(const DynStrandId &id) {
  auto iter = this->strandMap.find(id);
  if (iter == this->strandMap.end()) {
    return nullptr;
  }
  return iter->second;
}

MLCDynStream *
MLCStrandManager::getStreamFromCoreSliceId(const DynStreamSliceId &sliceId) {
  if (!sliceId.isValid()) {
    return nullptr;
  }
  // TODO: Support the translation.
  auto dynS = this->getStreamFromStrandId(sliceId.getDynStrandId());
  if (dynS) {
    assert(
        dynS->getDynStrandId().totalStrands == 1 &&
        "Translation between CoreSlice and StrandSlice not implemented yet.");
  }
  return dynS;
}

void MLCStrandManager::checkCoreCommitProgress() {
  for (auto &idStream : this->strandMap) {
    auto S = dynamic_cast<MLCDynDirectStream *>(idStream.second);
    if (!S || !S->shouldRangeSync()) {
      continue;
    }
    S->checkCoreCommitProgress();
  }
}