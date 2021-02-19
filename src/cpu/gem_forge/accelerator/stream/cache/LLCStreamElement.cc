#include "LLCStreamElement.hh"

#include "mem/simple_mem.hh"

#include "debug/LLCRubyStreamBase.hh"
#define DEBUG_TYPE LLCRubyStreamBase
#include "../stream_log.hh"

LLCStreamElement::LLCStreamElement(Stream *_S,
                                   AbstractStreamAwareController *_controller,
                                   const DynamicStreamId &_dynStreamId,
                                   uint64_t _idx, Addr _vaddr, int _size)
    : S(_S), controller(_controller), dynStreamId(_dynStreamId), idx(_idx),
      size(_size), vaddr(_vaddr), readyBytes(0) {
  if (this->size > sizeof(this->value)) {
    panic("LLCStreamElement size overflow %d, %s.\n", this->size,
          this->dynStreamId);
  }
  if (!this->controller) {
    panic("LLCStreamElement allocated without controller.\n");
  }
  this->value.fill(0);
}

int LLCStreamElement::curLLCBank() const {
  return this->controller->getMachineID().getNum();
}

StreamValue LLCStreamElement::getValue(int offset, int size) const {
  if (this->size < offset + size) {
    panic("Try to get StreamValue (offset %d size %d) for LLCStreamElement of "
          "size %d.",
          offset, size, this->size);
  }
  StreamValue v;
  memcpy(v.uint8Ptr(), this->getUInt8Ptr(offset), size);
  return v;
}

uint8_t *LLCStreamElement::getUInt8Ptr(int offset) {
  assert(offset < this->size);
  return reinterpret_cast<uint8_t *>(this->value.data()) + offset;
}

const uint8_t *LLCStreamElement::getUInt8Ptr(int offset) const {
  assert(offset < this->size);
  return reinterpret_cast<const uint8_t *>(this->value.data()) + offset;
}

uint64_t LLCStreamElement::getUInt64ByStreamId(uint64_t streamId) const {
  assert(this->isReady());
  int32_t offset = 0;
  int size = this->size;
  this->S->getCoalescedOffsetAndSize(streamId, offset, size);
  assert(size <= sizeof(uint64_t) && "ElementSize overflow.");
  assert(offset + size <= this->size && "Size overflow.");
  return GemForgeUtils::rebuildData(this->getUInt8Ptr(offset), size);
}

void LLCStreamElement::setValue(const StreamValue &value) {
  assert(this->readyBytes == 0 && "Already ready.");
  if (this->size > sizeof(StreamValue)) {
    panic("Try to set StreamValue for LLCStreamElement of size %d.",
          this->size);
  }
  memcpy(this->getUInt8Ptr(), value.uint8Ptr(), this->size);
  this->readyBytes += this->size;
}

int LLCStreamElement::computeOverlap(Addr rangeVAddr, int rangeSize,
                                     int &rangeOffset,
                                     int &elementOffset) const {
  if (this->vaddr == 0) {
    panic("Try to computeOverlap without elementVAddr.");
  }
  // Compute the overlap between the element and the slice.
  Addr overlapLHS = std::max(this->vaddr, rangeVAddr);
  Addr overlapRHS = std::min(this->vaddr + this->size, rangeVAddr + rangeSize);
  // Check that the overlap is within the same line.
  assert(overlapLHS < overlapRHS && "Empty overlap.");
  assert(makeLineAddress(overlapLHS) == makeLineAddress(overlapRHS - 1) &&
         "Illegal overlap.");
  auto overlapSize = overlapRHS - overlapLHS;
  rangeOffset = overlapLHS - rangeVAddr;
  elementOffset = overlapLHS - this->vaddr;
  return overlapSize;
}

void LLCStreamElement::extractElementDataFromSlice(
    GemForgeCPUDelegator *cpuDelegator, const DynamicStreamSliceId &sliceId,
    const DataBlock &dataBlock) {
  /**
   * Extract the element data and update the LLCStreamElement.
   */
  auto elementIdx = this->idx;
  auto elementSize = this->size;
  if (this->vaddr == 0) {
    // This is indirect stream, at most one element per slice.
    // We recover the element vaddr here.
    if (sliceId.getNumElements() > 1) {
      LLC_SLICE_PANIC(sliceId, "LLCIndirectSlice should only have 1 element.");
    }
    if (sliceId.getSize() != elementSize) {
      LLC_SLICE_PANIC(sliceId,
                      "Can not reconstruct multi-line indirect element");
    }
    this->vaddr = sliceId.vaddr;
  }

  int sliceOffset;
  int elementOffset;
  int overlapSize = this->computeOverlap(sliceId.vaddr, sliceId.getSize(),
                                         sliceOffset, elementOffset);
  assert(overlapSize > 0 && "Empty overlap.");
  Addr overlapLHS = this->vaddr + elementOffset;

  LLC_SLICE_DPRINTF(
      sliceId, "Received element %lu size %d Overlap [%lu, %lu).\n", elementIdx,
      elementSize, elementOffset, elementOffset + overlapSize);

  auto rubySystem = this->controller->params()->ruby_system;
  if (rubySystem->getAccessBackingStore()) {
    // Get the data from backing store.
    Addr paddr;
    assert(cpuDelegator->translateVAddrOracle(overlapLHS, paddr) &&
           "Failed to translate address for accessing backing storage.");
    RequestPtr req = std::make_shared<Request>(paddr, overlapSize,
                                               0 /* Flags */, 0 /* MasterId */);
    PacketPtr pkt = Packet::createRead(req);
    pkt->dataStatic(this->getUInt8Ptr(elementOffset));
    rubySystem->getPhysMem()->functionalAccess(pkt);
    delete pkt;
  } else {
    // Get the data from the cache line.
    auto data = dataBlock.getData(overlapLHS % RubySystem::getBlockSizeBytes(),
                                  overlapSize);
    memcpy(this->getUInt8Ptr(elementOffset), data, overlapSize);
  }
  // Mark these bytes ready.
  this->readyBytes += overlapSize;
  if (this->readyBytes > this->size) {
    LLC_SLICE_PANIC(
        sliceId,
        "Too many ready bytes %lu Overlap [%lu, %lu), ready %d > size %d.",
        elementIdx, elementOffset, elementOffset + overlapSize,
        this->readyBytes, this->size);
  }
}