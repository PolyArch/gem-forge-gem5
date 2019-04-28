#include "cpu/gem_forge/llvm_insts.hh"
#include "cpu/gem_forge/llvm_static_insts.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"

#include "debug/LLVMTraceCPU.hh"

std::unordered_map<std::string, LLVMInstInfo> LLVMDynamicInst::instInfo = {
    // Binary operator.
    {"add", {.opClass = IntAluOp, .numOperands = 2, .numResults = 1}},
    {"fadd", {.opClass = FloatAddOp, .numOperands = 2, .numResults = 1}},
    {"sub", {.opClass = IntAluOp, .numOperands = 2, .numResults = 1}},
    {"fsub", {.opClass = FloatAddOp, .numOperands = 2, .numResults = 1}},
    {"mul", {.opClass = IntMultOp, .numOperands = 2, .numResults = 1}},
    {"fmul", {.opClass = FloatMultOp, .numOperands = 2, .numResults = 1}},
    {"udiv", {.opClass = IntDivOp, .numOperands = 2, .numResults = 1}},
    {"sdiv", {.opClass = IntDivOp, .numOperands = 2, .numResults = 1}},
    {"fdiv", {.opClass = FloatDivOp, .numOperands = 2, .numResults = 1}},
    {"urem", {.opClass = IntDivOp, .numOperands = 2, .numResults = 1}},
    {"srem", {.opClass = IntDivOp, .numOperands = 2, .numResults = 1}},
    {"frem", {.opClass = FloatDivOp, .numOperands = 2, .numResults = 1}},
    // Bitwise binary operator.
    {"shl", {.opClass = IntAluOp, .numOperands = 2, .numResults = 1}},
    {"lshr", {.opClass = IntAluOp, .numOperands = 2, .numResults = 1}},
    {"ashr", {.opClass = IntAluOp, .numOperands = 2, .numResults = 1}},
    {"and", {.opClass = IntAluOp, .numOperands = 2, .numResults = 1}},
    {"or", {.opClass = IntAluOp, .numOperands = 2, .numResults = 1}},
    {"xor", {.opClass = IntAluOp, .numOperands = 2, .numResults = 1}},
    // Conversion operator.
    // Truncation requires no FU.
    {"trunc",
     {.opClass = Enums::No_OpClass, .numOperands = 1, .numResults = 1}},
    {"zext", {.opClass = IntAluOp, .numOperands = 1, .numResults = 1}},
    {"sext", {.opClass = IntAluOp, .numOperands = 1, .numResults = 1}},
    {"fptrunc", {.opClass = FloatCvtOp, .numOperands = 1, .numResults = 1}},
    {"fpext", {.opClass = FloatCvtOp, .numOperands = 1, .numResults = 1}},
    {"fptoui", {.opClass = FloatCvtOp, .numOperands = 1, .numResults = 1}},
    {"fptosi", {.opClass = FloatCvtOp, .numOperands = 1, .numResults = 1}},
    {"uitofp", {.opClass = FloatCvtOp, .numOperands = 1, .numResults = 1}},
    {"sitofp", {.opClass = FloatCvtOp, .numOperands = 1, .numResults = 1}},
    {"ptrtoint",
     {.opClass = Enums::No_OpClass, .numOperands = 1, .numResults = 1}},
    {"inttoptr",
     {.opClass = Enums::No_OpClass, .numOperands = 1, .numResults = 1}},
    {"bitcast",
     {.opClass = Enums::No_OpClass, .numOperands = 1, .numResults = 1}},
    // Other insts.
    {"icmp", {.opClass = IntAluOp, .numOperands = 2, .numResults = 1}},
    {"fcmp", {.opClass = FloatCmpOp, .numOperands = 2, .numResults = 1}},
    // We assume the branching requires address computation.
    {"br", {.opClass = IntAluOp, .numOperands = 1, .numResults = 0}},
    // Our special accelerator inst.
    {"cca",
     {.opClass = Enums::OpClass::Accelerator,
      .numOperands = 2,
      .numResults = 1}},
    {"load",
     {.opClass = Enums::OpClass::MemRead, .numOperands = 1, .numResults = 1}},
    {"store",
     {.opClass = Enums::OpClass::MemWrite, .numOperands = 2, .numResults = 0}},
};

uint64_t LLVMDynamicInst::currentSeqNum = 0;
uint64_t LLVMDynamicInst::allocateSeqNum() {
  // 0 is reserved for invalid seq num.
  return ++LLVMDynamicInst::currentSeqNum;
}

uint64_t LLVMDynamicInst::getDynamicNextPC() const {
  if (!this->isBranchInst()) {
    panic("getDynamicNextPC called on non conditional branch instructions.");
  }
  return this->TDG.branch().dynamic_next_pc();
}

uint64_t LLVMDynamicInst::getStaticNextPC() const {
  if (!this->isBranchInst()) {
    panic("getStaticNextPC called on non conditional branch instructions.");
  }
  return this->TDG.branch().static_next_pc();
}

StaticInstPtr LLVMDynamicInst::getStaticInst() const {
  if (!this->isBranchInst()) {
    panic("getStaticInst called on non conditional branch instructions.");
  }
  return StaticInstPtr(new LLVMStaticInst(this));
}

bool LLVMDynamicInst::isBranchInst() const { return this->TDG.has_branch(); }

bool LLVMDynamicInst::isStoreInst() const {
  const auto &op = this->getInstName();
  return op == "store" || op == "memset";
}

bool LLVMDynamicInst::isLoadInst() const {
  return this->getInstName() == "load";
}

bool LLVMDynamicInst::isDependenceReady(LLVMTraceCPU *cpu) const {

  bool hasStreamUse = false;
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::REGISTER) {
      if (!cpu->isInstFinished(dep.dependent_id())) {
        return false;
      }
    }
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::MEMORY) {
      if (!cpu->isInstFinished(dep.dependent_id())) {
        return false;
      }
    }
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      hasStreamUse = true;
      break;
    }
  }
  if (hasStreamUse) {
    auto SE = cpu->getAcceleratorManager()->getStreamEngine();
    if (!SE->areUsedStreamsReady(this)) {
      return false;
    }
  }

  return true;
}

void LLVMDynamicInst::dispatch(LLVMTraceCPU *cpu) {
  // Inform the the stream engine.
  bool hasStreamUse = false;
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      hasStreamUse = true;
      break;
    }
  }
  if (hasStreamUse) {
    auto SE = cpu->getAcceleratorManager()->getStreamEngine();
    SE->dispatchStreamUser(this);
  }
}

void LLVMDynamicInst::commit(LLVMTraceCPU *cpu) {
  // Default implementation will commit stream users, if any.
  bool hasStreamUse = false;
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      hasStreamUse = true;
      break;
    }
  }
  if (hasStreamUse) {
    auto SE = cpu->getAcceleratorManager()->getStreamEngine();
    SE->commitStreamUser(this);
  }
}

void LLVMDynamicInst::dumpBasic() const {
  inform("Inst seq %lu, id %lu, op %s.\n", this->seqNum, this->getId(),
         this->getInstName().c_str());
}

void LLVMDynamicInst::dumpDeps(LLVMTraceCPU *cpu) const {
  this->dumpBasic();
  inform("Reg Deps Begin ===========================\n");
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() != ::LLVM::TDG::TDGInstructionDependence::REGISTER) {
      continue;
    }
    auto dependentInstId = dep.dependent_id();
    auto depInst = cpu->getInflyInstNullable(dependentInstId);
    if (depInst == nullptr) {
      inform("Dep id %lu nullptr\n", dependentInstId);
    } else {
      depInst->dumpBasic();
    }
  }
  inform("Mem Deps Begin ===========================\n");
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() != ::LLVM::TDG::TDGInstructionDependence::MEMORY) {
      continue;
    }
    auto dependentInstId = dep.dependent_id();
    auto depInst = cpu->getInflyInstNullable(dependentInstId);
    if (depInst == nullptr) {
      inform("Dep id %lu nullptr\n", dependentInstId);
    } else {
      depInst->dumpBasic();
    }
  }
  inform("Deps End   ===========================\n");
}

// For now just return IntAlu.
OpClass LLVMDynamicInst::getOpClass() const {
  auto iter = LLVMDynamicInst::instInfo.find(this->getInstName());
  if (iter == LLVMDynamicInst::instInfo.end()) {
    // For unknown, simply return IntAlu.
    return No_OpClass;
  }
  return iter->second.opClass;
}

int LLVMDynamicInst::getNumOperands() const {
  auto iter = LLVMDynamicInst::instInfo.find(this->getInstName());
  if (iter == LLVMDynamicInst::instInfo.end()) {
    // For unknown, simply return 2.
    return 2;
  }
  return iter->second.numOperands;
}

int LLVMDynamicInst::getNumResults() const {
  auto iter = LLVMDynamicInst::instInfo.find(this->getInstName());
  if (iter == LLVMDynamicInst::instInfo.end()) {
    // For unknown, simply return 1.
    return 1;
  }
  return iter->second.numResults;
}

bool LLVMDynamicInst::isFloatInst() const {
  switch (this->getOpClass()) {
  case FloatAddOp:
  case FloatMultOp:
  case FloatMultAccOp:
  case FloatDivOp:
  case FloatCvtOp:
  case FloatCmpOp: {
    return true;
  }
  default: { return false; }
  }
}

bool LLVMDynamicInst::isCallInst() const {
  return this->getInstName() == "call" || this->getInstName() == "invoke";
}

bool LLVMDynamicInst::canWriteBack(LLVMTraceCPU *cpu) const { return true; }

LLVMDynamicInstMem::LLVMDynamicInstMem(const LLVM::TDG::TDGInstruction &_TDG,
                                       uint8_t _numMicroOps, Addr _align,
                                       Type _type)
    : LLVMDynamicInst(_TDG, _numMicroOps), align(_align), type(_type),
      value(nullptr), loadStartCycle(0), loadEndCycle(0) {
  if (this->type == ALLOCA) {
    if (!this->TDG.has_alloc()) {
      panic("Alloc without extra alloc information from TDG.");
    }
  } else if (this->type == LOAD) {
    panic_if(!this->TDG.has_load(), "Load without extra information from TDG.");
  } else if (this->type == STORE) {
    panic_if(!this->TDG.has_store(),
             "Store without extra information from TDG.");
    const auto &storeExtra = this->TDG.store();
    // Get the stored value.
    // Special case for memset: transform into a huge store.
    // TODO: handle this more gracefully.
    this->value = new uint8_t[storeExtra.size()];
    if (this->getInstName() == "store") {
      if (storeExtra.size() != storeExtra.value().size()) {
        panic("Unmatched stored value size, size %ul, value.size %ul.\n",
              storeExtra.size(), storeExtra.value().size());
      }
      memcpy(value, storeExtra.value().data(), storeExtra.value().size());
    } else if (this->getInstName() == "memset") {
      if (storeExtra.value().size() != 1) {
        panic("Memset with value.size %ul.\n", storeExtra.value().size());
      }
      memset(value, storeExtra.value().front(), storeExtra.size());
    }
  }
}

LLVMDynamicInstMem::~LLVMDynamicInstMem() {
  if (this->value != nullptr) {
    delete this->value;
    this->value = nullptr;
  }
}

void LLVMDynamicInstMem::execute(LLVMTraceCPU *cpu) {
  // Notify the stream engine.
  bool hasStreamUse = false;
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      hasStreamUse = true;
      break;
    }
  }
  if (hasStreamUse) {
    auto SE = cpu->getAcceleratorManager()->getStreamEngine();
    SE->executeStreamUser(this);
  }

  switch (this->type) {
  case Type::ALLOCA: {
    // We need to handle stack allocation only
    // when we have a driver.
    if (!cpu->isStandalone()) {
      if (this->TDG.alloc().new_base() == "") {
        panic("Alloc with empty new base for integrated mode.\n");
      }
      Addr vaddr = cpu->allocateStack(this->TDG.alloc().size(), this->align);
      // Set up the mapping.
      cpu->mapBaseNameToVAddr(this->TDG.alloc().new_base(), vaddr);
    }
    break;
  }
  case Type::STORE: {
    // Just construct the packets.
    // Only sent these packets at writeback.
    this->constructPackets(cpu);
    break;
  }
  case Type::LOAD: {
    this->constructPackets(cpu);
    this->loadStartCycle = cpu->curCycle();
    for (const auto &packet : this->packets) {
      auto pkt = TDGPacketHandler::createTDGPacket(
          packet.paddr, packet.size, this, packet.data, cpu->getDataMasterID(),
          0, this->TDG.pc());
      cpu->sendRequest(pkt);
      DPRINTF(LLVMTraceCPU, "Send request paddr %p size %u for inst %d\n",
              reinterpret_cast<void *>(packet.paddr), packet.size,
              this->getId());
    }
    break;
  }

  default: {
    panic("Unknown LLVMDynamicInstMem type %u\n", this->type);
    break;
  }
  }
}

void LLVMDynamicInstMem::constructPackets(LLVMTraceCPU *cpu) {
  if (this->type != Type::STORE && this->type != Type::LOAD) {
    panic("Calling writeback on non-store/load inst %u.\n", this->getId());
  }

  uint64_t size;
  if (this->type == Type::STORE) {
    size = this->TDG.store().size();
  } else {
    size = this->TDG.load().size();
  }

  for (int packetSize, inflyPacketsSize = 0; inflyPacketsSize < size;
       inflyPacketsSize += packetSize) {
    Addr paddr, vaddr;
    if (cpu->isStandalone()) {
      // When in stand alone mode, use the trace space address
      // directly as the virtual address.
      // No address translation.
      if (this->type == Type::STORE) {
        vaddr = this->TDG.store().addr() + inflyPacketsSize;
      } else {
        vaddr = this->TDG.load().addr() + inflyPacketsSize;
      }
      paddr = cpu->translateAndAllocatePhysMem(vaddr);
    } else {
      // When we have a driver, we have to translate trace space
      // address into simulation space and then use the process
      // page table to get physical address.
      if (this->type == Type::STORE) {
        vaddr = cpu->getVAddrFromBase(this->TDG.store().base()) +
                this->TDG.store().offset() + inflyPacketsSize;
      } else {
        vaddr = cpu->getVAddrFromBase(this->TDG.load().base()) +
                this->TDG.load().offset() + inflyPacketsSize;
      }
      paddr = cpu->getPAddrFromVaddr(vaddr);
      DPRINTF(LLVMTraceCPU,
              "vaddr %llu, infly %llu, paddr %llu, "
              "reintered %p\n",
              vaddr, inflyPacketsSize, paddr, reinterpret_cast<void *>(paddr));
    }
    // For now only support maximum 8 bytes access.
    packetSize = size - inflyPacketsSize;
    if (packetSize > 8) {
      packetSize = 8;
    }
    // Do not span across cache line.
    auto cacheLineSize = cpu->system->cacheLineSize();
    if (((paddr % cacheLineSize) + packetSize) > cacheLineSize) {
      packetSize = cacheLineSize - (paddr % cacheLineSize);
    }

    // Construct the packet.
    if (this->type == Type::LOAD) {
      this->packets.emplace_back(paddr, packetSize, nullptr);
    } else {
      this->packets.emplace_back(paddr, packetSize,
                                 this->value + inflyPacketsSize);
    }

    DPRINTF(LLVMTraceCPU,
            "Construct request %d vaddr %p paddr %p size %u for inst %d\n",
            this->packets.size(), reinterpret_cast<void *>(vaddr),
            reinterpret_cast<void *>(paddr), packetSize, this->getId());
  }
}

void LLVMDynamicInstMem::writeback(LLVMTraceCPU *cpu) {
  if (this->type != Type::STORE) {
    panic("Calling writeback on non-store inst %u.\n", this->getId());
  }
  // Start to send the packets to cpu for store.
  for (const auto &packet : this->packets) {
    if (packet.size == 8) {
      DPRINTF(LLVMTraceCPU, "Store data %f for inst %u to paddr %p\n",
              *(double *)(packet.data), this->getId(), packet.paddr);
    }
    auto pkt = TDGPacketHandler::createTDGPacket(
        packet.paddr, packet.size, this, packet.data, cpu->getDataMasterID(), 0,
        this->TDG.pc());
    cpu->sendRequest(pkt);
  }
}

bool LLVMDynamicInstMem::isCompleted() const {
  if (this->type != Type::STORE) {
    return this->packets.size() == 0 && this->remainingMicroOps == 0;
  } else {
    // For store, the infly packets doesn't control completeness.
    return this->remainingMicroOps == 0;
  }
}

bool LLVMDynamicInstMem::isWritebacked() const {
  if (this->type != Type::STORE) {
    panic("Calling isWritebacked on non-store inst %u.\n", this->getId());
  }
  return this->packets.size() == 0;
}

void LLVMDynamicInstMem::dumpBasic() const {
  inform("Inst seq %lu, id %lu, op %s, infly pkts %d.\n", this->seqNum,
         this->getId(), this->getInstName().c_str(), this->packets.size());
}

void LLVMDynamicInstMem::handlePacketResponse(LLVMTraceCPU *cpu,
                                              PacketPtr packet) {
  if (this->type != Type::STORE && this->type != Type::LOAD) {
    panic("LLVMDynamicInstMem::handlePacketResponse called for non store/load "
          "inst %d, but type %d.\n",
          this->getId(), this->type);
  }

  // Check if the load will produce a new base.
  if (this->type == Type::LOAD && this->TDG.load().new_base() != "") {
    uint64_t vaddr = packet->get<uint64_t>();
    cpu->mapBaseNameToVAddr(this->TDG.load().new_base(), vaddr);
  }

  // We don't care about which packet for now, just mark one completed.
  this->packets.pop_front();
  DPRINTF(LLVMTraceCPU, "Get response for inst %u, remain infly packets %d\n",
          this->getId(), this->packets.size());

  // Check if we completed.
  if (this->type == Type::LOAD && this->isCompleted()) {
    assert(this->loadEndCycle == 0 && "We already complemte.");
    this->loadEndCycle = cpu->curCycle();
    cpu->getRunTimeProfiler()->profileLoadLatency(
        this->getTDG().pc(), this->loadEndCycle - this->loadStartCycle);
  }
  // Remember to release the packet.
  delete packet->req;
  delete packet;
}

void LLVMDynamicInstCompute::execute(LLVMTraceCPU *cpu) {
  // Notify the stream engine.
  bool hasStreamUse = false;
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      hasStreamUse = true;
      break;
    }
  }
  if (hasStreamUse) {
    auto SE = cpu->getAcceleratorManager()->getStreamEngine();
    SE->executeStreamUser(this);
  }
  this->fuLatency = cpu->getOpLatency(this->getOpClass());
  /**
   * Hack here: for branching instructions, add one more cycle of latency.
   * As gem5 generally translates branch to multiple microops like:
   * rdip
   * wrip
   */
  // if (this->isBranchInst()) {
  //   ++this->fuLatency;
  // }
}
