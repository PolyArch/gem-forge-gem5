#include "LLCDynamicStream.hh"

#include "cpu/gem_forge/accelerator/stream/coalesced_stream.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "debug/LLCRubyStream.hh"
#define DEBUG_TYPE LLCRubyStream
#include "../stream_log.hh"

std::unordered_map<DynamicStreamId, LLCDynamicStream *, DynamicStreamIdHasher>
    LLCDynamicStream::GlobalLLCDynamicStreamMap;

uint64_t LLCStreamElement::getData(uint64_t streamId) const {
  assert(this->isReady());
  auto S = this->dynS->getStaticStream();
  int32_t offset = 0;
  int size = this->size;
  if (auto CS = dynamic_cast<CoalescedStream *>(S)) {
    // Handle offset for coalesced stream.
    CS->getCoalescedOffsetAndSize(streamId, offset, size);
  }
  assert(size <= sizeof(uint64_t) && "ElementSize overflow.");
  assert(offset + size <= this->size && "Size overflow.");
  switch (size) {
  case 8:
    return *reinterpret_cast<const uint64_t *>(this->data.data() + offset);
  case 4:
    return *reinterpret_cast<const uint32_t *>(this->data.data() + offset);
  case 2:
    return *reinterpret_cast<const uint16_t *>(this->data.data() + offset);
  case 1:
    return *reinterpret_cast<const uint8_t *>(this->data.data() + offset);
  default: { panic("Unsupported element size %d.\n", size); }
  }
}

// TODO: Support real flow control.
LLCDynamicStream::LLCDynamicStream(AbstractStreamAwareController *_controller,
                                   CacheStreamConfigureData *_configData)
    : configData(*_configData),
      slicedStream(_configData, true /* coalesceContinuousElements */),
      maxWaitingDataBaseRequests(8), controller(_controller),
      sliceIdx(0), allocatedSliceIdx(_configData->initAllocatedIdx),
      waitingDataBaseRequests(0) {
  if (this->configData.isPointerChase) {
    // Pointer chase stream can only have at most one base requests waiting for
    // data.
    this->maxWaitingDataBaseRequests = 1;
  }
  if (this->getStaticStream()->isReduction()) {
    // Copy the initial reduction value.
    this->reductionValue = this->configData.reductionInitValue;
  }
  assert(GlobalLLCDynamicStreamMap.emplace(this->getDynamicStreamId(), this)
             .second);
}

LLCDynamicStream::~LLCDynamicStream() {
  for (auto &indirectStream : this->indirectStreams) {
    delete indirectStream;
    indirectStream = nullptr;
  }
  this->indirectStreams.clear();
  assert(GlobalLLCDynamicStreamMap.erase(this->getDynamicStreamId()) == 1);
}

bool LLCDynamicStream::hasTotalTripCount() const {
  if (this->baseStream) {
    return this->baseStream->hasTotalTripCount();
  }
  return this->configData.totalTripCount != -1;
}

uint64_t LLCDynamicStream::getTotalTripCount() const {
  if (this->baseStream) {
    return this->baseStream->getTotalTripCount();
  }
  return this->configData.totalTripCount;
}

Addr LLCDynamicStream::peekVAddr() {
  return this->slicedStream.peekNextSlice().vaddr;
}

Addr LLCDynamicStream::getVAddr(uint64_t sliceIdx) const {
  panic("getVAddr is deprecated.\n");
  return 0;
}

bool LLCDynamicStream::translateToPAddr(Addr vaddr, Addr &paddr) const {
  // ! Do something reasonable here to translate the vaddr.
  auto cpuDelegator = this->configData.stream->getCPUDelegator();
  return cpuDelegator->translateVAddrOracle(vaddr, paddr);
}

void LLCDynamicStream::addCredit(uint64_t n) {
  this->allocatedSliceIdx += n;
  for (auto indirectStream : this->indirectStreams) {
    indirectStream->addCredit(n);
  }
}

void LLCDynamicStream::updateIssueClearCycle() {
  if (!this->shouldUpdateIssueClearCycle()) {
    return;
  }
  const auto *dynS =
      this->configData.stream->getDynamicStream(this->configData.dynamicId);
  if (dynS == nullptr) {
    // The dynS is already released.
    return;
  }
  auto avgTurnAroundCycle = dynS->getAvgTurnAroundCycle();
  auto avgLateElements = dynS->getNumLateElement();
  uint64_t newIssueClearCycle = this->issueClearCycle;
  if (avgTurnAroundCycle != 0) {
    // We need to adjust the turn around cycle from element to slice.
    uint64_t avgSliceTurnAroundCycle = static_cast<float>(avgTurnAroundCycle) *
                                       this->slicedStream.getElementPerSlice();
    // We divide by 1.5 so to reflect that we should be slighly faster than
    // core.
    uint64_t adjustSliceTurnAroundCycle = avgSliceTurnAroundCycle * 2 / 3;
    if (avgLateElements >= 2) {
      // If we have late elements, try to issue faster.
      newIssueClearCycle = std::max(1ul, this->issueClearCycle / 2);
    } else {
      newIssueClearCycle = adjustSliceTurnAroundCycle;
    }
    if (newIssueClearCycle != this->issueClearCycle) {
      /**
       * Some the stats from the core may be disruptly. we have some simple
       * threshold here.
       * TODO: Improve this.
       */
      const uint64_t IssueClearThreshold = 1024;
      LLC_S_DPRINTF(this->configData.dynamicId,
                    "Update IssueClearCycle %lu -> %lu (%lu), avgEleTurn %lu, "
                    "avgSliceTurn %lu, avgLateEle %d, elementPerSlice %f.\n",
                    this->issueClearCycle, newIssueClearCycle,
                    IssueClearThreshold, avgTurnAroundCycle,
                    avgSliceTurnAroundCycle, avgLateElements,
                    this->slicedStream.getElementPerSlice());
      this->issueClearCycle =
          Cycles(newIssueClearCycle > IssueClearThreshold ? IssueClearThreshold
                                                          : newIssueClearCycle);
    }
  }
}

bool LLCDynamicStream::shouldUpdateIssueClearCycle() {
  if (!this->shouldUpdateIssueClearCycleInitialized) {
    // We do not constrain ourselves from the core if there are no core users
    // for both myself and all the indirect streams.
    this->shouldUpdateIssueClearCycleMemorized = true;
    if (!this->getStaticStream()->hasCoreUser()) {
      bool hasCoreUser = false;
      for (auto dynIS : this->indirectStreams) {
        auto IS = dynIS->getStaticStream();
        if (IS->hasCoreUser()) {
          hasCoreUser = true;
          break;
        }
      }
      if (!hasCoreUser) {
        // No core user. Turn off the IssueClearCycle.
        this->shouldUpdateIssueClearCycleMemorized = false;
      }
    }
  }

  this->shouldUpdateIssueClearCycleInitialized = true;
  return this->shouldUpdateIssueClearCycleMemorized;
}

void LLCDynamicStream::traceEvent(
    const ::LLVM::TDG::StreamFloatEvent::StreamFloatEventType &type) {
  auto &floatTracer = this->getStaticStream()->floatTracer;
  auto curCycle = this->controller->curCycle();
  auto llcBank = this->controller->getMachineID().num;
  floatTracer.traceEvent(curCycle, llcBank, type);
  // Do this for all indirect streams.
  for (auto IS : this->indirectStreams) {
    IS->traceEvent(type);
  }
}