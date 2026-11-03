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
#include <setjmp.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#ifndef S_IFMT
#define S_IFMT _S_IFMT
#define S_IFDIR _S_IFDIR
#endif
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

/* While the test runner is working, a panic or a failed assertion jumps back to it instead of ending the program. */
static _Thread_local jmp_buf *g_jmp;
static _Thread_local int g_jmp_kind; /* 1: a panic, 2: a failed assertion */
static _Thread_local char g_fail[2048];
static _Thread_local const char *g_at_file = "?";
static _Thread_local int g_at_line;

/* The file name without its directory (what the test report shows). */
static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *c = path; *c; c++)
        if (*c == '/' || *c == '\\') b = c + 1;
    return b;
}

void zevic_panic(const char *msg, int64_t len, const char *file, int32_t line) {
    if (g_jmp) {
        snprintf(g_fail, sizeof g_fail, "%s:%d: panic: %.*s", base_name(file), (int)line, (int)len, msg);
        g_jmp_kind = 1;
        longjmp(*g_jmp, 1);
    }
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

/* The full path of the running program ("" if the system will not tell). */
void zevic_exe_path(zevic_str *out) {
    char buf[4096];
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
    if (n > 0 && n < sizeof buf) {
        str_set(out, buf, (int64_t)n);
        return;
    }
#elif defined(__APPLE__)
    uint32_t sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) == 0) {
        char *real = realpath(buf, NULL);
        str_set(out, real ? real : buf, (int64_t)strlen(real ? real : buf));
        free(real);
        return;
    }
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        str_set(out, buf, (int64_t)n);
        return;
    }
#endif
    str_set(out, "", 0);
}

void zevic_cwd(zevic_str *out) {
    char buf[4096];
#ifdef _WIN32
    char *r = _getcwd(buf, sizeof buf);
#else
    char *r = getcwd(buf, sizeof buf);
#endif
    str_set(out, r ? buf : "", r ? (int64_t)strlen(buf) : 0);
}

void zevic_exit(int32_t code) {
    fflush(stdout);
    fflush(stderr);
    exit(code);
}

/* ---- input ---- */

/* One line without its line ending (\n or \r\n). Whether a line was read (0 at the end of the input) is
 * answered by zevic_read_line_ok() straight afterwards, because a String result takes the return slot. */
static _Thread_local int32_t g_read_ok;

int32_t zevic_read_line_ok(void) {
    return g_read_ok;
}

void zevic_read_line(zevic_str *out) {
    g_read_ok = 0;
    fflush(stdout);
    int64_t cap = 128, n = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) exit(101);
    int c, got = 0;
    while ((c = fgetc(stdin)) != EOF) {
        got = 1;
        if (c == '\n') break;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) exit(101);
        }
        buf[n++] = (char)c;
    }
    if (!got) {
        free(buf);
        str_set(out, "", 0);
        return;
    }
    if (n > 0 && buf[n - 1] == '\r') n--;
    str_set(out, buf, n);
    free(buf);
    g_read_ok = 1;
}

/* One UTF-8 character as a code point, or -1 at the end of the input. */
int32_t zevic_read_char(void) {
    fflush(stdout);
    int c = fgetc(stdin);
    if (c == EOF) return -1;
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = extra == 3 ? (uint32_t)(c & 0x07) : extra == 2 ? (uint32_t)(c & 0x0F) : extra == 1 ? (uint32_t)(c & 0x1F) : 0xFFFD;
    for (int i = 0; i < extra; i++) {
        int d = fgetc(stdin);
        if (d == EOF || (d & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (uint32_t)(d & 0x3F);
    }
    return (int32_t)cp;
}

void zevic_read_all(zevic_str *out) {
    fflush(stdout);
    int64_t cap = 4096, n = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) exit(101);
    size_t r;
    while ((r = fread(buf + n, 1, (size_t)(cap - n), stdin)) > 0) {
        n += (int64_t)r;
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) exit(101);
        }
    }
    str_set(out, buf, n);
    free(buf);
}

/* ---- text helpers ---- */

/* Is [from, to) inside the string and on character boundaries? */
int32_t zevic_str_slice_ok(const char *p, int64_t n, int64_t from, int64_t to) {
    if (from < 0 || to < from || to > n) return 0;
    if (from < n && ((unsigned char)p[from] & 0xC0) == 0x80) return 0;
    if (to < n && ((unsigned char)p[to] & 0xC0) == 0x80) return 0;
    return 1;
}

void zevic_str_slice(zevic_str *out, const char *p, int64_t n, int64_t from, int64_t to) {
    (void)n;
    str_set(out, p + from, to - from);
}

/* Byte position of the first occurrence of q in p at or after `from`, or -1. */
int64_t zevic_str_index(const char *p, int64_t n, const char *q, int64_t m, int64_t from) {
    if (from < 0) from = 0;
    if (m == 0) return from <= n ? from : -1;
    for (int64_t i = from; i + m <= n; i++)
        if (p[i] == q[0] && memcmp(p + i, q, (size_t)m) == 0) return i;
    return -1;
}

/* ASCII case conversion: letters outside ASCII are left as they are. */
void zevic_str_upper(zevic_str *out, const char *p, int64_t n) {
    char *b = zevic_alloc_atomic(n + 1);
    for (int64_t i = 0; i < n; i++) b[i] = (p[i] >= 'a' && p[i] <= 'z') ? (char)(p[i] - 32) : p[i];
    b[n] = 0;
    out->p = b;
    out->n = n;
}

void zevic_str_lower(zevic_str *out, const char *p, int64_t n) {
    char *b = zevic_alloc_atomic(n + 1);
    for (int64_t i = 0; i < n; i++) b[i] = (p[i] >= 'A' && p[i] <= 'Z') ? (char)(p[i] + 32) : p[i];
    b[n] = 0;
    out->p = b;
    out->n = n;
}

/* ---- time ---- */

int64_t zevic_time_monotonic_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

int64_t zevic_time_unix_ns(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100 ns since 1601 */
    return (int64_t)((t - 116444736000000000ULL) * 100ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

void zevic_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    fflush(stdout);
#ifdef _WIN32
    Sleep((DWORD)((ns + 999999) / 1000000));
#else
    struct timespec ts = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
    while (nanosleep(&ts, &ts) != 0) {}
#endif
}

/* ---- files ----
 * A failing call leaves the reason in g_fs_errno; zevic_fs_errkind() and zevic_fs_errmsg() describe it. */

static _Thread_local int g_fs_errno;

/* Did the last file call fail? */
int32_t zevic_fs_failed(void) {
    return g_fs_errno != 0;
}

int32_t zevic_fs_errkind(void) {
    if (g_fs_errno == ENOENT) return 0; /* NotFound */
    if (g_fs_errno == EACCES || g_fs_errno == EPERM) return 1; /* Permission */
    return 2; /* Io */
}

void zevic_fs_errmsg(zevic_str *out) {
    const char *m = strerror(g_fs_errno);
    str_set(out, m, (int64_t)strlen(m));
}

static FILE *fs_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    /* paths are UTF-8 */
    wchar_t wp[4096], wm[8];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 4096) == 0) { errno = EINVAL; return NULL; }
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8);
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}

void zevic_fs_read_all(zevic_str *out, const char *p, int64_t n) {
    char *path = nul_copy(p, n);
    g_fs_errno = 0;
    FILE *f = fs_fopen(path, "rb");
    free(path);
    if (!f) {
        g_fs_errno = errno;
        str_set(out, "", 0);
        return;
    }
    int64_t cap = 4096, len = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) exit(101);
    size_t r;
    while ((r = fread(buf + len, 1, (size_t)(cap - len), f)) > 0) {
        len += (int64_t)r;
        if (len == cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) exit(101);
        }
    }
    if (ferror(f)) g_fs_errno = errno ? errno : EIO;
    fclose(f);
    str_set(out, buf, len);
    free(buf);
}

/* append = 0: replace the file; 1: add to its end. Returns 1 on success, 0 on failure. */
int32_t zevic_fs_write_all(const char *p, int64_t n, const char *d, int64_t dn, int32_t append) {
    char *path = nul_copy(p, n);
    g_fs_errno = 0;
    FILE *f = fs_fopen(path, append ? "ab" : "wb");
    free(path);
    if (!f) {
        g_fs_errno = errno;
        return 0;
    }
    int ok = dn == 0 || fwrite(d, 1, (size_t)dn, f) == (size_t)dn;
    if (!ok) g_fs_errno = errno ? errno : EIO;
    if (fclose(f) != 0 && ok) {
        g_fs_errno = errno ? errno : EIO;
        ok = 0;
    }
    return ok;
}

/* 0: does not exist, 1: a file, 2: a directory */
int32_t zevic_fs_kind(const char *p, int64_t n) {
    char *path = nul_copy(p, n);
    struct stat st;
    int rc = stat(path, &st);
    free(path);
    if (rc != 0) return 0;
    return (st.st_mode & S_IFMT) == S_IFDIR ? 2 : 1;
}

int32_t zevic_fs_remove(const char *p, int64_t n) {
    char *path = nul_copy(p, n);
    g_fs_errno = 0;
    int rc = remove(path);
    free(path);
    if (rc != 0) {
        g_fs_errno = errno;
        return 0;
    }
    return 1;
}

int32_t zevic_fs_rename(const char *a, int64_t an, const char *b, int64_t bn) {
    char *from = nul_copy(a, an);
    char *to = nul_copy(b, bn);
    g_fs_errno = 0;
    int rc = rename(from, to);
    free(from);
    free(to);
    if (rc != 0) {
        g_fs_errno = errno;
        return 0;
    }
    return 1;
}

int32_t zevic_fs_mkdir(const char *p, int64_t n) {
    char *path = nul_copy(p, n);
    g_fs_errno = 0;
#ifdef _WIN32
    int rc = _mkdir(path);
#else
    int rc = mkdir(path, 0777);
#endif
    free(path);
    if (rc != 0) {
        g_fs_errno = errno;
        return 0;
    }
    return 1;
}

/* The names in a directory (not "." and ".."), one per line. */
void zevic_fs_list(zevic_str *out, const char *p, int64_t n) {
    char *path = nul_copy(p, n);
    g_fs_errno = 0;
    int64_t cap = 256, len = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) exit(101);
#ifdef _WIN32
    char *pat = malloc((size_t)n + 3);
    memcpy(pat, path, (size_t)n);
    memcpy(pat + n, "\\*", 3);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) {
        g_fs_errno = ENOENT;
    } else {
        do {
            const char *name = fd.cFileName;
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            int64_t l = (int64_t)strlen(name);
            while (len + l + 2 > cap) { cap *= 2; buf = realloc(buf, (size_t)cap); if (!buf) exit(101); }
            memcpy(buf + len, name, (size_t)l);
            len += l;
            buf[len++] = '\n';
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(path);
    if (!d) {
        g_fs_errno = errno;
    } else {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *name = e->d_name;
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            int64_t l = (int64_t)strlen(name);
            while (len + l + 2 > cap) {
                cap *= 2;
                buf = realloc(buf, (size_t)cap);
                if (!buf) exit(101);
            }
            memcpy(buf + len, name, (size_t)l);
            len += l;
            buf[len++] = '\n';
        }
        closedir(d);
    }
#endif
    free(path);
    str_set(out, buf, len);
    free(buf);
}

/* ---- open files: mode 0 read, 1 write (replace), 2 append ---- */

void *zevic_fs_open(const char *p, int64_t n, int32_t mode) {
    char *path = nul_copy(p, n);
    g_fs_errno = 0;
    FILE *f = fs_fopen(path, mode == 0 ? "rb" : mode == 1 ? "wb" : "ab");
    free(path);
    if (!f) g_fs_errno = errno;
    return f;
}

int32_t zevic_fs_close(void *h) {
    g_fs_errno = 0;
    if (fclose((FILE *)h) != 0) {
        g_fs_errno = errno ? errno : EIO;
        return 0;
    }
    return 1;
}

int32_t zevic_fs_file_write(void *h, const char *d, int64_t dn) {
    g_fs_errno = 0;
    if (dn > 0 && fwrite(d, 1, (size_t)dn, (FILE *)h) != (size_t)dn) {
        g_fs_errno = errno ? errno : EIO;
        return 0;
    }
    return 1;
}

static _Thread_local int32_t g_file_line_ok;

int32_t zevic_fs_file_line_ok(void) {
    return g_file_line_ok;
}

/* One line (without \n or \r\n) from an open file; zevic_fs_file_line_ok() tells whether there was one. */
void zevic_fs_file_read_line(zevic_str *out, void *h) {
    FILE *f = h;
    g_file_line_ok = 0;
    int64_t cap = 128, n = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) exit(101);
    int c, got = 0;
    while ((c = fgetc(f)) != EOF) {
        got = 1;
        if (c == '\n') break;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) exit(101);
        }
        buf[n++] = (char)c;
    }
    if (got) {
        if (n > 0 && buf[n - 1] == '\r') n--;
        g_file_line_ok = 1;
    }
    str_set(out, buf, n);
    free(buf);
}

void zevic_fs_file_read_all(zevic_str *out, void *h) {
    FILE *f = h;
    int64_t cap = 4096, len = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) exit(101);
    size_t r;
    while ((r = fread(buf + len, 1, (size_t)(cap - len), f)) > 0) {
        len += (int64_t)r;
        if (len == cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) exit(101);
        }
    }
    str_set(out, buf, len);
    free(buf);
}

/* ---- processes ----
 * A command is built with zevic_proc_begin / zevic_proc_arg, then run through the shell with stdout and stderr
 * redirected to temporary files. Every argument is quoted, so the shell does not interpret it. */


static _Thread_local char *g_proc_cmd;
static _Thread_local int64_t g_proc_len, g_proc_cap;
static _Thread_local int32_t g_proc_code;
static _Thread_local int32_t g_proc_failed;
static _Thread_local char *g_proc_err;
static _Thread_local int64_t g_proc_errlen;

static void proc_put(const char *p, int64_t n) {
    if (g_proc_len + n + 1 > g_proc_cap) {
        g_proc_cap = g_proc_cap ? g_proc_cap * 2 : 256;
        while (g_proc_cap < g_proc_len + n + 1) g_proc_cap *= 2;
        g_proc_cmd = realloc(g_proc_cmd, (size_t)g_proc_cap);
        if (!g_proc_cmd) exit(101);
    }
    memcpy(g_proc_cmd + g_proc_len, p, (size_t)n);
    g_proc_len += n;
    g_proc_cmd[g_proc_len] = 0;
}

void zevic_proc_begin(void) {
    g_proc_len = 0;
    proc_put("", 0);
}

void zevic_proc_arg(const char *p, int64_t n) {
    if (g_proc_len > 0) proc_put(" ", 1);
#ifdef _WIN32
    proc_put("\"", 1);
    int64_t backslashes = 0;
    for (int64_t i = 0; i < n; i++) {
        if (p[i] == '\\') {
            backslashes++;
            continue;
        }
        if (p[i] == '"') {
            for (int64_t k = 0; k < backslashes; k++) proc_put("\\", 1);
            proc_put("\\", 1);
        }
        backslashes = 0;
        proc_put(p + i, 1);
    }
    for (int64_t k = 0; k < backslashes; k++) proc_put("\\", 1);
    proc_put("\"", 1);
#else
    proc_put("'", 1);
    for (int64_t i = 0; i < n; i++) {
        if (p[i] == '\'') proc_put("'\\''", 4);
        else proc_put(p + i, 1);
    }
    proc_put("'", 1);
#endif
}

static char *temp_path(void) {
#ifdef _WIN32
    char dir[MAX_PATH], name[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, dir) || !GetTempFileNameA(dir, "zev", 0, name)) return NULL;
    return strdup(name);
#else
    char tmpl[] = "/tmp/zevic-proc-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    close(fd);
    return strdup(tmpl);
#endif
}

static char *slurp(const char *path, int64_t *len) {
    FILE *f = fopen(path, "rb");
    *len = 0;
    char *buf = malloc(1);
    if (!buf) exit(101);
    if (!f) return buf;
    int64_t cap = 1024;
    buf = realloc(buf, (size_t)cap);
    size_t r;
    while ((r = fread(buf + *len, 1, (size_t)(cap - *len), f)) > 0) {
        *len += (int64_t)r;
        if (*len == cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) exit(101);
        }
    }
    fclose(f);
    return buf;
}

/* Runs the command; its standard output is the result. Afterwards zevic_proc_failed() says whether it could not
 * be run at all, zevic_proc_code() gives the exit code and zevic_proc_stderr() what it wrote to stderr. */
void zevic_proc_run(zevic_str *out) {
    fflush(stdout);
    fflush(stderr);
    g_proc_failed = 0;
    g_proc_code = -1;
    g_proc_errlen = 0;
    free(g_proc_err);
    g_proc_err = NULL;
    char *fo = temp_path(), *fe = temp_path();
    if (!fo || !fe) {
        g_proc_failed = 1;
        str_set(out, "", 0);
        return;
    }
    int64_t base = g_proc_len;
    proc_put(" >", 2);
    proc_put(fo, (int64_t)strlen(fo));
    proc_put(" 2>", 3);
    proc_put(fe, (int64_t)strlen(fe));
#ifdef _WIN32
    /* cmd.exe strips one pair of outer quotes */
    char *line = malloc((size_t)g_proc_len + 3);
    sprintf(line, "\"%s\"", g_proc_cmd);
    int rc = system(line);
    free(line);
    g_proc_code = rc;
#else
    int rc = system(g_proc_cmd);
    if (rc == -1) g_proc_failed = 1;
    else if (WIFEXITED(rc)) g_proc_code = WEXITSTATUS(rc);
    else g_proc_code = 128 + (WIFSIGNALED(rc) ? WTERMSIG(rc) : 0);
#endif
    g_proc_len = base;
    g_proc_cmd[base] = 0;
    int64_t n;
    char *o = slurp(fo, &n);
    str_set(out, o, n);
    free(o);
    g_proc_err = slurp(fe, &g_proc_errlen);
    remove(fo);
    remove(fe);
    free(fo);
    free(fe);
}

/* Runs the command with this program's own standard input, output and error. Afterwards zevic_proc_failed()
 * and zevic_proc_code() tell what happened. */
void zevic_proc_inherit(void) {
    fflush(stdout);
    fflush(stderr);
    g_proc_failed = 0;
    g_proc_code = -1;
#ifdef _WIN32
    char *line = malloc((size_t)g_proc_len + 3);
    sprintf(line, "\"%s\"", g_proc_cmd);
    int rc = system(line);
    free(line);
    g_proc_code = rc;
#else
    int rc = system(g_proc_cmd);
    if (rc == -1) g_proc_failed = 1;
    else if (WIFEXITED(rc)) g_proc_code = WEXITSTATUS(rc);
    else g_proc_code = 128 + (WIFSIGNALED(rc) ? WTERMSIG(rc) : 0);
#endif
}

int32_t zevic_proc_failed(void) {
    return g_proc_failed;
}

int32_t zevic_proc_code(void) {
    return g_proc_code;
}

void zevic_proc_stderr(zevic_str *out) {
    str_set(out, g_proc_err ? g_proc_err : "", g_proc_err ? g_proc_errlen : 0);
}

/* ---- tests ---- */

typedef struct {
    const char *name;
    int64_t len;
    void (*fn)(void);
} test_case;

static test_case *g_tests;
static int g_ntests, g_captests;

void zevic_test_add(const char *name, int64_t len, void (*fn)(void)) {
    if (g_ntests == g_captests) {
        g_captests = g_captests ? g_captests * 2 : 16;
        g_tests = realloc(g_tests, sizeof(test_case) * (size_t)g_captests);
        if (!g_tests) exit(101);
    }
    g_tests[g_ntests].name = name;
    g_tests[g_ntests].len = len;
    g_tests[g_ntests].fn = fn;
    g_ntests++;
}

/* Called by the compiler before every call into the module `Zevic.Test`: the line of the caller. */
void zevic_test_at(const char *file, int32_t line) {
    g_at_file = file;
    g_at_line = (int)line;
}

/* A failed assertion: ends the running test. Outside the test runner it ends the program like a panic. */
void zevic_test_fail(const char *msg, int64_t len) {
    if (g_jmp) {
        snprintf(g_fail, sizeof g_fail, "%s:%d: %.*s", base_name(g_at_file), g_at_line, (int)len, msg);
        g_jmp_kind = 2;
        longjmp(*g_jmp, 1);
    }
    zevic_panic(msg, len, g_at_file, (int32_t)g_at_line);
}

/* Runs f and reports whether it panicked (1) or returned (0). A failed assertion inside f is not a panic:
 * it goes on to the test runner. `f` is a closure: the code pointer and its environment. */
int32_t zevic_catch(void (*fn)(void *), void *env) {
    jmp_buf here;
    jmp_buf *outer = g_jmp;
    const char *file = g_at_file;
    int line = g_at_line;
    g_jmp = &here;
    if (setjmp(here) == 0) {
        fn(env);
        g_jmp = outer;
        g_at_file = file;
        g_at_line = line;
        return 0;
    }
    g_jmp = outer;
    g_at_file = file;
    g_at_line = line;
    if (g_jmp_kind == 2 && outer) longjmp(*outer, 1);
    return 1;
}

/* Runs the registered tests (the first program argument selects those whose name contains it).
 * The exit code is 1 when a test failed. */
int32_t zevic_test_main(void) {
    const char *filter = g_argc > 1 ? g_argv[1] : NULL;
    int total = 0;
    for (int i = 0; i < g_ntests; i++)
        if (!filter || strstr(g_tests[i].name, filter)) total++;
    printf("running %d test%s\n", total, total == 1 ? "" : "s");
    fflush(stdout);
    int passed = 0, failed = 0;
    for (int i = 0; i < g_ntests; i++) {
        test_case *t = &g_tests[i];
        if (filter && !strstr(t->name, filter)) continue;
        jmp_buf here;
        g_jmp = &here;
        g_jmp_kind = 0;
        int ok = 1;
        if (setjmp(here) == 0) {
            t->fn();
        } else {
            ok = 0;
        }
        g_jmp = NULL;
        fflush(stdout);
        if (ok) {
            printf("  ok    %s\n", t->name);
            passed++;
        } else {
            printf("  FAIL  %s\n        %s\n", t->name, g_fail);
            failed++;
        }
        fflush(stdout);
    }
    printf("\n%d passed, %d failed\n", passed, failed);
    fflush(stdout);
    return failed ? 1 : 0;
}

/* ---------------------------------------------------------------------------------------------
 * Threads, mutexes and condition variables (Zevic.Async). The collector is built with GC_THREADS, so
 * threads started here are known to it. `fn(env)` is a Zevic closure: the code pointer and its environment.
 * ------------------------------------------------------------------------------------------- */
#ifdef _WIN32
typedef struct { HANDLE h; } zevic_thread;
typedef struct { void (*fn)(void *); void *env; } zevic_thread_start_info;

static DWORD WINAPI zevic_thread_main(LPVOID p) {
    zevic_thread_start_info *si = p;
    si->fn(si->env);
    return 0;
}

void zevic_thread_start(void (*fn)(void *), void *env) {
    zevic_thread *t = malloc(sizeof *t);
    zevic_thread_start_info *si = GC_malloc_uncollectable(sizeof *si);
    si->fn = fn;
    si->env = env;
    t->h = CreateThread(NULL, 0, zevic_thread_main, si, 0, NULL);
    if (t->h) CloseHandle(t->h); /* detached: a task reports its end through its own result slot */
    free(t);
}

void *zevic_mutex_new(void) {
    CRITICAL_SECTION *m = malloc(sizeof *m);
    InitializeCriticalSection(m);
    return m;
}
void zevic_mutex_lock(void *m) { EnterCriticalSection(m); }
void zevic_mutex_unlock(void *m) { LeaveCriticalSection(m); }

void *zevic_cond_new(void) {
    CONDITION_VARIABLE *c = malloc(sizeof *c);
    InitializeConditionVariable(c);
    return c;
}
void zevic_cond_wait(void *c, void *m) { SleepConditionVariableCS(c, m, INFINITE); }
void zevic_cond_broadcast(void *c) { WakeAllConditionVariable(c); }
#else
#include <pthread.h>

typedef struct { void (*fn)(void *); void *env; } zevic_thread_start_info;

static void *zevic_thread_main(void *p) {
    zevic_thread_start_info *si = p;
    void (*fn)(void *) = si->fn;
    void *env = si->env;
    fn(env);
    return NULL;
}

void zevic_thread_start(void (*fn)(void *), void *env) {
    pthread_t t;
    zevic_thread_start_info *si = GC_malloc(sizeof *si);
    si->fn = fn;
    si->env = env;
    if (pthread_create(&t, NULL, zevic_thread_main, si) != 0) {
        fprintf(stderr, "panic: cannot start a thread\n");
        exit(101);
    }
    pthread_detach(t); /* a task reports its end through its own result slot */
}

void *zevic_mutex_new(void) {
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutex_init(m, NULL);
    return m;
}
void zevic_mutex_lock(void *m) { pthread_mutex_lock(m); }
void zevic_mutex_unlock(void *m) { pthread_mutex_unlock(m); }

void *zevic_cond_new(void) {
    pthread_cond_t *c = malloc(sizeof *c);
    pthread_cond_init(c, NULL);
    return c;
}
void zevic_cond_wait(void *c, void *m) { pthread_cond_wait(c, m); }
void zevic_cond_broadcast(void *c) { pthread_cond_broadcast(c); }
#endif
