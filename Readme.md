# Custom Memory Allocator in C

A from-scratch implementation of `malloc`, `free`, `calloc`, and `realloc` in C, built without using any heap allocation functions from libc. Implements core allocator features including thread safety, per-thread caching, and large allocation handling via `mmap`.

---

## Features

| Feature | Description |
|---|---|
| Doubly linked heap list | Tracks all allocated/free blocks with `prev`/`next` pointers |
| Power-of-2 bins (8 bins) | Free blocks organized by size class for O(1) lookup |
| Block splitting | Large free blocks split to minimize internal fragmentation |
| Block coalescing | Adjacent free blocks merged to reduce external fragmentation |
| 8-byte alignment | All allocations aligned via `ALIGN(size)` macro |
| mmap chunk allocator | Heap grows in 2MB chunks via `mmap` instead of `sbrk` |
| Large allocation path | Allocations ≥ 128KB get dedicated `mmap` regions |
| Large block cache | Up to 8 large `mmap` blocks cached on free to avoid syscall overhead |
| Per-thread cache (tcache) | Each thread holds up to 64 free blocks per bin, zero-lock fast path |
| Thread safety | Global `heaplock` mutex protects all shared heap state |
| Allocator statistics | Tracks tcache hit rate, mmap calls, fragmentation, live allocations |

---

## API (methods which i wrote)

```c
void *mymalloc(size_t size);
void  myfree(void *ptr);
void *mycalloc(size_t n, size_t size);
void *myrealloc(void *ptr, size_t size);
void  allocator_print_stats(void);
```

---

## Architecture

```
mymalloc(size)
    │
    ├─ size < 128KB ──► check per-thread tcache
    │                       │
    │                   hit ─► return block (no lock)
    │                   miss ─► lock heaplock
    │                              │
    │                          find_free_block() ─► split if needed
    │                          reqestspace()     ─► carve from 2MB chunk
    │
    └─ size ≥ 128KB ──► check large_cache[]
                            │
                        hit ─► return cached mmap block (heaplock)
                        miss ─► mmap(size) syscall

myfree(ptr)
    │
    ├─ heap block ──► push to per-thread tcache
    │                    │
    │                count ≥ 64 ─► flush_tcache() ─► global bins + coalesce
    │
    └─ mmap block ──► push to large_cache[] or munmap() if large cache is full
```

### Memory Layout

Each allocation is preceded by a `block_header_t` (48 bytes):

```
┌──────────────────────────────┐
│ block_header_t               │  48 bytes
│  size_t size                 │
│  int isfree                  │
│  int ismmapped               │
│  block_header_t *next/prev   │  heap linked list
│  block_header_t *bin_next    │  bin free list
│  block_header_t *bin_prev    │  bin free list
├──────────────────────────────┤
│ user data                    │  size bytes  ← returned pointer
└──────────────────────────────┘
```

### Bin Layout

8 bins covering size ranges using `31 - __builtin_clz(size-1) - 3`:

```
bin[0]:  1  –   8 bytes
bin[1]:  9  –  16 bytes
bin[2]: 17  –  32 bytes
bin[3]: 33  –  64 bytes
bin[4]: 65  – 128 bytes
bin[5]: 129 – 256 bytes
bin[6]: 257 – 512 bytes
bin[7]: 513+ bytes
```

### Per-thread Cache (tcache)

Each thread gets its own `tcache_t` via `__thread` (TLS):

```c
typedef struct {
    block_header_t *bins[NUM_BINS];  // free list per bin
    int count[NUM_BINS];             // block count per bin
} tcache_t;

__thread tcache_t tcache = {0};
```

Hot path (tcache hit) requires zero locks. Cache flushes to global bins when any bin reaches `TCACHE_MAX = 64` blocks.

---

## Building

```bash
# compile allocator + tests
gcc -pthread -fsanitize=thread allocator.c tests.c -o test

# run correctness tests
./test

# compile benchmarks
gcc -pthread -O2 -DUSE_CUSTOM benchmark.c allocator.c -o bench_custom
gcc -pthread -O2            benchmark.c               -o bench_libc

# run benchmarks
./bench_libc
./bench_custom
```

---

## Benchmark Results

Tested on x86-64 Linux, 8 threads, `-O2`.

```
                        custom      libc      notes
single alloc+free:      ~80ms       ~27ms     3x slower
batch alloc (10k):      ~1.5ms      ~3.8ms    YOU WIN 2.5x
batch free  (10k):      ~0.5ms      ~1.0ms    YOU WIN 2x
mixed sizes (100k):     ~9ms        ~2.5ms    4x slower (was 86ms before large cache)
realloc chain (100k):   ~3ms        ~2.6ms    roughly tied
multithreaded (8T):     ~800ms      ~67ms     12x slower (heaplock contention)
```

tcache hit rate: **~73%** on typical workloads.

### Key observations

- **Batch alloc/free** — tcache dominates, beats libc because blocks stay thread-local
- **Mixed sizes** — large block cache reduced mmap syscalls from 10,000 to ~1, 10x improvement
- **Multithreaded** — single global `heaplock` is the bottleneck; per-thread arenas would close this gap
- **Single alloc+free** — libc wins due to ptmalloc per-arena design

---

## Allocator Stats

Call `allocator_print_stats()` after workload to see:

```
===allocator stats===
tcache hits:        3,800,000
tcache misses:      1,400,000
tcache hit rate:    73.1%
total allocs:       5,200,001
total frees:        5,200,001
live allocations:   0
mmap calls:         1
munmap calls:       0
large cache hits:   9,999
large cache wasted: 0 bytes (0.0 MB)
```

---

## Known Limitations

- **Single global lock** — all threads contend on `heaplock` on cache miss; per-thread arenas would fix this
- **No in-place realloc** — `myrealloc` always alloc+copy because tcache blocks can't be safely distinguished from global heap blocks without an extra flag
- **tcache fragmentation** — no coalescing inside tcache; adjacent free blocks in cache won't merge until flushed
- **Large block splitting** — mmap'd blocks can't be partially munmap'd so oversized cached blocks waste memory (tracked by `large_cache_wasted_bytes`)
- **No thread exit cleanup** — tcache blocks held by a thread are leaked if the thread exits without flushing

---

## What I Learned

- **Ownership is the key to thread safety** — most bugs came from unclear ownership boundaries between tcache, global bins, and heap blocks
- **The fast path creates the hardest races** — every optimization (tcache, lockless large alloc) introduced a new race condition
- **Atomic ops aren't free** — replacing per-operation atomics with thread-local counters + periodic flush recovered significant performance
- **Test harnesses can be wrong too** — `rand()` is not thread-safe; using it in multithreaded tests caused false corruption reports that looked like allocator bugs

---

## Possible Next Steps

- Per-thread arenas to eliminate heaplock contention
- Exact size classes to remove tcache fragmentation
- `in_tcache` flag in block header to re-enable in-place realloc
- Thread exit hook (`pthread_key_create`) to flush tcache on thread death
- Heap introspection — walk all blocks and compute fragmentation percentage
