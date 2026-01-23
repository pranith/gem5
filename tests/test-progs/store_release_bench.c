#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>

#define CACHELINE 64
#define PAYLOAD_WORDS 16
#define DEFAULT_ITERS 500000
#define DEFAULT_THREADS 4
#define COLD_SIZE (1 << 16)    // 64 KiB cold buffer to copy from

struct slot {
    atomic_uint seq;
    uint64_t data[PAYLOAD_WORDS];
} __attribute__((aligned(CACHELINE)));

static struct slot shared_slot;
static uint64_t *cold_buf;
static volatile uint64_t sink;
static int num_producers = 2;
static int num_consumers = 2;
static int iters = DEFAULT_ITERS;

static inline uint64_t nsec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static inline void fill_payload(uint64_t base) {
    for (int i = 0; i < PAYLOAD_WORDS; ++i) {
        size_t idx = (base + i * 13) & (COLD_SIZE - 1);
        shared_slot.data[i] = cold_buf[idx] ^ (base + i);
    }
}

static inline uint64_t consume_payload(void) {
    uint64_t acc = 0;
    for (int i = 0; i < PAYLOAD_WORDS; ++i) {
        // Simple mix to keep compiler from eliding loads.
        acc = (acc << 7) ^ (acc >> 3) ^ shared_slot.data[i];
    }
    return acc;
}

static void *producer(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    for (int i = 0; i < iters; ++i) {
        // wait for consumer to clear slot
        while (atomic_load_explicit(&shared_slot.seq, memory_order_acquire) != 0)
            ;
        fill_payload(((uint64_t)i << 16) | id);
        // release fence keeps payload stores ordered before publish
        atomic_thread_fence(memory_order_release);
        atomic_store_explicit(&shared_slot.seq, 1, memory_order_release);
    }
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    uint64_t local = 0;
    for (int i = 0; i < iters; ++i) {
        // spin until producer publishes
        while (atomic_load_explicit(&shared_slot.seq, memory_order_acquire) != 1)
            ;
        // matching acquire fence before consuming payload
        atomic_thread_fence(memory_order_acquire);
        local += consume_payload();
        // release to allow producer to proceed
        atomic_store_explicit(&shared_slot.seq, 0, memory_order_release);
    }
    sink += local;
    return NULL;
}

static void usage(const char *prog) {
    printf("Usage: %s [producers] [consumers] [iterations]\n", prog);
}

int main(int argc, char **argv) {
    if (argc > 1) num_producers = atoi(argv[1]);
    if (argc > 2) num_consumers = atoi(argv[2]);
    if (argc > 3) iters = atoi(argv[3]);
    if (num_producers <= 0) num_producers = DEFAULT_THREADS / 2;
    if (num_consumers <= 0) num_consumers = DEFAULT_THREADS / 2;
    if (iters <= 0) iters = DEFAULT_ITERS;
    if (num_producers + num_consumers <= 1) {
        usage(argv[0]);
        return 1;
    }

    if (posix_memalign((void **)&cold_buf, CACHELINE, COLD_SIZE * sizeof(uint64_t)) != 0)
        return 1;
    for (size_t i = 0; i < COLD_SIZE; ++i)
        cold_buf[i] = (i * 6364136223846793005ULL) ^ (i >> 3);

    atomic_init(&shared_slot.seq, 0);

    pthread_t *prods = calloc(num_producers, sizeof(pthread_t));
    pthread_t *cons = calloc(num_consumers, sizeof(pthread_t));
    if (!prods || !cons) return 1;

    uint64_t start = nsec_now();
    for (int i = 0; i < num_producers; ++i)
        pthread_create(&prods[i], NULL, producer, (void *)(uintptr_t)i);
    for (int i = 0; i < num_consumers; ++i)
        pthread_create(&cons[i], NULL, consumer, NULL);
    for (int i = 0; i < num_producers; ++i) pthread_join(prods[i], NULL);
    for (int i = 0; i < num_consumers; ++i) pthread_join(cons[i], NULL);
    uint64_t end = nsec_now();

    double sec = (double)(end - start) / 1e9;
    double handoffs = (double)iters * num_producers;
    printf("Handoffs: %.0f  time: %.3fs  throughput: %.0f ops/s\n",
           handoffs, sec, handoffs / sec);

    free(prods);
    free(cons);
    free(cold_buf);
    return 0;
}
