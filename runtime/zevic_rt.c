/* Zevic runtime. Everything here is called from generated LLVM IR or from the
 * standard library's `extern "zevic"` declarations. All names start with zevic_.
 *
 * ABI rules (see docs/design/ir-notes.md):
 *  - a Zevic String argument arrives as two parameters: (const char *p, int64_t n)
 *  - a String result is written through a leading `zevic_str *out` parameter
 *  - bools cross the boundary as int32_t
 */
#include "zevic_rt.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

static int g_argc;
static char **g_argv;

void zevic_init(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif
}

/* ---- memory ---- */

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

/* ---- panic ---- */

void zevic_panic(const char *msg, int64_t len, const char *file, int32_t line) {
    fflush(stdout);
    fprintf(stderr, "panic: %.*s\n  at %s:%d\n", (int)len, msg, file, (int)line);
    fflush(stderr);
    exit(101);
}

/* ---- output ---- */

static FILE *stream_of(int32_t s) {
    if (s == 2) {
        fflush(stdout);
        return stderr;
    }
    return stdout;
}

void zevic_print(const char *p, int64_t n) {
    fwrite(p, 1, (size_t)n, stdout);
}

void zevic_println(const char *p, int64_t n) {
    fwrite(p, 1, (size_t)n, stdout);
    fputc('\n', stdout);
}

void zevic_eprint(const char *p, int64_t n) {
    fflush(stdout);
    fwrite(p, 1, (size_t)n, stderr);
}

void zevic_eprintln(const char *p, int64_t n) {
    fflush(stdout);
    fwrite(p, 1, (size_t)n, stderr);
    fputc('\n', stderr);
}

/* ---- Printf pieces: the compiler splits the format and checks types ---- */

void zevic_printf_lit(int32_t s, const char *p, int64_t n) {
    fwrite(p, 1, (size_t)n, stream_of(s));
}

void zevic_printf_i(int32_t s, const char *fmt, int64_t v) {
    fprintf(stream_of(s), fmt, (long long)v);
}

void zevic_printf_f(int32_t s, const char *fmt, double v) {
    fprintf(stream_of(s), fmt, v);
}

void zevic_printf_s(int32_t s, const char *fmt, const char *p, int64_t n) {
    char *buf = malloc((size_t)n + 1);
    if (!buf) exit(101);
    memcpy(buf, p, (size_t)n);
    buf[n] = 0;
    fprintf(stream_of(s), fmt, buf);
    free(buf);
}

static int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

void zevic_printf_c(int32_t s, const char *fmt, uint32_t cp) {
    char buf[5];
    int n = utf8_encode(cp, buf);
    buf[n] = 0;
    fprintf(stream_of(s), fmt, buf);
}

/* ---- strings ---- */

static void str_set(zevic_str *out, const char *src, int64_t n) {
    char *p = zevic_alloc_atomic(n + 1);
    if (n > 0) memcpy(p, src, (size_t)n);
    p[n] = 0;
    out->p = p;
    out->n = n;
}

void zevic_str_concat(zevic_str *out, const char *a, int64_t an, const char *b, int64_t bn) {
    char *p = zevic_alloc_atomic(an + bn + 1);
    if (an > 0) memcpy(p, a, (size_t)an);
    if (bn > 0) memcpy(p + an, b, (size_t)bn);
    p[an + bn] = 0;
    out->p = p;
    out->n = an + bn;
}

int32_t zevic_str_eq(const char *a, int64_t an, const char *b, int64_t bn) {
    return an == bn && (an == 0 || memcmp(a, b, (size_t)an) == 0);
}

int32_t zevic_str_cmp(const char *a, int64_t an, const char *b, int64_t bn) {
    int64_t m = an < bn ? an : bn;
    int c = m > 0 ? memcmp(a, b, (size_t)m) : 0;
    if (c != 0) return c < 0 ? -1 : 1;
    return an < bn ? -1 : an > bn ? 1 : 0;
}

int64_t zevic_str_char_count(const char *p, int64_t n) {
    int64_t c = 0;
    for (int64_t i = 0; i < n; i++)
        if (((unsigned char)p[i] & 0xC0) != 0x80) c++;
    return c;
}

void zevic_i64_to_str(zevic_str *out, int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    str_set(out, buf, n);
}

void zevic_u64_to_str(zevic_str *out, int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)v);
    str_set(out, buf, n);
}

void zevic_f64_to_str(zevic_str *out, double v) {
    char buf[64];
    if (isnan(v)) {
        str_set(out, "NaN", 3);
        return;
    }
    if (isinf(v)) {
        if (v < 0) str_set(out, "-Infinity", 9);
        else str_set(out, "Infinity", 8);
        return;
    }
    int n = 0;
    for (int prec = 1; prec <= 17; prec++) {
        n = snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (strtod(buf, NULL) == v) break;
    }
    if (!strpbrk(buf, ".e")) {
        buf[n++] = '.';
        buf[n++] = '0';
        buf[n] = 0;
    }
    str_set(out, buf, n);
}

void zevic_bool_to_str(zevic_str *out, int32_t v) {
    if (v) str_set(out, "true", 4);
    else str_set(out, "false", 5);
}

void zevic_char_to_str(zevic_str *out, uint32_t cp) {
    char buf[5];
    int n = utf8_encode(cp, buf);
    str_set(out, buf, n);
}

/* ---- text to numbers ---- */

/* Is the text a plain decimal number: [+-] digits [. digits] [e [+-] digits] ? */
int32_t zevic_f64_valid(const char *p, int64_t n) {
    int64_t i = 0, digits = 0;
    if (i < n && (p[i] == '+' || p[i] == '-')) i++;
    while (i < n && p[i] >= '0' && p[i] <= '9') { i++; digits++; }
    if (i < n && p[i] == '.') {
        i++;
        int64_t frac = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') { i++; frac++; }
        if (frac == 0) return 0;
    }
    if (digits == 0) return 0;
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        i++;
        if (i < n && (p[i] == '+' || p[i] == '-')) i++;
        int64_t ed = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') { i++; ed++; }
        if (ed == 0) return 0;
    }
    return i == n;
}

double zevic_f64_parse(const char *p, int64_t n) {
    char *buf = malloc((size_t)n + 1);
    if (!buf) exit(101);
    memcpy(buf, p, (size_t)n);
    buf[n] = 0;
    double v = strtod(buf, NULL);
    free(buf);
    return v;
}

/* ---- string builder (used for "${...}" interpolation) ---- */

typedef struct {
    char *p;
    int64_t n, cap;
} sbuf;

void *zevic_sb_new(void) {
    return zevic_alloc(sizeof(sbuf));
}

void zevic_sb_add(void *h, const char *p, int64_t n) {
    sbuf *b = h;
    if (b->n + n + 1 > b->cap) {
        int64_t nc = b->cap ? b->cap : 32;
        while (nc < b->n + n + 1) nc *= 2;
        char *np = zevic_alloc_atomic(nc);
        if (b->n) memcpy(np, b->p, (size_t)b->n);
        b->p = np;
        b->cap = nc;
    }
    if (n > 0) memcpy(b->p + b->n, p, (size_t)n);
    b->n += n;
    b->p[b->n] = 0;
}

void zevic_sb_finish(zevic_str *out, void *h) {
    sbuf *b = h;
    if (!b->p) {
        str_set(out, "", 0);
        return;
    }
    out->p = b->p;
    out->n = b->n;
}
