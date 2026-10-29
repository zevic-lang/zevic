#include "zevic_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#define ZEVIC_NORETURN __declspec(noreturn)
#endif

static int g_argc;
static char **g_argv;

void zevic_init(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
    (void)g_argc;
    (void)g_argv;
}

void *zevic_alloc(int64_t size) {
    if (size <= 0) size = 1;
    void *p = calloc(1, (size_t)size);
    if (!p) {
        fputs("panic: out of memory\n", stderr);
        exit(101);
    }
    return p;
}

void *zevic_alloc_atomic(int64_t size) {
    return zevic_alloc(size);
}

void zevic_panic(const char *msg, int64_t len, const char *file, int32_t line) {
    fflush(stdout);
    fprintf(stderr, "panic: %.*s\n  at %s:%d\n", (int)len, msg, file, (int)line);
    fflush(stderr);
    exit(101);
}
