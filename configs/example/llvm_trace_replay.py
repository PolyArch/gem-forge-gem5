import optparse
import os

from m5.util import addToPath, fatal

addToPath('../')

from common import Options
from common import Simulation
from common import CacheConfig
from common import MemConfig
from common.Caches import *

parser = optparse.OptionParser()
Options.addCommonOptions(parser)
Options.addSEOptions(parser)

def parse_tdg_files(option, opt, value, parser):
    vs = value.split(',')
    setattr(parser.values, option.dest, vs)

parser.add_option("--gem-forge-cold-cache", action="store_true",
                  help="""start simulation without warming up the cache.""", default=False)
parser.add_option("--llvm-standalone", action="store_true",
                  help="""replay in stand alone mode""", default=False)
parser.add_option("--llvm-prefetch", action="store", type="int",
                  help="""whether to use a prefetcher""", default="0")
parser.add_option("--llvm-trace-file", action="callback", type="string",
                  help="""llvm trace file input LLVMTraceCPU""", default="",
                  callback=parse_tdg_files)
parser.add_option("--llvm-issue-width", action="store", type="int",
                  help="""llvm issue width""", default="8")
parser.add_option("--llvm-store-queue-size", action="store",
                  type="int", help="""store queue size""", default="32")
parser.add_option("--llvm-load-queue-size", action="store",
                  type="int", help="""load queue size""", default="32")
parser.add_option("--gem-forge-cache-load-ports", action="store", type="int",
                  help="""How many loads can be issued in one cycle""", default="4")
parser.add_option("--gem-forge-cache-store-ports", action="store", type="int",
                  help="""How many stores can be written-back in one cycle""", default="4")
parser.add_option("--gem-forge-hardware-contexts-per-core", action="store", type="int",
                  help="""How many thread context""", default="1")

parser.add_option("--llvm-mcpat", action="store", type="int",
                  help="""whether to use mcpat to estimate power""", default="0")
parser.add_option("--gem-forge-stream-engine-max-run-ahead-length", action="store", type="int",
                  help="""How many element can a stream run ahead""", default="10")
parser.add_option("--gem-forge-stream-engine-is-oracle", action="store", type="int",
                  help="""whether make the stream engine oracle""", default="0")
parser.add_option("--gem-forge-stream-engine-throttling", action="store", type="string",
                  help="""Throttling tenchique used by stream engine.""", default="static")
parser.add_option("--gem-forge-stream-engine-enable-coalesce", action="store", type="int",
                  help="""Enable stream coalesce in the stream engine.""", default="0")
parser.add_option("--gem-forge-stream-engine-enable-merge", action="store", type="int",
                  help="""Enable stream merge in the stream engine.""", default="0")
parser.add_option("--gem-forge-stream-engine-l1d",
                  type="string", default="original")
parser.add_option("--gem-forge-adfa-core-issue-width", action="store", type="int", default="16")
parser.add_option("--gem-forge-adfa-enable-speculation",
                  action="store", type="int", default="0")
parser.add_option("--gem-forge-adfa-break-iv-dep",
                  action="store", type="int", default="0")
parser.add_option("--gem-forge-adfa-break-rv-dep",
                  action="store", type="int", default="0")
parser.add_option("--gem-forge-adfa-break-unrollable-ctr-dep",
                  action="store", type="int", default="0")
parser.add_option("--gem-forge-adfa-num-banks",
                  action="store", type="int", default="1")
parser.add_option("--gem-forge-adfa-num-ports-per-bank",
                  action="store", type="int", default="1")
parser.add_option("--gem-forge-adfa-num-cores",
                  action="store", type="int", default="1")
parser.add_option("--gem-forge-adfa-enable-tls",
                  action="store", type="int", default="0")
parser.add_option("--gem-forge-adfa-ideal-mem", action="store", type="int", default="0")

(options, args) = parser.parse_args()

if args:
    fatal("Error: script doesn't take any positional arguments")


def get_processes(options):
    """Interprets provided options and returns a list of processes"""

    multiprocesses = []
    inputs = []
    outputs = []
    errouts = []
    pargs = []

    workloads = options.cmd.split(';')
    if options.input != "":
        inputs = options.input.split(';')
    if options.output != "":
        outputs = options.output.split(';')
    if options.errout != "":
        errouts = options.errout.split(';')
    if options.options != "":
        pargs = options.options.split(';')

    idx = 0
    for wrkld in workloads:
        process = Process()
        process.executable = wrkld
        process.cwd = os.getcwd()

        if len(pargs) > idx:
            process.cmd = [wrkld] + pargs[idx].split()
        else:
            process.cmd = [wrkld]

        if len(inputs) > idx:
            process.input = inputs[idx]
        if len(outputs) > idx:
            process.output = outputs[idx]
        if len(errouts) > idx:
            process.errout = errouts[idx]

        multiprocesses.append(process)
        idx += 1

    if options.smt:
        assert(options.cpu_type == "detailed" or options.cpu_type == "inorder")
        return multiprocesses, idx
    else:
        return multiprocesses, 1


if options.ruby:
    fatal("This script does not support Ruby configuration, mainly"
          " because Trace CPU has been tested only with classic memory system")

if options.cpu_type == "LLVMTraceCPU":
    fatal("The host CPU should be a normal CPU other than LLVMTraceCPU\n")

if options.num_cpus > 1:
    fatal("This script does not support multi-processor trace replay.\n")

(CPUClass, test_mem_mode, FutureClass) = Simulation.setCPUClass(options)

def setLLVMTraceCPUCommomParams(llvm_trace_cpu):
    llvm_trace_cpu.fetchWidth = options.llvm_issue_width
    llvm_trace_cpu.decodeWidth = options.llvm_issue_width
    llvm_trace_cpu.renameWidth = options.llvm_issue_width
    llvm_trace_cpu.dispatchWidth = options.llvm_issue_width
    llvm_trace_cpu.issueWidth = options.llvm_issue_width
    llvm_trace_cpu.writeBackWidth = options.llvm_issue_width
    llvm_trace_cpu.commitWidth = options.llvm_issue_width
    llvm_trace_cpu.hardwareContexts = options.gem_forge_hardware_contexts_per_core

    llvm_trace_cpu.warmCache = not options.gem_forge_cold_cache

    # ADFA options.
    llvm_trace_cpu.adfaCoreIssueWidth = options.gem_forge_adfa_core_issue_width
    llvm_trace_cpu.adfaEnableSpeculation = options.gem_forge_adfa_enable_speculation
    llvm_trace_cpu.adfaBreakIVDep = options.gem_forge_adfa_break_iv_dep
    llvm_trace_cpu.adfaBreakRVDep = options.gem_forge_adfa_break_rv_dep
    llvm_trace_cpu.adfaBreakUnrollableControlDep = \
        options.gem_forge_adfa_break_unrollable_ctr_dep
    llvm_trace_cpu.adfaNumBanks = options.gem_forge_adfa_num_banks
    llvm_trace_cpu.adfaNumPortsPerBank = options.gem_forge_adfa_num_ports_per_bank
    llvm_trace_cpu.adfaNumCores = options.gem_forge_adfa_num_cores
    llvm_trace_cpu.adfaEnableTLS = (options.gem_forge_adfa_enable_tls == 1)
    llvm_trace_cpu.adfaIdealMem = (options.gem_forge_adfa_ideal_mem == 1)

    llvm_trace_cpu.storeQueueSize = options.llvm_store_queue_size
    llvm_trace_cpu.loadQueueSize = options.llvm_load_queue_size
    if options.llvm_issue_width == 2:
        llvm_trace_cpu.robSize = 64
        llvm_trace_cpu.instQueueSize = 16
        llvm_trace_cpu.loadQueueSize = 16
        llvm_trace_cpu.storeQueueSize = 20
    elif options.llvm_issue_width == 4:
        llvm_trace_cpu.robSize = 168
        llvm_trace_cpu.instQueueSize = 24
        llvm_trace_cpu.loadQueueSize = 64
        llvm_trace_cpu.storeQueueSize = 36
    elif options.llvm_issue_width == 6:
        llvm_trace_cpu.robSize = 192
        llvm_trace_cpu.instQueueSize = 28
        llvm_trace_cpu.loadQueueSize = 42
        llvm_trace_cpu.storeQueueSize = 36

    llvm_trace_cpu.streamEngineIsOracle = (
        options.gem_forge_stream_engine_is_oracle != 0)
    llvm_trace_cpu.streamEngineMaxRunAHeadLength = (
        options.gem_forge_stream_engine_max_run_ahead_length
    )
    llvm_trace_cpu.streamEngineThrottling = options.gem_forge_stream_engine_throttling
    llvm_trace_cpu.streamEngineEnableCoalesce = (
        options.gem_forge_stream_engine_enable_coalesce != 0
    )
    llvm_trace_cpu.streamEngineEnableMerge = (
        options.gem_forge_stream_engine_enable_merge != 0
    )

def setDerivO3CPUCommomParams(o3cpu):
    o3cpu.fetchWidth = options.llvm_issue_width
    o3cpu.decodeWidth = options.llvm_issue_width
    o3cpu.renameWidth = options.llvm_issue_width
    o3cpu.dispatchWidth = options.llvm_issue_width
    o3cpu.issueWidth = options.llvm_issue_width
    o3cpu.wbWidth = options.llvm_issue_width
    o3cpu.commitWidth = options.llvm_issue_width
    # o3cpu.hardwareContexts = options.gem_forge_hardware_contexts_per_core

    o3cpu.SQEntries = options.llvm_store_queue_size
    o3cpu.LQEntries = options.llvm_load_queue_size
    if options.llvm_issue_width == 2:
        o3cpu.numROBEntries = 64
        o3cpu.numIQEntries = 16
        o3cpu.LQEntries = 16
        o3cpu.SQEntries = 20
    elif options.llvm_issue_width == 4:
        o3cpu.numROBEntries = 168
        o3cpu.numIQEntries = 24
        o3cpu.LQEntries = 64
        o3cpu.SQEntries = 36
    elif options.llvm_issue_width == 6:
        o3cpu.numROBEntries = 192
        o3cpu.numIQEntries = 28
        o3cpu.LQEntries = 42
        o3cpu.SQEntries = 36

if not options.llvm_standalone:
    multiprocesses, numThreads = get_processes(options)
    CPUClass.numThreads = numThreads
    # Non-standalone mode, intialize the driver and normal cpu.

    # In this case FutureClass will be None as there is not fast forwarding or
    # switching
    cpus = [CPUClass(cpu_id=i) for i in xrange(options.num_cpus)]

    # Set the workload for normal CPUs.
    for i in xrange(options.num_cpus):
        cpu = cpus[i]
        if options.smt:
            cpu.workload = multiprocesses
        elif len(multiprocesses) == 1:
            cpu.workload = multiprocesses[0]
        else:
            cpu.workload = multiprocesses[i]
        cpu.createThreads()
        # Also set the common parameters for the normal CPUs.
        if isinstance(cpu, DerivO3CPU):
            setDerivO3CPUCommomParams(cpu)
        else:
            raise ValueError("Unsupported cpu class.")

    # Add the emulated LLVM tracer driver for the process
    if options.llvm_trace_file:
        # Make sure that we have one trace file per processes
        assert(len(options.llvm_trace_file) == len(multiprocesses))
        for i in range(len(options.llvm_trace_file)):
            process = multiprocesses[i]
            tdg_fn = options.llvm_trace_file[i]

            driver = LLVMTraceCPUDriver()
            driver.filename = 'llvm_trace_cpu'
            process.drivers = [driver]

            # For each process, add a LLVMTraceCPU for simulation.
            llvm_trace_cpu = LLVMTraceCPU(cpu_id=len(cpus))

            setLLVMTraceCPUCommomParams(llvm_trace_cpu)

            llvm_trace_cpu.cpu_id = len(cpus)
            llvm_trace_cpu.traceFile = tdg_fn
            llvm_trace_cpu.driver = driver
            llvm_trace_cpu.totalCPUs = len(multiprocesses)

            cpus.append(llvm_trace_cpu)
    options.num_cpus = len(cpus)
else:
    # Standalone mode, just the replay cpu.
    # There should be a trace file for replay.
    assert options.llvm_trace_file != ''
    cpus = list()
    # No need to worry about the process.
    for tdg_fn in options.llvm_trace_file:

        # For each process, add a LLVMTraceCPU for simulation.
        llvm_trace_cpu = LLVMTraceCPU(cpu_id=len(cpus))

        setLLVMTraceCPUCommomParams(llvm_trace_cpu)

        # A dummy null driver to make the python script happy.
        llvm_trace_cpu.cpu_id = len(cpus)
        llvm_trace_cpu.traceFile = tdg_fn
        llvm_trace_cpu.driver = NULL
        llvm_trace_cpu.totalCPUs = len(options.llvm_trace_file)
        cpus.append(llvm_trace_cpu)
    options.num_cpus = len(cpus)

system = System(cpu=cpus,
                mem_mode=test_mem_mode,
                mem_ranges=[AddrRange(options.mem_size)],
                cache_line_size=options.cacheline_size)

Simulation.setWorkCountOptions(system, options)

# Create a top-level voltage domain
system.voltage_domain = VoltageDomain(voltage=options.sys_voltage)

# Create a source clock for the system. This is used as the clock period for
# xbar and memory
system.clk_domain = SrcClockDomain(clock=options.sys_clock,
                                   voltage_domain=system.voltage_domain)

# Create a CPU voltage domain
system.cpu_voltage_domain = VoltageDomain()

# Create a separate clock domain for the CPUs. In case of Trace CPUs this clock
# is actually used only by the caches connected to the CPU.
system.cpu_clk_domain = SrcClockDomain(clock=options.cpu_clock,
                                       voltage_domain=system.cpu_voltage_domain)

# All cpus belong to a common cpu_clk_domain, therefore running at a common
# frequency.
for cpu in system.cpu:
    cpu.clk_domain = system.cpu_clk_domain
    if isinstance(cpu, DerivO3CPU):
        cpu.issueWidth = options.llvm_issue_width
        cpu.SQEntries = options.llvm_store_queue_size
        cpu.branchPred = LocalBP(numThreads=1)

# Assign input trace files to the Trace CPU
# system.cpu.traceFile = options.llvm_trace_file

# Configure the classic memory system options
MemClass = Simulation.setMemClass(options)
system.membus = SystemXBar()
system.system_port = system.membus.slave
CacheConfig.config_cache(options, system)
MemConfig.config_mem(options, system)

if options.llvm_mcpat == 1:
    system.mcpat_manager = McPATManager()

# Disable snoop filter
if not options.no_l2bus:
    system.tol2bus.snoop_filter = NULL

for cpu in system.cpu:
    if options.l1_5dcache:
        # Add the L1.5 dcache.
        cpu.l1_5dcache = L1_5_DCache(
            assoc=options.l1_5d_assoc,
            mshrs=options.l1_5d_mshrs,
            size=options.l1_5d_size,
        )
        # Connect them
        cpu.dcache.mem_side = cpu.l1_5dcache.cpu_side
        cpu.l1_5dcache.mem_side = system.tol2bus.slave
        if (options.gem_forge_stream_engine_l1d == 'aware-l15-lru'):
            cpu.l1_5dcache.tags = StreamLRU()

    if options.gem_forge_stream_engine_l1d == 'aware-replace':
        # stream-aware cache must be used with stream merge.
        # assert(options.gem_forge_stream_engine_enable_merge == 1)
        cpu.dcache.stream_aware_replacement = True
    elif options.gem_forge_stream_engine_l1d == 'aware-lru':
        cpu.dcache.tags = StreamLRU()
        # if options.l1_5dcache:
        #     cpuel1_5dcache.tags = StreamLRU()
    elif options.gem_forge_stream_engine_l1d == 'aware-miss-spec':
        assert(options.gem_forge_stream_engine_enable_merge == 1)
        cpu.dcache.stream_aware_miss_speculation = True
        cpu.dcache.stream_aware_replacement = True
    elif options.gem_forge_stream_engine_l1d.startswith('placement'):
        cpu.streamEngineEnablePlacement = True
        if '#' in options.gem_forge_stream_engine_l1d:
            x, y = options.gem_forge_stream_engine_l1d.split('#')
        else:
            x = options.gem_forge_stream_engine_l1d
            y = ''
        cpu.streamEnginePlacement = x
        if x == 'placement-oracle':
            cpu.streamEngineEnablePlacementOracle = True
        if 'lru' in y:
            cpu.dcache.tags = StreamLRU()
        if 'sub' in y:
            cpu.streamEnginePlacementLat = 'sub'
        elif 'imm' in y:
            cpu.streamEnginePlacementLat = 'imm'
        if 'bus' in y:
            cpu.streamEngineEnablePlacementBus = True
        if 'nst' in y:
            cpu.streamEngineNoBypassingStore = True
        if 'cst' in y:
            cpu.streamEngineContinuousStore = True
        if 'rst' in y:
            cpu.streamEnginePeriodReset = True


if options.llvm_prefetch == 1:
    for cpu in system.cpu:
        cpu.dcache.prefetcher = StridePrefetcher(degree=8, latency=1)
        if options.l1_5dcache:
            cpu.l1_5dcache.prefetcher = StridePrefetcher(degree=8, latency=1)
    system.l2.prefetcher = StridePrefetcher(degree=8, latency=1)


root = Root(full_system=False, system=system)
Simulation.run(options, root, system, FutureClass)