#include "module.h"

#include "parser.h"

Program *prog_new(const char *std_dir) {
    Program *pg = zalloc(sizeof *pg);
    pg->std_dir = zstrdup(std_dir ? std_dir : "");
    return pg;
}

Module *prog_find_module(Program *pg, const char *name) {
    return dict_get(&pg->mod_by_name, name);
}

Decl *module_find(Module *m, const char *name) {
    return dict_get(&m->decls, name);
}

static Module *module_get(Program *pg, const char *name, bool is_std) {
    Module *m = prog_find_module(pg, name);
    if (m) return m;
    m = zalloc(sizeof *m);
    m->name = zstrdup(name);
    m->is_std = is_std;
    dict_put(&pg->mod_by_name, name, m);
    vec_push(&pg->modules, m);
    return m;
}

static void add_decl(Module *m, SourceFile *sf, Node *n, const char *abi) {
    if (n->kind == D_TEST) {
        /* a test becomes a function `test.N` that no name can reach; it is only built by `--test` */
        Node *fn = node_new(D_FN, n->pos);
        fn->name = zsprintf("test.%d", m->tests.n);
        fn->b = n->b;
        Decl *t = zalloc(sizeof *t);
        t->node = fn;
        t->mod = m;
        t->file = sf;
        t->name = fn->name;
        t->test_name = n->name;
        vec_push(&m->tests, t);
        return;
    }
    Decl *d = zalloc(sizeof *d);
    d->node = n;
    d->mod = m;
    d->file = sf;
    d->name = n->name;
    d->abi = (char *)abi;
    d->is_pub = n->name[0] >= 'A' && n->name[0] <= 'Z';
    Decl *old = module_find(m, n->name);
    if (old) {
        diag_error(n->pos, "`%s` is already defined in module %s", n->name, m->name);
        diag_note("the first definition is at %s:%d", src_name(old->node->pos.file), old->node->pos.line);
        return;
    }
    dict_put(&m->decls, n->name, d);
}

static bool reserved_name(const char *n) {
    static const char *names[] = {"I8", "I16", "I32", "I64", "U8", "U16", "U32", "U64", "F32", "F64",
                                  "Bool", "Char", "String", "Unit", "Array", "Map", "Option", "Result",
                                  "Error", "ErrorKind", "StringBuilder", "Some", "None", "Ok", "Err",
                                  "Panic", "Same", NULL};
    for (int i = 0; names[i]; i++)
        if (strcmp(names[i], n) == 0) return true;
    return false;
}

SourceFile *prog_load_file(Program *pg, const char *path, bool is_std) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) return NULL;
    int file = src_add(path, text);
    SourceFile *sf = parse_file(file, text, len);
    sf->is_std = is_std;
    vec_push(&pg->files, sf);
    if (!sf->module) return sf;
    /* user code cannot define the reserved `Zevic` root */
    if (!is_std && (strcmp(sf->module, "Zevic") == 0 || strncmp(sf->module, "Zevic.", 6) == 0))
        diag_error(sf->module_pos, "module names starting with `Zevic` are reserved for the standard library");
    Module *m = module_get(pg, sf->module, is_std);
    sf->mod = m;
    vec_push(&m->files, sf);
    for (int i = 0; i < sf->decls.n; i++) {
        Node *d = sf->decls.v[i];
        if (d->kind == D_EXTERN) {
            for (int k = 0; k < d->list.n; k++) {
                Node *e = d->list.v[k];
                add_decl(m, sf, e, d->name);
            }
            continue;
        }
        if (!is_std && d->name && reserved_name(d->name))
            diag_error(d->pos, "`%s` is a reserved name (it is part of the prelude)", d->name);
        add_decl(m, sf, d, NULL);
    }
    return sf;
}

static char *to_slash_path(const char *dotted) {
    char *p = zstrdup(dotted);
    for (char *c = p; *c; c++)
        if (*c == '.') *c = '/';
    return p;
}

static SourceFile *try_load_module(Program *pg, const char *name, bool *is_std) {
    char *rel = to_slash_path(name);
    if (strncmp(name, "Zevic.", 6) == 0) {
        char *path = zsprintf("%s/%s.zev", pg->std_dir, rel + 6);
        if (file_exists(path)) {
            *is_std = true;
            return prog_load_file(pg, path, true);
        }
        return NULL;
    }
    *is_std = false;
    for (int i = 0; i < pg->search_dirs.n; i++) {
        const char *dir = pg->search_dirs.v[i];
        char *p1 = zsprintf("%s/%s.zev", dir, rel);
        if (file_exists(p1)) return prog_load_file(pg, p1, false);
        char *p2 = zsprintf("%s/src/%s.zev", dir, rel);
        if (file_exists(p2)) return prog_load_file(pg, p2, false);
        /* the package name is the first segment: allow <dir>/<rest>.zev */
        const char *slash = strchr(rel, '/');
        if (slash) {
            char *p3 = zsprintf("%s/%s.zev", dir, slash + 1);
            if (file_exists(p3)) return prog_load_file(pg, p3, false);
        }
    }
    return NULL;
}

static void resolve_file_imports(Program *pg, SourceFile *sf) {
    if (!sf->mod) return;
    for (int i = 0; i < sf->imports.n; i++) {
        Node *im = sf->imports.v[i];
        char *name = path_join(&im->path, ".");
        if (!sf->is_std && strncmp(name, "Zevic.Intrinsics", 16) == 0) {
            diag_error(im->pos, "`Zevic.Intrinsics` is only for the standard library");
            continue;
        }
        Module *m = prog_find_module(pg, name);
        if (!m) {
            bool is_std = false;
            try_load_module(pg, name, &is_std);
            m = prog_find_module(pg, name);
        }
        if (!m) {
            diag_error(im->pos, "cannot find module `%s`", name);
            if (strncmp(name, "Zevic.", 6) == 0)
                diag_note("looked for the standard library file %s/%s.zev", pg->std_dir, to_slash_path(name) + 6);
            continue;
        }
        if (m == sf->mod) {
            diag_error(im->pos, "a module cannot import itself");
            continue;
        }
        const char *key = im->name ? im->name : name;
        if (dict_has(&sf->import_map, key)) {
            diag_error(im->pos, "`%s` is already imported (or the name is used by another import)", key);
            continue;
        }
        dict_put(&sf->import_map, key, m);
        if (!im->name && m->is_std && strncmp(name, "Zevic.", 6) == 0) {
            /* the standard library may be written without the leading `Zevic.` */
            const char *shortname = name + 6;
            if (dict_has(&sf->import_map, shortname)) {
                diag_error(im->pos, "the short name `%s` clashes with another import; use the full name or `as`", shortname);
            } else {
                dict_put(&sf->import_map, shortname, m);
            }
        }
        bool seen = false;
        for (int k = 0; k < sf->mod->deps.n; k++)
            if (sf->mod->deps.v[k] == m) seen = true;
        if (!seen) vec_push(&sf->mod->deps, m);
    }
}

static bool dfs_cycle(Module *m, Vec *stack) {
    if (m->visit == 2) return false;
    vec_push(stack, m);
    if (m->visit == 1) return true;
    m->visit = 1;
    for (int i = 0; i < m->deps.n; i++) {
        if (dfs_cycle(m->deps.v[i], stack)) return true;
    }
    m->visit = 2;
    stack->n--;
    return false;
}

void prog_resolve(Program *pg) {
    /* `fn Main(args: Array<String>)` gets its arguments from Zevic.Env, which the program need not import */
    Module *mm = prog_find_module(pg, "Main");
    Decl *md = mm ? module_find(mm, "Main") : NULL;
    if (md && md->node->kind == D_FN && md->node->list.n == 1 && !prog_find_module(pg, "Zevic.Env")) {
        bool is_std = false;
        try_load_module(pg, "Zevic.Env", &is_std);
    }
    /* `async` and `await` are sugar for Zevic.Async, which the program need not import */
    if (g_uses_async && !prog_find_module(pg, "Zevic.Async")) {
        bool is_std = false;
        try_load_module(pg, "Zevic.Async", &is_std);
    }
    for (int i = 0; i < pg->files.n; i++) resolve_file_imports(pg, pg->files.v[i]);
    for (int i = 0; i < pg->modules.n; i++) {
        Module *m = pg->modules.v[i];
        Vec stack = {0};
        if (dfs_cycle(m, &stack)) {
            Module *last = stack.v[stack.n - 1];
            SB sb = {0};
            bool on = false;
            for (int k = 0; k < stack.n; k++) {
                Module *x = stack.v[k];
                if (x == last) on = true;
                if (on) sb_printf(&sb, "%s%s", sb.len ? " -> " : "", x->name);
            }
            Pos pos = {0};
            if (last->files.n) pos = ((SourceFile *)last->files.v[0])->module_pos;
            diag_error(pos, "circular import: %s", sb_str(&sb));
            return;
        }
    }
}
