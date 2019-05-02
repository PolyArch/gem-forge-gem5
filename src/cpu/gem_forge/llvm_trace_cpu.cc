#include "llvm_trace_cpu.hh"

#include "base/loader/object_file.hh"
#include "cpu/thread_context.hh"
#include "debug/LLVMTraceCPU.hh"
#include "sim/process.hh"
#include "sim/sim_exit.hh"

LLVMTraceCPU::LLVMTraceCPU(LLVMTraceCPUParams *params)
    : BaseCPU(params), cpuParams(params),
      pageTable(params->name + ".page_table", 0),
      instPort(params->name + ".inst_port", this),
      dataPort(params->name + ".data_port", this),
      traceFileName(params->traceFile), totalCPUs(params->totalCPUs),
      itb(params->itb), dtb(params->dtb), fuPool(params->fuPool),
      regionStats(nullptr), currentStackDepth(0), warmUpTick(0),
      process(nullptr), thread_context(nullptr), stackMin(0),
      fetchStage(params, this), decodeStage(params, this),
      renameStage(params, this), iewStage(params, this),
      commitStage(params, this), fetchToDecode(5, 5), decodeToRename(5, 5),
      renameToIEW(5, 5), iewToCommit(5, 5), signalBuffer(5, 5),
      driver(params->driver), tickEvent(*this) {
  DPRINTF(LLVMTraceCPU, "LLVMTraceCPU constructed\n");

  assert(this->numThreads < LLVMTraceCPUConstants::MaxContexts &&
         "Number of thread context exceed the maximum.");

  // Set the trace folder.
  auto slashPos = this->traceFileName.rfind('/');
  if (slashPos == std::string::npos) {
    this->traceFolder = "";
  } else {
    this->traceFolder = this->traceFileName.substr(0, slashPos);
  }
  this->traceExtraFolder = this->traceFileName + ".extra";

  // Set the time buffer between stages.
  this->fetchStage.setToDecode(&this->fetchToDecode);
  this->decodeStage.setFromFetch(&this->fetchToDecode);
  this->decodeStage.setToRename(&this->decodeToRename);
  this->renameStage.setFromDecode(&this->decodeToRename);
  this->renameStage.setToIEW(&this->renameToIEW);
  this->iewStage.setFromRename(&this->renameToIEW);
  this->iewStage.setToCommit(&this->iewToCommit);
  this->commitStage.setFromIEW(&this->iewToCommit);

  this->commitStage.setSignal(&this->signalBuffer, 0);
  this->iewStage.setSignal(&this->signalBuffer, -1);
  this->renameStage.setSignal(&this->signalBuffer, -2);
  this->decodeStage.setSignal(&this->signalBuffer, -3);
  this->fetchStage.setSignal(&this->signalBuffer, -4);

  // Initialize the hardware contexts.
  this->activeThreads.resize(params->hardwareContexts, nullptr);

  // Initialize the main thread.
  {
    auto mainThreadId = LLVMTraceCPU::allocateContextID();
    this->mainThread =
        new LLVMTraceThreadContext(mainThreadId, this->traceFileName);
    this->activateThread(mainThread);
  }

  // Initialize the accelerators.
  // We need to keep the params as the sim object will store its address.
  this->accelManagerParams = new TDGAcceleratorManagerParams();
  accelManagerParams->name = this->name() + ".accs";
  this->accelManager = accelManagerParams->create();

  DPRINTF(LLVMTraceCPU, "Accelerator manager name %s.\n",
          this->accelManager->name().c_str());

  // Initialize the region stats from the main thread.
  const auto &staticInfo = this->mainThread->getStaticInfo();
  RegionStats::RegionMap regions;
  for (const auto &region : staticInfo.regions()) {
    const auto &regionId = region.name();
    DPRINTF(LLVMTraceCPU, "Found region %s.\n", regionId.c_str());
    if (regions.find(regionId) != regions.end()) {
      panic("Multiple defined region %s.\n", regionId.c_str());
    }

    auto &regionStruct =
        regions.emplace(regionId, RegionStats::Region()).first->second;
    regionStruct.name = regionId;
    regionStruct.parent = region.parent();
    for (const auto &bb : region.bbs()) {
      regionStruct.bbs.insert(bb);
    }
  }
  this->regionStats = new RegionStats(std::move(regions), "region.stats.txt");

  this->runTimeProfiler = new RunTimeProfiler();

  if (driver != nullptr) {
    // Handshake with the driver.
    driver->handshake(this);
    // Add the dump handler to dump region stats at the end.
    Stats::registerDumpCallback(
        new MakeCallback<RegionStats, &RegionStats::dump>(this->regionStats,
                                                          true));
  } else {
    // No driver, stand alone mode.
    // Schedule the first event.
    // And remember to initialize the stack depth to 1.
    this->currentStackDepth = 1;
    DPRINTF(LLVMTraceCPU, "Schedule initial tick event.\n");
    schedule(this->tickEvent, nextCycle());
  }
}

LLVMTraceCPU::~LLVMTraceCPU() {
  delete this->accelManager;
  this->accelManager = nullptr;
  delete this->accelManagerParams;
  this->accelManagerParams = nullptr;
  if (this->regionStats != nullptr) {
    delete this->regionStats;
    this->regionStats = nullptr;
  }
  delete this->runTimeProfiler;
  this->runTimeProfiler = nullptr;
  delete this->mainThread;
  this->mainThread = nullptr;
}

LLVMTraceCPU *LLVMTraceCPUParams::create() { return new LLVMTraceCPU(this); }

void LLVMTraceCPU::init() {
  // This can only happen here.
  this->accelManager->handshake(this);
}

void LLVMTraceCPU::tick() {
  if (curTick() % 100000000 == 0) {
    DPRINTF(LLVMTraceCPU, "Tick()\n");
    this->iewStage.dumpROB();
    this->accelManager->dump();
  }

  // First time.
  if (this->numCycles.value() == 0 && this->isStandalone()) {
    // Warm up the cache.
    // AdHoc for the cache warm up file name.
    if (this->cpuParams->warmCache) {
      this->warmUpTick = this->warmUpCache(this->traceFileName + ".cache");
    } else {
      this->warmUpTick = this->curCycle();
    }
  }

  this->numCycles++;
  if (this->cyclesToTicks(this->curCycle()) < this->warmUpTick) {
    // Waiting for warm up.
    schedule(this->tickEvent, nextCycle());
    return;
  }

  // Unblock the memory instructions.
  if (!this->dataPort.isBlocked()) {
    this->iewStage.unblockMemoryInsts();
  }

  this->fetchStage.tick();
  this->decodeStage.tick();
  this->renameStage.tick();
  this->iewStage.tick();
  this->accelManager->tick();
  this->commitStage.tick();

  // Send the packets.
  this->dataPort.sendReq();

  this->fetchToDecode.advance();
  this->decodeToRename.advance();
  this->renameToIEW.advance();
  this->iewToCommit.advance();
  this->signalBuffer.advance();

  // Exit condition.
  // 1. In standalone mode, we will exit when there is no infly instructions and
  //    the loaded instruction list is empty.
  // 2. In integrated mode, we will exit when there is no infly instructions and
  //    the stack depth is 0.
  bool done = false;
  if (this->isStandalone()) {
    if (this->cpuParams->max_insts_any_thread > 0) {
      auto &committedInsts = this->commitStage.instsCommitted;
      for (int i = 0; i < committedInsts.size(); ++i) {
        if (committedInsts[i].value() > this->cpuParams->max_insts_any_thread) {
          done = true;
          break;
        }
      }
    } else {
      done = (this->getNumActivateThreads() == 0);
      if (done) {
        assert(this->inflyInstStatus.empty() &&
               "Infly instruction status map is not empty when done.");
      }
    }
  } else {
    done = this->inflyInstStatus.empty() && this->currentStackDepth == 0;
  }
  if (done) {
    DPRINTF(LLVMTraceCPU, "We have no inst left to be scheduled.\n");
    // Wraps up the region stats by sending in the invalid bb.
    this->regionStats->update(RegionStats::InvalidBB);
    // If in standalone mode, we can exit.
    if (this->isStandalone()) {
      // Decrease the workitem count.
      auto workItemsEnd = this->system->incWorkItemsEnd();
      if (workItemsEnd == this->totalCPUs) {
        this->regionStats->dump();
        this->runTimeProfiler->dump("profile.txt");
        this->accelManager->exitDump();
        exitSimLoop("All datagraphs finished.\n");
      } else {
        DPRINTF(LLVMTraceCPU, "CPU %d done.\n", this->_cpuId);
      }

    } else {
      DPRINTF(LLVMTraceCPU, "Activate the normal CPU\n");
      this->thread_context->activate();
    }
    // Do not schedule next tick.
    return;
  } else {
    // Schedule next Tick event.
    schedule(this->tickEvent, nextCycle());
  }

  this->numPendingAccessDist.sample(this->dataPort.getPendingPacketsNum());

  this->numPendingAccessDist.sample(this->dataPort.getPendingPacketsNum());
}

Tick LLVMTraceCPU::warmUpCache(const std::string &fileName) {
  if (!this->isStandalone()) {
    // Only warm up cache in standalone mode.
    return 0;
  }

  std::ifstream cacheFile(fileName);
  if (!cacheFile.is_open()) {
    panic("Failed to open cache warm up file %s.\n", fileName.c_str());
  }
  // PortProxy proxy(this->dataPort, 64);
  Addr vaddr;
  constexpr auto size = 4;
  uint8_t data[size];

  Tick warmUpTick = 0;

  while (cacheFile >> std::hex >> vaddr) {
    auto paddr = this->translateAndAllocatePhysMem(vaddr);
    // proxy.readBlob(paddr, data, 4);

    int contextId = 0;
    RequestPtr req = new Request(paddr, size, 0, this->_dataMasterId,
                                 static_cast<InstSeqNum>(0), contextId);
    PacketPtr pkt;
    pkt = Packet::createRead(req);
    pkt->dataStatic(data);
    warmUpTick = std::max(warmUpTick, this->dataPort.sendAtomic(pkt));

    delete pkt->req;
    delete pkt;
  }
  cacheFile.close();

  return warmUpTick;
}

bool LLVMTraceCPU::handleTimingResp(PacketPtr pkt) {
  // Receive the response from port.
  TDGPacketHandler::handleTDGPacketResponse(this, pkt);
  return true;
}

bool LLVMTraceCPU::CPUPort::recvTimingResp(PacketPtr pkt) {
  if (this->inflyNumPackets == 0) {
    panic("Received timing response when there is no infly packets.");
  }
  this->inflyNumPackets--;
  return this->owner->handleTimingResp(pkt);
}

void LLVMTraceCPU::CPUPort::addReq(PacketPtr pkt) {
  DPRINTF(LLVMTraceCPU, "Add pkt at %p\n", pkt);
  this->blockedPacketPtrs.push(pkt);
  // Try to send request.
  this->sendReq();
}

bool LLVMTraceCPU::CPUPort::isBlocked() const {
  return this->blocked || (!this->blockedPacketPtrs.empty());
}

void LLVMTraceCPU::CPUPort::sendReq() {
  /**
   * At this level, we do not distinguish the load/store ports,
   * but only enfore the limit on the total number of ports.
   */
  unsigned usedPorts = 0;
  const auto cpuParams = this->owner->getLLVMTraceCPUParams();
  unsigned totalPorts = cpuParams->cacheLoadPorts + cpuParams->cacheStorePorts;
  while (!this->blocked && !this->blockedPacketPtrs.empty() &&
         usedPorts < totalPorts && this->inflyNumPackets < 80) {
    PacketPtr pkt = this->blockedPacketPtrs.front();
    DPRINTF(LLVMTraceCPU, "Try sending pkt at %p\n", pkt);
    bool success = MasterPort::sendTimingReq(pkt);
    if (!success) {
      DPRINTF(LLVMTraceCPU, "Blocked packet ptr %p\n", pkt);
      this->blocked = true;
    } else {
      this->inflyNumPackets++;
      this->blockedPacketPtrs.pop();
      usedPorts++;
    }
  }
}

void LLVMTraceCPU::CPUPort::recvReqRetry() {
  // std::lock_guard<std::mutex> guard(this->blockedPacketPtrsMutex);
  if (!this->blocked) {
    panic("Should be in blocked state when recvReqRetry is called\n");
  }
  // Unblock myself.
  this->blocked = false;
  // Keep retry until failed or blocked is empty.
  this->sendReq();
}

void LLVMTraceCPU::handleReplay(
    Process *p, ThreadContext *tc, const std::string &trace,
    const Addr finish_tag_vaddr,
    std::vector<std::pair<std::string, Addr>> maps) {
  panic_if(this->isStandalone(), "handleReplay called in standalone mode.");

  DPRINTF(LLVMTraceCPU, "Replay trace %s, finish tag at 0x%x, num maps %u\n",
          trace.c_str(), finish_tag_vaddr, maps.size());

  // Map base to vaddr.
  for (const auto &pair : maps) {
    this->mapBaseNameToVAddr(pair.first, pair.second);
  }

  // Set the process and tc.
  this->process = p;
  this->thread_context = tc;

  // Load the global symbols for global variables.
  this->process->objFile->loadAllSymbols(&this->symbol_table);

  // Get the bottom of the stack.
  this->stackMin = tc->readIntReg(TheISA::StackPointerReg);

  // Allocate a special stack slot for register spill.
  Addr spill = this->allocateStack(8, 8);
  this->mapBaseNameToVAddr("$sp", spill);

  // Suspend the thread from normal CPU.
  this->thread_context->suspend();
  DPRINTF(LLVMTraceCPU, "Suspend thread, status = %d\n",
          this->thread_context->status());

  if (!this->process->pTable->translate(finish_tag_vaddr,
                                        this->finish_tag_paddr)) {
    panic("Failed translating finish_tag_vaddr %p to paddr\n",
          reinterpret_cast<void *>(finish_tag_vaddr));
  }

  // Update the stack depth to 1.
  if (this->currentStackDepth != 0) {
    panic("Before replay the stack depth must be 0, now %u\n",
          this->currentStackDepth);
  }
  this->stackPush();

  // Schedule the next event.
  schedule(this->tickEvent, nextCycle());
}

LLVMDynamicInst *LLVMTraceCPU::getInflyInst(LLVMDynamicInstId id) {
  auto iter = this->inflyInstMap.find(id);
  panic_if(iter == this->inflyInstMap.end(), "Failed to find infly inst %u.\n",
           id);
  return iter->second;
}

LLVMDynamicInst *LLVMTraceCPU::getInflyInstNullable(LLVMDynamicInstId id) {
  auto iter = this->inflyInstMap.find(id);
  if (iter == this->inflyInstMap.end()) {
    return nullptr;
  }
  return iter->second;
}

void LLVMTraceCPU::stackPush() {
  // Ignore the stack adjustment if we are in standalone mode.
  if (this->isStandalone()) {
    return;
  }
  this->currentStackDepth++;
  this->framePointerStack.push_back(this->stackMin);
}

void LLVMTraceCPU::stackPop() {
  if (this->isStandalone()) {
    return;
  }
  this->currentStackDepth--;
  if (this->currentStackDepth < 0) {
    panic("Current stack depth is less than 0\n");
  }
  this->stackMin = this->framePointerStack.back();
  this->framePointerStack.pop_back();
}

Addr LLVMTraceCPU::allocateStack(Addr size, Addr align) {
  // We need to handle stack allocation only
  // when we have a driver.
  if (this->isStandalone()) {
    panic("LLVMTraceCPU::allocateStack called in standalone mode.\n");
  }
  // Allocate the stack starting from stackMin.
  // Note that since we are not acutall modifying the
  // stack pointer in the thread context, there is no
  // clean up necessary when we leaving this function.
  // Compute the bottom of the new stack.
  // Remember to round down to align.
  Addr bottom = roundDown(this->stackMin - size, align);
  // Try to map the bottom to see if there is already
  // a physical page for it.
  Addr paddr;
  if (!this->process->pTable->translate(bottom, paddr)) {
    // We need to allocate more page for the stack.
    if (!this->process->fixupStackFault(bottom)) {
      panic("Failed to allocate stack until %ull\n", bottom);
    }
  }
  // Update the stackMin.
  this->stackMin = bottom;
  return bottom;
}

Addr LLVMTraceCPU::translateAndAllocatePhysMem(Addr vaddr) {
  if (!this->isStandalone()) {
    panic("translateAndAllocatePhysMem called in non standalone mode.\n");
  }

  if (!this->pageTable.translate(vaddr)) {
    // Handle the page fault.
    Addr pageBytes = TheISA::PageBytes;
    Addr startVaddr = this->pageTable.pageAlign(vaddr);
    Addr startPaddr = this->system->allocPhysPages(1);
    this->pageTable.map(startVaddr, startPaddr, pageBytes, PageTableBase::Zero);
    DPRINTF(LLVMTraceCPU, "Map vaddr 0x%x to paddr 0x%x\n", startVaddr,
            startPaddr);
  }
  Addr paddr;
  if (!this->pageTable.translate(vaddr, paddr)) {
    panic("Failed to translate vaddr at 0x%x\n", vaddr);
  }
  DPRINTF(LLVMTraceCPU, "Translate vaddr 0x%x to paddr 0x%x\n", vaddr, paddr);
  return paddr;
}

void LLVMTraceCPU::mapBaseNameToVAddr(const std::string &base, Addr vaddr) {
  DPRINTF(LLVMTraceCPU, "map base %s to vaddr %p.\n", base.c_str(),
          reinterpret_cast<void *>(vaddr));
  this->mapBaseToVAddr[base] = vaddr;
}

Addr LLVMTraceCPU::getVAddrFromBase(const std::string &base) {
  if (this->mapBaseToVAddr.find(base) != this->mapBaseToVAddr.end()) {
    return this->mapBaseToVAddr.at(base);
  }
  // Try to look at the global symbol table of the process.
  Addr vaddr;
  if (this->symbol_table.findAddress(base, vaddr)) {
    return vaddr;
  }
  panic("Failed to look up base %s\n", base.c_str());
}

// Translate from vaddr to paddr.
Addr LLVMTraceCPU::getPAddrFromVaddr(Addr vaddr) {
  // Translate from vaddr to paddr.
  Addr paddr;
  if (!this->process->pTable->translate(vaddr, paddr)) {
    // Something goes wrong. The simulation process should
    // allocate this address.
    panic("Failed translating vaddr %p to paddr\n",
          reinterpret_cast<void *>(vaddr));
  }
  return paddr;
}

void LLVMTraceCPU::sendRequest(PacketPtr pkt) {
  // int contextId = 0;
  // if (!this->isStandalone()) {
  //   contextId = this->thread_context->contextId();
  // }
  this->dataPort.addReq(pkt);
}

Cycles LLVMTraceCPU::getOpLatency(OpClass opClass) {
  if (opClass == No_OpClass) {
    return Cycles(1);
  }
  return this->fuPool->getOpLatency(opClass);
}

void LLVMTraceCPU::regStats() {
  BaseCPU::regStats();

  this->fetchStage.regStats();
  this->decodeStage.regStats();
  this->renameStage.regStats();
  this->iewStage.regStats();
  this->commitStage.regStats();

  DPRINTF(LLVMTraceCPU, "Accelerator manager name %s.\n",
          this->accelManager->name().c_str());
  this->accelManager->regStats();

  this->numPendingAccessDist.init(0, 4, 1)
      .name(this->name() + ".pending_acc_per_cycle")
      .desc("Number of pending memory access each cycle")
      .flags(Stats::pdf);
}

ContextID LLVMTraceCPU::allocateContextID() {
  static ContextID contextId = 0;
  return contextId++;
}

void LLVMTraceCPU::activateThread(LLVMTraceThreadContext *thread) {
  auto freeContextId = -1;
  for (auto idx = 0; idx < this->activeThreads.size(); ++idx) {
    if (this->activeThreads[idx] == nullptr) {
      freeContextId = idx;
      break;
    }
  }
  assert(freeContextId != -1 &&
         "Failed to find free hardware context to activate thread.");
  this->activeThreads[freeContextId] = thread;
  thread->activate(this, freeContextId);
}

void LLVMTraceCPU::deactivateThread(LLVMTraceThreadContext *thread) {
  auto threadId = thread->getThreadId();
  assert(threadId >= 0 && threadId < this->activeThreads.size() &&
         "Invalid context id.");
  assert(this->activeThreads[threadId] == thread &&
         "Unmatched thread at the context.");
  thread->deactivate();
  this->activeThreads[threadId] = nullptr;
  this->fetchStage.clearThread(threadId);
  this->decodeStage.clearThread(threadId);
  this->renameStage.clearThread(threadId);
  this->iewStage.clearThread(threadId);
  this->commitStage.clearThread(threadId);
}

size_t LLVMTraceCPU::getNumActivateThreads() const {
  size_t activeThreads = 0;
  for (const auto &thread : this->activeThreads) {
    if (thread != nullptr) {
      activeThreads++;
    }
  }
  return activeThreads;
}