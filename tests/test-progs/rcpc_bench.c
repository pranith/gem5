#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#define CACHELINE 64
#define DEFAULT_ITERS 2000000
#define DEFAULT_WORDS 8

static volatile uint64_t sink;

struct shared_state {
    _Atomic uint64_t flag;
    uint64_t data[DEFAULT_WORDS];
} __attribute__((aligned(CACHELINE)));

static struct shared_state shared;
static uint64_t *scratch;

static inline void store_release_u64(_Atomic uint64_t *p, uint64_t v)
{
#if defined(__aarch64__)
    __asm__ volatile("stlr %0, [%1]" :: "r"(v), "r"(p) : "memory");
#else
    atomic_store_explicit(p, v, memory_order_release);
#endif
}

static inline uint64_t load_rcsc_u64(_Atomic uint64_t *p)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("ldar %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
#else
    return atomic_load_explicit(p, memory_order_seq_cst);
#endif
}

static inline uint64_t load_rcpc_u64(_Atomic uint64_t *p)
{
#if defined(__aarch64__) && defined(__ARM_FEATURE_RCPC)
    uint64_t v;
    __asm__ volatile("ldapr %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
#elif defined(__aarch64__)
    return load_rcsc_u64(p);
#else
    return atomic_load_explicit(p, memory_order_acquire);
#endif
}

static void *producer(void *arg)
{
    uint64_t iters = *(uint64_t *)arg;
    for (uint64_t i = 1; i <= iters; ++i) {
        for (size_t w = 0; w < DEFAULT_WORDS; ++w) {
            shared.data[w] = (i << 16) ^ (uint64_t)w;
        }
        store_release_u64(&shared.flag, i);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    struct {
        uint64_t iters;
        int use_rcpc;
    } *cfg = arg;

    uint64_t total = 0;
    for (uint64_t i = 1; i <= cfg->iters; ++i) {
        uint64_t v;
        do {
            v = cfg->use_rcpc ? load_rcpc_u64(&shared.flag)
                              : load_rcsc_u64(&shared.flag);
        } while (v < i);

        // Independent loads that RCsc will order after the acquire.
        total += scratch[(i * 7) & 1023];
        total += scratch[(i * 13) & 1023];

        for (size_t w = 0; w < DEFAULT_WORDS; ++w) {
            total += shared.data[w];
        }
    }
    sink += total;
    return NULL;
}

int main(int argc, char **argv)
{
    uint64_t iters = DEFAULT_ITERS;
    int use_rcpc = 0;
    if (argc > 1) iters = strtoull(argv[1], NULL, 0);
    if (argc > 2) use_rcpc = atoi(argv[2]) != 0;

    atomic_init(&shared.flag, 0);
    for (size_t w = 0; w < DEFAULT_WORDS; ++w) shared.data[w] = 0;

    scratch = aligned_alloc(CACHELINE, 1024 * sizeof(uint64_t));
    if (!scratch) return 1;
    for (size_t i = 0; i < 1024; ++i) scratch[i] = i * 3;

    pthread_t prod;
    pthread_t cons;
    struct {
        uint64_t iters;
        int use_rcpc;
    } cfg = {iters, use_rcpc};

    pthread_create(&prod, NULL, producer, &iters);
    pthread_create(&cons, NULL, consumer, &cfg);

    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    printf("done: %llu (mode=%s)\n",
           (unsigned long long)sink,
           use_rcpc ? "rcpc" : "rcsc");
    free(scratch);
    return 0;
}
