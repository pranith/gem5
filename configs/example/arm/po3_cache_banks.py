"""Run the PO3 cache-bank conflict microbenchmark in syscall-emulation mode."""

import argparse

import m5
from m5.objects import *


class L1Cache(Cache):
    assoc = 4
    tag_latency = 1
    data_latency = 1
    response_latency = 1
    mshrs = 16
    tgts_per_mshr = 8


parser = argparse.ArgumentParser()
parser.add_argument("binary", help="AArch64 microbenchmark binary")
parser.add_argument("banks", type=int, help="Number of modeled D-cache banks")
parser.add_argument(
    "--merge-buffer",
    action="store_true",
    help="Enable the PO3 store merge buffer",
)
parser.add_argument(
    "--merge-buffer-entries",
    type=int,
    default=32,
    help="Number of merge-buffer entries",
)
parser.add_argument(
    "--merge-buffer-retire-cycles",
    type=int,
    default=16,
    help="Cycles before a merge-buffer entry becomes drainable",
)
parser.add_argument("--max-ticks", type=int, default=10_000_000_000)
args = parser.parse_args()

system = System()
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=VoltageDomain()
)
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]

system.cpu = ArmPO3CPU()
system.cpu.cacheBanks = args.banks
system.cpu.cacheLoadPorts = 3
system.cpu.cacheStorePorts = 1
system.cpu.po3MemPipeline = True
system.cpu.po3MemAddrGenWidth = 3
system.cpu.po3MemTLBLookupWidth = 3
system.cpu.po3MemCacheAccessWidth = 3
system.cpu.po3CommitStageWidth = 2
system.cpu.useMergeBuffer = args.merge_buffer
system.cpu.mergeBufferEntries = args.merge_buffer_entries
system.cpu.mergeBufferRetireCycles = args.merge_buffer_retire_cycles
system.cpu.fetchWidth = 8
system.cpu.decodeWidth = 8
system.cpu.renameWidth = 8
system.cpu.dispatchWidth = 8
system.cpu.issueWidth = 8
system.cpu.wbWidth = 8
system.cpu.commitWidth = 8

system.cpu.addPrivateSplitL1Caches(
    L1Cache(size="32KiB"), L1Cache(size="32KiB")
)
system.membus = SystemXBar()
system.cpu.connectBus(system.membus)
system.cpu.createInterruptController()

system.memory = SimpleMemory(range=system.mem_ranges[0], latency="10ns")
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports

system.workload = SEWorkload.init_compatible(args.binary)
system.cpu.workload = Process(cmd=[args.binary])
system.cpu.createThreads()

root = Root(full_system=False, system=system)
m5.instantiate()
exitEvent = m5.simulate(args.max_ticks)
print(f"banks={args.banks} tick={m5.curTick()} cause={exitEvent.getCause()}")
