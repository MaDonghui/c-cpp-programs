#ifndef _COMMON_H
#define _COMMON_H

#ifndef ALLOC_TEST_FRAMEWORK
# error This may only be used by the internal test framework
#endif

#include <stdio.h>

extern int verbose;
extern int use_calloc;
extern int use_system_alloc;
extern size_t max_brk_size;

extern char *heap, *cur_brk;
extern unsigned long num_brk_increase_calls, num_brk_decrease_calls;

#define PGSIZE 4096

#define IS_ALIGNED(x, a) (((x) & ((typeof(x))(a) - 1)) == 0)
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((typeof(x))(a) - 1))

#define unreachable() error("ERROR: unreachable %s:%d", __FILE__, __LINE__)

#define error(fmt, ...) \
do { \
    fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
    exit(1); \
} while (0)

#define assert(cond, fmt, ...) \
do { \
    if (!(cond)) \
        error("Assertion \"%s\" at %s:%d failed: " fmt, #cond, __FILE__, \
              __LINE__, ##__VA_ARGS__); \
} while (0)

#define pr_debug(fmt, ...) \
do { \
    if (verbose) \
        printf("[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
} while (0)

void *orig_mmap(void *addr, size_t length, int prot, int flags, int fd,
                off_t offset);
int orig_munmap(void *addr, size_t length);
int orig_mprotect(void *addr, size_t len, int prot);

#endif
