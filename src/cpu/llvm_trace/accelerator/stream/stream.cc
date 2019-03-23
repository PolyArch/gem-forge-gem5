#include "stream.hh"
#include "insts.hh"
#include "stream_engine.hh"

#include "cpu/llvm_trace/llvm_trace_cpu.hh"

#include "base/misc.hh"
#include "base/trace.hh"
#include "debug/StreamEngine.hh"
#include "proto/protoio.hh"

#define STREAM_DPRINTF(format, args...)                                      \
  DPRINTF(StreamEngine, "Stream %s: " format, this->getStreamName().c_str(), \
          ##args)

#define STREAM_ENTRY_DPRINTF(entry, format, args...)                      \
  STREAM_DPRINTF("Entry (%lu, %lu): " format, (entry).idx.streamInstance, \
                 (entry).idx.entryIdx, ##args)

#define STREAM_HACK(format, args...) \
  hack("Stream %s: " format, this->getStreamName().c_str(), ##args)

#define STREAM_ENTRY_HACK(entry, format, args...)                      \
  STREAM_HACK("Entry (%lu, %lu): " format, (entry).idx.streamInstance, \
              (entry).idx.entryIdx, ##args)

#define STREAM_PANIC(format, args...)                                   \
  {                                                                     \
    this->dump();                                                       \
    panic("Stream %s: " format, this->getStreamName().c_str(), ##args); \
  }

#define STREAM_ENTRY_PANIC(entry, format, args...)                      \
  STREAM_PANIC("Entry (%lu, %lu): " format, (entry).idx.streamInstance, \
               (entry).idx.entryIdx, ##args)

Stream::Stream(LLVMTraceCPU *_cpu, StreamEngine *_se, bool _isOracle,
               size_t _maxRunAHeadLength, const std::string &_throttling)
    : cpu(_cpu),
      se(_se),
      isOracle(_isOracle),
      firstConfigSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
      configSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
      endSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
      storedData(nullptr),
      maxRunAHeadLength(_maxRunAHeadLength),
      runAHeadLength(_maxRunAHeadLength),
      throttling(_throttling) {
  /**
   * Throttling information initialization.
   */
  if (this->throttling != "static") {
    // We are doing dynamic throttling, we should start with a small
    // runAHeadLength and slowly increasing.
    this->runAHeadLength = 2;
  }

  this->storedData = new uint8_t[cpu->system->cacheLineSize()];

  this->configured = false;
  this->head = &this->nilTail;
  this->stepped = &this->nilTail;
  this->tail = &this->nilTail;
  this->allocSize = 0;
  this->stepSize = 0;
  this->maxSize = _maxRunAHeadLength;
  this->stepRootStream = nullptr;
  this->lateFetchCount = 0;
}

Stream::~Stream() {
  // Actually the stream is only deallocated at the end of the program.
  // But we still release the memory for completeness.
  if (this->storedData != nullptr) {
    delete[] this->storedData;
    this->storedData = nullptr;
  }

  for (auto memAccess : this->memAccesses) {
    delete memAccess;
  }
  this->memAccesses.clear();
}

bool Stream::isMemStream() const {
  return this->getStreamType() == "load" || this->getStreamType() == "store";
}

void Stream::addBaseStream(Stream *baseStream) {
  if (baseStream == this) {
    STREAM_PANIC("Base stream should not be self.");
  }
  this->baseStreams.insert(baseStream);
  baseStream->dependentStreams.insert(this);
}

void Stream::addBaseStepStream(Stream *baseStepStream) {
  if (baseStepStream == this) {
    STREAM_PANIC("Base stream should not be self.");
  }
  this->baseStepStreams.insert(baseStepStream);
  baseStepStream->dependentStepStreams.insert(this);
  if (baseStepStream->isStepRoot()) {
    this->baseStepRootStreams.insert(baseStepStream);
  } else {
    for (auto stepRoot : baseStepStream->baseStepRootStreams) {
      this->baseStepRootStreams.insert(stepRoot);
    }
  }
}

void Stream::registerStepDependentStreamToRoot(Stream *newStepDependentStream) {
  if (!this->isStepRoot()) {
    STREAM_PANIC("Try to register step instruction to non-root stream.");
  }
  for (auto &stepStream : this->stepStreamList) {
    if (stepStream == newStepDependentStream) {
      STREAM_PANIC(
          "The new step dependent stream has already been registered.");
    }
  }
  this->stepStreamList.emplace_back(newStepDependentStream);
}

uint64_t Stream::getCacheBlockAddr(uint64_t addr) const {
  return addr & (~(cpu->system->cacheLineSize() - 1));
}

void Stream::addAliveCacheBlock(uint64_t addr) const {
  if (this->getStreamType() == "phi") {
    return;
  }
  auto cacheBlockAddr = this->getCacheBlockAddr(addr);
  if (this->aliveCacheBlocks.count(cacheBlockAddr) == 0) {
    this->aliveCacheBlocks.emplace(cacheBlockAddr, 1);
  } else {
    this->aliveCacheBlocks.at(cacheBlockAddr)++;
  }
}

bool Stream::isCacheBlockAlive(uint64_t addr) const {
  if (this->getStreamType() == "phi") {
    return false;
  }
  auto cacheBlockAddr = this->getCacheBlockAddr(addr);
  return this->aliveCacheBlocks.count(cacheBlockAddr) != 0;
}

void Stream::removeAliveCacheBlock(uint64_t addr) const {
  if (this->getStreamType() == "phi") {
    return;
  }
  auto cacheBlockAddr = this->getCacheBlockAddr(addr);
  auto aliveMapIter = this->aliveCacheBlocks.find(cacheBlockAddr);
  if (aliveMapIter == this->aliveCacheBlocks.end()) {
    STREAM_PANIC("Missing alive cache block.");
  } else {
    if (aliveMapIter->second == 1) {
      this->aliveCacheBlocks.erase(aliveMapIter);
    } else {
      aliveMapIter->second--;
    }
  }
}

void Stream::updateRunAHeadLength(size_t newRunAHeadLength) {
  // So far we only increase run ahead length.
  if (newRunAHeadLength <= this->runAHeadLength) {
    return;
  }
  // if (newRunAHeadLength > this->maxRunAHeadLength) {
  //   return;
  // }
  auto delta = newRunAHeadLength - this->runAHeadLength;
  this->se->currentTotalRunAheadLength += delta;
  this->runAHeadLength = newRunAHeadLength;
  // Back pressure to base step streams.
  for (auto S : this->baseStepStreams) {
    S->updateRunAHeadLength(this->runAHeadLength);
  }
  // We also have to sync with dependent step streams.
  for (auto S : this->dependentStepStreams) {
    S->updateRunAHeadLength(this->runAHeadLength);
  }
}

void Stream::configure(StreamConfigInst *inst) {
  auto configSeqNum = inst->getSeqNum();
  STREAM_DPRINTF("Configured at seq num %lu.\n", configSeqNum);
  this->configSeqNum = configSeqNum;
  if (this->firstConfigSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
    this->firstConfigSeqNum = configSeqNum;
  }

  /**
   * For all the entries without stepSeqNum, this means that these entries are
   * speculative run ahead. Clear them out.
   * NOTE: The only exception is the first entry without stepSeqNum, which may
   * be the last element in the stream. To handle this, we use the end
   * inst as its step inst.
   */

  bool foundLastElement = false;
  auto FIFOIter = this->FIFO.begin();
  while (FIFOIter != this->FIFO.end()) {
    if (FIFOIter->stepped()) {
      // This one has already stepped.
      ++FIFOIter;
    } else {
      if (!foundLastElement && this->isStepRoot()) {
        /**
         * Only the step root stream can decide to keep the last element.
         */
        foundLastElement = true;
        this->stepImpl(configSeqNum);
        this->triggerStep(configSeqNum, this);
        FIFOIter++;
      } else {
        // This one should be cleared.
        STREAM_ENTRY_DPRINTF(*FIFOIter, "Clear run-ahead entry.\n");
        // Release the userToEntryMap.
        for (const auto &user : FIFOIter->users) {
          this->userToEntryMap.erase(user);
        }
        if (FIFOIter->isValueValid) {
          for (int i = 0; i < FIFOIter->cacheBlocks; ++i) {
            this->removeAliveCacheBlock(FIFOIter->cacheBlockAddrs[i]);
          }
        }
        FIFOIter = this->FIFO.erase(FIFOIter);
      }
    }
  }

  // Reset the FIFOIdx.
  this->FIFOIdx.newInstance(configSeqNum);
  if (!this->isConfigured()) {
    STREAM_PANIC(
        "After configure we should immediately be configured with "
        "config seq %lu, end seq %lu.",
        this->configSeqNum, this->endSeqNum);
  }
  while (this->FIFO.size() < this->runAHeadLength) {
    this->enqueueFIFO();
  }

  this->se->currentTotalRunAheadLength += this->runAHeadLength;
}

void Stream::commitConfigure(StreamConfigInst *inst) {
  auto configSeqNum = inst->getSeqNum();
  if (!this->isStepRoot()) {
    return;
  }
  if (this->FIFO.empty()) {
    return;
  }
  {
    auto &entry = this->FIFO.front();
    if (entry.stepSeqNum != configSeqNum) {
      // Nothing to step.
      return;
    }
    STREAM_ENTRY_DPRINTF(entry, "Commit config with seqNum %lu.\n",
                         configSeqNum);
  }
  this->commitStepImpl(configSeqNum);
  // Send out the step signal as the root stream.
  this->triggerCommitStep(configSeqNum, this);
}

void Stream::store(StreamStoreInst *inst) {
  auto storeSeqNum = inst->getSeqNum();
  STREAM_DPRINTF("Stored with seqNum %lu.\n", storeSeqNum);
  if (this->FIFO.empty()) {
    STREAM_PANIC("Store when the fifo is empty for stream %s.",
                 this->getStreamName().c_str());
  }

  if (this->storedData == nullptr) {
    STREAM_PANIC("StoredData is nullptr for store stream.");
  }

  auto entry = this->findCorrectUsedEntry(storeSeqNum);
  if (entry == nullptr) {
    STREAM_PANIC(
        "Try to store when there is no available entry. Something "
        "wrong in isReady.");
  }

  if (entry->stored()) {
    // if (!this->enableCoalesce && !this->enableMerge) {
    //   // For merge and coalesce, it allows multiple stores to the same entry.
    inst->dumpDeps(cpu);
    STREAM_ENTRY_PANIC(*entry,
                       "entry is already stored by %lu, now stored by %lu.",
                       entry->storeSeqNum, storeSeqNum);
    // }
  } else {
    entry->store(storeSeqNum);
  }

  /**
   * For store stream, if there is no base step stream, which means this is a
   * constant store or somehow, we can step it now.
   */
  if (this->isStepRoot()) {
    // Implicitly step the stream.
    this->step(storeSeqNum);
  }
}

void Stream::commitStore(StreamStoreInst *inst) {
  auto storeSeqNum = inst->getSeqNum();
  STREAM_DPRINTF("Store committed with seq %lu.\n", storeSeqNum);
  if (this->FIFO.empty()) {
    STREAM_PANIC("Commit store when the FIFO is empty.");
  }
  auto &entry = this->FIFO.front();
  if (entry.storeSeqNum != storeSeqNum) {
    STREAM_ENTRY_PANIC(
        entry, "Mismatch between the store seq num %lu with entry (%lu).",
        storeSeqNum, entry.storeSeqNum);
  }
  // Now actually send the committed data.
  if (this->storedData == nullptr) {
    STREAM_PANIC("StoredData is nullptr for store stream.");
  }

  /**
   * Send the write packet with random data.
   */
  bool shouldSend = false;
  bool cacheLineStore = false;
  if (entry.address != 0) {
    if (this->se->isContinuousStoreOptimized()) {
      if (this->isContinuous()) {
        auto N = cpu->system->cacheLineSize() / this->getElementSize();
        if (N == 0) {
          N = 1;
        }
        if (entry.idx.entryIdx % N == (N - 1)) {
          // We think we have buffered enough for a continuous store.
          shouldSend = true;
          cacheLineStore = true;
        }
      } else {
        shouldSend = true;
      }
    } else {
      shouldSend = true;
    }
  }
  if (shouldSend) {
    auto memAccess = new StreamMemAccess(this, entry.idx);
    this->memAccesses.insert(memAccess);
    auto paddr = cpu->translateAndAllocatePhysMem(entry.address);
    const auto elementSize = this->getElementSize();
    STREAM_DPRINTF("Send stream store packet at %p size %d.\n",
                   reinterpret_cast<void *>(entry.address), elementSize);
    // Be careful to not cross cache line.
    const auto cacheBlockSize = cpu->system->cacheLineSize();
    auto offset = paddr % cacheBlockSize;
    auto size = elementSize;
    if (size + offset > cacheBlockSize) {
      // Hack here, we should really break the store into multiple packets.
      size = cacheBlockSize - offset;
    }

    if (cacheLineStore) {
      paddr &= (~(cacheBlockSize - 1));
      size = cacheBlockSize;
    }

    auto streamPlacementManager = this->se->getStreamPlacementManager();
    if (streamPlacementManager != nullptr &&
        streamPlacementManager->access(this, paddr, size, memAccess)) {
      // The StreamPlacementManager handled this packet.
    } else {
      // Else we sent out the packet.
      cpu->sendRequest(paddr, size, memAccess, storedData);
    }
  }

  /**
   * Implicitly commit the step if we have no base stream.
   */
  if (this->isStepRoot()) {
    this->commitStepImpl(storeSeqNum);
  }
}

void Stream::end(StreamEndInst *inst) {
  auto endSeqNum = inst->getSeqNum();
  this->endSeqNum = endSeqNum;
  this->se->currentTotalRunAheadLength -= this->runAHeadLength;
  // /**
  //  * For all the entries without stepSeqNum, this means that these entries
  //  are
  //  * speculative run ahead. Clear them out.
  //  * NOTE: The only exception is the first entry without stepSeqNum, which
  //  may
  //  * be the last element in the stream. To handle this, we use the end
  //  * inst as its step inst.
  //  */
  // if (this->configSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
  //   STREAM_PANIC("Stream end for unconfigured stream with end seq %lu.",
  //                endSeqNum);
  // }

  // if (this->configSeqNum > endSeqNum) {
  //   STREAM_PANIC("Stream end for future configured stream with end seq %lu "
  //                "config seq %lu.",
  //                endSeqNum, this->configSeqNum);
  // }

  // bool foundLastElement = false;
  // auto FIFOIter = this->FIFO.begin();
  // while (FIFOIter != this->FIFO.end()) {
  //   if (FIFOIter->stepped()) {
  //     // This one has already stepped.
  //     ++FIFOIter;
  //   } else {
  //     if (!foundLastElement && this->isStepRoot()) {
  //       /**
  //        * Only the step root stream can decide to keep the last element.
  //        */
  //       foundLastElement = true;
  //       this->stepImpl(endSeqNum);
  //       this->triggerStep(endSeqNum, this);
  //       FIFOIter++;
  //     } else {
  //       // This one should be cleared.
  //       STREAM_ENTRY_DPRINTF(*FIFOIter, "Clear run-ahead entry.\n");
  //       // Release the userToEntryMap.
  //       for (const auto &user : FIFOIter->users) {
  //         this->userToEntryMap.erase(user);
  //       }
  //       this->removeAliveCacheBlock(FIFOIter->address);
  //       FIFOIter = this->FIFO.erase(FIFOIter);
  //     }
  //   }
  // }
}

void Stream::commitEnd(StreamEndInst *inst) {
  // if (!this->isStepRoot()) {
  //   return;
  // }
  // if (this->FIFO.empty()) {
  //   return;
  // }
  // {
  //   auto &entry = this->FIFO.front();
  //   if (entry.stepSeqNum != endSeqNum) {
  //     // Nothing to step.
  //     return;
  //   }
  //   STREAM_ENTRY_DPRINTF(entry, "Commit end with seqNum %lu.\n", endSeqNum);
  // }
  // this->commitStepImpl(endSeqNum);
  // // Send out the step signal as the root stream.
  // this->triggerCommitStep(endSeqNum, this);
}

Stream::FIFOEntry *Stream::findCorrectUsedEntry(uint64_t userSeqNum) {
  for (auto &entry : this->FIFO) {
    if (entry.stepSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      // This entry has not been stepped.
      return &entry;
    } else if (entry.stepSeqNum > userSeqNum) {
      // This entry is already stepped, but the stepped inst is younger than the
      // user, so the user should use this entry.
      return &entry;
    }
  }
  return nullptr;
}

const Stream::FIFOEntry *Stream::findCorrectUsedEntry(
    uint64_t userSeqNum) const {
  for (const auto &entry : this->FIFO) {
    if (entry.stepSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      // This entry has not been stepped.
      return &entry;
    } else if (entry.stepSeqNum > userSeqNum) {
      // This entry is already stepped, but the stepped inst is younger than the
      // user, so the user should use this entry.
      return &entry;
    }
  }
  return nullptr;
}

bool Stream::isReady(const LLVMDynamicInst *user) const {
  auto userSeqNum = user->getSeqNum();
  if (this->FIFO.empty()) {
    return false;
  }
  // STREAM_DPRINTF("Check if stream is ready for inst %lu.\n", userSeqNum);
  const auto *entry = this->findCorrectUsedEntry(userSeqNum);
  {
    auto debugUserSeqNum = 1287100;
    if (userSeqNum == debugUserSeqNum) {
      hack("Check is ready for %lu.\n", debugUserSeqNum);
      user->dumpDeps(cpu);
      this->dump();
      // (*this->baseStepRootStreams.begin())->dump();
      if (entry != nullptr) {
        hack("Find entry \n");
        entry->dump();
      } else {
        hack("Failed to find entry.\n");
      }
    }
  }
  if (entry != nullptr) {
    bool emplaced = this->userToEntryMap.emplace(userSeqNum, entry).second;
    if (!emplaced) {
      // This means that this is not the first time for the user to check
      // isReady.
      const auto &previousEntry = this->userToEntryMap.at(userSeqNum);
      if (previousEntry != entry) {
        // They are different entry.
        // Should panic here.
      }
    }
    if (entry->users.empty()) {
      // This is the first time some instructions check if this entry is ready.
      entry->firstCheckIfReadyCycles = cpu->curCycle();
    } else {
      // // This entry already has an user, if this is a store stream this
      // should
      // // never happen (two stores to one entry.)
      // if (this->getStreamType() == "store") {
      //   STREAM_ENTRY_PANIC(*entry, "Double user to store stream: %lu.\n",
      //                      userSeqNum);
      //   user->dumpBasic();
      // }
    }
    entry->users.insert(userSeqNum);
  }
  return (entry != nullptr) && (entry->isValueValid);
}

void Stream::use(const LLVMDynamicInst *user) {
  auto userSeqNum = user->getSeqNum();
  // if (userSeqNum == 3804373) {
  //   panic("Panic for debug.\n");
  // }
  if (!this->isReady(user)) {
    STREAM_PANIC("User %lu tries to use stream when the we are not ready.",
                 userSeqNum);
  }
  auto *entry = this->findCorrectUsedEntry(userSeqNum);
  STREAM_ENTRY_DPRINTF(*entry, "Used by %lu.\n", userSeqNum);
  if (!entry->used) {
    // Update the stats.
    entry->used = true;
    this->se->numElementsUsed++;
    if (this->isMemStream()) {
      this->se->numMemElementsUsed++;
    }
  }
}

bool Stream::canStep() const {
  for (const auto &entry : this->FIFO) {
    if (entry.stepSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      return true;
    }
  }
  return false;
}

bool Stream::checkIfEntryBaseValuesValid(const FIFOEntry &entry) const {
  const auto myLoopLevel = this->getLoopLevel();
  const auto myConfigLoopLevel = this->getConfigLoopLevel();
  for (const auto &baseStream : this->baseStreams) {
    // So far we only check the base streams that have the same loop_level and
    // configure_level.
    const auto baseLoopLevel = baseStream->getLoopLevel();
    const auto baseConfigLoopLevel = baseStream->getConfigLoopLevel();
    if (baseConfigLoopLevel != myConfigLoopLevel ||
        baseLoopLevel != myLoopLevel) {
      continue;
    }

    // If the perfect aligned stream doesn't have step inst, it is a constant
    // stream. We simply assume it's ready now.
    if (baseStream->baseStepRootStreams.empty()) {
      continue;
    }

    // If we are here, that means our FIFO is perfectly aligned.
    bool foundAlignedBaseEntry = false;
    for (const auto &baseEntry : baseStream->FIFO) {
      if (baseEntry.idx == entry.idx) {
        // We found the correct base entry to use.
        if (!baseEntry.isValueValid) {
          return false;
        }
        foundAlignedBaseEntry = true;
        break;
      }
      if (baseEntry.idx.streamInstance > entry.idx.streamInstance) {
        // The base stream is already configured into the next instance.
        // We will soon be configured and flushed. Simply return not ready.
        return false;
      }
    }
    if (!foundAlignedBaseEntry) {
      STREAM_ENTRY_PANIC(entry,
                         "Failed to find the aligned base entry from the "
                         "perfectly aligned base stream %s.\n",
                         baseStream->getStreamName().c_str());
    }
  }
  return true;
}

void Stream::triggerReady(Stream *rootStream, const FIFOEntryIdx &entryId) {
  for (auto &dependentStream : this->dependentStreams) {
    STREAM_DPRINTF("Trigger ready entry (%lu, %lu) root %s stream %s.\n",
                   entryId.streamInstance, entryId.entryIdx,
                   rootStream->getStreamName().c_str(),
                   dependentStream->getStreamName().c_str());
    dependentStream->receiveReady(rootStream, this, entryId);
  }
}

void Stream::receiveReady(Stream *rootStream, Stream *baseStream,
                          const FIFOEntryIdx &entryId) {
  if (this->baseStreams.count(baseStream) == 0) {
    STREAM_PANIC("Received ready signal from illegal base stream.");
  }
  if (rootStream == this) {
    STREAM_PANIC("Dependence cycle detected.");
  }
  STREAM_DPRINTF("Received ready signal for entry (%lu, %lu) from stream %s.\n",
                 entryId.streamInstance, entryId.entryIdx,
                 baseStream->getStreamName().c_str());
  // Here we simply do an thorough search for our current entries.
  for (auto &entry : this->FIFO) {
    if (entry.isAddressValid) {
      // This entry already has a valid address.
      continue;
    }
    if (this->checkIfEntryBaseValuesValid(entry)) {
      // We are finally ready.
      this->markAddressReady(entry);
      this->triggerReady(rootStream, entry.idx);
    }
  }
}

void Stream::step(StreamStepInst *inst) {
  auto stepSeqNum = inst->getSeqNum();
  this->step(stepSeqNum);
}

void Stream::step(uint64_t stepSeqNum) {
  if (!this->isStepRoot()) {
    STREAM_PANIC("Receive step signal from nowhere for a non-root stream.");
  }
  this->stepImpl(stepSeqNum);
  // Send out the step signal as the root stream.
  this->triggerStep(stepSeqNum, this);
}

void Stream::triggerStep(uint64_t stepSeqNum, Stream *rootStream) {
  if (!this->isStepRoot()) {
    STREAM_PANIC("Trigger step signal from a non-root stream.");
  }
  for (auto &dependentStepStream : this->stepStreamList) {
    STREAM_DPRINTF("Trigger step for stream %s.\n",
                   dependentStepStream->getStreamName().c_str());
    dependentStepStream->stepImpl(stepSeqNum);
  }
}

void Stream::stepImpl(uint64_t stepSeqNum) {
  if (this->FIFO.empty()) {
    STREAM_PANIC("Step when the fifo is empty for stream %s.",
                 this->getStreamName().c_str());
  }
  if (stepSeqNum == 31686 || stepSeqNum == 31644) {
    STREAM_HACK("Step for %lu step inst.", stepSeqNum);
    STREAM_HACK("Config seq num %lu.\n", this->configSeqNum);
    this->dump();
  }
  for (auto &entry : this->FIFO) {
    if (entry.stepSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      entry.step(stepSeqNum);
      STREAM_ENTRY_DPRINTF(entry, "Stepped with seqNum %lu.\n", stepSeqNum);
      if (stepSeqNum == 31686 || stepSeqNum == 31644) {
        STREAM_HACK("After step for %lu step inst.", stepSeqNum);
        this->dump();
      }
      return;
    }
  }
  if (!this->baseStepRootStreams.empty()) {
    auto stepRootStream = *(this->baseStepRootStreams.begin());
    stepRootStream->dump();
  }
  STREAM_PANIC("Failed to find available entry to step for %lu.", stepSeqNum);
}

void Stream::commitStep(StreamStepInst *inst) {
  auto stepSeqNum = inst->getSeqNum();
  if (!this->isStepRoot()) {
    STREAM_PANIC(
        "Receive commit step signal from nowhere for a non-root stream");
  }
  this->commitStepImpl(stepSeqNum);
  // Send out the step signal as the root stream.
  this->triggerCommitStep(stepSeqNum, this);
}

void Stream::triggerCommitStep(uint64_t stepSeqNum, Stream *rootStream) {
  if (!this->isStepRoot()) {
    STREAM_PANIC("Trigger commit step signal from a non-root stream");
  }
  for (auto &dependentStepStream : this->stepStreamList) {
    STREAM_DPRINTF("Trigger commit step seqNum %lu for stream %s.\n",
                   stepSeqNum, dependentStepStream->getStreamName().c_str());
    dependentStepStream->commitStepImpl(stepSeqNum);
  }
}

void Stream::commitStepImpl(uint64_t stepSeqNum) {
  if (this->FIFO.empty()) {
    STREAM_PANIC("Commit step when the fifo is empty for stream %s.",
                 this->getStreamName().c_str());
  }
  auto &entry = this->FIFO.front();
  STREAM_ENTRY_DPRINTF(entry, "Commit stepped with seqNum %lu.\n", stepSeqNum);
  if (stepSeqNum < entry.idx.configSeqNum) {
    STREAM_ENTRY_DPRINTF(entry,
                         "Ignore step signal before our configuration.\n");
    return;
  }
  if (entry.stepSeqNum != stepSeqNum) {
    STREAM_ENTRY_PANIC(entry, "Unmatched stepSeqNum for entry %lu with %lu.",
                       entry.stepSeqNum, stepSeqNum);
  }
  // Check for late fetch signal.
  if (entry.used && entry.firstCheckIfReadyCycles < entry.valueReadyCycles) {
    this->throttleLate();
  }

  // Release the userToEntryMap.
  for (const auto &user : entry.users) {
    this->userToEntryMap.erase(user);
  }
  if (entry.isValueValid) {
    for (int i = 0; i < entry.cacheBlocks; ++i) {
      this->removeAliveCacheBlock(entry.cacheBlockAddrs[i]);
    }
  }
  this->FIFO.pop_front();

  while (this->FIFO.size() < this->runAHeadLength) {
    this->enqueueFIFO();
  }
}

void Stream::commitUser(const LLVMDynamicInst *inst) {
  auto userSeqNum = inst->getSeqNum();
  // So far just do a thorough search.
  for (const auto &entry : this->FIFO) {
    entry.users.erase(userSeqNum);
  }
  this->userToEntryMap.erase(userSeqNum);
}

void Stream::StreamMemAccess::handlePacketResponse(LLVMTraceCPU *cpu,
                                                   PacketPtr packet) {
  if (this->additionalDelay == 0) {
    this->stream->handlePacketResponse(this->entryId, packet, this);
  } else {
    // Schedule the event and clear the result.
    Cycles delay(this->additionalDelay);
    // Remember to clear this additional delay as we have already paid it.
    this->additionalDelay = 0;
    auto responseEvent = new ResponseEvent(cpu, this, packet);
    cpu->schedule(responseEvent, cpu->clockEdge(delay));
  }
}

void Stream::StreamMemAccess::handlePacketResponse(PacketPtr packet) {
  if (this->additionalDelay == 0) {
    this->stream->handlePacketResponse(this->entryId, packet, this);
  } else {
    // Schedule the event and clear the result.
    auto cpu = this->stream->getCPU();
    Cycles delay(this->additionalDelay);
    // Remember to clear this additional delay as we have already paid it.
    this->additionalDelay = 0;
    auto responseEvent = new ResponseEvent(cpu, this, packet);
    cpu->schedule(responseEvent, cpu->clockEdge(delay));
  }
}

void Stream::FIFOEntry::markAddressReady(Cycles readyCycles) {
  this->isAddressValid = true;
  this->addressReadyCycles = readyCycles;
}

void Stream::FIFOEntry::markValueReady(Cycles readyCycles) {
  if (this->inflyLoadPackets > 0) {
    panic("Mark entry value valid when there is still infly load packets.");
  }
  this->isValueValid = true;
  this->valueReadyCycles = readyCycles;
}

void Stream::FIFOEntry::store(uint64_t storeSeqNum) {
  if (this->storeSeqNum != LLVMDynamicInst::INVALID_SEQ_NUM) {
    panic("This entry (%lu, %lu) has already been stored before.",
          this->idx.streamInstance, this->idx.entryIdx);
  }
  this->storeSeqNum = storeSeqNum;
}

void Stream::throttleLate() {
  if (this->throttling != "late") {
    return;
  }
  this->lateFetchCount++;
  if (this->lateFetchCount == 10) {
    // Check if we still have room to increase.
    inform("Late fetche! %d %d", this->se->currentTotalRunAheadLength,
           this->se->maxTotalRunAheadLength);
    if (this->se->currentTotalRunAheadLength <
        this->se->maxTotalRunAheadLength) {
      // Step by 2.
      this->updateRunAHeadLength(this->runAHeadLength + 2);
      // Clear the late FetchCount
      this->lateFetchCount = 0;
    }
  }
}
Stream::FIFOEntry::FIFOEntry(const FIFOEntryIdx &_idx, const bool _oracleUsed,
                             const uint64_t _address, uint64_t _size,
                             const uint64_t _prevSeqNum)
    : idx(_idx),
      oracleUsed(_oracleUsed),
      address(_address),
      size(_size),
      cacheBlocks(0),
      isAddressValid(false),
      isValueValid(false),
      used(false),
      inflyLoadPackets(0),
      prevSeqNum(_prevSeqNum),
      stepSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
      storeSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM) {
  // Initialize the cache blocks it touched.
  constexpr int cacheBlockSize = 64;
  auto lhsCacheBlock = this->address & (~(cacheBlockSize - 1));
  auto rhsCacheBlock =
      (this->address + this->size - 1) & (~(cacheBlockSize - 1));
  while (lhsCacheBlock <= rhsCacheBlock) {
    if (this->cacheBlocks >= FIFOEntry::MAX_CACHE_BLOCKS) {
      panic(
          "More than %d cache blocks for one stream element, address %lu "
          "size %lu.",
          this->cacheBlocks, this->address, this->size);
    }
    this->cacheBlockAddrs[this->cacheBlocks] = lhsCacheBlock;
    this->cacheBlocks++;
    if (lhsCacheBlock == 0xFFFFFFFFFFFFFFC0) {
      // This is the last block in the address space.
      // Something wrong here.
      break;
    }
    lhsCacheBlock += cacheBlockSize;
  }
  // if (this->cacheBlocks > 1) {
  //   inform("addr %x size %x lhs %x rhs %x blocks %d.\n", this->address,
  //          this->size, this->address & (~(cacheBlockSize - 1)),
  //          rhsCacheBlock, this->cacheBlocks);
  // }
}

void Stream::FIFOEntry::step(uint64_t stepSeqNum) {
  if (this->stepSeqNum != LLVMDynamicInst::INVALID_SEQ_NUM) {
    panic("This entry (%lu, %lu) has already been stepped before.",
          this->idx.streamInstance, this->idx.entryIdx);
  }
  this->stepSeqNum = stepSeqNum;
}

void Stream::FIFOEntry::dump() const {
  std::stringstream ss;
  for (const auto &user : this->users) {
    ss << user << ' ';
  }
  inform("entry (%lu, %lu) step %lu address %d value %d users %s\n",
         this->idx.streamInstance, this->idx.entryIdx, this->stepSeqNum,
         this->isAddressValid, this->isValueValid, ss.str());
}