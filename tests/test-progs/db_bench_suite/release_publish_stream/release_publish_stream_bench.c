#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CACHELINE 64
#define DEFAULT_PRODUCERS 3
#define DEFAULT_ITEMS 200000
#define DEFAULT_RING_SIZE (1 << 12)
#define DEFAULT_PAYLOAD_BYTES 256
#define DEFAULT_COLD_WORDS (1 << 20)

struct slot {
    atomic_size_t seq;
    uint64_t *payload;
};

static int num_producers = DEFAULT_PRODUCERS;
static int items_per_producer = DEFAULT_ITEMS;
static size_t ring_size = DEFAULT_RING_SIZE;
static size_t payload_bytes = DEFAULT_PAYLOAD_BYTES;
static size_t cold_words = DEFAULT_COLD_WORDS;

static struct slot *ring;
static uint64_t *payload_store;
static uint64_t *cold_buf;
static atomic_size_t tail;
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

static void *producer(void *arg) {
    unsigned tid = (unsigned)(uintptr_t)arg + 1;
    uint64_t rng = 0x9e3779b97f4a7c15ULL ^ (uint64_t)tid * 0xbf58476d1ce4e5b9ULL;
    size_t payload_words = payload_bytes / sizeof(uint64_t);
    uint64_t local = 0;

    pthread_barrier_wait(&start_barrier);

    for (int i = 0; i < items_per_producer; ++i) {
        size_t pos = atomic_fetch_add_explicit(&tail, 1, memory_order_acq_rel);
        struct slot *s = &ring[pos & (ring_size - 1)];
        while (atomic_load_explicit(&s->seq, memory_order_acquire) != pos)
            sched_yield();

        uint64_t base = lcg_next(&rng);
        uint64_t *payload = s->payload;
        for (size_t w = 0; w < payload_words; ++w) {
            size_t idx = (size_t)(base + w * 13) & (cold_words - 1);
            uint64_t v = cold_buf[idx] ^ (base + w);
            payload[w] = v;
            local ^= v;
        }

        atomic_thread_fence(memory_order_release);
        atomic_store_explicit(&s->seq, pos + 1, memory_order_release);
    }

    sink ^= local;
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    size_t total = (size_t)num_producers * (size_t)items_per_producer;
    size_t payload_words = payload_bytes / sizeof(uint64_t);
    uint64_t local = 0;

    pthread_barrier_wait(&start_barrier);

    for (size_t pos = 0; pos < total; ++pos) {
        struct slot *s = &ring[pos & (ring_size - 1)];
        while (atomic_load_explicit(&s->seq, memory_order_acquire) != pos + 1)
            sched_yield();

        uint64_t *payload = s->payload;
        for (size_t w = 0; w < payload_words; ++w)
            local ^= payload[w];

        atomic_store_explicit(&s->seq, pos + ring_size, memory_order_release);
    }

    sink ^= local;
    return NULL;
}

static void usage(const char *prog) {
    printf("Usage: %s [producers] [items_per_producer] [ring_size] [payload_bytes] [cold_words]\n",
           prog);
}

int main(int argc, char **argv) {
    if (argc > 1) num_producers = atoi(argv[1]);
    if (argc > 2) items_per_producer = atoi(argv[2]);
    if (argc > 3) ring_size = (size_t)strtoull(argv[3], NULL, 10);
    if (argc > 4) payload_bytes = (size_t)strtoull(argv[4], NULL, 10);
    if (argc > 5) cold_words = (size_t)strtoull(argv[5], NULL, 10);

    if (num_producers <= 0) num_producers = DEFAULT_PRODUCERS;
    if (items_per_producer <= 0) items_per_producer = DEFAULT_ITEMS;
    if (ring_size < 2 || (ring_size & (ring_size - 1)) != 0)
        ring_size = DEFAULT_RING_SIZE;
    if (payload_bytes == 0 || (payload_bytes % sizeof(uint64_t)) != 0)
        payload_bytes = DEFAULT_PAYLOAD_BYTES;
    if (cold_words == 0 || (cold_words & (cold_words - 1)) != 0)
        cold_words = DEFAULT_COLD_WORDS;

    if (posix_memalign((void **)&ring, CACHELINE, ring_size * sizeof(*ring)) != 0)
        return 1;
    if (posix_memalign((void **)&payload_store, CACHELINE,
                       ring_size * payload_bytes) != 0) {
        return 1;
    }
    if (posix_memalign((void **)&cold_buf, CACHELINE,
                       cold_words * sizeof(uint64_t)) != 0) {
        return 1;
    }

    for (size_t i = 0; i < ring_size; ++i) {
        atomic_init(&ring[i].seq, i);
        ring[i].payload = (uint64_t *)((uint8_t *)payload_store + i * payload_bytes);
    }
    for (size_t i = 0; i < cold_words; ++i)
        cold_buf[i] = (uint64_t)i * 11400714819323198485ULL;

    atomic_init(&tail, 0);

    int total_threads = num_producers + 1;
    pthread_t *threads = calloc((size_t)total_threads, sizeof(pthread_t));
    if (!threads) return 1;
    pthread_barrier_init(&start_barrier, NULL, (unsigned)total_threads);

    uint64_t start = nsec_now();
    pthread_create(&threads[0], NULL, consumer, NULL);
    for (int i = 0; i < num_producers; ++i)
        pthread_create(&threads[i + 1], NULL, producer, (void *)(uintptr_t)i);
    for (int i = 0; i < total_threads; ++i)
        pthread_join(threads[i], NULL);
    uint64_t end = nsec_now();

    pthread_barrier_destroy(&start_barrier);
    free(threads);
    free(cold_buf);
    free(payload_store);
    free(ring);

    double sec = (double)(end - start) / 1e9;
    double total_items = (double)num_producers * items_per_producer;
    printf("Items: %.0f  Payload: %zu bytes  time: %.3fs\n",
           total_items, payload_bytes, sec);
    printf("Throughput: %.0f items/s\n", total_items / sec);
    return 0;
}
