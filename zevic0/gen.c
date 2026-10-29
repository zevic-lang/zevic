#include "gen.h"

#include "types.h"

/* =========================================================================
 * State
 * ========================================================================= */

/* A mapping from type parameter names to types, plus `Self`. */
typedef struct Env {
    Vec names; /* char* */
    Vec types; /* Type* */
    Type *self;
} Env;

/* A function (or method) with its type arguments fixed: one LLVM function. */
typedef struct Inst {
    Decl *decl;
    Type *self;     /* owning type for methods */
    Env *env;
    char *name;     /* LLVM symbol without the @ */
    Vec ptypes;     /* Type* */
    Type *ret;
    bool emitted;
    bool is_extern;
} Inst;

struct Gen {
    Program *pg;
    SB globals;     /* string constants */
    SB decls;       /* declare ... */
    SB funcs;       /* define ... */
    Dict declared;
    Dict insts;
    Vec worklist;
    int nstr;
    Dict strconst;
    int tmp_global;
};

/* The type of an expression that already produced an error: it matches everything. */
static Type *ty_err;

Gen *gen_new(Program *pg) {
    types_init();
    ty_err = ty_param("error");
    Gen *g = zalloc(sizeof *g);
    g->pg = pg;
    return g;
}

/* =========================================================================
 * Names
 * ========================================================================= */

static char *mangle_part(const char *s) {
    SB sb = {0};
    for (; *s; s++) {
        if (*s == '.') sb_putc(&sb, '_');
        else sb_putc(&sb, *s);
    }
    return sb_str(&sb);
}

/* Type names can hold characters (<>, ()) that are fine inside quoted LLVM names. */
static char *llname(const char *s) {
    return zsprintf("\"%s\"", s);
}

static Env *env_new(void) {
    return zalloc(sizeof(Env));
}

static void env_add(Env *e, const char *name, Type *t) {
    vec_push(&e->names, zstrdup(name));
    vec_push(&e->types, t);
}

static Type *env_get(Env *e, const char *name) {
    if (!e) return NULL;
    for (int i = e->names.n - 1; i >= 0; i--)
        if (strcmp(e->names.v[i], name) == 0) return e->types.v[i];
    return NULL;
}

/* =========================================================================
 * Declaration lookup
 * ========================================================================= */

static Decl *lookup_in_prelude(Gen *g, const char *name) {
    Module *core = prog_find_module(g->pg, "Zevic.Core");
    if (!core) return NULL;
    Decl *d = module_find(core, name);
    return d && d->is_pub ? d : NULL;
}

/* Find the declaration a dotted path starts with. `*used` is the number of
 * segments consumed (the module prefix plus the declaration name). */
static Decl *find_decl(Gen *g, SourceFile *f, char **segs, int n, int *used, Pos pos) {
    if (!f || !f->mod) return NULL;
    for (int k = n - 1; k >= 1; k--) {
        SB sb = {0};
        for (int i = 0; i < k; i++) sb_printf(&sb, "%s%s", i ? "." : "", segs[i]);
        Module *m = dict_get(&f->import_map, sb_str(&sb));
        if (!m) continue;
        Decl *d = module_find(m, segs[k]);
        if (!d) {
            diag_error(pos, "module `%s` has no member `%s`", m->name, segs[k]);
            *used = -1;
            return NULL;
        }
        if (!d->is_pub && d->mod != f->mod) {
            diag_error(pos, "`%s` is private to module `%s` (names starting with a lowercase letter are not exported)",
                       segs[k], m->name);
            *used = -1;
            return NULL;
        }
        *used = k + 1;
        return d;
    }
    Decl *d = module_find(f->mod, segs[0]);
    if (d) {
        *used = 1;
        return d;
    }
    d = lookup_in_prelude(g, segs[0]);
    if (d) {
        *used = 1;
        return d;
    }
    return NULL;
}

/* =========================================================================
 * Strings
 * ========================================================================= */

static char *ll_escape(const char *s, int n) {
    SB sb = {0};
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 32 && c < 127 && c != '"' && c != '\\') sb_putc(&sb, (char)c);
        else sb_printf(&sb, "\\%02X", c);
    }
    return sb_str(&sb);
}

/* The LLVM constant for a string literal: `{ ptr @.str.N, i64 len }`. */
static char *str_const(Gen *g, const char *s, int n) {
    char *key = zstrndup(s, (size_t)n);
    /* keys cannot contain NULs; make them unique through the length prefix */
    key = zsprintf("%d:%s", n, key);
    char *name = dict_get(&g->strconst, key);
    if (!name) {
        name = zsprintf("@.str.%d", g->nstr++);
        dict_put(&g->strconst, key, name);
        sb_printf(&g->globals, "%s = private unnamed_addr constant [%d x i8] c\"%s\\00\"\n", name, n + 1,
                  ll_escape(s, n));
    }
    return zsprintf("{ ptr %s, i64 %d }", name, n);
}

static void declare(Gen *g, const char *name, const char *line) {
    if (dict_has(&g->declared, name)) return;
    dict_put(&g->declared, name, (void *)line);
    sb_printf(&g->decls, "%s\n", line);
}

/* =========================================================================
 * Types from syntax
 * ========================================================================= */

static Type *resolve_type(Gen *g, SourceFile *f, Env *env, Node *te);

static char *qualified_name(Decl *d) {
    if (strcmp(d->mod->name, "Zevic.Core") == 0) return d->name;
    return zsprintf("%s.%s", d->mod->name, d->name);
}

static Type *instantiate(Gen *g, Decl *d, Vec targs, Pos pos) {
    Node *n = d->node;
    if (n->list2.n != targs.n) {
        if (n->list2.n == 0)
            diag_error(pos, "type `%s` takes no type arguments", d->name);
        else
            diag_error(pos, "type `%s` needs %d type argument(s), found %d", d->name, n->list2.n, targs.n);
        return ty_err;
    }
    TKind k = n->kind == D_STRUCT ? T_STRUCT : n->kind == D_CLASS ? T_CLASS : T_ENUM;
    return ty_named(k, d, qualified_name(d), targs);
}

/* Resolve the fields (struct, class) or variants (enum) of a type. */
static void ensure_members(Gen *g, Type *t) {
    if (t->members_done) return;
    t->members_done = true;
    if (!t->decl) return;
    Node *n = t->decl->node;
    Env *env = env_new();
    env->self = t;
    for (int i = 0; i < n->list2.n; i++) env_add(env, ((Node *)n->list2.v[i])->name, t->args.v[i]);
    SourceFile *f = t->decl->file;
    if (n->kind == D_STRUCT || n->kind == D_CLASS) {
        for (int i = 0; i < n->list.n; i++) {
            Node *m = n->list.v[i];
            if (m->kind != N_FIELD || m->flag) continue;
            Field *fl = zalloc(sizeof *fl);
            fl->name = m->name;
            fl->t = resolve_type(g, f, env, m->a);
            vec_push(&t->fields, fl);
        }
    } else if (n->kind == D_ENUM) {
        int tag = 0;
        for (int i = 0; i < n->list.n; i++) {
            Node *m = n->list.v[i];
            if (m->kind != N_VARIANT) continue;
            Variant *v = zalloc(sizeof *v);
            v->name = m->name;
            v->tag = tag++;
            v->named = m->flag;
            Vec ptypes = {0};
            if (m->flag) {
                for (int k = 0; k < m->list2.n; k++) {
                    Node *fn = m->list2.v[k];
                    Field *fl = zalloc(sizeof *fl);
                    fl->name = fn->name;
                    fl->t = resolve_type(g, f, env, fn->a);
                    vec_push(&v->fields, fl);
                    vec_push(&ptypes, fl->t);
                }
            } else {
                for (int k = 0; k < m->list.n; k++) {
                    Field *fl = zalloc(sizeof *fl);
                    fl->name = zsprintf("%d", k);
                    fl->t = resolve_type(g, f, env, m->list.v[k]);
                    vec_push(&v->fields, fl);
                    vec_push(&ptypes, fl->t);
                }
            }
            if (ptypes.n) v->payload = ty_tuple(ptypes);
            vec_push(&t->variants, v);
        }
    }
}

static Type *resolve_type(Gen *g, SourceFile *f, Env *env, Node *te) {
    switch (te->kind) {
    case N_TYPE_TUPLE: {
        Vec elems = {0};
        for (int i = 0; i < te->list.n; i++) vec_push(&elems, resolve_type(g, f, env, te->list.v[i]));
        return ty_tuple(elems);
    }
    case N_TYPE_FN: {
        Vec ps = {0};
        for (int i = 0; i < te->list.n; i++) vec_push(&ps, resolve_type(g, f, env, te->list.v[i]));
        return ty_fn(ps, resolve_type(g, f, env, te->a));
    }
    case N_TYPE_PTR:
        return ty_ptr(resolve_type(g, f, env, te->a));
    case N_TYPE_NAME: {
        char **segs = (char **)te->path.v;
        int n = te->path.n;
        if (n == 1) {
            const char *name = segs[0];
            Type *bound = env_get(env, name);
            if (bound) return bound;
            if (strcmp(name, "Self") == 0) {
                if (!env || !env->self) {
                    diag_error(te->pos, "`Self` can only be used inside a type or interface");
                    return ty_err;
                }
                return env->self;
            }
            Type *p = ty_prim_by_name(name);
            if (p) {
                if (te->list.n) diag_error(te->pos, "type `%s` takes no type arguments", name);
                return p;
            }
            if (strcmp(name, "Never") == 0) return ty_never;
            if (strcmp(name, "Array") == 0) {
                if (te->list.n != 1) {
                    diag_error(te->pos, "`Array` needs exactly one type argument, like `Array<I32>`");
                    return ty_err;
                }
                return ty_array(resolve_type(g, f, env, te->list.v[0]));
            }
        }
        int used = 0;
        Decl *d = find_decl(g, f, segs, n, &used, te->pos);
        if (used < 0) return ty_err;
        if (!d || used != n) {
            diag_error(te->pos, "unknown type `%s`", path_join(&te->path, "."));
            return ty_err;
        }
        if (d->node->kind != D_STRUCT && d->node->kind != D_CLASS && d->node->kind != D_ENUM) {
            diag_error(te->pos, "`%s` is not a type", d->name);
            return ty_err;
        }
        Vec targs = {0};
        for (int i = 0; i < te->list.n; i++) vec_push(&targs, resolve_type(g, f, env, te->list.v[i]));
        return instantiate(g, d, targs, te->pos);
    }
    default:
        diag_error(te->pos, "expected a type");
        return ty_err;
    }
}

/* =========================================================================
 * Function instances
 * ========================================================================= */

static Env *make_env(Decl *d, Type *self, Vec *fn_targs) {
    Env *e = env_new();
    if (self) {
        e->self = self;
        Node *td = self->decl ? self->decl->node : NULL;
        if (td)
            for (int i = 0; i < td->list2.n && i < self->args.n; i++)
                env_add(e, ((Node *)td->list2.v[i])->name, self->args.v[i]);
    }
    Node *fn = d->node;
    for (int i = 0; i < fn->list2.n; i++)
        if (fn_targs && i < fn_targs->n) env_add(e, ((Node *)fn->list2.v[i])->name, fn_targs->v[i]);
    return e;
}

static Inst *get_inst(Gen *g, Decl *d, Type *self, Vec *targs) {
    Node *fn = d->node;
    SB key = {0};
    bool is_extern = d->abi != NULL;
    if (is_extern) {
        sb_puts(&key, fn->name2 ? fn->name2 : fn->name);
    } else if (self) {
        sb_printf(&key, "zevic_%s_%s", mangle_part(self->name), fn->name);
    } else {
        sb_printf(&key, "zevic_%s_%s", mangle_part(d->mod->name), fn->name);
    }
    if (targs && targs->n && !is_extern) {
        sb_putc(&key, '<');
        for (int i = 0; i < targs->n; i++) sb_printf(&key, "%s%s", i ? ", " : "", ((Type *)targs->v[i])->name);
        sb_putc(&key, '>');
    }
    char *k = sb_str(&key);
    Inst *in = dict_get(&g->insts, k);
    if (in) return in;
    in = zalloc(sizeof *in);
    in->decl = d;
    in->self = self;
    in->name = k;
    in->is_extern = is_extern;
    in->env = make_env(d, self, targs);
    for (int i = 0; i < fn->list.n; i++) {
        Node *p = fn->list.v[i];
        if (p->flag3) continue; /* `...` */
        Type *pt = resolve_type(g, d->file, in->env, p->a);
        if (p->flag2) pt = ty_array(pt); /* params x: T arrives as Array<T> */
        vec_push(&in->ptypes, pt);
    }
    in->ret = fn->a ? resolve_type(g, d->file, in->env, fn->a) : ty_unit;
    dict_put(&g->insts, k, in);
    if (!is_extern) vec_push(&g->worklist, in);
    return in;
}

/* =========================================================================
 * Function context and IR emission
 * ========================================================================= */

typedef struct {
    Type *t;
    char *v; /* LLVM operand ("%t3", "42", "{ ptr @.str.0, i64 5 }", "zeroinitializer") */
} Val;

typedef struct Local {
    char *name;
    Type *t;
    char *ptr; /* the alloca */
    bool mut;
    Pos pos;
} Local;

typedef struct Scope {
    struct Scope *up;
    Vec locals; /* Local* */
    Vec defers; /* Node* */
} Scope;

typedef struct {
    char *brk, *cont;
    Scope *scope;
} Loop;

typedef struct FnCtx {
    Gen *g;
    Inst *inst;
    SourceFile *file;
    Env *env;
    SB code, allocas, cold;
    int ntmp, nlbl, nslot;
    char *cur;
    bool term;
    Type *ret;
    Scope *scope;
    Vec loops;
    bool this_mut;
    Local *this_local;
} FnCtx;

static bool is_err(Type *t) { return t == ty_err; }

static Val unit_val(void) {
    Val v = {ty_unit, "zeroinitializer"};
    return v;
}

static Val err_val(void) {
    Val v = {ty_err, "undef"};
    return v;
}

/* The value of an expression that never completes, typed as `want`. */
static Val never_val(Type *want) {
    Val v = {want ? want : ty_never, "undef"};
    return v;
}

static char *tmp(FnCtx *F) {
    return zsprintf("%%t%d", F->ntmp++);
}

static char *newlabel(FnCtx *F, const char *hint) {
    return zsprintf("%s.%d", hint, F->nlbl++);
}

static void start_block(FnCtx *F, const char *label) {
    sb_printf(&F->code, "%s:\n", label);
    F->cur = zstrdup(label);
    F->term = false;
}

static void emit(FnCtx *F, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void emit(FnCtx *F, const char *fmt, ...) {
    if (F->term) start_block(F, newlabel(F, "dead")); /* unreachable code gets its own block */
    va_list ap;
    va_start(ap, fmt);
    char *s = zvsprintf(fmt, ap);
    va_end(ap);
    sb_printf(&F->code, "  %s\n", s);
}

/* Terminate the current block with a branch and start `label`. */
static void goto_block(FnCtx *F, const char *label) {
    if (!F->term) {
        sb_printf(&F->code, "  br label %%%s\n", label);
        F->term = true;
    }
    start_block(F, label);
}

static void br(FnCtx *F, const char *label) {
    emit(F, "br label %%%s", label);
    F->term = true;
}

static void condbr(FnCtx *F, const char *cond, const char *t, const char *f) {
    emit(F, "br i1 %s, label %%%s, label %%%s", cond, t, f);
    F->term = true;
}

static char *new_slot(FnCtx *F, Type *t) {
    char *name = zsprintf("%%s%d", F->nslot++);
    sb_printf(&F->allocas, "  %s = alloca %s, align %lld\n", name, ty_ll(t), (long long)(ty_align(t) < 1 ? 1 : ty_align(t)));
    return name;
}

static char *type_ll(Type *t) {
    return (char *)ty_ll(t);
}

static void type_error(Pos pos, Type *want, Type *got, const char *what) {
    if (is_err(want) || is_err(got)) return;
    diag_error(pos, "type mismatch%s%s: expected `%s`, found `%s`", what ? " in " : "", what ? what : "", want->name,
               got->name);
}

/* Check that `v` has type `want`; a value of type Never fits everywhere. */
static Val coerce(FnCtx *F, Pos pos, Val v, Type *want, const char *what) {
    (void)F;
    if (v.t == want || is_err(v.t) || is_err(want)) return v;
    if (v.t == ty_never) return never_val(want);
    type_error(pos, want, v.t, what);
    return err_val();
}

/* Is this expression a number literal whose type comes from the context? */
static bool is_untyped_lit(Node *n) {
    if (n->kind == N_UNARY && n->op == TK_MINUS) return is_untyped_lit(n->a);
    return (n->kind == N_INT || n->kind == N_FLOAT) && !n->suffix;
}

/* A runtime call that panics, in the cold section; returns the label to branch to. */
static char *cold_panic(FnCtx *F, const char *msg, Pos pos) {
    Gen *g = F->g;
    declare(g, "zevic_panic", "declare void @zevic_panic(ptr, i64, ptr, i32) noreturn");
    char *label = newlabel(F, "panic");
    const char *file = src_name(pos.file);
    char *m = str_const(g, msg, (int)strlen(msg));
    char *fl = str_const(g, file, (int)strlen(file));
    /* the constants are `{ ptr @x, i64 n }`; the runtime takes the pieces */
    char *mp = zstrndup(m + 6, strchr(m, ',') - (m + 6));
    char *fp = zstrndup(fl + 6, strchr(fl, ',') - (fl + 6));
    sb_printf(&F->cold, "%s:\n  call void @zevic_panic(ptr %s, i64 %d, ptr %s, i32 %d)\n  unreachable\n", label, mp,
              (int)strlen(msg), fp, pos.line);
    return label;
}

/* ---- scopes ---- */

static void push_scope(FnCtx *F) {
    Scope *s = zalloc(sizeof *s);
    s->up = F->scope;
    F->scope = s;
}

static Local *find_local(FnCtx *F, const char *name) {
    for (Scope *s = F->scope; s; s = s->up)
        for (int i = s->locals.n - 1; i >= 0; i--)
            if (strcmp(((Local *)s->locals.v[i])->name, name) == 0) return s->locals.v[i];
    return NULL;
}

static Local *declare_local(FnCtx *F, const char *name, Type *t, bool mut, Pos pos) {
    for (int i = 0; i < F->scope->locals.n; i++) {
        Local *o = F->scope->locals.v[i];
        if (strcmp(o->name, name) == 0) {
            diag_error(pos, "`%s` is already declared in this scope", name);
            diag_note("the first declaration is at line %d", o->pos.line);
            break;
        }
    }
    Local *l = zalloc(sizeof *l);
    l->name = zstrdup(name);
    l->t = t;
    l->mut = mut;
    l->pos = pos;
    l->ptr = new_slot(F, t);
    vec_push(&F->scope->locals, l);
    return l;
}

static Val load_local(FnCtx *F, Local *l) {
    if (l->t == ty_unit) return unit_val();
    char *t = tmp(F);
    emit(F, "%s = load %s, ptr %s", t, type_ll(l->t), l->ptr);
    Val v = {l->t, t};
    return v;
}

static void store_to(FnCtx *F, Type *t, Val v, const char *ptr) {
    emit(F, "store %s %s, ptr %s", type_ll(t), v.v, ptr);
}

static void run_defers_in(FnCtx *F, Scope *s);

/* Leave the innermost scope on the normal path. */
static void pop_scope(FnCtx *F) {
    Scope *s = F->scope;
    if (!F->term) run_defers_in(F, s);
    F->scope = s->up;
}
