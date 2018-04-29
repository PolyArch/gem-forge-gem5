#include <fstream>

#include "cpu/thread_context.hh"
#include "debug/LLVMTraceCPU.hh"
#include "llvm_trace_cpu.hh"
#include "sim/process.hh"

LLVMTraceCPU::LLVMTraceCPU(LLVMTraceCPUParams* params)
    : BaseCPU(params),
      pageTable(params->name + ".page_table", 0),
      instPort(params->name + ".inst_port", this),
      dataPort(params->name + ".data_port", this),
      traceFile(params->traceFile),
      itb(params->itb),
      dtb(params->dtb),
      currentStackDepth(0),
      currentInstId(0),
      process(nullptr),
      thread_context(nullptr),
      stackMin(0),
      fetchStage(params, this),
      decodeStage(params, this),
      renameStage(params, this),
      iewStage(params, this),
      commitStage(params, this),
      fetchToDecode(5, 5),
      decodeToRename(5, 5),
      renameToIEW(5, 5),
      iewToCommit(5, 5),
      signalBuffer(5, 5),
      driver(params->driver),
      tickEvent(*this) {
  DPRINTF(LLVMTraceCPU, "LLVMTraceCPU constructed\n");
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

  readTraceFile();
  if (driver != nullptr) {
    // Handshake with the driver.
    driver->handshake(this);
  } else {
    // No driver, stand alone mode.
    // Schedule the first event.
    schedule(this->tickEvent, nextCycle());
  }
}

LLVMTraceCPU::~LLVMTraceCPU() {}

LLVMTraceCPU* LLVMTraceCPUParams::create() { return new LLVMTraceCPU(this); }

void LLVMTraceCPU::readTraceFile() {
  std::ifstream stream(this->traceFile);
  if (!stream.is_open()) {
    fatal("Failed opening trace file %s\n", this->traceFile.c_str());
  }
  for (std::string line; std::getline(stream, line);) {
    DPRINTF(LLVMTraceCPU, "read in %s\n", line.c_str());

    auto inst = parseLLVMDynamicInst(this->dynamicInsts.size(), line);
    this->dynamicInsts.push_back(inst);

    DPRINTF(LLVMTraceCPU, "Parsed #%u dynamic inst with %s\n",
            this->dynamicInsts.size(),
            this->dynamicInsts.back()->toLine().c_str());
  }
  DPRINTF(LLVMTraceCPU, "Parsed total number of inst: %u\n",
          this->dynamicInsts.size());
}

void LLVMTraceCPU::tick() {
  if (curTick() % 100000000 == 0) {
    DPRINTF(LLVMTraceCPU, "Tick()\n");
  }

  this->numCycles++;

  this->fetchStage.tick();
  this->decodeStage.tick();
  this->renameStage.tick();
  this->iewStage.tick();
  this->commitStage.tick();

  this->fetchToDecode.advance();
  this->decodeToRename.advance();
  this->renameToIEW.advance();
  this->iewToCommit.advance();
  this->signalBuffer.advance();

  if (this->inflyInsts.empty() && this->currentStackDepth == 0) {
    DPRINTF(LLVMTraceCPU, "We have no inst left to be scheduled.\n");
    // No more need to write to the finish tag, simply restart the
    // normal thread.
    // Check the stack depth should be 0.
    if (this->currentStackDepth != 0) {
      panic("The stack depth should be 0 when we finished a replay, but %u\n",
            this->currentStackDepth);
    }

    DPRINTF(LLVMTraceCPU, "Activate the normal CPU\n");
    this->thread_context->activate();
    return;
  } else {
    // Schedule next Tick event.
    schedule(this->tickEvent, nextCycle());
  }

  this->numPendingAccessDist.sample(this->dataPort.getPendingPacketsNum());
}

bool LLVMTraceCPU::handleTimingResp(PacketPtr pkt) {
  // Receive the response from port.
  LLVMDynamicInstId instId =
      static_cast<LLVMDynamicInstId>(pkt->req->getReqInstSeqNum());
  if (instId < this->dynamicInsts.size()) {
    this->dynamicInsts[instId]->handlePacketResponse();
  } else {
    panic("Invalid instId %u, max instId %u\n", instId,
          this->dynamicInsts.size());
  }
  // Release the memory.
  delete pkt->req;
  delete pkt;

  return true;
}

void LLVMTraceCPU::CPUPort::sendReq(PacketPtr pkt) {
  // If there is already blocked req, just push to the queue.
  std::lock_guard<std::mutex> guard(this->blockedPacketPtrsMutex);
  if (this->blocked) {
    // We are blocked, push to the queue.
    DPRINTF(LLVMTraceCPU, "Already blocked, queue packet ptr %p\n", pkt);
    this->blockedPacketPtrs.push(pkt);
    return;
  }
  // Push to blocked ptrs if need retry, and set blocked flag.
  bool success = MasterPort::sendTimingReq(pkt);
  if (!success) {
    DPRINTF(LLVMTraceCPU, "Blocked packet ptr %p\n", pkt);
    this->blocked = true;
    this->blockedPacketPtrs.push(pkt);
  }
}

void LLVMTraceCPU::CPUPort::recvReqRetry() {
  std::lock_guard<std::mutex> guard(this->blockedPacketPtrsMutex);
  if (!this->blocked) {
    panic("Should be in blocked state when recvReqRetry is called\n");
  }
  // Unblock myself.
  this->blocked = false;
  // Keep retry until failed or blocked is empty.
  while (!this->blockedPacketPtrs.empty() && !this->blocked) {
    // If we have blocked packet, send again.
    PacketPtr pkt = this->blockedPacketPtrs.front();
    bool success = MasterPort::sendTimingReq(pkt);
    if (success) {
      DPRINTF(LLVMTraceCPU, "Retry blocked packet ptr %p: Succeed\n", pkt);
      this->blockedPacketPtrs.pop();
    } else {
      DPRINTF(LLVMTraceCPU, "Retry blocked packet ptr %p: Failed\n", pkt);
      this->blocked = true;
    }
  }
}

void LLVMTraceCPU::handleReplay(
    Process* p, ThreadContext* tc, const std::string& trace,
    const Addr finish_tag_vaddr,
    std::vector<std::pair<std::string, Addr>> maps) {
  DPRINTF(LLVMTraceCPU, "Replay trace %s, finish tag at 0x%x, num maps %u\n",
          trace.c_str(), finish_tag_vaddr, maps.size());

  // Map base to vaddr.
  for (const auto& pair : maps) {
    this->mapBaseNameToVAddr(pair.first, pair.second);
  }

  // Set the process and tc.
  this->process = p;
  this->thread_context = tc;

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
          reinterpret_cast<void*>(finish_tag_vaddr));
  }

  // Update the stack depth to 1.
  if (this->currentStackDepth != 0) {
    panic("Before replay the stack depth must be 0, now %u\n",
          this->currentStackDepth);
  }
  this->currentStackDepth = 1;

  // Schedule the next event.
  schedule(this->tickEvent, nextCycle());
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

void LLVMTraceCPU::mapBaseNameToVAddr(const std::string& base, Addr vaddr) {
  DPRINTF(LLVMTraceCPU, "map base %s to vaddr %p.\n", base.c_str(),
          reinterpret_cast<void*>(vaddr));
  this->mapBaseToVAddr[base] = vaddr;
}

// Translate from vaddr to paddr.
Addr LLVMTraceCPU::getPAddrFromVaddr(Addr vaddr) {
  // Translate from vaddr to paddr.
  Addr paddr;
  if (!this->process->pTable->translate(vaddr, paddr)) {
    // Something goes wrong. The simulation process should
    // allocate this address.
    panic("Failed translating vaddr %p to paddr\n",
          reinterpret_cast<void*>(vaddr));
  }
  return paddr;
}

void LLVMTraceCPU::sendRequest(Addr paddr, int size, LLVMDynamicInstId instId,
                               uint8_t* data) {
  RequestPtr req = new Request(paddr, size, 0, this->_dataMasterId, instId,
                               this->thread_context->contextId());
  PacketPtr pkt;
  uint8_t* pkt_data = new uint8_t[req->getSize()];
  if (data == nullptr) {
    pkt = Packet::createRead(req);
  } else {
    pkt = Packet::createWrite(req);
    // Copy the value to store.
    memcpy(pkt_data, data, req->getSize());
  }
  pkt->dataDynamic(pkt_data);
  this->dataPort.sendReq(pkt);
}

void LLVMTraceCPU::regStats() {
  BaseCPU::regStats();

  this->fetchStage.regStats();
  this->decodeStage.regStats();
  this->renameStage.regStats();
  this->iewStage.regStats();
  this->commitStage.regStats();

  this->numPendingAccessDist.init(0, 64, 2)
      .name(this->name() + ".pending_acc_per_cycle")
      .desc("Number of pending memory access each cycle")
      .flags(Stats::pdf);
}