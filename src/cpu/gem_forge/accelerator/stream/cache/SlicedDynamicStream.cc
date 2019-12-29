#include "SlicedDynamicStream.hh"
#include "debug/SlicedDynamicStream.hh"
#include "mem/ruby/common/Address.hh"
#include "mem/ruby/system/RubySystem.hh"

#define DEBUG_TYPE SlicedDynamicStream
#include "../stream_log.hh"

SlicedDynamicStream::SlicedDynamicStream(CacheStreamConfigureData *_configData,
                                         bool _coalesceContinuousElements)
    : streamId(_configData->dynamicId), formalParams(_configData->formalParams),
      addrGenCallback(_configData->addrGenCallback),
      elementSize(_configData->elementSize),
      totalTripCount(_configData->totalTripCount),
      coalesceContinuousElements(_coalesceContinuousElements),
      tailElementIdx(0), sliceHeadElementIdx(0) {}

DynamicStreamSliceId SlicedDynamicStream::getNextSlice() {
  while (slices.empty() ||
         slices.front().rhsElementIdx == this->tailElementIdx) {
    // Allocate until it's guaranteed that the first slice has no more
    // overlaps.
    this->allocateOneElement();
  }
  auto slice = this->slices.front();
  this->slices.pop_front();
  return slice;
}

const DynamicStreamSliceId &SlicedDynamicStream::peekNextSlice() const {
  while (slices.empty() ||
         slices.front().rhsElementIdx == this->tailElementIdx) {
    // Allocate until it's guaranteed that the first slice has no more
    // overlaps.
    this->allocateOneElement();
  }
  return this->slices.front();
}

Addr SlicedDynamicStream::getElementVAddr(uint64_t elementIdx) const {
  return this->addrGenCallback->genAddr(elementIdx, this->formalParams,
                                        getStreamValueFail);
}

void SlicedDynamicStream::allocateOneElement() const {

  // Let's not worry about indirect streams here.
  auto lhs = this->getElementVAddr(this->tailElementIdx);
  auto rhs = lhs + this->elementSize;

  // Break to cache line granularity, [lhsBlock, rhsBlock]
  auto lhsBlock = makeLineAddress(lhs);
  auto rhsBlock = makeLineAddress(rhs - 1);

  DYN_S_DPRINTF(this->streamId, "Allocate element %llu, block [%#x, %#x].\n",
                this->tailElementIdx, lhsBlock, rhsBlock);

  /**
   * Check if we can try to coalesce continuous elements.
   * Set the flag && not overflow.
   */
  auto curBlock = lhsBlock;
  if (this->coalesceContinuousElements &&
      !this->hasOverflowed(this->tailElementIdx)) {
    if (!this->slices.empty() && lhsBlock < this->slices.back().vaddr) {
      /**
       * Special case to handle decreasing address.
       * If there is a bump back to lower address, we make sure that it has no
       * overlap with existing slices and restart.
       */
      for (auto &slice : this->slices) {
        assert(rhsBlock < slice.vaddr && "Overlapped decreasing element.");
      }
      // Set sliceHeadElementIdx so that slicing branch below will ignore
      // previous slices and restart.
      this->sliceHeadElementIdx = this->tailElementIdx;
    } else {
      // Non-decreasing case, keep going.
      // Update existing slices to the new element if there is overlap.
      for (auto &slice : this->slices) {
        if (slice.lhsElementIdx < this->sliceHeadElementIdx) {
          // This slice is already "sealed" by a decreasing element.
          continue;
        }
        if (slice.vaddr == curBlock) {
          assert(slice.rhsElementIdx == tailElementIdx &&
                 "Hole in overlapping elements.");
          slice.rhsElementIdx++;
          curBlock += RubySystem::getBlockSizeBytes();
          if (curBlock > rhsBlock) {
            // We are done.
            break;
          }
        }
      }
    }
  } else {
    // Simple case: no coalescing.
    // For sanity check: we update the sliceHeadElementIdx.
    this->sliceHeadElementIdx = this->tailElementIdx;
  }

  // Insert new slices.
  while (curBlock <= rhsBlock) {
    this->slices.emplace_back();
    auto &slice = this->slices.back();
    slice.streamId = this->streamId;
    slice.lhsElementIdx = this->tailElementIdx;
    slice.rhsElementIdx = this->tailElementIdx + 1;
    slice.vaddr = curBlock;
    slice.size = RubySystem::getBlockSizeBytes();
    curBlock += RubySystem::getBlockSizeBytes();
  }

  this->tailElementIdx++;
}