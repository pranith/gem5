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
was taken for SimPoints in the
configs/example/gem5_library/checkpoints/simpoints-se-checkpoint.py.
The SimPoints, SimPoints interval length, and the warmup instruction length
are passed into the SimPoint module, so the SimPoint object will store and
calculate the warmup instruction length for each SimPoints based on the
available instructions before reaching the start of the SimPoint. With the
Simulator module, exit event will be generated to stop when the warmup session
ends and the SimPoints interval ends.

This script builds a more complex board than the board used for taking
checkpoint.

Usage
-----

```
scons build/X86/gem5.opt
./build/X86/gem5.opt \
    configs/example/gem5_library/checkpoints/simpoints-se-checkpoint.py

./build/X86/gem5.opt \
    configs/example/gem5_library/checkpoints/simpoints-se-restore.py
```

"""

import shutil
from pathlib import Path

import m5
from m5.objects import *
from m5.objects import ArmO3CPU
from m5.objects.IndexingPolicies import *
from m5.objects.ReplacementPolicies import *

from m5.util import addToPath

m5.util.addToPath("../../..")

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

from gem5.components.processors.base_cpu_core import BaseCPUCore
from gem5.components.processors.base_cpu_processor import BaseCPUProcessor

from gem5.isas import ISA
from gem5.resources.resource import (
    BinaryResource,
    CheckpointResource,
    SimpointResource,
    obtain_resource,
)
from gem5.resources.workload import Workload
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

requires(isa_required=ISA.ARM)

import gem5.utils.multisim as multisim

multisim.set_num_processes(48)

spec_dir = "/home/pranith/work/spec2017_chkpts_r_arm64/{x_workload}"

spec_rate_workloads = [
    "500.perlbench_r",
    "502.gcc_r",
    "505.mcf_r",
    "520.omnetpp_r",
    "523.xalancbmk_r",
    "525.x264_r",
    "531.deepsjeng_r",
    "541.leela_r",
    "548.exchange2_r",
    "557.xz_r",
]

spec_rate_binary = {
    "500.perlbench_r": "perlbench_r",
    "502.gcc_r": "cpugcc_r",
    "505.mcf_r": "mcf_r",
    "520.omnetpp_r": "omnetpp_r",
    "523.xalancbmk_r": "cpuxalan_r",
    "525.x264_r": "x264_r",
    "531.deepsjeng_r": "deepsjeng_r",
    "541.leela_r": "leela_r",
    "548.exchange2_r": "exchange2_r",
    "557.xz_r": "xz_r",
}

spec_rate_args = {
    "500.perlbench_r": "-I/home/pranith/work/spec2017_chkpts_r_arm64/500.perlbench_r/lib checkspam.pl 2500 5 25 11 150 1 1 1 1",
    "502.gcc_r": "gcc-pp.c -O3 -finline-limit=0 -fif-conversion -fif-conversion2 -o gcc-pp.opts-O3_-finline-limit_0_-fif-conversion_-fif-conversion2.s",
    "505.mcf_r": "inp.in",
    "520.omnetpp_r": "-c General -r 0",
    "523.xalancbmk_r": "-v t5.xml xalanc.xsl",
    "525.x264_r": "--pass 1 --stats x264_stats.log --bitrate 1000 --frames 1000 -o BuckBunny_New.264 BuckBunny.yuv 1280x720",
    "531.deepsjeng_r": "ref.txt",
    "541.leela_r": "ref.sgf",
    "548.exchange2_r": "6",
    "557.xz_r": "cld.tar.xz 160 19cf30ae51eddcbefda78dd06014b4b96281456e078ca7c13e1c0c9e6aaea8dff3efb4ad6b0456697718cede6bd5454852652806a657bb56e07d61128434b474 59796407 61004416 6",
}


class Rancho_BTB(SimpleBTB):
    numEntries = 256
    instShiftAmt = 2
    associativity = 4
    tagBits = 18
    btbReplPolicy = NRURP()
    btbIndexingPolicy = BTBSetAssociative(
        num_entries = Parent.numEntries,
        set_shift = Parent.instShiftAmt,
        assoc=Parent.associativity,
        tag_bits=Parent.tagBits
    )


class Rancho_BP(TournamentBP):
    btb = Rancho_BTB()
    ras = ReturnAddrStack(numEntries=8)
    localPredictorSize = 64
    localCtrBits = 2
    localHistoryTableSize = 64
    globalPredictorSize = 1024
    globalCtrBits = 2
    choicePredictorSize = 1024
    choiceCtrBits = 2
    instShiftAmt = 2


class CustomCore(BaseCPUCore):
    def __init__(self):
        super().__init__(ArmO3CPU(), ISA.ARM)

        self.core.branchPred = Rancho_BP()


class CustomProcessor(BaseCPUProcessor):
    def __init__(self):
        cores = [CustomCore()]
        super().__init__(cores)


def parse_simpoint_file(filename):
    if not Path(filename).exists():
        print(f"Missing file {simpts_file}")
        sys.exit(-1)

    with open(filename) as file:
        return [line.split()[0] for line in file.readlines()]


def max_inst():
    warmed_up = False
    if warmed_up:
        print("end of SimPoint interval")
        dump()
        return True
    else:
        print("end of warmup, starting to simulate SimPoint")
        warmed_up = True
        # Schedule a MAX_INSTS exit event during the simulation
        max_instructions = board.get_simpoint().get_simpoint_interval()
        print(max_instructions)
        simulator.schedule_max_insts(
            board.get_simpoint().get_simpoint_interval()
        )
        # dump()
        reset()
        return False


def get_checkpoint_list(x_path):
    dir_entries = os.listdir(x_path)
    cpt_dirs = [
        os.path.join(x_path, entry)
        for entry in dir_entries
        if os.path.isdir(os.path.join(x_path, entry))
        and entry.startswith("cpt")
    ]

    return cpt_dirs


binary_suffix = "_base.spec-64"


class CheckpointRun:

    def __init__(self, name, simulator, board):
        self.name = name
        self.simulator = simulator
        self.board = board

    def simpoint_warmup_end(self):
        print(
            "\n\n\n################## end of warmup, starting to simulate SimPoint"
        )
        self.simulator.schedule_max_insts(
            self.board.get_simpoint().get_simpoint_interval()
        )
        # dump()
        reset()

        return False

    def simpoint_interval_end(self):
        print("\n\n\n################## end of SimPoint interval\n\n\n")
        dump()

        return True


for workload in spec_rate_workloads:
    workload_dir = spec_dir.format(x_workload=workload)
    # print(workload_dir)
    # os.chdir(workload_dir)
    # binary_name = spec_rate_binary[workload] + binary_suffix
    binary_name = spec_rate_binary[workload] + binary_suffix

    binary_file = f"{workload_dir}/{binary_name}"

    # print(binary_file, spec_rate_args[workload])

    all_args = spec_rate_args[workload].split()
    argv = [binary_file] + all_args

    # print(argv)

    workload_name = spec_rate_binary[workload]

    simpts_file = f"{workload_dir}/{workload}.simpts"
    simpts_list = [int(e) for e in parse_simpoint_file(simpts_file)]

    weights_file = f"{workload_dir}/{workload}.weights"
    weights_list = [float(e) for e in parse_simpoint_file(weights_file)]

    import m5

    shutil.copy(weights_file, m5.options.outdir)

    chkpt_dirs = get_checkpoint_list(workload_dir)

    chkpt_idx = 0
    for chkpt in chkpt_dirs:

        # The cache hierarchy can be different from the cache hierarchy used in taking
        # the checkpoints
        cache_hierarchy = PrivateL1PrivateL2WalkCacheHierarchy(
            l1d_size="32KiB",
            l1i_size="32KiB",
            l2_size="256KiB",
        )

        # The memory structure can be different from the memory structure used in
        # taking the checkpoints, but the size of the memory must be maintained
        memory = DualChannelDDR4_2400(size="4GiB")

        workload_resource = BinaryResource(
            local_path=binary_file,
            arguments=argv[1:],
            stdout_file=f"{chkpt}/{binary_name}.txt",
            stderr_file=f"{chkpt}/{binary_name}.err",
        )

        processor = CustomProcessor()

        board = SimpleBoard(
            clk_freq="3GHz",
            processor=processor,
            memory=memory,
            cache_hierarchy=cache_hierarchy,
        )

        board.set_se_simpoint_workload(
            workload_resource,
            arguments=argv[1:],
            simpoint=SimpointResource(
                simpoint_interval=200000000,
                # simpoint_interval=20000,
                simpoint_list=simpts_list,
                weight_list=weights_list,
                warmup_interval=50000000,
                # warmup_interval=5000,
            ),
            checkpoint=CheckpointResource(local_path=chkpt),
        )

        simulator = Simulator(
            board=board,
            id=f"chkpt_{workload_name}_{chkpt_idx}",
        )

        chkpt_run = CheckpointRun(chkpt, simulator, board)

        on_exit_event = {
            ExitEvent.MAX_INSTS: (
                func()
                for func in [
                    chkpt_run.simpoint_warmup_end,
                    chkpt_run.simpoint_interval_end,
                ]
            )
        }

        simulator.set_on_exit_event(on_exit_event)
        simulator.schedule_max_insts(board.get_simpoint().get_warmup_list()[0])
        multisim.add_simulator(simulator)
        chkpt_idx = chkpt_idx + 1

        # break

    # break

    # simulator[workload].run()
