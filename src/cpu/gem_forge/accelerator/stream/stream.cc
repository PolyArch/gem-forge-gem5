#include "stream.hh"
#include "insts.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"

// #include "base/misc.hh""
#include "base/trace.hh"
#include "proto/protoio.hh"

#include "debug/StreamBase.hh"
#define DEBUG_TYPE StreamBase
#include "stream_log.hh"

#define STREAM_DPRINTF(format, args...) S_DPRINTF(this, format, ##args)

#define STREAM_HACK(format, args...)                                           \
  hack("Stream %s: " format, this->getStreamName().c_str(), ##args)

#define STREAM_ENTRY_HACK(entry, format, args...)                              \
  STREAM_HACK("Entry (%lu, %lu): " format, (entry).idx.streamInstance,         \
              (entry).idx.entryIdx, ##args)

#define STREAM_PANIC(format, args...)                                          \
  {                                                                            \
    this->dump();                                                              \
    panic("Stream %s: " format, this->getStreamName().c_str(), ##args);        \
  }

#define STREAM_ENTRY_PANIC(entry, format, args...)                             \
  STREAM_PANIC("Entry (%lu, %lu): " format, (entry).idx.streamInstance,        \
               (entry).idx.entryIdx, ##args)

namespace gem5 {

StreamValue GetCoalescedStreamValue::operator()(uint64_t streamId) const {
  assert(this->stream->isCoalescedHere(streamId) &&
         "Invalid CoalescedStreamId.");
  int32_t offset, size;
  this->stream->getCoalescedOffsetAndSize(streamId, offset, size);
  StreamValue ret;
  memcpy(ret.uint8Ptr(), this->streamValue.uint8Ptr(offset), size);
  return ret;
}

Stream::Stream(const StreamArguments &args)
    : staticId(args.staticId), streamName(args.name),
      dynInstance(DynStreamId::InvalidInstanceId), statistic(args.staticId),
      floatTracer(args.cpuDelegator->cpuId(), args.name), cpu(args.cpu),
      se(args.se) {

  this->allocSize = 0;
  this->stepSize = 0;
  this->maxSize = args.maxSize;
  this->stepRootStream = nullptr;
  this->lateFetchCount = 0;
  this->streamRegion = args.streamRegion;

  /**
   * ! This is a pure evil hack.
   * In GaussianElim, the outer-most stream is aliased with the inner
   * streams, and thus must runs non-speculatively.
   * The correct way is to detect the alias, flush any speculative states
   * and then restart. However, here I do not issue unless the element
   * reached the head of the queue.
   */
  if (this->getStreamName() == "gfm.gaussian_elim.akk.ld" ||
      this->getStreamName() == "gfm.gaussian_elim.bk.ld") {
    this->setDelayIssueUntilFIFOHead();
  }
}

Stream::~Stream() {
  for (auto &LS : this->logicals) {
    delete LS;
    LS = nullptr;
  }
  this->logicals.clear();
}

void Stream::dumpStreamStats(std::ostream &os) const {
  if (this->statistic.numConfigured == 0 &&
      this->statistic.numMisConfigured == 0 &&
      this->statistic.numAllocated == 0) {
    // I have not been configured at all -- maybe only used in fast forward.
    return;
  }
  os << this->getStreamName() << '\n';
  this->statistic.dump(os);
  this->floatTracer.dump();
}

GemForgeCPUDelegator *Stream::getCPUDelegator() const {
  return this->se->getCPUDelegator();
}

StreamEngine *Stream::getSE() const { return this->se; }

bool Stream::isAtomicStream() const {
  return this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_AT;
}
bool Stream::isStoreStream() const {
  return this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_ST;
}
bool Stream::isLoadStream() const {
  return this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_LD;
}
bool Stream::isUpdateStream() const {
  return this->isLoadStream() && this->getEnabledStoreFunc();
}

bool Stream::isMemStream() const {
  switch (this->getStreamType()) {
  case ::LLVM::TDG::StreamInfo_Type_LD:
  case ::LLVM::TDG::StreamInfo_Type_ST:
  case ::LLVM::TDG::StreamInfo_Type_AT:
    return true;
  case ::LLVM::TDG::StreamInfo_Type_IV:
    return false;
  default:
    STREAM_PANIC("Invalid stream type.");
  }
}

bool Stream::isAffineIVStream() const {
  if (this->getStreamType() != ::LLVM::TDG::StreamInfo_Type_IV) {
    return false;
  }
  if (this->isReduction() || this->isPointerChaseIndVar()) {
    return false;
  }
  if (!std::dynamic_pointer_cast<LinearAddrGenCallback>(
          this->addrGenCallback)) {
    return false;
  }
  return true;
}

void Stream::addBaseStream(StreamDepEdge::TypeE type, bool isInnerLoop,
                           StaticId baseId, StaticId depId, Stream *baseS) {

  if (isInnerLoop) {
    this->innerLoopBaseEdges.emplace_back(type, depId, baseId, baseS);
    baseS->innerLoopDepEdges.emplace_back(type, baseId, depId, this);
  } else {
    this->baseEdges.emplace_back(type, depId, baseId, baseS);
    baseS->depEdges.emplace_back(type, baseId, depId, this);
  }

  if (type == StreamDepEdge::TypeE::Addr) {
    if (baseS == this) {
      STREAM_PANIC("AddrBaseStream should not be self.");
    }
    this->addrBaseStreams.insert(baseS);
    baseS->addrDepStreams.insert(this);
  } else if (type == StreamDepEdge::TypeE::Value) {
    if (baseS == this) {
      STREAM_PANIC("ValueBaseStream should not be self.");
    }
    this->valueBaseStreams.insert(baseS);
    baseS->valueDepStreams.insert(this);
  } else if (type == StreamDepEdge::TypeE::Back) {
    this->backBaseStreams.insert(baseS);
    baseS->backDepStreams.insert(this);
    if (this->isReduction()) {
      baseS->hasBackDepReductionStream = true;
    }
  } else {
    STREAM_PANIC("Unknown StreamEdgeType %s.", type);
  }
}

void Stream::addPredBaseStream(bool predValue, bool isInnerLoop,
                               StaticId baseId, StaticId depId, Stream *baseS) {
  const auto type = StreamDepEdge::TypeE::Pred;
  if (isInnerLoop) {
    this->innerLoopBaseEdges.emplace_back(type, depId, baseId, baseS,
                                          predValue);
    baseS->innerLoopDepEdges.emplace_back(type, baseId, depId, this, predValue);
  } else {
    this->baseEdges.emplace_back(type, depId, baseId, baseS, predValue);
    baseS->depEdges.emplace_back(type, baseId, depId, this, predValue);
  }
  this->predBaseStreams.insert(baseS);
  baseS->predDepStreams.insert(this);
}

bool Stream::getPredValue(StaticId depId) {
  for (const auto &edge : this->depEdges) {
    if (edge.type != StreamDepEdge::TypeE::Pred) {
      continue;
    }
    if (edge.toStaticId != depId) {
      continue;
    }
    return edge.predValue;
  }
  STREAM_PANIC("Miss PredValue for DepS %d.", depId);
}

void Stream::addBaseStepStream(Stream *baseStepStream) {
  if (baseStepStream == this) {
    STREAM_PANIC("Base stream should not be self.");
  }
  this->baseStepStreams.insert(baseStepStream);
  baseStepStream->depStepStreams.insert(this);
  if (baseStepStream->isStepRoot()) {
    this->baseStepRootStreams.insert(baseStepStream);
  } else {
    for (auto stepRoot : baseStepStream->baseStepRootStreams) {
      this->baseStepRootStreams.insert(stepRoot);
    }
  }
}

void Stream::registerStepDependentStreamToRoot(Stream *newStepDependentStream) {
  if (!this->isStepRoot()) {
    STREAM_PANIC("Try to register step instruction to non-root stream.");
  }
  for (auto &stepStream : this->stepStreamList) {
    if (stepStream == newStepDependentStream) {
      STREAM_PANIC(
          "The new step dependent stream has already been registered.");
    }
  }
  this->stepStreamList.emplace_back(newStepDependentStream);
}

void Stream::initializeAliasStreamsFromProtobuf(
    const ::LLVM::TDG::StaticStreamInfo &info) {
  auto aliasBaseStreamId = info.alias_base_stream().id();
  this->aliasBaseStream = this->se->tryGetStream(aliasBaseStreamId);
  /**
   * If can not find the alias base stream, simply use myself.
   */
  if (!this->aliasBaseStream) {
    this->aliasBaseStream = this;
  }
  this->aliasOffset = info.alias_offset();
  /**
   * Inform the AliasBaseStream that whether I am store.
   * Notice that this should not be implemented in the opposite way, as
   * AliasedStream may not be finalized yet, and getStreamType() may not be
   * available.
   */
  if (this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_ST) {
    this->aliasBaseStream->hasAliasedStoreStream = true;
  }
  /**
   * Same thing for update stream.
   */
  if (this->hasUpdate() && !this->getEnabledStoreFunc()) {
    // There are StoreStream that update this LoadStream.
    this->aliasBaseStream->hasAliasedStoreStream = true;
  }

  for (const auto &aliasedStreamId : info.aliased_streams()) {
    auto aliasedS = this->se->tryGetStream(aliasedStreamId.id());
    if (aliasedS) {
      // It's possible some streams may not be configured, e.g.
      // StoreStream that is merged into the LoadStream, making
      // them a UpdateStream.
      this->aliasedStreams.push_back(aliasedS);
    }
  }
}

void Stream::initializeCoalesceGroupStreams() {
  if (this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_IV) {
    // Not MemStream.
    return;
  }
  auto sid = this->getCoalesceBaseStreamId();
  if (sid == 0) {
    // This is invalid.
    return;
  }
  auto coalesceBaseS = this->se->getStream(sid);
  coalesceBaseS->coalesceGroupStreams.insert(this);
}

void Stream::dispatchStreamConfig(uint64_t seqNum, ThreadContext *tc) {
  // Allocate the new DynStream.
  this->dynamicStreams.emplace_back(this, this->allocateNewInstance(), seqNum,
                                    this->getCPUDelegator()->curCycle(), tc,
                                    this->se);
}

void Stream::executeStreamConfig(uint64_t seqNum,
                                 const DynStreamParamV *inputVec) {
  auto &dynStream = this->getDynStream(seqNum);
  assert(!dynStream.configExecuted && "StreamConfig already executed.");
  dynStream.configExecuted = true;

  /**
   * We intercept the extra input value here.
   */
  if (inputVec) {
    assert(!se->isTraceSim());
    DynStreamParamV inputVecCopy(*inputVec);
    this->extractExtraInputValues(dynStream, &inputVecCopy);
    this->setupAddrGen(dynStream, &inputVecCopy);
  } else {
    assert(se->isTraceSim());
    this->extractExtraInputValues(dynStream, nullptr);
    this->setupAddrGen(dynStream, nullptr);
  }

  /**
   * We are going to copy the total trip count from the step root stream.
   */
  if (this->stepRootStream) {
    const auto &rootDynS = this->stepRootStream->getDynStream(seqNum);
    assert(rootDynS.configExecuted &&
           "Root dynamic stream should be executed.");
    if (rootDynS.hasTotalTripCount()) {
      if (dynStream.hasTotalTripCount()) {
        assert(dynStream.getTotalTripCount() == rootDynS.getTotalTripCount() &&
               "Mismatch in TotalTripCount.");
      } else {
        dynStream.setTotalAndInnerTripCount(rootDynS.getTotalTripCount());
        DYN_S_DPRINTF(dynStream.dynStreamId,
                      "Set TotalTripCount %llu from StepRoot.\n",
                      dynStream.getTotalTripCount());
      }
    } else {
      assert(!dynStream.hasTotalTripCount() &&
             "Missing TotalTripCount in StepRootStream.");
    }
  } else if (!this->backBaseStreams.empty()) {
    // Try to copy total trip count from back base stream.
    assert(this->backBaseStreams.size() == 1);
    auto backBaseS = *(this->backBaseStreams.begin());
    const auto &backBaseDynS = backBaseS->getDynStream(seqNum);
    if (backBaseDynS.configExecuted) {
      dynStream.setTotalAndInnerTripCount(backBaseDynS.getTotalTripCount());
      DYN_S_DPRINTF(dynStream.dynStreamId,
                    "Set TotalTripCount %llu from BackBase.\n",
                    dynStream.getTotalTripCount());
    }
  }

  // Try to set total trip count for back dependent streams.
  for (auto backDepS : this->backDepStreams) {
    if (backDepS->getConfigLoopLevel() != this->getConfigLoopLevel()) {
      // Not from the same loop level. Ignore. (Should be OuterLoopDepS).
      continue;
    }
    auto &backDepDynS = backDepS->getDynStream(seqNum);
    backDepDynS.setTotalAndInnerTripCount(dynStream.getTotalTripCount());
    DYN_S_DPRINTF(backDepDynS.dynStreamId,
                  "Set TotalTripCount %llu from BackBase.\n",
                  backDepDynS.getTotalTripCount());
  }
}

void Stream::commitStreamConfig(uint64_t seqNum) {
  auto &dynStream = this->getDynStream(seqNum);
  assert(dynStream.configExecuted && "StreamConfig committed before executed.");
  assert(!dynStream.configCommitted && "StreamConfig already committed.");
  dynStream.configCommitted = true;
}

void Stream::rewindStreamConfig(uint64_t seqNum) {
  // Rewind must happen in reverse order.
  assert(!this->dynamicStreams.empty() &&
         "Missing DynStream when rewinding StreamConfig.");
  auto &dynStream = this->dynamicStreams.back();
  assert(dynStream.configSeqNum == seqNum && "Mismatch configSeqNum.");

  // Check if we have offloaded this.
  if (dynStream.isFloatedToCache()) {
    // Sink streams should have already been taken care of.
    panic("Don't support rewind StreamConfig for offloaded stream.");
  }

  /**
   * Get rid of any unstepped elements of the dynamic stream.
   */
  while (dynStream.allocSize > dynStream.stepSize) {
    this->se->releaseElementUnstepped(dynStream);
  }

  // Get rid of the dynamicStream.
  this->dynamicStreams.pop_back();

  assert(dynStream.allocSize == dynStream.stepSize &&
         "Unstepped elements when rewind StreamConfig.");
  this->statistic.numMisConfigured++;
}

bool Stream::isStreamConfigureExecuted(uint64_t seqNum) {
  auto &dynStream = this->getDynStream(seqNum);
  return dynStream.configExecuted;
}

void Stream::releaseDynStream(uint64_t configSeqNum) {
  for (auto iter = this->dynamicStreams.begin();
       iter != this->dynamicStreams.end(); ++iter) {
    auto &dynS = *iter;
    if (dynS.endDispatched && dynS.configSeqNum == configSeqNum) {
      this->dynamicStreams.erase(iter);
      return;
    }
  }

  S_PANIC(this, "No DynS to release.");
}

void Stream::recordAggregateHistory(const DynStream &dynS) {
  if (this->aggregateHistory.size() == AggregateHistorySize) {
    this->aggregateHistory.pop_front();
  }
  this->aggregateHistory.emplace_back();
  auto &history = this->aggregateHistory.back();
  history.addrGenFormalParams = dynS.addrGenFormalParams;
  history.numReleasedElements = dynS.getNumReleasedElements();
  history.numIssuedRequests = dynS.getNumIssuedRequests();
  history.numPrivateCacheHits = dynS.getTotalHitPrivateCache();
  history.startVAddr = dynS.getStartVAddr();
  history.floated = dynS.isFloatedToCache();
}

DynStream &Stream::getDynStreamByInstance(InstanceId instance) {
  if (auto dynS = this->tryGetDynStreamByInstance(instance)) {
    return *dynS;
  }
  S_PANIC(this, "Failed to find DynStream by Instance %llu.\n", instance);
}

DynStream *Stream::tryGetDynStreamByInstance(InstanceId instance) {
  for (auto &dynStream : this->dynamicStreams) {
    if (dynStream.dynStreamId.streamInstance == instance) {
      return &dynStream;
    }
  }
  return nullptr;
}

DynStream &Stream::getDynStream(uint64_t seqNum) {
  for (auto &dynStream : this->dynamicStreams) {
    if (dynStream.configSeqNum == seqNum) {
      return dynStream;
    }
  }
  S_PANIC(this, "Failed to find DynStream by ConfigSeqNum %llu.\n", seqNum);
}

DynStream &Stream::getDynStreamByEndSeqNum(uint64_t seqNum) {
  for (auto &dynStream : this->dynamicStreams) {
    if (dynStream.endSeqNum == seqNum) {
      return dynStream;
    }
  }
  S_PANIC(this, "Failed to find DynStream by EndSeqNum %llu.\n", seqNum);
}

DynStream &Stream::getDynStreamBefore(uint64_t seqNum) {
  DynStream *dynS = nullptr;
  for (auto &dynStream : this->dynamicStreams) {
    if (dynStream.configSeqNum < seqNum) {
      dynS = &dynStream;
    }
  }
  if (!dynS) {
    S_PANIC(this, "Failed to find DynStream before SeqNum %llu.\n", seqNum);
  }
  return *dynS;
}

DynStream *Stream::getDynStream(const DynStreamId &dynId) {
  for (auto &dynStream : this->dynamicStreams) {
    if (dynStream.dynStreamId == dynId) {
      return &dynStream;
    }
  }
  return nullptr;
}

void Stream::setupLinearAddrFunc(DynStream &dynStream,
                                 const DynStreamParamV *inputVec,
                                 const LLVM::TDG::StreamInfo &info) {
  assert(inputVec && "Missing InputVec.");
  const auto &staticInfo = info.static_info();
  const auto &pattern = staticInfo.iv_pattern();
  assert(pattern.val_pattern() == ::LLVM::TDG::StreamValuePattern::LINEAR);
  /**
   * LINEAR pattern has 2n or (2n+1) parameters, where n is the difference of
   * loop level between ConfigureLoop and InnerMostLoop. It has the following
   * format, starting from InnerMostLoop. Stride0, [BackEdgeCount[i], Stride[i +
   * 1]]*, [BackEdgeCount[n]], Start We will add 1 to BackEdgeCount to get the
   * TripCount.
   */
  assert(pattern.params_size() >= 2 && "Number of parameters must be >= 2.");
  auto &formalParams = dynStream.addrGenFormalParams;
  auto inputIdx = 0;
  for (const auto &param : pattern.params()) {
    formalParams.emplace_back();
    auto &formalParam = formalParams.back();
    formalParam.isInvariant = true;
    if (param.is_static()) {
      // This param comes from the Configuration.
      // hack("Find valid param #%d, val %llu.\n", formalParams.size(),
      //      param.param());
      formalParam.invariant.uint64() = param.value();
    } else {
      // This should be an input.
      if (inputIdx >= inputVec->size()) {
        S_PANIC(this, "InputIdx (%d) overflowed, InputVec (%d).\n", inputIdx,
                inputVec->size());
      }
      // hack("Find input param #%d, val %llu.\n", formalParams.size(),
      //      inputVec->at(inputIdx));
      formalParam.invariant = inputVec->at(inputIdx);
      inputIdx++;
    }
  }

  assert(inputIdx == inputVec->size() && "Unused input value.");

  /**
   * We have to process the params to compute TotalTripCount for each nested
   * loop.
   * TripCount[i] = BackEdgeCount[i] + 1;
   * TotalTripCount[i] = TotalTripCount[i - 1] * TripCount[i];
   */
  DYN_S_DPRINTF(dynStream.dynStreamId,
                "Setup LinearAddrGenCallback with Input params --------\n");
  for (auto param : *inputVec) {
    DYN_S_DPRINTF(dynStream.dynStreamId, "%llu\n", param.at(0));
  }
  DYN_S_DPRINTF(dynStream.dynStreamId,
                "Setup LinearAddrGenCallback with params --------\n");
  for (auto param : formalParams) {
    DYN_S_DPRINTF(dynStream.dynStreamId, "%s\n", param.invariant);
  }

  int64_t totalTripCount = 1;
  for (auto idx = 1; idx < formalParams.size() - 1; idx += 2) {
    auto &formalParam = formalParams.at(idx);
    // TripCount.
    auto curTripCount = formalParam.invariant.uint64();
    if ((curTripCount >> 56) & 0xFF) {
      /**
       * ! Hack: LLVM may generate crazy trip count expression that does not
       * ! consider the case when TripCount could be negative or zero, as it
       * ! assumes such case is covered by some branch checking.
       * ! As a hack here, we check the most significant 8 bits, and if they
       * ! are set, we set the TripCount to 0.
       */
      DYN_S_WARN(dynStream.dynStreamId,
                 "Adjust Suspicious TripCount %d %lu To %lu.\n", idx,
                 curTripCount, 0);
      curTripCount = 0;
    }
    // TotalTripCount.
    totalTripCount *= curTripCount;
    formalParam.invariant.uint64() = totalTripCount;
  }

  DYN_S_DPRINTF(dynStream.dynStreamId,
                "Finalize LinearAddrGenCallback with params --------\n");
  for (auto param : formalParams) {
    DYN_S_DPRINTF(dynStream.dynStreamId, "%s\n", param.invariant);
  }

  // Set the callback.
  if (!this->addrGenCallback) {
    this->addrGenCallback = std::make_shared<LinearAddrGenCallback>();
  }
  dynStream.addrGenCallback = this->addrGenCallback;

  // Update the totalTripCount to the dynamic stream if possible.
  if (formalParams.size() % 2 == 1) {
    dynStream.setTotalAndInnerTripCount(
        formalParams.at(formalParams.size() - 2).invariant.uint64());
  }
}

void Stream::setupFuncAddrFunc(DynStream &dynStream,
                               const DynStreamParamV *inputVec,
                               const LLVM::TDG::StreamInfo &info) {

  const auto &addrFuncInfo = info.addr_func_info();
  assert(addrFuncInfo.name() != "" && "Missing AddrFuncInfo.");
  auto &formalParams = dynStream.addrGenFormalParams;
  auto usedInputs =
      this->setupFormalParams(inputVec, addrFuncInfo, formalParams);
  if (usedInputs < inputVec->size()) {
    S_PANIC(this, "Underflow of InputVec. Used %d of %d.", usedInputs,
            inputVec->size());
  }
  // Set the callback.
  if (!this->addrGenCallback) {
    auto addrExecFunc =
        std::make_shared<TheISA::ExecFunc>(dynStream.tc, addrFuncInfo);
    this->addrGenCallback = std::make_shared<FuncAddrGenCallback>(addrExecFunc);
  }
  dynStream.addrGenCallback = this->addrGenCallback;
}

int Stream::setupFormalParams(const DynStreamParamV *inputVec,
                              const LLVM::TDG::ExecFuncInfo &info,
                              DynStreamFormalParamV &formalParams) {
  assert(info.name() != "" && "Missing AddrFuncInfo.");
  int inputIdx = 0;
  for (const auto &arg : info.args()) {
    if (arg.is_stream()) {
      // This is a stream input.
      // hack("Find stream input param #%d id %llu.\n",
      // formalParams.size(),
      //      arg.stream_id());
      formalParams.emplace_back();
      auto &formalParam = formalParams.back();
      formalParam.isInvariant = false;
      formalParam.baseStreamId = arg.stream_id();
    } else {
      if (inputIdx >= inputVec->size()) {
        S_PANIC(this, "Missing input for %s: Given %llu, inputIdx %d.",
                info.name(), inputVec->size(), inputIdx);
      }
      // hack("Find invariant param #%d, val %llu.\n",
      // formalParams.size(),
      //      inputVec->at(inputIdx));
      formalParams.emplace_back();
      auto &formalParam = formalParams.back();
      formalParam.isInvariant = true;
      formalParam.invariant = inputVec->at(inputIdx);
      inputIdx++;
    }
  }
  return inputIdx;
}

/**
 * Extract extra input values from the inputVec. May modify inputVec.
 */
void Stream::extractExtraInputValues(DynStream &dynS,
                                     DynStreamParamV *inputVec) {

  /**
   * For trace simulation, we have no input.
   */
  if (se->isTraceSim()) {
    assert(this->getMergedPredicatedStreams().empty() &&
           "MergedPredicatedStreams in TraceSim.");
    assert(!this->getEnabledStoreFunc() && "StoreFunc in TraceSim.");
    assert(!this->getEnabledLoadFunc() && "LoadFunc in TraceSim.");
    assert(!this->isReduction() && "Reduction in TraceSim.");
    assert(!inputVec && "InputVec in TraceSim.");
    return;
  }

  /**
   * If this is load stream with merged predicated stream, check for
   * any inputs for the predication function.
   */
  assert(inputVec && "Missing InputVec.");
  const auto &predicatedStreams = this->getPredicatedStreams();

  int inputIdx = 0;

  if (predicatedStreams.size() > 0) {
    const auto &predFuncInfo = this->getPredicateFuncInfo();
    if (!this->predCallback) {
      this->predCallback =
          std::make_shared<TheISA::ExecFunc>(dynS.tc, predFuncInfo);
    }
    dynS.predCallback = this->predCallback;
    auto &predFormalParams = dynS.predFormalParams;
    auto usedInputs =
        this->setupFormalParams(inputVec, predFuncInfo, predFormalParams);
    // Consume these inputs.
    inputVec->erase(inputVec->begin(), inputVec->begin() + usedInputs);
    inputIdx += usedInputs;
  }
  /**
   * Handle StoreFunc.
   */
  if (this->getEnabledStoreFunc()) {
    assert(this->getStreamType() != ::LLVM::TDG::StreamInfo_Type_IV);
    const auto &storeFuncInfo = this->getStoreFuncInfo();
    if (!this->storeCallback) {
      this->storeCallback =
          std::make_shared<TheISA::ExecFunc>(dynS.tc, storeFuncInfo);
    }
    dynS.storeCallback = this->storeCallback;
    auto &storeFormalParams = dynS.storeFormalParams;
    auto usedInputs =
        this->setupFormalParams(inputVec, storeFuncInfo, storeFormalParams);
    // Consume these inputs.
    inputVec->erase(inputVec->begin(), inputVec->begin() + usedInputs);
    inputIdx += usedInputs;
  }
  if (this->getEnabledLoadFunc()) {
    const auto &info = this->getLoadFuncInfo();
    if (!this->loadCallback) {
      this->loadCallback = std::make_shared<TheISA::ExecFunc>(dynS.tc, info);
    }
    dynS.loadCallback = this->loadCallback;
    auto &loadFormalParams = dynS.loadFormalParams;
    if (this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_AT) {
      /**
       * For AtomicStreams, all inputs are the same as StoreFunc.
       */
      loadFormalParams = dynS.storeFormalParams;
    } else {
      /**
       * Other type streams has their own load inputs.
       */
      auto usedInputs =
          this->setupFormalParams(inputVec, info, loadFormalParams);
      // Consume these inputs.
      inputVec->erase(inputVec->begin(), inputVec->begin() + usedInputs);
      inputIdx += usedInputs;
    }
  }
  /**
   * If this is a Reduce/PtrChase stream, check for the initial value.
   * Also if the stream has FixedTripCount, check for the trip count.
   */
  if (this->isReduction() || this->isPointerChaseIndVar()) {
    if (this->getReduceFromZero()) {
      dynS.initialValue.fill(0);
    } else {
      assert(!inputVec->empty() &&
             "Missing initial value for Reduce/PtrChase stream.");
      dynS.initialValue = inputVec->front();
      inputVec->erase(inputVec->begin());
      inputIdx++;
    }

    if (this->isTripCountFixed()) {
      uint64_t tripCount = 1;
      for (int loopLevel = this->getLoopLevel();
           loopLevel >= this->getConfigLoopLevel(); --loopLevel) {
        uint64_t loopTripCount;

        if (inputIdx <
            this->primeLogical->info.static_info().iv_pattern().params_size()) {

          const auto &protoParam =
              this->primeLogical->info.static_info().iv_pattern().params(
                  inputIdx);
          if (protoParam.is_static()) {
            loopTripCount = protoParam.value();
          } else {
            if (inputVec->empty()) {
              DYN_S_PANIC(dynS.dynStreamId,
                          "[FixTripCount] Missing for LoopLevel %d "
                          "ConfigLoopLevel %d.\n",
                          loopLevel, this->getConfigLoopLevel());
            }
            loopTripCount = inputVec->front().uint64();
            inputVec->erase(inputVec->begin());
            inputIdx++;
          }
        } else {
          if (inputVec->empty()) {
            DYN_S_PANIC(dynS.dynStreamId,
                        "[FixTripCount] Missing for LoopLevel %d "
                        "ConfigLoopLevel %d.\n",
                        loopLevel, this->getConfigLoopLevel());
          }
          loopTripCount = inputVec->front().uint64();
          inputVec->erase(inputVec->begin());
          inputIdx++;
        }
        DYN_S_DPRINTF(dynS.dynStreamId,
                      "[FixTripCount] LoopLevel %d TripCount %lu x "
                      "TotalTripCount %lu = %lu.\n",
                      loopLevel, loopTripCount, tripCount,
                      loopTripCount * tripCount);
        tripCount *= loopTripCount;
      }
      dynS.setTotalAndInnerTripCount(tripCount);
    }
  }
}

CacheStreamConfigureDataPtr
Stream::allocateCacheConfigureData(uint64_t configSeqNum, bool isIndirect) {
  auto &dynStream = this->getDynStream(configSeqNum);
  auto configData = std::make_shared<CacheStreamConfigureData>(
      this, dynStream.dynStreamId, this->getMemElementSize(),
      dynStream.addrGenFormalParams, dynStream.addrGenCallback);

  // Set the totalTripCount.
  configData->totalTripCount = dynStream.getTotalTripCount();
  configData->innerTripCount = dynStream.getInnerTripCount();

  // Set the predication function.
  configData->predFormalParams = dynStream.predFormalParams;
  configData->predCallback = dynStream.predCallback;

  // Set the store and load func.
  configData->storeFormalParams = dynStream.storeFormalParams;
  configData->storeCallback = dynStream.storeCallback;
  configData->loadFormalParams = dynStream.loadFormalParams;
  configData->loadCallback = dynStream.loadCallback;

  // Set the reduction information.
  if (this->isReduction() || this->isPointerChaseIndVar()) {
    assert(this->getMemElementSize() <= sizeof(StreamValue) &&
           "Cannot offload reduction/ptr chase stream larger than 64 bytes.");
  }
  configData->reductionInitValue = dynStream.initialValue;
  // We need to the FinalReduceValue for core or InnerLoopDepS.
  configData->finalValueNeededByCore =
      this->isInnerFinalValueUsedByCore() || (!this->innerLoopDepEdges.empty());

  // Set the initial vaddr if this is not indirect stream.
  if (!isIndirect) {
    configData->initVAddr =
        dynStream.addrGenCallback
            ->genAddr(0, dynStream.addrGenFormalParams, getStreamValueFail)
            .front();
    // Remember to make line address.
    configData->initVAddr -=
        configData->initVAddr % this->getCPUDelegator()->cacheLineSize();

    Addr initPAddr;
    if (this->getCPUDelegator()->translateVAddrOracle(configData->initVAddr,
                                                      initPAddr)) {
      configData->initPAddr = initPAddr;
      configData->initPAddrValid = true;
    } else {
      /**
       * In case of faulted initVAddr, we simply set the initPAddr to 0
       * and mark it invalid.
       *
       * This is very likely from a misspeculated StreamConfig, thus in
       * StreamFloatController will delay offloading until the StreamConfig
       * is committed.
       *
       * Later the MLC StreamEngine will pick up
       * a physical address that maps to the closes LLC bank and let the
       * stream spin there until we have a valid address.
       */
      configData->initPAddr = 0;
      configData->initPAddrValid = false;
    }
  }

  if (dynStream.hasDepRemoteNestRegion()) {
    configData->hasDepRemoteNestRegion = true;
  }

  return configData;
}

CacheStreamConfigureDataPtr
Stream::allocateCacheConfigureDataForAffineIV(uint64_t configSeqNum) {

  assert(!this->isMemStream() && "This is not an AffineIV.");

  auto &dynS = this->getDynStream(configSeqNum);

  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(dynS.addrGenCallback);
  if (!linearAddrGen) {
    DYN_S_PANIC(dynS.dynStreamId, "This is not an AffineIV.");
  }

  auto configData = std::make_shared<CacheStreamConfigureData>(
      this, dynS.dynStreamId, this->getMemElementSize(),
      dynS.addrGenFormalParams, dynS.addrGenCallback);

  return configData;
}

bool Stream::shouldComputeValue() const {
  if (!this->isMemStream()) {
    // IV/Reduction stream always compute value.
    return true;
  }
  if (this->isStoreComputeStream() || this->isLoadComputeStream() ||
      this->isUpdateStream()) {
    // StoreCompute/LoadCompute/Update stream can also compute.
    // AtomicCompute is handled by sending out the AMO request.
    return true;
  }
  return false;
}

bool Stream::isDirectMemStream() const {
  if (!this->isMemStream()) {
    return false;
  }
  // So far only only one base stream of phi type of the same loop level.
  for (auto baseS : this->addrBaseStreams) {
    if (baseS->isMemStream()) {
      // I depend on a MemStream and am indirect MemStream.
      return false;
    }
    if (!baseS->backBaseStreams.empty()) {
      return false;
    }
  }
  return true;
}

bool Stream::isDirectLoadStream() const {
  return this->isLoadStream() && this->isDirectMemStream();
}

bool Stream::isDirectStoreStream() const {
  return this->isStoreStream() && this->isDirectMemStream();
}

bool Stream::isIndirectLoadStream() const {
  return this->isLoadStream() && !this->isDirectMemStream();
}

bool Stream::isConfigured() const {
  if (this->dynamicStreams.empty() ||
      this->dynamicStreams.back().endDispatched) {
    return false;
  }
  return true;
}

DynStreamId Stream::allocateNewInstance() {
  this->dynInstance++;
  return DynStreamId(this->getCPUId(), this->staticId, this->dynInstance,
                     this->streamName.c_str());
}

StreamElement *Stream::releaseElementUnstepped(DynStream &dynS) {
  auto element = dynS.releaseElementUnstepped();
  if (element) {
    this->allocSize--;
  }
  return element;
}

bool Stream::hasUnsteppedElement(DynStreamId::InstanceId instanceId) {
  if (!this->isConfigured()) {
    // This must be wrong.
    S_DPRINTF(this, "Not configured, so no unstepped element.\n");
    return false;
  }
  if (instanceId == DynStreamId::InvalidInstanceId) {
    auto &dynS = this->getFirstAliveDynStream();
    return dynS.hasUnsteppedElem();
  } else {
    auto &dynS = this->getDynStreamByInstance(instanceId);
    return dynS.hasUnsteppedElem();
  }
}

DynStream &Stream::getFirstAliveDynStream() {
  for (auto &dynS : this->dynamicStreams) {
    if (!dynS.endDispatched) {
      return dynS;
    }
  }
  this->se->dumpFIFO();
  S_PANIC(this, "No Alive DynStream.");
}

DynStream *Stream::tryGetFirstAliveDynStream() {
  for (auto &dynS : this->dynamicStreams) {
    if (!dynS.endDispatched) {
      return &dynS;
    }
  }
  return nullptr;
}

DynStream *Stream::getAllocatingDynStream() {
  if (!this->isConfigured()) {
    return nullptr;
  }
  for (auto &dynS : this->dynamicStreams) {
    if (dynS.endDispatched) {
      continue;
    }
    // Check if we have reached the allocation limit for this one.
    if (dynS.hasTotalTripCount()) {
      if (dynS.FIFOIdx.entryIdx == dynS.getTotalTripCount() + 1) {
        // Notice the +1 for the Element consumed by StreamEnd.
        continue;
      }
    }
    return &dynS;
  }
  return nullptr;
}

StreamElement *Stream::getFirstUnsteppedElement() {
  auto &dynS = this->getFirstAliveDynStream();
  auto element = dynS.getFirstUnsteppedElem();
  if (!element) {
    this->se->dumpFIFO();
    S_PANIC(this,
            "No allocated element to use, TotalTripCount %d, EndDispatched %d.",
            dynS.getTotalTripCount(), dynS.endDispatched);
  }
  return element;
}

StreamElement *Stream::getPrevElement(StreamElement *element) {
  auto dynS = element->dynS;
  // Check if there is no previous element
  auto prevElement = dynS->getPrevElement(element);
  if (prevElement == dynS->tail) {
    // There is no valid previous element.
    return nullptr;
  }
  return prevElement;
}

const ExecFuncPtr &Stream::getComputeCallback() const {
  if (this->isReduction() || this->isPointerChaseIndVar()) {
    // This is a reduction stream.
    auto funcAddrGenCallback =
        std::dynamic_pointer_cast<FuncAddrGenCallback>(this->addrGenCallback);
    if (!funcAddrGenCallback) {
      S_PANIC(this, "ReductionS without FuncAddrGenCallback.");
    }
    return funcAddrGenCallback->getExecFunc();
  } else if (this->isStoreComputeStream()) {
    return this->storeCallback;
  } else if (this->isUpdateStream()) {
    return this->storeCallback;
  } else if (this->isLoadComputeStream()) {
    // So far LoadComputeStream only takes loaded value as input.
    return this->loadCallback;
  } else if (this->isAtomicComputeStream()) {
    /**
     * AtomicOp has two callbacks, here I just return StoreCallback to
     * estimate MicroOps and Latency.
     */
    return this->storeCallback;
  } else {
    S_PANIC(this, "No Computation Callback.");
  }
}

int Stream::getComputationNumMicroOps() const {
  /**
   * Before we correctly reuse the computation in computeMinDistTo() in pntnet2,
   * we override the reduce op microops and latency.
   */
  if (this->isReduction() &&
      this->streamName.find("pntnet2") != std::string::npos &&
      this->streamName.find("computeMinDistTo") != std::string::npos) {
    return 1;
  }
  return this->getComputeCallback()->getNumInstructions();
}

Cycles Stream::getEstimatedComputationLatency() const {
  if (this->isReduction() &&
      this->streamName.find("pntnet2") != std::string::npos &&
      this->streamName.find("computeMinDistTo") != std::string::npos) {
    return Cycles(2);
  }
  return this->getComputeCallback()->getEstimatedLatency();
}

Stream::ComputationCategory Stream::getComputationCategory() const {
  if (this->computationCategoryMemorized) {
    return this->memorizedComputationCategory;
  }
  if (this->isLoadComputeStream()) {
    this->memorizedComputationCategory.first = ComputationType::LoadCompute;
  } else if (this->isStoreComputeStream()) {
    this->memorizedComputationCategory.first = ComputationType::StoreCompute;
  } else if (this->isUpdateStream()) {
    this->memorizedComputationCategory.first = ComputationType::Update;
  } else if (this->isReduction()) {
    this->memorizedComputationCategory.first = ComputationType::Reduce;
  } else if (this->isAtomicComputeStream()) {
    this->memorizedComputationCategory.first = ComputationType::AtomicCompute;
  } else if (this->isPointerChaseIndVar()) {
    // This is pointer chase IV stream. It is address computation.
    this->memorizedComputationCategory.first = ComputationType::Address;
  } else {
    this->memorizedComputationCategory.first =
        ComputationType::UnknownComputationType;
    S_PANIC(this, "Unknown Computation.");
  }

  int numAffineLoadS = 0;
  int numPointerChaseS = 0;
  int numIndirectLoadS = 0;
  for (auto valBaseS : this->valueBaseStreams) {
    if (valBaseS->isDirectLoadStream()) {
      numAffineLoadS++;
    } else if (valBaseS->isPointerChase()) {
      numPointerChaseS++;
    } else if (valBaseS->isLoadStream()) {
      numIndirectLoadS++;
    }
  }
  for (auto backBaseS : this->backBaseStreams) {
    if (backBaseS->isDirectLoadStream()) {
      numAffineLoadS++;
    } else if (backBaseS->isPointerChase()) {
      numPointerChaseS++;
    } else if (backBaseS->isLoadStream()) {
      numIndirectLoadS++;
    }
  }
  // Only consider InnerBaseS if we found no one.
  if (numAffineLoadS == 0 && numPointerChaseS == 0 && numIndirectLoadS == 0) {
    for (const auto &innerBaseE : this->innerLoopBaseEdges) {
      auto innerBaseS = innerBaseE.toStream;
      const auto innerCmpCategory = innerBaseS->getComputationCategory();
      this->memorizedComputationCategory.second = innerCmpCategory.second;
      this->computationCategoryMemorized = true;
      return this->memorizedComputationCategory;
    }
  }

  if (this->isAtomicComputeStream() || this->isLoadComputeStream() ||
      this->isStoreComputeStream() || this->isUpdateStream()) {
    if (numAffineLoadS > 1) {
      this->memorizedComputationCategory.second =
          ComputationAddressPattern::MultiAffine;
    } else if (this->isDirectMemStream()) {
      this->memorizedComputationCategory.second =
          ComputationAddressPattern::Affine;
    } else if (this->isPointerChaseLoadStream()) {
      this->memorizedComputationCategory.second =
          ComputationAddressPattern::PointerChase;
    } else {
      this->memorizedComputationCategory.second =
          ComputationAddressPattern::Indirect;
    }
  } else {
    if (numAffineLoadS > 1) {
      this->memorizedComputationCategory.second =
          ComputationAddressPattern::MultiAffine;
    } else if (numIndirectLoadS > 0) {
      this->memorizedComputationCategory.second =
          ComputationAddressPattern::Indirect;
    } else if (numAffineLoadS == 1) {
      this->memorizedComputationCategory.second =
          ComputationAddressPattern::Affine;
    } else if (numPointerChaseS > 0) {
      this->memorizedComputationCategory.second =
          ComputationAddressPattern::PointerChase;
    } else {
      S_PANIC(this,
              "Unknown Address Pattern: AffineLoad %d IndLoad %d PtrChase %d.",
              numAffineLoadS, numIndirectLoadS, numPointerChaseS);
    }
  }

  this->computationCategoryMemorized = true;
  return this->memorizedComputationCategory;
}

void Stream::recordComputationInCoreStats() const {
  const auto &func = this->getComputeCallback();
  const auto &insts = func->getStaticInsts();
  for (const auto &inst : insts) {
    this->getCPUDelegator()->recordStatsForFakeExecutedInst(inst);
  }
}

std::unique_ptr<StreamAtomicOp>
Stream::setupAtomicOp(FIFOEntryIdx idx, int memElementsize,
                      const DynStreamFormalParamV &formalParams,
                      GetStreamValueFunc getStreamValue) {
  // Turn the FormalParams to ActualParams, except the last atomic
  // operand.
  assert(!formalParams.empty() && "AtomicOp has at least one operand.");
  DynStreamParamV params;
  for (int i = 0; i + 1 < formalParams.size(); ++i) {
    const auto &formalParam = formalParams.at(i);
    if (formalParam.isInvariant) {
      params.push_back(formalParam.invariant);
    } else {
      auto baseStreamId = formalParam.baseStreamId;
      auto baseStreamValue = getStreamValue(baseStreamId);
      params.push_back(baseStreamValue);
    }
  }
  // Push the final atomic operand as a dummy 0.
  assert(!formalParams.back().isInvariant &&
         "AtomicOperand should be a stream.");
  assert(formalParams.back().baseStreamId == this->staticId &&
         "AtomicOperand should be myself.");
  params.emplace_back();
  auto atomicOp =
      std::make_unique<StreamAtomicOp>(this, idx, memElementsize, params,
                                       this->storeCallback, this->loadCallback);
  return atomicOp;
}

void Stream::handleMergedPredicate(const DynStream &dynS,
                                   StreamElement *element) {
  auto mergedPredicatedStreamIds = this->getMergedPredicatedStreams();
  if (!(mergedPredicatedStreamIds.size() > 0 &&
        element->isElemFloatedToCache())) {
    return;
  }
  panic("Deprecated, need refactor.");
  // // Prepare the predicate actual params.
  // uint64_t elementVal = 0;
  // element->getValue(element->addr, element->size,
  //                   reinterpret_cast<uint8_t *>(&elementVal));
  // GetStreamValueFunc getStreamValue = [elementVal,
  //                                      element](uint64_t streamId) ->
  //                                      uint64_t {
  //   assert(streamId == element->FIFOIdx.streamId.staticId &&
  //          "Mismatch stream id for predication.");
  //   return elementVal;
  // };
  // auto params =
  //     convertFormalParamToParam(dynS.predFormalParams, getStreamValue);
  // bool predTrue = dynS.predCallback->invoke(params) & 0x1;
  // for (auto predStreamId : mergedPredicatedStreamIds) {
  //   S_ELEMENT_DPRINTF(element, "Predicate %d %d: %s.\n", predTrue,
  //                     predStreamId.pred_true(), predStreamId.id().name());
  //   if (predTrue != predStreamId.pred_true()) {
  //     continue;
  //   }
  //   auto predS = this->se->getStream(predStreamId.id().id());
  //   // They should be configured by the same configure instruction.
  //   const auto &predDynS = predS->getDynStream(dynS.configSeqNum);
  //   if (predS->getStreamType() == ::LLVM::TDG::StreamInfo_Type_ST) {
  //     auto predElement =
  //     predDynS.getElementByIdx(element->FIFOIdx.entryIdx); if
  //     (!predElement) {
  //       S_ELEMENT_PANIC(element, "Failed to get predicated element.");
  //     }
  //     this->performStore(predDynS, predElement, predDynS.constUpdateValue);
  //   } else {
  //     S_ELEMENT_PANIC(element, "Can only handle merged store stream.");
  //   }
  // }
}

void Stream::performStore(const DynStream &dynS, StreamElement *elem,
                          uint64_t storeValue) {
  /**
   * * Perform const store here.
   * We can not do that in the Ruby because it uses BackingStore, which
   * would cause the core to read the updated value.
   */
  auto elemVAddr = elem->addr;
  auto elemSize = elem->size;
  [[maybe_unused]] auto blockSize = this->getCPUDelegator()->cacheLineSize();
  assert((elemVAddr % blockSize) + elemSize <=
             this->getCPUDelegator()->cacheLineSize() &&
         "Cannot support multi-line element with const update yet.");
  assert(elemSize <= sizeof(uint64_t) && "At most 8 byte element size.");
  Addr elemPAddr;
  panic_if(!this->getCPUDelegator()->translateVAddrOracle(elemVAddr, elemPAddr),
           "Failed to translate address for const update.");
  // ! Hack: Just do a functional write.
  this->getCPUDelegator()->writeToMem(
      elemVAddr, elemSize, reinterpret_cast<const uint8_t *>(&storeValue));
}

void Stream::sampleStatistic() {
  this->statistic.numSample++;
  this->statistic.numInflyRequest += this->numInflyStreamRequests;
  this->statistic.maxSize += this->maxSize;
  this->statistic.allocSize += this->allocSize;
  this->statistic.numDynStreams += this->dynamicStreams.size();
}

void Stream::incrementOffloadedStepped() {
  this->se->numOffloadedSteppedSinceLastCheck++;
}

void Stream::dump() const {
  NO_LOC_INFORM("===== -%d-%d- %50s DynS %d ==========================\n",
                this->getCPUDelegator()->cpuId(), this->staticId,
                this->getStreamName(), this->dynamicStreams.size());
  for (const auto &dynS : this->dynamicStreams) {
    dynS.dump();
  }
}
} // namespace gem5
