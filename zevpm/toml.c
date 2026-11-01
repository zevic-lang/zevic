#include "toml.h"

#include <ctype.h>

typedef enum { V_STR, V_ARR, V_TABLE } VKind;

typedef struct Value {
    VKind kind;
    char *s;
    Vec items; /* V_ARR: Value* */
    Vec keys;  /* V_TABLE: char* */
    Vec vals;  /* V_TABLE: Value* */
} Value;

typedef struct {
    const char *file;
    int line;
    const char *p; /* the rest of the line */
} Cur;

static void fail(Cur *c, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
static void fail(Cur *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = zvsprintf(fmt, ap);
    va_end(ap);
    fprintf(stderr, "error: %s:%d: %s\n", c->file, c->line, msg);
    exit(1);
}

static void skip_ws(Cur *c) {
    while (*c->p == ' ' || *c->p == '\t') c->p++;
}

static char *parse_string(Cur *c) {
    c->p++; /* the opening quote */
    SB sb = {0};
    while (*c->p && *c->p != '"') {
        if (*c->p == '\\') {
            c->p++;
            switch (*c->p) {
            case 'n': sb_putc(&sb, '\n'); break;
            case 't': sb_putc(&sb, '\t'); break;
            case '"': sb_putc(&sb, '"'); break;
            case '\\': sb_putc(&sb, '\\'); break;
            default: fail(c, "unknown escape `\\%c` in a string", *c->p ? *c->p : ' ');
            }
            c->p++;
        } else {
            sb_putc(&sb, *c->p++);
        }
    }
    if (*c->p != '"') fail(c, "the string is not closed");
    c->p++;
    return sb_str(&sb);
}

static char *parse_key(Cur *c) {
    const char *s = c->p;
    while (isalnum((unsigned char)*c->p) || *c->p == '_' || *c->p == '-') c->p++;
    if (c->p == s) fail(c, "expected a name");
    return zstrndup(s, (size_t)(c->p - s));
}

static Value *parse_value(Cur *c) {
    Value *v = zalloc(sizeof *v);
    skip_ws(c);
    if (*c->p == '"') {
        v->kind = V_STR;
        v->s = parse_string(c);
    } else if (*c->p == '[') {
        v->kind = V_ARR;
        c->p++;
        skip_ws(c);
        while (*c->p && *c->p != ']') {
            vec_push(&v->items, parse_value(c));
            skip_ws(c);
            if (*c->p == ',') {
                c->p++;
                skip_ws(c);
            } else if (*c->p != ']') {
                fail(c, "expected `,` or `]` in the list");
            }
        }
        if (*c->p != ']') fail(c, "the list is not closed (a list must fit on one line)");
        c->p++;
    } else if (*c->p == '{') {
        v->kind = V_TABLE;
        c->p++;
        skip_ws(c);
        while (*c->p && *c->p != '}') {
            vec_push(&v->keys, parse_key(c));
            skip_ws(c);
            if (*c->p != '=') fail(c, "expected `=` after the name");
            c->p++;
            vec_push(&v->vals, parse_value(c));
            skip_ws(c);
            if (*c->p == ',') {
                c->p++;
                skip_ws(c);
            } else if (*c->p != '}') {
                fail(c, "expected `,` or `}` in the table");
            }
        }
        if (*c->p != '}') fail(c, "the table is not closed (it must fit on one line)");
        c->p++;
    } else {
        fail(c, "expected a string, a list or a table; only strings are supported as values");
    }
    return v;
}

/* Cut a trailing `# comment` (a `#` inside a string stays). */
static char *strip_comment(const char *line) {
    SB sb = {0};
    bool in_str = false;
    for (const char *p = line; *p; p++) {
        if (in_str) {
            sb_putc(&sb, *p);
            if (*p == '\\' && p[1]) sb_putc(&sb, *++p);
            else if (*p == '"') in_str = false;
        } else if (*p == '#') {
            break;
        } else {
            if (*p == '"') in_str = true;
            sb_putc(&sb, *p);
        }
    }
    char *s = sb_str(&sb);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

bool is_pascal_case(const char *s) {
    if (!s[0] || !(s[0] >= 'A' && s[0] <= 'Z')) return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s)) return false;
    return true;
}

static bool is_version(const char *s) {
    int dots = 0;
    if (!isdigit((unsigned char)*s)) return false;
    for (; *s; s++) {
        if (*s == '.') {
            dots++;
            if (!isdigit((unsigned char)s[1])) return false;
        } else if (!isdigit((unsigned char)*s)) {
            return false;
        }
    }
    return dots == 2;
}

static char *want_str(Cur *c, Value *v, const char *what) {
    if (v->kind != V_STR) fail(c, "%s must be a string", what);
    return v->s;
}

static Dep *make_dep(Cur *c, const char *name, Value *v) {
    if (!is_pascal_case(name)) fail(c, "the dependency name `%s` must be PascalCase", name);
    if (v->kind != V_TABLE) fail(c, "dependency `%s` must be a table such as { git = \"...\", tag = \"v1.0.0\" }", name);
    Dep *d = zalloc(sizeof *d);
    d->name = zstrdup(name);
    for (int i = 0; i < v->keys.n; i++) {
        const char *k = v->keys.v[i];
        char *s = want_str(c, v->vals.v[i], zsprintf("`%s` of dependency `%s`", k, name));
        if (strcmp(k, "git") == 0) d->git = s;
        else if (strcmp(k, "tag") == 0) d->tag = s;
        else if (strcmp(k, "branch") == 0) d->branch = s;
        else if (strcmp(k, "rev") == 0) d->rev = s;
        else if (strcmp(k, "path") == 0) d->path = s;
        else fail(c, "unknown key `%s` in dependency `%s` (expected git, tag, branch, rev or path)", k, name);
    }
    int specs = (d->tag != NULL) + (d->branch != NULL) + (d->rev != NULL);
    if (!d->git && !d->path) fail(c, "dependency `%s` needs `git` or `path`", name);
    if (d->git && d->path) fail(c, "dependency `%s` has both `git` and `path`; use one", name);
    if (d->path && specs) fail(c, "dependency `%s`: `tag`, `branch` and `rev` go with `git`, not `path`", name);
    if (specs > 1) fail(c, "dependency `%s`: use only one of `tag`, `branch` and `rev`", name);
    return d;
}

Manifest *manifest_load(const char *dir) {
    Manifest *m = zalloc(sizeof *m);
    m->dir = zstrdup(dir);
    m->file = path_join(dir, "zevic.toml");
    size_t len;
    char *text = read_file(m->file, &len);
    if (!text) {
        fprintf(stderr, "error: cannot read `%s` (is this a Zevic package? `zevpm init` makes one)\n", m->file);
        exit(1);
    }
    enum { S_NONE, S_PACKAGE, S_DEPS, S_NATIVE } sect = S_NONE;
    bool seen_package = false;
    Cur c = {m->file, 0, NULL};
    char *p = text;
    while (*p) {
        char *eol = strchr(p, '\n');
        char *raw = eol ? zstrndup(p, (size_t)(eol - p)) : zstrdup(p);
        p = eol ? eol + 1 : p + strlen(p);
        c.line++;
        char *line = strip_comment(raw);
        c.p = line;
        skip_ws(&c);
        if (!*c.p) continue;
        if (*c.p == '[') {
            char *end = strchr(c.p, ']');
            if (!end) fail(&c, "expected `]` after the section name");
            char *name = zstrndup(c.p + 1, (size_t)(end - c.p - 1));
            if (strcmp(name, "package") == 0) {
                sect = S_PACKAGE;
                seen_package = true;
            } else if (strcmp(name, "deps") == 0) sect = S_DEPS;
            else if (strcmp(name, "native") == 0) sect = S_NATIVE;
            else fail(&c, "unknown section `[%s]` (expected [package], [deps] or [native])", name);
            continue;
        }
        char *key = parse_key(&c);
        skip_ws(&c);
        if (*c.p != '=') fail(&c, "expected `=` after `%s`", key);
        c.p++;
        Value *v = parse_value(&c);
        skip_ws(&c);
        if (*c.p) fail(&c, "unexpected text after the value");
        switch (sect) {
        case S_NONE: fail(&c, "`%s` is outside any section", key);
        case S_PACKAGE: {
            char *s = want_str(&c, v, zsprintf("`%s`", key));
            if (strcmp(key, "name") == 0) m->name = s;
            else if (strcmp(key, "version") == 0) m->version = s;
            else if (strcmp(key, "kind") == 0) m->kind = s;
            else if (strcmp(key, "zevic") == 0) m->zevic_req = s;
            else fail(&c, "unknown key `%s` in [package] (expected name, version, kind or zevic)", key);
            break;
        }
        case S_DEPS:
            if (manifest_find_dep(m, key)) fail(&c, "dependency `%s` is listed twice", key);
            vec_push(&m->deps, make_dep(&c, key, v));
            break;
        case S_NATIVE:
            if (strcmp(key, "libs") != 0) fail(&c, "unknown key `%s` in [native] (expected libs)", key);
            if (v->kind != V_ARR) fail(&c, "`libs` must be a list of strings");
            for (int i = 0; i < v->items.n; i++) vec_push(&m->libs, want_str(&c, v->items.v[i], "a library name"));
            break;
        }
    }
    c.line = 1;
    if (!seen_package) fail(&c, "the [package] section is missing");
    if (!m->name) fail(&c, "[package] needs `name`");
    if (!is_pascal_case(m->name)) fail(&c, "the package name `%s` must be PascalCase (for example \"MyApp\")", m->name);
    if (!m->version) fail(&c, "[package] needs `version`");
    if (!is_version(m->version)) fail(&c, "the version `%s` must look like 1.2.3", m->version);
    if (!m->kind) m->kind = "bin";
    if (strcmp(m->kind, "bin") != 0 && strcmp(m->kind, "lib") != 0)
        fail(&c, "`kind` must be \"bin\" or \"lib\", not \"%s\"", m->kind);
    return m;
}

Dep *manifest_find_dep(Manifest *m, const char *name) {
    for (int i = 0; i < m->deps.n; i++)
        if (strcmp(((Dep *)m->deps.v[i])->name, name) == 0) return m->deps.v[i];
    return NULL;
}

/* ---- editing ---- */

char *dep_line(const Dep *d) {
    SB sb = {0};
    sb_printf(&sb, "%s = { ", d->name);
    if (d->path) {
        sb_printf(&sb, "path = \"%s\"", d->path);
    } else {
        sb_printf(&sb, "git = \"%s\"", d->git);
        if (d->tag) sb_printf(&sb, ", tag = \"%s\"", d->tag);
        if (d->branch) sb_printf(&sb, ", branch = \"%s\"", d->branch);
        if (d->rev) sb_printf(&sb, ", rev = \"%s\"", d->rev);
    }
    sb_puts(&sb, " }");
    return sb_str(&sb);
}

/* The file as a list of lines. */
static Vec split_lines(const char *text) {
    Vec lines = {0};
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        vec_push(&lines, zstrndup(p, n));
        p += n + (eol ? 1 : 0);
    }
    return lines;
}

static void save_lines(Manifest *m, Vec *lines) {
    SB sb = {0};
    for (int i = 0; i < lines->n; i++) sb_printf(&sb, "%s\n", (char *)lines->v[i]);
    if (!os_write_file(m->file, sb_str(&sb))) {
        fprintf(stderr, "error: cannot write `%s`\n", m->file);
        exit(2);
    }
}

static bool is_section_line(const char *l) {
    while (*l == ' ' || *l == '\t') l++;
    return *l == '[';
}

static bool line_is_dep(const char *l, const char *name) {
    while (*l == ' ' || *l == '\t') l++;
    size_t n = strlen(name);
    if (strncmp(l, name, n) != 0) return false;
    l += n;
    while (*l == ' ' || *l == '\t') l++;
    return *l == '=';
}

/* The index of the line `[deps]`, or -1. */
static int find_deps_header(Vec *lines) {
    for (int i = 0; i < lines->n; i++) {
        char *l = lines->v[i];
        while (*l == ' ' || *l == '\t') l++;
        if (strncmp(l, "[deps]", 6) == 0) return i;
    }
    return -1;
}

void manifest_add_dep(Manifest *m, const Dep *d) {
    size_t len;
    char *text = read_file(m->file, &len);
    Vec lines = split_lines(text ? text : "");
    char *line = dep_line(d);
    int h = find_deps_header(&lines);
    if (h < 0) {
        if (lines.n && ((char *)lines.v[lines.n - 1])[0]) vec_push(&lines, zstrdup(""));
        vec_push(&lines, zstrdup("[deps]"));
        vec_push(&lines, line);
        save_lines(m, &lines);
        return;
    }
    int end = h + 1; /* one past the last dependency line */
    for (int i = h + 1; i < lines.n && !is_section_line(lines.v[i]); i++) {
        if (line_is_dep(lines.v[i], d->name)) {
            lines.v[i] = line;
            save_lines(m, &lines);
            return;
        }
        if (((char *)lines.v[i])[0]) end = i + 1;
    }
    vec_push(&lines, NULL);
    for (int i = lines.n - 1; i > end; i--) lines.v[i] = lines.v[i - 1];
    lines.v[end] = line;
    save_lines(m, &lines);
}

bool manifest_remove_dep(Manifest *m, const char *name) {
    size_t len;
    char *text = read_file(m->file, &len);
    Vec lines = split_lines(text ? text : "");
    int h = find_deps_header(&lines);
    if (h < 0) return false;
    for (int i = h + 1; i < lines.n && !is_section_line(lines.v[i]); i++) {
        if (line_is_dep(lines.v[i], name)) {
            for (int k = i; k < lines.n - 1; k++) lines.v[k] = lines.v[k + 1];
            lines.n--;
            save_lines(m, &lines);
            return true;
        }
    }
    return false;
}
