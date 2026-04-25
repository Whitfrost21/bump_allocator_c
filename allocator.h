#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include<stddef.h>

void *mymalloc(size_t size);
void myfree(void* ptr);
void *mycalloc(size_t n,size_t size);
void *myrealloc(void* blk,size_t size);

#endif // ! ALLOCATOR_H

