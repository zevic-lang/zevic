/* util.h - memory, strings, source files and diagnostics */
#ifndef ZEVIC_UTIL_H
#define ZEVIC_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A source position. `file` indexes the table filled by src_add(). */
typedef struct {
    int file;
    int line; /* 1-based */
    int col;  /* 1-based, in bytes */
} Pos;

/* Memory: the compiler never frees. */
void *zalloc(size_t n);
void *zrealloc(void *p, size_t old, size_t n);
char *zstrdup(const char *s);
char *zstrndup(const char *s, size_t n);
char *zsprintf(const char *fmt, ...);
char *zvsprintf(const char *fmt, va_list ap);

/* Growable string buffer. */
typedef struct {
    char *p;
    size_t len, cap;
} SB;

void sb_putc(SB *sb, char c);
void sb_putn(SB *sb, const char *s, size_t n);
void sb_puts(SB *sb, const char *s);
void sb_printf(SB *sb, const char *fmt, ...);
char *sb_str(SB *sb); /* NUL-terminated, never NULL */

/* Growable pointer vector. */
typedef struct {
    void **v;
    int n, cap;
} Vec;

void vec_push(Vec *v, void *x);

/* Source files and diagnostics. */
int src_add(const char *name, char *text);
const char *src_name(int file);
const char *src_text(int file);

extern int zv_error_count;
extern int zv_warn_count;

void diag_error(Pos pos, const char *fmt, ...);
void diag_warn(Pos pos, const char *fmt, ...);
void diag_note(const char *fmt, ...);
void die(const char *fmt, ...) __attribute__((noreturn));

/* Files. Returns NULL when the file cannot be read. The result is NUL-terminated. */
char *read_file(const char *path, size_t *len);
bool file_exists(const char *path);
bool dir_exists(const char *path);

#endif
