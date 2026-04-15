#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#define NUM_BINS 8
typedef struct block_header {
  size_t size;
  int isfree;
  struct block_header *next;
  struct block_header *prev;
  struct block_header *bin_next;
  struct block_header *bin_prev;
} block_header_t;

static block_header_t *bins[NUM_BINS];

pthread_mutex_t globallock=PTHREAD_MUTEX_INITIALIZER;
int bin_index(size_t size) {
  if (size <= 8)
    return 0;
  int index = __builtin_clz(size - 1) - 2;
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

block_header_t *reqestspace(block_header_t *last, size_t size) {
  block_header_t *block = sbrk(0);
  void *request = sbrk(sizeof(block_header_t) + size);
  if (request == (void *)-1)
    return NULL;

  block->size = size;
  block->isfree = 0;
  block->prev = last;
  block->next = NULL;
  block->bin_next = NULL;
  block->bin_prev = NULL;
  if (last) {
    last->next = block;
  }
  lastblock = block;
  return block;
}

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

void coalesce(block_header_t *block) {
   // prev is free
  if (block->prev && block->isfree && block->prev->isfree &&
      (char *)(block->prev + 1) + block->prev->size == (char *)block) {
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
      if (block->size >= size)
        return block;
      block = block->bin_next;
    }
    index++;
  }
  return NULL;
}

void *mymalloc(size_t size) {
  if (size == 0)
    return NULL;
  pthread_mutex_lock(&globallock);
  block_header_t *block = find_free_block(size);
  
  if (block) {
    bin_remove(block);//remove block before splitting to avoid bin confusions
    if (block->size >= size + sizeof(block_header_t) + 1) {
      splitblocks(block, size);
    }
    block->isfree = 0;
  } else {
    block = reqestspace(lastblock, size);
    if (!block){pthread_mutex_unlock(&globallock);
      return NULL;}
  }
  pthread_mutex_unlock(&globallock);
  return (void *)(block + 1);
}

void myfree(void *ptr) {
    if (!ptr){ 
    return;
  }
  pthread_mutex_lock(&globallock);

  block_header_t *block = (block_header_t *)ptr - 1;
  block->isfree = 1;
  bin_insert(block);
  coalesce(block);
  pthread_mutex_unlock(&globallock);
}

int main() {
  
  //bins 
  int *a=(int*)mymalloc(8);
  myfree(a);
  int *b=(int*)mymalloc(8);
  printf("reuse same bin a==b:%s\n",a==b?"yes":"no");

  char* x=(char*)mymalloc(16);
  char* y=(char*)mymalloc(16);
  myfree(x);
  myfree(y);
  char* z=(char*)mymalloc(32);
  printf("coalesce x and y though addresses are same x==z:%s\n",(void*)x==(void*)z?"yes":"no");

  char* large=(char*)mymalloc(64);
  myfree(large);
  char* sm1=(char*)mymalloc(8);
  char* sm2=(char*)mymalloc(8);
block_header_t *h1 = (block_header_t *)sm1 - 1;
block_header_t *h2 = (block_header_t *)sm2 - 1;
printf("small1 block size: %zu\n", h1->size);
printf("small2 block size: %zu\n", h2->size);
printf("actual gap: %ld\n", (char *)sm2 - (char *)sm1);
printf("expected gap: %zu\n", 8 + sizeof(block_header_t));
  printf("split large with sm1 and sm2 i.e sm1==large:%s\n",(void*)large==(void*)sm1?"yes":"no");
  printf("sm2->data is sm1 + sizeof(header) i.e sm2-sm1=8+sizeof(header):%s\n",(char*)sm2-(char*)sm1==8+sizeof(block_header_t)?"yes":"no");

void* n=mymalloc(0);//null on 0 size test 
printf("when size is 0 we get NULL:%s\n",n==NULL?"yes":"no");




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
  return 0;
}
