#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdalign.h>
#include <stdatomic.h>

#define DEBUG 0

#define CACHELINE 64
#define BARRIER_ROUNDS 500000
#define CHASE_LEN 9
// Use a stride that maps all nodes to the same L1D set on typical 64B-line,
// 8-way, 32KiB caches: 64 sets -> stride of 64*64 = 4096 bytes.
#define THRASH_STRIDE 4096

static int num_threads = 4;
static volatile uint64_t sink;
static atomic_flag lock = ATOMIC_FLAG_INIT;

struct Node {
    struct Node *next;
    char pad[CACHELINE - sizeof(struct Node *)];
} __attribute__((aligned(CACHELINE)));

static struct Node *chain_head;

static inline void pointer_chase(void) {
    struct Node *p = chain_head;
    for (int i = 0; i < CHASE_LEN; ++i) {
        // Pointer chase forces dependent loads; nodes share index bits.
        p = p->next;
        sink += (uintptr_t)p;
    }
}

static void *worker(void *arg) {
    (void)arg;
    for (int r = 0; r < BARRIER_ROUNDS; ++r) {
        while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire))
            ;
        pointer_chase();
        atomic_flag_clear_explicit(&lock, memory_order_release);
#if DEBUG
        printf("Chasing... %d\n", r);
#endif
    }
    return NULL;
}

static struct Node *init_chain(void) {
    size_t buf_size = THRASH_STRIDE * CHASE_LEN;
    char *raw = NULL;
    if (posix_memalign((void **)&raw, CACHELINE, buf_size) != 0)
        return NULL;

    for (int i = 0; i < CHASE_LEN; ++i) {
        struct Node *node = (struct Node *)(raw + i * THRASH_STRIDE);
        int next_idx = (i + 1) % CHASE_LEN;
        node->next = (struct Node *)(raw + next_idx * THRASH_STRIDE);
    }
    return (struct Node *)raw;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        num_threads = atoi(argv[1]);
        if (num_threads <= 0) num_threads = 4;
    }

    chain_head = init_chain();
    if (!chain_head) return 1;

    pthread_t *threads = calloc(num_threads, sizeof(pthread_t));
    if (!threads) return 1;

    for (int i = 0; i < num_threads; ++i)
        pthread_create(&threads[i], NULL, worker, NULL);
    for (int i = 0; i < num_threads; ++i)
        pthread_join(threads[i], NULL);

    free((void *)chain_head);
    free(threads);
    printf("done\n");
    return 0;
}
