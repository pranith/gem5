#define _XOPEN_SOURCE 700
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CACHELINE 64
#define DEFAULT_ITERS 10000000ULL
#define DEFAULT_WORDS 1024
#define DEFAULT_STORE_WORDS 8192

static volatile uint64_t sink;

static inline uint64_t
read_cntvct(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0; isb" : "=r"(v));
    return v;
}

static inline uint64_t
load_acquire_u64(const uint64_t *p)
{
    uint64_t v;
    __asm__ volatile("ldar %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static inline uint64_t
load_acquire_pc_u64(const uint64_t *p)
{
#if defined(__ARM_FEATURE_RCPC)
    uint64_t v;
    __asm__ volatile("ldapr %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
#else
    return load_acquire_u64(p);
#endif
}

static inline void
store_release_u64(uint64_t *p, uint64_t v)
{
    __asm__ volatile("stlr %0, [%1]" ::"r"(v), "r"(p) : "memory");
}

static uint64_t
run_loop(uint64_t *buf, uint64_t *store_buf, uint64_t iters,
         int use_acquire_pc, uint64_t store_mask, uint64_t release_mask,
         uint64_t store_burst)
{
    uint64_t idx = 0;
    uint64_t store_idx = 0;
    uint64_t start = read_cntvct();
    if (use_acquire_pc) {
        for (uint64_t i = 0; i < iters; ++i) {
            for (uint64_t s = 0; s < store_burst; ++s) {
                uint64_t tag = i * store_burst + s;
                if ((tag & release_mask) == 0) {
                    store_release_u64(&store_buf[store_idx], tag + store_idx);
                } else {
                    store_buf[store_idx] = tag + store_idx;
                }
                store_idx = (store_idx + 1) & store_mask;
            }
            idx = load_acquire_pc_u64(&buf[idx]);
        }
    } else {
        for (uint64_t i = 0; i < iters; ++i) {
            for (uint64_t s = 0; s < store_burst; ++s) {
                uint64_t tag = i * store_burst + s;
                if ((tag & release_mask) == 0) {
                    store_release_u64(&store_buf[store_idx], tag + store_idx);
                } else {
                    store_buf[store_idx] = tag + store_idx;
                }
                store_idx = (store_idx + 1) & store_mask;
            }
            idx = load_acquire_u64(&buf[idx]);
        }
    }
    uint64_t end = read_cntvct();
    uint64_t cycles = end - start;
    sink += idx;
    return cycles;
}

int
main(int argc, char **argv)
{
    uint64_t iters = DEFAULT_ITERS;
    uint64_t store_words = 8192;
    uint64_t release_interval = 2;
    uint64_t store_burst = 8;
    if (argc > 1) {
        iters = strtoull(argv[1], NULL, 0);
        if (iters == 0) {
            iters = DEFAULT_ITERS;
        }
    }
    if (argc > 2) {
        store_words = strtoull(argv[2], NULL, 0);
        if (store_words == 0) {
            store_words = 2048;
        }
    }
    if (argc > 3) {
        release_interval = strtoull(argv[3], NULL, 0);
        if (release_interval == 0) {
            release_interval = 64;
        }
    }
    if (argc > 4) {
        store_burst = strtoull(argv[4], NULL, 0);
        if (store_burst == 0) {
            store_burst = 1;
        }
    }
    if ((release_interval & (release_interval - 1)) != 0) {
        fprintf(stderr, "release_interval must be a power of two\n");
        return 1;
    }
    if ((store_words & (store_words - 1)) != 0) {
        fprintf(stderr, "store_words must be a power of two\n");
        return 1;
    }
    if (store_burst > 64) {
        fprintf(stderr, "store_burst must be <= 64\n");
        return 1;
    }
    if (store_words > DEFAULT_STORE_WORDS) {
        fprintf(stderr, "store_words must be <= %u\n", DEFAULT_STORE_WORDS);
        return 1;
    }

    uint64_t *buf = aligned_alloc(CACHELINE, DEFAULT_WORDS * sizeof(uint64_t));
    if (!buf) {
        return 1;
    }
    uint64_t *store_buf =
        aligned_alloc(CACHELINE, DEFAULT_STORE_WORDS * sizeof(uint64_t));
    if (!store_buf) {
        return 1;
    }

    /* Build a full-cycle permutation for pointer chasing. */
    for (uint64_t i = 0; i < DEFAULT_WORDS; ++i) {
        buf[i] = (5 * i + 1) & (DEFAULT_WORDS - 1);
    }

    /* Warm-up. */
    uint64_t idx = 0;
    for (uint64_t i = 0; i < DEFAULT_WORDS; ++i) {
        idx = buf[idx];
    }
    sink += idx;

    uint64_t store_mask = store_words - 1;
    uint64_t release_mask = release_interval - 1;
    uint64_t cycles_acquire = run_loop(buf, store_buf, iters, 0, store_mask,
                                       release_mask, store_burst);
    uint64_t cycles_acquire_pc = run_loop(buf, store_buf, iters, 1, store_mask,
                                          release_mask, store_burst);

    printf("iters=%" PRIu64 " store_words=%" PRIu64
           " release_interval=%" PRIu64 " store_burst=%" PRIu64 "\n",
           iters, store_words, release_interval, store_burst);
    printf("ldar  cycles=%" PRIu64 " cycles/iter=%.2f\n", cycles_acquire,
           (double)cycles_acquire / (double)iters);
    printf("ldapr cycles=%" PRIu64 " cycles/iter=%.2f\n", cycles_acquire_pc,
           (double)cycles_acquire_pc / (double)iters);
    if (cycles_acquire_pc > 0) {
        printf("speedup (ldar/ldapr)=%.3f\n",
               (double)cycles_acquire / (double)cycles_acquire_pc);
    }

    free(store_buf);
    free(buf);
    return 0;
}
