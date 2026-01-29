Storage allocator benchmark
===========================

Models a concurrent allocator with per-thread free lists and a shared global
pool. Threads use acquire/release to refill or return spans, stressing lock and
barrier-like synchronization under contention.

Examples
--------
./storage_allocator_bench 2 200000 65536 32 64
