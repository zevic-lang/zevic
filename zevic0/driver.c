#define _POSIX_C_SOURCE 200809L
#include "driver.h"

#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#define SEP '\\'
#else
#include <unistd.h>
#define SEP '/'
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

/* ---- the running executable ---- */

static char *exe_path(const char *argv0) {
#ifdef _WIN32
    char buf[4096];
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
    if (n > 0 && n < sizeof buf) return zstrndup(buf, n);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) == 0) {
        char *real = realpath(buf, NULL);
        if (real) return real;
        return zstrdup(buf);
    }
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) return zstrndup(buf, (size_t)n);
#endif
    return zstrdup(argv0 ? argv0 : "zevic");
}

static char *dir_of(const char *path) {
    const char *last = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') last = p;
    if (!last) return zstrdup(".");
    return zstrndup(path, (size_t)(last - path));
}

static bool is_home(const char *dir) {
    char *rt = zsprintf("%s/runtime/zevic_rt.c", dir);
    char *sd = zsprintf("%s/stdlib", dir);
    return file_exists(rt) && dir_exists(sd);
}

char *driver_find_home(const char *argv0) {
    const char *env = getenv("ZEVIC_HOME");
    if (env && *env && is_home(env)) return zstrdup(env);
    char *dir = dir_of(exe_path(argv0));
    char *cands[] = {zsprintf("%s/../share/zevic", dir), zsprintf("%s/..", dir), zsprintf("%s", dir), zstrdup(".")};
    for (int i = 0; i < 4; i++)
        if (is_home(cands[i])) return cands[i];
    return NULL;
}

/* ---- running commands ---- */

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

static int run(const char *cmd) {
#ifdef _WIN32
    char *full = zsprintf("\"%s\"", cmd); /* cmd.exe strips one pair of outer quotes */
    int rc = system(full);
#else
    int rc = system(cmd);
    if (rc != -1 && (rc & 0x7f) == 0) rc = (rc >> 8) & 0xff;
#endif
    return rc;
}

/* Run `cmd` and capture the first line of its output. */
static char *first_line(const char *cmd) {
    FILE *p = POPEN(cmd, "r");
    if (!p) return NULL;
    char buf[512];
    char *res = NULL;
    if (fgets(buf, sizeof buf, p)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        res = zstrdup(buf);
    }
    PCLOSE(p);
    return res;
}

/* ---- clang ---- */

/* Returns the major version, 0 when unknown, -1 when it does not run. */
static int clang_major(const char *path) {
#ifdef _WIN32
    char *cmd = zsprintf("\"%s --version 2>NUL\"", quote(path));
#else
    char *cmd = zsprintf("%s --version 2>/dev/null", quote(path));
#endif
    char *line = first_line(cmd);
    if (!line) return -1;
    if (strstr(line, "Apple clang")) return 99; /* Apple's numbering differs; it is recent enough */
    char *v = strstr(line, "clang version ");
    if (!v) return -1;
    return atoi(v + 14);
}

static void print_install_help(const char *why) {
    fprintf(stderr, "error: %s\n", why);
    fprintf(stderr,
            "zevic needs clang 15 or newer to build programs. Install LLVM with one of:\n"
            "  Ubuntu / Debian:  sudo apt install clang\n"
            "  Windows:          winget install LLVM.LLVM\n"
            "  macOS:            brew install llvm\n"
            "Then run zevic again. If clang is installed somewhere else, set ZEVIC_CLANG\n"
            "to its path or pass --cc <path>.\n");
}

char *driver_find_clang(const char *cli_cc) {
    Vec cands = {0};
    if (cli_cc) vec_push(&cands, (void *)cli_cc);
    const char *env = getenv("ZEVIC_CLANG");
    if (env && *env) vec_push(&cands, (void *)env);
    vec_push(&cands, "clang");
#ifdef _WIN32
    vec_push(&cands, "C:\\Program Files\\LLVM\\bin\\clang.exe");
    vec_push(&cands, "C:\\Program Files (x86)\\LLVM\\bin\\clang.exe");
#else
    vec_push(&cands, "/opt/homebrew/opt/llvm/bin/clang");
    vec_push(&cands, "/usr/local/opt/llvm/bin/clang");
    for (int v = 22; v >= 15; v--) vec_push(&cands, zsprintf("/usr/lib/llvm-%d/bin/clang", v));
#endif
    int found_old = 0;
    for (int i = 0; i < cands.n; i++) {
        const char *c = cands.v[i];
        int m = clang_major(c);
        if (m >= 15 || m == 0) return zstrdup(c);
        if (m > 0 && m < 15) found_old = m;
        if (cli_cc && i == 0) {
            print_install_help(zsprintf("`%s` is not a working clang", cli_cc));
            return NULL;
        }
    }
    if (found_old) print_install_help(zsprintf("clang %d was found, but clang 15 or newer is required", found_old));
    else print_install_help("clang was not found");
    return NULL;
}

static bool is_cpp_file(const char *f) {
    size_t n = strlen(f);
    return (n > 4 && strcmp(f + n - 4, ".cpp") == 0) || (n > 3 && strcmp(f + n - 3, ".cc") == 0) ||
           (n > 4 && strcmp(f + n - 4, ".cxx") == 0);
}

/* clang++ next to clang: "clang" -> "clang++", "clang.exe" -> "clang++.exe", "clang-18" -> "clang++-18". */
static char *sibling_cxx(const char *clang) {
    const char *exe = "";
    size_t n = strlen(clang);
    if (n > 4 && strcmp(clang + n - 4, ".exe") == 0) {
        exe = ".exe";
        n -= 4;
    }
    const char *dash = NULL; /* a version suffix such as -18 */
    for (size_t i = n; i > 0 && clang[i - 1] != '/' && clang[i - 1] != '\\'; i--)
        if (clang[i - 1] == '-') {
            dash = clang + i - 1;
            break;
        }
    size_t stem = dash ? (size_t)(dash - clang) : n;
    return zsprintf("%.*s++%.*s%s", (int)stem, clang, (int)(n - stem), clang + stem, exe);
}

int driver_build(const char *clang, const char *home, const char *ll_path, const char *out_path, BuildOpts *o) {
    SB cmd = {0};
    bool has_cpp = false;
    for (int i = 0; i < o->inputs.n; i++)
        if (is_cpp_file(o->inputs.v[i])) has_cpp = true;
    /* C++ code needs the C++ driver, so the standard library gets linked */
    const char *driver = has_cpp ? (o->cxx ? o->cxx : sibling_cxx(clang)) : clang;
    sb_puts(&cmd, quote(driver));
    sb_printf(&cmd, " %s -w -Wno-override-module", o->opt ? o->opt : "-O1");
    if (o->debug) sb_puts(&cmd, " -g");
    sb_printf(&cmd, " %s", quote(ll_path));
    /* the runtime is C even when the driver is clang++ */
    sb_printf(&cmd, " -x c %s -x none", quote(zsprintf("%s/runtime/zevic_rt.c", home)));
    for (int i = 0; i < o->inputs.n; i++) {
        const char *f = o->inputs.v[i];
        size_t fl = strlen(f);
        bool is_c = fl > 2 && strcmp(f + fl - 2, ".c") == 0;
        /* clang++ would read a .c file as C++ */
        if (is_c && has_cpp) sb_printf(&cmd, " -x c %s -x none", quote(f));
        else sb_printf(&cmd, " %s", quote(f));
    }
    for (int i = 0; i < o->incdirs.n; i++) sb_printf(&cmd, " -I%s", quote(o->incdirs.v[i]));
    for (int i = 0; i < o->defines.n; i++) sb_printf(&cmd, " -D%s", quote(o->defines.v[i]));
    for (int i = 0; i < o->libdirs.n; i++) sb_printf(&cmd, " -L%s", quote(o->libdirs.v[i]));
    for (int i = 0; i < o->libs.n; i++) sb_printf(&cmd, " -l%s", (char *)o->libs.v[i]);
    sb_printf(&cmd, " -o %s", quote(out_path));
#ifndef _WIN32
    sb_puts(&cmd, " -lm");
#endif
    return run(sb_str(&cmd));
}

void driver_remove(const char *path) {
    remove(path);
}
