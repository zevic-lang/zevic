/* zevpm - the package and build manager for Zevic */
#define _POSIX_C_SOURCE 200809L
#include "deps.h"
#include "os.h"
#include "toml.h"

#include <ctype.h>

#ifndef ZEVPM_VERSION
#define ZEVPM_VERSION "dev"
#endif

#ifdef _WIN32
#define EXE ".exe"
#else
#define EXE ""
#endif

static void usage(FILE *to) {
    fputs("usage: zevpm <command> [options]\n"
          "\n"
          "  new <name> [--lib]   make a new package in a new directory\n"
          "  init [--lib]         make the current directory a package\n"
          "  build [--release]    build the package into build/\n"
          "  run [--release] [-- args]\n"
          "                       build and run it\n"
          "  test [filter]        run the tests (those whose name contains `filter`)\n"
          "  fmt                  format the sources (needs zevfmt)\n"
          "  clean                remove build/\n"
          "  get <repo>           add a dependency: owner/name, github.com/owner/name or a git address,\n"
          "                       with @tag, @branch or @commit at the end\n"
          "  remove <name>        remove a dependency\n"
          "  update [name]        move the dependencies to their newest commits\n"
          "  --version, --help\n",
          to);
}

static void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = zvsprintf(fmt, ap);
    va_end(ap);
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

/* ---- finding zevic ---- */

static char *find_zevic(void) {
    const char *env = getenv("ZEVIC");
    if (env && *env) return zstrdup(env);
    char *sibling = path_join(os_exe_dir(), "zevic" EXE);
    if (os_is_file(sibling)) return sibling;
    if (os_find_in_path("zevic")) return zstrdup("zevic");
    fprintf(stderr, "error: cannot find `zevic` (it belongs next to zevpm, or on PATH; or set ZEVIC)\n");
    exit(2);
}

/* ">=0.2" against the version zevic prints. */
static void check_zevic_version(const char *zevic, Manifest *m) {
    if (!m->zevic_req) return;
    Vec a = {0};
    vec_push(&a, (void *)zevic);
    vec_push(&a, "--version");
    char *out = NULL;
    if (os_run_capture(&a, NULL, &out) != 0 || !out) fatal("cannot run `%s --version`", zevic);
    const char *v = strchr(out, ' ');
    /* the standard library is the package `Zevic`: its version is what `zevic = ">=..."` asks for */
    Vec b = {0};
    vec_push(&b, (void *)zevic);
    vec_push(&b, "--print-stdlib");
    char *sd = NULL;
    if (os_run_capture(&b, NULL, &sd) == 0 && sd) {
        size_t n = strlen(sd);
        while (n && (sd[n - 1] == '\n' || sd[n - 1] == '\r')) sd[--n] = 0;
        Manifest *zm = manifest_load(sd);
        if (zm && zm->version) v = zm->version - 1;
    }
    int have[3] = {0, 0, 0}, want[3] = {0, 0, 0};
    if (!v || sscanf(v + 1, "%d.%d.%d", &have[0], &have[1], &have[2]) < 2) return; /* a development build */
    const char *r = m->zevic_req;
    if (strncmp(r, ">=", 2) != 0) fatal("%s: `zevic = \"%s\"` must look like \">=0.2\"", m->file, r);
    if (sscanf(r + 2, "%d.%d.%d", &want[0], &want[1], &want[2]) < 1) fatal("%s: `zevic = \"%s\"` must look like \">=0.2\"", m->file, r);
    for (int i = 0; i < 3; i++) {
        if (have[i] > want[i]) return;
        if (have[i] < want[i]) {
            fprintf(stderr, "error: %s needs zevic %s, but this is zevic %d.%d.%d\n", m->name, r, have[0], have[1], have[2]);
            exit(1);
        }
    }
}

/* ---- the package directory ---- */

/* The manifest of the package we are in (the current directory). */
static Manifest *load_here(void) {
    return manifest_load(".");
}

static void add_sources(Vec *zev, Vec *native, const char *pkg_dir, bool with_tests) {
    static const char *const zevx[] = {".zev", NULL};
    static const char *const nativex[] = {".c", ".cpp", ".cc", NULL};
    os_walk(path_join(pkg_dir, "src"), zevx, zev);
    if (with_tests) os_walk(path_join(pkg_dir, "tests"), zevx, zev);
    os_walk(path_join(pkg_dir, "native"), nativex, native);
}

static void add_lib(Vec *libs, const char *name) {
    for (int i = 0; i < libs->n; i++)
        if (strcmp(libs->v[i], name) == 0) return;
    vec_push(libs, (void *)name);
}

typedef enum { B_NORMAL, B_TEST } BuildMode;

/* Builds the package; returns the path of what was made. */
static char *build(Manifest *m, bool release, BuildMode mode, const char *update) {
    char *zevic = find_zevic();
    check_zevic_version(zevic, m);
    Vec pkgs = {0};
    deps_resolve(m, update, &pkgs);

    Vec zev = {0}, native = {0}, libs = {0};
    add_sources(&zev, &native, m->dir, mode == B_TEST);
    for (int i = 0; i < m->libs.n; i++) add_lib(&libs, m->libs.v[i]);
    for (int i = 0; i < pkgs.n; i++) {
        Pkg *p = pkgs.v[i];
        add_sources(&zev, &native, p->dir, false);
        for (int k = 0; k < p->m->libs.n; k++) add_lib(&libs, p->m->libs.v[k]);
    }
    bool is_lib = strcmp(m->kind, "lib") == 0 && mode == B_NORMAL;
    if (zev.n == 0) fatal("there are no .zev files in %s", path_join(m->dir, "src"));

    os_mkdirs("build");
    char *out;
    if (mode == B_TEST) out = zsprintf("build/%s-test" EXE, m->name);
    else if (is_lib)
#ifdef _WIN32
        out = zsprintf("build/%s.lib", m->name);
#else
        out = zsprintf("build/lib%s.a", m->name);
#endif
    else out = zsprintf("build/%s" EXE, m->name);

    Vec a = {0};
    vec_push(&a, zevic);
    for (int i = 0; i < zev.n; i++) vec_push(&a, zev.v[i]);
    for (int i = 0; i < native.n; i++) vec_push(&a, native.v[i]);
    if (release) vec_push(&a, "-O2");
    for (int i = 0; i < libs.n; i++) vec_push(&a, zsprintf("-l%s", (char *)libs.v[i]));
    if (mode == B_TEST) vec_push(&a, "--test");
    if (is_lib) {
        os_mkdirs("build/include");
        vec_push(&a, "--lib");
        vec_push(&a, "--emit-header");
        vec_push(&a, zsprintf("build/include/%s.h", m->name));
    }
    vec_push(&a, "-o");
    vec_push(&a, out);
    int rc = os_run(&a, NULL);
    if (rc != 0) exit(rc == 1 ? 1 : 2);
    return out;
}

/* ---- new and init ---- */

static char *pascal_name(const char *dir_name) {
    SB sb = {0};
    bool up = true;
    for (const char *p = dir_name; *p; p++) {
        if (isalnum((unsigned char)*p)) {
            sb_putc(&sb, up ? (char)toupper((unsigned char)*p) : *p);
            up = false;
        } else {
            up = true; /* `-` and `_` start a new word */
        }
    }
    char *s = sb_str(&sb);
    if (!is_pascal_case(s)) fatal("cannot make a package name (PascalCase) out of `%s`", dir_name);
    return s;
}

static void write_new(const char *dir, const char *name, bool lib, bool must_be_new_files) {
    os_mkdirs(path_join(dir, "src"));
    const char *major_minor = "0.1";
    char *toml = zsprintf("[package]\nname = \"%s\"\nversion = \"0.1.0\"\nkind = \"%s\"\nzevic = \">=%s\"\n", name,
                          lib ? "lib" : "bin", major_minor);
    char *main_file = path_join(dir, lib ? zsprintf("src/%s.zev", name) : "src/Main.zev");
    char *main_text;
    if (lib)
        main_text = zsprintf("module %s\n\n"
                             "import Zevic.Test\n\n"
                             "// Say hello to `who`.\n"
                             "fn Greet(who: String) -> String {\n"
                             "    \"Hello, ${who}!\"\n"
                             "}\n\n"
                             "test \"Greet names the person\" {\n"
                             "    Test.Equal(Greet(\"Zevic\"), \"Hello, Zevic!\")\n"
                             "}\n",
                             name);
    else
        main_text = zsprintf("module Main\n\n"
                             "import Zevic.Console\n\n"
                             "fn Main() {\n"
                             "    Console.Println(\"Hello from %s!\")\n"
                             "}\n",
                             name);
    struct {
        char *path, *text;
    } files[] = {
        {path_join(dir, "zevic.toml"), toml},
        {main_file, main_text},
        {path_join(dir, ".gitignore"), "build/\n"},
    };
    for (int i = 0; i < 3; i++) {
        if (os_is_file(files[i].path)) {
            if (must_be_new_files) fatal("`%s` already exists", files[i].path);
            fprintf(stderr, "note: `%s` already exists; left as it is\n", files[i].path);
            continue;
        }
        if (!os_write_file(files[i].path, files[i].text)) {
            fprintf(stderr, "error: cannot write `%s`\n", files[i].path);
            exit(2);
        }
    }
}

static int cmd_new(int argc, char **argv) {
    const char *name = NULL;
    bool lib = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--lib") == 0) lib = true;
        else if (argv[i][0] == '-') fatal("unknown option `%s`", argv[i]);
        else if (!name) name = argv[i];
        else fatal("too many arguments");
    }
    if (!name) fatal("usage: zevpm new <name> [--lib]");
    if (os_is_dir(name) || os_is_file(name)) fatal("`%s` already exists", name);
    char *pkg = pascal_name(path_base(name));
    if (!os_mkdirs(name)) fatal("cannot create the directory `%s`", name);
    write_new(name, pkg, lib, true);
    printf("Created %s package `%s` in %s/\n", lib ? "library" : "program", pkg, name);
    printf("  cd %s && zevpm %s\n", name, lib ? "test" : "run");
    return 0;
}

static int cmd_init(int argc, char **argv) {
    bool lib = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--lib") == 0) lib = true;
        else fatal("unknown argument `%s`", argv[i]);
    }
    if (os_is_file("zevic.toml")) fatal("`zevic.toml` already exists here");
    char *cwd_name = ".";
    {
        /* the name of the current directory: ask the shell-free way */
        char *pwd = getenv("PWD");
        if (pwd && *pwd) cwd_name = zstrdup(path_base(pwd));
    }
    char *pkg = pascal_name(strcmp(cwd_name, ".") == 0 ? "App" : cwd_name);
    write_new(".", pkg, lib, false);
    printf("Made this directory the %s package `%s`\n", lib ? "library" : "program", pkg);
    return 0;
}

/* ---- the other commands ---- */

static int cmd_build(int argc, char **argv) {
    bool release = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--release") == 0) release = true;
        else fatal("unknown argument `%s`", argv[i]);
    }
    Manifest *m = load_here();
    char *out = build(m, release, B_NORMAL, NULL);
    printf("Built %s\n", out);
    return 0;
}

static int cmd_run(int argc, char **argv) {
    bool release = false;
    int i = 0;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--release") == 0) release = true;
        else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else fatal("unknown argument `%s` (put the program's own arguments after `--`)", argv[i]);
    }
    Manifest *m = load_here();
    if (strcmp(m->kind, "lib") == 0) fatal("`%s` is a library; there is nothing to run (try `zevpm test`)", m->name);
    char *out = build(m, release, B_NORMAL, NULL);
    Vec a = {0};
    vec_push(&a, path_join(".", out));
    for (; i < argc; i++) vec_push(&a, argv[i]);
    int rc = os_run(&a, NULL);
    if (rc < 0) fatal("cannot start `%s`", out);
    return rc;
}

static int cmd_test(int argc, char **argv) {
    const char *filter = NULL;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '-') fatal("unknown option `%s`", argv[i]);
        else if (!filter) filter = argv[i];
        else fatal("too many arguments (one name filter at most)");
    }
    Manifest *m = load_here();
    char *out = build(m, false, B_TEST, NULL);
    Vec a = {0};
    vec_push(&a, path_join(".", out));
    if (filter) vec_push(&a, (void *)filter);
    int rc = os_run(&a, NULL);
    if (rc < 0) fatal("cannot start `%s`", out);
    return rc;
}

static int cmd_clean(void) {
    load_here();
    if (os_is_dir("build") && !os_rmtree("build")) fatal("cannot remove build/");
    puts("Removed build/");
    return 0;
}

static int cmd_fmt(void) {
    load_here();
    char *fmt = path_join(os_exe_dir(), "zevfmt" EXE);
    if (!os_is_file(fmt)) {
        fprintf(stderr, "error: zevfmt is not available yet\n");
        return 2;
    }
    Vec files = {0}, a = {0};
    static const char *const zevx[] = {".zev", NULL};
    os_walk("src", zevx, &files);
    os_walk("tests", zevx, &files);
    vec_push(&a, fmt);
    vec_push(&a, "-w");
    for (int i = 0; i < files.n; i++) vec_push(&a, files.v[i]);
    return os_run(&a, NULL);
}

static int cmd_get(int argc, char **argv) {
    if (argc != 1) fatal("usage: zevpm get <repo>[@version]");
    Manifest *m = load_here();
    char *name = deps_get(m, argv[0]);
    printf("Added dependency `%s`\n", name);
    m = load_here();
    Vec pkgs = {0};
    deps_resolve(m, NULL, &pkgs);
    return 0;
}

static int cmd_remove(int argc, char **argv) {
    if (argc != 1) fatal("usage: zevpm remove <name>");
    Manifest *m = load_here();
    if (!deps_remove(m, argv[0])) fatal("`%s` is not a dependency of %s", argv[0], m->name);
    printf("Removed dependency `%s`\n", argv[0]);
    return 0;
}

static int cmd_update(int argc, char **argv) {
    if (argc > 1) fatal("usage: zevpm update [name]");
    Manifest *m = load_here();
    if (argc == 1 && !manifest_find_dep(m, argv[0])) {
        /* it may be a dependency of a dependency: the resolver finds it by name */
    }
    Vec pkgs = {0};
    deps_resolve(m, argc == 1 ? argv[0] : "", &pkgs);
    printf("Dependencies are up to date (%d package%s)\n", pkgs.n, pkgs.n == 1 ? "" : "s");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    const char *cmd = argv[1];
    int n = argc - 2;
    char **rest = argv + 2;
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
        printf("zevpm %s\n", ZEVPM_VERSION);
        return 0;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "new") == 0) return cmd_new(n, rest);
    if (strcmp(cmd, "init") == 0) return cmd_init(n, rest);
    if (strcmp(cmd, "build") == 0) return cmd_build(n, rest);
    if (strcmp(cmd, "run") == 0) return cmd_run(n, rest);
    if (strcmp(cmd, "test") == 0) return cmd_test(n, rest);
    if (strcmp(cmd, "clean") == 0) return cmd_clean();
    if (strcmp(cmd, "fmt") == 0) return cmd_fmt();
    if (strcmp(cmd, "get") == 0) return cmd_get(n, rest);
    if (strcmp(cmd, "remove") == 0) return cmd_remove(n, rest);
    if (strcmp(cmd, "update") == 0) return cmd_update(n, rest);
    fprintf(stderr, "error: unknown command `%s`\n\n", cmd);
    usage(stderr);
    return 1;
}
