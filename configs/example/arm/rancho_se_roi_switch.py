# Copyright (c) 2016-2017, 2022-2023 Arm Limited
# All rights reserved.
#
# Modified local copy for PARSEC ROI runs with Rancho caches enabled.

"""SE mode runner with Atomic warmup and Rancho ROI execution.

This variant starts benchmarks on Atomic CPUs. On ROI begin, it switches to
Rancho CPUs, resets stats, and continues until ROI end (or maxinsts, if set).
"""

import argparse
import os
import shlex
import sys

import m5
from m5.objects import *
from m5.util import addToPath

# Resolve imports exactly like the upstream config regardless of launch cwd.
m5.util.addToPath("../..")

import devices
from common import (
    MemConfig,
    ObjectList,
)
from common.cores.arm import Rancho

cpu_types = {
    "atomic": (AtomicSimpleCPU, None, None, None),
    "rancho": (
        Rancho.Rancho,
        Rancho.Rancho_ICache,
        Rancho.Rancho_DCache,
        Rancho.Rancho_L2,
    ),
}


def get_processes(cmd, process_cwd=None):
    """Interprets commands to run and returns a list of processes."""

    cwd = os.path.abspath(
        process_cwd if process_cwd is not None else os.getcwd()
    )
    if not os.path.isdir(cwd):
        print(f"Error: Requested cwd does not exist: {cwd}")
        sys.exit(1)

    openmp_env = []
    for key in (
        "OMP_NUM_THREADS",
        "OMP_DYNAMIC",
        "OMP_PROC_BIND",
        "OMP_PLACES",
    ):
        val = os.environ.get(key)
        if val is not None:
            openmp_env.append(f"{key}={val}")

    multiprocesses = []
    for idx, c in enumerate(cmd):
        argv = shlex.split(c)
        process = Process(pid=100 + idx, cwd=cwd, cmd=argv, executable=argv[0])
        process.gid = os.getgid()
        if openmp_env:
            process.env = openmp_env

        print("info: %d. command and arguments: %s" % (idx + 1, process.cmd))
        print("info: %d. process cwd: %s" % (idx + 1, process.cwd))
        if openmp_env:
            print("info: %d. OpenMP env: %s" % (idx + 1, openmp_env))
        multiprocesses.append(process)

    return multiprocesses


def _configure_rancho_cluster(cpus, args):
    for cpu in cpus:
        cpu.speculativeBarrierIssue = True
        cpu.cacheOrderingTagEntries = args.cache_ordering_tag_entries

    if args.sq_entries is not None:
        for cpu in cpus:
            cpu.SQEntries = args.sq_entries

    if args.merge_buffer is not None:
        use_mb = args.merge_buffer == "on"
        for cpu in cpus:
            cpu.useMergeBuffer = use_mb

    if args.merge_buffer_entries is not None:
        for cpu in cpus:
            cpu.mergeBufferEntries = args.merge_buffer_entries

    if args.merge_buffer_prefetch is not None:
        use_pf = args.merge_buffer_prefetch == "on"
        for cpu in cpus:
            cpu.mergeBufferPrefetch = use_pf

    if args.versioning is not None:
        enable_versioning = args.versioning == "on"
        enable_store_release_opt = args.optimize_release == "on"
        enable_acquire_pc_opt = args.optimize_acquire_pc == "on"
        enable_safe_stlf_loads_bypass_mb = args.safeStlfBypass == "on"
        enable_safe_cache_loads_bypass_mb = args.safeCacheBypass == "on"
        for cpu in cpus:
            cpu.enableVersioning = enable_versioning
            cpu.optimizeStoreRelease = enable_store_release_opt
            cpu.optimizeAcquirePC = enable_acquire_pc_opt
            cpu.safeStlfLoadsBypassMBDrain = enable_safe_stlf_loads_bypass_mb
            cpu.safeCacheLoadsBypassMBDrain = enable_safe_cache_loads_bypass_mb


def create(args):
    """Create and configure the system object."""

    # Warmup on Atomic; switch to Rancho O3 at ROI begin.
    system = devices.SimpleSeSystem(
        mem_mode=cpu_types["atomic"][0].memory_mode()
    )
    system.work_begin_exit_count = 1
    system.work_end_exit_count = 1

    # Build caches on the active (atomic) CPUs so switched-out Rancho CPUs
    # can safely take over already-connected cache ports at ROI begin.
    system.atomic_cluster = devices.ArmCpuCluster(
        system,
        args.num_cpus,
        args.cpu_freq,
        "1.2V",
        AtomicSimpleCPU,
        devices.L1I,
        devices.L1D,
        devices.L2,
    )

    system.rancho_cluster = devices.ArmCpuCluster(
        system,
        args.num_cpus,
        args.cpu_freq,
        "1.2V",
        *cpu_types["rancho"],
        tarmac_gen=args.tarmac_gen,
        tarmac_dest=args.tarmac_dest,
    )

    for cpu in system.rancho_cluster.cpus:
        cpu.switched_out = True

    # CPU handover requires matching CPU ids.
    for old_cpu, new_cpu in zip(
        system.atomic_cluster.cpus, system.rancho_cluster.cpus
    ):
        new_cpu.cpu_id = old_cpu.cpu_id

    # Attach a private L1 + shared L2 hierarchy to the currently-active
    # atomic cluster. On switch, Rancho CPUs inherit these cache-connected
    # ports via takeOverFrom().
    system.atomic_cluster.addL1()
    system.atomic_cluster.addL2(system.atomic_cluster.clk_domain)
    system.atomic_cluster.connectMemSide(system.membus)

    system.mem_ranges = [AddrRange(start=0, size=args.mem_size)]
    MemConfig.config_mem(args, system)
    system.connect()

    processes = get_processes(args.commands_to_run, args.cwd)
    if not processes:
        print("Error: No commands provided to run.")
        sys.exit(1)

    # pthread-style benchmark: one app process mapped across multiple cores.
    if len(processes) == 1 and args.num_cpus > 1:
        system.multi_thread = True

    if len(processes) > args.num_cpus:
        print(
            f"info: more commands ({len(processes)}) than cores ({args.num_cpus}); "
            "truncating extra workloads"
        )
        processes = processes[: args.num_cpus]
    elif len(processes) < args.num_cpus and len(processes) != 1:
        print(
            f"Error: fewer commands ({len(processes)}) than cores ({args.num_cpus}) without a single multithreaded workload."
        )
        sys.exit(1)

    system.workload = SEWorkload.init_compatible(processes[0].executable)

    if len(processes) == 1:
        for cpu in system.atomic_cluster.cpus:
            cpu.workload = processes[0]
        for cpu in system.rancho_cluster.cpus:
            cpu.workload = processes[0]
    else:
        for cpu, workload in zip(system.atomic_cluster.cpus, processes):
            cpu.workload = workload
        for cpu, workload in zip(system.rancho_cluster.cpus, processes):
            cpu.workload = workload

    _configure_rancho_cluster(system.rancho_cluster.cpus, args)

    if args.maxinsts:
        for cpu in system.rancho_cluster.cpus:
            cpu.max_insts_any_thread = args.maxinsts

    switch_cpu_list = list(
        zip(system.atomic_cluster.cpus, system.rancho_cluster.cpus)
    )
    return system, switch_cpu_list


def main():
    parser = argparse.ArgumentParser(epilog=__doc__)

    parser.add_argument(
        "commands_to_run",
        metavar="command(s)",
        nargs="*",
        help="Command(s) to run",
    )
    parser.add_argument(
        "--cwd",
        type=str,
        default=".",
        help="Working directory for the simulated process (default: launcher's current directory)",
    )
    parser.add_argument(
        "--cpu",
        type=str,
        choices=list(cpu_types.keys()),
        default="rancho",
        help="Kept for compatibility; starts atomic and switches to rancho at ROI.",
    )
    parser.add_argument("--cpu-freq", type=str, default="3GHz")
    parser.add_argument(
        "--num-cpus", type=int, default=1, help="Number of CPUs"
    )
    parser.add_argument(
        "--merge-buffer",
        choices=["on", "off"],
        default=None,
        help="Force enable/disable the O3 merge buffer (default uses CPU template setting)",
    )
    parser.add_argument(
        "--merge-buffer-entries",
        type=int,
        default=None,
        help="Override number of merge buffer entries",
    )
    parser.add_argument(
        "--merge-buffer-prefetch",
        choices=["on", "off"],
        default=None,
        help="Enable/disable prefetch on merge buffer allocation",
    )
    parser.add_argument(
        "--versioning",
        choices=["on", "off"],
        default=None,
        help="Enable/disable versioning",
    )
    parser.add_argument(
        "--safeCacheBypass",
        choices=["on", "off"],
        default=None,
        help="Enable/disable safe loads from cache bypass MB drain",
    )
    parser.add_argument(
        "--sq-entries",
        type=int,
        default=None,
        help="Override number of store queue entries (SQEntries)",
    )
    parser.add_argument(
        "--cache-ordering-tag-entries",
        type=int,
        default=8192,
        help="Maximum entries in cache ordering tag map per CPU",
    )
    parser.add_argument(
        "--safeStlfBypass",
        choices=["on", "off"],
        default=None,
        help="Enable/disable safe loads from SQ/MB bypass MB drain",
    )
    parser.add_argument(
        "--optimize-release",
        choices=["on", "off"],
        default=None,
        help="Enable/disable store release optimization",
    )
    parser.add_argument(
        "--optimize-acquire-pc",
        choices=["on", "off"],
        default=None,
        help="Enable/disable acquire-PC optimization",
    )
    parser.add_argument(
        "--mem-type",
        default="DDR3_1600_8x8",
        choices=ObjectList.mem_list.get_names(),
        help="type of memory to use",
    )
    parser.add_argument(
        "--mem-channels", type=int, default=2, help="number of memory channels"
    )
    parser.add_argument(
        "--mem-ranks",
        type=int,
        default=None,
        help="number of memory ranks per channel",
    )
    parser.add_argument(
        "--mem-size",
        action="store",
        type=str,
        default="2GB",
        help="Specify the physical memory size",
    )
    parser.add_argument(
        "--tarmac-gen",
        action="store_true",
        help="Write a Tarmac trace.",
    )
    parser.add_argument(
        "--tarmac-dest",
        choices=TarmacDump.vals,
        default="stdoutput",
        help="Destination for the Tarmac trace output. [Default: stdoutput]",
    )
    parser.add_argument(
        "--maxinsts", type=int, default=None, help="max instructions"
    )

    args = parser.parse_args()

    root = Root(full_system=False)
    root.system, switch_cpu_list = create(args)

    m5.instantiate()

    switched = False
    saw_roi_begin = False
    while True:
        event = m5.simulate()
        cause = event.getCause()
        lcause = cause.lower()
        print(f"{cause} ({event.getCode()}) @ {m5.curTick()}")

        if not switched and (
            "workbegin" in lcause or "work started" in lcause
        ):
            print("info: ROI begin detected, switching from atomic to rancho")
            m5.switchCpus(root.system, switch_cpu_list)
            switched = True
            saw_roi_begin = True
            m5.stats.reset()
            continue

        if (
            cause == "m5_exit instruction encountered"
            or cause == "user interrupt received"
            or cause == "simulate() limit reached"
            or cause == "a thread reached the max instruction count"
            or "exiting with last active thread context" in cause
        ):
            break

        if "workend" in lcause or (
            "work" in lcause and "exit count" in lcause and "item" in lcause
        ):
            print("info: ROI end detected, stopping simulation")
            break

        print(f"info: unhandled exit event '{cause}', continuing simulation")

    if not saw_roi_begin:
        print(
            "warning: ROI workbegin was never seen; run completed without CPU switch"
        )
    sys.exit(event.getCode())


if __name__ == "__m5_main__":
    main()
