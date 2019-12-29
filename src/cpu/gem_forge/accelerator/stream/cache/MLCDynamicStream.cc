#include "MLCDynamicStream.hh"

// Generated by slicc.
#include "mem/ruby/protocol/CoherenceMsg.hh"
#include "mem/ruby/protocol/RequestMsg.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStream.hh"

#define DEBUG_TYPE MLCRubyStream
#include "../stream_log.hh"

MLCDynamicStream::MLCDynamicStream(CacheStreamConfigureData *_configData,
                                   AbstractStreamAwareController *_controller,
                                   MessageBuffer *_responseMsgBuffer,
                                   MessageBuffer *_requestToLLCMsgBuffer)
    : stream(_configData->stream), dynamicStreamId(_configData->dynamicId),
      isPointerChase(_configData->isPointerChase), controller(_controller),
      responseMsgBuffer(_responseMsgBuffer),
      requestToLLCMsgBuffer(_requestToLLCMsgBuffer),
      maxNumSlices(_controller->getMLCStreamBufferInitNumEntries()),
      headSliceIdx(0), tailSliceIdx(0),
      advanceStreamEvent([this]() -> void { this->advanceStream(); },
                         "MLC::advanceStream",
                         false /*delete after process. */) {

  /**
   * ! You should never call any virtual function in the
   * ! constructor/deconstructor.
   */

  // Schedule the first advanceStreamEvent.
  this->stream->getCPUDelegator()->schedule(&this->advanceStreamEvent,
                                            Cycles(1));
}

MLCDynamicStream::~MLCDynamicStream() {
  // We got to deschedule the advanceStreamEvent.
  if (this->advanceStreamEvent.scheduled()) {
    this->stream->getCPUDelegator()->deschedule(&this->advanceStreamEvent);
  }
}

void MLCDynamicStream::receiveStreamData(const ResponseMsg &msg) {
  const auto &sliceId = msg.m_sliceId;
  assert(sliceId.isValid() && "Invalid stream slice id for stream data.");

  auto numElements = sliceId.getNumElements();
  assert(this->dynamicStreamId == sliceId.streamId &&
         "Unmatched dynamic stream id.");
  MLC_SLICE_DPRINTF(sliceId, "Receive data %#x.\n", sliceId.vaddr);

  /**
   * It is possible when the core stream engine runs ahead than
   * the LLC stream engine, and the stream data is delivered after
   * the slice is released. In such case we will ignore the
   * stream data.
   *
   * TODO: Properly handle this with sliceIdx.
   */
  if (this->slices.empty()) {
    assert(this->hasOverflowed() && "No slices when not overflowed yet.");
    // Simply ignore it.
    return;
  } else {
    // TODO: Properly detect that the slice is lagging behind.
    if (sliceId.vaddr < this->slices.front().sliceId.vaddr) {
      // The stream data is lagging behind. The slice is already
      // released.
      return;
    }
  }

  /**
   * Find the correct stream slice and insert the data there.
   * Here we reversely search for it to save time.
   */
  for (auto slice = this->slices.rbegin(), end = this->slices.rend();
       slice != end; ++slice) {
    if (this->matchSliceId(slice->sliceId, sliceId)) {
      // Found the slice.
      if (slice->sliceId.getNumElements() != numElements) {
        MLC_S_PANIC("Mismatch numElements, incoming %d, slice %d.\n",
                    numElements, slice->sliceId.getNumElements());
      }
      slice->setData(msg.m_DataBlk);
      if (slice->coreStatus == MLCStreamSlice::CoreStatusE::WAIT) {
        this->makeResponse(*slice);
      }
      this->advanceStream();
      return;
    }
  }

  MLC_SLICE_PANIC(sliceId, "Fail to find the slice. Tail %lu.\n",
                  this->tailSliceIdx);
}

void MLCDynamicStream::receiveStreamRequest(
    const DynamicStreamSliceId &sliceId) {
  MLC_SLICE_DPRINTF(sliceId, "Receive request to %#x. Tail %lu.\n",
                    sliceId.vaddr, this->tailSliceIdx);

  /**
   * Let's not make assumption that the request will come in order.
   */
  if (this->slices.empty()) {
    MLC_S_PANIC("No slices for request, overflowed %d, totalTripCount %lu.\n",
                this->hasOverflowed(), this->getTotalTripCount());
  }
  bool found = false;
  for (auto &slice : this->slices) {
    /**
     * So far we match them on vaddr.
     * TODO: Really assign the sliceIdx and match that.
     */
    if (this->matchSliceId(slice.sliceId, sliceId)) {
      // Found the slice.
      assert(slice.coreStatus == MLCStreamSlice::CoreStatusE::NONE &&
             "Already seen a request.");
      found = true;
      MLC_SLICE_DPRINTF(slice.sliceId, "Matched to request.\n");
      slice.coreStatus = MLCStreamSlice::CoreStatusE::WAIT;
      slice.coreSliceId = sliceId;
      if (slice.dataReady) {
        // Sanity check the address.
        // ! Core is line address.
        if (slice.coreSliceId.vaddr != makeLineAddress(slice.sliceId.vaddr)) {
          MLC_SLICE_PANIC(sliceId, "Mismatch between Core %#x and LLC %#x.\n",
                          slice.coreSliceId.vaddr, slice.sliceId.vaddr);
        }
        this->makeResponse(slice);
      }
      break;
    }
  }

  if (!found) {
    MLC_S_PANIC("Failed to find slice %s.\n", sliceId);
  }
  this->advanceStream();
}

void MLCDynamicStream::endStream() {
  for (auto &slice : this->slices) {
    if (slice.coreStatus == MLCStreamSlice::CoreStatusE::WAIT) {
      // Make a dummy response.
      // Ignore whether the data is ready.
      // ! For indirect stream, the sliceId may not have vaddr.
      // ! In such case, we set it from core's sliceId.
      // TODO: Fix this in a more rigorous way.
      if (slice.sliceId.vaddr == 0) {
        slice.sliceId.vaddr = slice.coreSliceId.vaddr;
      }
      this->makeResponse(slice);
    }
  }
}

void MLCDynamicStream::receiveStreamRequestHit(
    const DynamicStreamSliceId &sliceId) {
  MLC_SLICE_DPRINTF(sliceId, "Receive request hit.\n");

  /**
   * Let's not make assumption that the request will come in order.
   */
  assert(!this->slices.empty() && "Empty slice list.");
  for (auto &slice : this->slices) {
    if (this->matchSliceId(slice.sliceId, sliceId)) {
      // Found the slice.
      assert(slice.coreStatus == MLCStreamSlice::CoreStatusE::NONE &&
             "Already seen a request.");
      slice.coreStatus = MLCStreamSlice::CoreStatusE::DONE;
      slice.coreSliceId = sliceId;
      break;
    }
  }
  this->advanceStream();
}

void MLCDynamicStream::popStream() {
  /**
   * Maybe let's make release in order.
   * The slice is released once the core status is DONE or FAULTED.
   */
  while (!this->slices.empty()) {
    const auto &slice = this->slices.front();
    if (slice.coreStatus == MLCStreamSlice::CoreStatusE::DONE ||
        slice.coreStatus == MLCStreamSlice::CoreStatusE::FAULTED) {
      MLC_SLICE_DPRINTF(slice.sliceId, "Pop.\n");
      this->headSliceIdx++;
      this->slices.pop_front();
    } else {
      // We made no progress.
      break;
    }
  }
}

void MLCDynamicStream::makeResponse(MLCStreamSlice &slice) {
  assert(slice.coreStatus == MLCStreamSlice::CoreStatusE::WAIT &&
         "Element core status should be WAIT to make response.");
  Addr paddr = this->translateVAddr(slice.sliceId.vaddr);
  auto paddrLine = makeLineAddress(paddr);

  auto selfMachineId = this->controller->getMachineID();
  auto upperMachineId = MachineID(
      static_cast<MachineType>(selfMachineId.type - 1), selfMachineId.num);
  auto msg = std::make_shared<CoherenceMsg>(this->controller->clockEdge());
  msg->m_addr = paddrLine;
  msg->m_Class = CoherenceClass_DATA_EXCLUSIVE;
  msg->m_Sender = selfMachineId;
  msg->m_Dest = upperMachineId;
  msg->m_MessageSize = MessageSizeType_Response_Data;

  MLC_SLICE_DPRINTF(slice.sliceId, "Make response.\n");
  // The latency should be consistency with the cache controller.
  // However, I still failed to find a clean way to exponse this info
  // to the stream engine. So far I manually set it to the default
  // value from the L1 cache controller.
  // TODO: Make it consistent with the cache controller.
  Cycles latency(2);
  this->responseMsgBuffer->enqueue(msg, this->controller->clockEdge(),
                                   this->controller->cyclesToTicks(latency));
  // Set the core status to DONE.
  slice.coreStatus = MLCStreamSlice::CoreStatusE::DONE;
}

MLCDynamicStream::MLCStreamSlice &
MLCDynamicStream::getSlice(uint64_t sliceIdx) {
  assert(sliceIdx >= this->headSliceIdx && "Underflow of sliceIdx.");
  assert(sliceIdx < this->tailSliceIdx && "Overflow of sliceIdx.");
  return this->slices.at(sliceIdx - this->headSliceIdx);
}

const MLCDynamicStream::MLCStreamSlice &
MLCDynamicStream::getSlice(uint64_t sliceIdx) const {
  assert(sliceIdx >= this->headSliceIdx && "Underflow of sliceIdx.");
  assert(sliceIdx < this->tailSliceIdx && "Overflow of sliceIdx.");
  return this->slices.at(sliceIdx - this->headSliceIdx);
}

Addr MLCDynamicStream::translateVAddr(Addr vaddr) const {
  auto cpuDelegator = this->getStaticStream()->getCPUDelegator();
  Addr paddr;
  if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
    panic("Failed translate vaddr %#x.\n", vaddr);
  }
  return paddr;
}

MachineID MLCDynamicStream::mapPAddrToLLCBank(Addr paddr) const {
  auto selfMachineId = this->controller->getMachineID();
  auto llcMachineId = this->controller->mapAddressToLLC(
      paddr, static_cast<MachineType>(selfMachineId.type + 1));
  return llcMachineId;
}

void MLCDynamicStream::panicDump() const {
  MLC_S_DPRINTF("-------------------Panic Dump--------------------\n");
  for (const auto &slice : this->slices) {
    MLC_SLICE_DPRINTF(
        slice.sliceId, "Data %d Core %s.\n", slice.dataReady,
        MLCStreamSlice::convertCoreStatusToString(slice.coreStatus).c_str());
  }
}