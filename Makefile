CC = gcc
CFLAGS = -Wall -Wextra -O2

all: bench_libc bench_cus test

# libc version (no flag)
bench_libc: allocator.c benchmark.c
	$(CC) $(CFLAGS) benchmark.c -o bench_libc

# custom allocator version
bench_cus: allocator.c benchmark.c
	$(CC) $(CFLAGS) -DUSE_CUSTOM allocator.c benchmark.c -o bench_cus

# tests (always use your allocator)
test: allocator.c tests.c
	$(CC) $(CFLAGS) -DUSE_CUSTOM allocator.c tests.c -o test

clean:
	rm -f bench_libc bench_cus test *.o
