# Copyright (c) 2021 The Regents of the University of California
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
This script shows an example of running a full system RISCV Ubuntu boot
simulation using the gem5 library. This simulation boots Ubuntu 20.04 using
2 TIMING CPU cores. The simulation ends when the startup is completed
successfully.

Usage
-----

```
scons build/RISCV/gem5.opt
./build/RISCV/gem5.opt \
    configs/example/gem5_library/riscv-ubuntu-run.py
```
"""

import argparse

import m5
from m5.objects import Root

import gem5.utils.multisim as multisim
from gem5.components.boards.riscv_board import RiscvBoard
from gem5.components.cachehierarchies.classic.private_l1_private_l2_walk_cache_hierarchy import (
    PrivateL1PrivateL2WalkCacheHierarchy,
)
from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import (
    CPUTypes,
    get_cpu_type_from_str,
)
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import (
    DiskImageResource,
    KernelResource,
    obtain_resource,
)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.exit_handler import (
    AfterBootExitHandler,
    ExitHandler,
    WorkBeginExitHandler,
)
from gem5.simulate.simulator import Simulator
from gem5.utils.override import overrides
from gem5.utils.requires import requires

# This runs a check to ensure the gem5 binary is compiled for RISCV.

requires(isa_required=ISA.RISCV)

parser = argparse.ArgumentParser()

parser.add_argument("--num-cores", type=int, required=True)

args = parser.parse_args()


class AfterBootTakeCheckpoint(AfterBootExitHandler):
    def _process(self, simulator: "Simulator") -> None:
        # checkpoint_path = f"./riscv-{args.num_cores}core-systemboot-checkpoint"
        checkpoint_path = (
            f"./riscv-ubuntu-24.04-boot-{args.num_cores}-core-checkpoint"
        )
        print(f"Taking a checkpoint at {checkpoint_path}")
        simulator.save_checkpoint(checkpoint_path)
        print("Done taking a checkpoint")
        print("Scheduling exit in 10 million ticks!")
        m5.scheduleTickExitFromCurrent(
            10_000_000
        )  # exit 10 million ticks after system boot

    def _exit_simulation(self) -> bool:
        return False


cache_hierarchy = PrivateL1PrivateL2WalkCacheHierarchy(
    l1d_size="16KiB", l1i_size="16KiB", l2_size="256KiB"
)

# Memory: Dual Channel DDR4 2400 DRAM device.

memory = DualChannelDDR4_2400(size="3GiB")

# Here we setup the processor. We use a simple processor.
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.ATOMIC,
    switch_core_type=CPUTypes.ATOMIC,  # It doesn't matter what this is, since we never switch cores
    isa=ISA.RISCV,
    num_cores=args.num_cores,
)

# Here we setup the board. The RiscvBoard allows for Full-System RISCV
# simulations.
board = RiscvBoard(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

# Here we a full system workload: "riscv-ubuntu-20.04-boot" which boots
# Ubuntu 20.04. Once the system successfully boots it encounters an `m5_exit`
# instruction which stops the simulation. When the simulation has ended you may
# inspect `m5out/system.pc.com_1.device` to see the stdout.
# board.set_kernel_disk_workload(
#     kernel=KernelResource("/projects/gem5/new-base-imgs-w-hypercalls/riscv-disk-image-24-04/riscv-vmlinux-6.8.12"),
#     disk_image=DiskImageResource("/projects/gem5/new-base-imgs-w-hypercalls/disk-image-riscv-npb/riscv-ubuntu-24.04-npb-20250515", root_partition="1"),
#     bootloader=obtain_resource("riscv-bootloader-opensbi-1.3.1", resource_version="1.0.0"),
#     readfile_contents="/home/gem5/NPB3.4-OMP/bin/cg.S.x; sleep 5;",
# )

board.set_kernel_disk_workload(
    kernel=obtain_resource(
        "riscv-linux-6.8.12-kernel", resource_version="1.0.0"
    ),
    disk_image=obtain_resource(
        "riscv-ubuntu-24.04-npb-img", resource_version="1.0.0"
    ),
    bootloader=obtain_resource(
        "riscv-bootloader-opensbi-1.3.1", resource_version="1.0.0"
    ),
    readfile_contents="/home/gem5/NPB3.4-OMP/bin/cg.S.x; sleep 5;",
)

# board.set_workload(
#     obtain_resource("riscv-ubuntu-24.04-npb-cg-s", resource_version="2.0.0")
# )

simulator = Simulator(
    board=board,
)
simulator.run()
