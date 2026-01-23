#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CACHELINE 64
#define DEFAULT_THREADS 4
#define DEFAULT_OPS 500000
#define DEFAULT_TABLE_SIZE (1 << 15)
#define DEFAULT_HOT_PCT 10
#define WORK_ITERS 8

static int num_threads = DEFAULT_THREADS;
static int ops_per_thread = DEFAULT_OPS;
static size_t table_size = DEFAULT_TABLE_SIZE;
static int hot_pct = DEFAULT_HOT_PCT;

static atomic_uint *latches;
static uint64_t *pages;
static pthread_barrier_t start_barrier;
static volatile uint64_t sink;

static inline uint64_t nsec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static inline uint64_t lcg_next(uint64_t *state) {
    *state = (*state * 6364136223846793005ULL) + 1;
    return *state;
}

static inline void latch_acquire(size_t idx, unsigned owner) {
    unsigned expected;
    int spins = 0;
    for (;;) {
        expected = 0;
        if (atomic_compare_exchange_weak_explicit(&latches[idx], &expected, owner,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
            return;
        }
        if ((++spins & 0x3f) == 0) sched_yield();
    }
}

static inline void latch_release(size_t idx) {
    atomic_store_explicit(&latches[idx], 0, memory_order_release);
}

static void *worker(void *arg) {
    unsigned tid = (unsigned)(uintptr_t)arg + 1;
    uint64_t rng = 0x9e3779b97f4a7c15ULL ^ (uint64_t)tid * 0xbf58476d1ce4e5b9ULL;
    uint64_t local = 0;
    size_t hot_size = (table_size * (size_t)hot_pct) / 100;
    if (hot_size == 0) hot_size = 1;

    pthread_barrier_wait(&start_barrier);

    for (int i = 0; i < ops_per_thread; ++i) {
        size_t idx;
        if ((lcg_next(&rng) & 0xff) < (unsigned)hot_pct) {
            idx = (size_t)(lcg_next(&rng) % hot_size);
        } else {
            idx = (size_t)(lcg_next(&rng) % table_size);
        }

        latch_acquire(idx, tid);
        local += pages[idx];
        pages[idx] ^= local + (uint64_t)i;
        for (int k = 0; k < WORK_ITERS; ++k)
            local ^= (local << 3) + (uint64_t)k;
        latch_release(idx);
    }

    sink ^= local;
    return NULL;
}

static void usage(const char *prog) {
    printf("Usage: %s [threads] [ops_per_thread] [table_size] [hot_pct]\n", prog);
}

int main(int argc, char **argv) {
    if (argc > 1) num_threads = atoi(argv[1]);
    if (argc > 2) ops_per_thread = atoi(argv[2]);
    if (argc > 3) table_size = (size_t)strtoull(argv[3], NULL, 10);
    if (argc > 4) hot_pct = atoi(argv[4]);

    if (num_threads <= 0) num_threads = DEFAULT_THREADS;
    if (ops_per_thread <= 0) ops_per_thread = DEFAULT_OPS;
    if (table_size == 0) table_size = DEFAULT_TABLE_SIZE;
    if (hot_pct < 0 || hot_pct > 100) hot_pct = DEFAULT_HOT_PCT;

    if (posix_memalign((void **)&latches, CACHELINE, table_size * sizeof(*latches)) != 0)
        return 1;
    if (posix_memalign((void **)&pages, CACHELINE, table_size * sizeof(*pages)) != 0)
        return 1;

    for (size_t i = 0; i < table_size; ++i) {
        atomic_init(&latches[i], 0);
        pages[i] = (uint64_t)i * 1315423911u;
    }

    pthread_t *threads = calloc((size_t)num_threads, sizeof(pthread_t));
    if (!threads) return 1;
    pthread_barrier_init(&start_barrier, NULL, (unsigned)num_threads);

    uint64_t start = nsec_now();
    for (int i = 0; i < num_threads; ++i)
        pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i);
    for (int i = 0; i < num_threads; ++i)
        pthread_join(threads[i], NULL);
    uint64_t end = nsec_now();

    pthread_barrier_destroy(&start_barrier);
    free(threads);
    free(pages);
    free(latches);

    double sec = (double)(end - start) / 1e9;
    double total_ops = (double)ops_per_thread * num_threads;
    printf("Operations: %.0f  time: %.3fs  throughput: %.0f ops/s\n",
           total_ops, sec, total_ops / sec);
    return 0;
}
