#ifndef _CHECKED_ALLOC_H
#define _CHECKED_ALLOC_H

#ifndef ALLOC_TEST_FRAMEWORK
# error This may only be used by the internal test framework
#endif

#include <stdlib.h>

extern int checked_alloc_disable_integrity_check;

void *checked_alloc_array(size_t nmemb, size_t size);
void *checked_alloc(size_t size);
void checked_free(void *ptr);
void *checked_realloc(void *ptr, size_t size);

#endif
