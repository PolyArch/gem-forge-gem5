#include "LLCStreamRangeBuilder.hh"

#include "debug/StreamRangeSync.hh"
#define DEBUG_TYPE StreamRangeSync
#include "../stream_log.hh"

namespace gem5 {

LLCStreamRangeBuilder::LLCStreamRangeBuilder(LLCDynStream *_stream,
                                             int64_t _totalTripCount)
    : stream(_stream), totalTripCount(_totalTripCount) {}

void LLCStreamRangeBuilder::addElementAddress(uint64_t elementIdx, Addr vaddr,
                                              Addr paddr, int size) {
  /**
   * So far this is pretty limited and we enforce these checks:
   * 1. The element can not across multiple pages.
   * 2. Element address is added in order.
   */
  if (elementIdx != this->nextElementIdx) {
    LLC_S_PANIC(
        this->stream->getDynStrandId(),
        "[RangeBuilder] Element not added in order: expect %llu got %llu.",
        this->nextElementIdx, elementIdx);
  }
  if (vaddr == 0 || paddr == 0) {
    LLC_S_PANIC(this->stream->getDynStrandId(),
                "[RangeBuilder] Invalid element %llu vaddr %#x paddr %#x.",
                elementIdx, vaddr, paddr);
  }
  const Addr PageSize = 4096;
  if (size >= PageSize) {
    LLC_S_PANIC(this->stream->getDynStrandId(),
                "[RangeBuilder] Element across pages: vaddr %#x, size %d.",
                vaddr, size);
  }
  // We allow spanning at most two pages.
  auto firstPageRemain = PageSize - (paddr % PageSize);
  auto firstPageSize = size < firstPageRemain ? size : firstPageRemain;
  auto secondPageSize = size - firstPageSize;
  if (this->totalTripCount != InvalidTotalTripCount &&
      elementIdx >= this->totalTripCount) {
    LLC_S_PANIC(this->stream->getDynStrandId(),
                "[RangeBuilder] ElementIdx overflow, total %llu.",
                this->totalTripCount);
  }
  this->vaddrRange.add(vaddr, vaddr + size);
  this->paddrRange.add(paddr, paddr + firstPageSize);
  if (secondPageSize > 0) {
    // Hack to translate the second page here.
    auto secondPageVAddr = vaddr + firstPageSize;
    Addr secondPagePAddr;
    if (!this->stream->translateToPAddr(secondPageVAddr, secondPagePAddr)) {
      LLC_S_PANIC(
          this->stream->getDynStrandId(),
          "[RangeBuilder] Element %llu Failed to translate the second page.",
          elementIdx);
    }
    this->paddrRange.add(secondPagePAddr, secondPageSize);
  }
  LLC_S_DPRINTF(
      this->stream->getDynStrandId(),
      "[RangeBuilder] Add Element %llu VAddr %#x PAddr %#x Size %d.\n",
      elementIdx, vaddr, paddr, size);
  this->nextElementIdx++;
  this->tryBuildRange();
}

bool LLCStreamRangeBuilder::hasReadyRanges() const {
  if (this->readyRanges.empty()) {
    return false;
  }
  // Recursively check all indirect streams.
  for (auto dynIS : this->stream->getIndStreams()) {
    if (dynIS->shouldRangeSync() &&
        !dynIS->getRangeBuilder()->hasReadyRanges()) {
      return false;
    }
  }
  return true;
}

DynStreamAddressRangePtr LLCStreamRangeBuilder::popReadyRange() {
  auto range = this->readyRanges.front();
  this->readyRanges.pop_front();
  // Recursively merge all indirect streams' range.
  for (auto dynIS : this->stream->getIndStreams()) {
    if (dynIS->shouldRangeSync()) {
      auto indRange = dynIS->getRangeBuilder()->popReadyRange();
      range->addRange(indRange);
    }
  }
  return range;
}

void LLCStreamRangeBuilder::tryBuildRange() {
  if (this->nextRangeTailElementIdxQueue.empty()) {
    LLC_S_PANIC(this->stream->getDynStrandId(),
                "[RangeBuilder] No NextRangeTailElementIdx to build range.");
    return;
  }
  auto nextRangeTailElementIdx = this->nextRangeTailElementIdxQueue.front();
  if ((this->totalTripCount != InvalidTotalTripCount &&
       this->nextElementIdx == this->totalTripCount) ||
      this->nextElementIdx == nextRangeTailElementIdx) {
    // Time to build another range.
    DynStrandElementRangeId elementRange;
    elementRange.strandId = this->stream->getDynStrandId();
    elementRange.lhsElementIdx = this->prevBuiltElementIdx;
    elementRange.rhsElementIdx = this->nextElementIdx;
    auto range = std::make_shared<DynStreamAddressRange>(
        elementRange, this->vaddrRange, this->paddrRange);
    LLC_S_DPRINTF(this->stream->getDynStrandId(), "[RangeBuilder] Built %s.\n",
                  *range);
    this->readyRanges.push_back(range);
    this->prevBuiltElementIdx = this->nextElementIdx;
    this->vaddrRange.clear();
    this->paddrRange.clear();
    this->nextRangeTailElementIdxQueue.pop_front();
  }
}

void LLCStreamRangeBuilder::pushNextRangeTailElementIdx(
    uint64_t nextRangeTailElemIdx) {
  /**
   * Due to multi-slice elements, it is possible that we have multiple same
   * nextRangeTailElemIdx pushed. Handle this case by ensuring non-decreasing
   * property.
   */
  if (nextRangeTailElemIdx == 0) {
    LLC_S_PANIC(this->stream->getDynStrandId(),
                "[RangeBuilder] Zero NextRangeTailElementIdx.\n");
  }
  if (this->prevNextRangeTailElementIdx > nextRangeTailElemIdx) {
    LLC_S_PANIC(this->stream->getDynStrandId(),
                "[RangeBuilder] NextRangeTailElementIdx out-of-order %llu < "
                "back %llu.",
                nextRangeTailElemIdx, this->prevNextRangeTailElementIdx);
  } else if (this->prevNextRangeTailElementIdx == nextRangeTailElemIdx) {
    LLC_S_DPRINTF(this->stream->getDynStrandId(),
                  "[RangeBuilder] Ignore NextRangeTailElementIdx %llu == "
                  "PrevTailElementIdx %llu.",
                  nextRangeTailElemIdx, this->prevNextRangeTailElementIdx);
    return;
  }
  LLC_S_DPRINTF(this->stream->getDynStrandId(),
                "[RangeBuilder] NextRangeTailElementIdx %llu.\n",
                nextRangeTailElemIdx);
  this->nextRangeTailElementIdxQueue.push_back(nextRangeTailElemIdx);
  this->prevNextRangeTailElementIdx = nextRangeTailElemIdx;
}

void LLCStreamRangeBuilder::receiveLoopBoundRet(int64_t totalTripCount) {
  if (this->totalTripCount != InvalidTotalTripCount) {
    LLC_S_PANIC(
        this->stream->getDynStrandId(),
        "[RangeBuilder] LLCRangeBuilder reset TotalTripCount %lld to %lld.",
        this->totalTripCount, totalTripCount);
  }
  LLC_S_DPRINTF(this->stream->getDynStrandId(),
                "[RangeBuilder] LLCRangeBuilder cut TotalTripCount %lld to "
                "%lld. PrevBuiltElementIdx %llu NextElementIdx %llu.\n",
                this->totalTripCount, totalTripCount, this->prevBuiltElementIdx,
                this->nextElementIdx);
  this->totalTripCount = totalTripCount;
  if (this->stream->shouldRangeSync()) {
    if (this->prevBuiltElementIdx < totalTripCount) {
      this->tryBuildRange();
    }
  }
}
} // namespace gem5
