#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// compile two ways:
// gcc -pthread -O2 -DUSE_CUSTOM benchmark.c allocator.c -o bench_custom
// gcc -pthread -O2            benchmark.c                -o bench_libc

#ifdef USE_CUSTOM
#include "allocator.h"
#define MALLOC mymalloc
#define FREE myfree
#define REALLOC myrealloc
#define CALLOC mycalloc
#else
#define MALLOC malloc
#define FREE free
#define REALLOC realloc
#define CALLOC calloc
#endif

#define NS(ts) ((ts).tv_sec * 1000000000LL + (ts).tv_nsec)

static double elapsed_ms(struct timespec a, struct timespec b) {
  return (NS(b) - NS(a)) / 1e6;
}

// ── Bench 1: single-threaded alloc+free pairs ────────────────────────────────
void bench_single(int iterations) {
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);

  for (int i = 0; i < iterations; i++) {
    void *p = MALLOC((rand() % 512) + 8);
    FREE(p);
  }

  clock_gettime(CLOCK_MONOTONIC, &b);
  printf("  single alloc+free (%dk iters): %.2f ms\n", iterations / 1000,
         elapsed_ms(a, b));
}

// ── Bench 2: batch alloc then batch free ─────────────────────────────────────
void bench_batch(int n) {
  void **ptrs = malloc(n * sizeof(void *)); // always libc for harness
  struct timespec a, b;

  clock_gettime(CLOCK_MONOTONIC, &a);
  for (int i = 0; i < n; i++)
    ptrs[i] = MALLOC((rand() % 256) + 8);
  clock_gettime(CLOCK_MONOTONIC, &b);
  printf("  batch alloc  (%dk): %.2f ms\n", n / 1000, elapsed_ms(a, b));

  clock_gettime(CLOCK_MONOTONIC, &a);
  for (int i = 0; i < n; i++)
    FREE(ptrs[i]);
  clock_gettime(CLOCK_MONOTONIC, &b);
  printf("  batch free   (%dk): %.2f ms\n", n / 1000, elapsed_ms(a, b));

  free(ptrs);
}

// ── Bench 3: mixed sizes (small + large) ─────────────────────────────────────
void bench_mixed_sizes(int iterations) {
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);

  for (int i = 0; i < iterations; i++) {
    size_t size = (i % 10 == 0) ? 200000 : (rand() % 512) + 8;
    void *p = MALLOC(size);
    FREE(p);
  }

  clock_gettime(CLOCK_MONOTONIC, &b);
  printf("  mixed sizes  (%dk iters): %.2f ms\n", iterations / 1000,
         elapsed_ms(a, b));
}

// ── Bench 4: realloc chain ───────────────────────────────────────────────────
void bench_realloc(int iterations) {
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);

  void *p = MALLOC(8);
  for (int i = 0; i < iterations; i++) {
    p = REALLOC(p, (i % 512) + 8);
  }
  FREE(p);

  clock_gettime(CLOCK_MONOTONIC, &b);
  printf("  realloc chain(%dk iters): %.2f ms\n", iterations / 1000,
         elapsed_ms(a, b));
}

// ── Bench 5: multithreaded ───────────────────────────────────────────────────
#define BENCH_THREADS 8
#define BENCH_ITERS 5000

void *thread_bench(void *arg) {
  void *ptrs[100];
  unsigned int seed = (unsigned int)(size_t)arg; // unique seed per thread

  for (int i = 0; i < BENCH_ITERS; i++) {
    for (int j = 0; j < 100; j++)
      ptrs[j] = MALLOC((rand_r(&seed) % 256) + 8);
    for (int j = 0; j < 100; j++)
      FREE(ptrs[j]);
  }
  return NULL;
}

void bench_multithreaded() {
  pthread_t threads[BENCH_THREADS];
  struct timespec a, b;

  clock_gettime(CLOCK_MONOTONIC, &a);
  for (int i = 0; i < BENCH_THREADS; i++)
    pthread_create(&threads[i], NULL, thread_bench, (void *)(size_t)(i + 1));
  for (int i = 0; i < BENCH_THREADS; i++)
    pthread_join(threads[i], NULL);
  clock_gettime(CLOCK_MONOTONIC, &b);

  printf("  multithreaded(%d threads, %dk iters each): %.2f ms\n",
         BENCH_THREADS, BENCH_ITERS / 1000, elapsed_ms(a, b));
}

int main() {
#ifdef USE_CUSTOM
  printf("=== custom allocator ===\n");
#else
  printf("=== libc allocator ===\n");
#endif

  srand(42); // same seed for fair comparison

  printf("\n-- single threaded --\n");
  bench_single(1000000);
  bench_batch(10000);
  bench_mixed_sizes(100000);
  bench_realloc(100000);

  printf("\n-- multithreaded --\n");
  bench_multithreaded();

  printf("\ndone\n");

#ifdef USE_CUSTOM
  allocator_print_stats();
#endif
  return 0;
}
