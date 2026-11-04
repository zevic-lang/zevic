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

/* On Windows a path is written with `/` only: git and friends misread a mix like C:\\Users\\x/git/y. */
static char *slashes(const char *p) {
    char *r = zstrdup(p);
#ifdef _WIN32
    for (char *c = r; *c; c++)
        if (*c == '\\') *c = '/';
#endif
    return r;
}

char *path_join(const char *a0, const char *b0) {
    const char *a = a0 ? slashes(a0) : NULL;
    const char *b = slashes(b0);
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

/* remove() on a file; on Windows a read-only file (git writes its objects that way) is made writable first */
static bool remove_file(const char *path) {
    if (remove(path) == 0) return true;
#ifdef _WIN32
    if (errno == EACCES && SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL) && remove(path) == 0) return true;
#endif
    return errno == ENOENT;
}

bool os_rmtree(const char *path) {
    if (!os_is_dir(path)) return remove_file(path);
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

#ifdef _WIN32
/* The command line of a Windows program, quoted the way the C runtime reads it back. */
static char *win_command_line(Vec *argv) {
    SB sb = {0};
    for (int i = 0; i < argv->n; i++) {
        if (i) sb_putc(&sb, ' ');
        const char *a = argv->v[i];
        sb_putc(&sb, '"');
        int backslashes = 0;
        for (; *a; a++) {
            if (*a == '\\') {
                backslashes++;
                continue;
            }
            /* backslashes only matter in front of a quote: each is written twice, and the quote gets one more */
            for (int k = 0; k < backslashes; k++) sb_putc(&sb, '\\');
            if (*a == '"') {
                for (int k = 0; k < backslashes; k++) sb_putc(&sb, '\\');
                sb_putc(&sb, '\\');
            }
            backslashes = 0;
            sb_putc(&sb, *a);
        }
        for (int k = 0; k < 2 * backslashes; k++) sb_putc(&sb, '\\'); /* in front of the closing quote */
        sb_putc(&sb, '"');
    }
    return sb_str(&sb);
}

/* Starts a program without cmd.exe and waits. `out` is its standard output (NULL: ours). Returns the exit code,
 * 127 when the program does not exist, -1 when it cannot be started. */
static int win_run(Vec *argv, const char *dir, HANDLE out) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out ? out : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    SetHandleInformation(si.hStdInput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(si.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(si.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    char *line = win_command_line(argv);
    BOOL ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, dir, &si, &pi);
    DWORD last = GetLastError();
    if (!ok) return (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND) ? 127 : -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}
#endif

int os_run(Vec *argv, const char *dir) {
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    return win_run(argv, dir, NULL);
#else
    char *cmd = command_line(argv, dir);
    return exit_code(system(cmd));
#endif
}

int os_run_capture(Vec *argv, const char *dir, char **out) {
    fflush(stdout);
    fflush(stderr);
    SB sb = {0};
    int rc;
#ifdef _WIN32
    /* the output goes to a temporary file, so a program that prints a lot cannot block on a full pipe */
    char tdir[MAX_PATH], tname[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, tdir) || !GetTempFileNameA(tdir, "zpm", 0, tname)) return -1;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    HANDLE h = CreateFileA(tname, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    rc = win_run(argv, dir, h);
    CloseHandle(h);
    FILE *f = fopen(tname, "rb");
    if (f) {
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_putn(&sb, buf, n);
        fclose(f);
    }
    remove(tname);
#else
    char *cmd = command_line(argv, dir);
    FILE *p = POPEN(cmd, "r");
    if (!p) return -1;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) sb_putn(&sb, buf, n);
    rc = exit_code(PCLOSE(p));
#endif
    char *s = sb_str(&sb);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) s[--len] = 0;
    *out = s;
    return rc;
}
