#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>


// increasing heap size page by page
#define PAGE_SIZE 4096
#define ALIGNMENT (sizeof(long))
#define FLAG_BIT 63

typedef struct meta_t {
    // first bit used as free mark, 1 means Free, 0 means inuse
    // rest as the offset of ALIGNMENT bytes to next block
    size_t descriptor;
} meta_t;

void check_meta(meta_t *meta);

void dump_heap();

bool check_free(meta_t *meta);

void set_descriptor(meta_t *meta, size_t total_size);

void set_free(meta_t *meta);

void set_inuse(meta_t *meta);

meta_t *get_next_meta(meta_t *meta);

size_t get_data_size(meta_t *meta);

size_t align(size_t size);

meta_t *get_free_block(size_t aligned_size);

meta_t *expand_heap(size_t aligned_size);

meta_t *split_block(meta_t *left, size_t aligned_size);

// beginning of heap chain
static meta_t *base_block = NULL;

// recently freed for locality
//static meta_t *recent_freed[5] = {NULL, NULL, NULL, NULL, NULL};
// every 10 free triggers a complete free
#define FULL_CYCLE 10
//static unsigned int free_cycle = 0;
// location of last known, most front free block, handled by SPLIT and Free by comparing location
static meta_t *first_free = NULL;
static meta_t *recent_freed[5] = {NULL, NULL, NULL, NULL, NULL};

// real stuff
void *mymalloc(size_t size) {
    if (size == 0) return NULL;

    size_t aligned_size = align(size);

    // get from existing free block
    meta_t *result = get_free_block(aligned_size);
    set_inuse(result);

    return result + 1;
}

void *mycalloc(size_t nmemb, size_t size) {
    meta_t *result = mymalloc(nmemb * size);

    if (nmemb != 0 && size != 0) {
        memset(result, 0, nmemb * size);
    }

    return result;
}

void myfree(void *ptr) {
    if (ptr == NULL) return;    // is this a joke?
    if (base_block == NULL) perror("No memory has been allocated yet\n");
    if (!(uintptr_t) ptr % ALIGNMENT != 0) perror("Bad alignment, not a possible block\n");

    // going through our sad one way list see if I can find it
    meta_t *curr = base_block;

    //printf("-----------------------------------------------\np %p\n", ptr - 8);
    // free target
    while (curr != sbrk(0)) {
        // meta_t *next = get_next_meta(curr);

        // hit
        if (curr + 1 == ptr && !check_free(curr)) {
            //printf("hit\n");
            set_free(curr);

//            // unmap last none base block greater then 512bytes
//            if (next == sbrk(0)) {
////                if (check_free(curr) && get_data_size(curr) >= 512) {
////                    brk(sbrk(0) - get_data_size(curr) - sizeof(meta_t));
////                }
//                break;
//            }
//
//            if (check_free(get_next_meta(curr))) {
//                size_t offset = get_data_size(get_next_meta(curr)) + sizeof(meta_t);
//                set_descriptor(curr, (get_data_size(curr) + sizeof(meta_t) + offset));
//            }

            // update left most
            if (first_free == NULL || curr < first_free) first_free = curr;
            // add to recent freed
            // shift all free, oldest one gets ditched
            for (int i = 4; i > 0; --i) {
                recent_freed[i] = recent_freed[i - 1];
            }
            recent_freed[0] = curr;

            break;
        }

        // no hit
        curr = get_next_meta(curr);
    }

    if (curr == sbrk(0)) perror("can find the block\n");

//    // full cycle
//    if (free_cycle % 5 == 0 && free_cycle != 0) {
//        // merge any two neighbours
//
//        // reset free_cycle
//        free_cycle = 0;
//    }
}

void *myrealloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return mymalloc(size);
    }

    if (size == 0) {
        myfree(ptr);
        return NULL;
    }

    if (base_block == NULL) {
        perror("no memory has yet been allocated\n");
        return NULL;
    }

    meta_t *meta = ptr - sizeof(meta_t);
    size_t aligned_size = align(size);

    // bigger
    if (aligned_size > get_data_size(meta)) {
        void *new_ptr = mymalloc(aligned_size);
        if (new_ptr == NULL) return NULL;

        memcpy(new_ptr, ptr, get_data_size(meta));
        myfree(ptr);
        return new_ptr;
    }
    // smaller but can not split
    if (get_data_size(meta) - aligned_size > sizeof(meta_t) + ALIGNMENT) {
        split_block(meta, aligned_size);
    }


    return meta + 1;
}

//// Memory Management
// align the allocated data blocks in units of ALIGNMENT bytes
size_t align(size_t size) {
    if (size == 0) return ALIGNMENT;
    if (size % ALIGNMENT != 0) return size + ALIGNMENT - size % ALIGNMENT;
    return size;
}

// return a first fit free meta block pointer
// if no available ones, create one then return it
meta_t *get_free_block(size_t aligned_size) {
    meta_t *result = NULL;

    if (base_block == NULL) goto heap_expand;       // uninitialised

    // check recent 5
    for (int i = 0; i < 5; ++i) {
        if(recent_freed[i] == NULL) continue;

        if (get_data_size(recent_freed[i]) >= aligned_size) {
            result = recent_freed[i];
            recent_freed[i] = NULL;
            goto block_split;
        }
    }

    // loop thorough first fit, starting with first available block
    result = first_free;
    while (true) {
        // reached the end of heap
        if (result == sbrk(0)) goto heap_expand;

        // in-use
        if (!check_free(result)) {
            //printf("\t-----jumping to %p-----\n", get_next_meta(result));
            result = get_next_meta(result);
            continue;
        }

        // matched
        if (get_data_size(result) >= aligned_size) goto block_split;

        // no match this round, move to next block
        result = get_next_meta(result);
    }


    // no match, create new page
heap_expand:
    result = expand_heap(aligned_size);
    if (base_block == NULL) base_block = result;    // initialisation
    // at this point, result is available

block_split:
    // matched / created, split if more than a minimal block is wasted
    if ((get_data_size(result) - aligned_size) > (sizeof(meta_t) + ALIGNMENT)) {
        result = split_block(result, aligned_size);
    }

    return result;
}

// only create when reach the sbrk(0)
meta_t *expand_heap(size_t aligned_size) {
    meta_t *result;
    // alloc pages
    size_t page_request = (aligned_size + sizeof(meta_t)) / PAGE_SIZE + 1;
    result = sbrk(page_request * PAGE_SIZE);
    // create block
    set_descriptor(result, page_request * PAGE_SIZE);
    set_free(result);

    // see if is the front_most_free
    first_free = result;

    return result;
}

meta_t *split_block(meta_t *left, size_t aligned_size) {
    size_t total_size = get_data_size(left);
    // adjust left size
    set_descriptor(left, aligned_size + sizeof(meta_t));
    set_free(left);

    // create block on right, add to known_most recently free
    meta_t *right = get_next_meta(left);
    set_descriptor(right, total_size - aligned_size);
    set_free(right);

    // see if is the front_most_free
    first_free = right;

    return left;
}


//// Descriptor management
// return 1 when block is free, return 0 when in-use
bool check_free(meta_t *meta) {
    return meta->descriptor & (1UL << FLAG_BIT);
}

// create a new block descriptor with free flag
void set_descriptor(meta_t *meta, size_t total_size) {
    meta->descriptor = total_size / ALIGNMENT;
    set_free(meta);
}

// set meta flag bit to 1 : free
void set_free(meta_t *meta) {
    meta->descriptor |= (1UL << FLAG_BIT);
}

// set meta flag bit to 0 : in-use
void set_inuse(meta_t *meta) {
    meta->descriptor &= ~(1UL << FLAG_BIT);
}

// return the next block's meta pointer
meta_t *get_next_meta(meta_t *meta) {
    size_t offset = (meta->descriptor & ~(1UL << FLAG_BIT));
    return meta + offset;
};

// return the size of data section
size_t get_data_size(meta_t *meta) {
    size_t offset = (meta->descriptor & ~(1UL << FLAG_BIT));
    if (offset == 8) {
        return 0;
    } else {
        return offset * 8 - 8;
    }
}

// debug tool
void check_meta(meta_t *meta) {
    printf("\t%i\tptr:%p -> %p\tdescriptor:%zu\ttotal size:%zu\tdata size:%zu\n",
           check_free(meta),
           meta, get_next_meta(meta),
           meta->descriptor, (meta->descriptor & ~(1UL << FLAG_BIT)) * 8, get_data_size(meta));
    fflush(stdout);
}

void dump_heap() {
    meta_t *meta = base_block;
    while (get_next_meta(meta) != sbrk(0)) {
        printf("[%p - %p , size:%zu]\n", meta, get_next_meta(meta), get_data_size(meta) + sizeof(meta_t));
        meta = get_next_meta(meta);
    }
}

/*
 * Enable the code below to enable system allocator support for your allocator.
 * Doing so will make debugging much harder (e.g., using printf may result in
 * infinite loops).
 */
#if 1
void *malloc(size_t size) { return mymalloc(size); }
void *calloc(size_t nmemb, size_t size) { return mycalloc(nmemb, size); }
void *realloc(void *ptr, size_t size) { return myrealloc(ptr, size); }
void free(void *ptr) { myfree(ptr); }
#endif



// experimental

#define LEFT_CHILD(index) ((index) * 2 + 1)
#define RIGHT_CHILD(index) ((index) * 2 + 2)
#define PARENT(index) ( ((index) + 1) / 2 - 1)

// using binary buddy system
//typedef struct buddy_t {
//    unsigned size;
//    unsigned binary_tree_array[1];
//} buddy_t;
//
//buddy_t *init_buddy(size_t size){
//    // initialise the buddy tree size. in it's max
//}