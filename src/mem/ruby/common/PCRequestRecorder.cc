#include "PCRequestRecorder.hh"

#include "base/logging.hh"
#include "base/output.hh"

#include <algorithm>
#include <vector>

namespace gem5 {
namespace ruby {

void PCRequestRecorder::recordReq(Addr pc, RubyRequestType type, bool isStream,
                                  const char *streamName, Cycles latency,
                                  int hitLevel) {
  auto stat = this->pcLatencySet.emplace(pc, type, isStream, streamName).first;
  assert(stat->isStream == isStream && "Changed isStream.");
  stat->totalReqs++;
  stat->totalLatency += latency;
  stat->recordHitLevel(static_cast<HitPlaceE>(hitLevel), latency);
}

void PCRequestRecorder::reset() { this->pcLatencySet.clear(); }

void PCRequestRecorder::dump() {
  if (this->pcLatencySet.empty()) {
    return;
  }
  std::vector<const RequestLatencyStats *> sortedStats;
  sortedStats.reserve(this->pcLatencySet.size());
  uint64_t totalReqs = 0;
  uint64_t totalLatency = 0;
  for (const auto &stat : this->pcLatencySet) {
    sortedStats.emplace_back(&stat);
    totalReqs += stat.totalReqs;
    totalLatency += stat.totalLatency;
  }
  std::sort(sortedStats.begin(), sortedStats.end(),
            [this](const RequestLatencyStats *a,
                   const RequestLatencyStats *b) -> bool {
              return a->operator<(*b);
            });

  if (!this->pcLatencyStream) {
    auto dir = simout.findOrCreateSubdirectory("pc.req");
    const std::string fname = csprintf("pclat.%s", this->name);
    this->pcLatencyStream = dir->findOrCreate(fname)->stream();
  }
  ccprintf(*this->pcLatencyStream, "---------------------------\n");
  for (const auto &stat : sortedStats) {
    ccprintf(*this->pcLatencyStream,
             "%10#x %4s %4s %10llu (%05.2f) %12llu (%05.2f) %8.2f %s\n",
             stat->pc, stat->isStream ? "SSP" : "Core",
             RubyRequestType_to_string(stat->type), stat->totalReqs,
             static_cast<double>(stat->totalReqs * 100) / totalReqs,
             stat->totalLatency,
             static_cast<double>(stat->totalLatency * 100) / totalLatency,
             static_cast<double>(stat->totalLatency) /
                 static_cast<double>(stat->totalReqs),
             stat->streamName ? stat->streamName : "");
    for (int i = HitPlaceE::INVALID; i <= HitPlaceE::LAST_HITPLACE; ++i) {
      auto &v = stat->getHitLevel(static_cast<HitPlaceE>(i));
      if (v.first > 0) {
        ccprintf(*this->pcLatencyStream, "    Hit %2d %12llu (%5.2f) %8.2f\n", i,
                 v.first, static_cast<double>(v.first * 100) / stat->totalReqs,
                 static_cast<double>(v.second) / v.first);
      }
    }
  }
}
} // namespace ruby
} // namespace gem5
