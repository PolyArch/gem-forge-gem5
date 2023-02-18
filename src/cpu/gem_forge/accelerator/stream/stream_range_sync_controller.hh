#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_RANGE_SYNC_CONTROLLER_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_RANGE_SYNC_CONTROLLER_HH__

#include "stream_engine.hh"

namespace gem5 {

class StreamRangeSyncController {
public:
  StreamRangeSyncController(StreamEngine *_se);

  /**
   * Check that ranges for all active dynamic streams are ready,
   * so the core can start to commit and check against the range.
   * @return The first DynS that has no ready range.
   */
  DynStream *getNoRangeDynS();

  /**
   * Helper function to get the element idx we should check range for.
   */
  uint64_t getCheckElemIdx(DynStream *dynS);

private:
  StreamEngine *se;

  using DynStreamVec = std::vector<DynStream *>;

  DynStreamVec getCurrentDynStreams();
  void updateCurrentWorkingRange(DynStreamVec &dynStreams);
  void checkAliasBetweenRanges(DynStreamVec &dynStreams,
                               const DynStreamAddressRangePtr &newRange);
};

} // namespace gem5

#endif