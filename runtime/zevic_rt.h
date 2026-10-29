/* Zevic runtime: called from generated LLVM IR. All names start with zevic_. */
#ifndef ZEVIC_RT_H
#define ZEVIC_RT_H

#include <stdint.h>
#include <stddef.h>

/* A Zevic String: pointer to UTF-8 bytes (always followed by a NUL) and a byte length. */
typedef struct {
    const char *p;
    int64_t n;
} zevic_str;

/* Memory. Stage 1-4: malloc and never free. Stage 5 swaps these for Boehm GC. */
void *zevic_alloc(int64_t size);
void *zevic_alloc_atomic(int64_t size);

/* Panic: prints "panic: <msg>\n  at <file>:<line>" to stderr and exits with code 101. */
void zevic_panic(const char *msg, int64_t len, const char *file, int32_t line) __attribute__((noreturn));

/* Program start/stop, called from the generated C-compatible main. */
void zevic_init(int argc, char **argv);

#endif
