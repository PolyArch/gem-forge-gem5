#ifndef __CPU_GEM_FORGE_LLC_STREAM_SLICE_HH__
#define __CPU_GEM_FORGE_LLC_STREAM_SLICE_HH__

#include "DynStreamSliceId.hh"

#include "mem/ruby/common/DataBlock.hh"

#include <memory>

namespace gem5 {

class LLCStreamSlice;
using LLCStreamSlicePtr = std::shared_ptr<LLCStreamSlice>;

class LLCStreamEngine;

/**
 * Each LLCDynStream is managed at two level of granularity:
 * LLCStreamElement:
 *  This is the basic unit to interact with the core (one iteration).
 *  Thus, this is used for computing and range-sync.
 * LLCStreamSlice:
 *  This is the basic unit to interact with the cache (one request).
 *
 * There exists a mapping relationship between these two units:
 * 1. For direct streams, this is a multi-to-multi mapping, subjected
 * to coalescing continuous elements requesting the same cache line,
 * and multi-line elements.
 * 2. For indirect streams, this is one-to-many mapping, as one element
 * can still access at most two lines, but we don't coalesce indirect
 * elements to the same cache line.
 *
 * There are some exceptions:
 * 1. If an element is a placeholder to receive data from a stream in
 * other bank, it has no corresponding slice as it does not issue to
 * memory.
 * 2. ReductionStream has no slices, of course.
 *
 * The element remembers a vector of pointers, but not the other way
 * around to break circular dependence.
 */

class Stream;
class LLCStreamSlice {
public:
  LLCStreamSlice(Stream *_S, const DynStreamSliceId &_sliceId);

  enum State {
    /**
     * The states are:
     * - INITIALIZED: Initialized in the MLC SE. Can not be used yet in LLC.
     * - ALLOCATED: The LLC SE received the credit and allocated it.
     * - ISSUED: The LLC SE issued the request to the cache.
     * - RESPONEDED: The LLC SE already received the response.
     *
     * - FAULTED: The slice has faulted virtual address.
     * - RELEASED: The slice is released by LLC SE.
     *
     * Some tricky points:
     * 1. For indirect requests, it is the remote LLC SE who received the
     * response.
     * 2. For streams that has computation with loaded values, e.g.
     * Atomic/Update streams, they will compute in RESPONDED state.
     */
    INITIALIZED,
    ALLOCATED,
    ISSUED,
    RESPONDED,
    FAULTED,
    RELEASED,
  };
  static const char *stateToString(State state);
  State getState() const { return this->state; }

  void allocate(LLCStreamEngine *llcSE);
  void issue();
  void responded(const ruby::DataBlock &loadBlock, const ruby::DataBlock &storeBlock);
  void faulted();
  void released();

  const DynStreamSliceId &getSliceId() const { return this->sliceId; }

  const ruby::DataBlock &getLoadBlock() const { return this->loadBlock; }
  const ruby::DataBlock &getStoreBlock() const { return this->storeBlock; }

  bool isLoadComputeValueSent() const { return this->loadComputeValueSent; }
  void setLoadComputeValueSent();

private:
  Stream *S;
  DynStreamSliceId sliceId;
  State state = State::INITIALIZED;
  LLCStreamEngine *llcSE = nullptr;
  ruby::DataBlock loadBlock;
  ruby::DataBlock storeBlock;

  /**
   * Whether the LoadComputeValue has been sent to the core.
   */
  bool loadComputeValueSent = false;

  /**
   * Two extra states for Update slices.
   * Whether the Update slice has been processed.
   * Whether the computation is done.
   * Temporary results waiting for final computated.
   */
public:
  void setProcessed();
  bool isProcessed() const { return this->processed; }

private:
  bool processed = false;

private:
  /**
   * Cycles for statistics.
   */
  Cycles issuedCycle = Cycles(0);
  Cycles respondedCycle = Cycles(0);
};

} // namespace gem5

#endif