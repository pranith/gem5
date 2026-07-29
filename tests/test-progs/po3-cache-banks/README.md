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
Merge-buffer statistics are under `system.cpu.lsq0.mb*`.
