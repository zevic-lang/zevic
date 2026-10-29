#define _POSIX_C_SOURCE 200809L
#include "util.h"

#include <sys/stat.h>

#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

void *zalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

void *zrealloc(void *p, size_t old, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    if (n > old) memset((char *)q + old, 0, n - old);
    return q;
}

char *zstrndup(const char *s, size_t n) {
    char *p = zalloc(n + 1);
    memcpy(p, s, n);
    return p;
}

char *zstrdup(const char *s) {
    return zstrndup(s, strlen(s));
}

char *zvsprintf(const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    char *p = zalloc((size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    return p;
}

char *zsprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = zvsprintf(fmt, ap);
    va_end(ap);
    return s;
}

static void sb_reserve(SB *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t nc = sb->cap ? sb->cap : 64;
    while (nc < sb->len + extra + 1) nc *= 2;
    sb->p = zrealloc(sb->p, sb->cap, nc);
    sb->cap = nc;
}

void sb_putc(SB *sb, char c) {
    sb_reserve(sb, 1);
    sb->p[sb->len++] = c;
    sb->p[sb->len] = 0;
}

void sb_putn(SB *sb, const char *s, size_t n) {
    sb_reserve(sb, n);
    memcpy(sb->p + sb->len, s, n);
    sb->len += n;
    sb->p[sb->len] = 0;
}

void sb_puts(SB *sb, const char *s) {
    sb_putn(sb, s, strlen(s));
}

void sb_printf(SB *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = zvsprintf(fmt, ap);
    va_end(ap);
    sb_puts(sb, s);
}

char *sb_str(SB *sb) {
    sb_reserve(sb, 0);
    return sb->p;
}

void vec_push(Vec *v, void *x) {
    if (v->n == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        v->v = zrealloc(v->v, sizeof(void *) * (size_t)v->cap, sizeof(void *) * (size_t)nc);
        v->cap = nc;
    }
    v->v[v->n++] = x;
}

/* ---- source files ---- */

static Vec g_names, g_texts;

int src_add(const char *name, char *text) {
    vec_push(&g_names, zstrdup(name));
    vec_push(&g_texts, text);
    return g_names.n - 1;
}

const char *src_name(int file) {
    return (file >= 0 && file < g_names.n) ? g_names.v[file] : "<unknown>";
}

const char *src_text(int file) {
    return (file >= 0 && file < g_texts.n) ? g_texts.v[file] : "";
}

/* ---- diagnostics ---- */

int zv_error_count;
int zv_warn_count;

static void print_source_line(Pos pos) {
    const char *text = src_text(pos.file);
    if (!text || pos.line < 1) return;
    const char *p = text;
    for (int l = 1; l < pos.line && *p; p++)
        if (*p == '\n') l++;
    const char *e = p;
    while (*e && *e != '\n' && *e != '\r') e++;
    fprintf(stderr, "  %.*s\n  ", (int)(e - p), p);
    for (int i = 1; i < pos.col; i++) {
        char c = (i - 1 < (int)(e - p)) ? p[i - 1] : ' ';
        fputc(c == '\t' ? '\t' : ' ', stderr);
    }
    fputs("^\n", stderr);
}

static void vdiag(const char *kind, Pos pos, const char *fmt, va_list ap) {
    char *msg = zvsprintf(fmt, ap);
    fprintf(stderr, "%s:%d:%d: %s: %s\n", src_name(pos.file), pos.line, pos.col, kind, msg);
    print_source_line(pos);
}

void diag_error(Pos pos, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vdiag("error", pos, fmt, ap);
    va_end(ap);
    zv_error_count++;
}

void diag_warn(Pos pos, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vdiag("warning", pos, fmt, ap);
    va_end(ap);
    zv_warn_count++;
}

void diag_note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = zvsprintf(fmt, ap);
    va_end(ap);
    fprintf(stderr, "  note: %s\n", msg);
}

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = zvsprintf(fmt, ap);
    va_end(ap);
    fprintf(stderr, "zevic: error: %s\n", msg);
    exit(2);
}

/* ---- files ---- */

char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = zalloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
