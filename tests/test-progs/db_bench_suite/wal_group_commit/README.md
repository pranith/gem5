WAL group-commit benchmark
==========================

Models a database write-ahead log where multiple workers append log records and
synchronize on a group commit boundary. This stresses acquire/release ordering
around publishing log records and the barrier-like commit synchronization.

Examples
--------
./wal_group_commit_bench 2 200000 8 1048576
