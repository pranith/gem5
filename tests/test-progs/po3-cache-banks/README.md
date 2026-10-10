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
    -o atomic_merge_buffer atomic_merge_buffer.S
```

Run, selecting the number of modeled banks:

```sh
build/ARM/gem5.opt configs/example/arm/po3_cache_banks.py \
    bank_conflict 4
```

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

To exercise release dependencies in the merge buffer, build `release_order.c`
with the same compiler flags and run:

```sh
build/ARM/gem5.opt configs/example/arm/po3_cache_banks.py \
    release_order 4 --merge-buffer --optimize-store-release
```

The run should exit normally. `system.cpu.lsq0.mbReleaseWaitCycles` counts
cycles that release entries waited for older drains.

`atomic_merge_buffer` places a regular store in the merge buffer before an
LSE atomic add to the same cache line. The atomic must return the buffered
store's value and update memory before the test exits:

```sh
build/ARM/gem5.opt configs/example/arm/po3_cache_banks.py \
    atomic_merge_buffer 4 --merge-buffer \
    --merge-buffer-entries 4 --merge-buffer-retire-cycles 4
```

A successful run exits normally; a bad atomic result loops until the
configured maximum tick count.
