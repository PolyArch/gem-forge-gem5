#ifndef __CPU_GEM_FORGE_DYN_STREAM_ADDRESS_RANGE_ID_HH__
#define __CPU_GEM_FORGE_DYN_STREAM_ADDRESS_RANGE_ID_HH__

#include "DynStrandElementRangeId.hh"

#include <vector>

namespace gem5 {

/**
 * This is a simple helper structure that represents a range of elements
 * from [lhsElementIdx, rhsElementIdx].
 */

struct AddressRange {
  Addr lhs = 0;
  Addr rhs = 0;
  Addr size() const { return this->rhs - this->lhs; }
  void add(Addr l, Addr r) {
    if (this->size() == 0) {
      this->lhs = l;
      this->rhs = r;
      return;
    }
    this->lhs = std::min(this->lhs, l);
    this->rhs = std::max(this->rhs, r);
  }
  void add(const AddressRange &range) { this->add(range.lhs, range.rhs); }
  void clear() {
    this->lhs = 0;
    this->rhs = 0;
  }
  bool hasOverlap(Addr l, Addr r) const {
    return !(r <= this->lhs || l >= this->rhs);
  }
  bool hasOverlap(const AddressRange &other) const {
    return this->hasOverlap(other.lhs, other.rhs);
  }
};

std::ostream &operator<<(std::ostream &os, const AddressRange &range);

struct DynStreamAddressRange;
using DynStreamAddressRangePtr = std::shared_ptr<DynStreamAddressRange>;
using DynStreamAddressRangeVec = std::vector<DynStreamAddressRangePtr>;

struct DynStreamAddressRange {
  DynStrandElementRangeId elementRange;
  AddressRange vaddrRange;
  AddressRange paddrRange;

  // Store the unioned address range.
  DynStreamAddressRangeVec subRanges;

  DynStreamAddressRange(const DynStrandElementRangeId &_elementRange,
                        const AddressRange &_vaddrRange,
                        const AddressRange &_paddrRange);

  void addRange(DynStreamAddressRangePtr &range);

  bool isValid() const { return this->elementRange.isValid(); }
  bool isUnion() const { return this->subRanges.size() > 0; }
  uint64_t getNumElements() const {
    return this->elementRange.getNumElements();
  }
};

std::ostream &operator<<(std::ostream &os, const DynStreamAddressRange &range);

} // namespace gem5

#endif