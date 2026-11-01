#define _POSIX_C_SOURCE 200809L
#include "os.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define POPEN _popen
#define PCLOSE _pclose
#else
#include <dirent.h>
#include <unistd.h>
#define POPEN popen
#define PCLOSE pclose
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

/* ---- paths (always written with `/`; Windows accepts it too) ---- */

char *path_join(const char *a, const char *b) {
    if (!a || !*a || strcmp(a, ".") == 0) return zstrdup(b);
    size_t n = strlen(a);
    if (a[n - 1] == '/' || a[n - 1] == '\\') return zsprintf("%s%s", a, b);
    return zsprintf("%s/%s", a, b);
}

const char *path_base(const char *path) {
    const char *b = path;
    for (const char *c = path; *c; c++)
        if ((*c == '/' || *c == '\\') && c[1]) b = c + 1;
    return b;
}

char *path_dir(const char *path) {
    const char *last = NULL;
    for (const char *c = path; *c; c++)
        if (*c == '/' || *c == '\\') last = c;
    if (!last) return zstrdup(".");
    if (last == path) return zstrdup("/");
    return zstrndup(path, (size_t)(last - path));
}

/* ---- files ---- */

bool os_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool os_is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool make_dir(const char *path) {
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0777) == 0 || errno == EEXIST;
#endif
}

bool os_mkdirs(const char *path) {
    char *p = zstrdup(path);
    for (char *c = p + 1; *c; c++) {
        if (*c == '/' || *c == '\\') {
            char saved = *c;
            *c = 0;
            if (!os_is_dir(p) && !make_dir(p)) return false;
            *c = saved;
        }
    }
    return os_is_dir(p) || make_dir(p);
}

bool os_write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

bool os_rename(const char *from, const char *to) {
    return rename(from, to) == 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The names in a directory (without . and ..), sorted. */
static void list_dir(const char *dir, Vec *names) {
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path_join(dir, "*"), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) vec_push(names, zstrdup(fd.cFileName));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) vec_push(names, zstrdup(e->d_name));
    }
    closedir(d);
#endif
    if (names->n > 1) qsort(names->v, (size_t)names->n, sizeof(void *), cmp_str);
}

static bool has_ext(const char *name, const char *const *exts) {
    size_t n = strlen(name);
    for (int i = 0; exts[i]; i++) {
        size_t e = strlen(exts[i]);
        if (n >= e && strcmp(name + n - e, exts[i]) == 0) return true;
    }
    return false;
}

void os_walk(const char *dir, const char *const *exts, Vec *out) {
    Vec names = {0};
    list_dir(dir, &names);
    for (int i = 0; i < names.n; i++) {
        char *full = path_join(dir, names.v[i]);
        if (os_is_dir(full)) os_walk(full, exts, out);
        else if (has_ext(names.v[i], exts)) vec_push(out, full);
    }
}

bool os_rmtree(const char *path) {
    if (!os_is_dir(path)) return remove(path) == 0 || errno == ENOENT;
    Vec names = {0};
    list_dir(path, &names);
    bool ok = true;
    for (int i = 0; i < names.n; i++) ok = os_rmtree(path_join(path, names.v[i])) && ok;
#ifdef _WIN32
    /* a read-only file (git objects) cannot be removed; make the directory removable first */
    return (_rmdir(path) == 0) && ok;
#else
    return (rmdir(path) == 0) && ok;
#endif
}

/* ---- the system ---- */

char *os_exe_dir(void) {
    char buf[4096];
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
    if (n > 0 && n < sizeof buf) return path_dir(zstrndup(buf, n));
#elif defined(__APPLE__)
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) == 0) {
        char *real = realpath(buf, NULL);
        return path_dir(real ? real : buf);
    }
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        return path_dir(buf);
    }
#endif
    return zstrdup(".");
}

char *os_home_dir(void) {
    const char *h = getenv("HOME");
    if (!h || !*h) h = getenv("USERPROFILE");
    return h && *h ? zstrdup(h) : NULL;
}

bool os_find_in_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path) return false;
#ifdef _WIN32
    const char sep = ';';
    const char *suffix = ".exe";
#else
    const char sep = ':';
    const char *suffix = "";
#endif
    const char *s = path;
    while (*s) {
        const char *e = strchr(s, sep);
        size_t n = e ? (size_t)(e - s) : strlen(s);
        char *dir = zstrndup(s, n);
        if (os_is_file(zsprintf("%s/%s%s", dir, name, suffix))) return true;
        if (!e) break;
        s = e + 1;
    }
    return false;
}

static char *quote(const char *s) {
    SB sb = {0};
#ifdef _WIN32
    sb_putc(&sb, '"');
    sb_puts(&sb, s);
    sb_putc(&sb, '"');
#else
    sb_putc(&sb, '\'');
    for (; *s; s++) {
        if (*s == '\'') sb_puts(&sb, "'\\''");
        else sb_putc(&sb, *s);
    }
    sb_putc(&sb, '\'');
#endif
    return sb_str(&sb);
}

static char *command_line(Vec *argv, const char *dir) {
    SB sb = {0};
    if (dir) {
#ifdef _WIN32
        sb_printf(&sb, "cd /d %s && ", quote(dir));
#else
        sb_printf(&sb, "cd %s && ", quote(dir));
#endif
    }
    for (int i = 0; i < argv->n; i++) {
        if (i) sb_putc(&sb, ' ');
        char *arg = argv->v[i];
#ifdef _WIN32
        if (i == 0) { /* cmd.exe reads `./x.exe` as a switch; the program path needs backslashes */
            arg = zstrdup(arg);
            for (char *c = arg; *c; c++)
                if (*c == '/') *c = '\\';
        }
#endif
        sb_puts(&sb, quote(arg));
    }
    return sb_str(&sb);
}

static int exit_code(int rc) {
#ifdef _WIN32
    return rc;
#else
    if (rc == -1) return -1;
    if ((rc & 0x7f) == 0) return (rc >> 8) & 0xff;
    return 128 + (rc & 0x7f); /* killed by a signal */
#endif
}

int os_run(Vec *argv, const char *dir) {
    char *cmd = command_line(argv, dir);
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    cmd = zsprintf("\"%s\"", cmd); /* cmd.exe strips one pair of outer quotes */
#endif
    return exit_code(system(cmd));
}

int os_run_capture(Vec *argv, const char *dir, char **out) {
    char *cmd = command_line(argv, dir);
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    cmd = zsprintf("\"%s\"", cmd);
#endif
    FILE *p = POPEN(cmd, "r");
    if (!p) return -1;
    SB sb = {0};
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) sb_putn(&sb, buf, n);
    int rc = PCLOSE(p);
    char *s = sb_str(&sb);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) s[--len] = 0;
    *out = s;
    return exit_code(rc);
}
