#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/mman.h>
#include "allocator.h"

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

pthread_mutex_t bin_locks[NUM_BINS];

#define LARGE_ALLOC 131072

void allocator_init() {
  for (int i = 0; i < NUM_BINS; i++) {
    pthread_mutex_init(&bin_locks[i], NULL);
  }
}

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
#define CHUNK_SIZE (2*1024*1024) // chunk size of 2mb for fixed chunks in mmap
static block_header_t *current_chunk=NULL;
static size_t chunk_remaining=0;


block_header_t *reqestspace(block_header_t *last, size_t size) {
  // block_header_t *block = sbrk(0);
  // if (block == (void *)-1) {
  //   return NULL;
  // }
  size_t needed=ALIGN(sizeof(block_header_t)+size);
  if(chunk_remaining<needed){
    if(chunk_remaining>=sizeof(block_header_t)+8){
      block_header_t* leftover=(block_header_t*)current_chunk;
      size_t usable=chunk_remaining-sizeof(block_header_t);
      usable=usable & ~7;
      if(usable>=8){
      leftover->size=usable;
        leftover->isfree=1;
        leftover->ismmapped=0;
        leftover->prev=NULL;
        leftover->next=NULL;
        leftover->bin_next=NULL;
        leftover->bin_prev=NULL;
      int bindex=bin_index(leftover->size);
      pthread_mutex_lock(&bin_locks[bindex]);
      bin_insert(leftover);
      pthread_mutex_unlock(&bin_locks[bindex]);
      }
    }
    current_chunk= mmap(NULL,CHUNK_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(current_chunk==MAP_FAILED)return NULL;
    chunk_remaining=CHUNK_SIZE;
  }
  block_header_t* block=(block_header_t*)current_chunk;
  current_chunk=(block_header_t*)((char*)current_chunk+needed);
  chunk_remaining-=needed;
   block->size = ALIGN(size);
  block->isfree = 0;
  block->ismmapped=0;
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
  leftover->ismmapped=0;
  leftover->bin_prev=NULL;
  leftover->bin_next=NULL;
  if (block->next) {
    block->next->prev = leftover;
  }
  block->next = leftover;
  block->size = size;
  bin_insert(leftover);
  if (block == lastblock)
    lastblock = leftover;
}

int idxcmp(const void *a, const void *b) { return (*(int *)a - *(int *)b); }
// lock 2 bins in ascending order to avoid deadlock in every thread
void lock_bins(int *indices, int n) {
  qsort(indices, n, sizeof(int), idxcmp);
  for (int i = 0; i < n; i++) {
    if (i == 0 || indices[i] != indices[i - 1]) {
      pthread_mutex_lock(&bin_locks[indices[i]]);
    }
  }
}

void unlock_bins(int *indices, int n) {
  for (int i = n - 1; i >= 0; i--) {
    if (i == n - 1 || indices[i] != indices[i + 1]) {
      pthread_mutex_unlock(&bin_locks[indices[i]]);
    }
  }
}

void coalesce(block_header_t *block) {

  // prev is free
  if (block->prev && block->isfree && block->prev->isfree &&
      (char *)(block->prev + 1) + block->prev->size == (char *)block) {
    int previndex = bin_index(block->prev->size);
    int blk_index = bin_index(block->size);
    int updsizeidx =
        bin_index(block->prev->size + sizeof(block_header_t) + block->size);
    int indices[3] = {previndex, blk_index, updsizeidx};
    lock_bins(indices, 3);
    bin_remove(block->prev);
    bin_remove(block);
    if (block == lastblock){
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
    unlock_bins(indices, 3);
  }
  // next is free
  if (block->next && block->isfree && block->next->isfree &&
      ((char *)(block + 1) + block->size == (char *)block->next)) {
    int nxtindex = bin_index(block->next->size);
    int blk_index = bin_index(block->size);
    int newidx =
        bin_index(block->size + sizeof(block_header_t) + block->next->size);
    int indices[3] = {nxtindex, blk_index, newidx};
    lock_bins(indices, 3);
    bin_remove(block->next);
    bin_remove(block);
    if (block->next == lastblock)
      lastblock = block;
    block->size = block->size + sizeof(block_header_t) + block->next->size;
    block->next = block->next->next;
    if (block->next)
      block->next->prev = block;
    bin_insert(block);
    unlock_bins(indices, 3);
  }
}

block_header_t *find_free_block(size_t size, int *lock_index) {
  int index = bin_index(size);
  while (index < NUM_BINS) {
    pthread_mutex_lock(&bin_locks[index]);
    block_header_t *block = bins[index];
    while (block) {
      if (block->size >= size) {
        *lock_index = index;
        return block;
      }
      block = block->bin_next;
    }
    pthread_mutex_unlock(&bin_locks[index]);
    index++;
  }
  return NULL;
}

void *mymalloc(size_t size) {
  if (size == 0)
    return NULL;
   size = ALIGN(size);
  pthread_mutex_lock(&heaplock);
  int lock_index;
  block_header_t *block = find_free_block(size, &lock_index);

  if (block) {
    bin_remove(block); // remove block before splitting to avoid bin confusions
    pthread_mutex_unlock(&bin_locks[lock_index]);
    if (block->size >= size + sizeof(block_header_t) + 1) {
      int leftover_index =
          bin_index(block->size - size - sizeof(block_header_t));
      pthread_mutex_lock(&bin_locks[leftover_index]);
      splitblocks(block, size);
      pthread_mutex_unlock(&bin_locks[leftover_index]);
    }
    block->isfree = 0;
  } else {
    if(size>=LARGE_ALLOC){
    block=mmap(NULL, sizeof(block_header_t)+size, PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS, -1,0);
    if(block==MAP_FAILED){
        pthread_mutex_unlock(&heaplock);
        return NULL;
      }
    block->size=size;
    block->ismmapped=1;
    block->isfree=0;
    block->next = NULL;
    block->prev = NULL;
    block->bin_next = NULL;
    block->bin_prev = NULL;
    pthread_mutex_unlock(&heaplock);
    return (void*)(block+1);
  }
    block = reqestspace(lastblock, size);
    if (!block) {
      pthread_mutex_unlock(&heaplock);
      return NULL;
    }
  }
  pthread_mutex_unlock(&heaplock);
  return (void *)(block + 1);
}

void myfree(void *ptr) {
  if (!ptr) {
    return;
  }
  pthread_mutex_lock(&heaplock);
  block_header_t *block = (block_header_t *)ptr - 1;
  if(block->ismmapped){
    munmap(block,sizeof(block_header_t)+block->size);
    pthread_mutex_unlock(&heaplock);
    return;
  }
  block->isfree = 1;
  bin_insert(block);
  coalesce(block);
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
  pthread_mutex_lock(&heaplock);
  block_header_t *block = (block_header_t *)blk - 1;
  if(block->ismmapped){
    size_t oldsize=block->size;
   pthread_mutex_unlock(&heaplock);
    void* newblock=mymalloc(size);
    if(!newblock)return NULL;
    memcpy(newblock, block,oldsize);
    munmap(block,sizeof(block_header_t)+block->size);
    return newblock;
  }
  if (block->next) {
    if ((char *)(block + 1) + block->size == (char *)block->next &&
        block->next->isfree &&
        block->size + sizeof(block_header_t) + block->next->size >= size) {
      int adjidx=bin_index(block->next->size);
      pthread_mutex_lock(&bin_locks[adjidx]);
      bin_remove(block->next);
      if (block->next == lastblock)
        lastblock = block;
      block->size = block->size + sizeof(block_header_t) + block->next->size;
      block->next = block->next->next;
      if (block->next)
        block->next->prev = block;
      pthread_mutex_unlock(&bin_locks[adjidx]);
      if (block->size > size + sizeof(block_header_t) + 8) {
        int leftoveridx=bin_index(block->size-size-sizeof(block_header_t));
        pthread_mutex_lock(&bin_locks[leftoveridx]);
        splitblocks(block, size);
        pthread_mutex_unlock(&bin_locks[leftoveridx]);
      }
      pthread_mutex_unlock(&heaplock);
      return (void *)(block + 1);
    }
  }
  size_t oldsize=block->size;
  pthread_mutex_unlock(&heaplock);
  void *newblock = mymalloc(size);
  if(!newblock)return NULL;
  memcpy(newblock, blk,oldsize);
  myfree(blk);
  return (void *)newblock;
}

