#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CACHELINE 64
#define WORK_ITERS 16              // ~16 sink touches per round
#define BARRIER_ROUNDS 100000
#define LL_SIZE (1 << 20)          // 1 MiB buffer to force cache misses
#define LL_MASK (LL_SIZE - 1)
#define LL_OPS 9                   // ~9 long-latency loads per round

static int num_threads = 4;
static pthread_barrier_t barrier;
static volatile uint64_t sink;
static uint64_t *long_lat_buf;
static atomic_uint acq_rel_counter;

static inline void work_loop(void) {
    for (int i = 0; i < WORK_ITERS; ++i) {
        sink += (uint64_t)i * 17 + 3;
    }
}

static inline void long_latency_touch(int tid, int iter) {
    size_t idx = ((size_t)tid << 10) ^ (size_t)iter;
    for (int k = 0; k < LL_OPS; ++k) {
        idx = (idx * 1315423911u + 104729u) & LL_MASK;
        sink += long_lat_buf[idx];
    }
}

static inline void acq_rel_ops(void) {
    atomic_fetch_add_explicit(&acq_rel_counter, 1, memory_order_acquire);
    atomic_fetch_add_explicit(&acq_rel_counter, 1, memory_order_release);
}

static void *worker(void *arg) {
    int tid = (int)(intptr_t)arg;
    for (int r = 0; r < BARRIER_ROUNDS; ++r) {
        work_loop();
        long_latency_touch(tid, r);
        acq_rel_ops();
        pthread_barrier_wait(&barrier);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        num_threads = atoi(argv[1]);
        if (num_threads <= 0) num_threads = 4;
    }

    pthread_t *threads = calloc(num_threads, sizeof(pthread_t));
    if (!threads) return 1;

    if (posix_memalign((void **)&long_lat_buf, CACHELINE, LL_SIZE * sizeof(uint64_t)) != 0)
        return 1;
    for (size_t i = 0; i < LL_SIZE; ++i) long_lat_buf[i] = (uint64_t)i * 17;
    atomic_init(&acq_rel_counter, 0);

    pthread_barrier_init(&barrier, NULL, num_threads);

    for (int i = 0; i < num_threads; ++i) {
        pthread_create(&threads[i], NULL, worker, (void *)(intptr_t)i);
    }

    for (int i = 0; i < num_threads; ++i) {
        pthread_join(threads[i], NULL);
    }

    pthread_barrier_destroy(&barrier);
    free(long_lat_buf);
    free(threads);
    printf("done\n");
    return 0;
}
