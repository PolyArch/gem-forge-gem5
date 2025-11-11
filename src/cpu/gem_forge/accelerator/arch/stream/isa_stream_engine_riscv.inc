// RISCV-specific implementation for ISAStreamEngine
// This file is included by isa_stream_engine.cc when THE_ISA == RISCV_ISA

#include "arch/riscv/insts/standard.hh"

template <typename T>
T ISAStreamEngine::extractImm(const StaticInst *staticInst) const {
  auto immOp = dynamic_cast<const RiscvISA::ImmOp<T> *>(staticInst);
  assert(immOp && "Invalid ImmOp.");
  return immOp->getImm();
}
