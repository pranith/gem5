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
#define DEFAULT_TXNS 200000
#define DEFAULT_LOCKS_PER_TXN 8
#define DEFAULT_TABLE_SIZE (1 << 14)
#define DEFAULT_BARRIER_INTERVAL 0
#define WORK_ITERS 16

static atomic_uint *lock_table;
static uint64_t *record_data;
static int num_threads = DEFAULT_THREADS;
static int txns_per_thread = DEFAULT_TXNS;
static int locks_per_txn = DEFAULT_LOCKS_PER_TXN;
static size_t table_size = DEFAULT_TABLE_SIZE;
static int barrier_interval = DEFAULT_BARRIER_INTERVAL;
static pthread_barrier_t ready_barrier;
static pthread_barrier_t epoch_barrier;
static atomic_int go_flag;
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

static inline void lock_acquire(size_t idx, unsigned owner) {
    unsigned expected;
    int spins = 0;
    for (;;) {
        expected = 0;
        if (atomic_compare_exchange_weak_explicit(&lock_table[idx], &expected, owner,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
            return;
        }
        if ((++spins & 0x3f) == 0) sched_yield();
    }
}

static inline void lock_release(size_t idx) {
    atomic_store_explicit(&lock_table[idx], 0, memory_order_release);
}

static void sort_indices(size_t *idxs, int count) {
    for (int i = 1; i < count; ++i) {
        size_t key = idxs[i];
        int j = i - 1;
        while (j >= 0 && idxs[j] > key) {
            idxs[j + 1] = idxs[j];
            --j;
        }
        idxs[j + 1] = key;
    }
}

static void *worker(void *arg) {
    unsigned tid = (unsigned)(uintptr_t)arg + 1;
    uint64_t rng = 0x9e3779b97f4a7c15ULL ^ (uint64_t)tid * 0xbf58476d1ce4e5b9ULL;
    size_t *idxs = calloc((size_t)locks_per_txn, sizeof(size_t));
    if (!idxs) return NULL;

    pthread_barrier_wait(&ready_barrier);
    while (!atomic_load_explicit(&go_flag, memory_order_acquire))
        ;

    uint64_t local = 0;
    for (int t = 0; t < txns_per_thread; ++t) {
        for (int i = 0; i < locks_per_txn; ++i) {
            idxs[i] = (size_t)(lcg_next(&rng) % table_size);
        }
        sort_indices(idxs, locks_per_txn);

        for (int i = 0; i < locks_per_txn; ++i) {
            lock_acquire(idxs[i], tid);
        }

        for (int i = 0; i < locks_per_txn; ++i) {
            local ^= record_data[idxs[i]] + (uint64_t)(i + t);
        }
        for (int i = 0; i < WORK_ITERS; ++i) {
            local ^= (uint64_t)i * 17 + (local >> 3);
        }

        for (int i = locks_per_txn - 1; i >= 0; --i) {
            lock_release(idxs[i]);
        }

        if (barrier_interval > 0 && ((t + 1) % barrier_interval) == 0) {
            pthread_barrier_wait(&epoch_barrier);
        }
    }

    sink ^= local;
    free(idxs);
    return NULL;
}

static void usage(const char *prog) {
    printf("Usage: %s [threads] [txns_per_thread] [locks_per_txn] [table_size] [barrier_interval]\n",
           prog);
}

int main(int argc, char **argv) {
    if (argc > 1) num_threads = atoi(argv[1]);
    if (argc > 2) txns_per_thread = atoi(argv[2]);
    if (argc > 3) locks_per_txn = atoi(argv[3]);
    if (argc > 4) table_size = (size_t)strtoull(argv[4], NULL, 10);
    if (argc > 5) barrier_interval = atoi(argv[5]);

    if (num_threads <= 0) num_threads = DEFAULT_THREADS;
    if (txns_per_thread <= 0) txns_per_thread = DEFAULT_TXNS;
    if (locks_per_txn <= 0) locks_per_txn = DEFAULT_LOCKS_PER_TXN;
    if (table_size == 0) table_size = DEFAULT_TABLE_SIZE;
    if (barrier_interval < 0) barrier_interval = DEFAULT_BARRIER_INTERVAL;

    if (posix_memalign((void **)&lock_table, CACHELINE, table_size * sizeof(*lock_table)) != 0)
        return 1;
    if (posix_memalign((void **)&record_data, CACHELINE, table_size * sizeof(*record_data)) != 0)
        return 1;

    for (size_t i = 0; i < table_size; ++i) {
        atomic_init(&lock_table[i], 0);
        record_data[i] = (uint64_t)i * 11400714819323198485ULL;
    }

    pthread_t *threads = calloc((size_t)num_threads, sizeof(pthread_t));
    if (!threads) return 1;

    pthread_barrier_init(&ready_barrier, NULL, (unsigned)num_threads + 1);
    if (barrier_interval > 0)
        pthread_barrier_init(&epoch_barrier, NULL, (unsigned)num_threads);

    atomic_init(&go_flag, 0);

    for (int i = 0; i < num_threads; ++i) {
        pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i);
    }

    pthread_barrier_wait(&ready_barrier);
    uint64_t start = nsec_now();
    atomic_store_explicit(&go_flag, 1, memory_order_release);

    for (int i = 0; i < num_threads; ++i) {
        pthread_join(threads[i], NULL);
    }
    uint64_t end = nsec_now();

    if (barrier_interval > 0)
        pthread_barrier_destroy(&epoch_barrier);
    pthread_barrier_destroy(&ready_barrier);

    double sec = (double)(end - start) / 1e9;
    double total_txns = (double)txns_per_thread * num_threads;
    double total_lock_ops = total_txns * locks_per_txn;
    printf("Transactions: %.0f  Locks/txn: %d  time: %.3fs\n",
           total_txns, locks_per_txn, sec);
    printf("Throughput: %.0f txns/s  %.0f lock ops/s\n",
           total_txns / sec, total_lock_ops / sec);

    free(threads);
    free(record_data);
    free(lock_table);
    return 0;
}
