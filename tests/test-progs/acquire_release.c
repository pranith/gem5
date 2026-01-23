#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#define ITERS 2000000
#define CACHELINE 64
#define LL_SIZE (1 << 20)          // 1 MiB buffer to drive cache misses
#define LL_MASK (LL_SIZE - 1)
#define LL_OPS 21                  // ~25 memory ops total per iteration

static int num_producers = 2;
static int num_consumers = 2;
static volatile uint64_t sink;
static uint64_t *long_lat_buf;
static atomic_uint acq_rel_counter;

struct slot {
    atomic_uint seq;
    uint64_t payload;
} __attribute__((aligned(CACHELINE)));

static struct slot shared_slot;

static inline void long_latency_touch(unsigned tid, uint64_t iter) {
    size_t idx = ((size_t)tid << 10) ^ (size_t)iter;
    for (int k = 0; k < LL_OPS; ++k) {
        idx = (idx * 2654435761u + 104729u) & LL_MASK;
        sink += long_lat_buf[idx];
    }
}

static inline void acq_rel_ops(void) {
    atomic_fetch_add_explicit(&acq_rel_counter, 1, memory_order_acquire);
    atomic_fetch_add_explicit(&acq_rel_counter, 1, memory_order_release);
}

static void *producer(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    for (uint64_t i = 1; i <= ITERS; ++i) {
        // wait for consumer to clear
        while (atomic_load_explicit(&shared_slot.seq, memory_order_acquire) != 0)
            ;
        long_latency_touch(id, i);
        acq_rel_ops();
        shared_slot.payload = (i << 16) | id;
        // publish with release
        atomic_store_explicit(&shared_slot.seq, 1, memory_order_release);
    }
    return NULL;
}

static void *consumer(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    uint64_t local = 0;
    for (uint64_t i = 0; i < ITERS; ++i) {
        // wait for producer to publish
        while (atomic_load_explicit(&shared_slot.seq, memory_order_acquire) != 1)
            ;
        long_latency_touch(id, i);
        acq_rel_ops();
        uint64_t v = shared_slot.payload;
        local += (v >> 16);
        // clear with release so producer can proceed
        atomic_store_explicit(&shared_slot.seq, 0, memory_order_release);
    }
    sink += local;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1) num_producers = atoi(argv[1]);
    if (argc > 2) num_consumers = atoi(argv[2]);
    if (num_producers <= 0) num_producers = 2;
    if (num_consumers <= 0) num_consumers = 2;

    atomic_init(&shared_slot.seq, 0);
    shared_slot.payload = 0;
    atomic_init(&acq_rel_counter, 0);

    if (posix_memalign((void **)&long_lat_buf, CACHELINE, LL_SIZE * sizeof(uint64_t)) != 0)
        return 1;
    for (size_t i = 0; i < LL_SIZE; ++i) long_lat_buf[i] = (uint64_t)i * 17;

    pthread_t *prods = calloc(num_producers, sizeof(pthread_t));
    pthread_t *cons = calloc(num_consumers, sizeof(pthread_t));
    if (!prods || !cons) return 1;

    for (int i = 0; i < num_producers; ++i)
        pthread_create(&prods[i], NULL, producer, (void *)(uintptr_t)i);
    for (int i = 0; i < num_consumers; ++i)
        pthread_create(&cons[i], NULL, consumer, (void *)(uintptr_t)i);

    for (int i = 0; i < num_producers; ++i) pthread_join(prods[i], NULL);
    for (int i = 0; i < num_consumers; ++i) pthread_join(cons[i], NULL);

    free(long_lat_buf);
    free(prods);
    free(cons);
    printf("done\n");
    return 0;
}
