#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stdatomic.h>
#include <stddef.h>
typedef struct {
  atomic_size_t tcache_hits;
  atomic_size_t tcache_misses;
  atomic_size_t mmap_calls;
  atomic_size_t munmap_calls;
  atomic_size_t large_cache_hits;
  atomic_size_t alloc_count;
  atomic_size_t free_count;
} alloc_stats_t;
extern alloc_stats_t stats;
void allocator_print_stats(void);

void *mymalloc(size_t size);
void myfree(void *ptr);
void *mycalloc(size_t n, size_t size);
void *myrealloc(void *blk, size_t size);

#endif // ! ALLOCATOR_H
