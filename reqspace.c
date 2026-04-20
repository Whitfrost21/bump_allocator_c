#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/mman.h>
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
    if (block == lastblock)
      lastblock = block->prev;
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
  pthread_mutex_unlock(&heaplock);
  void *newblock = mymalloc(size);
  memcpy(newblock, blk, block->size);
  myfree(blk);
  return (void *)newblock;
}

// test case for threads safety
// #define NUM_THREADS 4
// #define ALLOCS_PER_THREAD 100
// //
// void *thread_work(void *arg) {
//     int id = *(int *)arg;
//     char *ptrs[ALLOCS_PER_THREAD];
//     size_t sizes[ALLOCS_PER_THREAD];
//
//     // interleave alloc and free randomly
//     for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
//         sizes[i] = rand() % 512 + 1;
//         ptrs[i] = (char *)mymalloc(sizes[i]);
//         if (ptrs[i]) ptrs[i][0] = id;  // write only 1 byte
//
//         // randomly free a previous allocation
//         if (i > 0 && rand() % 2) {
//             int victim = rand() % i;
//             if (ptrs[victim]) {
//                 myfree(ptrs[victim]);
//                 ptrs[victim] = NULL;
//             }
//         }
//     }
//     // verify and free remaining
//     int corrupted = 0;
//     for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
//         if (ptrs[i]) {
//             if (ptrs[i][0] != id) corrupted++;
//             myfree(ptrs[i]);
//         }
//     }
//     printf("Thread %d - corrupted: %d %s\n",
//            id, corrupted, corrupted == 0 ? "PASS" : "FAIL");
//     return NULL;
// }





int main() {
  // all the code here in main is to test the functions of malloc in different
  // test cases
  // printf("header size=%zu\n",sizeof(block_header_t)); //size of my
  // block_header is 48 here
  allocator_init();


  // thread safety
  // pthread_t threads[NUM_THREADS];
  // int ids[NUM_THREADS];
  //
  // for (int i = 0; i < NUM_THREADS; i++) {
  //   ids[i] = i;
  //   pthread_create(&threads[i], NULL, thread_work, &ids[i]);
  // }
  //
  // for (int i = 0; i < NUM_THREADS; i++) {
  //   pthread_join(threads[i], NULL);
  // }
  //
  // printf("all threads done\n");



  // realloc
  // char *a = (char *)mymalloc(16);
  // char *b = (char *)mymalloc(64);
  // myfree(b);
  // char *a1 = (char *)myrealloc(
  //     a, 40); // both a and a1 have same address now , modifying any of them
  //             // changes value at address
  // printf("reallocated b's freed block to a so (a1==a) with extra space:%s\n",
  //        (a == a1) ? "yes" : "no");
  //
  // char *x = (char *)mymalloc(16);
  // strcpy(x, "hello");
  // char *y = (char *)mymalloc(16);
  // char *x1 = (char *)myrealloc(x, 64);
  // printf("x cannot use y's block cause it's not free so x1 allocates new
  // block "
  //        "with x's data\n");
  // printf("now x1 contains hello with extra space:%s\n",
  //        strcmp(x1, "hello") == 0 ? "yes" : "no");
  // myfree(x1);
  // myfree(y); // x is already freed while creating x1
  //
  // char *z = (char *)mymalloc(sizeof(char));
  // void *z1 = myrealloc(z, 0);
  // printf("size is 0 so it returns null that is free the block:%s\n",
  //        (z1 == NULL) ? "yes" : "no");




  // calloc
  //  int *arr = (int *)mycalloc(10, sizeof(int));
  //  int zerocheck = 1;
  //  for (int i = 0; i < 10; i++) {
  //    if (arr[i] != 0)
  //      zerocheck = 0;
  //  }
  //  printf("mycalloc has intialized all bytes to 0: %s\n",
  //         zerocheck == 1 ? "yes" : "no");
  //
  //  void *overflow = (void *)mycalloc(SIZE_MAX, 2);
  //  printf("calloc cannot asign larger size than max size of size_t:%s\n",
  //         overflow == NULL ? "yes" : "no");




  // bins
  //  int *a = (int *)mymalloc(8);
  // myfree(a);
  // int *b = (int *)mymalloc(8);
  // printf("reuse same bin a==b:%s\n", a == b ? "yes" : "no");
  //
  // char *x = (char *)mymalloc(16);
  // char *y = (char *)mymalloc(16);
  // myfree(x);
  // myfree(y);
  // char *z = (char *)mymalloc(32);
  // printf("coalesce x and y though addresses are same x==z:%s\n",
  //        (void *)x == (void *)z ? "yes" : "no");
  //
  // char *large = (char *)mymalloc(64);
  // myfree(large);
  // char *sm1 = (char *)mymalloc(8);
  // char *sm2 = (char *)mymalloc(8);
  // block_header_t *h1 = (block_header_t *)sm1 - 1;
  // block_header_t *h2 = (block_header_t *)sm2 - 1;
  // printf("small1 block size: %zu\n", h1->size);
  // printf("small2 block size: %zu\n", h2->size);
  // printf("actual gap: %ld\n", (char *)sm2 - (char *)sm1);
  // printf("expected gap: %zu\n", 8 + sizeof(block_header_t));
  // printf("split large with sm1 and sm2 i.e sm1==large:%s\n",
  //        (void *)large == (void *)sm1 ? "yes" : "no");
  // printf("sm2->data is sm1 + sizeof(header) i.e
  // sm2-sm1=8+sizeof(header):%s\n",
  //        (char *)sm2 - (char *)sm1 == 8 + sizeof(block_header_t) ? "yes"
  //                                                                : "no");
  //
  // void *n = mymalloc(0); // null on 0 size test
  // printf("when size is 0 we get NULL:%s\n", n == NULL ? "yes" : "no");





  // coalesce
  // char *a = (char *)mymalloc(10);
  // char *b = (char *)mymalloc(10);
  //
  // myfree(a);
  // myfree(b);
  //
  // char *d = (char *)mymalloc(20);
  // printf("address of a:%p\n", (void *)a);
  // printf("address of d:%p\n", (void *)d);
  // printf("both are same though coalescing works:%s\n",
  //        (void *)a == (void *)d ? "Yes" : "no");




  // splitting
  //  char* a=(char*)mymalloc(100);
  //  myfree(a);
  //
  //  char* b=(char*)mymalloc(10);
  //  char* c=(char*)mymalloc(10);
  //
  //  printf("a at %p\n",(void*)a);
  //  printf("b at %p\n",(void*)b);
  //  printf("c at %p\n",(void*)c);
  //
  //  printf("gap between b and c:%ld\n",(char*)c-(char*)b);




  // find free block
  // int* a=(int*)mymalloc(sizeof(int));
  // *a=100;
  // printf("a is at : %p\n",(void*)a);
  // myfree(a);
  //
  // int* b=(int*)mymalloc(sizeof(int));
  // *b=200;
  // printf("b is at : %p\n",(void*)b);
  //
  // int* c = (int*)mymalloc(sizeof(int));
  // *c=400;
  // printf("c is at : %p\n",(void*)c);
  //
  // printf("a and b share same address:%s\n",a==b?"yes":"no");
  // printf("b and c are on same address:%s\n",b==c?"yes":"no");
  // printf("gap between b and c:%ld\n",(char*)c-(char*)b);
  //
  
  
  return 0;
}
