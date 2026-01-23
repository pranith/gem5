#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define DEFAULT_THREADS 4
#define DEFAULT_TASKS 200000
#define DEFAULT_DEQUE_SIZE (1 << 12)
#define WORK_ITERS 16

struct deque {
    uint64_t *tasks;
    size_t mask;
    atomic_size_t top;
    atomic_size_t bottom;
};

static int num_threads = DEFAULT_THREADS;
static int tasks_per_thread = DEFAULT_TASKS;
static size_t deque_size = DEFAULT_DEQUE_SIZE;
static struct deque *deques;
static pthread_barrier_t start_barrier;
static atomic_ulong total_done;
static volatile uint64_t sink;

static inline uint64_t nsec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static inline void deque_init(struct deque *d, size_t size) {
    d->tasks = calloc(size, sizeof(uint64_t));
    d->mask = size - 1;
    atomic_init(&d->top, 0);
    atomic_init(&d->bottom, 0);
}

static inline void deque_push(struct deque *d, uint64_t task) {
    size_t b = atomic_load_explicit(&d->bottom, memory_order_relaxed);
    d->tasks[b & d->mask] = task;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&d->bottom, b + 1, memory_order_release);
}

static inline int deque_pop(struct deque *d, uint64_t *task) {
    size_t b = atomic_load_explicit(&d->bottom, memory_order_relaxed);
    if (b == 0)
        return 0;
    b -= 1;
    atomic_store_explicit(&d->bottom, b, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    size_t t = atomic_load_explicit(&d->top, memory_order_acquire);
    if (t <= b) {
        *task = d->tasks[b & d->mask];
        if (t == b) {
            if (!atomic_compare_exchange_strong_explicit(&d->top, &t, t + 1,
                                                         memory_order_seq_cst,
                                                         memory_order_relaxed)) {
                atomic_store_explicit(&d->bottom, t + 1, memory_order_relaxed);
                return 0;
            }
            atomic_store_explicit(&d->bottom, t + 1, memory_order_relaxed);
        }
        return 1;
    }
    atomic_store_explicit(&d->bottom, t, memory_order_relaxed);
    return 0;
}

static inline int deque_steal(struct deque *d, uint64_t *task) {
    size_t t = atomic_load_explicit(&d->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    size_t b = atomic_load_explicit(&d->bottom, memory_order_acquire);
    if (t >= b)
        return 0;
    *task = d->tasks[t & d->mask];
    if (atomic_compare_exchange_strong_explicit(&d->top, &t, t + 1,
                                                memory_order_seq_cst,
                                                memory_order_relaxed)) {
        return 1;
    }
    return 0;
}

static void *worker(void *arg) {
    int tid = (int)(intptr_t)arg;
    uint64_t local = 0;
    unsigned long total_tasks = (unsigned long)num_threads * (unsigned long)tasks_per_thread;

    struct deque *my = &deques[tid];
    for (int i = 0; i < tasks_per_thread; ++i)
        deque_push(my, ((uint64_t)tid << 32) | (uint64_t)i);

    pthread_barrier_wait(&start_barrier);

    while (atomic_load_explicit(&total_done, memory_order_acquire) < total_tasks) {
        uint64_t task;
        if (deque_pop(my, &task)) {
            local ^= task;
            for (int k = 0; k < WORK_ITERS; ++k)
                local ^= (local << 5) + (uint64_t)k;
            atomic_fetch_add_explicit(&total_done, 1, memory_order_acq_rel);
            continue;
        }

        int stole = 0;
        for (int v = 0; v < num_threads; ++v) {
            if (v == tid) continue;
            if (deque_steal(&deques[v], &task)) {
                local ^= task;
                for (int k = 0; k < WORK_ITERS; ++k)
                    local ^= (local << 3) + (uint64_t)k;
                atomic_fetch_add_explicit(&total_done, 1, memory_order_acq_rel);
                stole = 1;
                break;
            }
        }
        if (!stole)
            sched_yield();
    }

    sink ^= local;
    return NULL;
}

static void usage(const char *prog) {
    printf("Usage: %s [threads] [tasks_per_thread] [deque_size]\n", prog);
}

int main(int argc, char **argv) {
    if (argc > 1) num_threads = atoi(argv[1]);
    if (argc > 2) tasks_per_thread = atoi(argv[2]);
    if (argc > 3) deque_size = (size_t)strtoull(argv[3], NULL, 10);

    if (num_threads <= 0) num_threads = DEFAULT_THREADS;
    if (tasks_per_thread <= 0) tasks_per_thread = DEFAULT_TASKS;
    if (deque_size < 2 || (deque_size & (deque_size - 1)) != 0)
        deque_size = DEFAULT_DEQUE_SIZE;

    deques = calloc((size_t)num_threads, sizeof(*deques));
    if (!deques) return 1;
    for (int i = 0; i < num_threads; ++i)
        deque_init(&deques[i], deque_size);

    pthread_t *threads = calloc((size_t)num_threads, sizeof(pthread_t));
    if (!threads) return 1;
    pthread_barrier_init(&start_barrier, NULL, (unsigned)num_threads);
    atomic_init(&total_done, 0);

    uint64_t start = nsec_now();
    for (int i = 0; i < num_threads; ++i)
        pthread_create(&threads[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < num_threads; ++i)
        pthread_join(threads[i], NULL);
    uint64_t end = nsec_now();

    pthread_barrier_destroy(&start_barrier);
    for (int i = 0; i < num_threads; ++i)
        free(deques[i].tasks);
    free(deques);
    free(threads);

    double sec = (double)(end - start) / 1e9;
    double total_tasks = (double)tasks_per_thread * num_threads;
    printf("Tasks: %.0f  time: %.3fs  throughput: %.0f tasks/s\n",
           total_tasks, sec, total_tasks / sec);
    return 0;
}
