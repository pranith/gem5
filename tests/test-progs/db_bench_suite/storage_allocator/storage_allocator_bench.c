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
#define DEFAULT_OPS 200000
#define DEFAULT_POOL_SIZE (1 << 16)
#define DEFAULT_BATCH 32
#define DEFAULT_LOCAL 64

static int num_threads = DEFAULT_THREADS;
static int ops_per_thread = DEFAULT_OPS;
static size_t pool_size = DEFAULT_POOL_SIZE;
static int batch_size = DEFAULT_BATCH;
static int local_capacity = DEFAULT_LOCAL;

static uint64_t *pool;
static int *global_stack;
static int global_top;
static atomic_uint global_lock;
static pthread_barrier_t start_barrier;
static volatile uint64_t sink;

static inline uint64_t nsec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static inline void lock_acquire(atomic_uint *lock) {
    unsigned expected;
    int spins = 0;
    for (;;) {
        expected = 0;
        if (atomic_compare_exchange_weak_explicit(lock, &expected, 1,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
            return;
        }
        if ((++spins & 0x3f) == 0) sched_yield();
    }
}

static inline void lock_release(atomic_uint *lock) {
    atomic_store_explicit(lock, 0, memory_order_release);
}

static void global_push(int id) {
    global_stack[global_top++] = id;
}

static int global_pop(void) {
    if (global_top == 0)
        return -1;
    return global_stack[--global_top];
}

static void *worker(void *arg) {
    (void)arg;
    int *local = calloc((size_t)local_capacity, sizeof(int));
    if (!local) return NULL;
    int local_top = 0;
    uint64_t local_sink = 0;

    pthread_barrier_wait(&start_barrier);

    for (int i = 0; i < ops_per_thread; ++i) {
        int id = -1;
        if (local_top > 0) {
            id = local[--local_top];
        } else {
            lock_acquire(&global_lock);
            for (int k = 0; k < batch_size && local_top < local_capacity; ++k) {
                int val = global_pop();
                if (val < 0)
                    break;
                local[local_top++] = val;
            }
            lock_release(&global_lock);
            if (local_top > 0)
                id = local[--local_top];
        }

        if (id >= 0) {
            local_sink ^= pool[(size_t)id] + (uint64_t)i;
            pool[(size_t)id] ^= local_sink;
        }

        if ((i & 1) == 0) {
            if (local_top < local_capacity && id >= 0) {
                local[local_top++] = id;
            } else if (id >= 0) {
                lock_acquire(&global_lock);
                if (global_top < (int)pool_size)
                    global_push(id);
                lock_release(&global_lock);
            }
        }
    }

    sink ^= local_sink;
    free(local);
    return NULL;
}

static void usage(const char *prog) {
    printf("Usage: %s [threads] [ops_per_thread] [pool_size] [batch_size] [local_capacity]\n",
           prog);
}

int main(int argc, char **argv) {
    if (argc > 1) num_threads = atoi(argv[1]);
    if (argc > 2) ops_per_thread = atoi(argv[2]);
    if (argc > 3) pool_size = (size_t)strtoull(argv[3], NULL, 10);
    if (argc > 4) batch_size = atoi(argv[4]);
    if (argc > 5) local_capacity = atoi(argv[5]);

    if (num_threads <= 0) num_threads = DEFAULT_THREADS;
    if (ops_per_thread <= 0) ops_per_thread = DEFAULT_OPS;
    if (pool_size == 0) pool_size = DEFAULT_POOL_SIZE;
    if (batch_size <= 0) batch_size = DEFAULT_BATCH;
    if (local_capacity <= 0) local_capacity = DEFAULT_LOCAL;
    if (batch_size > local_capacity) batch_size = local_capacity;

    if (posix_memalign((void **)&pool, CACHELINE, pool_size * sizeof(*pool)) != 0)
        return 1;
    global_stack = calloc(pool_size, sizeof(int));
    if (!global_stack) return 1;

    for (size_t i = 0; i < pool_size; ++i) {
        pool[i] = (uint64_t)i * 2654435761u;
        global_stack[i] = (int)i;
    }
    global_top = (int)pool_size;
    atomic_init(&global_lock, 0);

    pthread_t *threads = calloc((size_t)num_threads, sizeof(pthread_t));
    if (!threads) return 1;
    pthread_barrier_init(&start_barrier, NULL, (unsigned)num_threads);

    uint64_t start = nsec_now();
    for (int i = 0; i < num_threads; ++i)
        pthread_create(&threads[i], NULL, worker, NULL);
    for (int i = 0; i < num_threads; ++i)
        pthread_join(threads[i], NULL);
    uint64_t end = nsec_now();

    pthread_barrier_destroy(&start_barrier);
    free(threads);
    free(global_stack);
    free(pool);

    double sec = (double)(end - start) / 1e9;
    double total_ops = (double)ops_per_thread * num_threads;
    printf("Operations: %.0f  time: %.3fs  throughput: %.0f ops/s\n",
           total_ops, sec, total_ops / sec);
    return 0;
}
