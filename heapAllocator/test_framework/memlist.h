#ifndef _MEMLIST_H
#define _MEMLIST_H

#ifndef ALLOC_TEST_FRAMEWORK
# error This may only be used by the internal test framework
#endif

#include <stdlib.h>
#include <stdint.h>

/* Describes a single contiguous region of memory at [start,end). */
struct mem_region {
    uintptr_t start, end;
    unsigned long data;
    struct mem_region *next, *prev;
};

struct mem_region_list {
    struct mem_region *head, *tail;
};

extern struct mem_region_list vmas;
extern struct mem_region_list allocs;
extern struct mem_region_list freed_allocs;
extern struct mem_region_list zero_sized_allocs;

void memlist_free_region_object(struct mem_region *obj);
int regions_overlap(struct mem_region *region1, struct mem_region *region2);
void memlist_insert_front(struct mem_region_list *list,
                               struct mem_region *new);
void memlist_insert_after(struct mem_region_list *list,
                          struct mem_region *after, struct mem_region *new);
void memlist_remove(struct mem_region_list *list, struct mem_region *obj);
struct mem_region *memlist_find_overlap(struct mem_region_list *list,
                                        struct mem_region *region);
struct mem_region *memlist_find_overlap_unsorted(struct mem_region_list *list,
                                                 struct mem_region *region);
struct mem_region *memlist_add_region(struct mem_region_list *list,
                                      uintptr_t start, size_t len);
size_t memlist_length(struct mem_region_list *list);
size_t memlist_byte_size(struct mem_region_list *list);
void memlist_dump(struct mem_region_list *list, char *str);
struct mem_region *memlist_find(struct mem_region_list *list, uintptr_t ptr);
void region_check_data(struct mem_region *region);
void memlist_check_data(struct mem_region_list *list);

#endif
