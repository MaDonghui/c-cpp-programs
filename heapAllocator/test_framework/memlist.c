/*
 * Memlist: A data structure for keeping track of multiple regions of memory.
 *
 * Authors:
 *  Koen Koning <koen.koning@vu.nl>
 */

#include <sys/mman.h>

#include "common.h"
#include "memlist.h"

/* Areas allocated by the tests (the areas returned by calling `malloc` etc. */
struct mem_region_list allocs;

/* Areas previously allocated by the tests that we freed. */
struct mem_region_list freed_allocs;

/* Non-null pointers returned by malloc(0) and similar. */
struct mem_region_list zero_sized_allocs;

/* Unused regions which can be used for new `allocs` etc entries. */
static struct mem_region_list freelist;

/*
 * Allocates a bunch of new mem_region objects and places them on the freelist.
 */
static void populate_freelist(void)
{
    size_t alloc_size, num_regions;
    struct mem_region *regions;
    unsigned i;

    assert(freelist.head == NULL, "Cannot populate non-empty freelist");

    alloc_size = PGSIZE * 16;
    regions = orig_mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    num_regions = alloc_size / sizeof(struct mem_region);
    for (i = 0; i < num_regions; i++)
        memlist_insert_front(&freelist, &regions[i]);
}

static struct mem_region *alloc_region_object(uintptr_t start, uintptr_t end)
{
    struct mem_region *ret;
    if (!freelist.head)
        populate_freelist();
    assert(freelist.head, "Must have free objects to allocate");
    ret = freelist.head;
    memlist_remove(&freelist, ret);
    ret->start = start;
    ret->end = end;
    return ret;
}

void memlist_free_region_object(struct mem_region *obj)
{
    memlist_insert_front(&freelist, obj);
}

int regions_overlap(struct mem_region *region1, struct mem_region *region2)
{
    /* Regions do *not* overlap if one is entirely before or after the other. */
    return !(region1->end <= region2->start || region1->start >= region2->end);
}

struct mem_region *memlist_find_overlap(struct mem_region_list *list,
                                        struct mem_region *region)
{
    struct mem_region *tmp;

    /* Bail out quickly for sorted lists if the region is outside the list. */
    if (!list->head || region->end <= list->head->start ||
            region->start >= list->tail->end)
        return NULL;

    tmp = list->head;
    while (tmp) {
        if (regions_overlap(tmp, region))
            return region;
        if (tmp->end <= region->start)
            return NULL;
        tmp = tmp->next;
    }
    return NULL;
}

struct mem_region *memlist_find_overlap_unsorted(struct mem_region_list *list,
                                                 struct mem_region *region)
{
    struct mem_region *tmp = list->head;
    while (tmp) {
        if (regions_overlap(tmp, region))
            return region;
        tmp = tmp->next;
    }
    return NULL;
}

/*
 * Add an existing object to the front of a list, without any overlap checks,
 * sorting or merging.
 */
void memlist_insert_front(struct mem_region_list *list,
                               struct mem_region *new)
{
    new->next = list->head;
    if (list->head)
        list->head->prev = new;
    list->head = new;
    if (!list->tail)
        list->tail = new;
}

/*
 * Insert a given object into a list after a specific element. Does not perform
 * merging or overlap checks.
 */
void memlist_insert_after(struct mem_region_list *list,
                          struct mem_region *after, struct mem_region *new)
{
    new->next = after->next;
    new->prev = after;
    after->next = new;
    if (new->next)
        new->next->prev = new;
    else
        list->tail = new;
}

void memlist_remove(struct mem_region_list *list, struct mem_region *obj)
{
    pr_debug("Removing %#lx-%#lx p=%p n=%p\n", obj->start, obj->end, obj->prev, obj->next);
    if (obj->next)
        obj->next->prev = obj->prev;
    if (obj->prev)
        obj->prev->next = obj->next;
    if (list->head == obj)
        list->head = obj->next;
    if (list->tail == obj)
        list->tail = obj->prev;
    obj->next = obj->prev = NULL;
}

/*
 * Add a new range to the given sorted list in the correct position.
 */
struct mem_region *memlist_add_region(struct mem_region_list *list,
                                      uintptr_t start, size_t len)
{
    struct mem_region *region, *new_region;
    uintptr_t end = start + len;
    assert(start < end, "Invalid region start=%#lx end=%#lx len=%#zx", start,
           end, len);

    new_region = alloc_region_object(start, end);

    if (!list->head || end <= list->head->start) {
        /* No regions on list, or we go before first one. */
        memlist_insert_front(list, new_region);
        return new_region;
    } else if (start >= list->tail->end) {
        memlist_insert_after(list, list->tail, new_region);
        return new_region;
    }

    for (region = list->head; region; region = region->next) {
        if (regions_overlap(new_region, region))
            error("New region %#lx-%#lx overlaps with existing region "
                  "%#lx-%#lx\n", start, end, region->start, region->end);

        if (start >= region->end && (!region->next ||
                                     region->next->start >= end)) {
            memlist_insert_after(list, region, new_region);
            return new_region;
        }
    }

    unreachable();
}

/*
 * Number of items on a list.
 */
size_t memlist_length(struct mem_region_list *list)
{
    struct mem_region *region;
    size_t cnt = 0;

    for (region = list->head; region; region = region->next)
        cnt++;
    return cnt;
}

/*
 * Size in bytes of all memory areas on a list.
 */
size_t memlist_byte_size(struct mem_region_list *list)
{
    struct mem_region *region;
    size_t cnt = 0;

    for (region = list->head; region; region = region->next)
        cnt += region->end - region->start;
    return cnt;
}

void memlist_dump(struct mem_region_list *list, char *str)
{
    struct mem_region *region;
    pr_debug("Dumping list %s\n", str);
    for (region = list->head; region; region = region->next)
        pr_debug(" region %#lx-%#lx (size=%#lx) p=%p n=%p\n", region->start,
                 region->end, region->end - region->start, region->prev,
                 region->next);
}

/*
 * Return the memory area containing `ptr`.
 */
struct mem_region *memlist_find(struct mem_region_list *list, uintptr_t ptr)
{
    struct mem_region *region = list->head;
    while (region && ptr >= region->start) {
        if (ptr >= region->start && ptr < region->end)
            return region;
        region = region->next;
    }
    return NULL;
}

void region_check_data(struct mem_region *region)
{
    unsigned char *endptr = (unsigned char *)region->end;
    unsigned char *ptr;
    for (ptr = (unsigned char *)region->start; ptr < endptr; ptr++) {
        assert(*ptr == (unsigned char)region->data, "Contents of allocation at "
               "%#lx-%#lx changed: expected %#x at %p, found %#x",
               region->start, region->end, (unsigned char)region->data, ptr,
               *ptr);
    }
}

/*
 * Checks the integrity of a list by comparing all memory contents to the `data`
 * field of that area. Only works for `allocs` list.
 */
void memlist_check_data(struct mem_region_list *list)
{
    struct mem_region *region;
    for (region = list->head; region; region = region->next)
        region_check_data(region);
}
