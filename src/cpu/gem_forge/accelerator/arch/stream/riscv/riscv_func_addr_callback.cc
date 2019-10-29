#include "riscv_func_addr_callback.hh"

#include "arch/riscv/decoder.hh"
#include "base/loader/object_file.hh"
#include "base/loader/symtab.hh"
#include "cpu/exec_context.hh"
#include "debug/FuncAddrCallback.hh"
#include "sim/process.hh"

#define FUNC_ADDR_DPRINTF(format, args...)                               \
  DPRINTF(FuncAddrCallback, "[%s]: " format, this->func.name().c_str(),   \
          ##args)

namespace {
/**
 * A taylored ExecContext that only provides integer register file,
 * for the address computation.
 */
class AddrFuncExecContext : public ExecContext {
public:
  /** Reads an integer register. */
  RegVal readIntRegOperand(const StaticInst *si, int idx) override {
    return this->readIntRegOperand(si->srcRegIdx(idx));
  }

  /** Sets an integer register to a value. */
  void setIntRegOperand(const StaticInst *si, int idx, RegVal val) override {
    this->setIntRegOperand(si->destRegIdx(idx), val);
  }

  /** Directly read/set the integer register, used to pass in arguments. **/
  RegVal readIntRegOperand(const RegId &reg) {
    assert(reg.isIntReg());
    // For RISCV, this is directly flattened.
    return this->intRegs[reg.index()];
  }

  void setIntRegOperand(const RegId &reg, RegVal val) {
    assert(reg.isIntReg());
    this->intRegs[reg.index()] = val;
  }

  /** @} */

  /**
   * @{
   * @name Floating Point Register Interfaces
   */

  /** Reads a floating point register in its binary format, instead
   * of by value. */
  RegVal readFloatRegOperandBits(const StaticInst *si, int idx) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Sets the bits of a floating point register of single width
   * to a binary value. */
  void setFloatRegOperandBits(const StaticInst *si, int idx,
                              RegVal val) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** @} */

  /** Vector Register Interfaces. */
  /** @{ */
  /** Reads source vector register operand. */
  const VecRegContainer &readVecRegOperand(const StaticInst *si,
                                           int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Gets destination vector register operand for modification. */
  VecRegContainer &getWritableVecRegOperand(const StaticInst *si,
                                            int idx) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Sets a destination vector register operand to a value. */
  void setVecRegOperand(const StaticInst *si, int idx,
                        const VecRegContainer &val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  /** @} */

  /** Vector Register Lane Interfaces. */
  /** @{ */
  /** Reads source vector 8bit operand. */
  ConstVecLane8 readVec8BitLaneOperand(const StaticInst *si,
                                       int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Reads source vector 16bit operand. */
  ConstVecLane16 readVec16BitLaneOperand(const StaticInst *si,
                                         int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Reads source vector 32bit operand. */
  ConstVecLane32 readVec32BitLaneOperand(const StaticInst *si,
                                         int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Reads source vector 64bit operand. */
  ConstVecLane64 readVec64BitLaneOperand(const StaticInst *si,
                                         int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Write a lane of the destination vector operand. */
  /** @{ */
  void setVecLaneOperand(const StaticInst *si, int idx,
                         const LaneData<LaneSize::Byte> &val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void setVecLaneOperand(const StaticInst *si, int idx,
                         const LaneData<LaneSize::TwoByte> &val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void setVecLaneOperand(const StaticInst *si, int idx,
                         const LaneData<LaneSize::FourByte> &val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void setVecLaneOperand(const StaticInst *si, int idx,
                         const LaneData<LaneSize::EightByte> &val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  /** @} */

  /** Vector Elem Interfaces. */
  /** @{ */
  /** Reads an element of a vector register. */
  VecElem readVecElemOperand(const StaticInst *si, int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Sets a vector register to a value. */
  void setVecElemOperand(const StaticInst *si, int idx,
                         const VecElem val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  /** @} */

  /** Predicate registers interface. */
  /** @{ */
  /** Reads source predicate register operand. */
  const VecPredRegContainer &readVecPredRegOperand(const StaticInst *si,
                                                   int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Gets destination predicate register operand for modification. */
  VecPredRegContainer &getWritableVecPredRegOperand(const StaticInst *si,
                                                    int idx) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Sets a destination predicate register operand to a value. */
  void setVecPredRegOperand(const StaticInst *si, int idx,
                            const VecPredRegContainer &val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  /** @} */

  /**
   * @{
   * @name Condition Code Registers
   */
  RegVal readCCRegOperand(const StaticInst *si, int idx) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void setCCRegOperand(const StaticInst *si, int idx, RegVal val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  /** @} */

  /**
   * @{
   * @name Misc Register Interfaces
   */
  RegVal readMiscRegOperand(const StaticInst *si, int idx) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void setMiscRegOperand(const StaticInst *si, int idx, RegVal val) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /**
   * Reads a miscellaneous register, handling any architectural
   * side effects due to reading that register.
   */
  RegVal readMiscReg(int misc_reg) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /**
   * Sets a miscellaneous register, handling any architectural
   * side effects due to writing that register.
   */
  void setMiscReg(int misc_reg, RegVal val) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** @} */

  /**
   * @{
   * @name PC Control
   */
  PCState pcState() const override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void pcState(const PCState &val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  /** @} */

  /**
   * @{
   * @name Memory Interface
   */
  /**
   * Perform an atomic memory read operation.  Must be overridden
   * for exec contexts that support atomic memory mode.  Not pure
   * since exec contexts that only support timing memory
   * mode need not override (though in that case this function
   * should never be called).
   */
  Fault readMem(Addr addr, uint8_t *data, unsigned int size,
                Request::Flags flags,
                const std::vector<bool> &byteEnable = std::vector<bool>()) {
    panic("ExecContext::readMem() should be overridden\n");
  }

  /**
   * Initiate a timing memory read operation.  Must be overridden
   * for exec contexts that support timing memory mode.  Not pure
   * since exec contexts that only support atomic memory
   * mode need not override (though in that case this function
   * should never be called).
   */
  Fault
  initiateMemRead(Addr addr, unsigned int size, Request::Flags flags,
                  const std::vector<bool> &byteEnable = std::vector<bool>()) {
    panic("ExecContext::initiateMemRead() should be overridden\n");
  }

  /**
   * For atomic-mode contexts, perform an atomic memory write operation.
   * For timing-mode contexts, initiate a timing memory write operation.
   */
  Fault
  writeMem(uint8_t *data, unsigned int size, Addr addr, Request::Flags flags,
           uint64_t *res,
           const std::vector<bool> &byteEnable = std::vector<bool>()) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /**
   * For atomic-mode contexts, perform an atomic AMO (a.k.a., Atomic
   * Read-Modify-Write Memory Operation)
   */
  Fault amoMem(Addr addr, uint8_t *data, unsigned int size,
               Request::Flags flags, AtomicOpFunctor *amo_op) {
    panic("ExecContext::amoMem() should be overridden\n");
  }

  /**
   * For timing-mode contexts, initiate an atomic AMO (atomic
   * read-modify-write memory operation)
   */
  Fault initiateMemAMO(Addr addr, unsigned int size, Request::Flags flags,
                       AtomicOpFunctor *amo_op) {
    panic("ExecContext::initiateMemAMO() should be overridden\n");
  }

  /**
   * Sets the number of consecutive store conditional failures.
   */
  void setStCondFailures(unsigned int sc_failures) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /**
   * Returns the number of consecutive store conditional failures.
   */
  unsigned int readStCondFailures() const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** @} */

  /**
   * @{
   * @name SysCall Emulation Interfaces
   */

  /**
   * Executes a syscall specified by the callnum.
   */
  void syscall(int64_t callnum, Fault *fault) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** @} */

  /** Returns a pointer to the ThreadContext. */
  ThreadContext *tcBase() override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /**
   * @{
   * @name ARM-Specific Interfaces
   */

  bool readPredicate() const override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void setPredicate(bool val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  bool readMemAccPredicate() const override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void setMemAccPredicate(bool val) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** @} */

  /**
   * @{
   * @name X86-Specific Interfaces
   */

  /**
   * Invalidate a page in the DTLB <i>and</i> ITLB.
   */
  void demapPage(Addr vaddr, uint64_t asn) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void armMonitor(Addr address) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  bool mwait(PacketPtr pkt) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void mwaitAtomic(ThreadContext *tc) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  AddressMonitor *getAddrMonitor() override {
    panic("FuncAddrExecContext does not implement this.");
  }

protected:
  RegVal intRegs[TheISA::NumIntRegs];
};

/**
 * Since gem5 is single thread, and all the address computation is not
 * overlapped, we use a static global context.
 */
static AddrFuncExecContext addrFuncXC;

} // namespace

namespace RiscvISA {

FuncAddrGenCallback::FuncAddrGenCallback(ThreadContext *_tc,
                                         const ::LLVM::TDG::AddrFuncInfo &_func)
    : tc(_tc), func(_func), decoder(_tc->getDecoderPtr()), funcStartVAddr(0) {
  auto p = tc->getProcessPtr();
  auto obj = p->objFile;
  SymbolTable table;
  obj->loadAllSymbols(&table);
  assert(table.findAddress(this->func.name(), this->funcStartVAddr));
  FUNC_ADDR_DPRINTF("Start PC %#x.\n", this->funcStartVAddr);

  auto &prox = this->tc->getVirtProxy();
  auto pc = this->funcStartVAddr;

  while (true) {
    MachInst machInst;
    // Read the instructions.
    assert(prox.tryReadBlob(pc, &machInst, sizeof(machInst)) &&
           "Failed to read in instruction.");
    // Use the stateless decodeInst function.
    auto staticInst = this->decoder->decodeInst(machInst);

    if (staticInst->isCall()) {
      /**
       * Though wierd, RISCV jalr instruction is marked as
       * IsIndirectControl, IsUncondControl and IsCall.
       * We use IsCall to check if we are done.
       */
      break;
    }
    // We assume there is no branch.
    FUNC_ADDR_DPRINTF("Decode Inst %s.\n",
                            staticInst->disassemble(pc).c_str());
    assert(!staticInst->isControl() &&
           "No control instruction allowed in address function.");
    this->instructions.push_back(staticInst);
    pc += sizeof(machInst);
  }
}

uint64_t FuncAddrGenCallback::genAddr(uint64_t idx,
                                      const std::vector<uint64_t> &params) {
  /**
   * Prepare the arguments according to the calling convention.
   */
  // a0 starts at x10.
  const RegIndex a0RegIdx = 10;
  auto argIdx = a0RegIdx;
  for (auto param : params) {
    RegId reg(RegClass::IntRegClass, argIdx);
    addrFuncXC.setIntRegOperand(reg, param);
    FUNC_ADDR_DPRINTF("Arg %d %llu.\n", argIdx - a0RegIdx, param);
    argIdx++;
  }

  for (auto &staticInst : this->instructions) {
    staticInst->execute(&addrFuncXC, nullptr /* traceData. */);
  }

  // The result value should be in a0 = x10.
  RegId a0Reg(RegClass::IntRegClass, a0RegIdx);
  auto retAddr = addrFuncXC.readIntRegOperand(a0Reg);
  FUNC_ADDR_DPRINTF("Ret %llu.\n", retAddr);
  return retAddr;
}
} // namespace RiscvISA