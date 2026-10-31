/* main.c - command line entry point of zevic0 */
#include "ast.h"
#include "driver.h"
#include "gen.h"
#include "lexer.h"
#include "module.h"
#include "parser.h"
#include "util.h"

#define ZEVIC_VERSION "0.0.1"

static void usage(void) {
    fputs("usage: zevic <file.zev>... [native files] [-o output]\n"
          "\n"
          "  -o <file>          output file\n"
          "  --emit-ll          write the LLVM IR and stop\n"
          "  --emit-header <f>  also write a C header for the `export \"C\"` functions\n"
          "  -O0 -O1 -O2        optimisation level (default -O1)\n"
          "  -g                 debug information\n"
          "  -l<name> -L<dir> -I<dir> -D<k=v>   options passed to clang\n"
          "  --cc <path>        use this clang\n"
          "  --version          print the version\n"
          "\n"
          "Files ending in .c .cpp .o .a are linked into the program.\n",
          stderr);
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static char *dir_name(const char *path) {
    const char *last = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') last = p;
    return last ? zstrndup(path, (size_t)(last - path)) : zstrdup(".");
}

static char *base_no_ext(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    const char *dot = strrchr(b, '.');
    return dot ? zstrndup(b, (size_t)(dot - b)) : zstrdup(b);
}

/* ---- debugging dumps ---- */

static void dump_tokens(const char *path) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) die("cannot read %s", path);
    int file = src_add(path, text);
    TokenList tl = lex(file, text, len, 1, 1);
    for (int i = 0; i < tl.n; i++) {
        Token *t = &tl.v[i];
        printf("%d:%d %s", t->pos.line, t->pos.col, tok_name(t->k));
        switch (t->k) {
        case TK_IDENT: printf(" %s%s", t->text, t->quoted ? " (quoted)" : ""); break;
        case TK_INT: case TK_CHAR: case TK_BYTE:
            printf(" %llu", (unsigned long long)t->ival);
            if (t->suffix) printf(" _%s", t->suffix);
            break;
        case TK_FLOAT:
            printf(" %g", t->fval);
            if (t->suffix) printf(" _%s", t->suffix);
            break;
        case TK_STRING:
            for (int k = 0; k < t->nparts; k++) {
                StrPart *p = &t->parts[k];
                printf(" %s[", p->is_expr ? "expr" : "text");
                for (int j = 0; j < p->n; j++) {
                    unsigned char c = (unsigned char)p->s[j];
                    if (c == '\n') printf("\\n");
                    else if (c < 32) printf("\\x%02x", c);
                    else putchar(c);
                }
                printf("]");
            }
            break;
        default: break;
        }
        printf("\n");
    }
}

static void dump_ast(const char *path) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) die("cannot read %s", path);
    int file = src_add(path, text);
    SourceFile *sf = parse_file(file, text, len);
    if (!zv_error_count) ast_dump(sf, stdout);
}

int main(int argc, char **argv) {
    BuildOpts opts = {0};
    Vec zev_files = {0};
    const char *out = NULL;
    bool emit_ll = false;
    const char *header_path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--dump-tokens") == 0 && i + 1 < argc) {
            dump_tokens(argv[i + 1]);
            return zv_error_count ? 1 : 0;
        } else if (strcmp(a, "--dump-ast") == 0 && i + 1 < argc) {
            dump_ast(argv[i + 1]);
            return zv_error_count ? 1 : 0;
        } else if (strcmp(a, "--version") == 0) {
            printf("zevic %s\n", ZEVIC_VERSION);
            return 0;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(a, "--emit-header") == 0 && i + 1 < argc) {
            header_path = argv[++i];
        } else if (strcmp(a, "--emit-ll") == 0) {
            emit_ll = true;
        } else if (strcmp(a, "--keep-ll") == 0) {
            opts.keep_ll = true;
        } else if (strcmp(a, "-O0") == 0 || strcmp(a, "-O1") == 0 || strcmp(a, "-O2") == 0) {
            opts.opt = a;
        } else if (strcmp(a, "-g") == 0) {
            opts.debug = true;
        } else if (strcmp(a, "--cc") == 0 && i + 1 < argc) {
            opts.cc = argv[++i];
        } else if (strcmp(a, "--cxx") == 0 && i + 1 < argc) {
            opts.cxx = argv[++i];
        } else if (strncmp(a, "-l", 2) == 0 && a[2]) {
            vec_push(&opts.libs, (void *)(a + 2));
        } else if (strncmp(a, "-L", 2) == 0 && a[2]) {
            vec_push(&opts.libdirs, (void *)(a + 2));
        } else if (strncmp(a, "-I", 2) == 0 && a[2]) {
            vec_push(&opts.incdirs, (void *)(a + 2));
        } else if (strncmp(a, "-D", 2) == 0 && a[2]) {
            vec_push(&opts.defines, (void *)(a + 2));
        } else if (ends_with(a, ".zev")) {
            vec_push(&zev_files, (void *)a);
        } else if (ends_with(a, ".c") || ends_with(a, ".cpp") || ends_with(a, ".cc") || ends_with(a, ".o") ||
                   ends_with(a, ".a") || ends_with(a, ".obj") || ends_with(a, ".lib")) {
            vec_push(&opts.inputs, (void *)a);
        } else if (a[0] == '-') {
            fprintf(stderr, "error: unknown option `%s`\n", a);
            usage();
            return 1;
        } else {
            fprintf(stderr, "error: don't know what to do with `%s` (expected a .zev file)\n", a);
            return 1;
        }
    }
    if (zev_files.n == 0) {
        usage();
        return 1;
    }

    char *home = driver_find_home(argv[0]);
    if (!home) {
        fprintf(stderr, "error: cannot find the Zevic standard library and runtime.\n"
                        "Set ZEVIC_HOME to the directory that contains `stdlib/` and `runtime/`.\n");
        return 2;
    }

    Program *pg = prog_new(zsprintf("%s/stdlib", home));
    vec_push(&pg->search_dirs, dir_name(zev_files.v[0]));
    vec_push(&pg->search_dirs, zstrdup("."));
    for (int i = 0; i < zev_files.n; i++) {
        if (!prog_load_file(pg, zev_files.v[i], false)) {
            fprintf(stderr, "error: cannot read `%s`\n", (char *)zev_files.v[i]);
            return 1;
        }
    }
    char *core = zsprintf("%s/stdlib/Core.zev", home);
    if (file_exists(core)) prog_load_file(pg, core, true);
    prog_resolve(pg);

    SB ll = {0};
    Gen *g = gen_new(pg);
    bool ok = zv_error_count == 0 && gen_program(g, &ll);
    if (!ok || zv_error_count) {
        fprintf(stderr, "%d error%s found\n", zv_error_count, zv_error_count == 1 ? "" : "s");
        return 1;
    }

    char *base = base_no_ext(zev_files.v[0]);
    if (header_path) {
        SB h = {0};
        gen_header(g, "ZEVIC_EXPORTS_H", &h);
        FILE *hf = fopen(header_path, "wb");
        if (!hf) {
            fprintf(stderr, "error: cannot write `%s`\n", header_path);
            return 2;
        }
        fwrite(h.p, 1, h.len, hf);
        fclose(hf);
    }
    if (emit_ll) {
        const char *path = out ? out : zsprintf("%s.ll", base);
        FILE *f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "error: cannot write `%s`\n", path);
            return 2;
        }
        fwrite(ll.p, 1, ll.len, f);
        fclose(f);
        return 0;
    }

    char *clang = driver_find_clang(opts.cc);
    if (!clang) return 2;
#ifdef _WIN32
    const char *exe_suffix = ".exe";
#else
    const char *exe_suffix = "";
#endif
    char *exe = out ? zstrdup(out) : zsprintf("%s%s", base, exe_suffix);
    char *ll_path = zsprintf("%s.zevic.ll", exe);
    FILE *f = fopen(ll_path, "wb");
    if (!f) {
        fprintf(stderr, "error: cannot write `%s`\n", ll_path);
        return 2;
    }
    fwrite(ll.p, 1, ll.len, f);
    fclose(f);
    int rc = driver_build(clang, home, ll_path, exe, &opts);
    if (!opts.keep_ll) driver_remove(ll_path);
    if (rc != 0) {
        fprintf(stderr, "error: clang failed (exit code %d) while building `%s`\n", rc, exe);
        return 2;
    }
    return 0;
}
