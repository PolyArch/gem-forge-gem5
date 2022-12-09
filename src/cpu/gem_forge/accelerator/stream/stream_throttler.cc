#include "stream_throttler.hh"
#include "stream_region_controller.hh"

#include "debug/StreamThrottle.hh"

#define DEBUG_TYPE StreamThrottle
#include "stream_log.hh"

StreamThrottler::StreamThrottler(const std::string &_strategy,
                                 StreamEngine *_se)
    : se(_se) {
  if (_strategy == "static") {
    this->strategy = StrategyE::STATIC;
  } else if (_strategy == "dynamic") {
    this->strategy = StrategyE::DYNAMIC;
  } else {
    this->strategy = StrategyE::GLOBAL;
  }
}

const std::string StreamThrottler::name() const { return this->se->name(); }

/********************************************************************
 * Check if we actually want to throttle.
 *******************************************************************/

void StreamThrottler::throttleStream(StreamElement *element) {
  if (this->strategy == StrategyE::STATIC) {
    // Static means no throttling.
    return;
  }
  auto S = element->stream;
  if (S->isStoreStream()) {
    // No need to throttle for store stream.
    return;
  }
  if (element->FIFOIdx.entryIdx < S->maxSize) {
    // Do not throttle for the first maxSize elements.
    return;
  }
  if (element->valueReadyCycle == 0 || element->firstValueCheckCycle == 0) {
    // No valid cycle record, do nothing.
    return;
  }
  if (element->valueReadyCycle < element->firstValueCheckCycle + Cycles(2)) {
    // The element is ready earlier than user, do nothing.
    // We add 2 cycles buffer here.
    return;
  }
  // This is a late fetch, increase the counter.
  S->lateFetchCount++;
  S_ELEMENT_DPRINTF(element, "[Throttle] LateCount %d.\n", S->lateFetchCount);
  if (S->lateFetchCount < 10) {
    return;
  }

  // We have reached the threshold to allow the stream to run further
  // ahead.
  auto oldRunAheadSize = S->maxSize;
  /**
   * Get the step root stream.
   * Sometimes, it is possible that stepRootStream is nullptr,
   * which means that this is a constant stream.
   * We do not throttle in this case.
   */
  auto stepRootStream = S->stepRootStream;
  if (stepRootStream != nullptr) {
    const auto &streamList = this->se->getStepStreamList(stepRootStream);
    if (this->strategy == StrategyE::DYNAMIC) {
      // All streams with the same stepRootStream must have the same run
      // ahead length.
      auto totalRunAheadLength = this->se->getTotalRunAheadLength();
      // Only increase the run ahead length if the totalRunAheadLength is
      // within the 90% of the total FIFO entries. Need better solution
      // here.
      const auto incrementStep = 2;
      if (static_cast<float>(totalRunAheadLength) <
          0.9f * static_cast<float>(this->se->FIFOArray.size())) {
        for (auto stepS : streamList) {
          // Increase the run ahead length by step.
          stepS->maxSize += incrementStep;
        }
        assert(S->maxSize == oldRunAheadSize + 2 &&
               "RunAheadLength is not increased.");
      }
    } else if (this->strategy == StrategyE::GLOBAL) {
      this->tryGlobalThrottle(element->stream);
    }
    // No matter what, just clear the lateFetchCount in the whole step
    // group.
    for (auto stepS : streamList) {
      stepS->lateFetchCount = 0;
    }
  } else {
    // Otherwise, just clear my self.
    S->lateFetchCount = 0;
  }
}

/********************************************************************
 * Perform the actual throttling.
 *
 * When we trying to throttle a stream, the main problem is to avoid
 * deadlock, as we do not reclaim stream element once it is allocated until
 * it is stepped.
 *
 * To avoid deadlock, we leverage the information of total alive streams
 * that can coexist with the current stream, and assign InitMaxSize number
 * of elements to these streams, which is called BasicEntries.
 * * BasicEntries = TotalAliveStreams * InitMaxSize.
 *
 * Then we want to know how many of these BasicEntries is already assigned
 * to streams. This number is called AssignedBasicEntries.
 * * AssignedBasicEntries = CurrentAliveStreams * InitMaxSize.
 *
 * We also want to know the number of AssignedEntries and UnAssignedEntries.
 * * AssignedEntries = Sum(MaxSize, CurrentAliveStreams).
 * * UnAssignedEntries = FIFOSize - AssignedEntries.
 *
 * The available pool for throttling is:
 * * AvailableEntries = \
 * *   UnAssignedEntries - (BasicEntries - AssignedBasicEntries).
 *
 * Also we enforce an upper bound on the entries:
 * * UpperBoundEntries = \
 * *   (FIFOSize - BasicEntries) / StepGroupSize + InitMaxSize.
 *
 * As we are throttling streams altogether with the same stepRoot, the
 * condition is:
 * * AvailableEntries >= IncrementSize * StepGroupSize.
 * * CurrentMaxSize + IncrementSize <= UpperBoundEntries
 *
 * Updates: We used to model the FIFO only by the number of elements,
 * however, this is not quite accurate as different streams has
 * different element size, e.g. scalar vs. vectorized. Essentially,
 * stream elements are part of the core view, and as long as we do not
 * block core's dispatch due to lack of available elements, we are
 * fine. The bottleneck is the actual buffer size, which truly determines
 * the prefetch distance.
 * In real hardware, this should be split into two parts: one managing
 * stream elements (core view), and one managing prefetching requests
 * (memory view). However, it should be sufficient to just impose a
 * soft upper-bound to the throttler for the buffer size.
 *
 * NOTE: The memory view (bytes) only applies to load streams.
 ********************************************************************/

bool StreamThrottler::tryGlobalThrottle(Stream *S) {
  auto stepRootStream = S->stepRootStream;
  assert(stepRootStream != nullptr &&
         "Do not make sense to throttle for a constant stream.");
  const auto &streamList = this->se->getStepStreamList(stepRootStream);

  S_DPRINTF(S, "[Throttle] Do throttling.\n");

  /**
   * There is no point throttling more than our BackBaseStream. This is the case
   * for reduction streams.
   */
  for (auto backBaseS : S->backBaseStreams) {
    if (backBaseS->maxSize < S->maxSize) {
      S_DPRINTF(S, "[Not Throttle] MyMaxSize %d >= %d of BackBaseS %s.\n",
                S->maxSize, backBaseS->maxSize, backBaseS->getStreamName());
      return false;
    }
  }

  const int MaxSizeForOuterLoopStream = 8;
  if (!S->getIsInnerMostLoop()) {
    /**
     * For OuterS, there are two cases:
     * 1. If it controls some eliminated nested streams, it is limited by
     * elimNestStreamInstances.
     * 2. Otherwise, we take some heuristic MaxSizeForOuterLoopStream.
     */
    bool isElimNestOuterS = false;
    const auto &staticRegion = this->se->regionController->getStaticRegion(S);
    if (!staticRegion.dynRegions.empty()) {
      const auto &dynRegion = staticRegion.dynRegions.back();
      if (!dynRegion.nestConfigs.empty()) {
        const auto &staticNestRegion =
            dynRegion.nestConfigs.back().staticRegion;
        if (staticNestRegion->allStreamsLoopEliminated) {
          isElimNestOuterS = true;
          if (S->maxSize >= this->se->myParams->elimNestStreamInstances) {
            S_DPRINTF(
                S,
                "[Not Throttle] MyMaxSize %d >= %d ElimNestStreamInstances.\n",
                S->maxSize, this->se->myParams->elimNestStreamInstances);
            return false;
          }
        }
      }
    }
    if (!isElimNestOuterS && S->maxSize >= MaxSizeForOuterLoopStream) {
      S_DPRINTF(
          S, "[Not Throttle] MyMaxSize %d >= %d MaxSizeForOuterLoopStream.\n",
          S->maxSize, MaxSizeForOuterLoopStream);
      return false;
    }
  } else {
    /**
     * For InnerS, we do not allocate too much if it's:
     * 1. Nested.
     * 2. Eliminated.
     * 3. Can skip to end.
     */
    bool isElimNestInnerS = false;
    const auto &staticRegion = this->se->regionController->getStaticRegion(S);
    if (staticRegion.nestConfig.configFunc &&
        staticRegion.allStreamsLoopEliminated) {
      bool canSkipToEnd = true;
      for (const auto &dynRegion : staticRegion.dynRegions) {
        if (!dynRegion.canSkipToEnd) {
          canSkipToEnd = false;
          break;
        }
      }
      if (canSkipToEnd) {
        isElimNestInnerS = true;
      }
    }
    if (isElimNestInnerS &&
        S->maxSize >= this->se->myParams->elimNestStreamInstances + 1) {
      S_DPRINTF(
          S,
          "[Not Throttle] InnerS MyMaxSize %d >= %d ElimNestStreamInstances.\n",
          S->maxSize, this->se->myParams->elimNestStreamInstances);
      return false;
    }
  }

  // * AssignedEntries.
  auto currentAliveStreams = 0;
  auto assignedEntries = 0;
  auto assignedBytes = 0;
  for (const auto &IdStream : this->se->streamMap) {
    auto S = IdStream.second;
    if (!S->hasDynStream()) {
      continue;
    }
    currentAliveStreams++;
    assignedEntries += S->maxSize;
    if (S->isLoadStream()) {
      assignedBytes +=
          S->maxSize * S->getLastDynStream().getBytesPerMemElement();
    }
  }
  // * UnAssignedEntries.
  int unassignedEntries = this->se->totalRunAheadLength - assignedEntries;
  int unassignedBytes = this->se->totalRunAheadBytes - assignedBytes;
  // * BasicEntries.
  auto streamRegion = S->streamRegion;
  int totalAliveStreams = this->se->enableCoalesce
                              ? streamRegion->total_alive_coalesced_streams()
                              : streamRegion->total_alive_streams();
  int basicEntries = std::max(totalAliveStreams, currentAliveStreams) *
                     this->se->defaultRunAheadLength;
  // * AssignedBasicEntries.
  int assignedBasicEntries =
      currentAliveStreams * this->se->defaultRunAheadLength;
  // * AvailableEntries.
  int availableEntries =
      unassignedEntries - (basicEntries - assignedBasicEntries);
  // * UpperBoundEntries.
  int upperBoundEntries =
      (this->se->totalRunAheadLength - basicEntries) / streamList.size() +
      this->se->defaultRunAheadLength;
  const auto incrementStep = 1;
  int totalIncrementEntries = incrementStep * streamList.size();
  int totalIncrementBytes = 0;
  for (auto S : streamList) {
    if (S->isLoadStream()) {
      totalIncrementBytes +=
          incrementStep * S->getLastDynStream().getBytesPerMemElement();
    }
  }

  S_DPRINTF(S,
            "[Throttle] MaxSize %d + %d AssignedEntries %d AssignedBytes %d "
            "UnassignedEntries %d "
            "UnassignedBytes %d "
            "BasicEntries %d "
            "AssignedBasicEntries %d AvailableEntries %d UpperBoundEntries %d "
            "TotalIncrementEntries %d TotalIncrementBytes %d "
            "CurrentAliveStreams %d "
            "TotalAliveStreams %d.\n",
            S->maxSize, incrementStep, assignedEntries, assignedBytes,
            unassignedEntries, unassignedBytes, basicEntries,
            assignedBasicEntries, availableEntries, upperBoundEntries,
            totalIncrementEntries, totalIncrementBytes, currentAliveStreams,
            totalAliveStreams);

  if (availableEntries < totalIncrementEntries) {
    S_DPRINTF(S, "[Not Throttle]: Not enough available entries.\n");
    return false;
  } else if (totalAliveStreams * this->se->defaultRunAheadLength +
                 streamList.size() * (stepRootStream->maxSize + incrementStep -
                                      this->se->defaultRunAheadLength) >=
             this->se->totalRunAheadLength) {
    S_DPRINTF(S, "[Not Throttle]: Reserve for other streams.\n");
    return false;
  } else if (stepRootStream->maxSize + incrementStep > upperBoundEntries) {
    S_DPRINTF(S, "[Not Throttle]: Upperbound overflow.\n");
    return false;
  } else if (assignedBytes + totalIncrementBytes >
             this->se->totalRunAheadBytes) {
    S_DPRINTF(S, "[Not Throttle]: Total bytes overflow.\n");
    return false;
  }

  auto oldMaxSize = S->maxSize;
  for (auto stepS : streamList) {
    // Increase the run ahead length by 2.
    stepS->maxSize += incrementStep;
  }
  assert(S->maxSize == oldMaxSize + incrementStep &&
         "RunAheadLength is not increased.");
  return true;
}

void StreamThrottler::boostStreams(const Stream::StreamVec &stepRootStreams) {
  if (this->strategy != StrategyE::GLOBAL) {
    // No boost unless we have GLOBAL throttling.
    return;
  }
  while (true) {
    bool boosted = false;
    for (auto stepRootS : stepRootStreams) {
      if (this->tryGlobalThrottle(stepRootS)) {
        boosted = true;
      }
    }
    if (!boosted) {
      break;
    }
  }
}