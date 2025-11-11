// X86-specific implementation for ISAStreamEngine  
// This file is included by isa_stream_engine.cc when THE_ISA == X86_ISA

template <typename T>
T ISAStreamEngine::extractImm(const StaticInst *staticInst) const {
  auto *xsi = static_cast<const X86ISA::X86StaticInst *>(staticInst);
  return xsi->machInst.immediate;
}
