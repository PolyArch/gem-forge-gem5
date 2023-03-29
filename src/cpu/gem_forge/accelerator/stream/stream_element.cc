#include "stream_element.hh"
#include "stream.hh"
#include "stream_compute_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "debug/StreamElement.hh"

#define DEBUG_TYPE StreamElement
#include "stream_log.hh"

namespace gem5 {

std::string CacheBlockBreakdownAccess::stateToString(StateE state) {
  switch (state) {
  default:
    panic("Unknown CacheBlockBreakdonwAccess state %d.", state);
#define Case(x)                                                                \
  case x:                                                                      \
    return #x
    Case(None);
    Case(Initialized);
    Case(Faulted);
    Case(Issued);
    Case(PrevElement);
    Case(Ready);
#undef Case
  }
}
std::ostream &operator<<(std::ostream &os,
                         const CacheBlockBreakdownAccess::StateE &state) {
  return os << CacheBlockBreakdownAccess::stateToString(state);
}

StreamMemAccess::StreamMemAccess(Stream *_stream, StreamElement *_element,
                                 Addr _cacheBlockVAddr, Addr _vaddr, int _size,
                                 int _additionalDelay)
    : stream(_stream), element(_element), isReissue(_element->flushed),
      FIFOIdx(_element->FIFOIdx), cacheBlockVAddr(_cacheBlockVAddr),
      vaddr(_vaddr), size(_size), additionalDelay(_additionalDelay) {
  // Initialize it fairly simply.
  this->sliceId.getDynStrandId() = DynStrandId(this->FIFOIdx.streamId);
  this->sliceId.getStartIdx() = this->FIFOIdx.entryIdx;
  this->sliceId.getEndIdx() = this->FIFOIdx.entryIdx + 1;
  // ! So far always do cache line level.
  this->sliceId.vaddr = this->cacheBlockVAddr;
  this->sliceId.size = this->size;
}

void StreamMemAccess::registerReceiver(StreamElement *element) {
  // Sanity check that there are no duplicate receivers.
  for (int i = 0; i < this->numReceivers; ++i) {
    auto &receiver = this->receivers.at(i);
    if (receiver.first == element) {
      // It is possible that one element get reallocated.
      if (receiver.second) {
        S_ELEMENT_HACK(this->element, "Register receiver, my FIFOIdx is %s.\n",
                       this->FIFOIdx);
        S_ELEMENT_PANIC(element,
                        "Register duplicate receiver, still valid %d.\n",
                        this->receivers.at(i).second);
      } else {
        receiver.second = true;
        return;
      }
    }
  }
  if (this->numReceivers == StreamMemAccess::MAX_NUM_RECEIVERS) {
    for (int i = 0; i < this->numReceivers; ++i) {
      auto &receiver = this->receivers.at(i);
      if (receiver.second) {
        S_ELEMENT_HACK(receiver.first, "A valid receiver of [%#x, +%d).\n",
                       receiver.first->addr, receiver.first->size);
      } else {
        hack("In invalid receiver.\n");
      }
    }
    S_FIFO_ENTRY_PANIC(this->FIFOIdx, "Too many receivers.\n");
  }
  auto &newReceiver = this->receivers.at(this->numReceivers);
  newReceiver.first = element;
  newReceiver.second = true;
  this->numReceivers++;
}

void StreamMemAccess::deregisterReceiver(StreamElement *element) {
  for (int i = 0; i < this->numReceivers; ++i) {
    auto &receiver = this->receivers.at(i);
    if (receiver.first == element) {
      assert(receiver.second && "Receiver has already been deregistered.");
      receiver.second = false;
      return;
    }
  }
  assert(false && "Failed to find receiver.");
}

void StreamMemAccess::handlePacketResponse(PacketPtr pkt) {
  // API for stream-aware cache, as it doesn't have the cpu.
  this->handlePacketResponse(this->getStream()->getCPUDelegator(), pkt);
}

void StreamMemAccess::handlePacketResponse(GemForgeCPUDelegator *cpuDelegator,
                                           PacketPtr pkt) {
  if (this->additionalDelay != 0) {
    // We have to reschedule the event to pay for the additional delay.
    S_ELEMENT_DPRINTF(this->element,
                      "PacketResponse with additional delay of %d cycles.\n",
                      this->additionalDelay);
    auto responseEvent = new ResponseEvent(cpuDelegator, this, pkt);
    cpuDelegator->schedule(responseEvent, Cycles(this->additionalDelay));
    // Remember to reset the additional delay as we have already paid for it.
    this->additionalDelay = 0;
    return;
  }

  // Handle the request statistic.
  if (pkt->req->hasStatistic()) {
    bool hitInPrivateCache = false;
    auto statistic = pkt->req->getStatistic();
    switch (statistic->hitCacheLevel) {
    case RequestStatistic::HitPlaceE::INVALID: {
      // Invalid.
      break;
    }
    case RequestStatistic::HitPlaceE::MEM: // Hit in mem.
      this->stream->statistic.numMissL2++;
      this->stream->statistic.numMissL1++;
      this->stream->statistic.numMissL0++;
      hitInPrivateCache = false;
      break;
    case RequestStatistic::HitPlaceE::L1_STREAM_BUFFER:
      // This is considered hit in L2.
      this->stream->statistic.numMissL1++;
      this->stream->statistic.numMissL0++;
      hitInPrivateCache = false;
      break;
    case RequestStatistic::HitPlaceE::L2_CACHE:
      this->stream->statistic.numMissL1++;
      this->stream->statistic.numMissL0++;
      hitInPrivateCache = false;
      break;
    case RequestStatistic::HitPlaceE::L1_CACHE:
      this->stream->statistic.numMissL0++;
      hitInPrivateCache = true;
      break;
    case RequestStatistic::HitPlaceE::L0_CACHE: { // Hit in first level cache.
      hitInPrivateCache = true;
      break;
    }
    default: {
      panic("Invalid hitCacheLevel %d.\n", statistic->hitCacheLevel);
    }
    }
    // We just use the last dynamic stream.
    // Not 100% accurate but should be fine.
    if (this->stream->hasDynStream()) {
      auto &dynS = this->stream->getLastDynStream();
      dynS.recordHitHistory(hitInPrivateCache);
    }
  }

  // Check if this is a read request.
  if (pkt->isRead()) {
    // We should notify the stream engine that this cache line is coming back.
    this->stream->se->fetchedCacheBlock(this->cacheBlockVAddr, this);
  }

  // Notify all receivers.
  for (int i = 0; i < this->numReceivers; ++i) {
    auto &receiver = this->receivers.at(i);
    if (receiver.second) {
      // The receiver is still expecting the response.
      receiver.first->handlePacketResponse(this, pkt);
      receiver.second = false;
    }
  }

  // Decrement myself as infly.
  this->stream->decrementInflyStreamRequest();
  this->stream->se->decrementInflyStreamRequest();

  // Release myself.
  delete this;
  delete pkt;

  return;
}

void StreamMemAccess::issueToMemoryCallback(
    GemForgeCPUDelegator *cpuDelegator) {
  this->element->issueCycle = cpuDelegator->curCycle();
}

StreamElement::StreamElement(StreamEngine *_se) : se(_se) { this->clear(); }

bool StreamElement::isLastElement() const {
  assert(this->dynS && "This element has not been allocated.");
  assert(this->dynS->configExecuted && "The DynS has not be configured.");
  return this->dynS->hasTotalTripCount() &&
         this->FIFOIdx.entryIdx == this->dynS->getTotalTripCount();
}

bool StreamElement::isInnerSecondElem() const {
  assert(this->dynS && "This element has not been allocated.");
  return this->dynS->isInnerSecondElem(this->FIFOIdx.entryIdx);
}

bool StreamElement::isInnerLastElem() const {
  assert(this->dynS && "This element has not been allocated.");
  return this->dynS->isInnerLastElem(this->FIFOIdx.entryIdx);
}

bool StreamElement::isInnerSecondLastElem() const {
  assert(this->dynS && "This element has not been allocated.");
  return this->dynS->isInnerSecondLastElem(this->FIFOIdx.entryIdx);
}

bool StreamElement::shouldIssue() const {
  /**
   * So far there are two cases when we do not issue requests:
   * 1. DynStream says so, then we have two cases:
   *   a. DynStream is not floated, then we just don't issue.
   *   b. DynStream is floated, then we check if the element is floated, as
   *      the first few elements still need to be issued for MidwayFloating.
   * 2. LastElement that only uses to deal with StreamEnd.
   */
  if (!this->dynS->shouldCoreSEIssue()) {
    if (this->dynS->isFloatedToCache()) {
      return !this->isElemFloatedToCache();
    } else {
      return false;
    }
  }
  if (this->isLastElement()) {
    // Last element should never be issued.
    return false;
  }
  return true;
}

bool StreamElement::isFirstFloatElem() const {
  return this->dynS->getAdjustedFirstFloatElemIdx() == this->FIFOIdx.entryIdx;
}
bool StreamElement::isFloatElem() const {
  return this->dynS->getAdjustedFirstFloatElemIdx() <= this->FIFOIdx.entryIdx;
}
bool StreamElement::isElemFloatedToCacheAsRoot() const {
  return this->dynS->isFloatedToCacheAsRoot() && this->isFloatElem();
}
void StreamElement::checkIsElemFloatedToCache() {
  this->floatedToCache = this->dynS->isFloatedToCache() && this->isFloatElem();
}
bool StreamElement::isElemFloatedWithDependent() const {
  return this->dynS->isFloatedWithDependent() && this->isFloatElem();
}
bool StreamElement::isElemFloatedAsNDC() const {
  assert(
      (!this->dynS->isFloatedAsNDC() || this->dynS->getFloatPlan().empty()) &&
      "FloatPlan is not used for NDC.");
  return this->dynS->isFloatedAsNDC();
}
bool StreamElement::isElemFloatedAsNDCForward() const {
  assert((!this->dynS->isFloatedAsNDCForward() ||
          this->dynS->getFloatPlan().empty()) &&
         "FloatPlan is not used for NDCForward.");
  return this->dynS->isFloatedAsNDCForward();
}
bool StreamElement::isElemPseudoFloatedToCache() const {
  return this->dynS->isPseudoFloatedToCache() && this->isFloatElem();
}

bool StreamElement::isLoopElimInCoreStoreCmpElem() const {
  return !this->isElemFloatedToCache() &&
         this->stream->isStoreComputeStream() &&
         this->stream->isLoopEliminated();
}

void StreamElement::clear() {

  this->addrBaseElements.clear();
  this->valueBaseElements.clear();
  this->innerLoopDepElements.clear();
  this->hasUnInitInnerLoopAddrBaseElem = false;
  this->hasUnInitInnerLoopValueBaseElem = false;
  this->next = nullptr;
  this->stream = nullptr;
  this->dynS = nullptr;
  this->FIFOIdx = FIFOEntryIdx();
  this->isCacheBlockedValue = false;
  this->firstUserSeqNum = LLVMDynamicInst::INVALID_SEQ_NUM;
  this->firstStoreSeqNum = LLVMDynamicInst::INVALID_SEQ_NUM;
  this->isStepped = false;
  this->addrReady = false;
  this->reqIssued = false;
  this->prefetchIssued = false;
  this->isAddrAliased = false;
  this->isValueReady = false;
  this->updateValueReady = false;
  this->updateValue.fill(0);
  this->loadComputeValueReady = false;
  this->loadComputeValue.fill(0);
  this->isCacheAcked = false;
  this->flushed = false;

  this->allocateCycle = Cycles(0);
  this->valueReadyCycle = Cycles(0);
  this->firstValueCheckCycle = Cycles(0);
  this->firstValueCheckByCoreCycle = Cycles(0);

  this->addr = 0;
  this->size = 0;
  this->clearCacheBlocks();
  std::fill(this->value.begin(), this->value.end(), 0);

  this->stored = false;
  this->clearScheduledComputation();

  this->floatedToCache = false;
  this->remoteBank = InvalidRemoteBank;
}

void StreamElement::flush(bool aliased) {

  if (!this->stream->trackedByPEB()) {
    S_ELEMENT_PANIC(this, "Flushed Non-PEB element.");
  }

  // Clear the element to just allocate state.
  this->addrReady = false;
  this->reqIssued = false;
  this->prefetchIssued = false;
  this->isValueReady = false;
  this->updateValueReady = false;
  this->updateValue.fill(0);
  this->loadComputeValueReady = false;
  this->loadComputeValue.fill(0);

  // Raise the flush flag.
  this->flushed = true;
  if (aliased) {
    this->isAddrAliased = true;
  }

  this->valueReadyCycle = Cycles(0);
  this->firstValueCheckCycle = Cycles(0);
  this->firstValueCheckByCoreCycle = Cycles(0);

  this->addr = 0;
  this->size = 0;
  this->clearInflyMemAccesses();
  this->clearCacheBlocks();
  this->clearScheduledComputation();
  std::fill(this->value.begin(), this->value.end(), 0);

  this->floatedToCache = false;
}

void StreamElement::clearCacheBlocks() {
  for (int i = 0; i < this->cacheBlocks; ++i) {
    auto &block = this->cacheBlockBreakdownAccesses[i];
    if (block.memAccess) {
      panic("Still has unregistered StreamMemAccess.");
    }
    block.clear();
  }
  this->cacheBlocks = 0;
}

void StreamElement::clearInflyMemAccesses() {
  // Deregister all StreamMemAccesses.
  for (int i = 0; i < this->cacheBlocks; ++i) {
    auto &block = this->cacheBlockBreakdownAccesses[i];
    if (block.memAccess) {
      block.memAccess->deregisterReceiver(this);
      block.memAccess = nullptr;
    }
  }
}

void StreamElement::clearScheduledComputation() {
  if (this->scheduledComputation) {
    this->se->computeEngine->discardComputation(this);
  }
  assert(!this->scheduledComputation && "Still has scheduled computation.");
}

StreamMemAccess *StreamElement::allocateStreamMemAccess(
    const CacheBlockBreakdownAccess &cacheBlockBreakdown) {

  auto memAccess = new StreamMemAccess(
      this->getStream(), this, cacheBlockBreakdown.cacheBlockVAddr,
      cacheBlockBreakdown.vaddr, cacheBlockBreakdown.size);

  return memAccess;
}

void StreamElement::handlePacketResponse(StreamMemAccess *memAccess,
                                         PacketPtr pkt) {
  // Make sure I am still expect this.
  auto vaddr = memAccess->cacheBlockVAddr;
  auto size = pkt->getSize();
  auto blockIdx = this->mapVAddrToBlockOffset(vaddr, size);
  auto &block = this->cacheBlockBreakdownAccesses[blockIdx];
  assert(block.memAccess == memAccess &&
         "We are not expecting from this StreamMemAccess.");

  /**
   * Update the value vector.
   * Notice that pkt->getAddr() will give you physics address.
   * ! So far all requests are in cache line size.
   */
  auto data = pkt->getPtr<uint8_t>();
  auto S = this->stream;
  if (S->isAtomicComputeStream() && pkt->isAtomicOp()) {
    auto atomicOp = pkt->getAtomicOp();
    auto streamAtomicOp = dynamic_cast<StreamAtomicOp *>(atomicOp);
    assert(streamAtomicOp && "Missing StreamAtomicOp.");
    auto loadedValue = streamAtomicOp->getLoadedValue();
    // * We should not use block addr/size for atomic op.
    this->setValue(memAccess->vaddr, S->getCoreElementSize(),
                   loadedValue.uint8Ptr());
  } else if (S->isStoreStream()) {
    // StoreStream does not care about prefetch response.
  } else {
    this->setValue(vaddr, size, data);
  }

  // Clear the receiver.
  block.memAccess = nullptr;

  // Dummy way to check if this is a writeback mem access.
  for (auto &storeInstMemAccesses : this->inflyWritebackMemAccess) {
    storeInstMemAccesses.second.erase(memAccess);
  }
}

bool StreamElement::isFirstUserDispatched() const {
  return this->firstUserSeqNum != LLVMDynamicInst::INVALID_SEQ_NUM;
}

bool StreamElement::isFirstStoreDispatched() const {
  return this->firstStoreSeqNum != LLVMDynamicInst::INVALID_SEQ_NUM;
}

bool StreamElement::checkAddrBaseElementsReady(bool checkByCore) {
  if (this->hasUnInitInnerLoopAddrBaseElem) {
    this->dynS->tryAddInnerLoopBaseElements(this);
  }
  S_ELEMENT_DPRINTF(this, "[AddrBaseReady] Check.\n");
  if (this->hasUnInitInnerLoopAddrBaseElem) {
    S_ELEMENT_DPRINTF(this, "[AddrBaseReady][InnerLoopDep] NotReady: "
                            "HasUnInitInnerLoopBaseElem.\n");
    return false;
  }
  /**
   * If this is the LastElement of stream with TripCount 0, it should never to
   * AddrReady.
   */
  if (this->dynS->hasZeroTripCount() && this->isLastElement()) {
    S_ELEMENT_DPRINTF(this, "[AddrBaseReady] NotReady: ZeroTripCount.\n");
    return false;
  }
  bool ready = true;
  for (const auto &baseElement : this->addrBaseElements) {
    auto baseE = baseElement.getElement();
    if (baseE->stream == nullptr) {
      S_ELEMENT_PANIC(this, "BaseElement has no stream.\n");
    }
    if (this->stream->addrBaseStreams.count(baseE->stream) == 0 &&
        this->stream->backBaseStreams.count(baseE->stream) == 0) {
      // ! For reduction stream, myself is not in baseStreams.
      if (!this->stream->isReduction()) {
        S_ELEMENT_PANIC(this, "Different base streams from %s.\n",
                        baseE->FIFOIdx);
      }
    }
    S_ELEMENT_DPRINTF(this, "BaseE Ready %d %s.\n", baseE->isValueReady,
                      baseE->FIFOIdx);
    if (!baseE->checkValueReady(checkByCore)) {
      ready = false;
      break;
    }
  }
  return ready;
}

Addr StreamElement::computeAddr() {
  /**
   * Compute the address.
   */
  if (!this->stream->isMemStream()) {
    S_ELEMENT_PANIC(this, "ComputeAddr for Non-Mem Stream.");
  }
  GetStreamValueFunc getStreamValue =
      [this](uint64_t baseStreamId) -> StreamValue {
    auto baseStream = this->se->getStream(baseStreamId);
    for (const auto &baseElement : this->addrBaseElements) {
      auto baseE = baseElement.getElement();
      if (baseE->stream == baseStream) {
        // TODO: Check the FIFOIdx to make sure that the element is correct to
        // TODO: use.
        if (!baseE->isValueReady) {
          S_ELEMENT_PANIC(this, "BaseElement %s is not value ready.",
                          baseE->FIFOIdx);
        }
        auto vaddr = baseE->addr;
        int32_t size = baseE->size;
        // Handle offset for coalesced stream.
        int32_t offset;
        baseStream->getCoalescedOffsetAndSize(baseStreamId, offset, size);
        vaddr += offset;
        // TODO: Fix this for reduction stream.
        assert(size <= sizeof(StreamValue) &&
               "Base element too large, maybe coalesced?");
        // ! This effectively does zero extension.
        StreamValue baseValue;
        baseE->getValue(vaddr, size, baseValue.uint8Ptr());
        S_ELEMENT_DPRINTF(baseE,
                          "GetStreamValue vaddr %#x size %d value %llu.\n",
                          vaddr, size, baseValue.front());
        return baseValue;
      }
    }
    S_ELEMENT_PANIC(this, "Failed to find the base stream value of %s.\n",
                    baseStream->getStreamName());
  };
  Addr addr = this->dynS->addrGenCallback
                  ->genAddr(this->FIFOIdx.entryIdx,
                            this->dynS->addrGenFormalParams, getStreamValue)
                  .front();
  S_ELEMENT_DPRINTF(this, "ComputeAddr vaddr %#x.\n", addr);
  return addr;
}

void StreamElement::markAddrReady() {
  assert(!this->addrReady && "Addr is already ready.");
  this->addrReady = true;
  this->addrReadyCycle = this->stream->se->curCycle();

  /**
   * For non-mem streams, we set the address to 0 and directly set the value.
   * This is because other streams do not have address.
   */
  this->size = this->stream->getMemElementSize();
  if (this->stream->isMemStream()) {
    this->addr = this->computeAddr();
  } else {
    this->addr = 0;
  }

  S_ELEMENT_DPRINTF(this, "MarkAddrReady vaddr %#x size %d.\n", this->addr,
                    this->size);

  this->splitIntoCacheBlocks();
}

void StreamElement::readOracleValueFromMem() {
  const int MaxBufferSize = 64;
  uint8_t buffer[MaxBufferSize];
  assert(this->cacheBlockSize <= MaxBufferSize && "CacheLine too Large.");
  for (int i = 0; i < this->cacheBlocks; ++i) {
    auto &cacheBlock = this->cacheBlockBreakdownAccesses[i];
    this->stream->getCPUDelegator()->readFromMem(cacheBlock.cacheBlockVAddr,
                                                 this->cacheBlockSize, buffer);
    this->setValue(cacheBlock.cacheBlockVAddr, this->cacheBlockSize, buffer);
  }
  if (!this->isValueReady) {
    S_ELEMENT_PANIC(this, "Failed to ReadOracleValue.");
  }
}

void StreamElement::computeValue() {

  auto S = this->stream;
  auto dynS = this->dynS;
  if (!S->shouldComputeValue()) {
    S_ELEMENT_PANIC(this, "Cannot compute value.");
  }
  if (!this->isAddrReady()) {
    S_ELEMENT_PANIC(this, "ComputeValue should have addr ready.");
  }

  auto getBaseValue = [this](StaticId id) -> StreamValue {
    return this->getValueBaseByStreamId(id);
  };

  StreamValue result;
  Cycles estimatedLatency;
  if (S->isStoreComputeStream() || S->isUpdateStream()) {
    assert(!this->isElemFloatedToCache() &&
           "Should not compute for floating stream.");
    // Check for value base element.
    if (!this->checkValueBaseElemsValueReady()) {
      S_ELEMENT_PANIC(this, "StoreFunc with ValueBaseE not value ready.");
    }
    auto params =
        convertFormalParamToParam(dynS->storeFormalParams, getBaseValue);
    result = dynS->storeCallback->invoke(params);
    estimatedLatency = dynS->storeCallback->getEstimatedLatency();

    S_ELEMENT_DPRINTF(this, "StoreValue %s.\n", result);

  } else if (S->isLoadComputeStream()) {

    assert(!this->isElemFloatedToCache() &&
           "Should not compute for floating LoadComputeStream.");
    if (!this->checkValueBaseElemsValueReady()) {
      S_ELEMENT_PANIC(this, "LoadFunc with ValueBaseE not value ready.");
    }
    auto params =
        convertFormalParamToParam(dynS->loadFormalParams, getBaseValue);
    result = dynS->loadCallback->invoke(params);
    estimatedLatency = dynS->loadCallback->getEstimatedLatency();

    S_ELEMENT_DPRINTF(this, "LoadComputeValue %s.\n", result);

  } else if (S->isPointerChaseIndVar()) {

    if (this->FIFOIdx.entryIdx == 0) {
      this->setValue(this->addr, this->size, dynS->initialValue.uint8Ptr());
      return;
    }

    result = dynS->addrGenCallback->genAddr(
        this->FIFOIdx.entryIdx, dynS->addrGenFormalParams, getBaseValue);
    estimatedLatency = dynS->addrGenCallback->getEstimatedLatency();

  } else if (S->isReduction()) {

    /**
     * There are three special cases for ReductionStream.
     * 1. The first element should take the initial value.
     * 2. The InnerLastElem of floating ReductionStream should take the final
     * value.
     * 3. When computing NextElemValue, if InnerLastElem shoud be replaced with
     * initialValue.
     */

    if (this->FIFOIdx.entryIdx == 0) {
      // Case 1.
      this->setValue(this->addr, this->size, dynS->initialValue.uint8Ptr());
      return;
    } else if (this->isInnerLastElem() && !S->hasCoreUser() &&
               this->isElemFloatedToCache()) {
      assert(dynS->isInnerFinalValueReady(this->FIFOIdx.entryIdx) &&
             "FinalReductionValue should be ready.");
      this->setValue(
          this->addr, this->size,
          dynS->getInnerFinalValue(this->FIFOIdx.entryIdx).uint8Ptr());
      return;
    }

    // We need a special getBaseValue to handle case 3.
    auto getReduceBaseValue = [this](StaticId id) -> StreamValue {
      if (id == this->stream->staticId && this->isInnerSecondElem()) {
        // Special handling on the PrevReductionElem.
        return this->dynS->initialValue;
      } else {
        // Normal ValueBaseStream.
        return this->getValueBaseByStreamId(id);
      }
    };

    result = dynS->addrGenCallback->genAddr(
        this->FIFOIdx.entryIdx, dynS->addrGenFormalParams, getReduceBaseValue);
    estimatedLatency = dynS->addrGenCallback->getEstimatedLatency();

  } else {
    /**
     * This should be an normal IV.
     */
    result = dynS->addrGenCallback->genAddr(
        this->FIFOIdx.entryIdx, dynS->addrGenFormalParams, getBaseValue);
    estimatedLatency = dynS->addrGenCallback->getEstimatedLatency();
  }
  /**
   * We try to model the computation overhead for StoreStream, UpdateStreawm
   * and ReductionStream. For simple IVStream we do not bother.
   */
  if (S->isStoreComputeStream() || S->isLoadComputeStream() ||
      S->isUpdateStream() || S->isReduction()) {

    /**
     * Charge the initial latency to access the Core SIMD unit here.
     * 1. If this is SIMD operation.
     * 2. If the SE has not scalar ALU.
     */
    if (!this->se->myParams->hasScalarALU || S->isSIMDComputation()) {
      estimatedLatency += Cycles(this->se->myParams->computeSIMDDelay);
    }

    this->se->computeEngine->pushReadyComputation(this, std::move(result),
                                                  estimatedLatency);
  } else {
    // Set the element with the value.
    this->setValue(this->addr, this->size, result.uint8Ptr());
  }
}

void StreamElement::tryMarkValueReady() {
  for (int blockIdx = 0; blockIdx < this->cacheBlocks; ++blockIdx) {
    const auto &block = this->cacheBlockBreakdownAccesses[blockIdx];
    if (block.state != CacheBlockBreakdownAccess::StateE::Ready &&
        block.state != CacheBlockBreakdownAccess::StateE::Faulted) {
      return;
    }
  }
  this->markValueReady();
}

void StreamElement::markValueReady() {
  assert(!this->isValueReady && "Value is already ready.");
  this->isValueReady = true;
  this->valueReadyCycle = this->getStream()->getCPUDelegator()->curCycle();
  if (Debug::DEBUG_TYPE) {
    bool faulted = false;
    for (int blockIdx = 0; blockIdx < this->cacheBlocks; ++blockIdx) {
      const auto &block = this->cacheBlockBreakdownAccesses[blockIdx];
      if (block.state == CacheBlockBreakdownAccess::StateE::Faulted) {
        faulted = true;
        break;
      }
    }
    if (faulted) {
      S_ELEMENT_DPRINTF(this, "Value ready: faulted.\n");
    } else {
      S_ELEMENT_DPRINTF(this, "Value ready.\n");
    }
  }

  // Notify the stream for statistics.
  if (this->issueCycle >= this->addrReadyCycle &&
      this->issueCycle <= this->valueReadyCycle) {
    // The issue cycle is valid.
    this->stream->statistic.numCycleRequestLatency +=
        this->valueReadyCycle - this->issueCycle;
  }
}

void StreamElement::splitIntoCacheBlocks() {
  // TODO: Initialize this only once.
  this->cacheBlockSize = this->se->getCPUDelegator()->cacheLineSize();

  for (int currentSize, totalSize = 0; totalSize < this->size;
       totalSize += currentSize) {
    if (this->cacheBlocks >= StreamElement::MAX_CACHE_BLOCKS) {
      panic("More than %d cache blocks for one stream element, address %lu "
            "size %lu.",
            this->cacheBlocks, this->addr, this->size);
    }
    auto currentAddr = this->addr + totalSize;
    currentSize = this->size - totalSize;
    // Make sure we don't span across multiple cache blocks.
    if (((currentAddr % cacheBlockSize) + currentSize) > cacheBlockSize) {
      currentSize = cacheBlockSize - (currentAddr % cacheBlockSize);
    }
    // Create the breakdown.
    auto cacheBlockAddr = currentAddr & (~(cacheBlockSize - 1));
    auto &newCacheBlockBreakdown =
        this->cacheBlockBreakdownAccesses[this->cacheBlocks];
    newCacheBlockBreakdown.cacheBlockVAddr = cacheBlockAddr;
    newCacheBlockBreakdown.vaddr = currentAddr;
    newCacheBlockBreakdown.size = currentSize;
    newCacheBlockBreakdown.state =
        CacheBlockBreakdownAccess::StateE::Initialized;
    this->cacheBlocks++;
  }

  // Expand the value to match the number of cache blocks.
  // We never shrink this value vector.
  auto cacheBlockBytes = this->cacheBlocks * cacheBlockSize;
  while (this->value.size() < cacheBlockBytes) {
    this->value.push_back(0);
  }
}

void StreamElement::setValue(StreamElement *prevElement) {
  // Fill element cache blocks from previous element.
  // This should be completely overlapped.
  assert(prevElement->next == this && "Next element should be me.");
  for (int blockIdx = 0; blockIdx < this->cacheBlocks; ++blockIdx) {
    auto &block = this->cacheBlockBreakdownAccesses[blockIdx];
    if (block.state != CacheBlockBreakdownAccess::StateE::PrevElement) {
      continue;
    }
    // Get previous block.
    auto prevBlockOffset = prevElement->mapVAddrToBlockOffset(
        block.cacheBlockVAddr, this->cacheBlockSize);
    const auto &prevBlock =
        prevElement->cacheBlockBreakdownAccesses[prevBlockOffset];
    if (prevBlock.state == CacheBlockBreakdownAccess::StateE::Faulted) {
      // Propagate the faulted state.
      block.state = CacheBlockBreakdownAccess::StateE::Faulted;
      this->tryMarkValueReady();
      continue;
    }
    auto offset = prevElement->mapVAddrToValueOffset(block.cacheBlockVAddr,
                                                     this->cacheBlockSize);
    // Copy the value from prevElement.
    this->setValue(block.cacheBlockVAddr, this->cacheBlockSize,
                   &prevElement->value.at(offset));
    assert(block.state == CacheBlockBreakdownAccess::StateE::Ready);
  }
}

void StreamElement::setValue(Addr vaddr, int size, const uint8_t *val) {
  // Copy the data.
  auto initOffset = this->mapVAddrToValueOffset(vaddr, size);
  S_ELEMENT_DPRINTF(this, "SetValue [%#x, %#x), initOffset %d, data %s.\n",
                    vaddr, vaddr + size, initOffset,
                    GemForgeUtils::dataToString(val, size));
  for (int i = 0; i < size; ++i) {
    this->value.at(i + initOffset) = val[i];
  }
  // Mark the cache line ready.
  // Fast path for IV stream with exact match.
  // This is to avoid overflow for negative IV.
  if (!this->stream->isMemStream() && vaddr == this->addr &&
      size == this->size) {
    for (int blockIdx = 0; blockIdx < this->cacheBlocks; ++blockIdx) {
      auto &block = this->cacheBlockBreakdownAccesses[blockIdx];
      block.state = CacheBlockBreakdownAccess::StateE::Ready;
    }
    this->tryMarkValueReady();
    return;
  }
  // TODO: Really check that every byte is set.
  auto vaddrRHS = vaddr + size;
  for (int blockIdx = 0; blockIdx < this->cacheBlocks; ++blockIdx) {
    auto &block = this->cacheBlockBreakdownAccesses[blockIdx];
    auto blockRHS = block.cacheBlockVAddr + this->cacheBlockSize;
    // So far we just check for overlap.
    if (blockRHS >= block.cacheBlockVAddr && vaddrRHS >= vaddr) {
      // No overflow.
      if (vaddr >= blockRHS || vaddrRHS <= block.cacheBlockVAddr) {
        // No overlap.
        continue;
      }
    } else {
      // Both overflow. Definitely overlap.
      panic("Overflow in vaddr [%#x, +%d).\n", vaddr, size);
    }
    S_ELEMENT_DPRINTF(this, "Mark block ready: [%#x, %#x).\n",
                      block.cacheBlockVAddr,
                      block.cacheBlockVAddr + this->cacheBlockSize);
    block.state = CacheBlockBreakdownAccess::StateE::Ready;
  }

  this->tryMarkValueReady();
}

void StreamElement::getValue(Addr vaddr, int size, uint8_t *val) const {
  // Copy the data.
  auto ptr = this->getValuePtr(vaddr, size);
  for (int i = 0; i < size; ++i) {
    val[i] = ptr[i];
  }
}

const uint8_t *StreamElement::getValuePtr(Addr vaddr, int size) const {
  // Copy the data.
  auto initOffset = this->mapVAddrToValueOffset(vaddr, size);
  S_ELEMENT_DPRINTF(
      this, "GetValue [%#x, +%d), initOffset %d, data %s.\n", vaddr, size,
      initOffset,
      GemForgeUtils::dataToString(&this->value.at(initOffset), size));
  return this->value.data() + initOffset;
}

void StreamElement::getValueByStreamId(StaticId streamId, uint8_t *val,
                                       int valLen) const {
  auto vaddr = this->addr;
  int size = this->size;
  // Handle offset for coalesced stream.
  int32_t offset;
  this->stream->getCoalescedOffsetAndSize(streamId, offset, size);
  assert(size <= valLen && "ElemSize overflow.");
  vaddr += offset;
  this->getValue(vaddr, size, val);
}

const uint8_t *StreamElement::getValuePtrByStreamId(StaticId streamId) const {
  auto vaddr = this->addr;
  int size = this->size;
  // Handle offset for coalesced stream.
  int32_t offset;
  this->stream->getCoalescedOffsetAndSize(streamId, offset, size);
  vaddr += offset;
  auto initOffset = this->mapVAddrToValueOffset(vaddr, size);
  S_ELEMENT_DPRINTF(
      this, "GetValue [%#x, +%d), initOffset %d, data 0x%s.\n", vaddr, size,
      initOffset,
      GemForgeUtils::dataToString(&this->value.at(initOffset), size));
  return &this->value.at(initOffset);
}

const uint8_t *
StreamElement::getUpdateValuePtrByStreamId(StaticId streamId) const {
  auto vaddr = this->addr;
  int size = this->size;
  /**
   * UpdateValue is not handled at line granularity.
   */
  int32_t offset;
  this->stream->getCoalescedOffsetAndSize(streamId, offset, size);
  S_ELEMENT_DPRINTF(
      this, "GetUpdateValue [%#x, +%d), offset %d, data %s.\n", vaddr, size,
      offset,
      GemForgeUtils::dataToString(this->updateValue.uint8Ptr(offset), size));
  return this->updateValue.uint8Ptr(offset);
}

void StreamElement::receiveComputeResult(const StreamValue &result) {
  if (this->stream->isUpdateStream()) {
    // UpdateStream receive computation result in UpdateValue.
    if (this->isUpdateValueReady()) {
      S_ELEMENT_PANIC(this, "UpdateValue already ready.");
    }
    S_ELEMENT_DPRINTF(this, "Mark UpdateValue Ready.\n");
    this->updateValue = result;
    this->updateValueReady = true;
  } else if (this->stream->isLoadComputeStream()) {
    if (this->isLoadComputeValueReady()) {
      S_ELEMENT_PANIC(this, "LoadComputeValue already ready.");
    }
    S_ELEMENT_DPRINTF(this, "Mark LoadComputeValue Ready.\n");
    this->loadComputeValue = result;
    this->loadComputeValueReady = true;
  } else {
    this->setValue(this->addr, this->size, result.uint8Ptr());
  }
}

StreamValue StreamElement::getValueBaseByStreamId(StaticId id) {
  // Search the ValueBaseElements.
  auto baseS = this->se->getStream(id);
  for (const auto &baseElement : this->valueBaseElements) {
    auto baseE = baseElement.getElement();
    if (baseE->stream->isCoalescedHere(id)) {
      /**
       * For unfloated LoadComputeStream, we should use the LoadComputeValue.
       * Except when I am the LoadComputeStream of course.
       */
      StreamValue elemValue;
      if (baseE != this && baseE->stream->isLoadComputeStream() &&
          !baseE->isElemFloatedToCache()) {
        baseE->getLoadComputeValue(elemValue.uint8Ptr(), sizeof(elemValue));
      } else {
        baseE->getValueByStreamId(id, elemValue.uint8Ptr(), sizeof(elemValue));
      }
      S_ELEMENT_DPRINTF(this, "GetBaseValue %s Id %d Val %s.\n", baseE->FIFOIdx,
                        id, elemValue);
      return elemValue;
    }
  }
  S_ELEMENT_PANIC(this, "Failed to find ValueBaseE for %s.",
                  baseS->getStreamName());
}

bool StreamElement::isValueFaulted(Addr vaddr, int size) const {
  if (vaddr + size < vaddr) {
    // Wrap around.
    S_ELEMENT_DPRINTF(this, "ValueFaulted as vaddr overflow [%#x, +%d).\n",
                      vaddr, size);
    return true;
  }
  auto blockIdx = this->mapVAddrToBlockOffset(vaddr, size);
  auto blockEnd = this->mapVAddrToBlockOffset(vaddr + size - 1, 1);
  while (blockIdx <= blockEnd) {
    const auto &block = this->cacheBlockBreakdownAccesses[blockIdx];
    if (block.state == CacheBlockBreakdownAccess::Faulted) {
      return true;
    }
    blockIdx++;
  }
  return false;
}

void StreamElement::updateFirstValueCheckCycle(bool checkedByCore) const {
  if (this->firstValueCheckCycle == 0 ||
      (this->firstValueCheckByCoreCycle == 0 && checkedByCore)) {
    auto curCycle = this->se->curCycle();
    if (this->firstValueCheckCycle == 0) {
      this->firstValueCheckCycle = curCycle;
    }
    if (this->firstValueCheckByCoreCycle == 0 && checkedByCore) {
      this->firstValueCheckByCoreCycle = curCycle;
    }
    S_ELEMENT_DPRINTF(this,
                      "Mark FirstCheckCycle %lu, FirstCoreCheckCycle %llu, "
                      "AddrReady %d ValueReady %d UpdateValueReady %d.\n",
                      this->firstValueCheckCycle,
                      this->firstValueCheckByCoreCycle, this->isAddrReady(),
                      this->isValueReady, this->updateValueReady);
  }
}

bool StreamElement::shouldComputeValue() const {
  /**
   * 1. Stream need to compute value.
   * 2. One of:
   *  a. I am not floated to cache.
   *  b. I am the last value for reduction.
   *  c. I am the pointer-chase ind-var.
   */
  auto S = this->stream;
  return S->shouldComputeValue() &&
         ((!this->isElemFloatedToCache()) ||
          (S->isReduction() && this->isInnerLastElem()) ||
          S->isPointerChaseIndVar());
}

bool StreamElement::isComputeValueReady() const {
  if (this->stream->isUpdateStream()) {
    return this->isUpdateValueReady();
  } else if (this->stream->isLoadComputeStream()) {
    return this->isLoadComputeValueReady();
  } else {
    return this->isValueReady;
  }
}

bool StreamElement::checkValueReady(bool checkedByCore) const {
  this->updateFirstValueCheckCycle(checkedByCore);
  return this->isValueReady;
}

bool StreamElement::checkUpdateValueReady() const {
  // UpdateValue should only be checked by Core.
  this->updateFirstValueCheckCycle(true);
  return this->updateValueReady;
}

bool StreamElement::checkLoadComputeValueReady(bool checkedByCore) const {
  // LoadComputeValue should only be checked by Core.
  this->updateFirstValueCheckCycle(checkedByCore);
  return this->loadComputeValueReady;
}

void StreamElement::getLoadComputeValue(uint8_t *val, int valLen) const {
  if (!this->isLoadComputeValueReady()) {
    S_ELEMENT_PANIC(this, "LoadComputeValue is not ready yet.");
  }
  auto coreElementSize = this->stream->getCoreElementSize();
  if (valLen < coreElementSize) {
    S_ELEMENT_PANIC(this, "LoadComputeValue size %d > buffer size %d.",
                    coreElementSize, valLen);
  }
  for (auto i = 0; i < coreElementSize; ++i) {
    val[i] = this->loadComputeValue.uint8Ptr()[i];
  }
}

bool StreamElement::checkValueBaseElemsValueReady() {
  if (this->hasUnInitInnerLoopValueBaseElem) {
    this->dynS->tryAddInnerLoopBaseElements(this);
  }
  S_ELEMENT_DPRINTF(this, "[AddrBaseReady] Check.\n");
  if (this->hasUnInitInnerLoopValueBaseElem) {
    return false;
  }
  /**
   * Special case for LastElement of:
   * 1. ReduceS or PtrChaseIndVarS.
   * 2. No core user.
   * 3. No DepNestRegion.
   * 4. Floated.
   * Which is marked ready by checking the DynStream.
   */
  if ((this->stream->isReduction() || this->stream->isPointerChaseIndVar()) &&
      !this->stream->hasCoreUser() && !this->stream->hasDepNestRegion() &&
      this->isElemFloatedToCache()) {
    if (this->isInnerLastElem()) {
      return this->dynS->isInnerFinalValueReady(this->FIFOIdx.entryIdx);
    } else {
      // Should never be ready.
      return false;
    }
  }
  /**
   * Special case for the last element of stream with TripCount 0, we should
   * never be ready.
   */
  if (this->dynS->hasZeroTripCount() && this->isLastElement()) {
    return false;
  }
  for (const auto &baseElem : this->valueBaseElements) {
    if (!baseElem.isValid()) {
      S_ELEMENT_PANIC(this, "ValueBaseE released early: %s.",
                      baseElem.getIdx());
    }
    auto baseE = baseElem.getElement();
    if (baseE == this) {
      /**
       * Some ComputeStream require myself as the ValueBase. We don't call
       * checkValueReady to avoid recursive dependence information in the
       * firstCheckCycle.
       */
      if (!this->isValueReady) {
        return false;
      }
    } else {
      /**
       * Special case for unfloated LoadComputeStream, which we should check
       * the LoadComputeValue.
       */
      if (baseE->stream->isLoadComputeStream() &&
          !baseE->isElemFloatedToCache()) {
        if (!baseE->checkLoadComputeValueReady(false /* CheckedByCore */)) {
          return false;
        }
      } else {
        if (!baseE->checkValueReady(false /* CheckedByCore */)) {
          S_ELEMENT_DPRINTF(this, "ValueBaseE not ValueReady: %s.\n",
                            baseE->FIFOIdx);
          return false;
        }
      }
    }
  }
  return true;
}

uint64_t StreamElement::mapVAddrToValueOffset(Addr vaddr, int size) const {
  if (this->cacheBlocks == 0) {
    S_ELEMENT_PANIC(this, "There is no cache blocks. AddrReady %d.",
                    this->isAddrReady());
  }
  auto firstCacheBlockVAddr =
      this->cacheBlockBreakdownAccesses[0].cacheBlockVAddr;
  if (vaddr < firstCacheBlockVAddr) {
    S_ELEMENT_PANIC(this, "Underflow of vaddr %#x, [%#x, +%d).", vaddr,
                    this->addr, this->size);
  }
  auto initOffset = vaddr - firstCacheBlockVAddr;
  assert(initOffset + size <= this->value.size() && "Overflow of size.");
  return initOffset;
}

uint64_t StreamElement::mapVAddrToBlockOffset(Addr vaddr, int size) const {
  return this->mapVAddrToValueOffset(vaddr, size) / this->cacheBlockSize;
}

void StreamElement::setReqIssued() {
  if (this->reqIssued) {
    S_ELEMENT_PANIC(this, "Request already issued.\n");
  }
  this->reqIssued = true;
}

void StreamElement::setPrefetchIssued() {
  if (this->prefetchIssued) {
    S_ELEMENT_PANIC(this, "Prefetch already issued.\n");
  }
  this->prefetchIssued = true;
}

void StreamElement::dump() const {
  inform("Stream %50s %d.%d (%d%d).\n", this->stream->getStreamName().c_str(),
         this->FIFOIdx.streamId.streamInstance, this->FIFOIdx.entryIdx,
         static_cast<int>(this->isAddrReady()),
         static_cast<int>(this->isValueReady));
}
} // namespace gem5
