#ifndef __MEM_RUBY_COMMON_PC_REQUEST_RECORDER_HH__
#define __MEM_RUBY_COMMON_PC_REQUEST_RECORDER_HH__

#include "base/types.hh"
#include "mem/req_hit_level.hh"
#include "mem/ruby/protocol/RubyRequestType.hh"

#include <unordered_set>

namespace gem5 {
namespace ruby {

class PCRequestRecorder {
public:
  PCRequestRecorder(const std::string &_name) : name(_name) {}

  using HitPlaceE = ReqHitPlaceE;

  void recordReq(Addr pc, RubyRequestType type, bool isStream,
                 const char *streamName, Cycles latency, int hitLevel);

  void reset();
  void dump();

private:
  std::string name;
  std::ostream *pcLatencyStream = nullptr;

  //! Stats for recording latency by PC.
  struct RequestLatencyStats {
    RequestLatencyStats(Addr _pc, RubyRequestType _type, bool _isStream,
                        const char *_streamName)
        : pc(_pc), type(_type), isStream(_isStream), streamName(_streamName) {
      for (int i = HitPlaceE::INVALID; i <= HitPlaceE::LAST_HITPLACE; ++i) {
        this->getHitLevel(static_cast<HitPlaceE>(i)).first = 0;
      }
    }
    const Addr pc;
    const RubyRequestType type;
    const bool isStream;
    const char *streamName = nullptr;
    mutable uint64_t totalReqs = 0;
    mutable uint64_t totalLatency = 0;
    mutable std::array<std::pair<uint64_t, uint64_t>, HitPlaceE::NUM_HITPLACE>
        hitLevels;

    void recordHitLevel(HitPlaceE hitLevel, Cycles latency) const {
      hitLevels[hitLevel + 1].first++;
      hitLevels[hitLevel + 1].second += latency;
    }

    std::pair<uint64_t, uint64_t> &getHitLevel(HitPlaceE hitLevel) const {
      return hitLevels[hitLevel + 1];
    }

    bool operator==(const RequestLatencyStats &other) const {
      return pc == other.pc && type == other.type && isStream == other.isStream;
    }
    bool operator!=(const RequestLatencyStats &other) const {
      return !(this->operator==(other));
    }
    bool operator<(const RequestLatencyStats &other) const {
      if (pc != other.pc) {
        return pc < other.pc;
      }
      if (type != other.type) {
        return type < other.type;
      }
      return isStream < other.isStream;
    }
  };

  struct RequestLatencyStatsHasher {
    std::size_t operator()(const RequestLatencyStats &key) const {
      return (std::hash<int>()(key.type)) ^ (std::hash<uint64_t>()(key.pc)) ^
             (std::hash<bool>()(key.isStream));
    }
  };
  std::unordered_set<RequestLatencyStats, RequestLatencyStatsHasher>
      pcLatencySet;
};

} // namespace ruby
} // namespace gem5

#endif