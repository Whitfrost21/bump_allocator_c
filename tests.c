#include "allocator.h"
#include<stdio.h>
#include<pthread.h>
#include<string.h>

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






//test cases for mmap with mutlithreading
// 
// paste this into your main() replacing the existing thread test
// compile with: gcc -pthread -o test reqspace.c && ./test

#define NUM_THREADS 8
#define LARGE_SIZE  (131072 + 1)   // just over LARGE_ALLOC threshold -> mmap path
#define SMALL_SIZE  64             // goes through bin/heap path

// ── Test 1: concurrent large (mmap) allocs and frees ────────────────────────
void *test_concurrent_large(void *arg) {
    int id = *(int *)arg;
    void *ptrs[10];

    for (int round = 0; round < 50; round++) {
        // alloc 10 large blocks
        for (int i = 0; i < 10; i++) {
            ptrs[i] = mymalloc(LARGE_SIZE);
            if (!ptrs[i]) { printf("[T%d] FAIL: large mymalloc returned NULL\n", id); return NULL; }
            memset(ptrs[i], id, LARGE_SIZE);   // write full block
        }
        // verify then free
        for (int i = 0; i < 10; i++) {
            unsigned char *p = ptrs[i];
            for (int b = 0; b < 16; b++) {    // spot-check first 16 bytes
                if (p[b] != (unsigned char)id) {
                    printf("[T%d] FAIL: large block corrupted at round %d\n", id, round);
                    return NULL;
                }
            }
            myfree(ptrs[i]);
        }
    }
    printf("[T%d] concurrent_large  PASS\n", id);
    return NULL;
}

// ── Test 2: interleaved small + large allocs ─────────────────────────────────
void *test_mixed(void *arg) {
    int id = *(int *)arg;
    void *small_ptrs[20];
    void *large_ptrs[5];

    for (int round = 0; round < 30; round++) {
        // alternate: alloc small, alloc large, free large, free small
        for (int i = 0; i < 20; i++) {
            small_ptrs[i] = mymalloc(SMALL_SIZE);
            if (!small_ptrs[i]) { printf("[T%d] FAIL: small alloc NULL\n", id); return NULL; }
            ((char *)small_ptrs[i])[0] = (char)id;
        }
        for (int i = 0; i < 5; i++) {
            large_ptrs[i] = mymalloc(LARGE_SIZE);
            if (!large_ptrs[i]) { printf("[T%d] FAIL: large alloc NULL\n", id); return NULL; }
            ((char *)large_ptrs[i])[0] = (char)(id + 100);
        }
        for (int i = 0; i < 5; i++) {
            if (((char *)large_ptrs[i])[0] != (char)(id + 100)) {
                printf("[T%d] FAIL: large block corrupted in mixed test\n", id);
                return NULL;
            }
            myfree(large_ptrs[i]);
        }
        for (int i = 0; i < 20; i++) {
            if (((char *)small_ptrs[i])[0] != (char)id) {
                printf("[T%d] FAIL: small block corrupted in mixed test\n", id);
                return NULL;
            }
            myfree(small_ptrs[i]);
        }
    }
    printf("[T%d] mixed             PASS\n", id);
    return NULL;
}

// ── Test 3: large realloc under concurrency ──────────────────────────────────
void *test_large_realloc(void *arg) {
    int id = *(int *)arg;

    for (int round = 0; round < 20; round++) {
        // start small, grow past LARGE_ALLOC threshold
        char *p = mymalloc(1024);
        if (!p) { printf("[T%d] FAIL: initial alloc NULL\n", id); return NULL; }
        memset(p, id, 1024);

        // realloc to large (crosses mmap boundary)
        char *p2 = myrealloc(p, LARGE_SIZE);
        if (!p2) { printf("[T%d] FAIL: realloc to large NULL\n", id); return NULL; }

        // first 1024 bytes must still be id
        for (int b = 0; b < 1024; b++) {
            if ((unsigned char)p2[b] != (unsigned char)id) {
                printf("[T%d] FAIL: data lost after realloc at byte %d (got %d want %d)\n",
                       id, b, (unsigned char)p2[b], (unsigned char)id);
                myfree(p2);
                return NULL;
            }
        }

        // realloc back down (large -> small)
        char *p3 = myrealloc(p2, SMALL_SIZE);
        if (!p3) { printf("[T%d] FAIL: realloc to small NULL\n", id); return NULL; }
        myfree(p3);
    }
    printf("[T%d] large_realloc     PASS\n", id);
    return NULL;
}

// ── Test 4: double-free guard (single-threaded sanity) ───────────────────────
// uncomment only when testing manually – double-free is UB
// void test_double_free() {
//     void *p = mymalloc(LARGE_SIZE);
//     myfree(p);
//     myfree(p);  // should not crash if you add a guard
// }

// ── Test 5: free(NULL) is always safe ────────────────────────────────────────
void test_null_free() {
    myfree(NULL);       // must not crash
    myfree(NULL);
    printf("[--] null_free          PASS\n");
}

// ── runner ───────────────────────────────────────────────────────────────────
int main() {
    allocator_init();

    test_null_free();

    pthread_t threads[NUM_THREADS];
    int       ids[NUM_THREADS];
    void *(*funcs[3])(void *) = {
        test_concurrent_large,
        test_mixed,
        test_large_realloc,
    };
    const char *names[3] = { "concurrent_large", "mixed", "large_realloc" };

    for (int t = 0; t < 3; t++) {
        printf("\n-- running %s with %d threads --\n", names[t], NUM_THREADS);
        for (int i = 0; i < NUM_THREADS; i++) {
            ids[i] = i;
            pthread_create(&threads[i], NULL, funcs[t], &ids[i]);
        }
        for (int i = 0; i < NUM_THREADS; i++)
            pthread_join(threads[i], NULL);
    }

    printf("\nall tests done\n");
    return 0;
}



// int main() {
  // all the code here in main is to test the functions of malloc in different
  // test cases
  // printf("header size=%zu\n",sizeof(block_header_t)); //size of my
  // block_header is 48 here
  // allocator_init();


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
  // printf("x cannot use y's block cause it's not free so x1 allocates new block "
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
  
  
//   return 0;
// }
