/* Zevic runtime interface. */
#ifndef ZEVIC_RT_H
#define ZEVIC_RT_H

#include <stddef.h>
#include <stdint.h>

/* A Zevic String: UTF-8 bytes (always followed by a NUL) and a byte length. */
typedef struct {
    const char *p;
    int64_t n;
} zevic_str;

/* Memory: the Boehm collector (runtime/third_party/bgc). Everything allocated here is collected. */
void *zevic_alloc(int64_t size);
void *zevic_alloc_atomic(int64_t size);

/* Panic: prints "panic: <msg>" and the location to stderr, exits with code 101. */
#if defined(__GNUC__) || defined(__clang__)
void zevic_panic(const char *msg, int64_t len, const char *file, int32_t line) __attribute__((noreturn));
#else
void zevic_panic(const char *msg, int64_t len, const char *file, int32_t line);
#endif

/* Called first thing from the generated `main`. */
void zevic_init(int argc, char **argv);

#endif
