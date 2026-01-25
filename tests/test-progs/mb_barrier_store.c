#define _XOPEN_SOURCE 700
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#define CACHELINE 64
#define DEFAULT_ITERS 2000000
#define DEFAULT_BLOCKS 4

static volatile uint64_t sink;

static inline void mb_barrier(void) {
#if defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("dmb ish" ::: "memory");
#else
    atomic_thread_fence(memory_order_seq_cst);
#endif
}

int main(int argc, char **argv) {
    uint64_t iters = DEFAULT_ITERS;
    int blocks = DEFAULT_BLOCKS;
    if (argc > 1) iters = strtoull(argv[1], NULL, 0);
    if (argc > 2) blocks = atoi(argv[2]);
    if (blocks <= 0) blocks = DEFAULT_BLOCKS;

    size_t words_per_block = CACHELINE / sizeof(uint64_t);
    size_t total_words = (size_t)blocks * words_per_block;
    uint64_t *buf = NULL;
    if (posix_memalign((void **)&buf, CACHELINE, total_words * sizeof(uint64_t)) != 0)
        return 1;

    for (size_t i = 0; i < total_words; ++i) buf[i] = 0;

    for (uint64_t iter = 0; iter < iters; ++iter) {
        for (int b = 0; b < blocks; ++b) {
            size_t base = (size_t)b * words_per_block;
            // Multiple stores to the same cache line.
            for (size_t w = 0; w < words_per_block; ++w) {
                buf[base + w] = (iter << 8) ^ (uint64_t)w;
            }
            mb_barrier();
        }
        sink += buf[(iter * 7) % total_words];
    }

    printf("done: %llu\n", (unsigned long long)sink);
    free(buf);
    return 0;
}
