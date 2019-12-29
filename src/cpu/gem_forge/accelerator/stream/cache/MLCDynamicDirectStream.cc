#include "MLCDynamicDirectStream.hh"

// Generated by slicc.
#include "mem/ruby/protocol/CoherenceMsg.hh"
#include "mem/ruby/protocol/RequestMsg.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStream.hh"

#define DEBUG_TYPE MLCRubyStream
#include "../stream_log.hh"

MLCDynamicDirectStream::MLCDynamicDirectStream(
    CacheStreamConfigureData *_configData,
    AbstractStreamAwareController *_controller,
    MessageBuffer *_responseMsgBuffer, MessageBuffer *_requestToLLCMsgBuffer)
    : MLCDynamicStream(_configData, _controller, _responseMsgBuffer,
                       _requestToLLCMsgBuffer),
      slicedStream(_configData, true /* coalesceContinuousElements */),
      llcTailSliceIdx(0) {

  // Initialize the llc bank.
  assert(_configData->initPAddrValid && "InitPAddr should be valid.");
  this->tailPAddr = _configData->initPAddr;
  this->tailSliceLLCBank = this->mapPAddrToLLCBank(_configData->initPAddr);

  // Initialize the buffer for some slices.
  // Since the LLC is bounded by the credit, it's sufficient to only check
  // hasOverflowed() at MLC level.
  while (this->tailSliceIdx < this->maxNumSlices &&
         !this->slicedStream.hasOverflowed()) {
    this->allocateSlice();
  }

  this->llcTailSliceIdx = this->tailSliceIdx;
  this->llcTailPAddr = this->tailPAddr;
  this->llcTailSliceLLCBank = this->tailSliceLLCBank;

  // Set the CacheStreamConfigureData to inform the LLC stream engine
  // initial credit.
  _configData->initAllocatedIdx = this->llcTailSliceIdx;
}

void MLCDynamicDirectStream::advanceStream() {

  this->popStream();
  // Of course we need to allocate more slices.
  while (this->tailSliceIdx - this->headSliceIdx < this->maxNumSlices &&
         !this->hasOverflowed()) {
    this->allocateSlice();
  }

  // We may need to schedule advance stream if the first slice is FAULTED,
  // as no other event will cause it to be released.
  if (!this->slices.empty() &&
      this->slices.front().coreStatus == MLCStreamSlice::CoreStatusE::FAULTED) {
    if (!this->advanceStreamEvent.scheduled()) {
      this->stream->getCPUDelegator()->schedule(&this->advanceStreamEvent,
                                                Cycles(1));
    }
  }

  /**
   * There are two cases we need to send the token:
   * 1. We have allocated more half the buffer size.
   * 2. The stream has overflowed.
   */
  if (!this->slicedStream.hasOverflowed()) {
    if (this->tailSliceIdx - this->llcTailSliceIdx > this->maxNumSlices / 2) {
      this->sendCreditToLLC();
    }
  } else {
    if (this->tailSliceIdx > this->llcTailSliceIdx) {
      this->sendCreditToLLC();
    }
  }
}

void MLCDynamicDirectStream::allocateSlice() {
  auto sliceId = this->slicedStream.getNextSlice();
  MLC_SLICE_DPRINTF(sliceId, "Allocated %#x.\n", sliceId.vaddr);

  this->slices.emplace_back(sliceId);
  this->stream->statistic.numMLCAllocatedSlice++;

  // Try to handle faulted slice.
  Addr paddr;
  auto cpuDelegator = this->getStaticStream()->getCPUDelegator();
  if (cpuDelegator->translateVAddrOracle(sliceId.vaddr, paddr)) {
    // This is address is valid.
  } else {
    // This address is invalid. Mark the slice faulted.
    this->slices.back().coreStatus = MLCStreamSlice::CoreStatusE::FAULTED;
  }

  // Try to find where the LLC stream would be at this point.
  this->tailSliceIdx++;
  if (cpuDelegator->translateVAddrOracle(
          this->slicedStream.peekNextSlice().vaddr, paddr)) {
    // The next slice would be valid.
    this->tailPAddr = paddr;
    this->tailSliceLLCBank = this->mapPAddrToLLCBank(paddr);
  } else {
    // This address is invalid.
    // Do not update tailSliceLLCBank as the LLC stream would not move.
  }
}

void MLCDynamicDirectStream::sendCreditToLLC() {
  /**
   * The LLC stream will be at llcTailSliceLLCBank, and we need to
   * update its credit and the new location is tailSliceLLCBank.
   *
   * This will not work for pointer chasing stream.
   */
  assert(this->tailSliceIdx > this->llcTailSliceIdx &&
         "Don't know where to send credit.");

  // Send the flow control.
  MLC_S_DPRINTF("Extended %lu -> %lu, sent credit to LLC%d.\n",
                this->llcTailSliceIdx, this->tailSliceIdx,
                this->llcTailSliceLLCBank.num);
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = this->llcTailPAddr;
  msg->m_Type = CoherenceRequestType_STREAM_FLOW;
  msg->m_Requestor = this->controller->getMachineID();
  msg->m_Destination.add(this->llcTailSliceLLCBank);
  msg->m_MessageSize = MessageSizeType_Control;
  msg->m_sliceId.streamId = this->dynamicStreamId;
  msg->m_sliceId.lhsElementIdx = this->llcTailSliceIdx;
  msg->m_sliceId.rhsElementIdx = this->tailSliceIdx;

  Cycles latency(1); // Just use 1 cycle latency here.

  this->requestToLLCMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(latency));

  // Update the record.
  this->llcTailSliceIdx = this->tailSliceIdx;
  this->llcTailPAddr = this->tailPAddr;
  this->llcTailSliceLLCBank = this->tailSliceLLCBank;
}