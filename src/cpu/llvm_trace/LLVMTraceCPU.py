from m5.params import *

from BaseCPU import BaseCPU
from FuncUnit import *
from FuncUnitConfig import *
from FUPool import FUPool
from Process import EmulatedDriver

class LLVMAccel(FUDesc):
    opList = [OpDesc(opClass='Accelerator', opLat=1, pipelined=False)]
    count = 2


class LLVMIntMultDiv(FUDesc):
    opList = [OpDesc(opClass='IntMult', opLat=5),
              OpDesc(opClass='IntDiv', opLat=12, pipelined=False)]
    count = 2


class DefaultFUPool(FUPool):
    FUList = [IntALU(), LLVMIntMultDiv(), FP_ALU(), FP_MultDiv(), ReadPort(),
              SIMD_Unit(), WritePort(), RdWrPort(), IprPort(), LLVMAccel()]


class LLVMTraceCPU(BaseCPU):
    type = 'LLVMTraceCPU'
    cxx_header = 'cpu/llvm_trace/llvm_trace_cpu.hh'

    traceFile = Param.String('', 'The input llvm trace file.')
    driver = Param.LLVMTraceCPUDriver('The driver to control this cpu.')
    maxFetchQueueSize = Param.UInt64(32, 'Maximum size of the fetch queue.')
    maxReorderBufferSize = Param.UInt64(8, 'Maximum size of the rob.')
    maxInstructionQueueSize = Param.UInt64(
        8, 'Maximum size of the instruction queue.')
    loadStoreQueueSize = Param.UInt64(
        8, 'Maximum number of load store queue.')

    fetchToDecodeDelay = Param.Cycles(1, "Fetch to decode delay")
    fetchWidth = Param.Unsigned(8, "Fetch width")
    decodeToRenameDelay = Param.Cycles(1, "Decode to rename delay")
    decodeWidth = Param.Unsigned(8, "Decode width")
    decodeQueueSize = Param.Unsigned(32, "Decode queue size")
    renameToIEWDelay = Param.Cycles(
        2, "Rename to Issue/Execute/Writeback delay")
    renameWidth = Param.Unsigned(8, "Rename width")
    renameBufferSize = Param.Unsigned(32, "Rename buffer size")
    robSize = Param.Unsigned(192, "ROB size")
    iewToCommitDelay = Param.Cycles(
        1, "Issue/Execute/Writeback to commit delay")
    dispatchWidth = Param.Unsigned(8, "dispatch width")
    issueWidth = Param.Unsigned(8, "Issue width")
    writeBackWidth = Param.Unsigned(8, "Write back width")
    instQueueSize = Param.Unsigned(32, "Inst queue size")
    loadQueueSize = Param.Unsigned(32, "Load queue size")
    storeQueueSize = Param.Unsigned(32, "Store queue size")
    commitWidth = Param.Unsigned(8, "Commit width")
    commitQueueSize = Param.Unsigned(32, "Commit queue size")

    fuPool = Param.FUPool(DefaultFUPool(), "Functional Unit pool")

    @classmethod
    def memory_mode(cls):
        return 'timing'

    @classmethod
    def require_caches(cls):
        return False

    @classmethod
    def support_take_over(cls):
        return False


class LLVMTraceCPUDriver(EmulatedDriver):
    type = 'LLVMTraceCPUDriver'
    cxx_header = 'cpu/llvm_trace/llvm_trace_cpu_driver.hh'