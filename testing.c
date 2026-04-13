#include<unistd.h>
#include<stddef.h>
#include<stdio.h>

typedef struct block_header{
  size_t size;
  int isfree;
  struct block_header* next;
}block_header_t;

block_header_t *requestspace(size_t size){
  block_header_t *block=sbrk(0);
  void *request=sbrk(sizeof(block_header_t)+size);
  if(request==(void*)-1)return NULL;
  block->size=size;
  block->isfree=0;
  block->next=NULL;

  return block;
}
static block_header_t *freelist=NULL;
block_header_t *findfreespace(size_t size){
  block_header_t *curr=freelist;
  while(curr){
    if(curr->isfree && curr->size>=size){
      return curr;
    }
    curr=curr->next;
  }
  return NULL;
}

void *mymalloc(size_t size){
  if(size==0)return NULL;
  block_header_t *block=findfreespace(size);
  if(block){
    block->isfree=0;
  }else{
    block=requestspace(size);
    if(!block)return NULL;
    if(!freelist)freelist=block;
    else{
    block_header_t *curr=freelist;
    while(curr->next)curr=curr->next;
    curr->next=block;

    }
  }
  return (void*)(block+1);
}
void myfree(void* ptr){
  if(!ptr)return;
  block_header_t *block=(block_header_t*)ptr-1;
  block->isfree=1;
}
int main(){
  int* a=mymalloc(sizeof(int));
  *a=234;
  printf("a at %p\n",(void*)a);
  myfree(a);
  // printf("freed a");
  int* b=mymalloc(sizeof(int));
  // printf("allocated b");
  *b=333;
  printf("b at %p\n",(void*)b);
  char* c=mymalloc(sizeof(char));
  *c='h';
  printf("c at %p\n",(void*)c);
  return 0;
  
}

