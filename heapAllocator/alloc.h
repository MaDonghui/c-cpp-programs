#ifndef _ALLOC_H
#define _ALLOC_H

void *mymalloc(size_t size);
void *mycalloc(size_t nmemb, size_t size);
void myfree(void *ptr);
void *myrealloc(void *ptr, size_t size);

#endif
