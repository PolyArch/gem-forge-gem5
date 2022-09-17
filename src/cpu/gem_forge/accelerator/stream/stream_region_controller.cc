#include "stream_region_controller.hh"
#include "stream_throttler.hh"

#include "base/trace.hh"
#include "debug/StreamRegion.hh"

#define DEBUG_TYPE StreamRegion
#include "stream_log.hh"

#define SE_PANIC(format, args...)                                              \
  panic("[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamRegion, format, ##args)

StreamRegionController::StreamRegionController(StreamEngine *_se)
    : se(_se), isaHandler(_se->getCPUDelegator()) {}

StreamRegionController::~StreamRegionController() {}

void StreamRegionController::initializeRegion(
    const ::LLVM::TDG::StreamRegion &region) {

  this->staticRegionMap.emplace(std::piecewise_construct,
                                std::forward_as_tuple(region.region()),
                                std::forward_as_tuple(region));

  auto &staticRegion = this->staticRegionMap.at(region.region());

  /**
   * Collect streams within this region.
   */
  StaticRegion::StreamSet streams;
  staticRegion.allStreamsLoopEliminated = true;
  staticRegion.someStreamsLoopEliminated = false;
  for (const auto &streamInfo : region.streams()) {
    auto S = this->se->getStream(streamInfo.id());
    if (streams.count(S)) {
      // Coalesced stream.
      continue;
    }
    staticRegion.streams.push_back(S);
    streams.insert(S);
    // Record LoopLiminated information.
    if (S->isLoopEliminated()) {
      staticRegion.someStreamsLoopEliminated = true;
    } else {
      staticRegion.allStreamsLoopEliminated = false;
    }
  }
  if (staticRegion.region.loop_eliminated() &&
      !staticRegion.allStreamsLoopEliminated) {
    SE_PANIC("[Region] All Streams should be LoopEliminated %s.",
             staticRegion.region.region());
  }
  if (!staticRegion.allStreamsLoopEliminated &&
      staticRegion.someStreamsLoopEliminated) {
    if (staticRegion.region.is_loop_bound()) {
      SE_PANIC("[Region] LoopBound not work with PartialElimnation %s.",
               staticRegion.region.region());
    }
  }

  this->initializeNestStreams(region, staticRegion);
  this->initializeStreamLoopBound(region, staticRegion);
  this->initializeStep(region, staticRegion);
}

void StreamRegionController::dispatchStreamConfig(const ConfigArgs &args) {
  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->se->getStreamRegion(infoRelativePath);
  auto &staticRegion = this->getStaticRegion(streamRegion.region());
  auto &dynRegion = this->pushDynRegion(staticRegion, args.seqNum);

  this->dispatchStreamConfigForNestStreams(args, dynRegion);
  this->dispatchStreamConfigForLoopBound(args, dynRegion);
  this->dispatchStreamConfigForStep(args, dynRegion);
}

void StreamRegionController::executeStreamConfig(const ConfigArgs &args) {
  assert(this->activeDynRegionMap.count(args.seqNum) && "Missing DynRegion.");
  auto &dynRegion = *this->activeDynRegionMap.at(args.seqNum);
  this->executeStreamConfigForNestStreams(args, dynRegion);
  this->executeStreamConfigForLoopBound(args, dynRegion);
  this->executeStreamConfigForStep(args, dynRegion);
  this->trySkipToStreamEnd(dynRegion);

  SE_DPRINTF("[Region] Executed Config SeqNum %llu for region %s.\n",
             args.seqNum, dynRegion.staticRegion->region.region());

  dynRegion.configExecuted = true;

  /**
   * Try to boost the streams if this is a Eliminated InnerMost Loop.
   * This enables simultaneous multiple inner loop dynamic streams.
   */
  auto &staticRegion = *dynRegion.staticRegion;
  if (staticRegion.region.loop_eliminated() &&
      staticRegion.streams.front()->getIsInnerMostLoop()) {
    se->throttler->boostStreams(staticRegion.step.stepRootStreams);
  }
}

void StreamRegionController::commitStreamConfig(const ConfigArgs &args) {
  assert(this->activeDynRegionMap.count(args.seqNum) && "Missing DynRegion.");
  auto &dynRegion = *this->activeDynRegionMap.at(args.seqNum);

  SE_DPRINTF("[Region] Commit Config SeqNum %llu for region %s.\n", args.seqNum,
             dynRegion.staticRegion->region.region());

  dynRegion.configCommitted = true;
}

void StreamRegionController::rewindStreamConfig(const ConfigArgs &args) {
  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->se->getStreamRegion(infoRelativePath);

  auto &staticRegion = this->getStaticRegion(streamRegion.region());
  assert(!staticRegion.dynRegions.empty() && "Missing DynRegion.");

  const auto &dynRegion = staticRegion.dynRegions.back();
  assert(dynRegion.seqNum == args.seqNum && "Mismatch in rewind seqNum.");

  SE_DPRINTF("[Region] Rewind DynRegion for region %s.\n",
             streamRegion.region());
  this->checkRemainingNestRegions(dynRegion);

  this->activeDynRegionMap.erase(args.seqNum);
  staticRegion.dynRegions.pop_back();
}

void StreamRegionController::commitStreamEnd(const EndArgs &args) {
  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->se->getStreamRegion(infoRelativePath);

  auto &staticRegion = this->getStaticRegion(streamRegion.region());
  assert(!staticRegion.dynRegions.empty() && "Missing DynRegion.");

  const auto &dynRegion = staticRegion.dynRegions.front();
  if (dynRegion.seqNum > args.seqNum) {
    /**
     * We allow the == case because in nested stream, it is still
     * possible that InnerStreamEnd comes right after OuterStreamConfig,
     * leaving there no space to insert the InnerStreamConfig.
     */
    SE_PANIC("[Region] %s End (%lu) before Configure (%lu).\n",
             streamRegion.region(), args.seqNum, dynRegion.seqNum);
  }

  SE_DPRINTF(
      "[Region] Release DynRegion SeqNum %llu for region %s, remaining %llu.\n",
      dynRegion.seqNum, streamRegion.region(),
      staticRegion.dynRegions.size() - 1);
  this->checkRemainingNestRegions(dynRegion);

  this->activeDynRegionMap.erase(dynRegion.seqNum);
  staticRegion.dynRegions.pop_front();
}

void StreamRegionController::tick() {
  for (auto &entry : this->activeDynRegionMap) {
    auto &dynRegion = *entry.second;
    if (dynRegion.configExecuted) {
      if (dynRegion.configCommitted) {
        // Do not config nest streams until committed.
        for (auto &dynNestConfig : dynRegion.nestConfigs) {
          this->configureNestStream(dynRegion, dynNestConfig);
        }
      }
      this->checkLoopBound(dynRegion);

      /**
       * For now, StreamAllocate must happen in order.
       * Check that this is the first DynStream.
       * But StreamStep is relaxed to be only inorder within each DynStream.
       */
      this->stepStream(dynRegion);
      if (dynRegion.seqNum ==
          dynRegion.staticRegion->dynRegions.front().seqNum) {
        this->allocateElements(*dynRegion.staticRegion);
      }
    }
  }
}

void StreamRegionController::takeOverBy(GemForgeCPUDelegator *newCPUDelegator) {
  this->isaHandler.takeOverBy(newCPUDelegator);
}

StreamRegionController::DynRegion &
StreamRegionController::pushDynRegion(StaticRegion &staticRegion,
                                      uint64_t seqNum) {
  staticRegion.dynRegions.emplace_back(&staticRegion, seqNum);
  auto &dynRegion = staticRegion.dynRegions.back();
  assert(this->activeDynRegionMap
             .emplace(std::piecewise_construct, std::forward_as_tuple(seqNum),
                      std::forward_as_tuple(&dynRegion))
             .second &&
         "Multiple nesting is not supported.");

  SE_DPRINTF("[Region] Initialized DynRegion SeqNum %llu for region %s. "
             "Current %d.\n",
             seqNum, staticRegion.region.region(),
             staticRegion.dynRegions.size());
  return dynRegion;
}

StreamRegionController::StaticRegion &
StreamRegionController::getStaticRegion(const std::string &regionName) {
  auto iter = this->staticRegionMap.find(regionName);
  if (iter == this->staticRegionMap.end()) {
    SE_PANIC("Failed to find StaticRegion %s.\n", regionName);
  }
  return iter->second;
}

StreamRegionController::DynRegion &
StreamRegionController::getDynRegion(const std::string &msg,
                                     InstSeqNum seqNum) {
  auto iter = this->activeDynRegionMap.find(seqNum);
  if (iter == this->activeDynRegionMap.end()) {
    SE_PANIC("Failed to find DynRegion SeqNum %llu: %s.\n", seqNum, msg);
  }
  return *iter->second;
}

void StreamRegionController::buildFormalParams(
    const ConfigArgs::InputVec &inputVec, int &inputIdx,
    const ::LLVM::TDG::ExecFuncInfo &funcInfo,
    DynStreamFormalParamV &formalParams) {
  for (const auto &arg : funcInfo.args()) {
    if (arg.is_stream()) {
      // This is a stream input.
      formalParams.emplace_back();
      auto &formalParam = formalParams.back();
      formalParam.isInvariant = false;
      formalParam.baseStreamId = arg.stream_id();
    } else {
      if (inputIdx >= inputVec.size()) {
        panic("Missing input for %s: Given %llu, inputIdx %d.", funcInfo.name(),
              inputVec.size(), inputIdx);
      }
      formalParams.emplace_back();
      auto &formalParam = formalParams.back();
      formalParam.isInvariant = true;
      formalParam.invariant = inputVec.at(inputIdx);
      inputIdx++;
    }
  }
}

StreamValue StreamRegionController::GetStreamValueFromElementSet::operator()(
    uint64_t streamId) const {
  StreamValue ret;
  for (auto baseElement : this->elements) {
    if (!baseElement->getStream()->isCoalescedHere(streamId)) {
      continue;
    }
    baseElement->getValueByStreamId(streamId, ret.uint8Ptr(),
                                    sizeof(StreamValue));
    return ret;
  }
  panic("%s Failed to find base element for stream %llu.", streamId);
  return ret;
}

bool StreamRegionController::canSkipToStreamEnd(
    const DynRegion &dynRegion) const {
  /**
   * Check that:
   * 1. Loop is eliminated.
   * 2. RangeSync disabled.
   * 3. Float enabled.
   * 4. No NestRegion.
   * For all streams:
   * 1. Only has affine streams (no reduction/pointer-chase/indirect), or all
   * indirect streams has no core user and no final value needed.
   * 2. No last/second-last element user.
   * 3. Has known TripCount.
   */
  auto staticRegion = dynRegion.staticRegion;
  if (!staticRegion->region.loop_eliminated()) {
    SE_DPRINTF("[Region] NoSkipToEnd: LoopNotEliminated.\n");
    return false;
  }
  if (se->myParams->enableRangeSync) {
    return false;
  }
  if (!se->myParams->streamEngineEnableFloat) {
    return false;
  }
  if (!dynRegion.nestConfigs.empty()) {
    SE_DPRINTF("[Region] NoSkipToEnd: NestConfigs.\n");
    return false;
  }
  for (auto S : staticRegion->streams) {
    auto &dynS = S->getDynStream(dynRegion.seqNum);
    // if (S->isMemStream() && !S->isDirectMemStream()) {
    //   // Indirect Mem stream.
    //   DYN_S_DPRINTF(dynS.dynStreamId, "[Region] NoSkipToEnd:
    //   IndirectMemS.\n"); return false;
    // }
    if (S->isInnerFinalValueUsedByCore() ||
        S->isInnerSecondFinalValueUsedByCore() || S->hasCoreUser()) {
      DYN_S_DPRINTF(dynS.dynStreamId,
                    "[Region] NoSkipToEnd: NeededByCore InnerFinalValue %d "
                    "InnerSecondFinalValue %d CoreUser %d.\n",
                    S->isInnerFinalValueUsedByCore(),
                    S->isInnerSecondFinalValueUsedByCore(), S->hasCoreUser());
      return false;
    }
    if (!dynS.hasTotalTripCount()) {
      DYN_S_DPRINTF(dynS.dynStreamId, "[Region] NoSkipToEnd: No TripCount.\n");
      return false;
    }
  }
  return true;
}

void StreamRegionController::trySkipToStreamEnd(DynRegion &dynRegion) {
  if (!this->canSkipToStreamEnd(dynRegion)) {
    return;
  }
  auto staticRegion = dynRegion.staticRegion;
  for (auto S : staticRegion->streams) {
    auto &dynS = S->getDynStream(dynRegion.seqNum);
    DYN_S_DPRINTF(dynS.dynStreamId, "[Region] Skip to End %llu.\n",
                  dynS.getTotalTripCount());
    dynS.FIFOIdx.entryIdx = dynS.getTotalTripCount();
  }
  // Don't forget to update the Stepper.
  for (auto &group : dynRegion.step.stepGroups) {
    SE_DPRINTF("[Region] Skip Group to End %llu.\n", group.totalTripCount);
    group.nextElemIdx = group.totalTripCount;
  }
}