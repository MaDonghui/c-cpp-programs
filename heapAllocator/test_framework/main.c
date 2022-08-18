/*
 * Test framework for Assignment 2: Allocator.
 *
 * Launches one or more tests and allows enabling/disabling of debug output and
 * certain other features.
 *
 * Authors:
 *  Koen Koning <koen.koning@vu.nl>
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "common.h"
#include "tests.h"
#include "memlist.h"

/* Print debug output. */
int verbose = 0;

/* Use mycalloc instead of mymalloc for allocations. */
int use_calloc = 0;

/* Use malloc and friend instead of my* functions. */
int use_system_alloc = 0;

/* Maximum brk (heap) size. */
size_t max_brk_size = 128 * 1024 * 1024;

static struct option long_options[] = {
    { "help",       no_argument,        NULL, 'h' },
    { "verbose",    no_argument,        NULL, 'v' },
    { "use-calloc", no_argument,        NULL, 'c' },
    { "use-system", no_argument,        NULL, 'l' },
    { "stats",      no_argument,        NULL, 's' },
    { "brk-size",   required_argument,  NULL, 'm' },
    { 0, 0, 0, 0 }
};

static void usage(char *progname)
{
    int i;
    printf("%s [OPTION]... [TEST]...\n"
           "Test program for OS Assignment 2: Allocator.\n"
           "Available tests:\n",
           progname);
    for (i = 0; tests[i].name; i++)
        printf("  %s\n", tests[i].name);
    printf("Additionally, the following options are accepted:\n");
    printf(
    "  -v, --verbose    Print what the test framework is doing\n"
    "  -c, --use-calloc Use calloc instead of malloc for any allocations\n"
    "                   the test framework does\n"
    "  -l, --use-system Use system allocator functions (malloc etc) instead\n"
    "                   of the my* functions.\n"
    "  -s, --stats      Print statistics on heap usage at end of tests\n"
    "  -m, --brk-size   Maximum brk (heap) size in bytes (default 128M).\n");
}

static void run_test(char *name)
{
    struct test_case *test;
    for (test = &tests[0]; test->name; test++) {
        if (!strcmp(name, test->name)) {
            test->func();
            return;
        }
    }
    error("Unknown test %s", name);
}

void print_stats(void)
{
    size_t objs = memlist_length(&allocs);
    size_t obj_bytes = memlist_byte_size(&allocs);
    size_t heap_bytes = cur_brk - heap;
    size_t empty_bytes = heap_bytes - obj_bytes;
    printf("Number of active heap objects: %zu\n", objs);
    printf("Size in bytes of active heap objects: %zu\n", obj_bytes);
    printf("Total heap size reserved: %zu\n", heap_bytes);
    printf("Heap space empty: %zu\n", empty_bytes);
    printf("Heap fragmentation: %.2f bytes per object\n",
           1. * empty_bytes / objs);
}

int main(int argc, char **argv)
{
    int flag_print_stats = 0;
    int i;

    if (argc == 1) {
        usage(argv[0]);
        return 1;
    }

    for (;;) {
        int option_index = 0;
        int c;

        c = getopt_long(argc, argv, "hvclsm:", long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 'h': usage(argv[0]); return 0;
        case 'v': verbose = 1; break;
        case 'c': use_calloc = 1; break;
        case 'l': use_system_alloc = 1; break;
        case 's': flag_print_stats = 1; break;
        case 'm': max_brk_size = strtoul(optarg, NULL, 0); break;
        default: return 1;
        }
    }

    for (i = optind; i < argc; i++)
        run_test(argv[i]);

    if (flag_print_stats)
        print_stats();

    return 0;
}
