/*
 * Libc interceptors: Intercept calls to brk, mmap, etc, made by the student
 * allocator. We implement our own brk implementation here using mmap/mprotect.
 *
 * Authors:
 *  Koen Koning <koen.koning@vu.nl>
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/mman.h>

#include "common.h"
#include "memlist.h"

char *heap = NULL;
char *cur_brk;
unsigned long num_brk_increase_calls = 0;
unsigned long num_brk_decrease_calls = 0;

static void init_heap(void)
{
    assert(!heap, "Heap already initialized: %p\n", heap);
    heap = orig_mmap(NULL, max_brk_size, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    assert(heap != MAP_FAILED, "Could not preallocate heap space: %d\n", errno);
    cur_brk = heap;
}

static void do_brk(char *addr)
{
    uintptr_t pg_cur = ALIGN_UP((uintptr_t)cur_brk, PGSIZE);
    uintptr_t pg_new = ALIGN_UP((uintptr_t)addr, PGSIZE);
    char *tmp;
    assert(addr >= heap, "New brk %p before heap start %p", addr, heap);
    assert(addr <= heap + max_brk_size, "New brk %p beyond max heap size "
           "(max heap size=%zu, max heap=%p)", addr, max_brk_size,
           heap + max_brk_size);
    if (addr == cur_brk)
        return;
    if (addr > cur_brk) {
        num_brk_increase_calls++;

        if (pg_new > pg_cur)
            orig_mprotect((void *)pg_cur, pg_new - pg_cur,
                          PROT_READ | PROT_WRITE);

        /* Poison the contents of the new heap area. This is not exactly what
         * Linux/glibc do (they zero-initialize heap regions), but it is not
         * defined behavior and allows us to better test calloc. */
        for (tmp = cur_brk; tmp < addr; tmp++)
            *tmp = 0xad;

    } else {
        struct mem_region *alloc;
        struct mem_region remove_region = {
            .start = (uintptr_t)addr,
            .end = (uintptr_t)cur_brk
        };
        num_brk_decrease_calls++;
        alloc = memlist_find_overlap(&allocs, &remove_region);
        assert(!alloc, "Area freed by lowering brk from %p to %p still "
               "contains allocation %#lx-%#lx", cur_brk, addr, alloc->start,
               alloc->end);
        if (pg_new < pg_cur)
            orig_mprotect((void *)pg_new, pg_cur - pg_new, PROT_NONE);
    }
    cur_brk = addr;
}

int brk(void *addr)
{
    pr_debug("brk(%p), cur=%p\n", addr, cur_brk);
    if (!heap)
        init_heap();

    do_brk(addr);
    return 0; /* We error and exit in do_brk if something went wrong. */
}

void *sbrk(intptr_t increment)
{
    void *old_brk;
    pr_debug("sbrk(%ld), %p -> %p\n", increment, cur_brk, cur_brk + increment);
    if (!heap)
        init_heap();
    old_brk = cur_brk;
    do_brk(cur_brk + increment);
    return old_brk;
}

/*
 * Intercepts mmap. The nice thing about glibc is that it uses __mmap
 * internally, so we do not catch all the internal mmap calls, and don't have to
 * filter out the application specific ones.
 */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)addr, (void)length, (void)prot, (void)flags, (void)fd, (void)offset;
    error("Using mmap is not allowed.\n");
    return NULL;
}

int munmap(void *addr, size_t length)
{
    (void)addr, (void)length;
    error("Using munmap is not allowed.\n");
    return -1;
}

int mprotect(void *addr, size_t len, int prot)
{
    (void)addr, (void)len, (void)prot;
    error("Using mprotect is not allowed.\n");
    return -1;
}


/*
 * Calls the real system mmap and munmap, instead of our intercepted versions.
 */

void *orig_mmap(void *addr, size_t length, int prot, int flags, int fd,
                off_t offset)
{
    static void *(*_orig_mmap)(void *, size_t, int, int, int, off_t);
    void *ret;
    if (!_orig_mmap)
        _orig_mmap = dlsym(RTLD_NEXT, "mmap");
    ret = _orig_mmap(addr, length, prot, flags, fd, offset);
    assert(ret != MAP_FAILED, "mmap(%p, %zu, %d, %d, %d, %ld) failed", addr,
           length, prot, flags, fd, offset);
    return ret;
}

int orig_munmap(void *addr, size_t length)
{
    static int (*_orig_munmap)(void *, size_t);
    int ret;
    if (!_orig_munmap)
        _orig_munmap = dlsym(RTLD_NEXT, "munmap");
    ret = _orig_munmap(addr, length);
    assert(!ret, "munmap(%p, %zu) failed (%d)", addr, length, ret);
    return ret;
}

int orig_mprotect(void *addr, size_t len, int prot)
{
    static int (*_orig_mprotect)(void *, size_t, int);
    int ret;
    if (!_orig_mprotect)
        _orig_mprotect = dlsym(RTLD_NEXT, "mprotect");
    ret = _orig_mprotect(addr, len, prot);
    assert(!ret, "mprotect(%p, %zu, %d) failed (%d)", addr, len, prot, ret);
    return ret;
}
