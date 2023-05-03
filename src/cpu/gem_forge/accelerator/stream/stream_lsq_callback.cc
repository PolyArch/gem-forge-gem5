
#include "stream_lsq_callback.hh"

#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"

#include "debug/StreamBase.hh"
#define DEBUG_TYPE StreamBase
#include "stream_log.hh"

namespace gem5 {

bool StreamLQCallback::getAddrSize(Addr &addr, uint32_t &size) const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  // Check if the address is ready.
  if (!this->element->isAddrReady()) {
    return false;
  }
  addr = this->element->addr;
  size = this->element->size;
  return true;
}

bool StreamLQCallback::hasNonCoreDependent() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  return this->element->stream->hasNonCoreDependent();
}

bool StreamLQCallback::isIssued() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  return this->element->isReqIssued();
}

bool StreamLQCallback::isValueReady() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");

  /**
   * We can directly check for element->isValueReady, but instead we
   * call areUsedStreamReady() so that StreamEngine can mark the
   * firstValueCheckCycle for the element, hence it can throttle the stream.
   */
  return this->element->se->areUsedStreamsReady(this->args);
}

void StreamLQCallback::RAWMisspeculate() {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  /**
   * Disable this for now.
   */
  // cpu->getIEWStage().misspeculateInst(userInst);
  this->element->se->RAWMisspeculate(this->element);
}

bool StreamLQCallback::bypassAliasCheck() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  // Only bypass alias check if the stream is marked FloatManual.
  return this->element->stream->getFloatManual();
}

StreamSQCallback::StreamSQCallback(StreamElement *_element,
                                   uint64_t _userSeqNum, Addr _userPC,
                                   const std::vector<uint64_t> &_usedStreamIds)
    : element(_element), FIFOIdx(_element->FIFOIdx),
      usedStreamIds(_usedStreamIds),
      args(_userSeqNum, _userPC, usedStreamIds, true /* isStore */) {
  /**
   * If the StoreStream is floated, it is possible that there are
   * still some SQCallbacks for the first few elements.
   */
  if (this->element->isElemFloatedToCache()) {
    S_ELEMENT_PANIC(this->element,
                    "StoreStream floated with outstanding SQCallback.");
  }
}

bool StreamSQCallback::getAddrSize(Addr &addr, uint32_t &size) const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  // Check if the address is ready.
  if (!this->element->isAddrReady()) {
    return false;
  }
  addr = this->element->addr;
  size = this->element->size;
  return true;
}

bool StreamSQCallback::hasNonCoreDependent() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  return this->element->stream->hasNonCoreDependent();
}

bool StreamSQCallback::isIssued() const {
  /**
   * Store Request is issued by core, not stream engine.
   */
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  return false;
}

bool StreamSQCallback::isValueReady() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");

  /**
   * We can directly check for element->isValueReady, but instead we
   * call areUsedStreamReady() so that StreamEngine can mark the
   * firstValueCheckCycle for the element, hence it can throttle the stream.
   */
  return this->element->se->areUsedStreamsReady(this->args);
}

const uint8_t *StreamSQCallback::getValue() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  assert(this->isValueReady() && "GetValue before it's ready.");
  assert(this->usedStreamIds.size() == 1 && "GetValue for multiple streams.");
  S_ELEMENT_DPRINTF(this->element,
                    "SQCallback get value, AddrReady %d ValueReady %d.\n",
                    this->element->isAddrReady(), this->element->isValueReady);
  /**
   * Handle the special case for UpdateStream, which takes the UpdateValue.
   */
  auto usedStreamId = this->usedStreamIds.front();
  if (this->element->stream->isUpdateStream()) {
    auto value = this->element->getUpdateValuePtrByStreamId(usedStreamId);
    return value;
  } else {
    return this->element->getValuePtrByStreamId(usedStreamId);
  }
}

void StreamSQCallback::RAWMisspeculate() {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  /**
   * SQCallback never triggers RAW misspeculation.
   */
  return;
}

bool StreamSQCallback::bypassAliasCheck() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  // Only bypass alias check if the stream is marked FloatManual.
  return this->element->stream->getFloatManual();
}

bool StreamSQDeprecatedCallback::getAddrSize(Addr &addr, uint32_t &size) {
  // Check if the address is ready.
  if (!this->element->isAddrReady()) {
    return false;
  }
  addr = this->element->addr;
  size = this->element->size;
  return true;
}

void StreamSQDeprecatedCallback::writeback() {
  // Start inform the stream engine to write back.
  this->element->se->writebackElement(this->element, this->storeInst);
}

bool StreamSQDeprecatedCallback::isWritebacked() {
  assert(this->element->inflyWritebackMemAccess.count(this->storeInst) != 0 &&
         "Missing writeback StreamMemAccess?");
  // Check if all the writeback accesses are done.
  return this->element->inflyWritebackMemAccess.at(this->storeInst).empty();
}

void StreamSQDeprecatedCallback::writebacked() {
  // Remember to clear the inflyWritebackStreamAccess.
  assert(this->element->inflyWritebackMemAccess.count(this->storeInst) != 0 &&
         "Missing writeback StreamMemAccess?");
  this->element->inflyWritebackMemAccess.erase(this->storeInst);
  // Remember to change the status of the stream store to committed.
  auto cpu = this->element->se->cpu;
  auto storeInstId = this->storeInst->getId();
  assert(cpu->getInflyInstStatus(storeInstId) ==
             LLVMTraceCPU::InstStatus::COMMITTING &&
         "Writebacked instructions should be committing.");
  cpu->updateInflyInstStatus(storeInstId, LLVMTraceCPU::InstStatus::COMMITTED);
}
} // namespace gem5
