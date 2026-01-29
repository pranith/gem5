Work-stealing scheduler benchmark
=================================

Models a runtime task scheduler with per-thread deques and cross-thread steals.
The hot path uses acquire/release ordering on deque indices and the steal path
adds contention and synchronization overhead.

Examples
--------
./work_stealing_bench 2 200000 4096
