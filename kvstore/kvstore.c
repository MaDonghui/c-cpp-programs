#include <semaphore.h>
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>

#include "server_utils.h"
#include "common.h"
#include "request_dispatcher.h"
#include "hash.h"
#include "kvstore.h"

// DO NOT MODIFY THIS.
// ./check.py assumes the hashtable has 256 buckets.
// You should initialize your hashtable with this capacity.
#define HT_CAPACITY 256

/*
 * if found key in table, return the item's pointer
 * else return NULL
*/
hash_item_t *get_item(char *key) {
    //printf("finding key: %s\t", key);
    unsigned bucket_index = hash(key) % HT_CAPACITY;

    // if the bucket is not empty
    if (ht->items[bucket_index]) {
        hash_item_t *iterator = ht->items[bucket_index];

        while (iterator && strcmp(iterator->key, key) != 0) {
            iterator = iterator->next;
        }

        return iterator ? iterator : NULL;  // if iterator is not NULL return it, else return NULL
    }

    // bucket empty
    return NULL;
}

hash_item_t *init_hash_item() {
    hash_item_t *res = (hash_item_t *) calloc(1, sizeof(hash_item_t));

    res->prev = NULL;
    res->next = NULL;
    res->key = (char *) calloc(4096, sizeof(char));
    res->value = NULL;
    res->value_size = 0;

    // locks
    res->user = (struct user_item *) calloc(1, sizeof(struct user_item));
    pthread_rwlock_init(&res->user->rwlock, NULL);
    return res;
}

int set_request(int socket, struct request *request) {
    size_t len = 0;
    size_t expected_len = request->msg_len;

    // 1. Lock the hashtable entry. Create it if the key is not in the store.

    // find hash_item, or create new one
    //// lock bucket
    hash_item_t *target = get_item(request->key);
    //// lock target if found
    //// unlock bucket

    char *buf = (char *) calloc(expected_len, sizeof(char));
    while (len < expected_len) {
        size_t received = read_payload(socket, request, expected_len, buf + len);
        len += received;
    }

    // finalise the SET
    if (check_payload(socket, request, expected_len) == 0) {
        // payload OK
        if (target) {
            // key exist
            target->value_size = len;
            target->value = buf;
            //// unlock target
        } else {
            // a new item is required
            //// lock bucket
            hash_item_t *new_head = init_hash_item();
            strcpy(new_head->key, request->key);
            new_head->value = buf;
            new_head->value_size = len;

            // insert new item to bucket
            unsigned bucket_index = hash(request->key) % HT_CAPACITY;
            if (ht->items[bucket_index]) {
                //// lock old head
                hash_item_t *old_head = ht->items[bucket_index];
                old_head->prev = new_head;

                new_head->prev = NULL;
                new_head->next = old_head;

                ht->items[bucket_index] = new_head;
                //// unlock old head
            } else {
                new_head->prev = NULL;
                new_head->next = NULL;
                ht->items[bucket_index] = new_head;
            }
            //// unlock bucket
        }

        send_response(socket, OK, 0, NULL);
    } else {
        // abort

    }

    // Optionally you can close the connection
    // You should do it ONLY on errors:
    // request->connection_close = 1;
    return len;
}

int get_request(int socket, struct request *request) {
    //// lock bucket
    hash_item_t *target = get_item(request->key);
    //// READ LOCK target if found
    //// unlock bucket

    if (target) {
        send_response(socket, OK, target->value_size, target->value);
    } else {
        send_response(socket, KEY_ERROR, 0, NULL);
    }

    return 0;
}

void del_item(hash_item_t *target) {
    unsigned bucket_index = hash(target->key) % HT_CAPACITY;

    if (target->prev && target->next) {
        // in the middle
        target->prev->next = target->next;
        target->next->prev = target->prev;
    } else if (target->prev) {
        // tail of bucket
        target->prev->next = NULL;
    } else if (target->next) {
        // head of bucket
        target->next->prev = NULL;
        ht->items[bucket_index] = target->next;
    } else {
        // only item in bucket
        ht->items[bucket_index] = NULL;
    }

    free(target->key);
    free(target->value);
    free(target->user);
    free(target);
}

int del_request(int socket, struct request *request) {
    //// lock bucket, no READ Write
    hash_item_t *target = get_item(request->key);

    if (target) {
        del_item(target);
        send_response(socket, OK, 0, NULL);
    } else {
        send_response(socket, KEY_ERROR, 0, NULL);
    }

    //// unlock bucket
    return 0;
}

void *main_job(void *arg) {
    int method;
    struct conn_info *conn_info = arg;
    struct request *request = allocate_request();
    request->connection_close = 0;

    pr_info("Starting new session from %s:%d\n",
            inet_ntoa(conn_info->addr.sin_addr),
            ntohs(conn_info->addr.sin_port));

    do {
        method = recv_request(conn_info->socket_fd, request);
        // Insert your operations here
        switch (method) {
            case SET:
                set_request(conn_info->socket_fd, request);
                break;
            case GET:
                get_request(conn_info->socket_fd, request);
                break;
            case DEL:
                del_request(conn_info->socket_fd, request);
                break;
            case RST:
                // ./check.py issues a reset request after each test
                // to bring back the hashtable to a known state.
                // Implement your reset command here.
                send_response(conn_info->socket_fd, OK, 0, NULL);
                break;
        }

        if (request->key) {
            free(request->key);
        }

    } while (!request->connection_close);

    close_connection(conn_info->socket_fd);
    free(request);
    free(conn_info);
    return (void *) NULL;
}

hashtable_t *init_hashtable() {
    hashtable_t *res = (hashtable_t *) malloc(sizeof(hashtable_t));

    res->capacity = HT_CAPACITY;

    res->items = (hash_item_t **) calloc(HT_CAPACITY, sizeof(hash_item_t *));
    for (size_t i = 0; i < HT_CAPACITY; i++) {
        res->items[i] = NULL;
    }

    // bucket locks
    res->user = (struct user_ht *) calloc(1, sizeof(struct user_ht));
    for (size_t i = 0; i < HT_CAPACITY; i++) {
        pthread_mutex_init(&res->user->bucket_locks[i], NULL);
    }

    return res;
}

job_queue_t *init_job_queue() {
    job_queue_t *res = (job_queue_t *) malloc(sizeof(job_queue_t));
    res->front = NULL;
    res->rear = NULL;

    return res;
}

void job_enqueue(job_queue_t *queue, job_t *new_job) {
    // empty queue
    if (queue->rear == NULL) {
        queue->front = queue->rear = new_job;
        return;
    }

    queue->rear->next = new_job;
    queue->rear = new_job;

    return;
}

job_t *job_dequeue(job_queue_t *queue) {
    if (queue->front == NULL) {
        return NULL;
    }

    job_t *res = queue->front;

    queue->front = queue->front->next;

    if (queue->front == NULL) {
        queue->rear = NULL;
    }

    return res;
}


// lock
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

// job queue
job_queue_t *job_pool;

// worker function and threads
void *worker() {
    printf("%ld created\n", pthread_self());

    for (;;) {
        // lock the job queue
        pthread_mutex_lock(&mtx);
        printf("%ld Locked\n",  pthread_self());

        while (1) {
            pthread_cond_wait(&cond, &mtx);
            printf("%ld signal!\n", pthread_self());
            break;
        }

        struct conn_info *conn_info = job_dequeue(job_pool)->connection;
        printf("%ld Job fetched: %d\n", pthread_self(), conn_info->socket_fd);
        pthread_mutex_unlock(&mtx);
        printf("%ld unLocked\n",  pthread_self());

        main_job(conn_info);
    }
}

#define MAX_WORKERS 64
pthread_t thread_pool[MAX_WORKERS];

int main(int argc, char *argv[]) {
    int listen_sock;

    listen_sock = server_init(argc, argv);

    // Initialuze your hashtable.
    // @see kvstore.h for hashtable struct declaration
    ht = init_hashtable();

    //initialise job queue
    job_pool = init_job_queue();

    // create threads pool
    for (int i = 0; i < MAX_WORKERS; ++i) {
        pthread_create(&thread_pool[i], NULL, worker, NULL);
    }

    // producer
    for (;;) {
        struct conn_info *conn_info =
                calloc(1, sizeof(struct conn_info));
        if (accept_new_connection(listen_sock, conn_info) < 0) {
            error("Cannot accept new connection");
            free(conn_info);
            continue;
        }

        // add new job to the queue
        pthread_mutex_lock(&mtx);

        job_t *new_job = (job_t *) malloc(sizeof(job_t));
        new_job->connection = conn_info;
        new_job->next = NULL;
        job_enqueue(job_pool, new_job);

        printf("Producer: new job added\n");
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mtx);
    }

    return 0;
}
