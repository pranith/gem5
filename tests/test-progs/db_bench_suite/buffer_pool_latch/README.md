Buffer pool latch benchmark
===========================

Models a database buffer pool with hot and cold pages. Threads repeatedly pin
and update pages under lightweight latches, stressing lock acquire/release on
contended hot pages and regular accesses on cold pages.

Examples
--------
./buffer_pool_latch_bench 2 300000 65536 15
