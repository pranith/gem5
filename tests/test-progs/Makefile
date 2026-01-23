# Simple makefile to build the standalone test programs in this directory.

CC ?= gcc
CFLAGS ?= -std=c11 -O2
LDFLAGS ?= -pthread -static

PROGS := acquire_release microbench thrasher

.PHONY: all clean

all: $(PROGS)

acquire_release: acquire_release.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

microbench: microbench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

thrasher: thrasher.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(PROGS)
