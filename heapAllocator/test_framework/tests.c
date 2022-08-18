/*
 * Tests: Small functions testing one particular aspect of the allocator.
 *
 * Authors:
 *  Koen Koning <koen.koning@vu.nl>
 */

#include "common.h"
#include "memlist.h"
#include "checked_alloc.h"
#include "tests.h"

void randomized_allocs(size_t num_allocs, size_t max_size, size_t align)
{
    size_t i;
    srand(max_size);
    memlist_check_data(&allocs);
    checked_alloc_disable_integrity_check = 1;
    for (i = 0; i < num_allocs; i++) {
        checked_alloc(ALIGN_UP((size_t)rand() % max_size, align));
    }
    checked_alloc_disable_integrity_check = 0;
    memlist_check_data(&allocs);
}


float overhead_per_alloc(void)
{
    size_t objs = memlist_length(&allocs);
    size_t obj_bytes = memlist_byte_size(&allocs);
    size_t heap_bytes = cur_brk - heap;
    size_t empty_bytes = heap_bytes - obj_bytes;
    return 1. * empty_bytes / objs;
}

static void test_malloc_simple(void)
{
    checked_alloc(1);
    checked_alloc(8);
    checked_alloc(128);
    checked_alloc(4096);
    checked_alloc(4097);
}

static void test_malloc_zero(void)
{
    test_malloc_simple();
    checked_alloc(0);
    checked_alloc(0);
    test_malloc_simple();
}

static void test_malloc_orders(void)
{
    const int max_order = 26; /* up to 32 MB */
    int order;
    for (order = 0; order < max_order; order++)
        checked_alloc(1UL << order);
}

static void test_malloc_random(void)
{
    const size_t max_size = 1024;
    const int num_allocs = 10000;
    randomized_allocs(num_allocs, max_size, 1);
}

static void test_calloc(void)
{
    use_calloc = 1;
    checked_alloc_array(0, 1);
    checked_alloc_array(1, 0);
    checked_alloc_array(1, 1);
    checked_alloc_array(1, 8);
    checked_alloc_array(8, 1);
    checked_alloc_array(128, 127);
    checked_alloc_array(127, 128);
    checked_alloc_array(127, 4096);
}

static void test_free_random(void)
{
    const size_t max_size = 1024;
    const int num_alloc = 1000;
    void **bufs;
    int i;
    srand(0);
    bufs = malloc(sizeof(void*) * num_alloc); /* May be student alloc :) */
    for (i = 0; i < num_alloc; i++) {
        bufs[i] = checked_alloc((size_t)rand() % max_size);
    }
    for (i = 0; i < num_alloc; i++) {
        checked_free(bufs[i]);
    }
    for (i = 0; i < num_alloc; i++) {
        bufs[i] = checked_alloc((size_t)rand() % max_size);
    }
    for (i = 0; i < num_alloc; i++) {
        checked_free(bufs[i]);
    }
}

static void test_free_reuse(void)
{
    const int num_alloc = 500;
    const size_t sz[] = { 64, 96, 128, 4097 };
    const int num_sz = sizeof(sz) / sizeof(sz[0]);
    struct mem_region *allocs_iter;
    int i, j;

    for (i = 0; i < num_alloc; i++) {
        for (j = 0; j < num_sz; j++)
            checked_alloc(sz[j]);
    }

    allocs_iter = allocs.head;
    while (allocs_iter) {
        struct mem_region *cur = allocs_iter;
        allocs_iter = allocs_iter->next;
        checked_free((void *)cur->start);
    }

    for (i = num_sz - 1; i >= 0; i--) {
        struct mem_region *alloc, *old_alloc;
        void *ptr;

        ptr = checked_alloc(sz[i]);
        alloc = memlist_find(&allocs, (uintptr_t)ptr);
        old_alloc = memlist_find_overlap_unsorted(&freed_allocs, alloc);
        assert(old_alloc, "New allocation did not reuse any freed memory "
               "(size=%zu, new=%p)", sz[i], ptr);
    }
}

static void test_free_reuse_split(void)
{
    const size_t bigbuf_size = 4096;
    const int num_alloc = 64; /* This should fit in bigbuf easily */
    void *buf, *bigbuf, *bigbuf_end;
    int i;

    bigbuf = checked_alloc(bigbuf_size);
    bigbuf_end = (char*)bigbuf + bigbuf_size;
    checked_free(bigbuf);

    for (i = 0; i < num_alloc; i++) {
        buf = checked_alloc(8);
        assert(buf >= bigbuf && buf < bigbuf_end, "New 8-byte alloc %p did not "
               "come out of old freed chunk %p-%p", buf, bigbuf, bigbuf_end);
    }
}

static void test_free_reuse_merge(void)
{
    const size_t bigbuf_size = 1024;
    const int num_alloc = 128;
    const int ps = num_alloc / 4;
    struct mem_region *alloc, *old_alloc;
    void *buf1, *buf2, *low, *high, *bigbuf;
    void **bufs;
    int i;

    assert(ps * 4 == num_alloc, "Incorrect num_allocs value %d", num_alloc);

    bufs = malloc(sizeof(void*) * num_alloc); /* May be student alloc :) */

    buf1 = checked_alloc(8);
    for (i = 0; i < num_alloc; i++)
        bufs[i] = checked_alloc(8);
    buf2 = checked_alloc(8);

    low = buf1 < buf2 ? buf1 : buf2;
    high = buf1 > buf2 ? buf1 : buf2;
    for (i = 0; i < num_alloc; i++)
        assert(low < bufs[i] && bufs[i] < high, "Allocation %d at %p not "
               "between first and last allocation %p and %p", i, bufs[i],
               low, high);

    for (i = 0; i < ps; i++)
        checked_free(bufs[i]);
    for (i = ps * 2; i < ps * 3; i++)
        checked_free(bufs[i]);
    for (i = (ps * 2) - 1; i >= ps; i--)
        checked_free(bufs[i]);
    for (i = (ps * 4) - 1; i >= ps * 3; i--)
        checked_free(bufs[i]);

    bigbuf = checked_alloc(bigbuf_size);

    assert(low < bigbuf && bigbuf < high, "Big allocation at %p not in freed "
           "area %p-%p", bigbuf, bufs[0], bufs[num_alloc - 1]);
    alloc = memlist_find(&allocs, (uintptr_t)bigbuf);
    old_alloc = memlist_find_overlap_unsorted(&freed_allocs, alloc);
    assert(old_alloc, "Big alloc %p did not reuse any freed memory", bigbuf);
}

static void test_realloc(void)
{
    const size_t sz[8] = { 1, 2, 8, 64, 96, 128, 4096, 4097 };
    void *buf[8];
    int i, j;

    for (i = 0; i < 8; i++)
        buf[i] = checked_alloc(sz[i]);

    for (j = 0; j < 8; j++)
        for (i = 0; i < 8; i++)
            buf[i] = checked_realloc(buf[i], sz[j]);
}

static void test_realloc_zero(void)
{
    void *buf1, *buf2, *buf3, *buf4, *buf5, *buf6;
    buf1 = checked_alloc(10);
    buf2 = checked_realloc(0, 10);
    buf3 = checked_alloc(10);

    checked_realloc(buf2, 0);
    checked_realloc(buf1, 0);
    checked_realloc(buf3, 0);

    buf4 = checked_alloc(10);
    buf5 = checked_realloc(0, 10);
    buf6 = checked_alloc(10);

    assert(buf4 == buf1 || buf5 == buf1 || buf6 == buf1,
           "No reuse for freed realloc");
    assert(buf4 == buf2 || buf5 == buf2 || buf6 == buf2,
           "No reuse for freed realloc");
    assert(buf4 == buf3 || buf5 == buf3 || buf6 == buf3,
           "No reuse for freed realloc");
}

static void test_realloc_opt(void)
{
    const size_t sz[8] = { 1, 2, 8, 64, 96, 128, 4096, 4097 };
    void *buf[8];
    void *newbuf;
    int i;

    for (i = 0; i < 8; i++)
        buf[i] = checked_alloc(sz[i]);

    for (i = 0; i < 8; i++) {
        newbuf = checked_realloc(buf[i], 128);
        if (sz[i] >= 128)
            assert(newbuf == buf[i], "New size 128 of %p would have fit in "
                    "old allocation size %zu for %p", newbuf, sz[i],
                    buf[i]);
        buf[i] = newbuf;
    }

    for (i = 0; i < 8; i++) {
        newbuf = checked_realloc(buf[i], 16);
        assert(newbuf == buf[i], "New size 16 of %p would have fit in "
                "old allocation size 128 for %p", newbuf, buf[i]);
    }
}

static void test_batch(void)
{
    const size_t allocs_per_size = 32;
    const size_t sz[4] = { 1, 8, 16, 32 };
    size_t i, j;

    for (i = 0; i < allocs_per_size; i++)
        for (j = 0; j < 4; j++)
            checked_alloc(sz[j]);

    assert(num_brk_increase_calls < 8, "Calls to brk not batched, got %lu brk "
           "increases for %zu allocations", num_brk_increase_calls,
           allocs_per_size * 4);
}


static void test_fragmentation16(void)
{
    const size_t max_size = 128;
    const int num_allocs = 10000;
    float overhead;
    randomized_allocs(num_allocs, max_size, 8);
    overhead = overhead_per_alloc();
    assert(overhead != 0, "Overhead per alloc is zero, no metadata?");
    assert(overhead < 17, "Overhead per alloc of %.2f byte too high", overhead);
}

static void test_fragmentation8(void)
{
    const size_t max_size = 128;
    const int num_allocs = 10000;
    float overhead;
    randomized_allocs(num_allocs, max_size, 8);
    overhead = overhead_per_alloc();
    assert(overhead != 0, "Overhead per alloc is zero, no metadata?");
    assert(overhead < 9, "Overhead per alloc of %.2f byte too high", overhead);
}

static void test_locality(void)
{
    const int dealloc_order[5] = { 0, 4, 3, 1, 2 };
    void *gen1[5], *tmp;
    int i;

    checked_alloc(8);
    for (i = 0; i < 5; i++) {
        gen1[i] = checked_alloc(8);
        checked_alloc(8);
    }

    for (i = 0; i < 5; i++)
        checked_free(gen1[dealloc_order[i]]);

    for (i = 0; i < 5; i++) {
        tmp = checked_alloc(8);
        assert(tmp == gen1[dealloc_order[4 - i]], "Expected allocation %d to "
               "go in slot %d (%p), got %p", i, dealloc_order[4 - i],
               gen1[dealloc_order[4 - i]], tmp);
    }
}

static void test_unmap(void)
{
    const int num_allocs = 64;
    const size_t alloc_size = 512;
    char *buf1, *buf2, *low, *high;
    char *ptrs[num_allocs];
    int check_start, check_end;
    int i;

    buf1 = checked_alloc(alloc_size);
    for (i = 0; i < num_allocs; i++)
        ptrs[i] = checked_alloc(alloc_size);
    buf2 = checked_alloc(alloc_size);

    low = buf1 < buf2 ? buf1 : buf2;
    high = buf1 > buf2 ? buf1 : buf2;
    for (i = 0; i < num_allocs; i++)
        assert(low < ptrs[i] && ptrs[i] < high, "Allocation %d at %p not "
               "between first and last allocation %p and %p", i, ptrs[i],
               low, high);

    assert(num_brk_decrease_calls == 0, "Got %lu brk calls that decrease heap "
           "while no object have been freed yet", num_brk_decrease_calls);

    for (i = 0; i < num_allocs; i++)
        checked_free(ptrs[i]);

    assert(num_brk_decrease_calls == 0, "Got %lu brk calls that decrease heap "
           "while the last object has not been freed", num_brk_decrease_calls);

    checked_free(high);
    assert(num_brk_decrease_calls > 0, "Heap size not decreased while all "
           "objects are freed");

    /* Check 8 highest allocs */
    if (ptrs[0] < ptrs[num_allocs - 1]) {
        check_start = num_allocs - 8;
        check_end = num_allocs;
    } else {
        check_start = 0;
        check_end = 8;
    }
    for (i = check_start; i < check_end; i++) {
        assert(cur_brk < ptrs[i], "Allocation %p (#%d/%d) has been freed but "
            "is still part of the allocated heap (brk=%p)", ptrs[i], i,
            num_allocs, cur_brk);
    }
}

static void test_out_of_band_metadata(void)
{
    const int num_allocs = 500;
    const size_t alloc_size = 8;
    char *start, *expected, *tmp;
    float overhead;
    int i;

    start = checked_alloc(alloc_size);
    for (i = 1; i < num_allocs; i++) {
        expected = start + i * alloc_size;
        tmp = checked_alloc(alloc_size);
        assert(tmp == expected, "Allocation %d at %p not at expected location "
               "%p (first allocation at %p)", i, tmp, expected, start);
    }
    overhead = overhead_per_alloc();
    assert(overhead != 0, "Overhead per alloc is zero, no metadata?");
}

static void test_system_malloc(void)
{
    const size_t max_size = 8 * 1024;
    const int num_allocs = 10;

    use_system_alloc = 1;

    use_calloc = 0;
    randomized_allocs(num_allocs, max_size, 1);

    use_calloc = 1;
    randomized_allocs(num_allocs, max_size, 1);

    test_realloc();

    test_free_reuse();
}

static void test_heap_fill(void)
{
    const size_t alloc_size = 8;
    const size_t assumed_block_size = 64; /* Allow 56 byte metadata per obj. */
    const size_t num_allocs = max_brk_size / assumed_block_size;
    size_t i;

    checked_alloc_disable_integrity_check = 1;
    for (i = 0; i < num_allocs; i++)
        checked_alloc(alloc_size);
    checked_alloc_disable_integrity_check = 0;
}


struct test_case tests[] = {
    { "malloc-simple", test_malloc_simple },
    { "malloc-zero", test_malloc_zero },
    { "malloc-orders", test_malloc_orders },
    { "malloc-random", test_malloc_random },
    { "calloc", test_calloc },
    { "free-random", test_free_random },
    { "free-reuse", test_free_reuse },
    { "free-reuse-split", test_free_reuse_split },
    { "free-reuse-merge", test_free_reuse_merge },
    { "realloc", test_realloc },
    { "realloc-zero", test_realloc_zero },
    { "realloc-opt", test_realloc_opt },
    { "batch", test_batch },
    { "fragmentation-16", test_fragmentation16 },
    { "fragmentation-8", test_fragmentation8 },
    { "locality", test_locality },
    { "unmap", test_unmap },
    { "out-of-band-metadata", test_out_of_band_metadata },
    { "system-malloc", test_system_malloc },
    { "heap-fill", test_heap_fill },
    { NULL, NULL },
};

