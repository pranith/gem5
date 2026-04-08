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

import os
import shutil
import sys
from pathlib import Path

import m5
from m5.objects import *
from m5.objects import ArmO3CPU
from m5.objects.IndexingPolicies import *
from m5.objects.ReplacementPolicies import *
from m5.util import addToPath

m5.util.addToPath("../../..")

# Normalize stats file name in case argparse left it as a list.
if isinstance(m5.options.stats_file, list):
    m5.options.stats_file = m5.options.stats_file[0]

from m5.stats import (
    dump,
    reset,
)

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.cachehierarchies.abstract_three_level_cache_hierarchy import (
    AbstractThreeLevelCacheHierarchy,
)
from gem5.components.cachehierarchies.classic.abstract_classic_cache_hierarchy import (
    AbstractClassicCacheHierarchy,
)
from gem5.components.cachehierarchies.classic.caches.l1dcache import L1DCache
from gem5.components.cachehierarchies.classic.caches.l1icache import L1ICache
from gem5.components.cachehierarchies.classic.caches.l2cache import L2Cache
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.components.memory import DIMM_DDR5_4400
from gem5.components.processors.base_cpu_core import BaseCPUCore
from gem5.components.processors.base_cpu_processor import BaseCPUProcessor
from gem5.components.processors.cpu_types import CPUTypes
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

multisim.set_num_processes(24)


def _env_on_off(name):
    value = os.environ.get(name)
    if value is None:
        return None
    value = value.strip().lower()
    if value == "on":
        return True
    if value == "off":
        return False
    raise ValueError(f"{name} must be set to 'on' or 'off'")


def _env_workload_filter(name):
    value = os.environ.get(name)
    if value is None:
        return None
    workloads = [item.strip() for item in value.split(",") if item.strip()]
    return set(workloads) if workloads else None


def _env_positive_int(name, default):
    value = os.environ.get(name)
    if value is None:
        return default
    try:
        parsed = int(value)
    except ValueError as exc:
        raise ValueError(f"{name} must be an integer") from exc
    if parsed < 0:
        raise ValueError(f"{name} must be non-negative")
    return parsed


SIMPOINT_INTERVAL = _env_positive_int("MEASURE_INSTS", 200000000)
SIMPOINT_WARMUP = _env_positive_int("WARMUP_INSTS", 100000000)

spec_dir = "/home/pranith/work/spec2017_chkpts_r_arm64_barriers/{x_workload}"

spec_rate_workloads = [
    "500.perlbench_r_checkspam",
    "500.perlbench_r_splitmail",
    "500.perlbench_r_diffmail",
    "502.gcc_r_pp_O3",
    "502.gcc_r_pp_O2",
    "502.gcc_r_ref32_O3",
    "502.gcc_r_ref32_O5",
    "502.gcc_r_smaller",
    "505.mcf_r",
    "520.omnetpp_r",
    "523.xalancbmk_r",
    "525.x264_r",
    "531.deepsjeng_r",
    "541.leela_r",
    "557.xz_r",
]

workload_filter = _env_workload_filter("WORKLOAD_FILTER")
if workload_filter is not None:
    unknown = sorted(workload_filter.difference(spec_rate_workloads))
    if unknown:
        raise ValueError(
            "Unknown workloads in WORKLOAD_FILTER: " + ", ".join(unknown)
        )
    spec_rate_workloads = [
        workload for workload in spec_rate_workloads if workload in workload_filter
    ]

spec_rate_binary = {
    "500.perlbench_r_checkspam": "perlbench_r",
    "500.perlbench_r_splitmail": "perlbench_r",
    "500.perlbench_r_diffmail": "perlbench_r",
    "502.gcc_r_pp_O3": "cpugcc_r",
    "502.gcc_r_pp_O2": "cpugcc_r",
    "502.gcc_r_ref32_O3": "cpugcc_r",
    "502.gcc_r_ref32_O5": "cpugcc_r",
    "502.gcc_r_smaller": "cpugcc_r",
    "505.mcf_r": "mcf_r",
    "520.omnetpp_r": "omnetpp_r",
    "523.xalancbmk_r": "cpuxalan_r",
    "525.x264_r": "x264_r",
    "531.deepsjeng_r": "deepsjeng_r",
    "541.leela_r": "leela_r",
    "557.xz_r": "xz_r",
}

spec_rate_args = {
    "500.perlbench_r_checkspam": "-I./lib checkspam.pl 2500 5 25 11 150 1 1 1 1",
    "500.perlbench_r_splitmail": "-I./lib splitmail.pl 6400 12 26 16 100 0",
    "500.perlbench_r_diffmail": "-I./lib diffmail.pl 4 800 10 17 19 300",
    "502.gcc_r_pp_O3": "gcc-pp.c -O3 -finline-limit=0 -fif-conversion -fif-conversion2 -o gcc-pp.opts-O3_-finline-limit_0_-fif-conversion_-fif-conversion2.s",
    "502.gcc_r_pp_O2": "gcc-pp.c -O2 -finline-limit=36000 -fpic -o gcc-pp.opts-O2_-finline-limit_36000_-fpic.s",
    "502.gcc_r_ref32_O3": "ref32.c -O3 -fselective-scheduling -fselective-scheduling2 -o ref32.opts-O3_-fselective-scheduling_-fselective-scheduling2.s",
    "502.gcc_r_ref32_O5": "ref32.c -O5 -o ref32.opts-O5.s",
    "502.gcc_r_smaller": "gcc-smaller.c -O3 -fipa-pta -o gcc-smaller.opts-O3_-fipa-pta.s",
    "505.mcf_r": "inp.in",
    "520.omnetpp_r": "-c General -r 0",
    "523.xalancbmk_r": "-v t5.xml xalanc.xsl",
    "525.x264_r": "--pass 1 --stats x264_stats.log --bitrate 1000 --frames 1000 -o BuckBunny_New.264 BuckBunny.yuv 1280x720",
    "531.deepsjeng_r": "ref.txt",
    "541.leela_r": "ref.sgf",
    "557.xz_r": "cld.tar.xz 160 19cf30ae51eddcbefda78dd06014b4b96281456e078ca7c13e1c0c9e6aaea8dff3efb4ad6b0456697718cede6bd5454852652806a657bb56e07d61128434b474 59796407 61004416 6",
}


def resolve_workload_args(workload, workload_dir):
    all_args = spec_rate_args[workload].split()

    if workload.startswith("500.perlbench_r_"):
        resolved = []
        for arg in all_args:
            if arg == "-I./lib":
                resolved.append(f"-I{workload_dir}/lib")
            elif arg.endswith(".pl"):
                resolved.append(f"{workload_dir}/{arg}")
            else:
                resolved.append(arg)
        return resolved

    if workload == "525.x264_r":
        return [
            "--pass",
            "1",
            "--stats",
            f"{workload_dir}/x264_stats.log",
            "--bitrate",
            "1000",
            "--frames",
            "1000",
            "-o",
            f"{workload_dir}/BuckBunny_New.264",
            f"{workload_dir}/BuckBunny.yuv",
            "1280x720",
        ]

    return all_args


class CustomCore(BaseCPUCore):
    def __init__(self):
        super().__init__(ArmO3CPU(), ISA.ARM)

        # Merge buffer + versioning knobs (mirroring rancho_o3.py options).
        self.core.speculativeBarrierIssue = True
        self.core.useMergeBuffer = True
        self.core.mergeBufferEntries = 32
        self.core.mergeBufferPrefetch = True
        versioning = _env_on_off("VERSIONING")
        if versioning is None:
            self.core.enableVersioning = True
        else:
            self.core.enableVersioning = versioning
        zfence = _env_on_off("ZFENCE")
        if zfence is not None:
            self.core.zfenceEnable = zfence
        self.core.optimizeStoreRelease = False
        self.core.optimizeAcquirePC = False
        self.core.safeStlfLoadsBypassMBDrain = True
        self.core.safeCacheLoadsBypassMBDrain = True


class CustomProcessor(BaseCPUProcessor):
    def __init__(self):
        cores = [CustomCore()]
        super().__init__(cores)


class PrivateL1PrivateL2SharedL3CacheHierarchy(
    AbstractClassicCacheHierarchy, AbstractThreeLevelCacheHierarchy
):
    """Classic private-L1, private-L2, shared-L3 hierarchy."""

    def __init__(
        self,
        l1d_size: str,
        l1i_size: str,
        l2_size: str,
        l3_size: str,
    ) -> None:
        AbstractClassicCacheHierarchy.__init__(self)
        AbstractThreeLevelCacheHierarchy.__init__(
            self,
            l1i_size=l1i_size,
            l1i_assoc=8,
            l1d_size=l1d_size,
            l1d_assoc=8,
            l2_size=l2_size,
            l2_assoc=16,
            l3_size=l3_size,
            l3_assoc=16,
        )

        self.membus = SystemXBar(width=64)
        self.membus.badaddr_responder = BadAddr()
        self.membus.default = self.membus.badaddr_responder.pio

    def get_mem_side_port(self):
        return self.membus.mem_side_ports

    def get_cpu_side_port(self):
        return self.membus.cpu_side_ports

    def incorporate_cache(self, board: AbstractBoard) -> None:
        board.connect_system_port(self.membus.cpu_side_ports)

        for _, port in board.get_mem_ports():
            self.membus.mem_side_ports = port

        self.l3_bus = L2XBar()
        l3_node = self.add_root_child(
            "l3-cache",
            L2Cache(
                size=self._l3_size,
                assoc=self._l3_assoc,
                tag_latency=20,
                data_latency=20,
                response_latency=20,
                clusivity="mostly_excl",
            ),
        )
        self.l3_bus.mem_side_ports = l3_node.cache.cpu_side
        self.membus.cpu_side_ports = l3_node.cache.mem_side

        num_cores = board.get_processor().get_num_cores()
        self._l2buses = []
        for i in range(num_cores):
            bus = L2XBar()
            setattr(self, f"l2_bus_{i}", bus)
            self._l2buses.append(bus)

        for i, cpu in enumerate(board.get_processor().get_cores()):
            l2_node = l3_node.add_child(
                f"l2-cache-{i}",
                L2Cache(size=self._l2_size, assoc=self._l2_assoc),
            )
            l1i_node = l2_node.add_child(
                f"l1i-cache-{i}", L1ICache(size=self._l1i_size)
            )
            l1d_node = l2_node.add_child(
                f"l1d-cache-{i}", L1DCache(size=self._l1d_size)
            )

            self._l2buses[i].mem_side_ports = l2_node.cache.cpu_side
            self.l3_bus.cpu_side_ports = l2_node.cache.mem_side

            l1i_node.cache.mem_side = self._l2buses[i].cpu_side_ports
            l1d_node.cache.mem_side = self._l2buses[i].cpu_side_ports

            cpu.connect_icache(l1i_node.cache.cpu_side)
            cpu.connect_dcache(l1d_node.cache.cpu_side)
            cpu.connect_walker_ports(
                self._l2buses[i].cpu_side_ports,
                self._l2buses[i].cpu_side_ports,
            )

            if board.get_processor().get_isa() == ISA.X86:
                int_req_port = self.membus.mem_side_ports
                int_resp_port = self.membus.cpu_side_ports
                cpu.connect_interrupt(int_req_port, int_resp_port)
            else:
                cpu.connect_interrupt()

        if board.has_coherent_io():
            self._iocache = Cache(
                assoc=8,
                tag_latency=50,
                data_latency=50,
                response_latency=50,
                mshrs=20,
                size="1KiB",
                tgts_per_mshr=12,
                addr_ranges=board.mem_ranges,
            )
            self._iocache.mem_side = self.membus.cpu_side_ports
            self._iocache.cpu_side = board.get_mem_side_coherent_io_port()


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


binary_suffix = "_base.barrier-m64"


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

    all_args = resolve_workload_args(workload, workload_dir)
    argv = [binary_file] + all_args

    # print(argv)

    workload_name = workload.split(".", 1)[-1]

    simpts_file = f"{workload_dir}/{workload}.simpts"
    simpts_list = [int(e) for e in parse_simpoint_file(simpts_file)]

    weights_file = f"{workload_dir}/{workload}.weights"
    weights_list = [float(e) for e in parse_simpoint_file(weights_file)]

    shutil.copy(weights_file, m5.options.outdir)

    chkpt_dirs = get_checkpoint_list(workload_dir)

    chkpt_idx = 0
    for chkpt in chkpt_dirs:

        # The cache hierarchy can be different from the cache hierarchy used in taking
        # the checkpoints
        cache_hierarchy = PrivateL1PrivateL2SharedL3CacheHierarchy(
            l1d_size="32KiB",
            l1i_size="32KiB",
            l2_size="2MiB",
            l3_size="16MiB",
        )

        # The memory structure can be different from the memory structure used in
        # taking the checkpoints, but the size of the memory must be maintained
        memory = DIMM_DDR5_4400(size="4GiB")

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
                simpoint_interval=SIMPOINT_INTERVAL,
                simpoint_list=simpts_list,
                weight_list=weights_list,
                warmup_interval=SIMPOINT_WARMUP,
            ),
            checkpoint=CheckpointResource(local_path=chkpt),
        )
        # Ensure relative file accesses resolve inside the workload directory.
        for core in processor.get_cores():
            if not hasattr(core, "core") or not hasattr(core.core, "workload"):
                continue
            workload_obj = core.core.workload
            if hasattr(workload_obj, "__iter__"):
                for proc in workload_obj:
                    proc.cwd = workload_dir
            else:
                workload_obj.cwd = workload_dir

        chkpt_id = f"chkpt_{workload_name}_{chkpt_idx}"
        simulator = Simulator(
            board=board,
            id=chkpt_id,
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
