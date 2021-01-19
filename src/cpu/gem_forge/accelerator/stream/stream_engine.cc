#include "stream_engine.hh"
#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"
#include "stream_compute_engine.hh"
#include "stream_float_controller.hh"
#include "stream_lsq_callback.hh"
#include "stream_throttler.hh"

#include "base/trace.hh"
#include "debug/CoreRubyStreamLife.hh"
#include "debug/RubyStream.hh"
#include "debug/StreamAlias.hh"
#include "debug/StreamEngine.hh"
#include "debug/StreamThrottle.hh"

namespace {
static std::string DEBUG_STREAM_NAME =
    "(particlefilter.c::415(.omp_outlined..2) 444 bb45 bb91::tmp96(load))";

bool isDebugStream(Stream *S) {
  return S->getStreamName() == DEBUG_STREAM_NAME;
}

} // namespace

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamEngine, format, ##args)

#define DEBUG_TYPE StreamEngine
#include "stream_log.hh"

StreamEngine::StreamEngine(Params *params)
    : GemForgeAccelerator(params), streamPlacementManager(nullptr),
      isOracle(false), writebackCacheLine(nullptr),
      throttler(new StreamThrottler(params->throttling, this)) {

  this->isOracle = params->streamEngineIsOracle;
  this->defaultRunAheadLength = params->defaultRunAheadLength;
  this->currentTotalRunAheadLength = 0;
  this->totalRunAheadLength = params->totalRunAheadLength;
  this->totalRunAheadBytes = params->totalRunAheadBytes;
  this->enableLSQ = params->streamEngineEnableLSQ;
  this->enableCoalesce = params->streamEngineEnableCoalesce;
  this->enableMerge = params->streamEngineEnableMerge;
  this->enableStreamPlacement = params->streamEngineEnablePlacement;
  this->enableStreamPlacementOracle = params->streamEngineEnablePlacementOracle;
  this->enableStreamPlacementBus = params->streamEngineEnablePlacementBus;
  this->noBypassingStore = params->streamEngineNoBypassingStore;
  this->continuousStore = params->streamEngineContinuousStore;
  this->enablePlacementPeriodReset = params->streamEnginePeriodReset;
  this->placementLat = params->streamEnginePlacementLat;
  this->placement = params->streamEnginePlacement;
  this->enableStreamFloat = params->streamEngineEnableFloat;
  this->enableStreamFloatIndirect = params->streamEngineEnableFloatIndirect;
  this->enableStreamFloatPseudo = params->streamEngineEnableFloatPseudo;
  this->enableStreamFloatCancel = params->streamEngineEnableFloatCancel;
  auto streamFloatPolicy = m5::make_unique<StreamFloatPolicy>(
      this->enableStreamFloat, params->streamEngineFloatPolicy);
  this->floatController = m5::make_unique<StreamFloatController>(
      this, std::move(streamFloatPolicy));
  this->computeEngine = m5::make_unique<StreamComputeEngine>(this, params);

  this->initializeFIFO(this->totalRunAheadLength);
}

StreamEngine::~StreamEngine() {
  if (this->streamPlacementManager != nullptr) {
    delete this->streamPlacementManager;
    this->streamPlacementManager = nullptr;
  }

  // Clear all the allocated streams.
  for (auto &streamIdStreamPair : this->streamMap) {
    delete streamIdStreamPair.second;
    streamIdStreamPair.second = nullptr;
  }
  this->streamMap.clear();
  delete[] this->writebackCacheLine;
  this->writebackCacheLine = nullptr;
}

void StreamEngine::handshake(GemForgeCPUDelegator *_cpuDelegator,
                             GemForgeAcceleratorManager *_manager) {
  GemForgeAccelerator::handshake(_cpuDelegator, _manager);

  LLVMTraceCPU *_cpu = nullptr;
  if (auto llvmTraceCPUDelegator =
          dynamic_cast<LLVMTraceCPUDelegator *>(_cpuDelegator)) {
    _cpu = llvmTraceCPUDelegator->cpu;
  }
  // assert(_cpu != nullptr && "Only work for LLVMTraceCPU so far.");
  this->cpu = _cpu;

  this->writebackCacheLine = new uint8_t[cpuDelegator->cacheLineSize()];
  if (this->enableStreamPlacement) {
    this->streamPlacementManager = new StreamPlacementManager(this);
  }

  // Set up the translation buffer.
  this->translationBuffer = m5::make_unique<StreamTranslationBuffer<void *>>(
      cpuDelegator->getDataTLB(),
      [this](PacketPtr pkt, ThreadContext *tc, void *) -> void {
        this->cpuDelegator->sendRequest(pkt);
      },
      false /* AccessLastLevelTLBOnly */, true /* MustDoneInOrder */);
}

void StreamEngine::takeOverBy(GemForgeCPUDelegator *newCpuDelegator,
                              GemForgeAcceleratorManager *newManager) {
  GemForgeAccelerator::takeOverBy(newCpuDelegator, newManager);
}

void StreamEngine::regStats() {
  GemForgeAccelerator::regStats();
  assert(this->manager && "No handshake.");

#define scalar(stat, describe)                                                 \
  this->stat.name(this->manager->name() + (".se." #stat))                      \
      .desc(describe)                                                          \
      .prereq(this->stat)

  scalar(numConfigured, "Number of streams configured.");
  scalar(numStepped, "Number of streams stepped.");
  scalar(numUnstepped, "Number of streams unstepped.");
  scalar(numElementsAllocated, "Number of stream elements allocated.");
  scalar(numElementsUsed, "Number of stream elements used.");
  scalar(numCommittedStreamUser, "Number of committed StreamUser.");
  scalar(entryWaitCycles, "Number of cycles form first check to ready.");
  scalar(numStoreElementsAllocated,
         "Number of store stream elements allocated.");
  scalar(numStoreElementsStepped, "Number of store stream elements fetched.");
  scalar(numStoreElementsUsed, "Number of store stream elements used.");
  scalar(numLoadElementsAllocated, "Number of load stream elements allocated.");
  scalar(numLoadElementsFetched, "Number of load stream elements fetched.");
  scalar(numLoadElementsStepped, "Number of load stream elements fetched.");
  scalar(numLoadElementsUsed, "Number of load stream elements used.");
  scalar(numLoadElementWaitCycles,
         "Number of cycles from first check to ready for load element.");
  scalar(numLoadCacheLineUsed, "Number of cache line used.");
  scalar(numLoadCacheLineFetched, "Number of cache line fetched.");
  scalar(streamUserNotDispatchedByLoadQueue,
         "Number of cycles a stream user cannot dispatch due LQ full.");
  scalar(streamStoreNotDispatchedByStoreQueue,
         "Number of cycles a stream store cannot dispatch due SQ full.");
  scalar(numFloated, "Number of floated streams.");
  scalar(numLLCSentSlice, "Number of LLC sent slices.");
  scalar(numLLCMigrated, "Number of LLC stream migration.");
  scalar(numMLCResponse, "Number of MLCStreamEngine response.");
#undef scalar

  this->numTotalAliveElements.init(0, 1000, 50)
      .name(this->manager->name() + ".stream.numTotalAliveElements")
      .desc("Number of alive stream elements in each cycle.")
      .flags(Stats::pdf);
  this->numTotalAliveCacheBlocks.init(0, 1000, 50)
      .name(this->manager->name() + ".stream.numTotalAliveCacheBlocks")
      .desc("Number of alive cache blocks in each cycle.")
      .flags(Stats::pdf);
  this->numRunAHeadLengthDist.init(0, 15, 1)
      .name(this->manager->name() + ".stream.numRunAHeadLengthDist")
      .desc("Number of run ahead length for streams.")
      .flags(Stats::pdf);
  this->numTotalAliveMemStreams.init(0, 15, 1)
      .name(this->manager->name() + ".stream.numTotalAliveMemStreams")
      .desc("Number of alive memory stream.")
      .flags(Stats::pdf);
  this->numInflyStreamRequestDist.init(0, 16, 1)
      .name(this->manager->name() + ".stream.numInflyStreamDist")
      .desc("Distribution of infly stream requests.")
      .flags(Stats::pdf);

  this->numAccessPlacedInCacheLevel.init(3)
      .name(this->manager->name() + ".stream.numAccessPlacedInCacheLevel")
      .desc("Number of accesses placed in different cache level.")
      .flags(Stats::total);
  this->numAccessHitHigherThanPlacedCacheLevel.init(3)
      .name(this->manager->name() +
            ".stream.numAccessHitHigherThanPlacedCacheLevel")
      .desc("Number of accesses hit in higher level than placed cache.")
      .flags(Stats::total);
  this->numAccessHitLowerThanPlacedCacheLevel.init(3)
      .name(this->manager->name() +
            ".stream.numAccessHitLowerThanPlacedCacheLevel")
      .desc("Number of accesses hit in lower level than placed cache.")
      .flags(Stats::total);

  this->numAccessFootprintL1.init(0, 500, 100)
      .name(this->manager->name() + ".stream.numAccessFootprintL1")
      .desc("Number of accesses with footprint at L1.")
      .flags(Stats::pdf);
  this->numAccessFootprintL2.init(0, 4096, 1024)
      .name(this->manager->name() + ".stream.numAccessFootprintL2")
      .desc("Number of accesses with footprint at L2.")
      .flags(Stats::pdf);
  this->numAccessFootprintL3.init(0, 131072, 26214)
      .name(this->manager->name() + ".stream.numAccessFootprintL3")
      .desc("Number of accesses with footprint at L3.")
      .flags(Stats::pdf);
}

bool StreamEngine::canStreamConfig(const StreamConfigArgs &args) const {
  /**
   * A stream can be configured iff. we can guarantee that it will be
   * allocate one entry when configured.
   *
   * If this this the first time we encounter the stream, we check the
   * number of free entries. Otherwise, we ALSO ensure that allocSize <
   * maxSize.
   */

  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);
  auto configuredStreams = this->enableCoalesce
                               ? streamRegion.coalesced_stream_ids_size()
                               : streamRegion.streams_size();

  // Sanity check on the number of configured streams.
  {
    if (configuredStreams * 3 > this->totalRunAheadLength) {
      panic("Too many streams configuredStreams for %s %d, FIFOSize %d.\n",
            infoRelativePath.c_str(), configuredStreams,
            this->totalRunAheadLength);
    }
  }

  if (this->numFreeFIFOEntries < configuredStreams) {
    // Not enough free entries for each stream.
    return false;
  }

  // Check that allocSize < maxSize.
  if (this->enableCoalesce) {
    for (const auto &streamId : streamRegion.coalesced_stream_ids()) {
      auto iter = this->streamMap.find(streamId);
      if (iter != this->streamMap.end()) {
        // Check if we have quota for this stream.
        auto S = iter->second;
        if (S->getAllocSize() == S->maxSize) {
          // No more quota.
          return false;
        }
      }
    }
  } else {
    for (const auto &streamInfo : streamRegion.streams()) {
      auto streamId = streamInfo.id();
      auto iter = this->streamMap.find(streamId);
      if (iter != this->streamMap.end()) {
        // Check if we have quota for this stream.
        auto S = iter->second;
        if (S->getAllocSize() == S->maxSize) {
          // No more quota.
          return false;
        }
      }
    }
  }
  return true;
}

void StreamEngine::dispatchStreamConfig(const StreamConfigArgs &args) {
  assert(this->canStreamConfig(args) && "Cannot configure stream.");

  this->numConfigured++;
  this->numInflyStreamConfigurations++;
  // We require next tick.
  this->manager->scheduleTickNextCycle();
  assert(this->numInflyStreamConfigurations < 100 &&
         "Too many infly StreamConfigurations.");

  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);

  SE_DPRINTF("Dispatch StreamConfig for %s, %s.\n", streamRegion.region(),
             args.infoRelativePath);

  // Initialize all the streams if this is the first time we encounter the
  // loop.
  for (const auto &streamInfo : streamRegion.streams()) {
    const auto &streamId = streamInfo.id();
    // Remember to also check the coalesced id map.
    if (this->streamMap.count(streamId) == 0 &&
        this->coalescedStreamIdMap.count(streamId) == 0) {
      // We haven't initialize streams in this loop.
      this->initializeStreams(streamRegion);
      break;
    }
  }

  auto configStreams = this->getConfigStreamsInRegion(streamRegion);
  for (auto &S : configStreams) {
    if (S->configured) {
      S_DPRINTF(S, "This stream has already been configured.\n");
      assert(false && "The stream should not be configured.");
    }
    S->configured = true;
    S->statistic.numConfigured++;

    // Notify the stream.
    S->configure(args.seqNum, args.tc);
  }

  // Handle dynamic stream dependence.
  // This is split out from S->configure() to ensure all DynS are created
  // before we handle dynamic dependence.
  for (auto &S : configStreams) {
    auto &dynS = S->getLastDynamicStream();
    dynS.addBaseDynStreams();
  }

  // Allocate one new entries for all streams.
  for (auto S : configStreams) {
    // hack("Allocate element for stream %s.\n",
    // S->getStreamName().c_str());
    assert(this->hasFreeElement());
    assert(S->getAllocSize() < S->maxSize);
    const auto &dynS = S->getLastDynamicStream();
    assert(dynS.areNextBaseElementsAllocated());
    this->allocateElement(S);
  }
}

void StreamEngine::executeStreamConfig(const StreamConfigArgs &args) {

  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);

  SE_DPRINTF("Execute StreamConfig for %s.\n", streamRegion.region());

  auto configStreams = this->getConfigStreamsInRegion(streamRegion);

  /**
   * First notify the stream. This will set up the addr gen function.
   * This has to be done before trying to offload stream.
   */
  for (auto &S : configStreams) {
    const StreamConfigArgs::InputVec *inputVec = nullptr;
    if (args.inputMap) {
      inputVec = &(args.inputMap->at(S->staticId));
    }
    S->executeStreamConfig(args.seqNum, inputVec);
  }

  /**
   * Then we try to compute the reuse between streams.
   * This has to be done after initializing the addr gen function.
   */
  std::list<DynamicStream *> configDynStreams;
  for (auto &S : configStreams) {
    auto &dynS = S->getLastDynamicStream();
    dynS.configureAddrBaseDynStreamReuse();
    configDynStreams.push_back(&dynS);
  }

  /**
   * Then we try to float streams.
   */
  this->floatController->floatStreams(streamRegion, configDynStreams);
}

void StreamEngine::commitStreamConfig(const StreamConfigArgs &args) {
  // So far we don't need to do anything.
}

void StreamEngine::rewindStreamConfig(const StreamConfigArgs &args) {

  const auto &infoRelativePath = args.infoRelativePath;
  const auto &configSeqNum = args.seqNum;
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);

  SE_DPRINTF("Rewind StreamConfig %s.\n", infoRelativePath);

  auto configStreams = this->getConfigStreamsInRegion(streamRegion);

  /**
   * First we need to rewind any floated streams.
   */
  std::vector<DynamicStreamId> floatedIds;
  for (auto &S : configStreams) {
    auto &dynS = S->getLastDynamicStream();
    if (dynS.offloadedToCache) {
      dynS.offloadedToCache = false;
      S->statistic.numFloatRewinded++;
      // Sanity check that we don't break semantics.
      DYN_S_DPRINTF(dynS.dynamicStreamId, "Rewind floated stream.\n");
      if (S->isAtomicComputeStream() || S->isStoreComputeStream()) {
        DYN_S_PANIC(dynS.dynamicStreamId,
                    "Rewind a floated Atomic/StoreCompute stream.");
      }
      if (dynS.offloadedToCacheAsRoot) {
        floatedIds.push_back(dynS.dynamicStreamId);
        dynS.offloadedToCacheAsRoot = false;
      }
    }
  }
  if (!floatedIds.empty()) {
    this->sendStreamFloatEndPacket(floatedIds);
  }

  for (auto &S : configStreams) {
    // This file is already too long, move this to stream.cc.
    S->rewindStreamConfig(configSeqNum);
  }

  this->numInflyStreamConfigurations--;
}

bool StreamEngine::canDispatchStreamStep(uint64_t stepStreamId) const {
  // We check two things:
  // 1. We have an unstepped element.
  // 2. For reduction streams, we need two unstepped elements to ensure
  //    that values are ready.
  auto stepStream = this->getStream(stepStreamId);
  for (auto S : this->getStepStreamList(stepStream)) {
    if (!S->hasUnsteppedElement()) {
      return false;
    }
    if (S->isReduction()) {
      auto &dynS = S->getLastDynamicStream();
      auto element = dynS.getFirstUnsteppedElement();
      assert(element && "We should have unstepped element.");
      if (!element->next) {
        S_ELEMENT_DPRINTF(element, "Cannot dispatch Step for ReductionStream: "
                                   "Missing next UnsteppedElement.\n");
        return false;
      }
    }
  }
  return true;
}

void StreamEngine::dispatchStreamStep(uint64_t stepStreamId) {
  /**
   * For all the streams get stepped, increase the stepped pointer.
   */

  assert(this->canDispatchStreamStep(stepStreamId) &&
         "canDispatchStreamStep assertion failed.");
  this->numStepped++;

  auto stepStream = this->getStream(stepStreamId);

  for (auto S : this->getStepStreamList(stepStream)) {
    assert(S->configured && "Stream should be configured to be stepped.");
    this->stepElement(S);
  }
  /**
   * * Enforce that stepSize is the same within the stepGroup.
   * ! This may be the case if they are configured in different loop level.
   * TODO Fix this corner case.
   */
  // for (auto S : this->getStepStreamList(stepStream)) {
  //   if (S->stepSize != stepStream->stepSize) {
  //     this->dumpFIFO();
  //     panic("Streams within the same stepGroup should have the same
  //     stepSize:
  //     "
  //           "%s, %s.",
  //           S->getStreamName().c_str(),
  //           stepStream->getStreamName().c_str());
  //   }
  // }
  if (isDebugStream(stepStream)) {
  }
}

bool StreamEngine::canCommitStreamStep(uint64_t stepStreamId) {
  auto stepStream = this->getStream(stepStreamId);

  const auto &stepStreams = this->getStepStreamList(stepStream);

  for (auto S : stepStreams) {
    // Since commit happens in-order, we know it's the FirstDynamicStream.
    const auto &dynS = S->getFirstDynamicStream();
    if (!dynS.configExecuted) {
      return false;
    }
    auto stepElement = dynS.tail->next;
    /**
     * For floating streams enabled StoreFunc, we have to check for StreamAck.
     */
    if (S->getEnabledStoreFunc()) {
      if (dynS.offloadedToCache && !dynS.shouldCoreSEIssue()) {
        if (dynS.cacheAckedElements.count(stepElement->FIFOIdx.entryIdx) == 0) {
          S_ELEMENT_DPRINTF(stepElement, "Can not step as no Ack.\n");
          return false;
        }
      }
    }
    // Check for unoffloaded ReductionStream. The next steped element should be
    // ValueReady.
    if (S->isReduction() && !dynS.offloadedToCache) {
      auto stepNextElement = stepElement->next;
      if (!stepNextElement) {
        if (stepElement->FIFOIdx.entryIdx == 0) {
          /**
           * Due to the allocation algorithm, the only case that StepNextElement
           * is not allocated is at the first element, where we are waiting for
           * StreamConfig to be executed.
           */
          S_ELEMENT_DPRINTF(stepElement, "Failed to find StepNextElement.\n");
          return false;
        }
        S_ELEMENT_PANIC(
            stepElement,
            "No StepNextElement for ReductionStream, TotalTripCount %llu.\n",
            dynS.getTotalTripCount());
      }
      if (!stepNextElement->isValueReady) {
        return false;
      }
    }
    if (S->isLoadStream() && !dynS.offloadedToCache && !S->hasCoreUser() &&
        S->hasBackDepReductionStream) {
      /**
       * S is a load stream that is not offloaded, with no core user and
       * reduction stream. We have to make sure the element is value ready so
       * that the reduction is correctly performed.
       */
      if (!stepElement->isValueReady) {
        return false;
      }
    }
    /**
     * Since we coalesce for continuous DirectMemStream, we delay releasing
     * stepped element here if the next element is not addr ready. This is
     * to ensure that it is correctly coalesced.
     */
    if (S->isDirectMemStream() && dynS.shouldCoreSEIssue()) {
      auto stepNextElement = stepElement->next;
      if (!stepNextElement) {
        return false;
      }
      if (!stepNextElement->isAddrReady()) {
        return false;
      }
    }
  }
  return true;
}

void StreamEngine::commitStreamStep(uint64_t stepStreamId) {
  auto stepStream = this->getStream(stepStreamId);

  const auto &stepStreams = this->getStepStreamList(stepStream);

  for (auto S : stepStreams) {
    /**
     * Why only throttle for streamStep?
     * Normally you want to throttling when you release the element.
     * However, so far the throttling is constrainted by the
     * totalRunAheadLength, which only considers configured streams.
     * Therefore, we can not throttle for the last element (streamEnd), as
     * some base streams may already be cleared, and we get an inaccurate
     * totalRunAheadLength, causing the throttling to exceed the limit and
     * deadlock.
     *
     * To solve this, we only do throttling for streamStep.
     */
    this->releaseElementStepped(S, false /* isEnd */, true /* doThrottle */);
  }

  // ! Do not allocate here.
  // ! allocateElements() will handle it.

  if (isDebugStream(stepStream)) {
  }
}

void StreamEngine::rewindStreamStep(uint64_t stepStreamId) {
  this->numUnstepped++;
  auto stepStream = this->getStream(stepStreamId);
  for (auto S : this->getStepStreamList(stepStream)) {
    assert(S->configured && "Stream should be configured to be stepped.");
    this->unstepElement(S);
  }
}

int StreamEngine::getStreamUserLQEntries(const StreamUserArgs &args) const {
  // Only care this if we enable lsq for the stream engine.
  if (!this->enableLSQ) {
    return 0;
  }

  // Collect all the elements used.
  std::unordered_set<StreamElement *> usedElementSet;
  for (const auto &streamId : args.usedStreamIds) {
    auto S = this->getStream(streamId);
    if (!S->configured) {
      // Ignore the out-of-loop use (see dispatchStreamUser).
      continue;
    }
    auto element = S->getFirstUnsteppedElement();
    usedElementSet.insert(element);
  }
  /**
   * The only thing we need to worry about is to check there are
   * enough space in the load queue to hold all the first use of the
   * load stream element.
   */
  auto firstUsedLoadStreamElement = 0;
  for (auto &element : usedElementSet) {
    if (element->stream->getStreamType() != ::LLVM::TDG::StreamInfo_Type_LD) {
      // Not a load stream. Ignore it.
      continue;
    }
    if (element->isFirstUserDispatched()) {
      // Not the first user of the load stream element. Ignore it.
      continue;
    }
    firstUsedLoadStreamElement++;
  }

  return firstUsedLoadStreamElement;
}

int StreamEngine::createStreamUserLSQCallbacks(
    const StreamUserArgs &args, GemForgeLSQCallbackList &callbacks) {
  auto seqNum = args.seqNum;
  auto &elementSet = this->userElementMap.at(seqNum);
  auto numCallbacks = 0;
  for (auto &element : elementSet) {
    if (element == nullptr) {
      continue;
    }
    auto S = element->stream;
    auto dynS = element->dynS;
    if (S->isLoadStream() ||
        (S->isAtomicComputeStream() && !dynS->offloadedToCache)) {
      if (element->firstUserSeqNum == seqNum) {
        // Insert into the load queue if we model the lsq.
        if (this->enableLSQ) {
          assert(numCallbacks < callbacks.size() && "LQCallback overflows.");
          callbacks.at(numCallbacks) = m5::make_unique<StreamLQCallback>(
              element, seqNum, args.pc, args.usedStreamIds);
          numCallbacks++;
        }
      }
    } else if (S->isStoreComputeStream() && !dynS->offloadedToCache) {
      if (element->firstUserSeqNum == seqNum) {
        // Insert into the store queue for StoreStream executed at the core.
        if (!this->enableLSQ) {
          S_ELEMENT_PANIC(element,
                          "StoreStream executed at core requires LSQ.");
        }
        assert(numCallbacks < callbacks.size() && "SQCallback overflows.");
        callbacks.at(numCallbacks) = m5 ::make_unique<StreamSQCallback>(
            element, seqNum, args.pc, args.usedStreamIds);
        numCallbacks++;
      }
    }
  }
  return numCallbacks;
}

bool StreamEngine::hasUnsteppedElement(const StreamUserArgs &args) {
  for (const auto &streamId : args.usedStreamIds) {
    auto S = this->getStream(streamId);
    if (!S->hasUnsteppedElement()) {
      return false;
    }
  }
  return true;
}

bool StreamEngine::hasIllegalUsedLastElement(const StreamUserArgs &args) {
  for (const auto &streamId : args.usedStreamIds) {
    auto S = this->getStream(streamId);
    if (!S->configured) {
      continue;
    }
    if (S->isReduction()) {
      // The only exception is for ReductionStream, whose LastElement is used to
      // convey back the final value.
      continue;
    }
    auto &dynS = S->getLastDynamicStream();
    if (!dynS.configExecuted) {
      continue;
    }
    auto element = dynS.getFirstUnsteppedElement();
    assert(element && "Has no unstepped element.");
    if (element->isLastElement()) {
      S_ELEMENT_DPRINTF(element, "Used LastElement total %d next %s.\n",
                        dynS.totalTripCount, dynS.FIFOIdx);
      return true;
    }
  }
  return false;
}

bool StreamEngine::canDispatchStreamUser(const StreamUserArgs &args) {
  if (!this->hasUnsteppedElement(args)) {
    return false;
  }
  /**
   * Additional condition for StoreStream with enabled StoreFunc, we
   * wait for config to be executed to avoid creating SQCallback for
   * floating store streams.
   */
  for (const auto &streamId : args.usedStreamIds) {
    auto S = this->getStream(streamId);
    auto &dynS = S->getLastDynamicStream();
    if (!dynS.configExecuted) {
      return false;
    }
  }
  return true;
}

void StreamEngine::dispatchStreamUser(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  SE_DPRINTF("Dispatch StreamUser %llu.\n", seqNum);
  assert(this->userElementMap.count(seqNum) == 0);
  assert(this->hasUnsteppedElement(args) && "Don't have used elements.\n");

  auto &elementSet =
      this->userElementMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(seqNum),
                   std::forward_as_tuple())
          .first->second;

  for (const auto &streamId : args.usedStreamIds) {
    auto S = this->getStream(streamId);
    if (!S->hasCoreUser()) {
      // There are two exception for this sanity check.
      // 1. ReductionStream may use LastElement to convey back the final value.
      // 2. StoreComputeStream enabled uses StreamStore inst to get the
      // address and value from the SE so the core can finally write back.
      // 3. AtomicComputeStream always have a StreamAtomic inst as a place
      // holder.
      if (!S->isReduction() && !S->isStoreComputeStream() &&
          !S->isAtomicComputeStream()) {
        S_PANIC(S, "Try to use a stream with no core user.");
      }
    }

    /**
     * It is possible that the stream is unconfigured (out-loop use).
     * In such case we assume it's ready and use a nullptr as a special
     * element
     */
    if (!S->configured) {
      elementSet.insert(nullptr);
    } else {

      assert(!S->isMerged() &&
             "Merged stream should never be used by the core.");

      auto element = S->getFirstUnsteppedElement();
      // Mark the first user sequence number.
      if (!element->isFirstUserDispatched()) {
        element->firstUserSeqNum = seqNum;
        // Remember the first core user pc.
        S->setFirstCoreUserPC(args.pc);
        if (S->trackedByPEB() && element->isReqIssued()) {
          // The element should already be in peb, remove it.
          this->peb.removeElement(element);
        }
      }
      elementSet.insert(element);
      // Construct the elementUserMap.
      this->elementUserMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(element),
                   std::forward_as_tuple())
          .first->second.insert(seqNum);
    }
  }
}

bool StreamEngine::areUsedStreamsReady(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  assert(this->userElementMap.count(seqNum) != 0);

  bool ready = true;
  for (auto &element : this->userElementMap.at(seqNum)) {
    if (element == nullptr) {
      /**
       * Sometimes there is use after stream end,
       * in such case we assume the element is copied to register and
       * is ready.
       */
      continue;
    }
    // Floating StoreStream will only check for Ack when stepping.
    auto S = element->stream;
    if (S->isStoreStream() && S->getEnabledStoreFunc() &&
        element->dynS->offloadedToCache) {
      continue;
    }
    if (!(element->isAddrReady() &&
          element->checkValueReady(true /* CheckedByCore */))) {
      S_ELEMENT_DPRINTF(element, "NotReady: AddrReady %d ValueReady %d.\n",
                        element->isAddrReady(), element->isValueReady);
      ready = false;
    }
  }

  return ready;
}

void StreamEngine::executeStreamUser(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  assert(this->userElementMap.count(seqNum) != 0);

  if (args.values == nullptr) {
    // This is traced base simulation, and they do not require us to provide
    // the value.
    return;
  }
  std::unordered_map<Stream *, StreamElement *> streamToElementMap;
  for (auto &element : this->userElementMap.at(seqNum)) {
    assert(element && "Out-of-loop use after StreamEnd cannot be handled in "
                      "execution-based simulation.");
    auto inserted = streamToElementMap
                        .emplace(std::piecewise_construct,
                                 std::forward_as_tuple(element->stream),
                                 std::forward_as_tuple(element))
                        .second;
    assert(inserted && "Using two elements from the same stream.");
  }
  for (auto streamId : args.usedStreamIds) {
    /**
     * This is necessary, we can not directly use the usedStreamId cause it
     * may be a coalesced stream.
     */
    auto S = this->getStream(streamId);
    auto element = streamToElementMap.at(S);
    args.values->emplace_back();
    /**
     * Make sure we zero out the data.
     */
    args.values->back().fill(0);
    if (element->stream->isStoreStream()) {
      /**
       * This should be a stream store. Just leave it there.
       */
      continue;
    } else {
      /**
       * Read in the value.
       */
      element->getValueByStreamId(streamId, args.values->back().data(),
                                  StreamUserArgs::MaxElementSize);
      S_ELEMENT_DPRINTF(element, "Execute StreamUser, Value %s.\n",
                        GemForgeUtils::dataToString(args.values->back().data(),
                                                    element->size));
    }
  }
}

void StreamEngine::commitStreamUser(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  SE_DPRINTF("Commit StreamUser %llu.\n", seqNum);
  assert(this->userElementMap.count(seqNum) && "UserElementMap not correct.");
  // Remove the entry from the elementUserMap.
  for (auto element : this->userElementMap.at(seqNum)) {
    /**
     * As a hack, we use nullptr to represent an out-of-loop use.
     * TODO: Fix this.
     */
    if (!element) {
      continue;
    }

    if (!element->isValueReady) {
      // The only exception is the StoreStream is floated.
      if (element->stream->isStoreStream() &&
          element->stream->getEnabledStoreFunc() &&
          element->dynS->offloadedToCache) {
      } else {
        S_ELEMENT_PANIC(element, "Commit user, but value not ready.");
      }
    }

    /**
     * Sanity check that no faulted block is used.
     */
    auto S = element->getStream();
    // Dummy way to check the streamId.
    for (auto streamId : args.usedStreamIds) {
      // Check if this streamId corresponding to S.
      if (this->getStream(streamId) != S) {
        continue;
      }
      auto vaddr = element->addr;
      int32_t size = element->size;
      // Handle offset for coalesced stream.
      int32_t offset;
      S->getCoalescedOffsetAndSize(streamId, offset, size);
      vaddr += offset;
      if (element->isValueFaulted(vaddr, size)) {
        S_ELEMENT_PANIC(element, "Commit user of faulted value.");
      }
    }

    auto &userSet = this->elementUserMap.at(element);
    assert(userSet.erase(seqNum) && "Not found in userSet.");
  }
  // Remove the entry in the userElementMap.
  this->userElementMap.erase(seqNum);
  this->numCommittedStreamUser++;
}

void StreamEngine::rewindStreamUser(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  for (auto element : this->userElementMap.at(seqNum)) {
    // The element should be in unstepped state.
    assert(!element->isStepped && "Rewind user of stepped element.");
    if (element->firstUserSeqNum == seqNum) {
      // I am the first user.
      element->firstUserSeqNum = LLVMDynamicInst::INVALID_SEQ_NUM;
      // Check if the element should go back to PEB.
      if (element->stream->trackedByPEB() && element->isReqIssued()) {
        this->peb.addElement(element);
      }
    }
    // Remove the entry from the elementUserMap.
    auto &userSet = this->elementUserMap.at(element);
    assert(userSet.erase(seqNum) && "Not found in userSet.");
  }
  // Remove the entry in the userElementMap.
  this->userElementMap.erase(seqNum);
}

bool StreamEngine::hasUnsteppedElement(const StreamEndArgs &args) {
  const auto &streamRegion = this->getStreamRegion(args.infoRelativePath);
  const auto &endStreamInfos = streamRegion.streams();
  for (auto iter = endStreamInfos.rbegin(), end = endStreamInfos.rend();
       iter != end; ++iter) {
    // Release in reverse order.
    auto streamId = iter->id();
    auto S = this->getStream(streamId);
    if (!S->hasUnsteppedElement()) {
      // We don't have element for this used stream.
      return false;
    }
  }
  return true;
}

void StreamEngine::dispatchStreamEnd(const StreamEndArgs &args) {
  const auto &streamRegion = this->getStreamRegion(args.infoRelativePath);
  const auto &endStreamInfos = streamRegion.streams();

  SE_DPRINTF("Dispatch StreamEnd for %s.\n", streamRegion.region().c_str());
  assert(this->hasUnsteppedElement(args) &&
         "StreamEnd without unstepped elements.");

  /**
   * Dedup the coalesced stream ids.
   */
  std::unordered_set<Stream *> endedStreams;
  for (auto iter = endStreamInfos.rbegin(), end = endStreamInfos.rend();
       iter != end; ++iter) {
    // Release in reverse order.
    auto streamId = iter->id();
    auto S = this->getStream(streamId);
    if (endedStreams.count(S) != 0) {
      continue;
    }
    endedStreams.insert(S);

    // 1. Step one element.
    this->stepElement(S);

    // 2. Mark the dynamicStream as ended.
    S->dispatchStreamEnd(args.seqNum);
    if (isDebugStream(S)) {
      S_DPRINTF(S, "Dispatch End");
    }
  }
}

bool StreamEngine::canExecuteStreamEnd(const StreamEndArgs &args) {
  const auto &streamRegion = this->getStreamRegion(args.infoRelativePath);
  const auto &endStreamInfos = streamRegion.streams();

  SE_DPRINTF("CanExecute StreamEnd for %s.\n", streamRegion.region().c_str());
  /**
   * Dedup the coalesced stream ids.
   */
  std::unordered_set<Stream *> endedStreams;
  for (auto iter = endStreamInfos.rbegin(), end = endStreamInfos.rend();
       iter != end; ++iter) {
    // Release in reverse order.
    auto streamId = iter->id();
    auto S = this->getStream(streamId);
    if (endedStreams.count(S) != 0) {
      continue;
    }
    endedStreams.insert(S);
    // Check for StreamAck. So far that's only floating store stream.
    const auto &dynS = S->getLastDynamicStream();
    if (S->isStoreStream()) {
      if (!dynS.configExecuted || dynS.configSeqNum >= args.seqNum) {
        return false;
      }
      if (dynS.offloadedToCache &&
          dynS.cacheAcked + 1 < dynS.FIFOIdx.entryIdx) {
        // We are not ack the LastElement.
        S_DPRINTF(S, "Cannot execute StreamEnd. Cache acked %llu, need %llu.\n",
                  dynS.cacheAcked, dynS.FIFOIdx.entryIdx);
        return false;
      }
    }
  }
  return true;
}

void StreamEngine::rewindStreamEnd(const StreamEndArgs &args) {
  const auto &streamRegion = this->getStreamRegion(args.infoRelativePath);
  const auto &endStreamInfos = streamRegion.streams();

  SE_DPRINTF("Rewind StreamEnd for %s.\n", streamRegion.region().c_str());

  /**
   * Dedup the coalesced stream ids.
   */
  std::unordered_set<Stream *> endedStreams;
  for (auto iter = endStreamInfos.rbegin(), end = endStreamInfos.rend();
       iter != end; ++iter) {
    // Rewind in reverse order.
    auto streamId = iter->id();
    auto S = this->getStream(streamId);
    if (endedStreams.count(S) != 0) {
      continue;
    }
    endedStreams.insert(S);

    // 1. Restart the last dynamic stream.
    S->rewindStreamEnd(args.seqNum);

    // 2. Unstep one element.
    this->unstepElement(S);
    if (isDebugStream(S)) {
      S_DPRINTF(S, "Rewind End");
    }
  }
}

void StreamEngine::commitStreamEnd(const StreamEndArgs &args) {

  this->numInflyStreamConfigurations--;
  assert(this->numInflyStreamConfigurations >= 0 &&
         "Negative infly StreamConfigurations.");

  const auto &streamRegion = this->getStreamRegion(args.infoRelativePath);
  const auto &endStreamInfos = streamRegion.streams();

  SE_DPRINTF("Commit StreamEnd for %s.\n", streamRegion.region().c_str());

  /**
   * Deduplicate the streams due to coalescing.
   * Releasing is again in two phases:
   * 1. Release all elements first.
   * 2. Release all dynamic streams.
   * This is to ensure that all dynamic streams are released at the same time.
   */
  std::unordered_set<Stream *> endedStreams;
  for (auto iter = endStreamInfos.rbegin(), end = endStreamInfos.rend();
       iter != end; ++iter) {
    // Release in reverse order.
    auto streamId = iter->id();
    auto S = this->getStream(streamId);
    if (endedStreams.count(S) != 0) {
      continue;
    }
    endedStreams.insert(S);

    assert(!S->dynamicStreams.empty() &&
           "Failed to find ended DynamicInstanceState.");
    auto &endedDynS = S->dynamicStreams.front();
    /**
     * Release all unstepped element until there is none.
     */
    while (this->releaseElementUnstepped(endedDynS)) {
    }

    /**
     * Release the last element we stepped at dispatch.
     */
    this->releaseElementStepped(S, true /* isEnd */, false /* doThrottle */);
  }
  std::vector<DynamicStreamId> endedFloatRootIds;
  for (auto S : endedStreams) {
    if (isDebugStream(S)) {
      S_DPRINTF(S, "Commit End");
    }
    assert(!S->dynamicStreams.empty() &&
           "Failed to find ended DynamicInstanceState.");
    auto &endedDynS = S->dynamicStreams.front();
    /**
     * Check if this stream is offloaded and if so, send the StreamEnd
     * packet.
     */
    if (endedDynS.offloadedToCacheAsRoot) {
      endedFloatRootIds.push_back(endedDynS.dynamicStreamId);
    }
    // Notify the stream.
    S->commitStreamEnd(args.seqNum);
  }
  // Finally send out the StreanEnd packet.
  if (!endedFloatRootIds.empty()) {
    this->sendStreamFloatEndPacket(endedFloatRootIds);
  }
}

bool StreamEngine::canStreamStoreDispatch(const StreamStoreInst *inst) const {
  /**
   * * The only requirement about the SQ is already handled in the CPU.
   */
  return true;
}

std::list<std::unique_ptr<GemForgeSQDeprecatedCallback>>
StreamEngine::createStreamStoreSQCallbacks(StreamStoreInst *inst) {
  std::list<std::unique_ptr<GemForgeSQDeprecatedCallback>> callbacks;
  if (!this->enableLSQ) {
    return callbacks;
  }
  // ! This is the old implementation of GemForgeSQDeprecatedCallbacks. Not used
  // any more.
  assert(false && "We are moving to a new implemenation of StreamStore.");
  // // So far we only support LSQ for LLVMTraceCPU.
  // assert(cpuDelegator->cpuType == GemForgeCPUDelegator::CPUTypeE::LLVM_TRACE
  // &&
  //        "LSQ only works for LLVMTraceCPU.");
  // // Find the element to be stored.
  // StreamElement *storeElement = nullptr;
  // auto storeStream =
  // this->getStream(inst->getTDG().stream_store().stream_id()); for (auto
  // element : this->userElementMap.at(inst->getSeqNum())) {
  //   if (element == nullptr) {
  //     continue;
  //   }
  //   if (element->stream == storeStream) {
  //     // Found it.
  //     storeElement = element;
  //     break;
  //   }
  // }
  // assert(storeElement != nullptr && "Failed to found the store element.");
  // callbacks.emplace_back(
  //     new StreamSQDeprecatedCallback(storeElement, inst));
  return callbacks;
}

void StreamEngine::dispatchStreamStore(StreamStoreInst *inst) {
  // So far we just do nothing.
}

void StreamEngine::executeStreamStore(StreamStoreInst *inst) {
  auto seqNum = inst->getSeqNum();
  assert(this->userElementMap.count(seqNum) != 0);
  // Check my element.
  auto storeStream = this->getStream(inst->getTDG().stream_store().stream_id());
  for (auto element : this->userElementMap.at(seqNum)) {
    if (element == nullptr) {
      continue;
    }
    if (element->stream == storeStream) {
      // Found it.
      element->stored = true;
      // Mark the stored element value ready.
      // No one is going to use it.
      if (!element->isValueReady) {
        element->markValueReady();
      }
      break;
    }
  }
}

void StreamEngine::commitStreamStore(StreamStoreInst *inst) {
  if (!this->enableLSQ) {
    return;
  }
  // So far we only support LSQ for LLVMTraceCPU.
  assert(cpuDelegator->cpuType == GemForgeCPUDelegator::CPUTypeE::LLVM_TRACE &&
         "LSQ only works for LLVMTraceCPU.");
}

void StreamEngine::cpuStoreTo(Addr vaddr, int size) {
  if (this->numInflyStreamConfigurations == 0) {
    return;
  }
  if (auto element = this->peb.isHit(vaddr, size)) {
    // hack("CPU stores to (%#x, %d), hits in PEB.\n", vaddr, size);
    S_ELEMENT_DPRINTF_(StreamAlias, element, "CPUStoreTo aliased %#x, +%d.\n",
                       vaddr, size);
    // Remember that this element's address is aliased.
    element->isAddrAliased = true;
    this->flushPEB(vaddr, size);
  }
}

void StreamEngine::initializeStreams(
    const ::LLVM::TDG::StreamRegion &streamRegion) {

  Stream::StreamArguments args;
  args.cpu = cpu;
  args.cpuDelegator = cpuDelegator;
  args.se = this;
  args.maxSize = this->defaultRunAheadLength;
  args.streamRegion = &streamRegion;

  // Sanity check that we do not have too many streams.
  auto totalAliveStreams = this->enableCoalesce
                               ? streamRegion.total_alive_coalesced_streams()
                               : streamRegion.total_alive_streams();
  if (totalAliveStreams * this->defaultRunAheadLength >
      this->totalRunAheadLength) {
    // If there are too many streams, we reduce the maxSize.
    args.maxSize = this->totalRunAheadLength / totalAliveStreams;
    if (args.maxSize < 3) {
      panic("Too many streams %s TotalAliveStreams %d, FIFOSize %d.\n",
            streamRegion.region().c_str(), totalAliveStreams,
            this->totalRunAheadLength);
    }
  }
  SE_DPRINTF_(StreamThrottle,
              "[Throttle] Initialize MaxSize %d TotalAliveStreasm %d\n",
              args.maxSize, totalAliveStreams);

  this->generateCoalescedStreamIdMap(streamRegion, args);

  std::vector<Stream *> createdStreams;
  for (const auto &streamInfo : streamRegion.streams()) {
    const auto &streamId = streamInfo.id();
    // Set per stream field in stream args.
    args.staticId = streamId;
    args.name = streamInfo.name().c_str();

    // Check if this stream belongs to CoalescedStream.
    auto coalescedIter = this->coalescedStreamIdMap.find(streamId);
    assert(coalescedIter != this->coalescedStreamIdMap.end() &&
           "Every stream should be a coalesced stream.");
    auto coalescedStreamId = coalescedIter->second;
    auto coalescedStream = this->streamMap.at(coalescedStreamId);
    assert(coalescedStream && "Illegal type for CoalescedStream.");
    coalescedStream->addStreamInfo(streamInfo);
    // Don't forget to push into created streams.
    if (std::find(createdStreams.begin(), createdStreams.end(),
                  coalescedStream) == createdStreams.end()) {
      createdStreams.push_back(coalescedStream);
    }
  }

  /**
   * Remember to finalize the streams.
   */
  for (auto newStream : createdStreams) {
    newStream->finalize();
  }
  /**
   * ! Hack: Some stream has crazy large element size, e.g. vectorized
   * ! stream_memset, we should limit the maxSize for them to 2.
   * Notice that this doesn't work with throttling so far, but stream_memset
   * has only StoreStream (which is currently not throttled), this should
   * be fine.
   * Currently the threshold is 8 cache lines.
   */
  for (auto newStream : createdStreams) {
    size_t memElementSize = newStream->getMemElementSize();
    if (memElementSize >= cpuDelegator->cacheLineSize() * 8) {
      // For now I just update
      newStream->maxSize =
          std::min(newStream->maxSize, static_cast<size_t>(2ull));
    }
  }
}

void StreamEngine::generateCoalescedStreamIdMap(
    const ::LLVM::TDG::StreamRegion &streamRegion,
    Stream::StreamArguments &args) {

  /**
   * The compiler would provide coalesce info based on offset, however,
   * we would like to split large offset into multiple streams.
   * For example, accessing a 2-D array a[i] and a[i + cols].
   * The offset is cols * sizeof(element), which is a row. These two accesses
   * are not coalesced, as it would require very large element size.
   */
  std::list<std::vector<const ::LLVM::TDG::StreamInfo *>> coalescedGroup;
  // Collect coalesced info and reconstruct coalesced group.
  for (const auto &streamInfo : streamRegion.streams()) {
    const auto &coalesceInfo = streamInfo.coalesce_info();
    auto coalesceGroup = coalesceInfo.base_stream();
    constexpr uint64_t InvalidCoalesceGroup = 0;
    assert(coalesceGroup != InvalidCoalesceGroup && "Invalid CoalesceGroup.");
    // Search for the group. This is O(N^2).
    bool found = false;
    for (auto &group : coalescedGroup) {
      if (group.front()->coalesce_info().base_stream() == coalesceGroup) {
        group.push_back(&streamInfo);
        found = true;
        break;
      }
    }
    if (!found) {
      coalescedGroup.emplace_back();
      coalescedGroup.back().push_back(&streamInfo);
    }
  }
  // Sort each group with increasing order of offset.
  for (auto &group : coalescedGroup) {
    std::sort(group.begin(), group.end(),
              [](const ::LLVM::TDG::StreamInfo *a,
                 const ::LLVM::TDG::StreamInfo *b) -> bool {
                auto offsetA = a->coalesce_info().offset();
                auto offsetB = b->coalesce_info().offset();
                return offsetA < offsetB;
              });
  }
  // Resplit each group dependending on the expansion.
  for (auto groupIter = coalescedGroup.begin();
       groupIter != coalescedGroup.end(); ++groupIter) {
    if (groupIter->size() == 1) {
      // Ignore single streams.
      continue;
    }
    auto baseS = groupIter->front();
    auto baseOffset = baseS->coalesce_info().offset();
    auto endOffset = baseOffset;
    size_t nStream = 0;
    for (auto streamIter = groupIter->begin(), streamEnd = groupIter->end();
         streamIter != streamEnd; ++streamIter, ++nStream) {
      const auto *streamInfo = *streamIter;
      auto offset = streamInfo->coalesce_info().offset();
      if ((!this->enableCoalesce && nStream == 1) || offset > endOffset) {
        /**
         * Split the group if disabled coalescing, or expansion broken.
         */
        assert(nStream != 0 && "Emplty LHS group.");
        coalescedGroup.emplace_back(streamIter, streamEnd);
        groupIter->resize(nStream);
        break;
      } else {
        // The expansion keeps going.
        endOffset = std::max(
            endOffset, offset + streamInfo->static_info().mem_element_size());
      }
    }
  }
  // For each split group, generate a Stream.
  for (auto &group : coalescedGroup) {
    Stream *stream = nullptr;
    uint64_t coalescedStreamId = 0;
    for (int i = 0; i < group.size(); ++i) {
      const auto &streamInfo = *(group[i]);
      const auto &streamId = streamInfo.id();
      args.staticId = streamId;
      args.name = streamInfo.name().c_str();
      if (i == 0) {
        // The first stream is the leading stream.
        /**
         * I know this is confusing. But there are two possible interpretation
         * of coalesce group: trace-based ane execution-based.
         * Both version uses coalesce base stream id as the coalesce group.
         * 1. In the old trace based implementation, coalesced stream will have
         *    offset -1 (we don't calculate the offset in transformation).
         * 2. In the static transform implementation, the offset should be >= 0.
         * NOTE: Now we always use new execution-based coalescing, even in trace
         * simulation.
         */

        // TODO: I don't like this design, all initialization should happen
        // TODO: in the function initializeStreams().
        stream = new Stream(args);
        coalescedStreamId = streamId;
        this->streamMap.emplace(coalescedStreamId, stream);
      }
      this->coalescedStreamIdMap.emplace(streamId, coalescedStreamId);
    }
  }
}

Stream *StreamEngine::getStream(uint64_t streamId) const {
  if (this->coalescedStreamIdMap.count(streamId)) {
    streamId = this->coalescedStreamIdMap.at(streamId);
  }
  auto iter = this->streamMap.find(streamId);
  if (iter == this->streamMap.end()) {
    panic("Failed to find stream %lu.\n", streamId);
  }
  return iter->second;
}

Stream *StreamEngine::tryGetStream(uint64_t streamId) const {
  if (this->coalescedStreamIdMap.count(streamId)) {
    streamId = this->coalescedStreamIdMap.at(streamId);
  }
  auto iter = this->streamMap.find(streamId);
  if (iter == this->streamMap.end()) {
    return nullptr;
  }
  return iter->second;
}

void StreamEngine::tick() {
  this->allocateElements();
  this->issueElements();
  this->computeEngine->startComputation();
  this->computeEngine->completeComputation();
  if (curTick() % 10000 == 0) {
    this->updateAliveStatistics();
  }

  if (this->numInflyStreamConfigurations > 0) {
    // We require next tick.
    this->manager->scheduleTickNextCycle();
  }
}

void StreamEngine::updateAliveStatistics() {
  int totalAliveElements = 0;
  int totalAliveMemStreams = 0;
  std::unordered_set<Addr> totalAliveCacheBlocks;
  this->numRunAHeadLengthDist.reset();
  for (const auto &streamPair : this->streamMap) {
    const auto &stream = streamPair.second;
    if (stream->isMemStream()) {
      this->numRunAHeadLengthDist.sample(stream->getAllocSize());
    }
    if (!stream->configured) {
      continue;
    }
    if (stream->isMemStream()) {
      totalAliveMemStreams++;
    }
    stream->statistic.sampleStats(stream->numInflyStreamRequests,
                                  stream->maxSize);
  }
  this->numTotalAliveElements.sample(totalAliveElements);
  this->numTotalAliveCacheBlocks.sample(totalAliveCacheBlocks.size());
  this->numTotalAliveMemStreams.sample(totalAliveMemStreams);
  // Update infly StreamRequests distribution.
  this->numInflyStreamRequestDist.sample(this->numInflyStreamRequests);
}

void StreamEngine::initializeFIFO(size_t totalElements) {
  panic_if(!this->FIFOArray.empty(), "FIFOArray has already been initialized.");

  this->FIFOArray.reserve(totalElements);
  while (this->FIFOArray.size() < totalElements) {
    this->FIFOArray.emplace_back(this);
  }
  this->FIFOFreeListHead = nullptr;
  this->numFreeFIFOEntries = 0;
  for (auto &element : this->FIFOArray) {
    this->addFreeElement(&element);
  }
}

void StreamEngine::addFreeElement(StreamElement *element) {
  element->clearInflyMemAccesses();
  element->clear();
  element->next = this->FIFOFreeListHead;
  this->FIFOFreeListHead = element;
  this->numFreeFIFOEntries++;
}

StreamElement *StreamEngine::removeFreeElement() {
  assert(this->hasFreeElement() && "No free element to remove.");
  auto newElement = this->FIFOFreeListHead;
  this->FIFOFreeListHead = this->FIFOFreeListHead->next;
  this->numFreeFIFOEntries--;
  newElement->clear();
  return newElement;
}

bool StreamEngine::hasFreeElement() const {
  return this->numFreeFIFOEntries > 0;
}

const std::list<Stream *> &
StreamEngine::getStepStreamList(Stream *stepS) const {
  assert(stepS != nullptr && "stepS is nullptr.");
  if (this->memorizedStreamStepListMap.count(stepS) != 0) {
    return this->memorizedStreamStepListMap.at(stepS);
  }
  /**
   * Create the list by topological sort.
   */
  std::list<Stream *> stepList;
  std::list<Stream *> stack;
  std::unordered_map<Stream *, int> stackStatusMap;

  auto pushToStack = [&stack, &stackStatusMap](Stream *S) -> void {
    auto status = stackStatusMap.emplace(S, 0).first->second;
    if (status == 1) {
      // Cycle dependence found.
      panic("Cycle dependence found %s.", S->getStreamName());
    } else if (status == 2) {
      // This one has already dumped.
      return;
    } else {
      // This one has not been visited.
      stack.emplace_back(S);
    }
  };

  stack.emplace_back(stepS);
  stackStatusMap.emplace(stepS, 0);
  while (!stack.empty()) {
    auto S = stack.back();
    if (stackStatusMap.at(S) == 0) {
      // First time.
      for (auto depS : S->addrDepStreams) {
        if (depS->getLoopLevel() != stepS->getLoopLevel()) {
          continue;
        }
        pushToStack(depS);
      }
      // Value dep stream.
      for (auto depS : S->valueDepStreams) {
        if (depS->getLoopLevel() != stepS->getLoopLevel()) {
          continue;
        }
        pushToStack(depS);
      }
      /**
       * Also respect the merged predicated relationship.
       */
      for (auto predStreamId : S->getMergedPredicatedStreams()) {
        auto predS = this->getStream(predStreamId.id().id());
        assert(predS->stepRootStream == stepS &&
               "PredicatedStream should have same step root.");
        pushToStack(predS);
      }
      stackStatusMap.at(S) = 1;
    } else if (stackStatusMap.at(S) == 1) {
      // Second time.
      stepList.emplace_front(S);
      stack.pop_back();
      stackStatusMap.at(S) = 2;
    } else {
      // Third time, ignore it as the stream is already in the list.
      stack.pop_back();
    }
  }

  return this->memorizedStreamStepListMap
      .emplace(std::piecewise_construct, std::forward_as_tuple(stepS),
               std::forward_as_tuple(stepList))
      .first->second;
}

std::list<Stream *> StreamEngine::getConfigStreamsInRegion(
    const LLVM::TDG::StreamRegion &streamRegion) {
  /**
   * Get all the configured streams.
   */
  std::list<Stream *> configStreams;
  std::unordered_set<Stream *> dedupSet;
  for (const auto &streamInfo : streamRegion.streams()) {
    // Deduplicate the streams due to coalescing.
    const auto &streamId = streamInfo.id();
    auto stream = this->getStream(streamId);
    if (dedupSet.count(stream) == 0) {
      configStreams.push_back(stream);
      dedupSet.insert(stream);
    }
  }
  return configStreams;
}

void StreamEngine::allocateElements() {
  /**
   * Try to allocate more elements for configured streams.
   * Set a target, try to make sure all streams reach this target.
   * Then increment the target.
   */
  std::vector<Stream *> configuredStepRootStreams;
  for (const auto &IdStream : this->streamMap) {
    auto S = IdStream.second;
    if (S->stepRootStream == S && S->configured &&
        S->getLastDynamicStream().configExecuted) {
      // This is a StepRootStream, with StreamConfig executed.
      configuredStepRootStreams.push_back(S);
    }
  }

  // Sort by the allocated size.
  std::sort(configuredStepRootStreams.begin(), configuredStepRootStreams.end(),
            [](Stream *SA, Stream *SB) -> bool {
              return SA->getAllocSize() < SB->getAllocSize();
            });

  for (auto stepRootStream : configuredStepRootStreams) {

    /**
     * ! A hack here to delay the allocation if the back base stream has
     * ! not caught up.
     */
    auto maxAllocSize = stepRootStream->maxSize;
    if (!stepRootStream->backBaseStreams.empty()) {
      for (auto backBaseS : stepRootStream->backBaseStreams) {
        if (backBaseS->stepRootStream == stepRootStream) {
          // ! This is acutally a pointer chasing pattern.
          // ! No constraint should be enforced here.
          continue;
        }
        if (backBaseS->stepRootStream == nullptr) {
          // ! THis is actually a constant load.
          // ! So far ignore this dependence.
          continue;
        }
        if (backBaseS->getAllocSize() < maxAllocSize) {
          // The back base stream is lagging behind.
          // Reduce the maxAllocSize.
          maxAllocSize = backBaseS->getAllocSize();
        }
      }
    }

    /**
     * Limit the maxAllocSize with totalTripCount to avoid allocation beyond
     * StreamEnd. Condition: maxAllocSize > allocSize: originally we are trying
     * to allocate more.
     * ! We allow (totalTripCount + 1) elements as StreamEnd would consume one
     * ! element.
     */
    {
      auto allocSize = stepRootStream->getAllocSize();
      auto &stepRootDynStream = stepRootStream->getLastDynamicStream();
      if (stepRootDynStream.totalTripCount > 0 && maxAllocSize > allocSize) {
        auto nextEntryIdx = stepRootDynStream.FIFOIdx.entryIdx;
        auto maxTripCount = stepRootDynStream.totalTripCount + 1;
        if (nextEntryIdx >= maxTripCount) {
          // We are already overflowed, set maxAllocSize to allocSize to stop
          // allocating. NOTE: This should not happen at all.
          maxAllocSize = allocSize;
        } else {
          maxAllocSize =
              std::min(maxAllocSize, (maxTripCount - nextEntryIdx) + allocSize);
        }
      }
    }

    const auto &stepStreams = this->getStepStreamList(stepRootStream);
    const auto &stepRootDynStream = stepRootStream->getLastDynamicStream();
    for (size_t targetSize = 1;
         targetSize <= maxAllocSize && this->hasFreeElement(); ++targetSize) {
      for (auto S : stepStreams) {
        if (!this->hasFreeElement()) {
          break;
        }
        if (!S->configured) {
          continue;
        }
        if (S->getAllocSize() >= targetSize) {
          continue;
        }
        if (S->getAllocSize() >= S->maxSize) {
          continue;
        }
        const auto &dynS = S->getLastDynamicStream();
        if (!dynS.areNextBaseElementsAllocated()) {
          continue;
        }
        if (S != stepRootStream) {
          if (S->getAllocSize() >= stepRootStream->getAllocSize()) {
            // It doesn't make sense to allocate ahead than the step root.
            continue;
          }
          if (dynS.allocSize >= stepRootDynStream.allocSize) {
            // It also doesn't make sense to allocate ahead than root dynS.
            continue;
          }
        }
        this->allocateElement(S);
      }
    }
  }
}

void StreamEngine::allocateElement(Stream *S) {
  assert(this->hasFreeElement());
  auto newElement = this->removeFreeElement();
  this->numElementsAllocated++;
  if (S->getStreamType() == ::LLVM::TDG::StreamInfo_Type_LD) {
    this->numLoadElementsAllocated++;
  } else if (S->getStreamType() == ::LLVM::TDG::StreamInfo_Type_ST) {
    this->numStoreElementsAllocated++;
  }

  S->allocateElement(newElement);
}

void StreamEngine::releaseElementStepped(Stream *S, bool isEnd,
                                         bool doThrottle) {

  /**
   * This function performs a normal release, i.e. release a stepped
   * element.
   */

  auto releaseElement = S->releaseElementStepped(isEnd);
  /**
   * How to handle short streams?
   * There is a pathological case when the streams are short, and
   * increasing the run ahead length beyond the stream length does not
   * make sense. We do not throttle if the element is within the run ahead
   * length.
   */
  if (doThrottle) {
    this->throttler->throttleStream(releaseElement);
  }

  const bool used = releaseElement->isFirstUserDispatched();
  if (releaseElement->isLastElement() && !S->isReduction()) {
    assert(!used && "LastElement of NonReductionStream released being used.");
  }

  /**
   * Sanity check that all the user are done with this element.
   */
  if (this->elementUserMap.count(releaseElement) != 0) {
    assert(this->elementUserMap.at(releaseElement).empty() &&
           "Some unreleased user instruction.");
  }

  if (S->isLoadStream()) {
    this->numLoadElementsStepped++;
    if (used) {
      this->numLoadElementsUsed++;
      // Update waited cycle information.
      auto waitedCycles = 0;
      if (releaseElement->valueReadyCycle >
          releaseElement->firstValueCheckCycle) {
        waitedCycles = releaseElement->valueReadyCycle -
                       releaseElement->firstValueCheckCycle;
      }
      this->numLoadElementWaitCycles += waitedCycles;
    }
  } else if (S->isStoreStream()) {
    this->numStoreElementsStepped++;
    if (used) {
      this->numStoreElementsUsed++;
    }
  }

  /**
   * For a issued element, if unused, it should be removed from PEB.
   */
  if (S->trackedByPEB() && releaseElement->isReqIssued()) {
    if (used) {
      if (this->peb.contains(releaseElement)) {
        S_ELEMENT_PANIC(releaseElement,
                        "Used element still in PEB when released.");
      }
    } else {
      this->peb.removeElement(releaseElement);
    }
  }

  // Decrease the reference count of the cache blocks.
  if (this->enableMerge) {
    for (int i = 0; i < releaseElement->cacheBlocks; ++i) {
      auto cacheBlockVAddr =
          releaseElement->cacheBlockBreakdownAccesses[i].cacheBlockVAddr;
      auto &cacheBlockInfo = this->cacheBlockRefMap.at(cacheBlockVAddr);
      if (used) {
        cacheBlockInfo.used = true;
      }
      cacheBlockInfo.reference--;
      if (cacheBlockInfo.reference == 0) {
        // Remember to remove the pendingAccesses.
        for (auto &pendingAccess : cacheBlockInfo.pendingAccesses) {
          pendingAccess->handleStreamEngineResponse();
        }
        if (cacheBlockInfo.used && cacheBlockInfo.requestedByLoad) {
          this->numLoadCacheLineUsed++;
        }
        this->cacheBlockRefMap.erase(cacheBlockVAddr);
      }
    }
  }

  this->addFreeElement(releaseElement);
}

bool StreamEngine::releaseElementUnstepped(DynamicStream &dynS) {
  auto S = dynS.stream;
  auto releaseElement = S->releaseElementUnstepped(dynS);
  if (releaseElement) {
    if (S->trackedByPEB() && releaseElement->isReqIssued()) {
      // This should be in PEB.
      this->peb.removeElement(releaseElement);
    }
    this->addFreeElement(releaseElement);
  }
  return releaseElement != nullptr;
}

void StreamEngine::stepElement(Stream *S) { S->stepElement(); }

void StreamEngine::unstepElement(Stream *S) { S->unstepElement(); }

std::vector<StreamElement *> StreamEngine::findReadyElements() {
  std::vector<StreamElement *> readyElements;

  auto areBaseElementsValReady = [this](StreamElement *element) -> bool {
    bool ready = true;
    S_ELEMENT_DPRINTF(element, "Check if base element is ready.\n");
    for (const auto &baseElement : element->addrBaseElements) {
      if (baseElement->stream == nullptr) {
        S_ELEMENT_PANIC(element, "BaseElement has no stream.\n");
      }
      if (element->stream->addrBaseStreams.count(baseElement->stream) == 0 &&
          element->stream->backBaseStreams.count(baseElement->stream) == 0) {
        // ! For reduction stream, myself is not in baseStreams.
        if (!element->stream->isReduction()) {
          S_ELEMENT_PANIC(element, "Different base streams from %s.\n",
                          baseElement->FIFOIdx);
        }
      }
      S_ELEMENT_DPRINTF(baseElement, "BaseElement Ready %d.\n",
                        baseElement->isValueReady);
      if (!baseElement->checkValueReady(false /* CheckByCore */)) {
        ready = false;
        break;
      }
    }
    return ready;
  };

  /**
   * We iterate through all configured streams' elements.
   * In this way, elements are marked ready in order, i.e. if
   * one element is not ready then we break searching in this stream.
   */
  for (const auto &idStream : this->streamMap) {
    auto S = idStream.second;
    for (auto &dynS : S->dynamicStreams) {
      if (!dynS.configExecuted) {
        // The StreamConfig has not been executed, do not issue.
        continue;
      }
      for (auto element = dynS.tail->next; element != nullptr;
           element = element->next) {
        assert(element->stream == S && "Sanity check that streams match.");

        /**
         * There three types of ready elements:
         * 1. All the address base elements are ready -> compute address.
         *  This represents all Mem streams.
         * 2. Address is ready, value base elements are ready -> compute value.
         *  This applies to IV/Reduction/StoreCompute streams.
         * 3. Address is ready, request not issued, first user is
         * non-speculative
         *  -> Issue the atomic request.
         *  This applies to AtomicCompute streams.
         */

        if (element->isAddrReady()) {
          // Address already ready. Check if we have type 2 or 3 ready elements.
          if (S->shouldComputeValue() && !element->isValueReady &&
              !element->scheduledComputation) {
            if (element->checkValueBaseElementsValueReady()) {
              S_ELEMENT_DPRINTF(element, "Found Value Ready.\n");
              readyElements.emplace_back(element);
            }
          }
          if (S->isAtomicComputeStream() && !element->dynS->offloadedToCache &&
              !element->isReqIssued()) {
            // Check that StreamAtomic inst is non-speculative, i.e. it checks
            // if my value is ready.
            if (element->firstValueCheckByCoreCycle != 0) {
              S_ELEMENT_DPRINTF(
                  element,
                  "StreamAtomic is non-speculative, ready to issue.\n");
              readyElements.emplace_back(element);
            }
          }
          continue;
        }
        /**
         * To avoid overhead, if an element is aliased, we do not try to
         * issue it until the first user is dispatched. However, now we
         * have removed the load b[i] for indirect access like a[b[i]],
         * we need to further check if my dependent stream has user dispatched
         * to avoid deadlock.
         *
         * TODO: This adhoc fix only works for one level, if it's a[b[c]] then
         * TODO: it will deadlock again.
         * TODO: Record dependent element information to avoid this expensive
         * TODO: search.
         */
        if (element->isAddrAliased && !element->isFirstUserDispatched()) {
          bool hasIndirectUserDispatched = false;
          for (auto depS : S->addrDepStreams) {
            auto &dynDepS = depS->getDynamicStream(dynS.configSeqNum);
            auto depElement =
                dynDepS.getElementByIdx(element->FIFOIdx.entryIdx);
            if (depElement && depElement->isFirstUserDispatched()) {
              hasIndirectUserDispatched = true;
            }
          }
          if (!hasIndirectUserDispatched) {
            break;
          }
        }
        auto baseElementsValReady = areBaseElementsValReady(element);
        if (baseElementsValReady) {
          S_ELEMENT_DPRINTF(element, "Found Addr Ready.\n");
          readyElements.emplace_back(element);
        } else {
          // We should not check the next one as we should issue inorder.
          S_ELEMENT_DPRINTF(element, "Not Addr Ready, break out.\n");
          break;
        }
      }
    }
  }

  return readyElements;
}

void StreamEngine::issueElements() {
  // Find all ready elements.
  auto readyElements = this->findReadyElements();

  /**
   * Sort the ready elements by create cycle and relative order within
   * the single stream.
   */
  std::sort(readyElements.begin(), readyElements.end(),
            [](const StreamElement *A, const StreamElement *B) -> bool {
              if (A->allocateCycle != B->allocateCycle) {
                return A->allocateCycle < B->allocateCycle;
              }
              if (A->stream != B->stream) {
                // Break the time by stream address.
                return reinterpret_cast<uint64_t>(A->stream) <
                       reinterpret_cast<uint64_t>(B->stream);
              }
              const auto &AIdx = A->FIFOIdx;
              const auto &BIdx = B->FIFOIdx;
              return BIdx > AIdx;
            });
  for (auto &element : readyElements) {

    if (element->isAddrReady() && element->stream->shouldComputeValue()) {
      // Type 2 ready elements.
      element->computeValue();
      continue;
    }

    if (!element->isAddrReady()) {
      element->markAddrReady();
    }

    if (element->stream->isMemStream()) {
      assert(!element->isReqIssued() && "Element already issued request.");

      /**
       * * New Feature: If the stream is merged, then we do not issue.
       * * The stream should never be used by the core.
       */
      if (element->stream->isMerged()) {
        continue;
      }

      if (!element->shouldIssue()) {
        continue;
      }

      /**
       * AtomicComputeStream will mark the AddrReady, but delay issuing
       * until the StreamAtomic instruction is non-speculative, which
       * means it started to check if element value is ready for writeback.
       * So we check the firstValueCheckByCoreCycle.
       * If the stream is floated, then we can immediately issue.
       */
      if (element->stream->isAtomicComputeStream() &&
          !element->dynS->offloadedToCache &&
          element->firstValueCheckByCoreCycle == 0) {
        S_ELEMENT_DPRINTF(
            element, "Delay issue as waiting for FirstValueCheckByCore.\n");
        continue;
      }

      // Increase the reference of the cache block if we enable merging.
      if (this->enableMerge) {
        for (int i = 0; i < element->cacheBlocks; ++i) {
          auto cacheBlockAddr =
              element->cacheBlockBreakdownAccesses[i].cacheBlockVAddr;
          this->cacheBlockRefMap
              .emplace(std::piecewise_construct,
                       std::forward_as_tuple(cacheBlockAddr),
                       std::forward_as_tuple())
              .first->second.reference++;
        }
      }
      // Issue the element.
      this->issueElement(element);
    }
  }
}
void StreamEngine::fetchedCacheBlock(Addr cacheBlockVAddr,
                                     StreamMemAccess *memAccess) {
  // Check if we still have the cache block.
  if (!this->enableMerge) {
    return;
  }
  if (this->cacheBlockRefMap.count(cacheBlockVAddr) == 0) {
    return;
  }
  auto &cacheBlockInfo = this->cacheBlockRefMap.at(cacheBlockVAddr);
  cacheBlockInfo.status = CacheBlockInfo::Status::FETCHED;
  // Notify all the pending streams.
  for (auto &pendingMemAccess : cacheBlockInfo.pendingAccesses) {
    assert(pendingMemAccess != memAccess &&
           "pendingMemAccess should not be fetching access.");
    pendingMemAccess->handleStreamEngineResponse();
  }
  // Remember to clear the pendingAccesses, as they are now released.
  cacheBlockInfo.pendingAccesses.clear();
}

void StreamEngine::issueElement(StreamElement *element) {
  assert(element->isAddrReady() && "Address should be ready.");
  assert(element->stream->isMemStream() &&
         "Should never issue element for IVStream.");
  assert(element->shouldIssue() && "Should not issue this element.");
  assert(!element->isReqIssued() && "Element req already issued.");

  auto S = element->stream;
  auto dynS = element->dynS;
  if (element->flushed) {
    if (!S->trackedByPEB()) {
      S_ELEMENT_PANIC(element, "Flushed Non-PEB stream element.");
    }
    S_ELEMENT_DPRINTF(element, "Issue - Reissue.\n");
  } else {
    S_ELEMENT_DPRINTF(element, "Issue.\n");
  }
  if (S->isLoadStream()) {
    this->numLoadElementsFetched++;
  }
  S->statistic.numFetched++;
  element->setReqIssued();
  if (S->trackedByPEB() && !element->isFirstUserDispatched()) {
    // Add to the PEB if the first user has not been dispatched.
    this->peb.addElement(element);
  }

  /**
   * A quick hack to coalesce continuous elements that completely overlap.
   */
  this->coalesceContinuousDirectMemStreamElement(element);

  for (size_t i = 0; i < element->cacheBlocks; ++i) {
    auto &cacheBlockBreakdown = element->cacheBlockBreakdownAccesses[i];

    // Normal case: really fetching this from the cache,
    // i.e. not merged & not handled by placement manager.
    // ! Always fetch the whole cache line, this is an
    // ! optimization for continuous load stream.
    // TODO: Continuous load stream should really be allocated in
    // TODO: granularity of cache lines (not stream elements).

    // Check if this cache line is already done.
    if (cacheBlockBreakdown.state !=
        CacheBlockBreakdownAccess::StateE::Initialized) {
      continue;
    }

    const auto cacheLineVAddr = cacheBlockBreakdown.cacheBlockVAddr;
    const auto cacheLineSize = cpuDelegator->cacheLineSize();
    if ((cacheLineVAddr % cacheLineSize) != 0) {
      S_ELEMENT_PANIC(element,
                      "CacheBlock %d LineVAddr %#x invalid, VAddr %#x.\n", i,
                      cacheLineVAddr, cacheBlockBreakdown.virtualAddr);
    }
    Addr cacheLinePAddr;
    if (!cpuDelegator->translateVAddrOracle(cacheLineVAddr, cacheLinePAddr)) {
      S_ELEMENT_DPRINTF(element, "Fault on vaddr %#x.\n", cacheLineVAddr);
      cacheBlockBreakdown.state = CacheBlockBreakdownAccess::StateE::Faulted;
      /**
       * The current mechanism to mark value ready is too hacky.
       * We rely on the setValue() to call tryMarkValueReady().
       * However, since Faulted is also considered ready, we have to
       * call tryMarkValueReady() whenver we set a block to Faulted state.
       * TODO: Improve this poor design.
       */
      element->tryMarkValueReady();
      continue;
    }
    if ((cacheLinePAddr % cacheLineSize) != 0) {
      S_ELEMENT_PANIC(
          element, "LinePAddr %#x invalid, LineVAddr %#x, VAddr %#x.\n",
          cacheLinePAddr, cacheLineVAddr, cacheBlockBreakdown.virtualAddr);
    }

    // Allocate the book-keeping StreamMemAccess.
    auto memAccess = element->allocateStreamMemAccess(cacheBlockBreakdown);
    PacketPtr pkt = nullptr;
    if (S->isAtomicStream() && S->getEnabledStoreFunc()) {
      if (element->cacheBlocks != 1) {
        S_ELEMENT_PANIC(element, "Illegal # of CacheBlocks %d for AtomicOp.",
                        element->cacheBlocks);
      }
      /**
       * It is the programmer/compiler's job to make sure no aliasing for
       * computation (i.e. StoreFunc), so the element should be flushed.
       */
      if (element->flushed) {
        S_ELEMENT_PANIC(element,
                        "AtomicStream with StoreFunc should not be flushed.");
      }
      if (!dynS->offloadedToCache) {
        // Special case to handle computation for atomic stream at CoreSE.
        auto atomicOp = S->setupAtomicOp(element->FIFOIdx, element->size,
                                         dynS->storeFormalParams);
        // * We should use element address here, not line address.
        auto elementVAddr = cacheBlockBreakdown.virtualAddr;
        auto lineOffset = elementVAddr % cacheLineSize;
        auto elementPAddr = cacheLinePAddr + lineOffset;

        pkt = GemForgePacketHandler::createGemForgeAMOPacket(
            elementVAddr, elementPAddr, element->size, memAccess,
            cpuDelegator->dataMasterId(), 0 /* ContextId */,
            S->getFirstCoreUserPC() /* PC */, std::move(atomicOp));
      } else {
        /**
         * For offloaded atomic stream, we issue normal load requests,
         * but marked NO_RUBY_BACK_STORE to avoid RubySequencer overwrite
         * the result with backing storage, and NO_RUBY_SEQUENCER_COALESCE
         * to avoid being load-to-load forwarded.
         */
        Request::Flags flags;
        flags.set(Request::NO_RUBY_BACK_STORE);
        flags.set(Request::NO_RUBY_SEQUENCER_COALESCE);
        pkt = GemForgePacketHandler::createGemForgePacket(
            cacheLinePAddr, cacheLineSize, memAccess, nullptr /* Data */,
            cpuDelegator->dataMasterId(), 0 /* ContextId */,
            S->getFirstCoreUserPC() /* PC */, flags);
        pkt->req->setVirt(cacheLineVAddr);
      }
    } else {
      /**
       * For offloaded streams, they rely on this request to advance in MLC.
       * Disable RubySequencer coalescing for that.
       * Unless this is a reissue request, which should be treated normally.
       */
      Request::Flags flags;
      if (dynS->offloadedToCache && !element->flushed) {
        flags.set(Request::NO_RUBY_SEQUENCER_COALESCE);
      }
      pkt = GemForgePacketHandler::createGemForgePacket(
          cacheLinePAddr, cacheLineSize, memAccess, nullptr /* Data */,
          cpuDelegator->dataMasterId(), 0 /* ContextId */,
          S->getFirstCoreUserPC() /* PC */, flags);
      pkt->req->setVirt(cacheLineVAddr);
    }
    pkt->req->getStatistic()->isStream = true;
    pkt->req->getStatistic()->streamName = S->streamName.c_str();
    S_ELEMENT_DPRINTF(element, "Issued %dth request to %#x %d.\n", i,
                      pkt->getAddr(), pkt->getSize());

    {
      // Sanity check that no multi-line element.
      auto lineOffset = pkt->getAddr() % cacheLineSize;
      if (lineOffset + pkt->getSize() > cacheLineSize) {
        S_ELEMENT_PANIC(element,
                        "Issued Multi-Line request to %#x size %d, "
                        "lineVAddr %#x linePAddr %#x.",
                        pkt->getAddr(), pkt->getSize(), cacheLineVAddr,
                        cacheLinePAddr);
      }
    }
    S->statistic.numIssuedRequest++;
    element->dynS->incrementNumIssuedRequests();
    S->incrementInflyStreamRequest();
    this->incrementInflyStreamRequest();

    // Mark the state.
    cacheBlockBreakdown.state = CacheBlockBreakdownAccess::StateE::Issued;
    cacheBlockBreakdown.memAccess = memAccess;
    memAccess->registerReceiver(element);

    if (cpuDelegator->cpuType == GemForgeCPUDelegator::ATOMIC_SIMPLE) {
      // Directly send to memory for atomic cpu.
      this->cpuDelegator->sendRequest(pkt);
    } else {
      // Send the pkt to translation.
      this->translationBuffer->addTranslation(
          pkt, cpuDelegator->getSingleThreadContext(), nullptr);
    }
  }
}

void StreamEngine::writebackElement(StreamElement *element,
                                    StreamStoreInst *inst) {
  assert(element->isAddrReady() && "Address should be ready.");
  auto S = element->stream;
  assert(S->getStreamType() == ::LLVM::TDG::StreamInfo_Type_ST &&
         "Should never writeback element for non store stream.");

  // Check the bookkeeping for infly writeback memory accesses.
  assert(element->inflyWritebackMemAccess.count(inst) == 0 &&
         "This StreamStoreInst has already been writebacked.");
  auto &inflyWritebackMemAccesses =
      element->inflyWritebackMemAccess
          .emplace(std::piecewise_construct, std::forward_as_tuple(inst),
                   std::forward_as_tuple())
          .first->second;

  S_ELEMENT_DPRINTF(element, "Writeback.\n");

  // hack("Send packt for stream %s.\n", S->getStreamName().c_str());

  for (size_t i = 0; i < element->cacheBlocks; ++i) {
    auto &cacheBlockBreakdown = element->cacheBlockBreakdownAccesses[i];

    // Translate the virtual address.
    auto vaddr = cacheBlockBreakdown.virtualAddr;
    auto packetSize = cacheBlockBreakdown.size;
    Addr paddr;
    if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
      panic("Failed translate vaddr %#x.\n", vaddr);
    }

    if (this->enableStreamPlacement) {
      // This means we have the placement manager.
      if (this->streamPlacementManager->access(cacheBlockBreakdown, element,
                                               true)) {
        // Stream placement manager handles this packet.
        continue;
      }
    }

    // Allocate the book-keeping StreamMemAccess.
    auto memAccess = element->allocateStreamMemAccess(cacheBlockBreakdown);
    inflyWritebackMemAccesses.insert(memAccess);
    // Create the writeback package.
    auto pkt = GemForgePacketHandler::createGemForgePacket(
        paddr, packetSize, memAccess, this->writebackCacheLine,
        cpuDelegator->dataMasterId(), 0, 0);
    S->incrementInflyStreamRequest();
    this->incrementInflyStreamRequest();
    cpuDelegator->sendRequest(pkt);
  }
}

void StreamEngine::dumpFIFO() const {
  inform("Total elements %d, free %d, totalRunAhead %d\n",
         this->FIFOArray.size(), this->numFreeFIFOEntries,
         this->getTotalRunAheadLength());

  for (const auto &IdStream : this->streamMap) {
    auto S = IdStream.second;
    if (!S->dynamicStreams.empty()) {
      S->dump();
    }
  }
}

void StreamEngine::dumpUser() const {
  for (const auto &userElement : this->userElementMap) {
    auto user = userElement.first;
    inform("--seqNum %llu used element.\n", user);
    for (auto element : userElement.second) {
      element->dump();
    }
  }
}

void StreamEngine::dump() {
  if (this->enableStreamPlacement) {
    this->streamPlacementManager->dumpCacheStreamAwarePortStatus();
  }
  this->dumpFIFO();
  this->dumpUser();
}

void StreamEngine::exitDump() const {
  if (streamPlacementManager != nullptr) {
    this->streamPlacementManager->dumpStreamCacheStats();
  }
  if (this->streamMap.empty()) {
    return;
  }
  std::vector<Stream *> allStreams;
  for (auto &pair : this->streamMap) {
    allStreams.push_back(pair.second);
  }
  // Try to sort them.
  std::sort(allStreams.begin(), allStreams.end(),
            [](const Stream *a, const Stream *b) -> bool {
              // Sort by region and then stream name.
              auto aId = a->streamRegion->region() + a->getStreamName();
              auto bId = b->streamRegion->region() + b->getStreamName();
              return aId < bId;
            });
  // Create the stream stats file.
  auto streamStatsFileName =
      "stream.stats." + std::to_string(cpuDelegator->cpuId()) + ".txt";
  auto &streamOS = *simout.findOrCreate(streamStatsFileName)->stream();
  for (auto &S : allStreams) {
    S->dumpStreamStats(streamOS);
  }
}

size_t StreamEngine::getTotalRunAheadLength() const {
  size_t totalRunAheadLength = 0;
  for (const auto &IdStream : this->streamMap) {
    auto S = IdStream.second;
    if (!S->configured) {
      continue;
    }
    totalRunAheadLength += S->maxSize;
  }
  return totalRunAheadLength;
}

const ::LLVM::TDG::StreamRegion &
StreamEngine::getStreamRegion(const std::string &relativePath) const {
  if (this->memorizedStreamRegionMap.count(relativePath) != 0) {
    return this->memorizedStreamRegionMap.at(relativePath);
  }

  auto fullPath = cpuDelegator->getTraceExtraFolder() + "/" + relativePath;
  ProtoInputStream istream(fullPath);
  auto &protobufRegion =
      this->memorizedStreamRegionMap
          .emplace(std::piecewise_construct,
                   std::forward_as_tuple(relativePath), std::forward_as_tuple())
          .first->second;
  if (!istream.read(protobufRegion)) {
    panic("Failed to read in the stream region from file %s.",
          fullPath.c_str());
  }
  return protobufRegion;
}

void StreamEngine::coalesceContinuousDirectMemStreamElement(
    StreamElement *element) {

  const bool enableCoalesceContinuousElement = true;
  if (!enableCoalesceContinuousElement) {
    return;
  }

  // Check if this is the first element.
  if (element->FIFOIdx.entryIdx == 0) {
    return;
  }
  auto S = element->stream;
  if (!S->isDirectMemStream()) {
    return;
  }
  // Never do this for not floated AtomicComputeStream.
  if (S->isAtomicComputeStream() && !element->dynS->offloadedToCache) {
    return;
  }
  // Check if this element is flushed.
  if (element->flushed) {
    S_ELEMENT_DPRINTF(element, "[NoCoalesce] Flushed.\n");
    return;
  }
  // Get the previous element.
  auto prevElement = S->getPrevElement(element);
  // Bail out if we have no previous element (we are the first).
  if (!prevElement) {
    S_ELEMENT_DPRINTF(element, "[NoCoalesce] No PrevElement.\n");
    return;
  }
  // We found the previous element. Check if completely overlap.
  if ((prevElement->FIFOIdx.streamId != element->FIFOIdx.streamId) ||
      (prevElement->FIFOIdx.entryIdx + 1 != element->FIFOIdx.entryIdx)) {
    S_ELEMENT_PANIC(element, "Mismatch FIFOIdx for prevElement %s.\n",
                    prevElement->FIFOIdx);
  }

  // Check if the previous element has the cache line.
  if (!prevElement->isCacheBlockedValue) {
    S_ELEMENT_DPRINTF(element, "[NoCoalesce] PrevElement not CacheBlocked.\n");
    return;
  }
  assert(prevElement->cacheBlocks && "No block in prevElement.");

  auto &prevElementMinBlockVAddr =
      prevElement->cacheBlockBreakdownAccesses[0].cacheBlockVAddr;
  for (int cacheBlockIdx = 0; cacheBlockIdx < element->cacheBlocks;
       ++cacheBlockIdx) {
    auto &block = element->cacheBlockBreakdownAccesses[cacheBlockIdx];
    assert(block.state == CacheBlockBreakdownAccess::StateE::Initialized);
    if (block.cacheBlockVAddr < prevElementMinBlockVAddr) {
      // Underflow.
      S_ELEMENT_DPRINTF(
          element,
          "[NoCoalece] %dth Block %#x, Underflow Prev MinBlockVAddr %#x.\n",
          cacheBlockIdx, block.cacheBlockVAddr, prevElementMinBlockVAddr);
      continue;
    }
    auto blockOffset = (block.cacheBlockVAddr - prevElementMinBlockVAddr) /
                       element->cacheBlockSize;
    if (blockOffset >= prevElement->cacheBlocks) {
      // Overflow.
      S_ELEMENT_DPRINTF(element,
                        "[NoCoalesce] %dth Block %#x, Overflow Prev "
                        "MinBlockVAddr %#x NumBlocks %d.\n",
                        cacheBlockIdx, block.cacheBlockVAddr,
                        prevElementMinBlockVAddr, prevElement->cacheBlocks);
      continue;
    }
    /**
     * We found a match in the previous element, which means a request for that
     * line has already been sent out. There are two cases here.
     * 1. If this is a LoadStream, we try to copy the line if ready, or register
     * as a receiver if the request has not come back yet.
     * 2. If this is a StoreStream, we simply mark this block as Issued, so that
     * we won't issue duplicate requests. Note that for StoreStreams, when the
     * request comes back, it won't set the data.
     */
    const auto &prevBlock =
        prevElement->cacheBlockBreakdownAccesses[blockOffset];
    bool shouldCopyFromPrev = false;
    if (S->isLoadStream()) {
      shouldCopyFromPrev = true;
    }
    if (S->isAtomicComputeStream() && element->dynS->offloadedToCache) {
      shouldCopyFromPrev = true;
    }
    if (prevBlock.state == CacheBlockBreakdownAccess::StateE::Faulted) {
      // Also mark this block faulted.
      block.state = CacheBlockBreakdownAccess::StateE::Faulted;
      element->tryMarkValueReady();
    } else if (prevBlock.state == CacheBlockBreakdownAccess::StateE::Ready) {
      if (shouldCopyFromPrev) {
        auto offset = prevElement->mapVAddrToValueOffset(
            block.cacheBlockVAddr, element->cacheBlockSize);
        element->setValue(block.cacheBlockVAddr, element->cacheBlockSize,
                          &prevElement->value.at(offset));
      } else {
        // Simply mark issued.
        block.state = CacheBlockBreakdownAccess::StateE::Issued;
      }
    } else if (prevBlock.state == CacheBlockBreakdownAccess::StateE::Issued) {
      if (shouldCopyFromPrev) {
        // Register myself as a receiver.
        if (!prevBlock.memAccess) {
          S_ELEMENT_PANIC(element,
                          "Missing memAccess for issued previous cache block.");
        }
        block.memAccess = prevBlock.memAccess;
        block.memAccess->registerReceiver(element);
        block.state = CacheBlockBreakdownAccess::StateE::Issued;
      } else {
        // Simply mark issued.
        block.state = CacheBlockBreakdownAccess::StateE::Issued;
      }
    }
    S_ELEMENT_DPRINTF(
        element,
        "[Coalesce] %dth Block %#x %s, PrevElement %dth Block %#x %s.\n",
        cacheBlockIdx, block.cacheBlockVAddr, block.state, blockOffset,
        prevBlock.cacheBlockVAddr, prevBlock.state);
  }
}

void StreamEngine::sendStreamFloatEndPacket(
    const std::vector<DynamicStreamId> &endedIds) {
  // We need to explicitly allocate and copy the all the ids in the packet.
  auto endedIdsCopy = new std::vector<DynamicStreamId>(endedIds);
  // The target address is just virtually 0 (should be set by MLC stream
  // engine).
  Addr initPAddr = 0;
  auto pkt = GemForgePacketHandler::createStreamControlPacket(
      initPAddr, cpuDelegator->dataMasterId(), 0, MemCmd::Command::StreamEndReq,
      reinterpret_cast<uint64_t>(endedIdsCopy));
  if (Debug::CoreRubyStreamLife) {
    std::stringstream ss;
    for (const auto &id : endedIds) {
      SE_DPRINTF_(CoreRubyStreamLife, "%s: Send FloatEnd.\n", id);
    }
  }
  cpuDelegator->sendRequest(pkt);
}

void StreamEngine::sendAtomicPacket(StreamElement *element,
                                    AtomicOpFunctorPtr atomicOp) {
  if (!element->isAddrReady()) {
    S_ELEMENT_PANIC(element,
                    "Element should be address ready to send AtomicOp.");
  }
  if (element->cacheBlocks != 1) {
    S_ELEMENT_PANIC(element, "Illegal # of CacheBlocks %d for AtomicOp.",
                    element->cacheBlocks);
  }
  const auto &cacheBlockBreakdownAccess =
      element->cacheBlockBreakdownAccesses[0];
  auto vaddr = cacheBlockBreakdownAccess.virtualAddr;
  auto size = cacheBlockBreakdownAccess.size;
  Addr paddr;
  if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
    S_ELEMENT_PANIC(element, "Fault on AtomicOp vaddr %#x.", vaddr);
  }
  auto pkt = GemForgePacketHandler::createGemForgeAMOPacket(
      vaddr, paddr, size, nullptr /* Handler */, cpuDelegator->dataMasterId(),
      0 /* ContextId */, 0 /* PC */, std::move(atomicOp));
  auto S = element->stream;
  S->statistic.numIssuedRequest++;
  // Send the packet to translation.
  if (cpuDelegator->cpuType == GemForgeCPUDelegator::ATOMIC_SIMPLE) {
    // Directly send to memory for atomic cpu.
    this->cpuDelegator->sendRequest(pkt);
  } else {
    this->translationBuffer->addTranslation(
        pkt, cpuDelegator->getSingleThreadContext(), nullptr);
  }
}

void StreamEngine::flushPEB(Addr vaddr, int size) {
  SE_DPRINTF_(StreamAlias, "====== Flush PEB %#x, +%d.\n", vaddr, size);
  bool foundAliasedIndirect = false;
  for (auto element : this->peb.elements) {
    assert(element->isAddrReady());
    assert(!element->isFirstUserDispatched());
    if (element->addr >= vaddr + size ||
        element->addr + element->size <= vaddr) {
      // Not aliased.
      continue;
    }
    if (!element->stream->hasNonCoreDependent()) {
      // No dependent streams.
      continue;
    }
    S_ELEMENT_DPRINTF_(StreamAlias, element,
                       "Found AliasedIndrect PEB %#x, +%d.\n", vaddr, size);
    foundAliasedIndirect = true;
  }
  for (auto elementIter = this->peb.elements.begin(),
            elementEnd = this->peb.elements.end();
       elementIter != elementEnd;) {
    auto element = *elementIter;
    bool aliased = !(element->addr >= vaddr + size ||
                     element->addr + element->size <= vaddr);
    if (!aliased && !foundAliasedIndirect) {
      // Not aliased, and we are selectively flushing.
      S_ELEMENT_DPRINTF_(StreamAlias, element, "Skip flush in PEB.\n");
      ++elementIter;
      continue;
    }
    S_ELEMENT_DPRINTF_(StreamAlias, element, "Flushed in PEB %#x, +%d.\n",
                       element->addr, element->size);
    if (element->dynS->offloadedToCache) {
      if (!element->getStream()->isLoadStream()) {
        // This must be computation offloading.
        S_ELEMENT_PANIC(element,
                        "Cannot flush offloaded non-load stream element.\n");
      }
    }
    if (element->scheduledComputation) {
      S_ELEMENT_PANIC(element, "Flush in PEB when scheduled computation.");
    }

    // Clear the element to just allocate state.
    element->flush(aliased);
    elementIter = this->peb.elements.erase(elementIter);
  }
}

void StreamEngine::RAWMisspeculate(StreamElement *element) {
  assert(!this->peb.contains(element) && "RAWMisspeculate on PEB element.");
  S_ELEMENT_DPRINTF_(StreamAlias, element, "RAWMisspeculated.\n");
  // Still, we flush the PEB when LQ misspeculate happens.
  this->flushPEB(element->addr, element->size);

  // Revert this element to just allocate state.
  element->flush(true /* aliased */);
}

void StreamEngine::resetStats() {
  for (auto &idStream : this->streamMap) {
    auto S = idStream.second;
    S->statistic.clear();
  }
}

StreamEngine *StreamEngineParams::create() { return new StreamEngine(this); }
