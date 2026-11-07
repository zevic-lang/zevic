#define _POSIX_C_SOURCE 200809L
#include "util.h"

#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#include <wchar.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

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

/* ---- dictionary ---- */

static size_t hash_str(const char *s) {
    size_t h = 1469598103934665603ull;
    while (*s) h = (h ^ (unsigned char)*s++) * 1099511628211ull;
    return h;
}

static size_t dict_slot(const Dict *d, const char *key) {
    size_t i = hash_str(key) & (d->cap - 1);
    while (d->keys[i] && strcmp(d->keys[i], key) != 0) i = (i + 1) & (d->cap - 1);
    return i;
}

void *dict_get(const Dict *d, const char *key) {
    if (!d->cap) return NULL;
    size_t i = dict_slot(d, key);
    return d->keys[i] ? d->vals[i] : NULL;
}

bool dict_has(const Dict *d, const char *key) {
    if (!d->cap) return false;
    return d->keys[dict_slot(d, key)] != NULL;
}

void dict_put(Dict *d, const char *key, void *val) {
    if (d->cap == 0 || (d->n + 1) * 2 > d->cap) {
        Dict nd = {0};
        nd.cap = d->cap ? d->cap * 2 : 16;
        nd.keys = zalloc(sizeof(char *) * nd.cap);
        nd.vals = zalloc(sizeof(void *) * nd.cap);
        for (size_t i = 0; i < d->cap; i++) {
            if (d->keys[i]) {
                size_t j = dict_slot(&nd, d->keys[i]);
                nd.keys[j] = d->keys[i];
                nd.vals[j] = d->vals[i];
                nd.n++;
            }
        }
        *d = nd;
    }
    size_t i = dict_slot(d, key);
    if (!d->keys[i]) {
        d->keys[i] = zstrdup(key);
        d->n++;
    }
    d->vals[i] = val;
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

#ifdef _WIN32
static wchar_t *to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = zalloc(sizeof(wchar_t) * (size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *from_wide(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return zstrdup("");
    char *s = zalloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* a path that is not valid UTF-8 (an ANSI name from an old script) is tried as it is, with the ANSI functions */
FILE *zv_fopen(const char *path, const char *mode) {
    wchar_t *wp = to_wide(path), *wm = to_wide(mode);
    if (wp && wm) return _wfopen(wp, wm);
    return fopen(path, mode);
}

char *zv_getenv(const char *name) {
    wchar_t *wn = to_wide(name);
    if (!wn) return NULL;
    const wchar_t *v = _wgetenv(wn);
    return v ? from_wide(v) : NULL;
}

int zv_remove(const char *path) {
    wchar_t *w = to_wide(path);
    return w ? _wremove(w) : remove(path);
}

int zv_rename(const char *from, const char *to) {
    wchar_t *a = to_wide(from), *b = to_wide(to);
    return a && b ? _wrename(a, b) : rename(from, to);
}

int zv_mkdir(const char *path) {
    wchar_t *w = to_wide(path);
    return w ? _wmkdir(w) : _mkdir(path);
}

int zv_system(const char *cmd) {
    wchar_t *w = to_wide(cmd);
    return w ? _wsystem(w) : system(cmd);
}

FILE *zv_popen(const char *cmd, const char *mode) {
    wchar_t *w = to_wide(cmd), *m = to_wide(mode);
    return w && m ? _wpopen(w, m) : _popen(cmd, mode);
}

int zv_pclose(FILE *f) { return _pclose(f); }

/* The command line split by the rules of the Microsoft C runtime (quotes, and backslashes before quotes). Done here
 * because CommandLineToArgvW lives in shell32, which is not linked by default. */
void zv_utf8_args(int *argc, char ***argv) {
    const wchar_t *p = GetCommandLineW();
    if (!p) return;
    int cap = 8, n = 0;
    char **v = zalloc(sizeof(char *) * (size_t)cap);
    wchar_t *buf = zalloc(sizeof(wchar_t) * (wcslen(p) + 1));
    for (;;) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        size_t len = 0;
        bool quoted = false;
        for (;;) {
            size_t bs = 0;
            while (*p == L'\\') {
                bs++;
                p++;
            }
            if (*p == L'"') {
                for (size_t k = 0; k < bs / 2; k++) buf[len++] = L'\\';
                if (bs % 2) {
                    buf[len++] = L'"';
                } else if (quoted && p[1] == L'"') {
                    buf[len++] = L'"'; /* "" inside quotes is a quote */
                    p++;
                } else {
                    quoted = !quoted;
                }
                p++;
            } else {
                for (size_t k = 0; k < bs; k++) buf[len++] = L'\\';
                if (!*p || (!quoted && (*p == L' ' || *p == L'\t'))) break;
                buf[len++] = *p++;
            }
        }
        buf[len] = 0;
        if (n + 1 >= cap) {
            v = zrealloc(v, sizeof(char *) * (size_t)cap, sizeof(char *) * (size_t)cap * 2);
            cap *= 2;
        }
        v[n++] = from_wide(buf);
    }
    if (n <= 0) return;
    v[n] = NULL;
    *argc = n;
    *argv = v;
}

char *zv_exe_path(void) {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    if (n > 0 && n < sizeof buf / sizeof buf[0]) return from_wide(buf);
    return NULL;
}

static bool path_kind(const char *path, bool want_dir) {
    wchar_t *w = to_wide(path);
    struct _stat64 st;
    if (!w || _wstat64(w, &st) != 0) return false;
    return want_dir ? (st.st_mode & _S_IFMT) == _S_IFDIR : (st.st_mode & _S_IFMT) == _S_IFREG;
}
#else
FILE *zv_fopen(const char *path, const char *mode) { return fopen(path, mode); }
char *zv_getenv(const char *name) {
    const char *v = getenv(name);
    return v ? zstrdup(v) : NULL;
}
int zv_remove(const char *path) { return remove(path); }
int zv_rename(const char *from, const char *to) { return rename(from, to); }
int zv_mkdir(const char *path) { return mkdir(path, 0777); }
int zv_system(const char *cmd) { return system(cmd); }
FILE *zv_popen(const char *cmd, const char *mode) { return popen(cmd, mode); }
int zv_pclose(FILE *f) { return pclose(f); }
void zv_utf8_args(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
}

char *zv_exe_path(void) {
    char buf[4096];
#ifdef __APPLE__
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) == 0) {
        char *real = realpath(buf, NULL);
        return real ? real : zstrdup(buf);
    }
    return NULL;
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) return zstrndup(buf, (size_t)n);
    return NULL;
#endif
}

static bool path_kind(const char *path, bool want_dir) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return want_dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode);
}
#endif

char *read_file(const char *path, size_t *len) {
    FILE *f = zv_fopen(path, "rb");
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

bool file_exists(const char *path) { return path_kind(path, false); }

bool dir_exists(const char *path) { return path_kind(path, true); }
