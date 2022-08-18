#ifndef KVSTORE_H
#define KVSTORE_H

#include "common.h"
#include "hash.h"

#define HT_CAPACITY 256

struct user_item {
    // Add your fields here.
    // You can access this structure from ht_item's user field defined in hash.h
    pthread_rwlock_t rwlock;
};

struct user_ht {
    // Add your fields here.
    // You can access this structure from the hashtable_t's user field define in has.h
    pthread_mutex_t bucket_locks[HT_CAPACITY];
};


typedef struct job {
    struct conn_info *connection;
    struct job *next;
} job_t;

typedef struct job_queue {
    job_t *front;
    job_t *rear;
} job_queue_t;

#endif
