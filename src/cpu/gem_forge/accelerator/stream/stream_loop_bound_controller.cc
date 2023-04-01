#include "stream_region_controller.hh"

#include "base/trace.hh"
#include "debug/StreamLoopBound.hh"

#define DEBUG_TYPE StreamLoopBound
#include "stream_log.hh"

#define SE_PANIC(format, args...)                                              \
  panic("[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamLoopBound, format, ##args)

namespace gem5 {

void StreamRegionController::initializeStreamLoopBound(
    const ::LLVM::TDG::StreamRegion &region, StaticRegion &staticRegion) {
  if (!region.is_loop_bound()) {
    return;
  }

  const auto &boundFuncInfo = region.loop_bound_func();
  auto boundFunc = std::make_shared<TheISA::ExecFunc>(
      se->getCPUDelegator()->getSingleThreadContext(), boundFuncInfo);
  const bool boundRet = region.loop_bound_ret();

  SE_DPRINTF("[LoopBound] Init StaticLoopBound for region %s. BoundRet %d.\n",
             region.region(), boundRet);
  auto &staticBound = staticRegion.loopBound;
  staticBound.boundFunc = boundFunc;
  staticBound.boundRet = boundRet;

  for (const auto &arg : region.loop_bound_func().args()) {
    if (arg.is_stream()) {
      // This is a stream input. Remember this in the base stream.
      auto S = this->se->getStream(arg.stream_id());
      staticBound.baseStreams.insert(S);
    }
  }
}

void StreamRegionController::dispatchStreamConfigForLoopBound(
    const ConfigArgs &args, DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  if (!staticRegion.region.is_loop_bound()) {
    return;
  }
  dynRegion.loopBound.boundFunc = staticRegion.loopBound.boundFunc;
  SE_DPRINTF("[LoopBound] Dispatch DynLoopBound for region %s.\n",
             staticRegion.region.region());
}

void StreamRegionController::executeStreamConfigForLoopBound(
    const ConfigArgs &args, DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  if (!staticRegion.region.is_loop_bound()) {
    return;
  }
  auto &dynBound = dynRegion.loopBound;

  assert(args.inputMap && "Missing InputMap.");
  assert(args.inputMap->count(
             ::LLVM::TDG::ReservedStreamRegionId::LoopBoundFuncInputRegionId) &&
         "Missing InputVec for LoopBound.");
  auto &inputVec = args.inputMap->at(
      ::LLVM::TDG::ReservedStreamRegionId::LoopBoundFuncInputRegionId);

  int inputIdx = 0;

  // Construct the NestConfigFunc formal params.
  SE_DPRINTF("[LoopBound] Executed DynLoopBound for region %s.\n",
             staticRegion.region.region());
  {
    auto &formalParams = dynBound.formalParams;
    const auto &funcInfo = dynBound.boundFunc->getFuncInfo();
    SE_DPRINTF("[LoopBound] boundFunc %#x.\n", dynBound.boundFunc);
    this->buildFormalParams(inputVec, inputIdx, funcInfo, formalParams);
  }
  SE_DPRINTF("[LoopBound] Executed DynLoopBound for region %s.\n",
             staticRegion.region.region());
}

void StreamRegionController::checkLoopBound(DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  if (!staticRegion.region.is_loop_bound()) {
    return;
  }

  auto &staticBound = staticRegion.loopBound;
  auto &dynBound = dynRegion.loopBound;
  if (dynBound.brokenOut) {
    // We already reached the end of the loop.
    return;
  }

  if (dynBound.offloaded &&
      dynBound.nextElemIdx >= dynBound.offloadedFirstElementIdx) {
    // Starting from this point, we have been offloaded.
    return;
  }

  auto nextElemIdx = dynBound.nextElemIdx;
  std::unordered_set<StreamElement *> baseElements;
  for (auto baseS : staticBound.baseStreams) {
    auto &baseDynS = baseS->getDynStream(dynRegion.seqNum);
    auto baseElement = baseDynS.getElemByIdx(nextElemIdx);
    if (!baseElement) {
      if (baseDynS.FIFOIdx.entryIdx > nextElemIdx) {
        DYN_S_PANIC(baseDynS.dynStreamId, "[LoopBound] Miss Element %llu.\n",
                    nextElemIdx);
      } else {
        // The base element is not allocated yet.
        DYN_S_DPRINTF(baseDynS.dynStreamId,
                      "[LoopBound] BaseElement %llu not Allocated.\n",
                      nextElemIdx);
        return;
      }
    }
    if (!baseElement->isValueReady) {
      S_ELEMENT_DPRINTF(baseElement, "[LoopBound] Not Ready.\n");
      return;
    }
    baseElements.insert(baseElement);
  }

  // All base elements are value ready.
  auto getStreamValue =
      GetStreamValueFromElementSet(baseElements, "[LoopBound]");

  auto actualParams =
      convertFormalParamToParam(dynBound.formalParams, getStreamValue);

  auto ret = dynBound.boundFunc->invoke(actualParams).front();
  if (ret == staticBound.boundRet) {
    /**
     * Should break out the loop.
     * So far we just set TotalTripCount for all DynStreams.
     */
    SE_DPRINTF(
        "[LoopBound] Break Elem %lu (%d == %d) Region %s TripCount %llu.\n",
        dynBound.nextElemIdx, ret, staticBound.boundRet,
        staticRegion.region.region(), dynBound.nextElemIdx + 1);
    dynBound.brokenOut = true;
    for (auto S : staticRegion.streams) {
      auto &dynS = S->getDynStream(dynRegion.seqNum);
      dynS.setTotalAndInnerTripCount(dynBound.nextElemIdx + 1);
      DYN_S_DPRINTF(dynS.dynStreamId,
                    "[LoopBound] Break (%d == %d) TotalTripCount %llu.\n", ret,
                    staticBound.boundRet, dynBound.nextElemIdx + 1);
    }

  } else {
    // Keep going.
    SE_DPRINTF("[LoopBound] Continue Elem %lu (%d != %d) Region %s.\n",
               dynBound.nextElemIdx, ret, staticBound.boundRet,
               staticRegion.region.region());
  }
  dynBound.nextElemIdx++;
}

void StreamRegionController::receiveOffloadedLoopBoundRet(
    const DynStreamId &dynStreamId, int64_t tripCount, bool brokenOut) {
  auto S = se->getStream(dynStreamId.staticId);
  auto dynS = S->getDynStream(dynStreamId);
  if (!dynS) {
    DYN_S_PANIC(dynStreamId, "[LoopBound] Failed to get DynS.");
  }
  auto seqNum = dynS->configSeqNum;
  auto &dynRegion = this->getDynRegion(S->getStreamName(), seqNum);
  auto &dynBound = dynRegion.loopBound;
  auto &staticRegion = *dynRegion.staticRegion;

  SE_DPRINTF("[LoopBound] Recv TripCount %llu BrokenOut %d S %s.\n", tripCount,
             brokenOut, dynStreamId);
  if (tripCount != dynBound.nextElemIdx + 1) {
    SE_PANIC("[LoopBound] Received TripCount %llu != NextElem %llu + 1, "
             "BrokenOut %d Region %s.\n",
             tripCount, dynBound.nextElemIdx, brokenOut,
             staticRegion.region.region());
  }

  dynBound.brokenOut = brokenOut;
  dynBound.nextElemIdx = tripCount;
  if (brokenOut) {
    for (auto S : staticRegion.streams) {
      auto &dynS = S->getDynStream(dynRegion.seqNum);
      dynS.setTotalAndInnerTripCount(tripCount);
    }
    for (auto &dynGroup : dynRegion.step.stepGroups) {
      assert(dynGroup.totalTripCount == 0 &&
             "Already have StepGroupTripCount.");
      dynGroup.totalTripCount = tripCount;
    }
    // If we delayed the SkipToEnd, we retry it.
    if (dynRegion.canSkipToEnd) {
      this->trySkipToStreamEnd(dynRegion);
    }
  }
}
} // namespace gem5
