#include "stream_statistic.hh"

#include <cassert>
#include <iomanip>

namespace gem5 {

std::map<uint64_t, StreamStatistic> StreamStatistic::staticStats;

StreamStatistic &StreamStatistic::getStaticStat(uint64_t staticStreamId) {
  if (!staticStats.count(staticStreamId)) {
    staticStats.emplace(staticStreamId, staticStreamId);
  }
  return staticStats.at(staticStreamId);
}

void StreamStatistic::sampleLLCElement(const LLCElementSample &s) {
  if (s.firstCheckCycle != 0 && s.valueReadyCycle != 0) {
    if (s.firstCheckCycle > s.valueReadyCycle) {
      numLLCEarlyCycle += s.firstCheckCycle - s.valueReadyCycle;
      numLLCEarlyElement++;
    } else {
      numLLCLateCycle += s.valueReadyCycle - s.firstCheckCycle;
      numLLCLateElement++;
    }
  }
  if (s.issueCycle > 0 && s.valueReadyCycle >= s.issueCycle) {
    auto issueToValueReadyCycle = s.valueReadyCycle - s.issueCycle;
    remoteIssueToValueReadyCycle.sample(issueToValueReadyCycle);
    this->getStaticStat().remoteIssueToValueReadyCycle.sample(
        issueToValueReadyCycle);
  }
  if (s.readyToIssueCycle > 0 && s.issueCycle >= s.readyToIssueCycle) {
    auto readyToIssueCycle = s.issueCycle - s.readyToIssueCycle;
    remoteReadyToIssueDelay.sample(readyToIssueCycle);
    this->getStaticStat().remoteReadyToIssueDelay.sample(readyToIssueCycle);
  }
}

void StreamStatistic::dump(std::ostream &os) const {
#define dumpScalar(stat)                                                       \
  os << std::setw(40) << "  " #stat << ' ' << stat << '\n'
#define dumpScalarIfNonZero(stat)                                              \
  if (stat != 0) {                                                             \
    dumpScalar(stat);                                                          \
  }
#define dumpNamedScalar(name, stat)                                            \
  os << std::setw(40) << (name) << ' ' << (stat) << '\n'
#define dumpAvg(name, dividend, divisor)                                       \
  {                                                                            \
    auto avg = (divisor > 0) ? static_cast<double>(dividend) /                 \
                                   static_cast<double>(divisor)                \
                             : -1;                                             \
    os << std::setw(40) << "  " #name << ' ' << std::setprecision(4)           \
       << std::setw(10) << std::left << avg << std::right << " x " << divisor  \
       << '\n';                                                                \
  }
#define dumpSingleAvgSample(name)                                              \
  {                                                                            \
    if (name.samples > 0) {                                                    \
      dumpAvg(name.avg, name.value, name.samples);                             \
    }                                                                          \
  }
  dumpScalar(numConfigured);
  dumpScalar(numMisConfigured);
  dumpScalar(numFloated);
  dumpScalarIfNonZero(numFloatPUM);
  dumpScalarIfNonZero(numFloatMem);
  dumpScalarIfNonZero(numFloatRewinded);
  dumpScalarIfNonZero(numFloatCancelled);
  dumpScalarIfNonZero(numPseudoFloated);
  dumpScalarIfNonZero(numFineGrainedOffloaded);
  dumpScalar(numAllocated);
  dumpScalar(numWithdrawn);
  dumpScalar(numFetched);
  dumpScalar(numPrefetched);
  dumpScalarIfNonZero(numNDCed);
  dumpScalar(numStepped);
  dumpScalar(numUsed);
  dumpScalar(numAliased);
  dumpScalar(numFlushed);
  dumpScalar(numFaulted);
  dumpScalar(numCycle);
  dumpAvg(avgTurnAroundCycle, numCycle, numStepped);

  dumpScalar(numSample);
  dumpAvg(avgInflyRequest, numInflyRequest, numSample);
  dumpAvg(avgMaxSize, maxSize, numSample);
  dumpAvg(avgAllocSize, allocSize, numSample);
  dumpAvg(avgNumDynStreams, numDynStreams, numSample);

  dumpScalar(numMLCAllocatedSlice);

  if (numRemoteConfig > 0) {
    dumpScalar(numLLCIssueSlice);
    dumpScalar(numLLCSentSlice);
    dumpScalar(numLLCMulticastSlice);
    dumpScalar(numLLCCanMulticastSlice);
    dumpSingleAvgSample(llcReqLat);
    dumpScalarIfNonZero(numLLCFaultSlice);
    dumpScalarIfNonZero(numLLCPredYSlice);
    dumpScalarIfNonZero(numLLCPredNSlice);

    dumpScalar(numMemIssueSlice);
    dumpSingleAvgSample(memReqLat);
    dumpScalar(numRemoteReuseSlice);
    dumpScalarIfNonZero(numRemoteMulticastSlice);

    dumpScalarIfNonZero(numStrands);
    dumpScalarIfNonZero(numPrefetchStrands);

    dumpScalar(numRemoteConfig);
    dumpAvg(avgRemoteConfigNoCCycle, numRemoteConfigNoCCycle, numRemoteConfig);
    dumpAvg(avgRemoteConfigCycle, numRemoteConfigCycle, numRemoteConfig);
    dumpScalar(numRemoteMigrate);
    dumpScalar(numRemoteMigrateCycle);
    dumpScalar(numRemoteMigrateDelayCycle);
    dumpAvg(avgMigrateCycle, numRemoteMigrateCycle, numRemoteMigrate);
    dumpScalar(numRemoteRunCycle);
    dumpAvg(avgRunCyclePerBank, numRemoteRunCycle, (numRemoteMigrate + 1));

    dumpSingleAvgSample(remoteForwardNoCDelay);
  }

  dumpSingleAvgSample(localAliveSlice);
  dumpSingleAvgSample(localCreditNotSentSlice);
  dumpSingleAvgSample(localCreditSentSlice);
  dumpSingleAvgSample(localWaitDataSlice);
  dumpSingleAvgSample(localWaitAckSlice);
  dumpSingleAvgSample(localAckReadySlice);
  dumpSingleAvgSample(localDoneSlice);
  dumpSingleAvgSample(remoteAliveElem);
  dumpSingleAvgSample(remoteInitializedElem);
  dumpSingleAvgSample(remoteReadyToIssueElem);
  dumpSingleAvgSample(remoteIssuedElem);
  dumpSingleAvgSample(remotePredicatedOffElem);
  dumpSingleAvgSample(remoteInflyReq);
  dumpSingleAvgSample(remoteInflyCmp);

  dumpSingleAvgSample(remoteReadyToIssueDelay);
  dumpSingleAvgSample(remoteIssueToValueReadyCycle);
  dumpSingleAvgSample(remoteIssueToReqGenDelay);
  dumpSingleAvgSample(remoteIndReqNoCDelay);
  dumpSingleAvgSample(remoteIndReqStreamBufInjectDelay);
  dumpSingleAvgSample(remoteIndReqMsgBufInjectDelay);
  dumpSingleAvgSample(remoteIndReqNoCInjectDelay);
  dumpSingleAvgSample(remoteToLocalAckNoCDelay);

  dumpAvg(avgLength, numStepped, numConfigured);
  dumpAvg(avgUsed, numUsed, numConfigured);

  dumpScalar(numCoreEarlyElement);
  dumpScalar(numCoreEarlyCycle);
  dumpAvg(avgCoreEarlyCycle, numCoreEarlyCycle, numCoreEarlyElement);

  dumpScalar(numCoreLateElement);
  dumpScalar(numCoreLateCycle);
  dumpAvg(avgCoreLateCycle, numCoreLateCycle, numCoreLateElement);

  if (numMLCEarlySlice > 0 || numMLCLateSlice > 0) {
    dumpScalar(numMLCEarlySlice);
    dumpAvg(avgMLCEarlyCycle, numMLCEarlyCycle, numMLCEarlySlice);
    dumpScalar(numMLCLateSlice);
    dumpAvg(avgMLCLateCycle, numMLCLateCycle, numMLCLateSlice);
  }
  if (numLLCEarlyElement > 0 || numLLCLateElement > 0) {
    dumpScalar(numLLCEarlyElement);
    dumpAvg(avgLLCEarlyCycle, numLLCEarlyCycle, numLLCEarlyElement);
    dumpScalar(numLLCLateElement);
    dumpAvg(avgLLCLateCycle, numLLCLateCycle, numLLCLateElement);
  }

  dumpScalar(numIssuedRequest);
  dumpScalar(numIssuedReadExRequest);
  dumpScalar(numIssuedPrefetchRequest);
  dumpScalar(numCycleRequestLatency);
  dumpAvg(avgRequestLatency, numCycleRequestLatency, numIssuedRequest);

  if (this->idealDataTrafficFix > 0) {
    dumpScalar(idealDataTrafficFix);
    dumpScalar(idealDataTrafficCached);
    dumpScalar(idealDataTrafficFloat);
  }

  dumpScalar(numLLCComputation);
  if (numLLCComputation > 0) {
    dumpAvg(avgLLCComputeLatency, numLLCComputationComputeLatency,
            numLLCComputation);
    dumpAvg(avgLLCWaitComputeLatency, numLLCComputationWaitLatency,
            numLLCComputation);
  }

  dumpScalar(numFloatAtomic);
  if (numFloatAtomic > 0) {
    dumpAvg(avgFloatAtomicRecvCommitCycle, numFloatAtomicRecvCommitCycle,
            numFloatAtomic);
    dumpAvg(avgFloatAtomicWaitForLockCycle, numFloatAtomicWaitForLockCycle,
            numFloatAtomic);
    dumpAvg(avgFloatAtomicWaitForCommitCycle, numFloatAtomicWaitForCommitCycle,
            numFloatAtomic);
    dumpAvg(avgFloatAtomicWaitForUnlockCycle, numFloatAtomicWaitForUnlockCycle,
            numFloatAtomic);
  }

  os << std::setw(40) << "LLCSendTo"
     << "\n";
  dumpSrcDest(this->numLLCSendTo, os);

  os << std::setw(40) << "remoteToLocalMsg"
     << "\n";
  dumpSrcDest(this->remoteToLocalMsg, os);

  os << std::setw(40) << "RemoteNestConfig"
     << "\n";
  dumpSrcDest(this->numRemoteNestConfig, os);

  dumpScalar(numMissL0);
  dumpScalar(numMissL1);
  dumpScalar(numMissL2);

  dumpScalarIfNonZero(remoteNestConfigMaxRegions);

  for (auto idx = 0; idx < this->llcIssueReasons.size(); ++idx) {
    if (this->llcIssueReasons.at(idx) > 0) {
      dumpNamedScalar(llcSEIssueReasonToString(
                          static_cast<LLCStreamEngineIssueReason>(idx)),
                      this->llcIssueReasons.at(idx));
    }
  }

  if (this->numFloatPUM > 0) {
    for (int i = 0; i < MAX_SYNCS; ++i) {
      dumpSingleAvgSample(pumCyclesBetweenSync[i]);
    }
  }

#undef dumpScalar
#undef dumpAvg
}

void StreamStatistic::dumpSrcDest(const SrcDestStatsT &stats,
                                  std::ostream &os) {
  if (!stats.empty()) {
    size_t total = 0;
    for (const auto &entry : stats) {
      total += entry.second;
    }
    os << std::setw(40) << " Total" << ' ' << total << '\n';
    for (const auto &entry : stats) {
      auto from = entry.first.first;
      auto to = entry.first.second;
      auto ratio = static_cast<float>(entry.second) / static_cast<float>(total);
      os << std::setw(5) << from << " -> " << std::setw(5) << to << " "
         << std::setw(10) << entry.second << " " << ratio << "\n";
    }
  }
}

const char *
StreamStatistic::llcSEIssueReasonToString(LLCStreamEngineIssueReason reason) {
#define Case(x)                                                                \
  case x:                                                                      \
    return #x
  switch (reason) {
    Case(Issued);
    Case(IndirectPriority);
    Case(NextSliceNotAllocated);
    Case(NextSliceOverTripCount);
    Case(MulticastPolicy);
    Case(IssueClearCycle);
    Case(MaxInflyRequest);
    Case(MaxEngineInflyRequest);
    Case(MaxIssueWidth);
    Case(PendingMigrate);
    Case(AliasedIndirectUpdate);
    Case(BaseValueNotReady);
    Case(ValueNotReady);
    Case(WaitingPUM);
    Case(NumLLCStreamEngineIssueReason);
#undef Case
  default:
    assert(false && "Invalid LLCStreamEngineIssueReason.");
  }
}

void StreamStatistic::clear() {
  this->numConfigured = 0;
  this->numMisConfigured = 0;
  this->numFloated = 0;
  this->numFloatPUM = 0;
  this->numFloatMem = 0;
  this->numFloatRewinded = 0;
  this->numFloatCancelled = 0;
  this->numPseudoFloated = 0;
  this->numFineGrainedOffloaded = 0;
  this->numAllocated = 0;
  this->numFetched = 0;
  this->numNDCed = 0;
  this->numStepped = 0;
  this->numUsed = 0;
  this->numAliased = 0;
  this->numFlushed = 0;
  this->numFaulted = 0;
  this->numCycle = 0;
  this->numSample = 0;
  this->numInflyRequest = 0;
  this->maxSize = 0;
  this->allocSize = 0;
  this->numDynStreams = 0;
  this->numMLCAllocatedSlice = 0;
  this->numLLCIssueSlice = 0;
  this->numLLCSentSlice = 0;
  this->numLLCMulticastSlice = 0;
  this->numLLCCanMulticastSlice = 0;
  this->numStrands = 0;
  this->numPrefetchStrands = 0;
  this->numLLCFaultSlice = 0;
  this->numLLCPredYSlice = 0;
  this->numLLCPredNSlice = 0;
  this->numMemIssueSlice = 0;
  this->numRemoteReuseSlice = 0;
  this->numRemoteConfig = 0;
  this->numRemoteConfigNoCCycle = 0;
  this->numRemoteConfigCycle = 0;
  this->numRemoteMigrate = 0;
  this->numRemoteMigrateCycle = 0;
  this->numRemoteMigrateDelayCycle = 0;
  this->numRemoteRunCycle = 0;
  this->numCoreEarlyElement = 0;
  this->numCoreEarlyCycle = 0;
  this->numCoreLateElement = 0;
  this->numCoreLateCycle = 0;
  this->numMLCEarlySlice = 0;
  this->numMLCEarlyCycle = 0;
  this->numMLCLateSlice = 0;
  this->numMLCLateCycle = 0;
  this->numLLCEarlyElement = 0;
  this->numLLCEarlyCycle = 0;
  this->numLLCLateElement = 0;
  this->numLLCLateCycle = 0;
  this->numIssuedRequest = 0;
  this->numIssuedReadExRequest = 0;
  this->numIssuedPrefetchRequest = 0;
  this->numCycleRequestLatency = 0;
  this->numMissL0 = 0;
  this->numMissL1 = 0;
  this->numMissL2 = 0;

  this->remoteNestConfigMaxRegions = 0;

  this->numLLCComputation = 0;
  this->numLLCComputationComputeLatency = 0;
  this->numLLCComputationWaitLatency = 0;
  this->numFloatAtomic = 0;
  this->numFloatAtomicRecvCommitCycle = 0;
  this->numFloatAtomicWaitForCommitCycle = 0;
  this->numFloatAtomicWaitForLockCycle = 0;
  this->numFloatAtomicWaitForUnlockCycle = 0;
  this->numLLCSendTo.clear();
  this->remoteToLocalMsg.clear();
  this->numRemoteNestConfig.clear();

  this->numRemoteMulticastSlice = 0;

  this->idealDataTrafficFix = 0;
  this->idealDataTrafficCached = 0;
  this->idealDataTrafficFloat = 0;

  this->localAliveSlice.clear();
  this->localCreditNotSentSlice.clear();
  this->localCreditSentSlice.clear();
  this->localWaitAckSlice.clear();
  this->localWaitDataSlice.clear();
  this->localAckReadySlice.clear();
  this->localDoneSlice.clear();
  this->remoteAliveElem.clear();
  this->remoteInitializedElem.clear();
  this->remoteReadyToIssueElem.clear();
  this->remoteIssuedElem.clear();
  this->remotePredicatedOffElem.clear();
  this->remoteInflyReq.clear();
  this->remoteInflyCmp.clear();
  this->remoteForwardNoCDelay.clear();
  this->remoteReadyToIssueDelay.clear();
  this->remoteIssueToValueReadyCycle.clear();
  this->remoteIssueToReqGenDelay.clear();
  this->remoteIndReqNoCDelay.clear();
  this->remoteIndReqStreamBufInjectDelay.clear();
  this->remoteIndReqMsgBufInjectDelay.clear();
  this->remoteIndReqNoCInjectDelay.clear();
  this->remoteToLocalAckNoCDelay.clear();
  this->llcReqLat.clear();
  this->memReqLat.clear();

  for (auto &reasons : this->llcIssueReasons) {
    reasons = 0;
  }

  for (auto &pumCycles : this->pumCyclesBetweenSync) {
    pumCycles.clear();
  }
}
} // namespace gem5
