# Simple makefile to build the standalone test programs in this directory.

CC ?= gcc
CFLAGS ?= -std=c11 -O2
LDFLAGS ?= -pthread -static

PROGS := acquire_release microbench thrasher store_release_bench db_bench_suite

.PHONY: all clean

all: $(PROGS)

acquire_release: acquire_release.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

microbench: microbench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

thrasher: thrasher.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

store_release_bench: store_release_bench.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

clean:
	rm -f $(PROGS)
	$(MAKE) -C db_lock_bench clean
	$(MAKE) -C db_bench_suite clean

.PHONY: db_bench_suite
db_bench_suite:
	$(MAKE) -C db_bench_suite
