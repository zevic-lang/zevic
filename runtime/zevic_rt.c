/* Zevic runtime. Everything here is called from generated LLVM IR or from the
 * standard library's `extern "zevic"` declarations. All names start with zevic_.
 *
 * ABI rules (see docs/design/ir-notes.md):
 *  - a Zevic String argument arrives as two parameters: (const char *p, int64_t n)
 *  - a String result is written through a leading `zevic_str *out` parameter
 *  - bools cross the boundary as int32_t
 */
#include "zevic_rt.h"

#include "gc.h"

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
    GC_INIT();
    g_argc = argc;
    g_argv = argv;
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif
}

/* ---- memory ---- */

void *zevic_alloc(int64_t size) {
    if (size <= 0) size = 1;
    void *p = GC_MALLOC((size_t)size); /* cleared, scanned for pointers */
    if (!p) {
        fputs("panic: out of memory\n", stderr);
        exit(101);
    }
    return p;
}

/* Memory that holds no pointers (string bytes): the collector does not scan it. Not cleared. */
void *zevic_alloc_atomic(int64_t size) {
    if (size <= 0) size = 1;
    void *p = GC_MALLOC_ATOMIC((size_t)size);
    if (!p) {
        fputs("panic: out of memory\n", stderr);
        exit(101);
    }
    return p;
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
    int n = 0, prec = 1;
    for (prec = 1; prec <= 17; prec++) {
        n = snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (strtod(buf, NULL) == v) break;
    }
    /* %g switches to exponent notation early (1000 -> 1e+03); keep plain digits for moderate sizes */
    char *ep = strchr(buf, 'e');
    if (ep) {
        int e = atoi(ep + 1);
        if (e >= -5 && e < 16) {
            int decimals = prec - 1 - e;
            if (decimals < 0) decimals = 0;
            n = snprintf(buf, sizeof buf, "%.*f", decimals, v);
        }
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

/* ---- C strings ---- */

/* Length of the valid UTF-8 sequence at p (0 if invalid). */
static int utf8_seq(const unsigned char *p, int64_t left) {
    unsigned c = p[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return left >= 2 && (p[1] & 0xC0) == 0x80 ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (left < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && p[1] < 0xA0) return 0;
        if (c == 0xED && p[1] >= 0xA0) return 0;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (left < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0 && p[1] < 0x90) return 0;
        if (c == 0xF4 && p[1] >= 0x90) return 0;
        return 4;
    }
    return 0;
}

/* Copy a NUL-terminated C string into a new String.
 * lossy = 0: returns 0 (and leaves *out empty) when the bytes are not UTF-8.
 * lossy = 1: bad bytes become U+FFFD; always returns 1. A NULL pointer gives "". */
int32_t zevic_str_from_c(zevic_str *out, const char *c, int32_t lossy) {
    if (!c) {
        str_set(out, "", 0);
        return 1;
    }
    int64_t n = (int64_t)strlen(c);
    const unsigned char *u = (const unsigned char *)c;
    if (!lossy) {
        for (int64_t i = 0; i < n;) {
            int k = utf8_seq(u + i, n - i);
            if (!k) {
                str_set(out, "", 0);
                return 0;
            }
            i += k;
        }
        str_set(out, c, n);
        return 1;
    }
    char *p = zevic_alloc_atomic(n * 3 + 1);
    int64_t m = 0;
    for (int64_t i = 0; i < n;) {
        int k = utf8_seq(u + i, n - i);
        if (k) {
            memcpy(p + m, u + i, (size_t)k);
            m += k;
            i += k;
        } else {
            memcpy(p + m, "\xEF\xBF\xBD", 3);
            m += 3;
            i++;
        }
    }
    p[m] = 0;
    out->p = p;
    out->n = m;
    return 1;
}

/* A copy of the bytes in malloc'd memory, NUL-terminated, for C to keep and free. */
char *zevic_new_cstring(const char *p, int64_t n) {
    char *c = malloc((size_t)n + 1);
    if (!c) {
        fputs("panic: out of memory\n", stderr);
        exit(101);
    }
    if (n > 0) memcpy(c, p, (size_t)n);
    c[n] = 0;
    return c;
}

/* ---- hashing (Hashable for the built-in types) ---- */

uint64_t zevic_hash_u64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

uint64_t zevic_str_hash(const char *p, int64_t n) {
    uint64_t h = 0xcbf29ce484222325ULL; /* FNV-1a */
    for (int64_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ---- handles: a table that lets C hold a number instead of a Zevic object ----
 * The table lives in memory the collector scans but never frees, so an object stays alive while it is listed. */

static void **g_handles;
static uint64_t g_handle_n, g_handle_cap;
static uint64_t *g_handle_free;
static uint64_t g_handle_nfree, g_handle_freecap;

uint64_t zevic_handle_new(void *obj) {
    if (g_handle_nfree > 0) {
        uint64_t id = g_handle_free[--g_handle_nfree];
        g_handles[id - 1] = obj;
        return id;
    }
    if (g_handle_n == g_handle_cap) {
        uint64_t nc = g_handle_cap ? g_handle_cap * 2 : 64;
        void **nt = GC_MALLOC_UNCOLLECTABLE((size_t)nc * sizeof(void *));
        if (!nt) {
            fputs("panic: out of memory\n", stderr);
            exit(101);
        }
        if (g_handles) {
            memcpy(nt, g_handles, (size_t)g_handle_n * sizeof(void *));
            GC_FREE(g_handles);
        }
        g_handles = nt;
        g_handle_cap = nc;
    }
    g_handles[g_handle_n++] = obj;
    return g_handle_n;
}

void *zevic_handle_get(uint64_t id) {
    if (id == 0 || id > g_handle_n) return NULL;
    return g_handles[id - 1];
}

void zevic_handle_release(uint64_t id) {
    if (id == 0 || id > g_handle_n || !g_handles[id - 1]) return;
    g_handles[id - 1] = NULL;
    if (g_handle_nfree == g_handle_freecap) {
        g_handle_freecap = g_handle_freecap ? g_handle_freecap * 2 : 64;
        g_handle_free = realloc(g_handle_free, (size_t)g_handle_freecap * sizeof(uint64_t));
        if (!g_handle_free) exit(101);
    }
    g_handle_free[g_handle_nfree++] = id;
}

/* ---- environment ---- */

static char *nul_copy(const char *p, int64_t n) {
    char *b = malloc((size_t)n + 1);
    if (!b) exit(101);
    memcpy(b, p, (size_t)n);
    b[n] = 0;
    return b;
}

int32_t zevic_env_has(const char *name, int64_t nn) {
    char *k = nul_copy(name, nn);
    int has = getenv(k) != NULL;
    free(k);
    return has;
}

void zevic_env_get(zevic_str *out, const char *name, int64_t nn) {
    char *k = nul_copy(name, nn);
    const char *v = getenv(k);
    free(k);
    if (!v) v = "";
    str_set(out, v, (int64_t)strlen(v));
}

int32_t zevic_env_set(const char *name, int64_t nn, const char *val, int64_t vn) {
    char *k = nul_copy(name, nn), *v = nul_copy(val, vn);
#ifdef _WIN32
    int rc = _putenv_s(k, v);
#else
    int rc = setenv(k, v, 1);
#endif
    free(k);
    free(v);
    return rc == 0;
}

int32_t zevic_argc(void) {
    return g_argc;
}

void zevic_arg(zevic_str *out, int32_t i) {
    if (i < 0 || i >= g_argc) {
        str_set(out, "", 0);
        return;
    }
    str_set(out, g_argv[i], (int64_t)strlen(g_argv[i]));
}

void zevic_exit(int32_t code) {
    fflush(stdout);
    fflush(stderr);
    exit(code);
}
