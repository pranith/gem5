# Simple makefile for the lock manager benchmark.

CC ?= gcc
CFLAGS ?= -std=c11 -O2
LDFLAGS ?= -pthread

PROG := db_lock_bench

.PHONY: all clean

all: $(PROG)

$(PROG): db_lock_bench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(PROG)
