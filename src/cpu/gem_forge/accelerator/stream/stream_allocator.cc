#include "stream_region_controller.hh"

#include "base/trace.hh"
#include "debug/CoreStreamAlloc.hh"

#define DEBUG_TYPE CoreStreamAlloc
#include "stream_log.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamLoopBound, format, ##args)
#define SE_PANIC(format, args...)                                              \
  panic("[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)

namespace gem5 {

bool StreamRegionController::canSkipAllocatingDynS(StaticRegion &staticRegion,
                                                   DynStream &stepRootDynS) {

  auto &stepDynStreams = stepRootDynS.stepDynStreams;

  /**
   * If the dynRegion has marked skipToEnd, but the dynS has no TotalTripCount,
   * we have to wait if we have reached the FirstFloatElemIdx.
   *
   * Things get complicated when we have MidWay float with PtrChase or Reduce,
   * where we can allocate until FirstFloatElemIdx + 1.
   */
  const auto &dynRegion =
      this->getDynRegion("CanSkipAllocatingDynS", stepRootDynS.configSeqNum);
  if (dynRegion.canSkipToEnd && !stepRootDynS.hasTotalTripCount()) {
    auto firstFloatElemIdx = stepRootDynS.getFirstFloatElemIdxOfStepGroup();
    auto nextElemIdx = stepRootDynS.FIFOIdx.entryIdx;
    if (nextElemIdx > firstFloatElemIdx) {
      DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                    "[StreamAlloc] NoAlloc as NoTripCount to SkipToEnd. Next "
                    "%lu == FirstFloatElemIdx %lu.\n",
                    nextElemIdx, firstFloatElemIdx);
      return true;
    }
  }

  int64_t maxTailElemIdx = -1;
  if (stepRootDynS.hasTotalTripCount()) {
    maxTailElemIdx =
        stepRootDynS.getTotalTripCount() + stepRootDynS.stepElemCount;
  } else {

    /**
     * Pointer-chase stream can only have 4 elements per DynStream.
     * If reached that limit, we try go to the next one.
     */
    auto stepRootS = stepRootDynS.stream;

    bool boundedByPointerChase = false;
    if (stepRootS->isPointerChase()) {
      boundedByPointerChase = true;
    } else {
      for (const auto &backBaseS : stepRootS->backBaseStreams) {
        if (backBaseS->stepRootStream->isPointerChase()) {
          boundedByPointerChase = true;
        }
      }
    }

    if (boundedByPointerChase) {
      if (stepRootDynS.allocSize >= 4) {
        DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                      "[StreamAlloc] BoundedPointerChase AllocSize %d "
                      "TailElemIdx %llu.\n",
                      stepRootDynS.allocSize, stepRootDynS.FIFOIdx.entryIdx);
        maxTailElemIdx = stepRootDynS.FIFOIdx.entryIdx;
      }
    }
  }

  /**
   * We also limit streams' alloc size if they are in:
   * 1. Eliminated Nested Loop.
   * 2. All memory streams are offloaded.
   * So that we can work on multiple DynStreams at the same time.
   */
  if (staticRegion.region.loop_eliminated() && staticRegion.region.is_nest()) {
    if (stepRootDynS.allocSize >= 8) {
      bool allStepMemStreamsOffloaded = true;
      for (auto stepDynS : stepDynStreams) {
        if (stepDynS->stream->isMemStream()) {
          if (!stepDynS->isFloatedToCache()) {
            allStepMemStreamsOffloaded = false;
            break;
          }
        }
      }
      if (allStepMemStreamsOffloaded) {
        DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                      "[StreamAlloc] BoundedElimNested Floated AllocSize "
                      "%d TailElemIdx %llu.\n ",
                      stepRootDynS.allocSize, stepRootDynS.FIFOIdx.entryIdx);
        maxTailElemIdx = stepRootDynS.FIFOIdx.entryIdx;
      }
    }
  }

  if (maxTailElemIdx != -1) {
    bool allStepStreamsAllocated = true;
    for (auto stepDynS : stepDynStreams) {
      // DYN_S_DPRINTF(stepDynS.dynStreamId,
      //               "TotalTripCount %d, Next FIFOIdx %s.\n",
      //               totalTripCount, stepDynS.FIFOIdx);
      if (stepDynS->FIFOIdx.entryIdx < maxTailElemIdx) {
        allStepStreamsAllocated = false;
        break;
      }
    }
    if (allStepStreamsAllocated) {
      // All allocated, we can move to next one.
      DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                    "All StepStreamAllocated. CanSkip. AllocSize %d "
                    "MaxTailElemIdx %llu.\n",
                    stepRootDynS.allocSize, maxTailElemIdx);
      return true;
    }
  } else {
    /**
     * Only skip this if we have no TotalTripCount. This is
     * because StreamEnd may be misspeculated. And we ended
     * using all the FIFO for the next nested dynamic stream.
     */
    if (stepRootDynS.endDispatched) {
      return true;
    }
  }
  return false;
}

void StreamRegionController::allocateElements(StaticRegion &staticRegion) {

  /**
   * We don't know if StreamEnd has already been dispatched for the last
   * DynamicRegion. Break if so.
   */
  if (!staticRegion.streams.front()->isConfigured()) {
    return;
  }

  /**
   * Try to allocate more elements for configured streams.
   * Set a target, try to make sure all streams reach this target.
   * Then increment the target.
   */
  // Make a copy of the StepRootStream.
  auto stepRootStreams = staticRegion.step.stepRootStreams;

  // Sort by the allocated size.
  std::sort(stepRootStreams.begin(), stepRootStreams.end(),
            [](Stream *SA, Stream *SB) -> bool {
              return SA->getAllocSize() < SB->getAllocSize();
            });

  for (auto stepRootStream : stepRootStreams) {

    /**
     * With the new NestStream, we have to search for the correct dynamic stream
     * to allocate for. It is the first DynStream that:
     * 1. StreamEnd not dispatched.
     * 2. StreamConfig executed.
     * 3. If has TotalTripCount, not all step streams has allocated all
     * elements.
     */
    const auto &stepStreams = se->getStepStreamList(stepRootStream);
    DynStream *allocatingStepRootDynS = nullptr;
    for (auto &stepRootDynS : stepRootStream->dynamicStreams) {
      if (!stepRootDynS.configExecuted) {
        // Configure not executed, can not allocate.
        break;
      }
      if (this->canSkipAllocatingDynS(staticRegion, stepRootDynS)) {
        continue;
      }
      // Found it.
      allocatingStepRootDynS = &stepRootDynS;
      break;
    }
    if (!allocatingStepRootDynS) {
      // Failed to find an allocating DynStream.
      S_DPRINTF(stepRootStream,
                "No Allocating DynStream, AllocSize %d MaxSize %d.\n",
                stepRootStream->getAllocSize(), stepRootStream->maxSize);
      continue;
    }

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
        if (backBaseS->getConfigLoopLevel() !=
            stepRootStream->getConfigLoopLevel()) {
          // This is actually a InnerLoopBaseS.
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
     *
     * We allow (TripCount + 1) elements as StreamEnd would consume one
     * element, with exception:
     * 1. TripCount is 0.
     *
     * In such case we won't allocate last element and SteamEnd would consume
     * no one.
     */
    const auto &allocRootDynId = allocatingStepRootDynS->dynStreamId;
    {
      const auto allocSize = allocatingStepRootDynS->allocSize;

      if (maxAllocSize > allocSize) {
        auto nextEntryIdx = allocatingStepRootDynS->FIFOIdx.entryIdx;
        auto maxAllocElemIdx = nextEntryIdx + maxAllocSize - allocSize - 1;
        if (maxAllocElemIdx > 0 &&
            !allocatingStepRootDynS->isElemFloatedToCache(nextEntryIdx)) {
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
            if (backBaseS->getConfigLoopLevel() !=
                stepRootStream->getConfigLoopLevel()) {
              // This is actually a InnerLoopBaseS.
              continue;
            }
            const auto &backBaseDynS =
                backBaseS->getDynStream(allocatingStepRootDynS->configSeqNum);
            auto backBaseLastElemIdx = backBaseDynS.FIFOIdx.entryIdx;
            if (nextEntryIdx != 0 && backBaseLastElemIdx < nextEntryIdx - 1) {
              DYN_S_PANIC(allocRootDynId,
                          "NextElemIdx %llu BackBaseLastElem %s.", nextEntryIdx,
                          backBaseDynS.FIFOIdx);
            }
            if (backBaseLastElemIdx < maxAllocElemIdx) {
              maxAllocSize -= (maxAllocElemIdx - backBaseLastElemIdx);
            }
          }
        }
      }

      if (allocatingStepRootDynS->hasTotalTripCount() &&
          maxAllocSize > allocSize) {
        auto nextEntryIdx = allocatingStepRootDynS->FIFOIdx.entryIdx;
        auto tripCount = allocatingStepRootDynS->getTotalTripCount();
        auto stepElemCount = allocatingStepRootDynS->stepElemCount;
        auto maxTripCount = tripCount == 0 ? 0 : (tripCount + stepElemCount);
        if (nextEntryIdx >= maxTripCount) {
          // We are already overflowed, set maxAllocSize to allocSize to stop
          // allocating. NOTE: This should not happen at all.
          maxAllocSize = allocSize;
        } else {
          auto elemUntilTripCount = maxTripCount - nextEntryIdx;
          assert(elemUntilTripCount % stepElemCount == 0);
          maxAllocSize = std::min(
              maxAllocSize, elemUntilTripCount / stepElemCount + allocSize);
        }
      }
      /**
       * For PointerChase streams, at most 4 elements per DynStream.
       */
      if (stepRootStream->isPointerChase()) {
        const int MaxElementPerPointerChaseDynStream = 4;
        if (maxAllocSize > MaxElementPerPointerChaseDynStream) {
          maxAllocSize = (allocSize > MaxElementPerPointerChaseDynStream)
                             ? allocSize
                             : MaxElementPerPointerChaseDynStream;
          DYN_S_DPRINTF(
              allocRootDynId,
              "Limit MaxElem/DynPointerChaseStream. Alloc %d MaxAlloc %d.\n",
              allocSize, maxAllocSize);
        }
      }
      const auto &dynRegion = this->getDynRegion(
          "AllocateElements", allocatingStepRootDynS->configSeqNum);
      if (dynRegion.canSkipToEnd &&
          !allocatingStepRootDynS->hasTotalTripCount()) {
        /**
         * Do not allocate beyond:
         * 1. If FirstFloatElemIdx == 0, this is not MidwayFloat. Do not
         * allocate 0.
         * 2. If FirstFloatElemIdx > 0, this is MidwayFloat. Do not
         * allocate FirstFloatElemIdx + 1.
         */
        auto firstFloatElemIdx =
            allocatingStepRootDynS->getFirstFloatElemIdxOfStepGroup();
        auto allocUntilElemIdx =
            (firstFloatElemIdx == 0) ? 0 : (firstFloatElemIdx + 1);
        auto nextElemIdx = allocatingStepRootDynS->FIFOIdx.entryIdx;
        assert(maxAllocSize >= allocSize);
        assert(nextElemIdx <= allocUntilElemIdx);
        auto extraElems = allocUntilElemIdx - nextElemIdx;
        if (maxAllocSize - allocSize > extraElems) {
          maxAllocSize = allocSize + extraElems;
          DYN_S_DPRINTF(allocRootDynId,
                        "Limit for DelayedSkipToEndS. Alloc %d MaxAlloc %d "
                        "FirstFloat %lu Until %lu Next %lu.\n",
                        allocSize, maxAllocSize, firstFloatElemIdx,
                        allocUntilElemIdx, nextElemIdx);
        }
      }
    }

    DYN_S_DPRINTF(
        allocRootDynId,
        "Allocating StepRootDynS AllocSize %d MaxSize %d MaxAllocSize %d.\n",
        stepRootStream->getAllocSize(), stepRootStream->maxSize, maxAllocSize);

    /**
     * We should try to limit maximum allocation per cycle cause I still see
     * some deadlock when one stream used all the FIFO entries orz.
     */
    const size_t MaxAllocationPerCycle = 4;
    for (size_t targetSize = 1, allocated = 0;
         targetSize <= maxAllocSize && se->hasFreeElement() &&
         allocated < MaxAllocationPerCycle;
         ++targetSize) {
      for (auto S : stepStreams) {
        assert(S->isConfigured() && "Try to allocate for unconfigured stream.");
        if (!se->hasFreeElement()) {
          S_DPRINTF(S, "No FreeElement.\n");
          break;
        }
        auto &dynS = S->getDynStreamByInstance(allocRootDynId.streamInstance);
        if (S->getAllocSize() >= S->maxSize) {
          DYN_S_DPRINTF(dynS.dynStreamId, "Reached MaxAllocSize %d >= %d.\n",
                        S->getAllocSize(), S->maxSize);
          continue;
        }
        if (dynS.allocSize >= targetSize) {
          DYN_S_DPRINTF(dynS.dynStreamId, "Reached TargetSize %d >= %d.\n",
                        dynS.allocSize, targetSize);
          continue;
        }
        if (!dynS.areNextBaseElementsAllocated()) {
          DYN_S_DPRINTF(dynS.dynStreamId, "NextBaseElem not allocated.\n");
          continue;
        }
        if (S != stepRootStream) {
          if (S->getAllocSize() >= stepRootStream->getAllocSize()) {
            // It doesn't make sense to allocate ahead than the step root.
            DYN_S_DPRINTF(dynS.dynStreamId,
                          "Do not allocate %d beyond StepRootS %d.\n",
                          S->getAllocSize(), stepRootStream->getAllocSize());
            continue;
          }
          if (dynS.allocSize >= allocatingStepRootDynS->allocSize) {
            // It also doesn't make sense to allocate ahead than root dynS.
            DYN_S_DPRINTF(dynS.dynStreamId,
                          "Do not allocate %d beyond StepRootDynS %d.\n",
                          dynS.allocSize, allocatingStepRootDynS->allocSize);
            continue;
          }
        }
        DYN_S_DPRINTF(dynS.dynStreamId, "Allocate %d.\n", dynS.allocSize);
        se->allocateElement(dynS);
        allocated++;
      }
    }
  }
}
} // namespace gem5
