# Pipelined O3 (PO3)

PO3 is a separate timing CPU model derived from gem5's O3 CPU. It adds
configurable, explicit pipeline stages while keeping the existing O3 model
available for comparison. The ISA-specific classes are `ArmPO3CPU`,
`X86PO3CPU`, `RiscvPO3CPU`, `MipsPO3CPU`, `PowerPO3CPU`, and `SparcPO3CPU`.
The standard library exposes the model as `CPUTypes.PO3`.

`BasePO3CPU.py` defines the stage switches, latencies, and widths.
`PO3Config.py` applies the default four-wide processor configuration to each
ISA-specific class. PO3 models the front end, decode, rename, dispatch,
issue selection, register read, execute, writeback, memory access, and commit
as separately configurable stages. Like O3, it requires a timing memory
system and caches.

PO3 also supports a configurable number of line-interleaved D-cache banks.
Reads and writes to the same bank in the same cycle conflict; throughput for
accesses of the same kind is controlled by the cache port counts. Partial-line
stores occupy a read, merge, and write sequence. The optional store merge
buffer is disabled by default (`useMergeBuffer = False`). When enabled, it
combines committed stores by cache line and drains through the modeled banks.
Memory-order versioning and speculative barrier issue are not enabled by
this model.

The ARM CPU tests exercise PO3 with Bubblesort and FloatMM. For a local
banking smoke test, build the benchmark in
`tests/test-progs/po3-cache-banks` and run it with
`configs/example/arm/po3_cache_banks.py` as described in that benchmark's
README.
