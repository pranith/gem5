# Copyright (c) 2022 The Regents of the University of California
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
This configuration script shows an example of how to restore a checkpoint that
was taken for SimPoints in the script
configs/example/gem5_library/checkpoints/simpoints-se-checkpoint.py.
The SimPoints, SimPoints' interval length, and the warmup instruction length
are passed into the SimPoint module, so the SimPoint object will store and
calculate the warmup instruction length for each SimPoint based on the
available instructions before reaching the start of the SimPoint. With the
Simulator module, an exit event will be generated to stop when the warmup
session ends and the SimPoints interval ends.

This script builds a more complex board than the board used for taking
checkpoints.

Usage
-----

```
scons build/ALL/gem5.opt
./build/ALL/gem5.opt \
    configs/example/gem5_library/checkpoints/simpoints-se-checkpoint.py

./build/ALL/gem5.opt \
    configs/example/gem5_library/checkpoints/simpoints-se-restore.py
```

"""

import m5
from m5.stats import (
    dump,
    reset,
)

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.classic.private_l1_private_l2_walk_cache_hierarchy import (
    PrivateL1PrivateL2WalkCacheHierarchy,
)
from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import obtain_resource
from gem5.simulate.exit_handler import ScheduledExitEventHandler
from gem5.simulate.simulator import Simulator
from gem5.utils.override import overrides
from gem5.utils.requires import requires

requires(isa_required=ISA.X86)

# The cache hierarchy can be different from the cache hierarchy used in taking
# the checkpoints
cache_hierarchy = PrivateL1PrivateL2WalkCacheHierarchy(
    l1d_size="32KiB",
    l1i_size="32KiB",
    l2_size="256KiB",
)

# The memory structure can be different from the memory structure used in
# taking the checkpoints, but the size of the memory must be maintained
memory = DualChannelDDR4_2400(size="2GiB")

processor = SimpleProcessor(
    cpu_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=1,
)

board = SimpleBoard(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

# Here we obtain the workload from gem5 resources, the checkpoint in this
# workload was generated from
# `configs/example/gem5_library/checkpoints/simpoints-se-checkpoint.py`.

# On gem5 resources, this workload says that it is compatible with gem5
# versions 23.1, 24.0, and 24.1. The resource may need to be updated or remade
# so it is compatible with v25.0
#
board.set_workload(
    obtain_resource(
        "x86-print-this-15000-with-simpoints-and-checkpoint",
        resource_version="2.0.0",
    )
)

# Below we set the simpoint manually.
#
# This loads a single checkpoint as an example of using simpoints to simulate
# the function of a single simpoint region.

# I feel that it would be good to keep the example of setting the simpoint
# manually, since many users of gem5 would probably set it manually instead
# of using a pre-existing workload or making their own workload. Would the
# best way to demonstrate this be to keep this example in the comments, or
# should we point users to the "Raw" tab of resources on the resources website?

# board.set_se_simpoint_workload(
#     binary=obtain_resource("x86-print-this"),
#     arguments=["print this", 15000],
#     simpoint=SimpointResource(
#         simpoint_interval=1000000,
#         simpoint_list=[2, 3, 4, 15],
#         weight_list=[0.1, 0.2, 0.4, 0.3],
#         warmup_interval=1000000,
#     ),
#     checkpoint=obtain_resource(
#         "simpoints-se-checkpoints", resource_version="3.0.0"
#     ),
# )


class SimpointScheduledExitHandler(ScheduledExitEventHandler):
    warmed_up = False

    @overrides(ScheduledExitEventHandler)
    def _process(self, simulator: "Simulator") -> None:
        if self.__class__.warmed_up:
            print("end of SimPoint interval")
        else:
            print("end of warmup, starting to simulate SimPoint")
            self.__class__.warmed_up = True
            # use m5.scheduleTickExitAbsolute to trigger another hypercall exit
            # later
            m5.scheduleTickExitAbsolute(
                board.get_simpoint().get_simpoint_interval()
            )
            dump()
            reset()

    @overrides(ScheduledExitEventHandler)
    def _exit_simulation(self) -> bool:
        if self.__class__.warmed_up:
            return True
        else:
            return False


simulator = Simulator(board=board)

# This script STILL NEEDS TO BE TESTED; m5.scheduleTickExitAbsolute() may work
# differently compared to simulator.schedule_max_insts()

# Here, m5.scheduleTickExitAbsolute schedules an exit event for the first
# SimPoint's warmup instructions
m5.scheduleTickExitAbsolute(board.get_simpoint().get_warmup_list()[0])
simulator.run()
