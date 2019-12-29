#ifndef __GEM_FORGE_SLICED_DYNAMIC_STREAM_H__
#define __GEM_FORGE_SLICED_DYNAMIC_STREAM_H__

/**
 * Since the cache stream engines manage streams in slice granularity,
 * we introduce this utility class to slice the stream.
 *
 * Only direct streams can be sliced.
 */

#include "CacheStreamConfigureData.hh"
#include "DynamicStreamSliceId.hh"

#include <deque>

class SlicedDynamicStream {
public:
  SlicedDynamicStream(CacheStreamConfigureData *_configData);

  DynamicStreamSliceId getNextSlice();
  const DynamicStreamSliceId &peekNextSlice() const;

  /**
   * Check if we have allocated beyond the end of the stream.
   * Instead of terminating the stream, here I take a "soft"
   * approach to ease the implementation complexicity.
   *
   * Notice that we allow (totalTripCount + 1) elements as
   * StreamEnd will consume one element and we have to be synchronized
   * with the core's StreamEngine.
   */
  bool hasOverflowed() const {
    return this->totalTripCount > 0 &&
           (this->peekNextSlice().startIdx >= (this->totalTripCount + 1));
  }

  int64_t getTotalTripCount() const { return this->totalTripCount; }

  /**
   * Helper function to get element vaddr and size.
   */
  Addr getElementVAddr(uint64_t elementIdx) const;
  int32_t getElementSize() const { return this->elementSize; }

private:
  // TODO: Move this out of SlicedDynamicStream and make it only
  // TODO: worry about slicing.
  DynamicStreamId streamId;
  DynamicStreamFormalParamV formalParams;
  AddrGenCallbackPtr addrGenCallback;
  int32_t elementSize;
  /**
   * -1 means indefinite.
   */
  const int64_t totalTripCount;

  /**
   * Internal states.
   * ! Evil trick to make peekNextSlice constant.
   */
  mutable uint64_t tailIdx;
  /**
   * The headIdx that can be checked for slicing.
   */
  mutable uint64_t sliceHeadIdx;
  mutable std::deque<DynamicStreamSliceId> slices;

  void allocateOneElement() const;
};

#endif
