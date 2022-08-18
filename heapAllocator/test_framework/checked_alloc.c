/*
 *  Checked allocator functions: Wrappers around the allocator that update
 *  metadata and perform sanity checks.
 *
 * Authors:
 *  Koen Koning <koen.koning@vu.nl>
 */

#include <stdint.h>
#include <string.h>

#include "common.h"
#include "memlist.h"

#include "../alloc.h"

enum allocator {
    ALLOC_DEFAULT,
    ALLOC_MYMALLOC,
    ALLOC_MYCALLOC,
    ALLOC_MYREALLOC,
    ALLOC_MYFREE,
    ALLOC_SYSMALLOC,
    ALLOC_SYSCALLOC,
    ALLOC_SYSREALLOC,
    ALLOC_SYSFREE,
};

int checked_alloc_disable_integrity_check = 0;

/* Generate unique data per allocation for corruption checks. */
static inline unsigned char get_data(void)
{
    static unsigned char data_cnt = 0;
    data_cnt++;
    if (data_cnt == 0 || data_cnt == 0xff)
        data_cnt = 1;
    return data_cnt;
}

/*
 * Allocates a buffer via the student's allocator, sanity checking the returned
 * memory region (e.g., falls in memory area previously allocated from kernel,
 * does not overlap with any other existing buffer).
 */
void *_checked_alloc(size_t nmemb, size_t size, enum allocator allocator)
{
    struct mem_region *alloc, *zero_sized_alloc;
    char *buf;
    uintptr_t ptr;
    size_t alloc_size = nmemb * size;

    if (allocator == ALLOC_DEFAULT) {
        if (use_system_alloc)
            allocator = use_calloc ? ALLOC_SYSCALLOC : ALLOC_SYSMALLOC;
        else
            allocator = use_calloc ? ALLOC_MYCALLOC : ALLOC_MYMALLOC;
    }

    switch (allocator) {
    case ALLOC_MYMALLOC:   buf = mymalloc(alloc_size); break;
    case ALLOC_MYCALLOC:   buf = mycalloc(nmemb, size); break;
    case ALLOC_MYREALLOC:  buf = myrealloc(NULL, alloc_size); break;
    case ALLOC_SYSMALLOC:  buf = malloc(alloc_size); break;
    case ALLOC_SYSCALLOC:  buf = calloc(nmemb, size); break;
    case ALLOC_SYSREALLOC: buf = realloc(NULL, alloc_size); break;
    default: error("Invalid allocator %d\n", allocator);
    }

    ptr = (uintptr_t)buf;

    if (nmemb == 0 || size == 0) {
        /* NULL or "unique pointer that can be passed to free" */
        alloc = memlist_find(&allocs, ptr);
        assert(!alloc, "malloc(0) returned ptr %p to existing object %#lx-%#lx",
               buf, alloc->start, alloc->end);
        memlist_check_data(&allocs);
        if (buf) {
            /* The returned pointer cannot be to an existing object, but
             * otherwise it can be anything (e.g., inside or outside brk, and
             * even non-canonical). To prevent an overflow in memlist we avoid
             * (void*)-1 pointers. */
            if (ptr == (uintptr_t)-1) ptr--;
            alloc = memlist_find(&zero_sized_allocs, ptr);
            if (alloc)
                alloc->data++;
            else {
                alloc = memlist_add_region(&zero_sized_allocs, ptr, 1);
                alloc->data = 1;
            }
        }

        return buf;
    }

    assert(buf, "Allocation of size %zu failed", alloc_size);
    pr_debug("mymalloc: Allocated %p\n", buf);

    if (allocator == ALLOC_MYCALLOC || allocator == ALLOC_SYSCALLOC) {
        size_t i;
        for (i = 0; i < alloc_size; i++)
            assert(buf[i] == 0, "Calloc did not clear %p", &buf[i]);
    }

    assert(IS_ALIGNED(ptr, sizeof(long)), "buffer not aligned to %zu bytes: "
           "%#lx", sizeof(void*), ptr);

    assert(buf >= heap, "Buffer %p not inside heap %p-%p\n", buf, heap,
           cur_brk);
    assert(buf + alloc_size <= cur_brk, "Buffer %p-%p not inside heap %p-%p\n",
           buf, buf + alloc_size, heap, cur_brk);

    alloc = memlist_add_region(&allocs, ptr, alloc_size);

    zero_sized_alloc = memlist_find_overlap(&zero_sized_allocs, alloc);
    assert(!zero_sized_alloc, "New allocation %p-%p overlaps with a pointer "
           "returned by a zero-sized allocation (%#lx, refcount=%lu)\n", buf,
           buf + alloc_size, zero_sized_alloc->start, zero_sized_alloc->data);

    alloc->data = get_data();
    memset(buf, alloc->data, alloc_size);
    if (!checked_alloc_disable_integrity_check)
        memlist_check_data(&allocs);
    return buf;
}

void *checked_alloc_array(size_t nmemb, size_t size)
{
    pr_debug("Allocating %zu * %zu = %zu bytes\n", nmemb, size, nmemb * size);
    return _checked_alloc(nmemb, size, ALLOC_DEFAULT);
}

void *checked_alloc(size_t size)
{
    size_t nmemb = 1;
    pr_debug("Allocating %zu bytes\n", size);
    if (size && (size & 1) == 0) {
        size = size / 2;
        nmemb = 2;
    }
    return _checked_alloc(nmemb, size, ALLOC_DEFAULT);
}

void _checked_free(void *ptr, enum allocator allocator)
{
    struct mem_region *alloc;

    if (allocator == ALLOC_DEFAULT)
        allocator = use_system_alloc ? ALLOC_SYSFREE : ALLOC_MYFREE;

    if (!ptr) {
        /* We don't keep any administration for returned NULL pointers. */
        switch (allocator) {
        case ALLOC_SYSFREE: free(ptr); break;
        case ALLOC_MYFREE: myfree(ptr); break;
        case ALLOC_SYSREALLOC: assert(!realloc(ptr, 0), ""); break;
        case ALLOC_MYREALLOC: myrealloc(ptr, 0); break;
        default: error("Invalid allocator %d\n", allocator);
        }
        return;
    }

    if (ptr == (void*)-1) ptr--; /* For zero-sized allocs. */
    alloc = memlist_find(&zero_sized_allocs, (uintptr_t)ptr);
    if (alloc) {
        alloc->data--;
        if (alloc->data == 0) {
            memlist_remove(&zero_sized_allocs, alloc);
            memlist_free_region_object(alloc);
        }
    } else {
        alloc = memlist_find(&allocs, (uintptr_t)ptr);
        assert(alloc, "Free of ptr %p does not have allocation?", ptr);
        assert((uintptr_t)ptr == alloc->start, "Free of %p in middle of allocation "
            "%#lx-%#lx", ptr, alloc->start, alloc->end);
        region_check_data(alloc);
        memlist_remove(&allocs, alloc);
        memset(ptr, 0xff, alloc->end - alloc->start);
        memlist_insert_front(&freed_allocs, alloc);
    }

    switch (allocator) {
    case ALLOC_SYSFREE: free(ptr); break;
    case ALLOC_MYFREE: myfree(ptr); break;
    case ALLOC_SYSREALLOC: assert(!realloc(ptr, 0), ""); break;
    case ALLOC_MYREALLOC: myrealloc(ptr, 0); break;
    default: error("Invalid allocator %d\n", allocator);
    }
    memlist_check_data(&allocs);
}

void checked_free(void *ptr)
{
    pr_debug("Free %p\n", ptr);
    _checked_free(ptr, ALLOC_DEFAULT);
}

void *checked_realloc(void *ptr, size_t size)
{
    struct mem_region *alloc;
    char *newptr;
    size_t old_size, check_size;
    size_t i;

    pr_debug("Realloc %p %zu\n", ptr, size);

    if (!ptr)
        return _checked_alloc(1, size, use_system_alloc ? ALLOC_SYSREALLOC :
                                                          ALLOC_MYREALLOC);

    if (!size) {
        _checked_free(ptr, use_system_alloc ? ALLOC_SYSFREE : ALLOC_MYFREE);
        return NULL;
    }

    alloc = memlist_find(&allocs, (uintptr_t)ptr);
    assert(alloc, "Realloc of ptr %p does not have allocation?", ptr);
    assert((uintptr_t)ptr == alloc->start, "Realloc of %p in middle of "
           "allocation %#lx-%#lx", ptr, alloc->start, alloc->end);
    old_size = alloc->end - alloc->start;
    region_check_data(alloc);

    if (use_system_alloc)
        newptr = realloc(ptr, size);
    else
        newptr = myrealloc(ptr, size);

    check_size = old_size < size ? old_size : size;
    for (i = 0; i < check_size; i++)
        assert((unsigned char)newptr[i] == (unsigned char)alloc->data,
               "Realloc of %p to new address %p did not retain value: found "
               "%#x instead of %#x at byte %zu", ptr, newptr, newptr[i],
               (unsigned char)alloc->data, i);

    memlist_dump(&allocs, "Allocs before");
    memlist_remove(&allocs, alloc);
    memlist_dump(&allocs, "Allocs after");
    alloc = memlist_add_region(&allocs, (uintptr_t)newptr, size);
    alloc->data = get_data();
    memset(newptr, alloc->data, size);
    memlist_check_data(&allocs);

    return newptr;

}
