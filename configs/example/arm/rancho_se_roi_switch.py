# Copyright (c) 2016-2017, 2022-2023 Arm Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""This script is the syscall emulation example script from the ARM
Research Starter Kit on System Modeling. More information can be found
at: http://www.arm.com/ResearchEnablement/SystemModeling
"""

import argparse
import os
import shlex
import sys

import m5
from m5.objects import *
from m5.util import addToPath

m5.util.addToPath("../..")

import devices
from common import (
    MemConfig,
    ObjectList,
)
from common.cores.arm import Rancho

# Pre-defined CPU configurations. Each tuple must be ordered as : (cpu_class,
# l1_icache_class, l1_dcache_class, walk_cache_class, l2_Cache_class). Any of
# the cache class may be 'None' if the particular cache is not present.
cpu_types = {
    "atomic": (AtomicSimpleCPU, None, None, None),
    #    "minor": (MinorCPU, devices.L1I, devices.L1D, devices.L2),
    "rancho": (
        Rancho.Rancho,
        Rancho.Rancho_ICache,
        Rancho.Rancho_DCache,
        Rancho.Rancho_L2,
    ),
}


def get_processes(cmd, process_cwd=None):
    """Interprets commands to run and returns a list of processes"""

    cwd = os.path.abspath(
        process_cwd if process_cwd is not None else os.getcwd()
    )
    if not os.path.isdir(cwd):
        print(f"Error: Requested cwd does not exist: {cwd}")
        sys.exit(1)
    # Forward selected OpenMP runtime controls from launcher env to
    # the simulated process so benchmarks can honor thread settings.
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
    # enable speculative post-barrier load/store issue
    for cpu in cpus:
        cpu.speculativeBarrierIssue = True

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
        for cpu in cpus:
            cpu.enableVersioning = enable_versioning
            cpu.optimizeStoreRelease = enable_store_release_opt
            cpu.optimizeAcquirePC = enable_acquire_pc_opt


def create(args):
    """Create and configure the system object."""

    # Start with atomic CPUs and switch to Rancho O3 on ROI begin.
    system = devices.SimpleSeSystem(mem_mode=AtomicSimpleCPU.memory_mode())
    # Exit simulate() when the first m5_work_begin is hit so we can switch CPUs.
    system.work_begin_exit_count = 1
    # Exit simulate() when m5_work_end is hit to stop exactly at ROI end.
    system.work_end_exit_count = 1

    system.atomic_cluster = devices.ArmCpuCluster(
        system,
        args.num_cpus,
        args.cpu_freq,
        "1.2V",
        *cpu_types["atomic"],
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

    # CPU handover requires matching CPU ids between old and new CPUs.
    for old_cpu, new_cpu in zip(
        system.atomic_cluster.cpus, system.rancho_cluster.cpus
    ):
        new_cpu.cpu_id = old_cpu.cpu_id

    # Atomic CPUs in this tree don't support private split L1 attachment.
    # Keep atomic cores directly connected.
    system.atomic_cluster.connectMemSide(system.membus)

    # Tell components about the expected physical memory ranges. This
    # is, for example, used by the MemConfig helper to determine where
    # to map DRAMs in the physical address space.
    system.mem_ranges = [AddrRange(start=0, size=args.mem_size)]

    # Configure the off-chip memory system.
    MemConfig.config_mem(args, system)

    # Wire up the system's memory system
    system.connect()

    # Parse the command line and get a list of Processes instances
    # that we can pass to gem5.
    processes = get_processes(args.commands_to_run, args.cwd)
    if not processes:
        print("Error: No commands provided to run.")
        sys.exit(1)

    if len(processes) == 1 and args.num_cpus > 1:
        # Enable pthread-style multithreading for a single benchmark process.
        system.multi_thread = True

    if len(processes) < args.num_cpus:
        # Repeat the last process to cover all cores, similar to se.py behaviour.
        last = processes[-1]
        for _ in range(args.num_cpus - len(processes)):
            processes.append(last)
        print(
            f"info: fewer commands than cores; repeating last workload to fill {args.num_cpus} cores"
        )
    elif len(processes) > args.num_cpus:
        print(
            f"info: more commands ({len(processes)}) than cores ({args.num_cpus}); "
            "truncating extra workloads"
        )
        processes = processes[: args.num_cpus]

    system.workload = SEWorkload.init_compatible(processes[0].executable)

    # Assign one workload to each CPU (both clusters).
    for cpu, workload in zip(system.atomic_cluster.cpus, processes):
        cpu.workload = workload
    for cpu, workload in zip(system.rancho_cluster.cpus, processes):
        cpu.workload = workload

    _configure_rancho_cluster(system.rancho_cluster.cpus, args)

    if args.maxinsts:
        # Apply max instruction count to the post-ROI Rancho phase.
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
        help="Kept for compatibility; this script always starts atomic and switches to rancho at ROI",
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

    # Create a single root node for gem5's object hierarchy. There can
    # only exist one root node in the simulator at any given
    # time. Tell gem5 that we want to use syscall emulation mode
    # instead of full system mode.
    root = Root(full_system=False)

    # Populate the root node with a system. A system corresponds to a
    # single node with shared memory.
    root.system, switch_cpu_list = create(args)

    # Instantiate the C++ object hierarchy. After this point,
    # SimObjects can't be instantiated anymore.
    m5.instantiate()

    switched = False
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
            # Ensure the measured ROI starts after CPU handover.
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

    if not switched:
        print(
            "warning: ROI workbegin was never seen; run completed without CPU switch"
        )
    sys.exit(event.getCode())


if __name__ == "__m5_main__":
    main()
