# Simple makefile to build the standalone test programs in this directory.

CC ?= gcc
CFLAGS ?= -std=c11 -O2
LDFLAGS ?= -pthread -static

PROGS := acquire_release microbench thrasher store_release_bench mb_barrier_store
DIRS  := db_bench_suite

.PHONY: all clean

all: $(PROGS) $(DIRS)

acquire_release: acquire_release.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

microbench: microbench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

thrasher: thrasher.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

store_release_bench: store_release_bench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

mb_barrier_store: mb_barrier_store.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(PROGS)
	$(MAKE) -C db_bench_suite clean

.PHONY: db_bench_suite
db_bench_suite:
	$(MAKE) -C db_bench_suite
