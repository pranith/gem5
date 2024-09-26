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
This configuration script shows an example of how to take checkpoints for
SimPoints using the gem5 stdlib. Simpoints are set via a Workload and the
gem5 SimPoint module will calculate where to take checkpoints based of the
SimPoints, SimPoints interval length, and the warmup instruction length.

This scipt builds a simple board with the gem5 stdlib with no cache and a
simple memory structure to take checkpoints. Some of the components, such as
cache hierarchy, can be changed when restoring checkpoints.

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

import argparse
import os
import shlex
import sys
from pathlib import Path

import m5
from m5.objects import *
from m5.util import addToPath

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.classic.no_cache import NoCache
from gem5.components.memory.single_channel import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import (
    BinaryResource,
    Resource,
    SimpointResource,
    obtain_resource,
)
from gem5.resources.workload import Workload
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.exit_event_generators import save_checkpoint_generator
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

m5.util.addToPath("../..")

parser = argparse.ArgumentParser(
    description="An example simpoint workload file path"
)

# The lone arguments is a file path to a directory to store the checkpoints.
parser.add_argument(
    "--checkpoint",
    type=str,
    required=True,
    help="Name of the benchmark to create checkpoints for",
)

parser.add_argument(
    "--checkpoint-dir",
    type=str,
    required=False,
    default="se_checkpoint_folder/",
    help="The top level directory to store the checkpoint.",
)

args = parser.parse_args()

# When taking a checkpoint, the cache state is not saved, so the cache
# hierarchy can be changed completely when restoring from a checkpoint.
# By using NoCache() to take checkpoints, it can slightly improve the
# performance when running in atomic mode, and it will not put any restrictions
# on what people can do with the checkpoints.
cache_hierarchy = NoCache()

# Using simple memory to take checkpoints might slightly imporve the
# performance in atomic mode. The memory structure can be changed when
# restoring from a checkpoint, but the size of the memory must be maintained.
memory = SingleChannelDDR3_1600(size="4GB")

processor = SimpleProcessor(
    cpu_type=CPUTypes.TIMING,
    isa=ISA.ARM,
    # SimPoints only works with one core
    num_cores=1,
)

board = SimpleBoard(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

# board.set_workload(
#    Workload("x86-print-this-15000-with-simpoints")
#
# **Note: This has been removed until we update the resources.json file to
# encapsulate the new Simpoint format.
# Below we set the simpount manually.

spec_dir = "/home/pranith/work/spec2017/benchspec/CPU/{x_workload}/run/run_base_refrate_spec-64.0000"
# spec_dir = "/home/pranith/work/spec2017/benchspec/CPU/{x_workload}/run/run_base_refspeed_spec-64.0000"
spec_speed_workloads = [
    "600.perlbench_s",
    "602.gcc_s",
    "605.mcf_s",
    "620.omnetpp_s",
    "623.xalancbmk_s",
    "625.x264_s",
    "631.deepsjeng_s",
    "641.leela_s",
    "648.exchange2_s",
    "657.xz_s",
]

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
    "500.perlbench_r": "-I./lib checkspam.pl 2500 5 25 11 150 1 1 1 1",
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

spec_arguments = {
    "600.perlbench_s": [
        "-I./lib",
        "checkspam.pl",
        "2500",
        "5",
        "25",
        "11",
        "150",
        "1",
        "1",
        "1",
        "1",
    ],
    "602.gcc_s": [
        "gcc-pp.c",
        "-O5",
        "-fipa-pta",
        "-o",
        "gcc-pp.opts-05_-fipa-pta.s",
    ],
    "605.mcf_s": ["inp.in"],
    "620.omnetpp_s": [
        "-c",
        "General",
        "-f",
        f"{spec_dir}/620.omnetpp_s/omnetpp.ini",
        "-r",
        "0",
    ],
    "623.xalancbmk_s": ["-v", "t5.xml", "xalanc.xsl"],
    "625.x264_s": [
        "--pass",
        1,
        "--stats",
        "x264_stats.log",
        "--bitrate",
        1000,
        "--frames",
        1000,
        "-o",
        "BuckBunny_New.264",
        "BuckBunny.yuv",
        "1280x720",
    ],
    "631.deepsjeng_s": ["ref.txt"],
    "641.leela_s": ["ref.sgf"],
    "648.exchange2_s": [6],
    "657.xz_s": [
        "cld.tar.xz",
        1400,
        "19cf30ae51eddcbefda78dd06014b4b96281456e078ca7c13e1c0c9e6aaea8dff3efb4ad6b0456697718cede6bd5454852652806a657bb56e07d61128434b474",
        "536995164",
        "539938872",
        "6",
    ],
}


def parse_simpoint_file(filename):
    if not Path(filename).exists():
        print(f"Missing file {simpts_file}")
        sys.exit(-1)

    with open(filename) as file:
        return [line.split()[0] for line in file.readlines()]


binary_suffix = "_base.spec-64"
simulator = {}

workloads = args.checkpoint.split()

for workload in workloads:
    workload_dir = spec_dir.format(x_workload=workload)
    print(workload_dir)
    os.chdir(workload_dir)
    # binary_name = spec_rate_binary[workload] + binary_suffix
    binary_name = spec_rate_binary[workload] + binary_suffix

    binary_file = f"{workload_dir}/{binary_name}"

    print(binary_file, spec_rate_args[workload])

    all_args = spec_rate_args[workload].split()
    argv = [binary_file] + all_args

    print(argv)

    workload_resource = BinaryResource(
        local_path=binary_file,
        arguments=argv[1:],
        stdout_file=f"{workload_dir}/{binary_name}.txt",
        stderr_file=f"{workload_dir}/{binary_name}.err",
    )

    simpts_file = f"{workload_dir}/{workload}.simpts"
    simpts_list = [int(e) for e in parse_simpoint_file(simpts_file)]

    weights_file = f"{workload_dir}/{workload}.weights"
    weights_list = [float(e) for e in parse_simpoint_file(weights_file)]

    board.set_se_simpoint_workload(
        workload_resource,
        arguments=argv[1:],
        simpoint=SimpointResource(
            simpoint_interval=20000000,
            simpoint_list=simpts_list,
            weight_list=weights_list,
            warmup_interval=40000000,
        ),
    )
    # board.set_se_binary_workload(workload_resource)

    chkpt_dir = os.path.join(args.checkpoint_dir, workload)
    os.makedirs(chkpt_dir, exist_ok=True)

    simulator[workload] = Simulator(
        board=board,
        on_exit_event={
            # using the SimPoints event generator in the standard library to take
            # checkpoints
            ExitEvent.SIMPOINT_BEGIN: save_checkpoint_generator(chkpt_dir)
        },
    )

    # simulator[workload].override_outdir(Path(chkpt_dir))
    print(f"Starting simulation. Checkpoints written to {chkpt_dir}")
    simulator[workload].run()
