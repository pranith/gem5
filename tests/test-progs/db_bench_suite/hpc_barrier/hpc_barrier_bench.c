#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CACHELINE 64
#define DEFAULT_THREADS 4
#define DEFAULT_PHASES 50000
#define DEFAULT_WORK 32
#define DEFAULT_ARRAY_SIZE (1 << 16)

static int num_threads = DEFAULT_THREADS;
static int phases = DEFAULT_PHASES;
static int work_iters = DEFAULT_WORK;
static size_t array_size = DEFAULT_ARRAY_SIZE;

static uint64_t *array_data;
static pthread_barrier_t phase_barrier;
static volatile uint64_t sink;

static inline uint64_t nsec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void *worker(void *arg) {
    int tid = (int)(intptr_t)arg;
    uint64_t local = 0;
    size_t stride = (array_size / (size_t)num_threads);
    if (stride == 0) stride = 1;
    size_t start = (size_t)tid * stride;
    size_t end = start + stride;
    if (end > array_size) end = array_size;

    for (int p = 0; p < phases; ++p) {
        for (size_t i = start; i < end; i += 4) {
            local ^= array_data[i] + (uint64_t)p;
        }
        for (int w = 0; w < work_iters; ++w)
            local ^= (local << 1) + (uint64_t)w;

        pthread_barrier_wait(&phase_barrier);
    }

    sink ^= local;
    return NULL;
}

static void usage(const char *prog) {
    printf("Usage: %s [threads] [phases] [work_iters] [array_size]\n", prog);
}

int main(int argc, char **argv) {
    if (argc > 1) num_threads = atoi(argv[1]);
    if (argc > 2) phases = atoi(argv[2]);
    if (argc > 3) work_iters = atoi(argv[3]);
    if (argc > 4) array_size = (size_t)strtoull(argv[4], NULL, 10);

    if (num_threads <= 0) num_threads = DEFAULT_THREADS;
    if (phases <= 0) phases = DEFAULT_PHASES;
    if (work_iters <= 0) work_iters = DEFAULT_WORK;
    if (array_size == 0) array_size = DEFAULT_ARRAY_SIZE;

    if (posix_memalign((void **)&array_data, CACHELINE,
                       array_size * sizeof(*array_data)) != 0) {
        return 1;
    }
    for (size_t i = 0; i < array_size; ++i)
        array_data[i] = (uint64_t)i * 11400714819323198485ULL;

    pthread_t *threads = calloc((size_t)num_threads, sizeof(pthread_t));
    if (!threads) return 1;
    pthread_barrier_init(&phase_barrier, NULL, (unsigned)num_threads);

    uint64_t start = nsec_now();
    for (int i = 0; i < num_threads; ++i)
        pthread_create(&threads[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < num_threads; ++i)
        pthread_join(threads[i], NULL);
    uint64_t end = nsec_now();

    pthread_barrier_destroy(&phase_barrier);
    free(threads);
    free(array_data);

    double sec = (double)(end - start) / 1e9;
    printf("Phases: %d  time: %.3fs  phase time: %.3fus\n",
           phases, sec, (sec * 1e6) / phases);
    return 0;
}
