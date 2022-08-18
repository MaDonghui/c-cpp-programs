#include "hash.h"

/*
 * Hash function by
 * http://www.cse.yorku.ca/~oz/hash.html
 */
unsigned int hash(char *str)
{
    unsigned int hash = 5381;
    int c;
    while ((c = *str++) != 0)
        hash = ((hash << 5) + hash) + c;

    return hash;
}
