#ifndef __CPU_GEM_FORGE_DYN_STREAM_SLICE_ID_HH__
#define __CPU_GEM_FORGE_DYN_STREAM_SLICE_ID_HH__

#include "DynStrandElementRangeId.hh"

namespace gem5 {

/**
 * The core stream engine manages stream at granularity of element.
 * However, this is not ideal for cache stream engine, as we want to
 * coalesce continuous elements to the same cache line. Things get
 * more complicated when there is overlapping between elements and
 * one element can span across multiple cache lines.
 *
 * This represent the basic unit how the cache system manages streams.
 * A slice is a piece of continuous memory, and does not span across
 * cache lines. It also remembers elements within this slice,
 * [lhsElementIdx, rhsElementIdx).
 *
 * Notice that one element may span across multiple cache lines, and
 * thus the data in a slice may only be a portion of the whole element.
 */
struct DynStreamSliceId {
  DynStrandElementRangeId elementRange;
  /**
   * Hack: This is abused for many things:
   * 1. Element vaddr for indirect streams.
   * 2. Line vaddr for direct sliced streams.
   * 3. Line **paddr** distinguish multicast receiving streams.
   */
  Addr vaddr;
  int size;

  DynStreamSliceId() : elementRange(), vaddr(0), size(0) {}

  bool isValid() const { return this->elementRange.isValid(); }
  void clear() {
    this->elementRange.clear();
    this->vaddr = 0;
    this->size = 0;
  }

  DynStreamId &getDynStreamId() { return this->getDynStrandId().dynStreamId; }
  const DynStreamId &getDynStreamId() const {
    return this->getDynStrandId().dynStreamId;
  }

  DynStrandId &getDynStrandId() { return this->elementRange.strandId; }
  const DynStrandId &getDynStrandId() const {
    return this->elementRange.strandId;
  }

  const uint64_t &getStartIdx() const {
    return this->elementRange.getLHSElementIdx();
  }
  uint64_t &getStartIdx() { return this->elementRange.getLHSElementIdx(); }

  const uint64_t &getEndIdx() const { return this->elementRange.rhsElementIdx; }
  uint64_t &getEndIdx() { return this->elementRange.rhsElementIdx; }

  uint64_t getNumElements() const {
    return this->elementRange.getNumElements();
  }
  int getSize() const { return this->size; }

  bool operator==(const DynStreamSliceId &other) const {
    return this->elementRange == other.elementRange;
  }

  bool operator!=(const DynStreamSliceId &other) const {
    return !(this->operator==(other));
  }
};

std::ostream &operator<<(std::ostream &os, const DynStreamSliceId &id);

struct DynStreamSliceIdHasher {
  std::size_t operator()(const DynStreamSliceId &key) const {
    return (DynStrandElementRangeIdHasher()(key.elementRange)) ^
           std::hash<uint64_t>()(key.vaddr);
  }
};

} // namespace gem5

#endif