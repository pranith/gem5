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
#define DEFAULT_RECORDS 8
#define DEFAULT_LOG_SIZE (1 << 20)

static int num_threads = DEFAULT_THREADS;
static int txns_per_thread = DEFAULT_TXNS;
static int records_per_txn = DEFAULT_RECORDS;
static size_t log_size = DEFAULT_LOG_SIZE;

static uint64_t *log_buf;
static atomic_size_t log_tail;
static atomic_uint ready_count;
static atomic_uint commit_epoch;
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

static void *worker(void *arg) {
    unsigned tid = (unsigned)(uintptr_t)arg;
    uint64_t rng = 0x9e3779b97f4a7c15ULL ^ (uint64_t)tid * 0xbf58476d1ce4e5b9ULL;
    uint64_t local = 0;

    pthread_barrier_wait(&start_barrier);

    for (int t = 0; t < txns_per_thread; ++t) {
        unsigned epoch = atomic_load_explicit(&commit_epoch, memory_order_acquire);

        for (int i = 0; i < records_per_txn; ++i) {
            local ^= lcg_next(&rng);
        }

        atomic_thread_fence(memory_order_release);
        atomic_fetch_add_explicit(&ready_count, 1, memory_order_acq_rel);

        if (tid == 0) {
            while (atomic_load_explicit(&ready_count, memory_order_acquire) <
                   (unsigned)num_threads) {
                sched_yield();
            }
            size_t total_records = (size_t)records_per_txn * (size_t)num_threads;
            size_t pos = atomic_fetch_add_explicit(&log_tail, total_records,
                                                   memory_order_acq_rel);
            for (size_t i = 0; i < total_records; ++i) {
                log_buf[(pos + i) % log_size] = local + i;
            }
            atomic_store_explicit(&ready_count, 0, memory_order_release);
            atomic_store_explicit(&commit_epoch, epoch + 1, memory_order_release);
        } else {
            while (atomic_load_explicit(&commit_epoch, memory_order_acquire) == epoch)
                ;
        }
    }

    sink ^= local;
    return NULL;
}

static void usage(const char *prog) {
    printf("Usage: %s [threads] [txns_per_thread] [records_per_txn] [log_size]\n",
           prog);
}

int main(int argc, char **argv) {
    if (argc > 1) num_threads = atoi(argv[1]);
    if (argc > 2) txns_per_thread = atoi(argv[2]);
    if (argc > 3) records_per_txn = atoi(argv[3]);
    if (argc > 4) log_size = (size_t)strtoull(argv[4], NULL, 10);

    if (num_threads <= 0) num_threads = DEFAULT_THREADS;
    if (txns_per_thread <= 0) txns_per_thread = DEFAULT_TXNS;
    if (records_per_txn <= 0) records_per_txn = DEFAULT_RECORDS;
    if (log_size == 0) log_size = DEFAULT_LOG_SIZE;

    if (posix_memalign((void **)&log_buf, CACHELINE, log_size * sizeof(uint64_t)) != 0)
        return 1;
    for (size_t i = 0; i < log_size; ++i) log_buf[i] = (uint64_t)i;

    atomic_init(&log_tail, 0);
    atomic_init(&ready_count, 0);
    atomic_init(&commit_epoch, 0);

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
    free(log_buf);

    double sec = (double)(end - start) / 1e9;
    double total_txns = (double)txns_per_thread * num_threads;
    printf("Transactions: %.0f  Records/txn: %d  time: %.3fs\n",
           total_txns, records_per_txn, sec);
    printf("Throughput: %.0f txns/s\n", total_txns / sec);
    return 0;
}
