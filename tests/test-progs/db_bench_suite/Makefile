# Build all workloads in the db benchmark suite.

CC ?= gcc
CFLAGS ?= -std=c11 -O2
LDFLAGS ?= -pthread -static

PROGS := \
	wal_group_commit/wal_group_commit_bench \
	buffer_pool_latch/buffer_pool_latch_bench \
	work_stealing/work_stealing_bench \
	network_ring/network_ring_bench \
	storage_allocator/storage_allocator_bench \
	release_publish_stream/release_publish_stream_bench \
	hpc_barrier/hpc_barrier_bench \
	db_lock_bench/db_lock_bench

.PHONY: all clean

all: $(PROGS)

wal_group_commit/wal_group_commit_bench: wal_group_commit/wal_group_commit_bench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

buffer_pool_latch/buffer_pool_latch_bench: buffer_pool_latch/buffer_pool_latch_bench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

work_stealing/work_stealing_bench: work_stealing/work_stealing_bench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

network_ring/network_ring_bench: network_ring/network_ring_bench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

storage_allocator/storage_allocator_bench: storage_allocator/storage_allocator_bench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

release_publish_stream/release_publish_stream_bench: release_publish_stream/release_publish_stream_bench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

hpc_barrier/hpc_barrier_bench: hpc_barrier/hpc_barrier_bench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

db_lock_bench/db_lock_bench: db_lock_bench/db_lock_bench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(PROGS)
