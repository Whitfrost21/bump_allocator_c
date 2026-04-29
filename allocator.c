#include "allocator.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#define NUM_BINS 8
#define ALIGN(size) (((size) + 7) & ~7)
typedef struct block_header {
  size_t size;
  int isfree;
  int ismmapped;
  struct block_header *next;
  struct block_header *prev;
  struct block_header *bin_next;
  struct block_header *bin_prev;
} block_header_t;

static block_header_t *bins[NUM_BINS];

#define TCACHE_MAX 64
typedef struct {
  block_header_t *bins[NUM_BINS];
  int count[NUM_BINS];
} tcache_t;
__thread tcache_t tcache = {0};

#define LARGE_ALLOC 131072

alloc_stats_t stats = {0};

int bin_index(size_t size) {
  if (size <= 8)
    return 0;
  int index = 31 - __builtin_clz(size - 1) - 2;
  if (index >= NUM_BINS)
    return NUM_BINS - 1;
  return index;
}

void bin_insert(block_header_t *block) {
  int index = bin_index(block->size);
  block->bin_prev = NULL;
  block->bin_next = bins[index];
  if (bins[index])
    bins[index]->bin_prev = block;
  bins[index] = block;
}

void bin_remove(block_header_t *block) {
  int index = bin_index(block->size);

  if (!block->bin_prev && bins[index] != block)
    return;
  if (block->bin_prev)
    block->bin_prev->bin_next = block->bin_next;
  else
    bins[index] = block->bin_next;
  if (block->bin_next) {
    block->bin_next->bin_prev = block->bin_prev;
  }
  block->bin_next = NULL;
  block->bin_prev = NULL;
}

static block_header_t *lastblock = NULL;
pthread_mutex_t heaplock = PTHREAD_MUTEX_INITIALIZER;
#define CHUNK_SIZE                                                             \
  (2 * 1024 * 1024) // chunk size of 2mb for fixed chunks in mmap
static block_header_t *current_chunk = NULL;
static size_t chunk_remaining = 0;

block_header_t *reqestspace(block_header_t *last, size_t size) {
  // block_header_t *block = sbrk(0);
  // if (block == (void *)-1) {
  //   return NULL;
  // }
  size_t needed = ALIGN(sizeof(block_header_t) + size);
  if (chunk_remaining < needed) {
    if (chunk_remaining >= sizeof(block_header_t) + 8) {
      block_header_t *leftover = (block_header_t *)current_chunk;
      size_t usable = chunk_remaining - sizeof(block_header_t);
      usable = usable & ~7;
      if (usable >= 8) {
        leftover->size = usable;
        leftover->isfree = 1;
        leftover->ismmapped = 0;
        leftover->prev = NULL;
        leftover->next = NULL;
        leftover->bin_next = NULL;
        leftover->bin_prev = NULL;
        bin_insert(leftover);
      }
    }
    current_chunk = mmap(NULL, CHUNK_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (current_chunk == MAP_FAILED)
      return NULL;
    chunk_remaining = CHUNK_SIZE;
  }
  block_header_t *block = (block_header_t *)current_chunk;
  current_chunk = (block_header_t *)((char *)current_chunk + needed);
  chunk_remaining -= needed;
  block->size = ALIGN(size);
  block->isfree = 0;
  block->ismmapped = 0;
  block->prev = last;
  block->next = NULL;
  block->bin_next = NULL;
  block->bin_prev = NULL;
  if (last) {
    last->next = block;
  }
  lastblock = block;
  return block;
};

void splitblocks(block_header_t *block, size_t size) {
  block_header_t *leftover = (block_header_t *)((char *)(block + 1) + size);
  leftover->size = block->size - size - sizeof(block_header_t);
  leftover->isfree = 1;
  leftover->next = block->next;
  leftover->prev = block;
  leftover->ismmapped = 0;
  leftover->bin_prev = NULL;
  leftover->bin_next = NULL;
  if (block->next) {
    block->next->prev = leftover;
  }
  block->next = leftover;
  block->size = size;
  bin_insert(leftover);
  if (block == lastblock)
    lastblock = leftover;
}

void coalesce(block_header_t *block) {

  // prev is free
  if (block->prev && block->isfree && block->prev->isfree &&
      (char *)(block->prev + 1) + block->prev->size == (char *)block) {

    bin_remove(block->prev);
    bin_remove(block);
    if (block == lastblock) {
      lastblock = block->prev;
    }
    block->prev->size =
        block->prev->size + sizeof(block_header_t) + block->size;
    block->prev->next = block->next;
    if (block->next) {
      block->next->prev = block->prev;
    }
    bin_insert(block->prev);
    block = block->prev;
  }
  // next is free
  if (block->next && block->isfree && block->next->isfree &&
      ((char *)(block + 1) + block->size == (char *)block->next)) {
    bin_remove(block->next);
    bin_remove(block);
    if (block->next == lastblock)
      lastblock = block;
    block->size = block->size + sizeof(block_header_t) + block->next->size;
    block->next = block->next->next;
    if (block->next)
      block->next->prev = block;
    bin_insert(block);
  }
}

block_header_t *find_free_block(size_t size) {
  int index = bin_index(size);
  while (index < NUM_BINS) {
    block_header_t *block = bins[index];
    while (block) {
      if (block->size >= size) {
        return block;
      }
      block = block->bin_next;
    }
    index++;
  }
  return NULL;
}

void flush_tcache(int idx) {
  pthread_mutex_lock(&heaplock);
  block_header_t *block = tcache.bins[idx];
  while (block) {
    block_header_t *next = block->bin_next;
    block->bin_next = NULL;
    block->bin_prev = NULL;
    bin_insert(block);
    block = next;
  }
  tcache.bins[idx] = NULL;
  tcache.count[idx] = 0;
  pthread_mutex_unlock(&heaplock);
}

void *mymalloc(size_t size) {
  if (size == 0)
    return NULL;
  size = ALIGN(size);

  if (size < LARGE_ALLOC) {
    int idx = bin_index(size);
    if (tcache.bins[idx]) {
      block_header_t *block = tcache.bins[idx];
      while (block) {
        if (block->size >= size) {
          if (block->bin_next)
            block->bin_next->bin_prev = block->bin_prev;
          if (block->bin_prev)
            block->bin_prev->bin_next = block->bin_next;
          else
            tcache.bins[idx] = block->bin_next;
          tcache.count[idx]--;
          block->isfree = 0;
          block->bin_next = NULL;
          block->bin_prev = NULL;
          atomic_fetch_add(&stats.tcache_hits, 1);
          atomic_fetch_add(&stats.alloc_count, 1);
          return (void *)(block + 1);
        }
        block = block->bin_next;
      }
    }
  }

  if (size >= LARGE_ALLOC) {

    block_header_t *block =
        mmap(NULL, sizeof(block_header_t) + size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block == MAP_FAILED) {
      return NULL;
    }
    block->size = size;
    block->ismmapped = 1;
    block->isfree = 0;
    block->next = NULL;
    block->prev = NULL;
    block->bin_next = NULL;
    block->bin_prev = NULL;
    atomic_fetch_add(&stats.mmap_calls, 1);
    atomic_fetch_add(&stats.alloc_count, 1);
    return (void *)(block + 1);
  }

  atomic_fetch_add(&stats.tcache_misses, 1);
  pthread_mutex_lock(&heaplock);

  block_header_t *block = find_free_block(size);

  if (block) {
    bin_remove(block); // remove block before splitting to avoid bin confusions
    if (block->size >= size + sizeof(block_header_t) + 1) {

      splitblocks(block, size);
    }
    block->isfree = 0;
  } else {

    block = reqestspace(lastblock, size);
    if (!block) {
      pthread_mutex_unlock(&heaplock);
      return NULL;
    }
  }
  atomic_fetch_add(&stats.alloc_count, 1);
  pthread_mutex_unlock(&heaplock);
  return (void *)(block + 1);
}

void myfree(void *ptr) {
  if (!ptr) {
    return;
  }
  block_header_t *block = (block_header_t *)ptr - 1;
  if (!block->ismmapped) {
    int idx = bin_index(block->size);
    if (tcache.count[idx] >= TCACHE_MAX) {
      flush_tcache(idx);
    }
    block->isfree = 1;
    block->bin_next = tcache.bins[idx];
    block->bin_prev = NULL;
    tcache.bins[idx] = block;
    tcache.count[idx]++;
    atomic_fetch_add(&stats.free_count, 1);
    return;
  }
  pthread_mutex_lock(&heaplock);
  if (block->ismmapped) {
    munmap(block, sizeof(block_header_t) + block->size);
    atomic_fetch_add(&stats.munmap_calls, 1);
    pthread_mutex_unlock(&heaplock);
    return;
  }
  block->isfree = 1;
  bin_insert(block);
  coalesce(block);
  atomic_fetch_add(&stats.free_count, 1);
  pthread_mutex_unlock(&heaplock);
}

void *mycalloc(size_t n, size_t size) {
  if (n != 0 && size > SIZE_MAX / n)
    return NULL;
  if (n == 0 || size == 0)
    return NULL;
  void *block = mymalloc(n * size);
  if (!block)
    return NULL;
  memset(block, 0, n * size);
  return block;
}

void *myrealloc(void *blk, size_t size) {
  if (size == 0) {
    myfree(blk);
    return NULL;
  }
  // pthread_mutex_lock(&heaplock);
  // block_header_t *block = (block_header_t *)blk - 1;
  // if (block->ismmapped) {
  //   size_t oldsize = block->size;
  //   pthread_mutex_unlock(&heaplock);
  //   void *newblock = mymalloc(size);
  //   if (!newblock)
  //     return NULL;
  //   size_t tocopy = oldsize < size ? oldsize : size;
  //   memcpy(newblock, blk, tocopy);
  //   myfree(blk);
  //   return newblock;
  // }
  //
  block_header_t *block = (block_header_t *)blk - 1;
  size_t oldsize = block->size;
  int wasmmapped = block->ismmapped;

  // skipping in place expansion cause i dont have any way to differentaite
  // between tcache blocks and global heap blocks if (block->next &&
  // block->next->isfree &&
  //     (block->next->bin_prev != NULL ||
  //      bins[bin_index(block->next->size)] == block->next) &&
  //     (char *)(block + 1) + block->size == (char *)block->next &&
  //     block->size + sizeof(block_header_t) + block->next->size >= size)
  // {
  //   if ((char *)(block + 1) + block->size == (char *)block->next &&
  //       block->next->isfree &&
  //       block->size + sizeof(block_header_t) + block->next->size >= size) {
  //     bin_remove(block->next);
  //     if (block->next == lastblock)
  //       lastblock = block;
  //     block->size = block->size + sizeof(block_header_t) + block->next->size;
  //     block->next = block->next->next;
  //     if (block->next)
  //       block->next->prev = block;
  //     if (block->size > size + sizeof(block_header_t) + 8) {
  //       splitblocks(block, size);
  //     }
  //     pthread_mutex_unlock(&heaplock);
  //     return (void *)(block + 1);
  //   }
  // }
  // size_t oldsize = block->size;
  // pthread_mutex_unlock(&heaplock);
  void *newblock = mymalloc(size);
  if (!newblock)
    return NULL;
  memcpy(newblock, blk, oldsize < size ? oldsize : size);
  myfree(blk);
  return (void *)newblock;
}

void allocator_print_stats(void) {
  size_t hits = atomic_load(&stats.tcache_hits);
  size_t misses = atomic_load(&stats.tcache_misses);
  size_t total = hits + misses;
  size_t allocs = atomic_load(&stats.alloc_count);
  size_t frees = atomic_load(&stats.free_count);
  printf("\n ===allocator stats===\n");
  printf("tcache hits: %zu\n", hits);
  printf("tcache misses: %zu\n", misses);
  printf("tcache hit rate : %.1f%%\n", total ? 100.0 * hits / total : 0.0);
  printf("total allocs:      %zu\n", allocs);
  printf("total frees:       %zu\n", frees);
  printf("live allocations:  %zu\n", allocs - frees);
  printf("mmap calls: %zu\n", atomic_load(&stats.mmap_calls));
  printf("munmap calls: %zu\n", atomic_load(&stats.munmap_calls));
  // live bytes is bugged causing size_t overflow
  //  printf("live bytes: %zu\n", atomic_load(&stats.live_bytes));
}
