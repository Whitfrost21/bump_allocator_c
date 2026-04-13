#include<unistd.h>
#include<stddef.h>
#include<stdio.h>
typedef struct block_header {
  size_t size;
  int isfree;
 struct block_header *next;
  struct block_header *prev;
}block_header_t;
static block_header_t *freelist = NULL;
static block_header_t *lastblock=NULL;
block_header_t *reqestspace(block_header_t* last,size_t size){
  block_header_t* block=sbrk(0);
  void *request = sbrk(sizeof(block_header_t)+size);
  if(request == (void*)-1)return NULL;

  block->size=size;
  block->isfree=0;
  block->prev=last;
  block->next=NULL;
  if(last){
    last->next=block;
  }
  lastblock=block;
   return block;
}

void splitblocks(block_header_t *block,size_t size){
  block_header_t *leftover=(block_header_t*)((char*)(block+1)+size);
  leftover->size=block->size-size-sizeof(block_header_t);
  leftover->isfree=1;
  leftover->next=block->next;
  leftover->prev=block;
  if(block->next){
    block->next->prev=leftover;
  }
  block->next=leftover;
  block->size=size;
  if(block==lastblock) lastblock=leftover;
}

void coalesce(block_header_t *block){
  //next is free
  if(block->next && block->isfree && block->next->isfree && ((char*)(block+1)+block->size==(char*)block->next)){
    if(block->next==lastblock)lastblock=block;
    block->size=block->size+sizeof(block_header_t)+block->next->size;
    block->next=block->next->next;
    if(block->next)block->next->prev=block;
  }
  //prev is free 
  if(block->prev && block->isfree &&block->prev->isfree &&(char*)(block->prev+1)+block->prev->size==(char*)block){
     if(block==lastblock)lastblock=block->prev;
    block->prev->size=block->prev->size + sizeof(block_header_t)+block->size;
    block->prev->next=block->next;
    if(block->next){
      block->next->prev=block->prev;
    }
     }
}

block_header_t *find_free_block(size_t size){
  block_header_t *curr=freelist;
  while(curr){
    // printf("checking block -isfree:%d  size:%zu\n",curr->isfree,curr->size);
    if(curr->isfree &&curr->size>=size){
      return curr;
    }
      curr=curr->next;
  }
  return NULL;
}

void *mymalloc(size_t size){
  if(size==0)return NULL;
  
  block_header_t *block=find_free_block(size);
  if(block){
    if(block->size>=size+sizeof(block_header_t)+1){
      splitblocks(block,size);
    }
   block->isfree=0; 
  }else{
  block= reqestspace(lastblock,size);
  if(!block)return NULL;
  if(!freelist)freelist=block;
  }

  return (void*)(block+1);
}

void myfree(void *ptr){
  if(!ptr)return;
  block_header_t *block=(block_header_t*)ptr-1;
  block->isfree=1;
  coalesce(block);
}

int main(){

  //coalesce 
  char* a= (char*)mymalloc(10);
  char* b=(char*)mymalloc(10);
 

  myfree(a);
  myfree(b);
 
  char* d=(char*)mymalloc(20);
  printf("address of a:%p\n",(void*)a);
  printf("address of d:%p\n",(void*)d);
  printf("both are same though coalescing works:%s\n",(void*)a==(void*)d?"Yes":"no");

  //splitting  
  // char* a=(char*)mymalloc(100);
  // myfree(a);
  //
  // char* b=(char*)mymalloc(10);
  // char* c=(char*)mymalloc(10);
  //
  // printf("a at %p\n",(void*)a);
  // printf("b at %p\n",(void*)b);
  // printf("c at %p\n",(void*)c);
  //
  // printf("gap between b and c:%ld\n",(char*)c-(char*)b);

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
