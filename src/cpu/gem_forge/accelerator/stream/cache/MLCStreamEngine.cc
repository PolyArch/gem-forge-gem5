
#include "MLCStreamEngine.hh"
#include "LLCStreamEngine.hh"
#include "MLCStrandManager.hh"
#include "MLCStreamNDCController.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"
#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"

// Generated by slicc.
#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStreamBase.hh"
#include "debug/MLCRubyStreamLife.hh"
#include "debug/MLCRubyStreamReuse.hh"
#include "debug/StreamNearDataComputing.hh"
#include "debug/StreamRangeSync.hh"

#define DEBUG_TYPE MLCRubyStreamBase
#include "../stream_log.hh"

#define MLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(MLCRubyStream, "[MLC_SE%d]: " format,                                \
          this->controller->getMachineID().num, ##args)

MLCStreamEngine::MLCStreamEngine(AbstractStreamAwareController *_controller,
                                 MessageBuffer *_responseToUpperMsgBuffer,
                                 MessageBuffer *_requestToLLCMsgBuffer)
    : Consumer(_controller), controller(_controller),
      responseToUpperMsgBuffer(_responseToUpperMsgBuffer),
      requestToLLCMsgBuffer(_requestToLLCMsgBuffer) {
  this->controller->registerMLCStreamEngine(this);

  this->ndcController = m5::make_unique<MLCStreamNDCController>(this);
  this->strandManager = m5::make_unique<MLCStrandManager>(this);
}

MLCStreamEngine::~MLCStreamEngine() {
  for (auto &idStream : this->idToStreamMap) {
    delete idStream.second;
    idStream.second = nullptr;
  }
  this->idToStreamMap.clear();
}

void MLCStreamEngine::receiveStreamConfigure(PacketPtr pkt) {
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream configure when stream float is disabled.\n");

  this->strandManager->receiveStreamConfigure(pkt);

  if (this->controller->isStreamRangeSyncEnabled()) {
    // Enable the range check.
    this->scheduleEvent(Cycles(1));
  }
}

void MLCStreamEngine::receiveStreamEnd(PacketPtr pkt) {
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream end when stream float is disabled.\n");
  this->strandManager->receiveStreamEnd(pkt);
}

void MLCStreamEngine::receiveStreamData(const ResponseMsg &msg) {
  if (msg.m_Type == CoherenceResponseType_STREAM_NDC) {
    this->receiveStreamNDCResponse(msg);
    return;
  }
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream data when stream float is disabled.\n");

  if (msg.m_Type == CoherenceResponseType_STREAM_RANGE) {
    auto sliceId = msg.m_sliceIds.singleSliceId();
    auto stream = this->getStreamFromDynamicId(sliceId.getDynStreamId());
    if (stream) {
      MLC_SLICE_DPRINTF_(StreamRangeSync, sliceId,
                         "[Range] Receive range: %s.\n", *msg.m_range);
      stream->receiveStreamRange(msg.m_range);
    } else {
      MLC_SLICE_DPRINTF_(StreamRangeSync, sliceId,
                         "[Range] Discard old range: %s.\n", *msg.m_range);
    }
    return;
  }
  if (msg.m_Type == CoherenceResponseType_STREAM_DONE) {
    auto sliceId = msg.m_sliceIds.singleSliceId();
    auto stream = this->getStreamFromDynamicId(sliceId.getDynStreamId());
    if (stream) {
      MLC_SLICE_DPRINTF_(StreamRangeSync, sliceId,
                         "[Commit] Receive StreamDone.\n");
      stream->receiveStreamDone(sliceId);
    } else {
      MLC_SLICE_DPRINTF_(StreamRangeSync, sliceId,
                         "[Commit] Receive StreamDone.\n");
    }
    return;
  }
  for (const auto &sliceId : msg.m_sliceIds.sliceIds) {
    /**
     * Due to multicast, it's possible we received sliceIds that
     * do not belong to this core. We simply ignore those.
     */
    auto sliceCoreId = sliceId.getDynStreamId().coreId;
    auto myCoreId = this->controller->getMachineID().getNum();
    if (sliceCoreId != myCoreId) {
      continue;
    }
    this->receiveStreamDataForSingleSlice(sliceId, msg.m_DataBlk,
                                          msg.getaddr());
  }
}

void MLCStreamEngine::receiveStreamDataForSingleSlice(
    const DynStreamSliceId &sliceId, const DataBlock &dataBlock,
    Addr paddrLine) {
  MLC_SLICE_DPRINTF(sliceId, "SE received data vaddr %#x.\n", sliceId.vaddr);
  auto stream = this->getStreamFromDynamicId(sliceId.getDynStreamId());
  if (stream) {
    // Found the stream.
    stream->receiveStreamData(sliceId, dataBlock, paddrLine);
    this->reuseSlice(sliceId, dataBlock);
    return;
  }
  // This is possible if the stream is already ended.
  if (this->endedStreamDynamicIds.count(sliceId.getDynStreamId()) > 0) {
    // The stream is already ended.
    // Sliently ignore it.
    return;
  }
  /**
   * ! Hack:
   * We haven't really support Two-Level Indorect StoreCompute, so their acks
   * will not be correctly handled. Here I just directly ack it.
   */
  if (!this->idToStreamMap.empty()) {
    auto coreSE = this->idToStreamMap.begin()->second->getStaticStream()->se;
    auto coreS = coreSE->getStream(sliceId.getDynStreamId().staticId);
    if (coreS->isStoreComputeStream() && !coreS->isDirectMemStream()) {
      if (auto dynCoreS = coreS->getDynStream(sliceId.getDynStreamId())) {
        if (dynCoreS->isFloatedToCache()) {
          MLC_SLICE_DPRINTF(
              sliceId,
              "HACK! Directly Ack for Two-Level Indirect StoreComputeStream.");
          dynCoreS->cacheAckedElements.insert(sliceId.getStartIdx());
          return;
        }
      }
    }
  }
  panic("Failed to find configured stream for %s.\n", sliceId.getDynStreamId());
}

MLCDynStream *MLCStreamEngine::getStreamFromDynamicId(const DynStreamId &id) {
  auto iter = this->idToStreamMap.find(id);
  if (iter == this->idToStreamMap.end()) {
    return nullptr;
  }
  return iter->second;
}

bool MLCStreamEngine::isStreamRequest(const DynStreamSliceId &slice) {
  if (!this->controller->isStreamFloatEnabled()) {
    return false;
  }
  if (!slice.isValid()) {
    return false;
  }
  // So far just check if the target stream is configured here.
  auto stream = this->getMLCDynStreamFromSlice(slice);
  if (!stream) {
    return false;
  }
  // If this is a PseudoOffload, we do not treate it as stream request.
  if (stream->getIsPseudoOffload()) {
    return false;
  }
  if (slice.getStartIdx() < stream->getFirstFloatElemIdx()) {
    return false;
  }
  return true;
}

bool MLCStreamEngine::isStreamOffloaded(const DynStreamSliceId &slice) {
  assert(this->isStreamRequest(slice) && "Should be a stream request.");
  return true;
}

bool MLCStreamEngine::isStreamCached(const DynStreamSliceId &slice) {
  assert(this->isStreamRequest(slice) && "Should be a stream request.");
  // So far no stream is cached.
  return false;
}

bool MLCStreamEngine::receiveOffloadStreamRequest(
    const DynStreamSliceId &sliceId) {
  assert(this->isStreamOffloaded(sliceId) &&
         "Should be an offloaded stream request.");
  auto stream = this->getMLCDynStreamFromSlice(sliceId);
  stream->receiveStreamRequest(sliceId);
  return true;
}

void MLCStreamEngine::receiveOffloadStreamRequestHit(
    const DynStreamSliceId &sliceId) {
  if (!this->isStreamOffloaded(sliceId)) {
    panic(MLC_SLICE_MSG(sliceId, "Receive hit request, but not floated."));
  }
  auto stream = this->getMLCDynStreamFromSlice(sliceId);
  stream->receiveStreamRequestHit(sliceId);
}

MLCDynStream *
MLCStreamEngine::getMLCDynStreamFromSlice(const DynStreamSliceId &slice) const {
  if (!slice.isValid()) {
    return nullptr;
  }
  auto iter = this->idToStreamMap.find(slice.getDynStreamId());
  if (iter != this->idToStreamMap.end()) {
    // Ignore it if the slice is not considered valid by the stream.
    if (iter->second->isSliceValid(slice)) {
      return iter->second;
    }
  }
  return nullptr;
}

void MLCStreamEngine::computeReuseInformation(
    CacheStreamConfigureVec &streamConfigs) {

  /**
   * This is an optimization to capture reuse in multiple streams, e.g.
   * a[i] and a[i + N], where N can be fit in the MLC stream buffer.
   * This should only apply to DirectStreams without any SendTo and UsedBy
   * dependence.
   */

  // 1. Group them by base.
  std::unordered_map<uint64_t, std::vector<CacheStreamConfigureDataPtr>> groups;
  for (auto &config : streamConfigs) {
    auto S = config->stream;
    auto groupId = S->getCoalesceBaseStreamId();
    if (groupId == 0) {
      MLC_S_DPRINTF_(MLCRubyStreamReuse, config->dynamicId,
                     "[MLC NoReuse] No coalesce group.\n");
      continue;
    }
    // Skip streams with any dependence.
    if (!config->depEdges.empty()) {
      MLC_S_DPRINTF_(MLCRubyStreamReuse, config->dynamicId,
                     "[MLC NoReuse] Have dependence.\n");
      continue;
    }
    // Check if continuous.
    auto linearAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
        config->addrGenCallback);
    if (!linearAddrGen) {
      MLC_S_DPRINTF_(MLCRubyStreamReuse, config->dynamicId,
                     "[MLC NotReuse] Not linear addr gen.\n");
      continue;
    }
    if (!linearAddrGen->isContinuous(config->addrGenFormalParams,
                                     config->elementSize)) {
      MLC_S_DPRINTF_(MLCRubyStreamReuse, config->dynamicId,
                     "[MLC NoReuse] Address pattern not continuous.\n");
      continue;
    }
    MLC_S_DPRINTF_(MLCRubyStreamReuse, config->dynamicId,
                   "[MLC Reuse] Add to group %llu.\n", groupId);
    groups
        .emplace(std::piecewise_construct, std::forward_as_tuple(groupId),
                 std::forward_as_tuple())
        .first->second.push_back(config);
  }
  // 2. Sort them with offset.
  for (auto &entry : groups) {
    auto &group = entry.second;
    std::sort(group.begin(), group.end(),
              [](const CacheStreamConfigureDataPtr &a,
                 const CacheStreamConfigureDataPtr &b) -> bool {
                return a->stream->getCoalesceOffset() <
                       b->stream->getCoalesceOffset();
              });
  }
  // 3. Build reuse chain.
  for (auto &entry : groups) {
    auto &group = entry.second;
    for (int i = 1; i < group.size(); ++i) {
      auto &lhsConfig = group[i - 1];
      auto &rhsConfig = group[i];
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
        MLC_S_DPRINTF_(
            MLCRubyStreamReuse, lhsConfig->dynamicId,
            "[MLC NoReuse] Ingore large reuse distance to %lu offset %lu.\n",
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
      MLC_S_DPRINTF_(MLCRubyStreamReuse, lhsConfig->dynamicId,
                     "[MLC Reuse] Add reuse chain -> %lu cut %lu.\n",
                     rhsConfig->dynamicId.staticId, lhsCutElementIdx);
    }
  }
}

void MLCStreamEngine::reuseSlice(const DynStreamSliceId &sliceId,
                                 const DataBlock &dataBlock) {
  auto streamId = sliceId.getDynStreamId();
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
    auto S = dynamic_cast<MLCDynDirectStream *>(
        this->idToStreamMap.at(targetStreamId));
    assert(S && "Only direct stream can have reuse.");
    S->receiveReuseStreamData(makeLineAddress(sliceId.vaddr), dataBlock);
    streamId = targetStreamId;
  }
}

void MLCStreamEngine::receiveStreamNDCRequest(PacketPtr pkt) {
  this->ndcController->receiveStreamNDCRequest(pkt);
}

void MLCStreamEngine::receiveStreamNDCResponse(const ResponseMsg &msg) {
  this->ndcController->receiveStreamNDCResponse(msg);
}

void MLCStreamEngine::wakeup() {
  if (!this->controller->isStreamRangeSyncEnabled()) {
    return;
  }
  for (auto &idStream : this->idToStreamMap) {
    auto S = dynamic_cast<MLCDynDirectStream *>(idStream.second);
    if (!S || !S->shouldRangeSync()) {
      continue;
    }
    S->checkCoreCommitProgress();
  }
  if (!this->idToStreamMap.empty()) {
    // Recheck next cycle.
    this->scheduleEvent(Cycles(1));
  }
}

void MLCStreamEngine::receiveStreamTotalTripCount(
    const DynStreamId &streamId, int64_t totalTripCount, Addr brokenPAddr,
    MachineType brokenMachineType) {
  auto dynS = this->getStreamFromDynamicId(streamId);
  if (!dynS) {
    MLC_S_PANIC_NO_DUMP(streamId, "Failed to get MLC S for StreamLoopBound.");
  }
  dynS->setTotalTripCount(totalTripCount, brokenPAddr, brokenMachineType);
}

void MLCStreamEngine::print(std::ostream &out) const {}