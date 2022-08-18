#ifndef _TESTS_H
#define _TESTS_H

#ifndef ALLOC_TEST_FRAMEWORK
# error This may only be used by the internal test framework
#endif

struct test_case {
    char *name;
    void (*func)(void);
};

extern struct test_case tests[];

#endif
