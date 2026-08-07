# Copyright (c) 2026
# All rights reserved.

"""A classic hierarchy with private L1/L2 caches and a shared L3 cache."""

from typing import Optional

from m5.objects import (
    BadAddr,
    BaseCPU,
    BaseXBar,
    Cache,
    L2XBar,
    SystemXBar,
)
from m5.params import Port

from ....isas import ISA
from ....utils.override import overrides
from ...boards.abstract_board import AbstractBoard
from ..abstract_cache_hierarchy import AbstractCacheHierarchy
from ..abstract_three_level_cache_hierarchy import (
    AbstractThreeLevelCacheHierarchy,
)
from .abstract_classic_cache_hierarchy import AbstractClassicCacheHierarchy
from .caches.l1dcache import L1DCache
from .caches.l1icache import L1ICache
from .caches.l2cache import L2Cache


class PrivateL1PrivateL2SharedL3CacheHierarchy(
    AbstractClassicCacheHierarchy, AbstractThreeLevelCacheHierarchy
):
    """Private L1I/L1D and L2 caches feeding one shared last-level cache."""

    def __init__(
        self,
        l1d_size: str,
        l1i_size: str,
        l2_size: str,
        l3_size: str,
        l1_latency: int = 4,
        membus: Optional[BaseXBar] = None,
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
        self._l1_latency = l1_latency
        self.membus = membus if membus else self._get_default_membus()

    @staticmethod
    def _get_default_membus() -> SystemXBar:
        membus = SystemXBar(width=64)
        membus.badaddr_responder = BadAddr()
        membus.default = membus.badaddr_responder.pio
        return membus

    @overrides(AbstractClassicCacheHierarchy)
    def get_mem_side_port(self) -> Port:
        return self.membus.mem_side_ports

    @overrides(AbstractClassicCacheHierarchy)
    def get_cpu_side_port(self) -> Port:
        return self.membus.cpu_side_ports

    @overrides(AbstractCacheHierarchy)
    def incorporate_cache(self, board: AbstractBoard) -> None:
        board.connect_system_port(self.membus.cpu_side_ports)
        for _, port in board.get_mem_ports():
            self.membus.mem_side_ports = port

        num_cores = board.get_processor().get_num_cores()
        self.l2buses = [L2XBar() for _ in range(num_cores)]
        self.l3bus = L2XBar()

        l3_cache = L2Cache(
            size=self._l3_size,
            assoc=self._l3_assoc,
            tag_latency=20,
            data_latency=45,
            mshrs=32,
            tgts_per_mshr=16,
        )
        if hasattr(l3_cache, "early_lock_coherence_point"):
            l3_cache.early_lock_coherence_point = True
        l3_node = self.add_root_child("l3-cache", l3_cache)
        self.l3bus.mem_side_ports = l3_node.cache.cpu_side
        self.membus.cpu_side_ports = l3_node.cache.mem_side

        for index, cpu in enumerate(board.get_processor().get_cores()):
            l2_node = l3_node.add_child(
                f"l2-cache-{index}",
                L2Cache(size=self._l2_size, assoc=self._l2_assoc),
            )
            l1i_node = l2_node.add_child(
                f"l1i-cache-{index}",
                L1ICache(
                    size=self._l1i_size,
                    assoc=self._l1i_assoc,
                    tag_latency=self._l1_latency,
                    data_latency=self._l1_latency,
                ),
            )
            l1d_node = l2_node.add_child(
                f"l1d-cache-{index}",
                L1DCache(
                    size=self._l1d_size,
                    assoc=self._l1d_assoc,
                    tag_latency=self._l1_latency,
                    data_latency=self._l1_latency,
                ),
            )

            self.l2buses[index].mem_side_ports = l2_node.cache.cpu_side
            self.l3bus.cpu_side_ports = l2_node.cache.mem_side
            l1i_node.cache.mem_side = self.l2buses[index].cpu_side_ports
            l1d_node.cache.mem_side = self.l2buses[index].cpu_side_ports

            cpu.connect_icache(l1i_node.cache.cpu_side)
            cpu.connect_dcache(l1d_node.cache.cpu_side)
            self._connect_table_walker(index, cpu)

            if board.get_processor().get_isa() == ISA.X86:
                cpu.connect_interrupt(
                    self.membus.mem_side_ports,
                    self.membus.cpu_side_ports,
                )
            else:
                cpu.connect_interrupt()

        if board.has_coherent_io():
            self._setup_io_cache(board)

    def _connect_table_walker(self, cpu_id: int, cpu: BaseCPU) -> None:
        cpu.connect_walker_ports(
            self.l2buses[cpu_id].cpu_side_ports,
            self.l2buses[cpu_id].cpu_side_ports,
        )

    def _setup_io_cache(self, board: AbstractBoard) -> None:
        self.iocache = Cache(
            assoc=8,
            tag_latency=50,
            data_latency=50,
            response_latency=50,
            mshrs=20,
            size="1KiB",
            tgts_per_mshr=12,
            addr_ranges=board.mem_ranges,
        )
        self.iocache.mem_side = self.membus.cpu_side_ports
        self.iocache.cpu_side = board.get_mem_side_coherent_io_port()
