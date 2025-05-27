"""
The original was
gem5-6th-worktree/gem5-dev/staging-24.1.1.0/multisim-testing-sprint-2/processor-switch/x86-npb-ind-handlers/processor-switch-x86-npb-ind-handlers.py
"""

import m5

import gem5.utils.multisim as multisim
from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.classic.no_cache import NoCache
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import (
    CPUTypes,
    get_cpu_type_from_str,
)
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import (
    ISA,
    get_isa_from_str,
)
from gem5.resources.resource import (
    DiskImageResource,
    KernelResource,
    obtain_resource,
)
from gem5.simulate.exit_handler import (
    AfterBootExitHandler,
    ExitHandler,
    KernelBootedExitHandler,
    WorkBeginExitHandler,
)
from gem5.simulate.simulator import Simulator
from gem5.utils.override import overrides

NUM_PROCESSES = 20

multisim.set_num_processes(NUM_PROCESSES)


import argparse

parser = argparse.ArgumentParser()

parser.add_argument("--num-cores", type=int, required=True)

args = parser.parse_args()


class AfterBootPrintStatus(AfterBootExitHandler):
    def _process(self, simulator: "Simulator") -> None:
        # checkpoint_path = f"./x86-{args.num_cores}core-systemboot-checkpoint"
        checkpoint_path = (
            f"./x86-ubuntu-24.04-boot-{args.num_cores}-core-checkpoint"
        )
        print(
            f"Taking a checkpoint at {checkpoint_path}",
        )
        simulator.save_checkpoint(checkpoint_path)
        print("Done taking a checkpoint")
        print("Scheduling exit in 10 million ticks!")
        m5.scheduleTickExitFromCurrent(
            10_000_000
        )  # exit 10 million ticks after system boot

    def _exit_simulation(self) -> bool:
        return False


cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
    l1d_size="16KiB",
    l1i_size="16KiB",
    l2_size="256KiB",
)
memory = SingleChannelDDR3_1600(size="3GiB")
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.KVM,
    isa=ISA.X86,
    num_cores=args.num_cores,
)

board = X86Board(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

# board.set_kernel_disk_workload(
#     kernel=KernelResource(
#         "/projects/gem5/new-base-imgs-w-hypercalls/x86-disk-image-24-04/vmlinux-x86-ubuntu-6.8.0-52-generic"
#     ),
#     disk_image=DiskImageResource(
#         "/projects/gem5/new-base-imgs-w-hypercalls/disk-image-x86-npb/x86-ubuntu-npb"
#     ),
#     kernel_args=[
#         "earlyprintk=ttyS0",
#         "console=ttyS0",
#         "lpj=7999923",
#         "root=/dev/sda2",
#     ],
#     readfile_contents=f"/home/gem5/NPB3.4-OMP/bin/cg.S.x; sleep 5;",
# )
board.set_kernel_disk_workload(
    kernel=obtain_resource(
        "x86-linux-kernel-6.8.0-52-generic", resource_version="1.0.0"
    ),
    disk_image=obtain_resource(
        "x86-ubuntu-24.04-npb-img", resource_version="5.0.0"
    ),
    kernel_args=[
        "earlyprintk=ttyS0",
        "console=ttyS0",
        "lpj=7999923",
        "root=/dev/sda2",
    ],
    readfile_contents=f"/home/gem5/NPB3.4-OMP/bin/cg.S.x; sleep 5;",
)

simulator = Simulator(board=board)
simulator.run()
