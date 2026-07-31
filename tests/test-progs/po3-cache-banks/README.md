# PO3 cache-bank conflict microbenchmark

This freestanding AArch64 benchmark issues eight independent loads followed by
eight partial stores. Every operand occupies a separate 64-byte cache line.
Each store stream is 64 lines above its corresponding load stream, so the
addresses have the same lower six bank-index bits and alias in configurations
with 64 or fewer banks.

Build:

```sh
aarch64-linux-gnu-gcc -O2 -nostdlib -static -fno-stack-protector \
    -Wl,-e,_start -o bank_conflict bank_conflict.c
aarch64-linux-gnu-gcc -nostdlib -static -Wl,-e,_start \
    -o l1_eviction_hazard l1_eviction_hazard.S
aarch64-linux-gnu-gcc -nostdlib -static -Wl,-e,_start \
    -o atomic_merge_buffer atomic_merge_buffer.S
```

Run, selecting the number of modeled banks:

```sh
build/ARM/gem5.opt configs/example/arm/po3_cache_banks.py \
    bank_conflict 4
```

Select decode-time RC or TSO ordering tags with `--memory-model rc` or
`--memory-model tso`. RC advances the tag at fences; TSO assigns consecutive
tags to ordinary stores and advances the load tag at fences.

Enable the PO3 merge buffer and select its capacity and retirement delay:

```sh
build/ARM/gem5.opt configs/example/arm/po3_cache_banks.py \
    bank_conflict 4 --merge-buffer --merge-buffer-entries 32 \
    --merge-buffer-retire-cycles 64
```

The relevant statistic is `system.cpu.loadStoreBankConflicts`.
`loadPipeReadPortUses` and `loadStorePipe[01]ReadPortUses` report traffic on
the three pipe-mapped cache read ports. `mergeBufferWritePortUses` and
`writePortUseCycles[01]` report use of the merge buffer's single cache write
port.
Merge-buffer statistics are under `system.cpu.lsq0.mb*`.
`mbLoadStorePipe[01]Writes` counts stores entering the merge buffer from each
load/store pipe, while `mbDualPipeWriteCycles` counts cycles in which both
input ports were used.

`l1_eviction_hazard` delays a program-order older load while younger loads
overflow one L1D set. In TSO mode,
`system.cpu.lsq0.tsoL1EvictionHazards` and
`system.cpu.lsq0.tsoLoadCompletionReschedules` should both be nonzero. In RC
mode both remain zero.

`atomic_merge_buffer` places a regular store in the merge buffer before an
LSE atomic add to the same cache line. The atomic must return the buffered
store's value and update memory before the test exits:

```sh
build/ARM/gem5.opt configs/example/arm/po3_cache_banks.py \
    atomic_merge_buffer 4 --memory-model rc --merge-buffer \
    --merge-buffer-entries 4 --merge-buffer-retire-cycles 4
```

Run it in both RC and TSO modes. A successful run exits with
`exiting with last active thread context`; a bad atomic result loops until the
configured maximum tick count.
