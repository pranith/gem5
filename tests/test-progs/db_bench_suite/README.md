DB benchmark suite
==================

Collection of microbenchmarks focused on acquire/release and barrier behavior.

Workloads
---------
- WAL group commit: wal_group_commit/
- Buffer pool latch: buffer_pool_latch/
- Work stealing: work_stealing/
- Network ring (MPSC): network_ring/
- Storage allocator: storage_allocator/
- Release publish stream: release_publish_stream/
- HPC barrier: hpc_barrier/

Build
-----
make -C tests/test-progs/db_bench_suite

Run
---
Each workload has a `README.md` with example commands and parameters.
