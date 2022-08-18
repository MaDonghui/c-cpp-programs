#ifndef KVSTORE_HASH_H
#define KVSTORE_HASH_H

#include <stdlib.h>
#include <string.h>
#include "kvstore.h"

// Do not change the existing fields.
typedef struct hash_item_t {
    struct hash_item_t *next, *prev;    // Next item in the bucket
    char *key;      // items'key
    char *value;        // items's value
    size_t value_size;  // items's value length

    // You can add your fields into the structure pointer by this variable.
    // `struct user_item` is defined in kvstore.h
    struct user_item *user;
} hash_item_t;

typedef struct {
    unsigned int capacity;  // Number of buckets
    hash_item_t **items;    // Hashtable buckets as single linked list

    // You can add your fields into the structure pointed by this variable.
    // `struct user_ht` is defined in kvstore.h
    struct user_ht *user;
} hashtable_t;

hashtable_t *ht;

unsigned int hash(char *str);

#endif
