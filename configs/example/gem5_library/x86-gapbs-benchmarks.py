# Copyright (c) 2021 The Regents of the University of California.
# All rights reserved.
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

"""
Script to run GAPBS benchmarks with gem5. The script expects the user to
provide the benchmark to run.
The system is fixed with 2 CPU cores, MESI Two Level system cache and 3 GiB
DDR4 memory. It uses the x86 board.

This script will count the total number of instructions executed
in the ROI. It also tracks how much wallclock and simulated time.

Usage:
------

```
scons build/ALL/gem5.opt
./build/ALL/gem5.opt \
    configs/example/gem5_library/x86-gabps-benchmarks.py \
    --benchmark <benchmark_name>
```
"""

import argparse
import sys
import time

import m5
from m5.objects import Root

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.x86_board import X86Board
from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import obtain_resource
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.exit_handler import (
    WorkBeginExitHandler,
    WorkEndExitHandler,
)
from gem5.simulate.simulator import Simulator
from gem5.utils.override import overrides
from gem5.utils.requires import requires

# This script has been extensively modified and needs to be tested

requires(
    isa_required=ISA.X86,
    coherence_protocol_required=CoherenceProtocol.MESI_TWO_LEVEL,
    kvm_required=True,
)

parser = argparse.ArgumentParser(
    description="An example configuration script to run the GAPBS benchmarks."
)

# This suite on the gem5 Resources website should be updated to use the latest
# GAPBS benchmarks. For now, I will modify this script not to use the suite
gapbs_suite = obtain_resource(
    "gapbs-benchmark-suite", resource_version="1.0.0"
)

# The only positional argument accepted is the benchmark name in this script.

parser.add_argument(
    "--benchmark",
    type=str,
    required=True,
    help="Input the benchmark program to execute.",
    choices=["bfs", "pr_spmv", "pr", "cc", "tc", "bc", "sssp", "cc_sv"],
)

args = parser.parse_args()


# Setting up all the fixed system parameters here
# Caches: MESI Two Level Cache Hierarchy

from gem5.components.cachehierarchies.ruby.mesi_two_level_cache_hierarchy import (
    MESITwoLevelCacheHierarchy,
)

cache_hierarchy = MESITwoLevelCacheHierarchy(
    l1d_size="32KiB",
    l1d_assoc=8,
    l1i_size="32KiB",
    l1i_assoc=8,
    l2_size="256KiB",
    l2_assoc=16,
    num_l2_banks=2,
)
# Memory: Dual Channel DDR4 2400 DRAM device.
# The X86 board only supports 3 GiB of main memory.

memory = DualChannelDDR4_2400(size="3GiB")

# Here we setup the processor. This is a special switchable processor in which
# a starting core type and a switch core type must be specified. Once a
# configuration is instantiated a user may call `processor.switch()` to switch
# from the starting core types to the switch core types. In this simulation
# we start with KVM cores to simulate the OS boot, then switch to the Timing
# cores for the command we wish to run after boot.

processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=2,
)

# Here we set up the board. The X86Board allows for Full-System X86 simulations

board = X86Board(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

# Here we set the FS workload, i.e., gapbs benchmark program
# After simulation has ended you may inspect `m5out/system.pc.com_1.device`
# to see the stdout of the simulated program.

# After the system boots, we execute the benchmark program and wait until the
# ROI `workbegin` annotation is reached. We start collecting the number of
# committed instructions until ROI ends (marked by `workend`). We then finish
# executing the rest of the benchmark.

board.set_workload(
    obtain_resource(
        f"x86-ubuntu-24.04-gapbs-{args.benchmark}-test",
        resource_version="1.0.0",
    )
)


class CustomWorkBeginExitHandler(WorkBeginExitHandler):
    @overrides(WorkBeginExitHandler)
    def _process(self, simulator: "Simulator") -> None:
        print("Done booting Linux")
        print("Resetting stats at the start of ROI!")
        # This is a hacky way to keep the script's ending print statement.
        simulator._tick_stopwatch.append(
            (ExitEvent.WORKBEGIN, simulator.get_current_tick())
        )
        m5.stats.reset()
        simulator.switch_processor()

    @overrides(WorkBeginExitHandler)
    def _exit_simulation(self) -> bool:
        return False


class CustomWorkEndExitHandler(WorkEndExitHandler):
    @overrides(WorkEndExitHandler)
    def _process(self, simulator: "Simulator") -> None:
        print("Dump stats at the end of the ROI!")

        m5.stats.dump()
        # This is a hacky way to keep the script's ending print statement.
        simulator._tick_stopwatch.append(
            (ExitEvent.WORKEND, simulator.get_current_tick())
        )

    @overrides(WorkEndExitHandler)
    def _exit_simulation(self) -> bool:
        return True


# def handle_workbegin():
#     print("Done booting Linux")
#     print("Resetting stats at the start of ROI!")
#     m5.stats.reset()
#     global start_tick
#     start_tick = m5.curTick()
#     processor.switch()
#     yield False  # E.g., continue the simulation.


# def handle_workend():
#     print("Dump stats at the end of the ROI!")
#     m5.stats.dump()
#     yield True  # Stop the simulation. We're done.


simulator = Simulator(board=board)

# We maintain the wall clock time.

globalStart = time.time()

print("Running the simulation")
print("Using KVM cpu")


simulator.run()

# Simulation is over at this point. We acknowledge that all the simulation
# events were successful.
print("All simulation events were successful.")

# We print the final simulation statistics.
print("Done with the simulation")
print("Performance statistics:")

import _m5.core

print(
    "Simulated time in ROI: "
    f"{((simulator.get_tick_stopwatch[ExitEvent.WORKBEGIN] -
    simulator.get_tick_stopwatch[ExitEvent.WORKEND]) /
    _m5.core.getClockFrequency()):.2f}s"
)
print(
    "Ran a total of"
    f"{simulator.get_current_tick() / _m5.core.getClockFrequency()}"
    "simulated seconds"
)
print(
    f"Total wallclock time: {(time.time() - globalStart):.2f}, which is "
    f"{((time.time() - globalStart) / 60):.2f} min"
)
