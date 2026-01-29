Lock manager benchmark
======================

This benchmark models a database-style lock manager to stress acquire/release
operations and optional barrier synchronization across worker threads.

Build
-----
make -C tests/test-progs/db_lock_bench

Run
---
tests/test-progs/db_lock_bench/lock_manager_bench \
  [threads] [txns_per_thread] [locks_per_txn] [table_size] [barrier_interval]

Parameters
----------
- threads: Worker threads (default 4).
- txns_per_thread: Transactions per thread (default 200000).
- locks_per_txn: Locks acquired per transaction (default 8).
- table_size: Number of lockable records (default 16384).
- barrier_interval: Barrier every N transactions (default 0, disabled).

Examples
--------
- Baseline lock-heavy run:
  tests/test-progs/db_lock_bench/db_lock_bench 2 200000 12 65536 0
- Add periodic epoch barriers:
  tests/test-progs/db_lock_bench/db_lock_bench 2 200000 12 65536 1000
