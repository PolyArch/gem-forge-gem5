#include "x86_func_addr_callback.hh"

#include "../func_addr_exec_context.hh"
#include "arch/x86/decoder.hh"
#include "arch/x86/insts/macroop.hh"
#include "base/loader/object_file.hh"
#include "base/loader/symtab.hh"
#include "cpu/exec_context.hh"
#include "debug/FuncAddrCallback.hh"
#include "sim/process.hh"

#define FUNC_ADDR_DPRINTF(format, args...)                                     \
  DPRINTF(FuncAddrCallback, "[%s]: " format, this->func.name().c_str(), ##args)

namespace {

/**
 * Since gem5 is single thread, and all the address computation is not
 * overlapped, we use a static global context.
 */
static AddrFuncExecContext addrFuncXC;

} // namespace

namespace X86ISA {

FuncAddrGenCallback::FuncAddrGenCallback(ThreadContext *_tc,
                                         const ::LLVM::TDG::AddrFuncInfo &_func)
    : tc(_tc), func(_func) {
  auto p = tc->getProcessPtr();
  auto obj = p->objFile;
  SymbolTable table;
  obj->loadAllSymbols(&table);
  Addr funcStartVAddr;
  assert(table.findAddress(this->func.name(), funcStartVAddr));
  FUNC_ADDR_DPRINTF("======= Start decoding from %#x.\n", funcStartVAddr);

  auto &prox = this->tc->getVirtProxy();

  /**
   * Let's create a new decoder to avoid interfering the original one's state.
   */
  TheISA::Decoder decoder;
  decoder.takeOverFrom(this->tc->getDecoderPtr());
  decoder.reset();
  TheISA::PCState pc(funcStartVAddr);
  auto fetchPC = funcStartVAddr;
  // Feed in the first line.
  MachInst machInst;
  assert(prox.tryReadBlob(fetchPC, &machInst, sizeof(machInst)) &&
         "Failed to read in next machine inst.");

  while (true) {
    /**
     * Although wierd, this is used to feed in data to the decoder,
     * even if it's from previous line.
     */
    FUNC_ADDR_DPRINTF("Feed in %#x %#x %#x %#x %#x %#x %#x %#x %#x.\n", fetchPC,
                      (machInst >> 0) & 0xff, (machInst >> 8) & 0xff,
                      (machInst >> 16) & 0xff, (machInst >> 24) & 0xff,
                      (machInst >> 32) & 0xff, (machInst >> 40) & 0xff,
                      (machInst >> 48) & 0xff, (machInst >> 56) & 0xff);
    decoder.moreBytes(pc, fetchPC, machInst);
    // Read in the next machInst.
    if (decoder.needMoreBytes()) {
      fetchPC += sizeof(machInst);
      assert(prox.tryReadBlob(fetchPC, &machInst, sizeof(machInst)) &&
             "Failed to read in next machine inst.");
    }
    if (!decoder.instReady()) {
      FUNC_ADDR_DPRINTF("Feed in %#x %#x %#x %#x %#x %#x %#x %#x %#x.\n",
                        fetchPC, (machInst >> 0) & 0xff, (machInst >> 8) & 0xff,
                        (machInst >> 16) & 0xff, (machInst >> 24) & 0xff,
                        (machInst >> 32) & 0xff, (machInst >> 40) & 0xff,
                        (machInst >> 48) & 0xff, (machInst >> 56) & 0xff);
      decoder.moreBytes(pc, fetchPC, machInst);
    }
    assert(decoder.instReady() && "Decoder should have the inst ready.");
    auto staticInst = decoder.decode(pc);
    assert(staticInst && "Failed to decode inst.");

    // We assume there is no branch.
    FUNC_ADDR_DPRINTF("Decode MacroInst %s.\n",
                      staticInst->disassemble(pc.pc()).c_str());

    auto macroop = dynamic_cast<MacroopBase *>(staticInst.get());
    if (staticInst->getName() == "ret") {
      break;
    }

    assert(!staticInst->isControl() &&
           "No control instruction allowed in address function.");
    auto numMicroops = macroop->getNumMicroops();
    for (auto upc = 0; upc < numMicroops; ++upc) {
      auto microop = staticInst->fetchMicroop(upc);
      FUNC_ADDR_DPRINTF("  Decode MicroInst %s.\n",
                        microop->disassemble(pc.pc()).c_str());
      this->instructions.push_back(microop);
    }

    // Advance to the next pc.
    pc.advance();
    FUNC_ADDR_DPRINTF("Next pc %#x.\n", pc.pc());
  }
  FUNC_ADDR_DPRINTF("Decode done.\n", pc.pc());
}

uint64_t FuncAddrGenCallback::genAddr(uint64_t idx,
                                      const std::vector<uint64_t> &params) {
  /**
   * We are assuming C calling convention.
   * Registers are passed in as $rdi, $rsi, $rdx, $rcx, $r8, $r9.
   * The address function should never use stack.
   */
  assert(params.size() <= 6 && "Too many arguments for address function.");

  const InstRegIndex regParams[6] = {
      InstRegIndex(IntRegIndex::INTREG_RDI),
      InstRegIndex(IntRegIndex::INTREG_RSI),
      InstRegIndex(IntRegIndex::INTREG_RDX),
      InstRegIndex(IntRegIndex::INTREG_RCX),
      InstRegIndex(IntRegIndex::INTREG_R8),
      InstRegIndex(IntRegIndex::INTREG_R9),
  };
  FUNC_ADDR_DPRINTF("Set up calling convention.\n");
  for (auto idx = 0; idx < params.size(); ++idx) {
    auto param = params.at(idx);
    const auto &reg = regParams[idx];
    addrFuncXC.setIntRegOperand(reg, param);
    FUNC_ADDR_DPRINTF("Arg %d Reg %s %llu.\n", idx, reg, param);
  }

  for (auto &staticInst : this->instructions) {
    staticInst->execute(&addrFuncXC, nullptr /* traceData. */);
  }

  // We expect the result in rax.
  const InstRegIndex rax(IntRegIndex::INTREG_RAX);
  auto retAddr = addrFuncXC.readIntRegOperand(rax);
  FUNC_ADDR_DPRINTF("Ret %#x.\n", retAddr);
  return retAddr;
}
} // namespace X86ISA