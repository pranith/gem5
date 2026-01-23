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
#define WORK_ITERS 8

struct slot {
    atomic_size_t seq;
    uint64_t data;
} __attribute__((aligned(CACHELINE)));

static int num_producers = DEFAULT_PRODUCERS;
static int items_per_producer = DEFAULT_ITEMS;
static size_t ring_size = DEFAULT_RING_SIZE;

static struct slot *ring;
static atomic_size_t head;
static atomic_size_t tail;
static pthread_barrier_t start_barrier;
static volatile uint64_t sink;

static inline uint64_t nsec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void *producer(void *arg) {
    int tid = (int)(intptr_t)arg + 1;
    uint64_t local = 0;

    pthread_barrier_wait(&start_barrier);

    for (int i = 0; i < items_per_producer; ++i) {
        size_t pos = atomic_fetch_add_explicit(&tail, 1, memory_order_acq_rel);
        struct slot *s = &ring[pos & (ring_size - 1)];
        while (atomic_load_explicit(&s->seq, memory_order_acquire) != pos)
            sched_yield();
        s->data = ((uint64_t)tid << 32) | (uint64_t)i;
        atomic_store_explicit(&s->seq, pos + 1, memory_order_release);
        local ^= s->data;
    }

    sink ^= local;
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    uint64_t local = 0;
    size_t total = (size_t)num_producers * (size_t)items_per_producer;

    pthread_barrier_wait(&start_barrier);

    for (size_t i = 0; i < total; ++i) {
        size_t pos = atomic_load_explicit(&head, memory_order_relaxed);
        struct slot *s = &ring[pos & (ring_size - 1)];
        while (atomic_load_explicit(&s->seq, memory_order_acquire) != pos + 1)
            sched_yield();
        uint64_t value = s->data;
        for (int k = 0; k < WORK_ITERS; ++k)
            local ^= (value << 3) + (uint64_t)k;
        atomic_store_explicit(&s->seq, pos + ring_size, memory_order_release);
        atomic_store_explicit(&head, pos + 1, memory_order_release);
    }

    sink ^= local;
    return NULL;
}

static void usage(const char *prog) {
    printf("Usage: %s [producers] [items_per_producer] [ring_size]\n", prog);
}

int main(int argc, char **argv) {
    if (argc > 1) num_producers = atoi(argv[1]);
    if (argc > 2) items_per_producer = atoi(argv[2]);
    if (argc > 3) ring_size = (size_t)strtoull(argv[3], NULL, 10);

    if (num_producers <= 0) num_producers = DEFAULT_PRODUCERS;
    if (items_per_producer <= 0) items_per_producer = DEFAULT_ITEMS;
    if (ring_size < 2 || (ring_size & (ring_size - 1)) != 0)
        ring_size = DEFAULT_RING_SIZE;

    if (posix_memalign((void **)&ring, CACHELINE, ring_size * sizeof(*ring)) != 0)
        return 1;
    for (size_t i = 0; i < ring_size; ++i) {
        atomic_init(&ring[i].seq, i);
        ring[i].data = 0;
    }

    atomic_init(&head, 0);
    atomic_init(&tail, 0);

    int total_threads = num_producers + 1;
    pthread_t *threads = calloc((size_t)total_threads, sizeof(pthread_t));
    if (!threads) return 1;

    pthread_barrier_init(&start_barrier, NULL, (unsigned)total_threads);

    uint64_t start = nsec_now();
    pthread_create(&threads[0], NULL, consumer, NULL);
    for (int i = 0; i < num_producers; ++i)
        pthread_create(&threads[i + 1], NULL, producer, (void *)(intptr_t)i);
    for (int i = 0; i < total_threads; ++i)
        pthread_join(threads[i], NULL);
    uint64_t end = nsec_now();

    pthread_barrier_destroy(&start_barrier);
    free(threads);
    free(ring);

    double sec = (double)(end - start) / 1e9;
    double total_items = (double)num_producers * items_per_producer;
    printf("Items: %.0f  time: %.3fs  throughput: %.0f items/s\n",
           total_items, sec, total_items / sec);
    return 0;
}
