/**
 * @author: Zhengrong Wang
 * @file
 * There is a need to collect statistics of a specific request.
 * This class should serve as a placeholder of the statistics of
 * a request. The cache hierarchy can set it if it is presented.
 *
 * Ruby system can also support this statistics.
 */

#ifndef __MEM_REQUEST_STATISTIC_HH__
#define __MEM_REQUEST_STATISTIC_HH__

#include <memory>

struct RequestStatistic {
  enum HitPlaceE {
    INVALID = -1,
    L0_CACHE = 0,
    L1_CACHE = 1,
    L2_CACHE = 2,
    L3_CACHE = 3,
    MEM = 4,
    L1_STREAM_BUFFER = 5,
  };
  HitPlaceE hitCacheLevel;
  RequestStatistic() : hitCacheLevel(HitPlaceE::INVALID) {}
  void setHitCacheLevel(int hitCacheLevel) {
    this->hitCacheLevel = static_cast<HitPlaceE>(hitCacheLevel);
  }
};

typedef std::shared_ptr<RequestStatistic> RequestStatisticPtr;

#endif