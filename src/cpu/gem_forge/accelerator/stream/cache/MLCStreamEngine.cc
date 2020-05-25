
#include "MLCStreamEngine.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"

// Generated by slicc.
#include "mem/ruby/protocol/RequestMsg.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStream.hh"

#define DEBUG_TYPE MLCRubyStream
#include "../stream_log.hh"

#define MLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(MLCRubyStream, "[MLC_SE%d]: " format,                                \
          this->controller->getMachineID().num, ##args)

#define MLC_STREAM_DPRINTF(streamId, format, args...)                          \
  DPRINTF(MLCRubyStream, "[MLC_SE%d][%llu]: " format,                          \
          this->controller->getMachineID().num, streamId, ##args)
#define MLC_STREAM_HACK(streamId, format, args...)                             \
  hack("[MLC_SE%d][%llu]: " format, this->controller->getMachineID().num,      \
       streamId, ##args)

MLCStreamEngine::MLCStreamEngine(AbstractStreamAwareController *_controller,
                                 MessageBuffer *_responseToUpperMsgBuffer,
                                 MessageBuffer *_requestToLLCMsgBuffer)
    : controller(_controller),
      responseToUpperMsgBuffer(_responseToUpperMsgBuffer),
      requestToLLCMsgBuffer(_requestToLLCMsgBuffer) {
  this->controller->registerMLCStreamEngine(this);
}

MLCStreamEngine::~MLCStreamEngine() {
  for (auto &stream : this->streams) {
    delete stream;
    stream = nullptr;
  }
  this->streams.clear();
}

void MLCStreamEngine::receiveStreamConfigure(PacketPtr pkt) {
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream configure when stream float is disabled.\n");
  auto streamConfigs = *(pkt->getPtr<CacheStreamConfigureVec *>());
  this->computeReuseInformation(*streamConfigs);
  for (auto streamConfigureData : *streamConfigs) {
    this->configureStream(streamConfigureData, pkt->req->masterId());
  }
  // Release the configure vec.
  delete streamConfigs;
  delete pkt;
}

void MLCStreamEngine::configureStream(
    CacheStreamConfigureData *streamConfigureData, MasterID masterId) {
  MLC_STREAM_DPRINTF(streamConfigureData->dynamicId.staticId,
                     "Received StreamConfigure, totalTripCount %lu.\n",
                     streamConfigureData->totalTripCount);
  /**
   * Do not release the pkt and streamConfigureData as they should be forwarded
   * to the LLC bank and released there. However, we do need to fix up the
   * initPAddr to our LLC bank if case it is not valid.
   * ! This has to be done before initializing the MLCDynamicStream so that it
   * ! knows the initial llc bank.
   */
  if (!streamConfigureData->initPAddrValid) {
    streamConfigureData->initPAddr = this->controller->getAddressToOurLLC();
    streamConfigureData->initPAddrValid = true;
  }

  /**
   * ! We initialize the indirect stream first so that
   * ! the direct stream's constructor can start notify it about base stream
   * data.
   */
  // Check if there is indirect stream.
  std::vector<MLCDynamicIndirectStream *> indirectStreams;
  for (auto &indirectStreamConfig : streamConfigureData->indirectStreams) {
    // Let's create an indirect stream.
    auto indirectStream = new MLCDynamicIndirectStream(
        indirectStreamConfig.get(), this->controller,
        this->responseToUpperMsgBuffer, this->requestToLLCMsgBuffer,
        streamConfigureData->dynamicId /* Root dynamic stream id. */);
    this->idToStreamMap.emplace(indirectStream->getDynamicStreamId(),
                                indirectStream);
    indirectStreams.push_back(indirectStream);
  }
  // Create the direct stream.
  auto directStream = new MLCDynamicDirectStream(
      streamConfigureData, this->controller, this->responseToUpperMsgBuffer,
      this->requestToLLCMsgBuffer, indirectStreams);
  this->idToStreamMap.emplace(directStream->getDynamicStreamId(), directStream);

  /**
   * If there is reuse for this stream, we cut the stream's totalTripCount.
   * ! This can only be done after initializing MLC streams, as only LLC streams
   * ! should be cut.
   */
  {
    auto reuseIter =
        this->reverseReuseInfoMap.find(streamConfigureData->dynamicId);
    if (reuseIter != this->reverseReuseInfoMap.end()) {
      auto cutElementIdx = reuseIter->second.targetCutElementIdx;
      auto cutLineVAddr = reuseIter->second.targetCutLineVAddr;
      if (streamConfigureData->totalTripCount == -1 ||
          streamConfigureData->totalTripCount > cutElementIdx) {
        streamConfigureData->totalTripCount = cutElementIdx;
        directStream->setLLCCutLineVAddr(cutLineVAddr);
        assert(streamConfigureData->indirectStreams.empty() &&
               "Reuse stream with indirect stream is not supported.");
      }
    }
  }

  // Create a new packet.
  RequestPtr req(new Request(streamConfigureData->initPAddr,
                             sizeof(streamConfigureData), 0, masterId));
  PacketPtr pkt = new Packet(req, MemCmd::StreamConfigReq);
  uint8_t *pktData = new uint8_t[req->getSize()];
  *(reinterpret_cast<uint64_t *>(pktData)) =
      reinterpret_cast<uint64_t>(streamConfigureData);
  pkt->dataDynamic(pktData);
  // Enqueue a configure packet to the target LLC bank.
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = streamConfigureData->initPAddr;
  msg->m_Type = CoherenceRequestType_STREAM_CONFIG;
  msg->m_XXNewRewquestor.add(this->controller->getMachineID());
  msg->m_Destination.add(this->mapPAddrToLLCBank(msg->m_addr));
  msg->m_MessageSize = MessageSizeType_Control;
  msg->m_pkt = pkt;

  Cycles latency(1); // Just use 1 cycle latency here.

  this->requestToLLCMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(latency));
}

void MLCStreamEngine::receiveStreamEnd(PacketPtr pkt) {
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream end when stream float is disabled.\n");
  auto endIds = *(pkt->getPtr<std::vector<DynamicStreamId> *>());
  for (const auto &endId : *endIds) {
    this->endStream(endId, pkt->req->masterId());
  }
  // Release the vector and packet.
  delete endIds;
  delete pkt;
}

void MLCStreamEngine::endStream(const DynamicStreamId &endId,
                                MasterID masterId) {
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream end when stream float is disabled.\n");
  MLC_STREAM_DPRINTF(endId.staticId, "Received StreamEnd.\n");

  // The PAddr of the llc stream. The cache controller uses this to find which
  // LLC bank to forward this StreamEnd message.
  auto rootStreamIter = this->idToStreamMap.find(endId);
  assert(rootStreamIter != this->idToStreamMap.end() &&
         "Failed to find the ending root stream.");
  Addr rootLLCStreamPAddr = rootStreamIter->second->getLLCStreamTailPAddr();

  // End all streams with the correct root stream id (indirect streams).
  for (auto streamIter = this->idToStreamMap.begin(),
            streamEnd = this->idToStreamMap.end();
       streamIter != streamEnd;) {
    auto stream = streamIter->second;
    if (stream->getRootDynamicStreamId() == endId) {
      /**
       * ? Can we release right now?
       * We need to make sure all the seen request is responded (with dummy
       * data).
       * TODO: In the future, if the core doesn't require to send the request,
       * TODO: we are fine to simply release the stream.
       */
      this->endedStreamDynamicIds.insert(stream->getDynamicStreamId());
      stream->endStream();
      delete stream;
      streamIter->second = nullptr;
      streamIter = this->idToStreamMap.erase(streamIter);
    } else {
      ++streamIter;
    }
  }

  // Clear the reuse information.
  if (this->reuseInfoMap.count(endId)) {
    this->reverseReuseInfoMap.erase(
        this->reuseInfoMap.at(endId).targetStreamId);
    this->reuseInfoMap.erase(endId);
  }

  // Create a new packet and send to LLC bank to terminate the stream.
  auto rootLLCStreamPAddrLine = makeLineAddress(rootLLCStreamPAddr);
  auto copyEndId = new DynamicStreamId(endId);
  RequestPtr req(
      new Request(rootLLCStreamPAddrLine, sizeof(copyEndId), 0, masterId));
  PacketPtr pkt = new Packet(req, MemCmd::StreamEndReq);
  uint8_t *pktData = new uint8_t[req->getSize()];
  *(reinterpret_cast<uint64_t *>(pktData)) =
      reinterpret_cast<uint64_t>(copyEndId);
  pkt->dataDynamic(pktData);
  // Enqueue a configure packet to the target LLC bank.
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = rootLLCStreamPAddrLine;
  msg->m_Type = CoherenceRequestType_STREAM_END;
  msg->m_XXNewRewquestor.add(this->controller->getMachineID());
  msg->m_Destination.add(this->mapPAddrToLLCBank(msg->m_addr));
  msg->m_MessageSize = MessageSizeType_Control;
  msg->m_pkt = pkt;

  Cycles latency(1); // Just use 1 cycle latency here.

  this->requestToLLCMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(latency));
}

void MLCStreamEngine::receiveStreamData(const ResponseMsg &msg) {
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream data when stream float is disabled.\n");
  for (const auto &sliceId : msg.m_sliceIds.sliceIds) {
    /**
     * Due to multicast, it's possible we received sliceIds that
     * do not belong to this core. We simply ignore those.
     */
    auto sliceCoreId = sliceId.streamId.coreId;
    auto myCoreId = this->controller->getMachineID().getNum();
    if (sliceCoreId != myCoreId) {
      continue;
    }
    this->receiveStreamDataForSingleSlice(sliceId, msg.m_DataBlk,
                                          msg.getaddr());
  }
}

void MLCStreamEngine::receiveStreamDataForSingleSlice(
    const DynamicStreamSliceId &sliceId, const DataBlock &dataBlock,
    Addr paddrLine) {
  MLC_SLICE_DPRINTF(sliceId, "SE received data %#x.\n", sliceId.vaddr);
  for (auto &iter : this->idToStreamMap) {
    if (iter.second->getDynamicStreamId() == sliceId.streamId) {
      // Found the stream.
      iter.second->receiveStreamData(sliceId, dataBlock, paddrLine);
      this->reuseSlice(sliceId, dataBlock);
      return;
    }
  }
  // This is possible if the stream is already ended.
  if (this->endedStreamDynamicIds.count(sliceId.streamId) > 0) {
    // The stream is already ended.
    // Sliently ignore it.
    return;
  }
  panic("Failed to find configured stream for %s.\n", sliceId.streamId);
}

bool MLCStreamEngine::isStreamRequest(const DynamicStreamSliceId &slice) {
  if (!this->controller->isStreamFloatEnabled()) {
    return false;
  }
  if (!slice.isValid()) {
    return false;
  }
  // So far just check if the target stream is configured here.
  auto stream = this->getMLCDynamicStreamFromSlice(slice);
  if (!stream) {
    return false;
  }
  // If this is a PseudoOffload, we do not treate it as stream request.
  if (stream->getIsPseudoOffload()) {
    return false;
  }
  return true;
}

bool MLCStreamEngine::isStreamOffloaded(const DynamicStreamSliceId &slice) {
  assert(this->isStreamRequest(slice) && "Should be a stream request.");
  auto stream = this->getMLCDynamicStreamFromSlice(slice);
  auto staticStream = stream->getStaticStream();
  // So far always offload for load stream.
  if (staticStream->getStreamType() == ::LLVM::TDG::StreamInfo_Type_LD) {
    return true;
  }
  return false;
}

bool MLCStreamEngine::isStreamCached(const DynamicStreamSliceId &slice) {
  assert(this->isStreamRequest(slice) && "Should be a stream request.");
  auto stream = this->getMLCDynamicStreamFromSlice(slice);
  auto staticStream = stream->getStaticStream();
  // So far do not cache for load stream.
  if (staticStream->getStreamType() == ::LLVM::TDG::StreamInfo_Type_LD) {
    return false;
  }
  return true;
}

bool MLCStreamEngine::receiveOffloadStreamRequest(
    const DynamicStreamSliceId &sliceId) {
  assert(this->isStreamOffloaded(sliceId) &&
         "Should be an offloaded stream request.");
  auto stream = this->getMLCDynamicStreamFromSlice(sliceId);
  stream->receiveStreamRequest(sliceId);
  return true;
}

void MLCStreamEngine::receiveOffloadStreamRequestHit(
    const DynamicStreamSliceId &sliceId) {
  assert(this->isStreamOffloaded(sliceId) &&
         "Should be an offloaded stream request.");
  auto stream = this->getMLCDynamicStreamFromSlice(sliceId);
  stream->receiveStreamRequestHit(sliceId);
}

MLCDynamicStream *MLCStreamEngine::getMLCDynamicStreamFromSlice(
    const DynamicStreamSliceId &slice) const {
  if (!slice.isValid()) {
    return nullptr;
  }
  auto iter = this->idToStreamMap.find(slice.streamId);
  if (iter != this->idToStreamMap.end()) {
    // Ignore it if the slice is not considered valid by the stream.
    if (iter->second->isSliceValid(slice)) {
      return iter->second;
    }
  }
  return nullptr;
}

MachineID MLCStreamEngine::mapPAddrToLLCBank(Addr paddr) const {
  auto selfMachineId = this->controller->getMachineID();
  auto llcMachineId = this->controller->mapAddressToLLC(
      paddr, static_cast<MachineType>(selfMachineId.type + 1));
  return llcMachineId;
}

void MLCStreamEngine::computeReuseInformation(
    CacheStreamConfigureVec &streamConfigs) {

  // 1. Group them by base.
  std::unordered_map<uint64_t, std::vector<CacheStreamConfigureData *>> groups;
  for (auto *config : streamConfigs) {
    auto S = config->stream;
    auto groupId = S->getCoalesceBaseStreamId();
    if (groupId == 0) {
      continue;
    }
    // Check if continuous.
    auto linearAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
        config->addrGenCallback);
    if (!linearAddrGen) {
      continue;
    }
    if (!linearAddrGen->isContinuous(config->addrGenFormalParams,
                                     config->elementSize)) {
      MLC_STREAM_DPRINTF(config->dynamicId.staticId,
                         "Address pattern not continuous.\n");
      continue;
    }
    groups
        .emplace(std::piecewise_construct, std::forward_as_tuple(groupId),
                 std::forward_as_tuple())
        .first->second.push_back(config);
  }
  // 2. Sort them with offset.
  for (auto &entry : groups) {
    auto &group = entry.second;
    std::sort(group.begin(), group.end(),
              [](const CacheStreamConfigureData *a,
                 const CacheStreamConfigureData *b) -> bool {
                return a->stream->getCoalesceOffset() <
                       b->stream->getCoalesceOffset();
              });
  }
  // 3. Build reuse chain.
  for (auto &entry : groups) {
    auto &group = entry.second;
    for (int i = 1; i < group.size(); ++i) {
      auto *lhsConfig = group[i - 1];
      auto *rhsConfig = group[i];
      auto lhsAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
          lhsConfig->addrGenCallback);
      auto rhsAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
          rhsConfig->addrGenCallback);
      // Compute the cut element idx.
      auto lhsStartAddr =
          lhsAddrGen->getStartAddr(lhsConfig->addrGenFormalParams);
      auto rhsStartAddr =
          rhsAddrGen->getStartAddr(rhsConfig->addrGenFormalParams);
      // Set the threshold to 32kB.
      constexpr uint64_t ReuseThreshold = 32 * 1024;
      assert(rhsStartAddr > lhsStartAddr && "Illegal reversed startAddr.");
      auto startOffset = rhsStartAddr - lhsStartAddr;
      if (startOffset > ReuseThreshold) {
        MLC_STREAM_DPRINTF(lhsConfig->dynamicId.staticId,
                           "Ingore large reuse distance -> %lu offset %lu.\n",
                           rhsConfig->dynamicId.staticId, startOffset);
        continue;
      }
      auto rhsStartLindAddr = makeLineAddress(rhsStartAddr);
      auto lhsCutElementIdx = lhsAddrGen->getFirstElementForAddr(
          lhsConfig->addrGenFormalParams, lhsConfig->elementSize,
          rhsStartLindAddr);
      // Store the reuse info.
      this->reuseInfoMap.emplace(
          std::piecewise_construct, std::forward_as_tuple(rhsConfig->dynamicId),
          std::forward_as_tuple(lhsConfig->dynamicId, lhsCutElementIdx,
                                rhsStartLindAddr));
      this->reverseReuseInfoMap.emplace(
          std::piecewise_construct, std::forward_as_tuple(lhsConfig->dynamicId),
          std::forward_as_tuple(rhsConfig->dynamicId, lhsCutElementIdx,
                                rhsStartLindAddr));
      MLC_STREAM_DPRINTF(lhsConfig->dynamicId.staticId,
                         "Add reuse chain -> %lu cut %lu.\n",
                         rhsConfig->dynamicId.staticId, lhsCutElementIdx);
      hack("Add reuse chain %lu -> %lu cut %lu.\n",
           lhsConfig->dynamicId.staticId, rhsConfig->dynamicId.staticId,
           lhsCutElementIdx);
    }
  }
}

void MLCStreamEngine::reuseSlice(const DynamicStreamSliceId &sliceId,
                                 const DataBlock &dataBlock) {
  auto streamId = sliceId.streamId;
  while (this->reuseInfoMap.count(streamId)) {
    const auto &reuseInfo = this->reuseInfoMap.at(streamId);
    const auto &targetStreamId = reuseInfo.targetStreamId;
    // Simply notify the target stream.
    if (this->idToStreamMap.count(targetStreamId) == 0) {
      if (this->endedStreamDynamicIds.count(targetStreamId) != 0) {
        // This stream has already ended.
        continue;
      }
      panic("Failed to find target stream %s.\n", targetStreamId);
    }
    auto S = dynamic_cast<MLCDynamicDirectStream *>(
        this->idToStreamMap.at(targetStreamId));
    assert(S && "Only direct stream can have reuse.");
    S->receiveReuseStreamData(makeLineAddress(sliceId.vaddr), dataBlock);
    streamId = targetStreamId;
  }
}