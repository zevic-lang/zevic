#include "gen.h"

#include "types.h"

#include <math.h>

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

/* =========================================================================
 * Literals
 * ========================================================================= */

static Val gen_expr(FnCtx *F, Node *n, Type *expect);

static Type *type_from_suffix(const char *s) {
    return ty_prim_by_name(s);
}

static Val gen_int_lit(FnCtx *F, Node *n, Type *expect, bool neg) {
    Type *t;
    if (n->suffix) t = type_from_suffix(n->suffix);
    else if (expect && ty_is_int(expect)) t = expect;
    else if (expect && ty_is_float(expect)) {
        diag_error(n->pos, "expected `%s`, found an integer (write `%llu.0` for a decimal number)", expect->name,
                   (unsigned long long)n->ival);
        return err_val();
    } else t = ty_i64;
    int bits = ty_bits(t);
    uint64_t v = n->ival;
    if (ty_is_signed(t)) {
        uint64_t max = ((uint64_t)1 << (bits - 1)) - 1 + (neg ? 1 : 0);
        if (v > max) {
            diag_error(n->pos, "the number %s%llu does not fit in `%s` (range %lld to %lld)", neg ? "-" : "",
                       (unsigned long long)v, t->name, (long long)(-(int64_t)(max - (neg ? 1 : 0)) - 1),
                       (long long)(max - (neg ? 1 : 0)));
            return err_val();
        }
    } else {
        if (neg && v != 0) {
            diag_error(n->pos, "an unsigned type (`%s`) cannot hold a negative number", t->name);
            return err_val();
        }
        uint64_t max = bits == 64 ? UINT64_MAX : (((uint64_t)1 << bits) - 1);
        if (v > max) {
            diag_error(n->pos, "the number %llu does not fit in `%s` (maximum %llu)", (unsigned long long)v, t->name,
                       (unsigned long long)max);
            return err_val();
        }
    }
    int64_t sv = neg ? (int64_t)(0 - v) : (int64_t)v;
    Val r = {t, zsprintf("%lld", (long long)sv)};
    (void)F;
    return r;
}

static char *double_hex(double d) {
    union { double d; uint64_t u; } x;
    x.d = d;
    return zsprintf("0x%016llX", (unsigned long long)x.u);
}

static Val gen_float_lit(FnCtx *F, Node *n, Type *expect, bool neg) {
    Type *t;
    (void)F;
    if (n->suffix) t = type_from_suffix(n->suffix);
    else if (expect && ty_is_float(expect)) t = expect;
    else if (expect && ty_is_int(expect)) {
        diag_error(n->pos, "expected `%s`, found a decimal number", expect->name);
        return err_val();
    } else t = ty_f64;
    double d = neg ? -n->fval : n->fval;
    if (t == ty_f32) d = (double)(float)d; /* f32 constants are written as doubles that are exactly representable */
    Val r = {t, double_hex(d)};
    return r;
}

/* ---- runtime helpers ---- */

static void rt_decl(FnCtx *F, const char *name, const char *line) {
    declare(F->g, name, line);
}

static Val str_val(char *constant) {
    Val v = {ty_string, constant};
    return v;
}

static void str_parts(FnCtx *F, Val s, char **p, char **n) {
    if (strncmp(s.v, "{ ptr ", 6) == 0 && strchr(s.v, '@')) {
        /* a constant: take the pieces apart textually */
        char *comma = strchr(s.v, ',');
        *p = zstrndup(s.v + 6, (size_t)(comma - (s.v + 6)));
        char *len = strstr(s.v, "i64 ");
        *n = zstrndup(len + 4, strlen(len + 4) - 2);
        return;
    }
    *p = tmp(F);
    *n = tmp(F);
    emit(F, "%s = extractvalue { ptr, i64 } %s, 0", *p, s.v);
    emit(F, "%s = extractvalue { ptr, i64 } %s, 1", *n, s.v);
}

/* Call a runtime function that returns a String through an out pointer. */
static Val call_str_out(FnCtx *F, const char *fn, const char *args) {
    char *slot = new_slot(F, ty_string);
    emit(F, "call void @%s(ptr %s%s%s)", fn, slot, args[0] ? ", " : "", args);
    char *t = tmp(F);
    emit(F, "%s = load { ptr, i64 }, ptr %s", t, slot);
    Val v = {ty_string, t};
    return v;
}

/* x.ToString() for the built-in types. */
static Val gen_to_string(FnCtx *F, Val v, Pos pos) {
    Type *t = v.t;
    if (is_err(t)) return str_val("{ ptr null, i64 0 }");
    if (t == ty_string) return v;
    if (ty_is_int(t)) {
        int bits = ty_bits(t);
        char *x = v.v;
        if (bits < 64) {
            x = tmp(F);
            emit(F, "%s = %s i%d %s to i64", x, ty_is_signed(t) ? "sext" : "zext", bits, v.v);
        }
        if (ty_is_signed(t)) {
            rt_decl(F, "zevic_i64_to_str", "declare void @zevic_i64_to_str(ptr, i64)");
            return call_str_out(F, "zevic_i64_to_str", zsprintf("i64 %s", x));
        }
        rt_decl(F, "zevic_u64_to_str", "declare void @zevic_u64_to_str(ptr, i64)");
        return call_str_out(F, "zevic_u64_to_str", zsprintf("i64 %s", x));
    }
    if (ty_is_float(t)) {
        char *x = v.v;
        if (t == ty_f32) {
            x = tmp(F);
            emit(F, "%s = fpext float %s to double", x, v.v);
        }
        rt_decl(F, "zevic_f64_to_str", "declare void @zevic_f64_to_str(ptr, double)");
        return call_str_out(F, "zevic_f64_to_str", zsprintf("double %s", x));
    }
    if (t == ty_bool) {
        char *x = tmp(F);
        emit(F, "%s = zext i1 %s to i32", x, v.v);
        rt_decl(F, "zevic_bool_to_str", "declare void @zevic_bool_to_str(ptr, i32)");
        return call_str_out(F, "zevic_bool_to_str", zsprintf("i32 %s", x));
    }
    if (t == ty_char) {
        rt_decl(F, "zevic_char_to_str", "declare void @zevic_char_to_str(ptr, i32)");
        return call_str_out(F, "zevic_char_to_str", zsprintf("i32 %s", v.v));
    }
    if (t == ty_unit) return str_val(str_const(F->g, "()", 2));
    diag_error(pos, "`ToString` is not available for type `%s` yet", t->name);
    return str_val("{ ptr null, i64 0 }");
}

static Val gen_string_lit(FnCtx *F, Node *n) {
    if (n->list.n == 0) return str_val(str_const(F->g, "", 0));
    if (n->list.n == 1 && ((Node *)n->list.v[0])->kind == N_STR_LIT) {
        Node *lit = n->list.v[0];
        return str_val(str_const(F->g, lit->name, lit->slen));
    }
    /* interpolation: build the result in a runtime string builder */
    rt_decl(F, "zevic_sb_new", "declare ptr @zevic_sb_new()");
    rt_decl(F, "zevic_sb_add", "declare void @zevic_sb_add(ptr, ptr, i64)");
    rt_decl(F, "zevic_sb_finish", "declare void @zevic_sb_finish(ptr, ptr)");
    char *sb = tmp(F);
    emit(F, "%s = call ptr @zevic_sb_new()", sb);
    for (int i = 0; i < n->list.n; i++) {
        Node *part = n->list.v[i];
        Val s;
        if (part->kind == N_STR_LIT) {
            s = str_val(str_const(F->g, part->name, part->slen));
        } else {
            Val v = gen_expr(F, part, NULL);
            if (v.t == ty_never) return never_val(ty_string);
            s = gen_to_string(F, v, part->pos);
        }
        char *p, *len;
        str_parts(F, s, &p, &len);
        emit(F, "call void @zevic_sb_add(ptr %s, ptr %s, i64 %s)", sb, p, len);
    }
    return call_str_out(F, "zevic_sb_finish", zsprintf("ptr %s", sb));
}

/* =========================================================================
 * Operators
 * ========================================================================= */

static char *int_intrinsic(FnCtx *F, const char *op, Type *t) {
    int bits = ty_bits(t);
    char *name = zsprintf("llvm.%s.with.overflow.i%d", op, bits);
    declare(F->g, name, zsprintf("declare { i%d, i1 } @%s(i%d, i%d)", bits, name, bits, bits));
    return name;
}

static Val gen_int_arith(FnCtx *F, TokKind op, Val a, Val b, Pos pos) {
    Type *t = a.t;
    int bits = ty_bits(t);
    bool sgn = ty_is_signed(t);
    char *ll = type_ll(t);
    if (op == TK_PLUS || op == TK_MINUS || op == TK_STAR) {
        const char *nm = op == TK_PLUS ? (sgn ? "sadd" : "uadd") : op == TK_MINUS ? (sgn ? "ssub" : "usub") : (sgn ? "smul" : "umul");
        char *fn = int_intrinsic(F, nm, t);
        char *r = tmp(F), *v = tmp(F), *o = tmp(F);
        emit(F, "%s = call { %s, i1 } @%s(%s %s, %s %s)", r, ll, fn, ll, a.v, ll, b.v);
        emit(F, "%s = extractvalue { %s, i1 } %s, 0", v, ll, r);
        emit(F, "%s = extractvalue { %s, i1 } %s, 1", o, ll, r);
        char *ok = newlabel(F, "ok");
        condbr(F, o, cold_panic(F, "integer overflow", pos), ok);
        start_block(F, ok);
        Val res = {t, v};
        return res;
    }
    if (op == TK_SLASH || op == TK_PERCENT) {
        char *z = tmp(F);
        emit(F, "%s = icmp eq %s %s, 0", z, ll, b.v);
        char *ok = newlabel(F, "ok");
        condbr(F, z, cold_panic(F, op == TK_SLASH ? "division by zero" : "remainder by zero", pos), ok);
        start_block(F, ok);
        if (sgn) {
            /* MIN / -1 overflows */
            char *min = zsprintf("%lld", bits == 64 ? (long long)INT64_MIN : -((long long)1 << (bits - 1)));
            char *c1 = tmp(F), *c2 = tmp(F), *both = tmp(F);
            emit(F, "%s = icmp eq %s %s, %s", c1, ll, a.v, min);
            emit(F, "%s = icmp eq %s %s, -1", c2, ll, b.v);
            emit(F, "%s = and i1 %s, %s", both, c1, c2);
            char *ok2 = newlabel(F, "ok");
            condbr(F, both, cold_panic(F, "integer overflow", pos), ok2);
            start_block(F, ok2);
        }
        char *r = tmp(F);
        emit(F, "%s = %s %s %s, %s", r, op == TK_SLASH ? (sgn ? "sdiv" : "udiv") : (sgn ? "srem" : "urem"), ll, a.v, b.v);
        Val res = {t, r};
        return res;
    }
    const char *ins = NULL;
    switch (op) {
    case TK_AMP: ins = "and"; break;
    case TK_PIPE: ins = "or"; break;
    case TK_CARET: ins = "xor"; break;
    default: break;
    }
    char *r = tmp(F);
    emit(F, "%s = %s %s %s, %s", r, ins, ll, a.v, b.v);
    Val res = {t, r};
    return res;
}

/* a << n and a >> n: the amount is an I32 and must be below the width. */
static Val gen_shift(FnCtx *F, TokKind op, Val a, Val amt, Pos pos) {
    Type *t = a.t;
    int bits = ty_bits(t);
    char *ll = type_ll(t);
    char *bad = tmp(F);
    emit(F, "%s = icmp uge i32 %s, %d", bad, amt.v, bits);
    char *ok = newlabel(F, "ok");
    condbr(F, bad, cold_panic(F, "shift amount is out of range", pos), ok);
    start_block(F, ok);
    char *x = amt.v;
    if (bits != 32) {
        x = tmp(F);
        emit(F, "%s = %s i32 %s to %s", x, bits < 32 ? "trunc" : "zext", amt.v, ll);
    }
    char *r = tmp(F);
    emit(F, "%s = %s %s %s, %s", r, op == TK_SHL ? "shl" : (ty_is_signed(t) ? "ashr" : "lshr"), ll, a.v, x);
    Val res = {t, r};
    return res;
}

static Val gen_float_arith(FnCtx *F, TokKind op, Val a, Val b) {
    const char *ins = op == TK_PLUS ? "fadd" : op == TK_MINUS ? "fsub" : op == TK_STAR ? "fmul" : op == TK_SLASH ? "fdiv" : "frem";
    char *r = tmp(F);
    emit(F, "%s = %s %s %s, %s", r, ins, type_ll(a.t), a.v, b.v);
    Val res = {a.t, r};
    return res;
}

static Val gen_compare(FnCtx *F, TokKind op, Val a, Val b, Pos pos) {
    Type *t = a.t;
    char *r = tmp(F);
    Val res = {ty_bool, r};
    if (ty_is_int(t) || t == ty_char || t == ty_bool) {
        bool sgn = ty_is_signed(t);
        const char *p = op == TK_EQEQ ? "eq" : op == TK_NEQ ? "ne"
                      : op == TK_LT ? (sgn ? "slt" : "ult") : op == TK_LE ? (sgn ? "sle" : "ule")
                      : op == TK_GT ? (sgn ? "sgt" : "ugt") : (sgn ? "sge" : "uge");
        emit(F, "%s = icmp %s %s %s, %s", r, p, type_ll(t), a.v, b.v);
        return res;
    }
    if (ty_is_float(t)) {
        const char *p = op == TK_EQEQ ? "oeq" : op == TK_NEQ ? "une" : op == TK_LT ? "olt" : op == TK_LE ? "ole" : op == TK_GT ? "ogt" : "oge";
        emit(F, "%s = fcmp %s %s %s, %s", r, p, type_ll(t), a.v, b.v);
        return res;
    }
    if (t == ty_string) {
        char *ap, *an, *bp, *bn;
        str_parts(F, a, &ap, &an);
        str_parts(F, b, &bp, &bn);
        char *c = tmp(F);
        if (op == TK_EQEQ || op == TK_NEQ) {
            rt_decl(F, "zevic_str_eq", "declare i32 @zevic_str_eq(ptr, i64, ptr, i64)");
            emit(F, "%s = call i32 @zevic_str_eq(ptr %s, i64 %s, ptr %s, i64 %s)", c, ap, an, bp, bn);
            emit(F, "%s = icmp %s i32 %s, 0", r, op == TK_EQEQ ? "ne" : "eq", c);
        } else {
            rt_decl(F, "zevic_str_cmp", "declare i32 @zevic_str_cmp(ptr, i64, ptr, i64)");
            emit(F, "%s = call i32 @zevic_str_cmp(ptr %s, i64 %s, ptr %s, i64 %s)", c, ap, an, bp, bn);
            const char *p = op == TK_LT ? "slt" : op == TK_LE ? "sle" : op == TK_GT ? "sgt" : "sge";
            emit(F, "%s = icmp %s i32 %s, 0", r, p, c);
        }
        return res;
    }
    if (t == ty_unit) {
        Val v = {ty_bool, op == TK_EQEQ ? "true" : "false"};
        return v;
    }
    diag_error(pos, "`%s` cannot compare values of type `%s` yet", tok_name(op), t->name);
    return err_val();
}

/* Generate two operands; a bare number literal takes the other side's type. */
static void gen_pair(FnCtx *F, Node *na, Node *nb, Type *hint, Val *va, Val *vb) {
    if (is_untyped_lit(na) && !is_untyped_lit(nb)) {
        *vb = gen_expr(F, nb, hint);
        *va = gen_expr(F, na, (is_err(vb->t) || vb->t == ty_never) ? hint : vb->t);
    } else {
        *va = gen_expr(F, na, hint);
        *vb = gen_expr(F, nb, (is_err(va->t) || va->t == ty_never) ? hint : va->t);
    }
}

static Val gen_logic(FnCtx *F, Node *n) {
    bool is_and = n->op == TK_ANDAND;
    Val a = gen_expr(F, n->a, ty_bool);
    a = coerce(F, n->a->pos, a, ty_bool, "the operand of a logical operator");
    if (a.t == ty_never) return never_val(ty_bool);
    char *from = F->cur;
    char *rhs = newlabel(F, is_and ? "and.rhs" : "or.rhs");
    char *end = newlabel(F, is_and ? "and.end" : "or.end");
    if (is_and) condbr(F, a.v, rhs, end);
    else condbr(F, a.v, end, rhs);
    start_block(F, rhs);
    Val b = gen_expr(F, n->b, ty_bool);
    b = coerce(F, n->b->pos, b, ty_bool, "the operand of a logical operator");
    char *from2 = F->cur;
    bool b_live = !F->term;
    if (b_live) br(F, end);
    start_block(F, end);
    char *r = tmp(F);
    if (b_live)
        emit(F, "%s = phi i1 [ %s, %%%s ], [ %s, %%%s ]", r, is_and ? "false" : "true", from, b.v, from2);
    else
        emit(F, "%s = phi i1 [ %s, %%%s ]", r, is_and ? "false" : "true", from);
    Val res = {ty_bool, r};
    return res;
}

static Val gen_binary(FnCtx *F, Node *n, Type *expect) {
    TokKind op = n->op;
    if (op == TK_ANDAND || op == TK_OROR) return gen_logic(F, n);
    bool cmp = op == TK_EQEQ || op == TK_NEQ || op == TK_LT || op == TK_GT || op == TK_LE || op == TK_GE;
    bool shift = op == TK_SHL || op == TK_SHR;
    Val a, b;
    if (shift) {
        a = gen_expr(F, n->a, expect);
        b = gen_expr(F, n->b, ty_i32);
    } else {
        gen_pair(F, n->a, n->b, cmp ? NULL : expect, &a, &b);
    }
    if (a.t == ty_never || b.t == ty_never) return never_val(cmp ? ty_bool : expect);
    if (is_err(a.t) || is_err(b.t)) return err_val();
    if (shift) {
        if (!ty_is_int(a.t)) {
            diag_error(n->pos, "`%s` needs an integer on the left, found `%s`", tok_name(op), a.t->name);
            return err_val();
        }
        if (b.t != ty_i32) {
            diag_error(n->b->pos, "the shift amount must be `I32`, found `%s`", b.t->name);
            return err_val();
        }
        return gen_shift(F, op, a, b, n->pos);
    }
    if (a.t != b.t) {
        diag_error(n->pos, "the operands of `%s` have different types: `%s` and `%s` (convert one explicitly, e.g. with `ToI64()`)",
                   tok_name(op), a.t->name, b.t->name);
        return err_val();
    }
    if (cmp) return gen_compare(F, op, a, b, n->pos);
    Type *t = a.t;
    if (t == ty_string && op == TK_PLUS) {
        char *ap, *an, *bp, *bn;
        str_parts(F, a, &ap, &an);
        str_parts(F, b, &bp, &bn);
        rt_decl(F, "zevic_str_concat", "declare void @zevic_str_concat(ptr, ptr, i64, ptr, i64)");
        return call_str_out(F, "zevic_str_concat", zsprintf("ptr %s, i64 %s, ptr %s, i64 %s", ap, an, bp, bn));
    }
    if (ty_is_float(t)) {
        if (op == TK_PLUS || op == TK_MINUS || op == TK_STAR || op == TK_SLASH || op == TK_PERCENT)
            return gen_float_arith(F, op, a, b);
        diag_error(n->pos, "`%s` is only for integers, found `%s`", tok_name(op), t->name);
        return err_val();
    }
    if (ty_is_int(t)) return gen_int_arith(F, op, a, b, n->pos);
    diag_error(n->pos, "`%s` cannot be used with values of type `%s`", tok_name(op), t->name);
    return err_val();
}

static Val gen_unary(FnCtx *F, Node *n, Type *expect) {
    if (n->op == TK_MINUS) {
        Node *x = n->a;
        if (x->kind == N_INT) return gen_int_lit(F, x, expect, true);
        if (x->kind == N_FLOAT) return gen_float_lit(F, x, expect, true);
    }
    Val v = gen_expr(F, n->a, n->op == TK_BANG ? ty_bool : expect);
    if (v.t == ty_never || is_err(v.t)) return v;
    Type *t = v.t;
    char *r = tmp(F);
    Val res = {t, r};
    switch (n->op) {
    case TK_MINUS:
        if (ty_is_float(t)) {
            emit(F, "%s = fneg %s %s", r, type_ll(t), v.v);
            return res;
        }
        if (ty_is_signed(t)) {
            Val zero = {t, "0"};
            return gen_int_arith(F, TK_MINUS, zero, v, n->pos);
        }
        diag_error(n->pos, "cannot negate a value of type `%s`", t->name);
        return err_val();
    case TK_BANG:
        if (t != ty_bool) {
            diag_error(n->pos, "`!` needs a `Bool`, found `%s`", t->name);
            return err_val();
        }
        emit(F, "%s = xor i1 %s, true", r, v.v);
        return res;
    case TK_TILDE:
        if (!ty_is_int(t)) {
            diag_error(n->pos, "`~` needs an integer, found `%s`", t->name);
            return err_val();
        }
        emit(F, "%s = xor %s %s, -1", r, type_ll(t), v.v);
        return res;
    default:
        return err_val();
    }
}

/* =========================================================================
 * Expressions
 * ========================================================================= */

static Val gen_block(FnCtx *F, Node *blk, Type *expect);
static Val gen_call(FnCtx *F, Node *n, Type *expect);
static void gen_stmt(FnCtx *F, Node *s);

static Val gen_ident(FnCtx *F, Node *n) {
    Local *l = find_local(F, n->name);
    if (l) return load_local(F, l);
    /* a constant */
    char *seg[1] = {n->name};
    int used = 0;
    Decl *d = find_decl(F->g, F->file, seg, 1, &used, n->pos);
    if (used < 0) return err_val();
    if (d && d->node->kind == D_CONST) {
        Type *t = resolve_type(F->g, d->file, make_env(d, NULL, NULL), d->node->a);
        Val v = gen_expr(F, d->node->b, t);
        return coerce(F, d->node->b->pos, v, t, "a constant");
    }
    diag_error(n->pos, "unknown name `%s`", n->name);
    return err_val();
}

static Val gen_tuple(FnCtx *F, Node *n, Type *expect) {
    Vec vals = {0}, tys = {0};
    for (int i = 0; i < n->list.n; i++) {
        Type *e = (expect && expect->k == T_TUPLE && i < expect->args.n) ? expect->args.v[i] : NULL;
        Val *v = zalloc(sizeof *v);
        *v = gen_expr(F, n->list.v[i], e);
        if (v->t == ty_never) return never_val(expect);
        vec_push(&vals, v);
        vec_push(&tys, v->t);
    }
    for (int i = 0; i < tys.n; i++)
        if (is_err(tys.v[i])) return err_val();
    Type *tt = ty_tuple(tys);
    char *acc = "undef";
    for (int i = 0; i < vals.n; i++) {
        Val *v = vals.v[i];
        char *r = tmp(F);
        emit(F, "%s = insertvalue %s %s, %s %s, %d", r, type_ll(tt), acc, type_ll(v->t), v->v, i);
        acc = r;
    }
    Val res = {tt, acc};
    return res;
}

static Val gen_if(FnCtx *F, Node *n, Type *expect) {
    Val c = gen_expr(F, n->a, ty_bool);
    c = coerce(F, n->a->pos, c, ty_bool, "an `if` condition");
    if (c.t == ty_never) return never_val(expect);
    char *then_l = newlabel(F, "then"), *else_l = newlabel(F, "else"), *end_l = newlabel(F, "endif");
    condbr(F, c.v, then_l, n->c ? else_l : end_l);
    start_block(F, then_l);
    Type *want = (expect && expect != ty_unit) ? expect : NULL;
    Val v1 = gen_block(F, n->b, n->c ? want : ty_unit);
    char *pred1 = F->cur;
    bool live1 = !F->term;
    if (live1) br(F, end_l);
    if (!n->c) {
        start_block(F, end_l);
        return unit_val();
    }
    start_block(F, else_l);
    Type *t1 = (v1.t == ty_never) ? want : v1.t;
    Val v2;
    if (n->c->kind == N_IF) v2 = gen_if(F, n->c, t1);
    else v2 = gen_block(F, n->c, t1);
    char *pred2 = F->cur;
    bool live2 = !F->term;
    if (live2) br(F, end_l);
    start_block(F, end_l);
    if (!live1 && !live2) {
        emit(F, "unreachable");
        F->term = true;
        return never_val(want);
    }
    Type *rt = live1 ? v1.t : v2.t;
    if (live1 && live2 && v1.t != v2.t && !is_err(v1.t) && !is_err(v2.t)) {
        diag_error(n->c->pos, "the branches of this `if` have different types: `%s` and `%s`", v1.t->name, v2.t->name);
        return err_val();
    }
    if (is_err(rt)) return err_val();
    if (rt == ty_unit) return unit_val();
    char *r = tmp(F);
    if (live1 && live2)
        emit(F, "%s = phi %s [ %s, %%%s ], [ %s, %%%s ]", r, type_ll(rt), v1.v, pred1, v2.v, pred2);
    else if (live1)
        emit(F, "%s = phi %s [ %s, %%%s ]", r, type_ll(rt), v1.v, pred1);
    else
        emit(F, "%s = phi %s [ %s, %%%s ]", r, type_ll(rt), v2.v, pred2);
    Val res = {rt, r};
    return res;
}

static Val gen_block(FnCtx *F, Node *blk, Type *expect) {
    push_scope(F);
    for (int i = 0; i < blk->list.n; i++) gen_stmt(F, blk->list.v[i]);
    Val v = unit_val();
    if (blk->a) {
        v = gen_expr(F, blk->a, expect == ty_unit ? NULL : expect);
        if (expect == ty_unit && v.t != ty_never) v = unit_val();
    }
    pop_scope(F);
    if (F->term && v.t != ty_never) return never_val(expect ? expect : v.t);
    return v;
}

static Val gen_expr(FnCtx *F, Node *n, Type *expect) {
    switch (n->kind) {
    case N_INT: return gen_int_lit(F, n, expect, false);
    case N_FLOAT: return gen_float_lit(F, n, expect, false);
    case N_BOOL: {
        Val v = {ty_bool, n->ival ? "true" : "false"};
        return v;
    }
    case N_CHAR: {
        Val v = {ty_char, zsprintf("%llu", (unsigned long long)n->ival)};
        return v;
    }
    case N_BYTE: {
        Val v = {ty_u8, zsprintf("%llu", (unsigned long long)n->ival)};
        return v;
    }
    case N_STRING: return gen_string_lit(F, n);
    case N_UNIT: return unit_val();
    case N_IDENT: return gen_ident(F, n);
    case N_TUPLE: return gen_tuple(F, n, expect);
    case N_UNARY: return gen_unary(F, n, expect);
    case N_BINARY: return gen_binary(F, n, expect);
    case N_IF: return gen_if(F, n, expect);
    case N_BLOCK: return gen_block(F, n, expect);
    case N_CALL: return gen_call(F, n, expect);
    case N_RETURN: {
        Val v = unit_val();
        if (n->a) {
            v = gen_expr(F, n->a, F->ret);
            v = coerce(F, n->a->pos, v, F->ret, "a `return` value");
            if (v.t == ty_never) return v;
        } else if (F->ret != ty_unit && !is_err(F->ret)) {
            diag_error(n->pos, "this function returns `%s`, so `return` needs a value", F->ret->name);
        }
        for (Scope *s = F->scope; s; s = s->up) run_defers_in(F, s);
        if (F->ret == ty_unit || F->ret == ty_never) emit(F, "ret void");
        else emit(F, "ret %s %s", type_ll(F->ret), v.v);
        F->term = true;
        return never_val(expect);
    }
    case N_BREAK: case N_CONTINUE: {
        if (!F->loops.n) {
            diag_error(n->pos, "`%s` can only be used inside a loop", n->kind == N_BREAK ? "break" : "continue");
            return err_val();
        }
        Loop *lp = F->loops.v[F->loops.n - 1];
        for (Scope *s = F->scope; s && s != lp->scope; s = s->up) run_defers_in(F, s);
        br(F, n->kind == N_BREAK ? lp->brk : lp->cont);
        return never_val(expect);
    }
    default:
        diag_error(n->pos, "this kind of expression is not supported yet (%s)", node_kind_name(n->kind));
        return err_val();
    }
}

/* =========================================================================
 * Statements
 * ========================================================================= */

static void run_defers_in(FnCtx *F, Scope *s) {
    for (int i = s->defers.n - 1; i >= 0; i--) {
        Scope *saved = F->scope;
        Scope *t = zalloc(sizeof *t);
        t->up = s;
        F->scope = t;
        Node *d = s->defers.v[i];
        if (d->kind == N_BLOCK) {
            Val v = gen_block(F, d, ty_unit);
            (void)v;
        } else {
            gen_stmt(F, d);
        }
        F->scope = saved;
    }
}

/* Bind an irrefutable pattern to a value. */
static void bind_pattern(FnCtx *F, Node *pat, Val v, bool mut) {
    switch (pat->kind) {
    case P_WILD:
        return;
    case P_BIND: {
        if (is_err(v.t)) {
            declare_local(F, pat->name, ty_err, mut, pat->pos);
            return;
        }
        Local *l = declare_local(F, pat->name, v.t, mut, pat->pos);
        if (v.t != ty_unit) store_to(F, v.t, v, l->ptr);
        return;
    }
    case P_TUPLE: {
        if (v.t->k != T_TUPLE) {
            if (!is_err(v.t)) diag_error(pat->pos, "cannot destructure a value of type `%s` as a tuple", v.t->name);
            return;
        }
        if (v.t->args.n != pat->list.n) {
            diag_error(pat->pos, "the tuple pattern has %d element(s), but the value has %d", pat->list.n, v.t->args.n);
            return;
        }
        for (int i = 0; i < pat->list.n; i++) {
            Type *et = v.t->args.v[i];
            char *r = tmp(F);
            emit(F, "%s = extractvalue %s %s, %d", r, type_ll(v.t), v.v, i);
            Val ev = {et, r};
            bind_pattern(F, pat->list.v[i], ev, mut);
        }
        return;
    }
    default:
        diag_error(pat->pos, "this pattern can fail to match; only `_`, names and tuples can be used here");
    }
}

static TokKind base_op(TokKind k) {
    switch (k) {
    case TK_PLUSEQ: return TK_PLUS;
    case TK_MINUSEQ: return TK_MINUS;
    case TK_STAREQ: return TK_STAR;
    case TK_SLASHEQ: return TK_SLASH;
    case TK_PERCENTEQ: return TK_PERCENT;
    case TK_AMPEQ: return TK_AMP;
    case TK_PIPEEQ: return TK_PIPE;
    case TK_CARETEQ: return TK_CARET;
    case TK_SHLEQ: return TK_SHL;
    case TK_SHREQ: return TK_SHR;
    default: return TK_ASSIGN;
    }
}

/* The address of an assignable place; sets *t to its type. NULL after an error. */
static char *gen_lvalue(FnCtx *F, Node *n, Type **t) {
    if (n->kind == N_IDENT) {
        Local *l = find_local(F, n->name);
        if (!l) {
            diag_error(n->pos, "cannot assign to `%s`: it is not a variable", n->name);
            return NULL;
        }
        if (!l->mut) {
            diag_error(n->pos, "cannot assign to `%s` because it is not `mut`", n->name);
            diag_note("declare it with `mut %s: %s = ...` to allow changes", n->name, l->t->name);
            return NULL;
        }
        *t = l->t;
        return l->ptr;
    }
    diag_error(n->pos, "this expression cannot be assigned to");
    return NULL;
}

static void gen_assign(FnCtx *F, Node *n) {
    Type *t = NULL;
    char *ptr = gen_lvalue(F, n->a, &t);
    if (!ptr) {
        gen_expr(F, n->b, NULL);
        return;
    }
    if (n->op == TK_ASSIGN) {
        Val v = gen_expr(F, n->b, t);
        v = coerce(F, n->b->pos, v, t, "an assignment");
        if (v.t == ty_never || is_err(v.t)) return;
        store_to(F, t, v, ptr);
        return;
    }
    TokKind op = base_op(n->op);
    char *cur = tmp(F);
    emit(F, "%s = load %s, ptr %s", cur, type_ll(t), ptr);
    Val a = {t, cur};
    bool shift = op == TK_SHL || op == TK_SHR;
    Val b = gen_expr(F, n->b, shift ? ty_i32 : t);
    if (b.t == ty_never || is_err(b.t) || is_err(t)) return;
    Val r;
    if (shift) {
        if (!ty_is_int(t) || b.t != ty_i32) {
            diag_error(n->pos, "`%s` needs an integer variable and an `I32` amount", tok_name(n->op));
            return;
        }
        r = gen_shift(F, op, a, b, n->pos);
    } else {
        b = coerce(F, n->b->pos, b, t, "a compound assignment");
        if (is_err(b.t)) return;
        if (t == ty_string && op == TK_PLUS) {
            char *ap, *an, *bp, *bn;
            str_parts(F, a, &ap, &an);
            str_parts(F, b, &bp, &bn);
            rt_decl(F, "zevic_str_concat", "declare void @zevic_str_concat(ptr, ptr, i64, ptr, i64)");
            r = call_str_out(F, "zevic_str_concat", zsprintf("ptr %s, i64 %s, ptr %s, i64 %s", ap, an, bp, bn));
        } else if (ty_is_float(t) && op != TK_AMP && op != TK_PIPE && op != TK_CARET) {
            r = gen_float_arith(F, op, a, b);
        } else if (ty_is_int(t)) {
            r = gen_int_arith(F, op, a, b, n->pos);
        } else {
            diag_error(n->pos, "`%s` cannot be used with values of type `%s`", tok_name(n->op), t->name);
            return;
        }
    }
    store_to(F, t, r, ptr);
}

static void gen_while(FnCtx *F, Node *n) {
    char *head = newlabel(F, "while.cond"), *body = newlabel(F, "while.body"), *end = newlabel(F, "while.end");
    goto_block(F, head);
    Val c = gen_expr(F, n->a, ty_bool);
    c = coerce(F, n->a->pos, c, ty_bool, "a `while` condition");
    if (c.t != ty_never) condbr(F, c.v, body, end);
    start_block(F, body);
    Loop lp = {end, head, F->scope};
    vec_push(&F->loops, &lp);
    gen_block(F, n->b, ty_unit);
    F->loops.n--;
    if (!F->term) br(F, head);
    start_block(F, end);
}

static void gen_loop(FnCtx *F, Node *n) {
    char *body = newlabel(F, "loop.body"), *end = newlabel(F, "loop.end");
    goto_block(F, body);
    Loop lp = {end, body, F->scope};
    vec_push(&F->loops, &lp);
    gen_block(F, n->b, ty_unit);
    F->loops.n--;
    if (!F->term) br(F, body);
    start_block(F, end);
}

static void gen_for_range(FnCtx *F, Node *n) {
    Node *r = n->d;
    Val a, b;
    gen_pair(F, r->a, r->b, NULL, &a, &b);
    if (a.t == ty_never || b.t == ty_never || is_err(a.t) || is_err(b.t)) return;
    if (!ty_is_int(a.t) || a.t != b.t) {
        diag_error(r->pos, "the bounds of a range must be integers of the same type, found `%s` and `%s`", a.t->name, b.t->name);
        return;
    }
    Type *t = a.t;
    char *ll = type_ll(t);
    bool sgn = ty_is_signed(t);
    push_scope(F);
    char *ctr = new_slot(F, t);
    store_to(F, t, a, ctr);
    char *head = newlabel(F, "for.cond"), *body = newlabel(F, "for.body"), *cont = newlabel(F, "for.next"),
         *end = newlabel(F, "for.end");
    goto_block(F, head);
    char *cur = tmp(F), *c = tmp(F);
    emit(F, "%s = load %s, ptr %s", cur, ll, ctr);
    const char *pred = r->flag ? (sgn ? "sle" : "ule") : (sgn ? "slt" : "ult");
    emit(F, "%s = icmp %s %s %s, %s", c, pred, ll, cur, b.v);
    condbr(F, c, body, end);
    start_block(F, body);
    /* the loop variable is an immutable alias of the counter */
    if (n->a->kind == P_BIND) {
        Local *l = zalloc(sizeof *l);
        l->name = n->a->name;
        l->t = t;
        l->ptr = ctr;
        l->mut = false;
        l->pos = n->a->pos;
        vec_push(&F->scope->locals, l);
    } else if (n->a->kind != P_WILD) {
        diag_error(n->a->pos, "a range loop variable must be a name or `_`");
    }
    if (n->c) diag_error(n->c->pos, "a range has no index; use a single loop variable");
    Loop lp = {end, cont, F->scope->up};
    vec_push(&F->loops, &lp);
    gen_block(F, n->b, ty_unit);
    F->loops.n--;
    if (!F->term) br(F, cont);
    start_block(F, cont);
    char *cur2 = tmp(F);
    emit(F, "%s = load %s, ptr %s", cur2, ll, ctr);
    if (r->flag) {
        /* inclusive: stop after the upper bound without overflowing */
        char *last = tmp(F);
        char *step = newlabel(F, "for.step");
        emit(F, "%s = icmp eq %s %s, %s", last, ll, cur2, b.v);
        condbr(F, last, end, step);
        start_block(F, step);
    }
    char *nx = tmp(F);
    emit(F, "%s = add %s %s, 1", nx, ll, cur2);
    store_to(F, t, (Val){t, nx}, ctr);
    br(F, head);
    start_block(F, end);
    pop_scope(F);
}

static void gen_for(FnCtx *F, Node *n) {
    if (n->d->kind == N_RANGE) {
        gen_for_range(F, n);
        return;
    }
    diag_error(n->d->pos, "`for` can only loop over a range (like `0..10`) so far");
}

static void gen_let(FnCtx *F, Node *n) {
    Type *t = resolve_type(F->g, F->file, F->env, n->b);
    Val v = gen_expr(F, n->c, t);
    v = coerce(F, n->c->pos, v, t, "a variable declaration");
    if (v.t == ty_never) {
        /* the initialiser never completes; still declare the names so later code can be checked */
        v = (Val){t, "undef"};
    }
    if (is_err(v.t)) v = (Val){t, "undef"};
    bind_pattern(F, n->a, v, n->flag);
}

static void gen_stmt(FnCtx *F, Node *s) {
    switch (s->kind) {
    case N_LET: gen_let(F, s); break;
    case N_ASSIGN: gen_assign(F, s); break;
    case N_EXPRSTMT: gen_expr(F, s->a, NULL); break;
    case N_WHILE: gen_while(F, s); break;
    case N_LOOP: gen_loop(F, s); break;
    case N_FOR: gen_for(F, s); break;
    case N_DEFER: vec_push(&F->scope->defers, s->a); break;
    default: gen_expr(F, s, NULL); break;
    }
}

/* =========================================================================
 * Calls
 * ========================================================================= */

static bool flatten_path(Node *n, Vec *segs, Vec *nodes) {
    if (n->kind == N_IDENT) {
        vec_push(segs, n->name);
        vec_push(nodes, n);
        return true;
    }
    if (n->kind == N_MEMBER && flatten_path(n->a, segs, nodes)) {
        vec_push(segs, n->name);
        vec_push(nodes, n);
        return true;
    }
    return false;
}

static bool is_fn_tparam(Node *fn, const char *name) {
    for (int i = 0; i < fn->list2.n; i++)
        if (strcmp(((Node *)fn->list2.v[i])->name, name) == 0) return true;
    return false;
}

/* Does this type syntax mention a type parameter of `fn` that is not bound yet? */
static bool syntax_uses_unbound(Node *te, Node *fn, Env *env) {
    switch (te->kind) {
    case N_TYPE_NAME:
        if (te->path.n == 1 && is_fn_tparam(fn, te->path.v[0]) && !env_get(env, te->path.v[0])) return true;
        for (int i = 0; i < te->list.n; i++)
            if (syntax_uses_unbound(te->list.v[i], fn, env)) return true;
        return false;
    case N_TYPE_TUPLE:
        for (int i = 0; i < te->list.n; i++)
            if (syntax_uses_unbound(te->list.v[i], fn, env)) return true;
        return false;
    case N_TYPE_FN:
        for (int i = 0; i < te->list.n; i++)
            if (syntax_uses_unbound(te->list.v[i], fn, env)) return true;
        return syntax_uses_unbound(te->a, fn, env);
    case N_TYPE_PTR:
        return syntax_uses_unbound(te->a, fn, env);
    default:
        return false;
    }
}

/* Bind the type parameters in `te` by matching it against `actual`. */
static void unify(Node *te, Type *actual, Node *fn, Env *env) {
    if (is_err(actual) || actual == ty_never) return;
    switch (te->kind) {
    case N_TYPE_NAME:
        if (te->path.n == 1 && is_fn_tparam(fn, te->path.v[0])) {
            if (!env_get(env, te->path.v[0])) env_add(env, te->path.v[0], actual);
            return;
        }
        if (strcmp(te->path.v[te->path.n - 1], "Array") == 0 && actual->k == T_ARRAY && te->list.n == 1) {
            unify(te->list.v[0], actual->args.v[0], fn, env);
            return;
        }
        if (actual->decl && te->list.n == actual->args.n)
            for (int i = 0; i < te->list.n; i++) unify(te->list.v[i], actual->args.v[i], fn, env);
        return;
    case N_TYPE_TUPLE:
        if (actual->k == T_TUPLE && actual->args.n == te->list.n)
            for (int i = 0; i < te->list.n; i++) unify(te->list.v[i], actual->args.v[i], fn, env);
        return;
    case N_TYPE_FN:
        if (actual->k == T_FN && actual->args.n == te->list.n) {
            for (int i = 0; i < te->list.n; i++) unify(te->list.v[i], actual->args.v[i], fn, env);
            unify(te->a, actual->ret, fn, env);
        }
        return;
    case N_TYPE_PTR:
        if (actual->k == T_PTR) unify(te->a, actual->args.v[0], fn, env);
        return;
    default:
        return;
    }
}

/* The pointer part of a string constant `{ ptr @x, i64 n }`. */
static char *const_ptr(const char *c) {
    return zstrndup(c + 6, (size_t)(strchr(c, ',') - (c + 6)));
}

static void emit_panic_call(FnCtx *F, Val msg, Pos pos) {
    declare(F->g, "zevic_panic", "declare void @zevic_panic(ptr, i64, ptr, i32) noreturn");
    char *p, *n;
    str_parts(F, msg, &p, &n);
    const char *file = src_name(pos.file);
    char *fc = str_const(F->g, file, (int)strlen(file));
    emit(F, "call void @zevic_panic(ptr %s, i64 %s, ptr %s, i32 %d)", p, n, const_ptr(fc), pos.line);
    emit(F, "unreachable");
    F->term = true;
}

/* Emit a direct call to a function instance. */
static Val emit_call(FnCtx *F, Inst *in, Vec *args) {
    Gen *g = F->g;
    SB a = {0};
    Type *ret = in->ret;
    char *slot = NULL;
    bool zevic_abi = in->is_extern && in->decl->abi && strcmp(in->decl->abi, "zevic") == 0;
    if (in->is_extern && !zevic_abi) {
        diag_error(in->decl->node->pos, "`extern \"%s\"` functions are not supported yet", in->decl->abi);
        return err_val();
    }
    if (zevic_abi) {
        SB ptypes = {0};
        if (ret == ty_string) {
            slot = new_slot(F, ty_string);
            sb_printf(&a, "ptr %s", slot);
            sb_puts(&ptypes, "ptr");
        }
        for (int i = 0; i < args->n; i++) {
            Val *v = args->v[i];
            if (v->t == ty_unit) continue;
            if (a.len) sb_puts(&a, ", ");
            if (ptypes.len) sb_puts(&ptypes, ", ");
            if (v->t == ty_string) {
                char *p, *n;
                str_parts(F, *v, &p, &n);
                sb_printf(&a, "ptr %s, i64 %s", p, n);
                sb_puts(&ptypes, "ptr, i64");
            } else {
                sb_printf(&a, "%s %s", type_ll(v->t), v->v);
                sb_puts(&ptypes, type_ll(v->t));
            }
        }
        const char *rl = (ret == ty_string || ret == ty_unit || ret == ty_never) ? "void" : type_ll(ret);
        declare(g, in->name, zsprintf("declare %s @%s(%s)", rl, in->name, sb_str(&ptypes)));
        if (ret == ty_string || ret == ty_unit || ret == ty_never) {
            emit(F, "call void @%s(%s)", in->name, sb_str(&a));
            if (ret == ty_never) {
                emit(F, "unreachable");
                F->term = true;
                return never_val(NULL);
            }
            if (ret == ty_unit) return unit_val();
            char *t = tmp(F);
            emit(F, "%s = load { ptr, i64 }, ptr %s", t, slot);
            Val r = {ty_string, t};
            return r;
        }
        char *t = tmp(F);
        emit(F, "%s = call %s @%s(%s)", t, type_ll(ret), in->name, sb_str(&a));
        Val r = {ret, t};
        return r;
    }
    for (int i = 0; i < args->n; i++) {
        Val *v = args->v[i];
        if (i) sb_puts(&a, ", ");
        sb_printf(&a, "%s %s", type_ll(v->t), v->v);
    }
    if (ret == ty_unit || ret == ty_never) {
        emit(F, "call void @\"%s\"(%s)", in->name, sb_str(&a));
        if (ret == ty_never) {
            emit(F, "unreachable");
            F->term = true;
            return never_val(NULL);
        }
        return unit_val();
    }
    char *t = tmp(F);
    emit(F, "%s = call %s @\"%s\"(%s)", t, type_ll(ret), in->name, sb_str(&a));
    Val r = {ret, t};
    return r;
}

/* Call function `d` with the arguments of `call`, inferring generic arguments. */
static Val gen_call_decl(FnCtx *F, Node *call, Decl *d, Type *self, Vec *explicit, Type *expect) {
    Gen *g = F->g;
    Node *fn = d->node;
    int ntp = fn->list2.n;
    Env *env = make_env(d, self, NULL);
    if (explicit && explicit->n) {
        if (explicit->n != ntp) {
            diag_error(call->pos, "`%s` takes %d type argument(s), found %d", fn->name, ntp, explicit->n);
            return err_val();
        }
        for (int i = 0; i < ntp; i++)
            env_add(env, ((Node *)fn->list2.v[i])->name, resolve_type(g, F->file, F->env, explicit->v[i]));
    }
    int nparams = 0;
    for (int i = 0; i < fn->list.n; i++)
        if (!((Node *)fn->list.v[i])->flag3) nparams++;
    if (call->list.n != nparams) {
        diag_error(call->pos, "`%s` takes %d argument(s), but %d were given", fn->name, nparams, call->list.n);
        return err_val();
    }
    Vec vals = {0};
    bool never = false;
    for (int i = 0; i < nparams; i++) {
        Node *pn = fn->list.v[i];
        Type *pexp = NULL;
        if (!syntax_uses_unbound(pn->a, fn, env)) pexp = resolve_type(g, d->file, env, pn->a);
        Val *v = zalloc(sizeof *v);
        *v = gen_expr(F, call->list.v[i], pexp);
        if (v->t == ty_never) never = true;
        else if (!pexp) unify(pn->a, v->t, fn, env);
        vec_push(&vals, v);
    }
    if (never) return never_val(expect);
    Vec targs = {0};
    for (int i = 0; i < ntp; i++) {
        Node *tp = fn->list2.v[i];
        Type *t = env_get(env, tp->name);
        if (!t && expect && fn->a) {
            unify(fn->a, expect, fn, env);
            t = env_get(env, tp->name);
        }
        if (!t) {
            diag_error(call->pos, "cannot infer the type parameter `%s` of `%s` (write it explicitly, like `%s<I32>(...)`)",
                       tp->name, fn->name, fn->name);
            return err_val();
        }
        vec_push(&targs, t);
    }
    Inst *in = get_inst(g, d, self, &targs);
    for (int i = 0; i < nparams; i++) {
        Val *v = vals.v[i];
        char *what = zsprintf("argument %d of `%s`", i + 1, fn->name);
        *v = coerce(F, ((Node *)call->list.v[i])->pos, *v, in->ptypes.v[i], what);
        if (is_err(v->t)) return err_val();
    }
    return emit_call(F, in, &vals);
}

/* ---- Printf ---- */

typedef struct {
    bool is_spec;
    char *text; /* literal text, or the C format fragment (e.g. "%5lld") */
    int tlen;
    char conv;
} FmtPiece;

static Val gen_printf(FnCtx *F, Node *n, bool to_err) {
    if (n->list.n < 1 || ((Node *)n->list.v[0])->kind != N_STRING) {
        diag_error(n->pos, "`Printf` needs a string literal as its format");
        return err_val();
    }
    Node *fs = n->list.v[0];
    SB fmt = {0};
    for (int i = 0; i < fs->list.n; i++) {
        Node *part = fs->list.v[i];
        if (part->kind != N_STR_LIT) {
            diag_error(fs->pos, "the format of `Printf` cannot contain `${...}`; pass the values as arguments");
            return err_val();
        }
        sb_putn(&fmt, part->name, (size_t)part->slen);
    }
    Vec pieces = {0};
    SB lit = {0};
    char *f = sb_str(&fmt);
    size_t len = fmt.len;
    for (size_t i = 0; i < len; i++) {
        if (f[i] != '%') {
            sb_putc(&lit, f[i]);
            continue;
        }
        if (i + 1 < len && f[i + 1] == '%') {
            sb_putc(&lit, '%');
            i++;
            continue;
        }
        if (lit.len) {
            FmtPiece *p = zalloc(sizeof *p);
            p->text = zstrndup(lit.p, lit.len);
            p->tlen = (int)lit.len;
            vec_push(&pieces, p);
            lit.len = 0;
        }
        SB flags = {0}, width = {0}, prec = {0};
        size_t j = i + 1;
        while (j < len && strchr("-+ 0#", f[j])) sb_putc(&flags, f[j++]);
        while (j < len && f[j] >= '0' && f[j] <= '9') sb_putc(&width, f[j++]);
        bool has_prec = false;
        if (j < len && f[j] == '.') {
            has_prec = true;
            j++;
            while (j < len && f[j] >= '0' && f[j] <= '9') sb_putc(&prec, f[j++]);
        }
        if (j >= len || !strchr("duxXfscDi", f[j])) {
            diag_error(fs->pos, "invalid format specifier in `Printf` (use %%d %%u %%x %%f %%s %%c or %%%%)");
            return err_val();
        }
        char conv = f[j] == 'i' ? 'd' : f[j];
        FmtPiece *p = zalloc(sizeof *p);
        p->is_spec = true;
        p->conv = conv;
        const char *length = (conv == 'd' || conv == 'u' || conv == 'x' || conv == 'X') ? "ll" : "";
        char cconv = (conv == 'c') ? 's' : conv;
        p->text = zsprintf("%%%s%s%s%s%s%c", sb_str(&flags), sb_str(&width), has_prec ? "." : "",
                           has_prec ? (prec.len ? sb_str(&prec) : "0") : "", length, cconv);
        p->tlen = (int)strlen(p->text);
        vec_push(&pieces, p);
        i = j;
    }
    if (lit.len) {
        FmtPiece *p = zalloc(sizeof *p);
        p->text = zstrndup(lit.p, lit.len);
        p->tlen = (int)lit.len;
        vec_push(&pieces, p);
    }
    int nspecs = 0;
    for (int i = 0; i < pieces.n; i++)
        if (((FmtPiece *)pieces.v[i])->is_spec) nspecs++;
    if (nspecs != n->list.n - 1) {
        diag_error(n->pos, "the format has %d placeholder(s), but %d value(s) were given", nspecs, n->list.n - 1);
        return err_val();
    }
    /* evaluate the arguments first, in order */
    Vec vals = {0};
    int ai = 1;
    for (int i = 0; i < pieces.n; i++) {
        FmtPiece *p = pieces.v[i];
        if (!p->is_spec) continue;
        Node *an = n->list.v[ai++];
        Type *hint = (p->conv == 'f') ? ty_f64 : NULL;
        Val *v = zalloc(sizeof *v);
        *v = gen_expr(F, an, hint);
        if (v->t == ty_never) return never_val(NULL);
        bool ok = true;
        const char *need = "";
        switch (p->conv) {
        case 'd': case 'u': case 'x': case 'X': ok = ty_is_int(v->t); need = "an integer"; break;
        case 'f': ok = ty_is_float(v->t); need = "a decimal number"; break;
        case 's': ok = v->t == ty_string; need = "a `String`"; break;
        case 'c': ok = v->t == ty_char; need = "a `Char`"; break;
        default: break;
        }
        if (!ok && !is_err(v->t)) {
            diag_error(an->pos, "`%%%c` needs %s, found `%s`", p->conv, need, v->t->name);
            return err_val();
        }
        vec_push(&vals, v);
    }
    int stream = to_err ? 2 : 1;
    rt_decl(F, "zevic_printf_lit", "declare void @zevic_printf_lit(i32, ptr, i64)");
    rt_decl(F, "zevic_printf_i", "declare void @zevic_printf_i(i32, ptr, i64)");
    rt_decl(F, "zevic_printf_f", "declare void @zevic_printf_f(i32, ptr, double)");
    rt_decl(F, "zevic_printf_s", "declare void @zevic_printf_s(i32, ptr, ptr, i64)");
    rt_decl(F, "zevic_printf_c", "declare void @zevic_printf_c(i32, ptr, i32)");
    int vi = 0;
    for (int i = 0; i < pieces.n; i++) {
        FmtPiece *p = pieces.v[i];
        char *c = str_const(F->g, p->text, p->tlen);
        if (!p->is_spec) {
            emit(F, "call void @zevic_printf_lit(i32 %d, ptr %s, i64 %d)", stream, const_ptr(c), p->tlen);
            continue;
        }
        Val *v = vals.v[vi++];
        if (is_err(v->t)) continue;
        if (p->conv == 'd' || p->conv == 'u' || p->conv == 'x' || p->conv == 'X') {
            char *x = v->v;
            int bits = ty_bits(v->t);
            if (bits < 64) {
                x = tmp(F);
                emit(F, "%s = %s i%d %s to i64", x, ty_is_signed(v->t) ? "sext" : "zext", bits, v->v);
            }
            emit(F, "call void @zevic_printf_i(i32 %d, ptr %s, i64 %s)", stream, const_ptr(c), x);
        } else if (p->conv == 'f') {
            char *x = v->v;
            if (v->t == ty_f32) {
                x = tmp(F);
                emit(F, "%s = fpext float %s to double", x, v->v);
            }
            emit(F, "call void @zevic_printf_f(i32 %d, ptr %s, double %s)", stream, const_ptr(c), x);
        } else if (p->conv == 's') {
            char *sp, *sn;
            str_parts(F, *v, &sp, &sn);
            emit(F, "call void @zevic_printf_s(i32 %d, ptr %s, ptr %s, i64 %s)", stream, const_ptr(c), sp, sn);
        } else {
            emit(F, "call void @zevic_printf_c(i32 %d, ptr %s, i32 %s)", stream, const_ptr(c), v->v);
        }
    }
    return unit_val();
}

/* ---- Panic and Assert ---- */

static Val gen_panic_builtin(FnCtx *F, Node *n) {
    if (n->list.n != 1) {
        diag_error(n->pos, "`Panic` takes one argument, the message");
        return err_val();
    }
    Val m = gen_expr(F, n->list.v[0], ty_string);
    m = coerce(F, ((Node *)n->list.v[0])->pos, m, ty_string, "the message of `Panic`");
    if (m.t == ty_never || is_err(m.t)) return m.t == ty_never ? m : err_val();
    emit_panic_call(F, m, n->pos);
    return never_val(NULL);
}

static Val gen_assert_builtin(FnCtx *F, Node *n) {
    if (n->list.n != 2) {
        diag_error(n->pos, "`Assert` takes two arguments: the condition and the message");
        return err_val();
    }
    Val c = gen_expr(F, n->list.v[0], ty_bool);
    c = coerce(F, ((Node *)n->list.v[0])->pos, c, ty_bool, "the condition of `Assert`");
    if (c.t == ty_never) return c;
    if (is_err(c.t)) return err_val();
    char *fail = newlabel(F, "assert.fail"), *ok = newlabel(F, "assert.ok");
    condbr(F, c.v, ok, fail);
    start_block(F, fail);
    Val m = gen_expr(F, n->list.v[1], ty_string);
    m = coerce(F, ((Node *)n->list.v[1])->pos, m, ty_string, "the message of `Assert`");
    if (m.t != ty_never && !is_err(m.t)) emit_panic_call(F, m, n->pos);
    else if (!F->term) {
        emit(F, "unreachable");
        F->term = true;
    }
    start_block(F, ok);
    return unit_val();
}

/* ---- methods on built-in types ---- */

static bool int_fits(Type *from, Type *to) {
    int fb = ty_bits(from), tb = ty_bits(to);
    if (ty_is_signed(from) == ty_is_signed(to)) return tb >= fb;
    if (!ty_is_signed(from) && ty_is_signed(to)) return tb > fb;
    return false;
}

static Val cast_int(FnCtx *F, Val v, Type *to) {
    int fb = ty_bits(v.t), tb = ty_bits(to);
    if (fb == tb) {
        Val r = {to, v.v};
        return r;
    }
    char *r = tmp(F);
    if (tb < fb) emit(F, "%s = trunc %s %s to %s", r, type_ll(v.t), v.v, type_ll(to));
    else emit(F, "%s = %s %s %s to %s", r, ty_is_signed(v.t) ? "sext" : "zext", type_ll(v.t), v.v, type_ll(to));
    Val res = {to, r};
    return res;
}

static Val cast_float_to_int(FnCtx *F, Val v, Type *to, Pos pos) {
    int bits = ty_bits(to);
    bool sgn = ty_is_signed(to);
    char *x = v.v;
    if (v.t == ty_f32) {
        x = tmp(F);
        emit(F, "%s = fpext float %s to double", x, v.v);
    }
    double lo = sgn ? -ldexp(1.0, bits - 1) : 0.0;
    double hi = ldexp(1.0, sgn ? bits - 1 : bits);
    char *c1 = tmp(F), *c2 = tmp(F), *both = tmp(F);
    emit(F, "%s = fcmp oge double %s, %s", c1, x, double_hex(lo));
    emit(F, "%s = fcmp olt double %s, %s", c2, x, double_hex(hi));
    emit(F, "%s = and i1 %s, %s", both, c1, c2);
    char *ok = newlabel(F, "ok"), *bad = cold_panic(F, "conversion from a decimal number is out of range (or NaN)", pos);
    condbr(F, both, ok, bad);
    start_block(F, ok);
    char *r = tmp(F);
    emit(F, "%s = %s double %s to %s", r, sgn ? "fptosi" : "fptoui", x, type_ll(to));
    Val res = {to, r};
    return res;
}

static Val cast_to_float(FnCtx *F, Val v, Type *to) {
    char *r = tmp(F);
    if (ty_is_int(v.t)) emit(F, "%s = %s %s %s to %s", r, ty_is_signed(v.t) ? "sitofp" : "uitofp", type_ll(v.t), v.v, type_ll(to));
    else if (v.t == to) return v;
    else emit(F, "%s = %s %s %s to %s", r, to == ty_f64 ? "fpext" : "fptrunc", type_ll(v.t), v.v, type_ll(to));
    Val res = {to, r};
    return res;
}

static Val gen_prim_method(FnCtx *F, Node *call, Val recv, const char *name, Type *expect) {
    (void)expect;
    Type *t = recv.t;
    int nargs = call->list.n;
    if (strcmp(name, "ToString") == 0) {
        if (nargs) {
            diag_error(call->pos, "`ToString` takes no arguments");
            return err_val();
        }
        return gen_to_string(F, recv, call->pos);
    }
    /* numeric conversions: ToI32, ToU8Wrap, ToF64, ... */
    if (name[0] == 'T' && name[1] == 'o' && (ty_is_numeric(t))) {
        char *base = zstrdup(name + 2);
        bool wrap = false;
        size_t bl = strlen(base);
        if (bl > 4 && strcmp(base + bl - 4, "Wrap") == 0) {
            wrap = true;
            base[bl - 4] = 0;
        }
        Type *to = ty_prim_by_name(base);
        if (to && ty_is_numeric(to)) {
            if (nargs) {
                diag_error(call->pos, "`%s` takes no arguments", name);
                return err_val();
            }
            if (ty_is_float(to)) {
                if (wrap) {
                    diag_error(call->pos, "`%s` does not exist (only integer conversions have a `Wrap` form)", name);
                    return err_val();
                }
                return cast_to_float(F, recv, to);
            }
            if (ty_is_int(t)) {
                if (wrap || int_fits(t, to)) return cast_int(F, recv, to);
                diag_error(call->pos,
                           "`%s` may not fit in `%s`, so it returns a `Result` (not available yet); use `%sWrap()` to wrap around",
                           name, to->name, name);
                return err_val();
            }
            if (wrap) {
                diag_error(call->pos, "`%s` does not exist for `%s`", name, t->name);
                return err_val();
            }
            return cast_float_to_int(F, recv, to, call->pos);
        }
    }
    if (ty_is_int(t) && (strcmp(name, "Min") == 0 || strcmp(name, "Max") == 0)) {
        if (nargs != 1) {
            diag_error(call->pos, "`%s` takes one argument", name);
            return err_val();
        }
        Val o = gen_expr(F, call->list.v[0], t);
        o = coerce(F, ((Node *)call->list.v[0])->pos, o, t, "the argument");
        if (o.t == ty_never || is_err(o.t)) return o.t == ty_never ? o : err_val();
        char *c = tmp(F), *r = tmp(F);
        bool sgn = ty_is_signed(t);
        const char *pred = strcmp(name, "Min") == 0 ? (sgn ? "slt" : "ult") : (sgn ? "sgt" : "ugt");
        emit(F, "%s = icmp %s %s %s, %s", c, pred, type_ll(t), recv.v, o.v);
        emit(F, "%s = select i1 %s, %s %s, %s %s", r, c, type_ll(t), recv.v, type_ll(t), o.v);
        Val res = {t, r};
        return res;
    }
    if (ty_is_int(t) && strcmp(name, "Abs") == 0 && !nargs) {
        if (!ty_is_signed(t)) return recv;
        char *neg = tmp(F), *r = tmp(F);
        Val zero = {t, "0"};
        Val n = gen_int_arith(F, TK_MINUS, zero, recv, call->pos);
        neg = n.v;
        emit(F, "%s = icmp slt %s %s, 0", r, type_ll(t), recv.v);
        char *sel = tmp(F);
        emit(F, "%s = select i1 %s, %s %s, %s %s", sel, r, type_ll(t), neg, type_ll(t), recv.v);
        Val res = {t, sel};
        return res;
    }
    if (ty_is_float(t) && strcmp(name, "Abs") == 0 && !nargs) {
        char *r = tmp(F);
        const char *intr = t == ty_f64 ? "llvm.fabs.f64" : "llvm.fabs.f32";
        declare(F->g, intr, zsprintf("declare %s @%s(%s)", type_ll(t), intr, type_ll(t)));
        emit(F, "%s = call %s @%s(%s %s)", r, type_ll(t), intr, type_ll(t), recv.v);
        Val res = {t, r};
        return res;
    }
    if (ty_is_float(t) && (strcmp(name, "IsNaN") == 0 || strcmp(name, "IsInf") == 0) && !nargs) {
        char *r = tmp(F);
        if (name[2] == 'N') {
            emit(F, "%s = fcmp uno %s %s, %s", r, type_ll(t), recv.v, recv.v);
        } else {
            const char *intr = t == ty_f64 ? "llvm.fabs.f64" : "llvm.fabs.f32";
            declare(F->g, intr, zsprintf("declare %s @%s(%s)", type_ll(t), intr, type_ll(t)));
            char *a = tmp(F);
            emit(F, "%s = call %s @%s(%s %s)", a, type_ll(t), intr, type_ll(t), recv.v);
            emit(F, "%s = fcmp oeq %s %s, 0x7FF0000000000000", r, type_ll(t), a);
        }
        Val res = {ty_bool, r};
        return res;
    }
    if (t == ty_string) {
        if (strcmp(name, "Len") == 0 && !nargs) {
            char *p, *n;
            str_parts(F, recv, &p, &n);
            Val res = {ty_i64, n};
            return res;
        }
        if (strcmp(name, "IsEmpty") == 0 && !nargs) {
            char *p, *n;
            str_parts(F, recv, &p, &n);
            char *r = tmp(F);
            emit(F, "%s = icmp eq i64 %s, 0", r, n);
            Val res = {ty_bool, r};
            return res;
        }
        if (strcmp(name, "CharCount") == 0 && !nargs) {
            char *p, *n;
            str_parts(F, recv, &p, &n);
            rt_decl(F, "zevic_str_char_count", "declare i64 @zevic_str_char_count(ptr, i64)");
            char *r = tmp(F);
            emit(F, "%s = call i64 @zevic_str_char_count(ptr %s, i64 %s)", r, p, n);
            Val res = {ty_i64, r};
            return res;
        }
        if (strcmp(name, "ByteAt") == 0 && nargs == 1) {
            Val i = gen_expr(F, call->list.v[0], ty_i64);
            i = coerce(F, ((Node *)call->list.v[0])->pos, i, ty_i64, "the argument of `ByteAt`");
            if (i.t == ty_never || is_err(i.t)) return i.t == ty_never ? i : err_val();
            char *p, *n;
            str_parts(F, recv, &p, &n);
            char *bad = tmp(F);
            emit(F, "%s = icmp uge i64 %s, %s", bad, i.v, n);
            char *ok = newlabel(F, "ok");
            condbr(F, bad, cold_panic(F, "string index out of range", call->pos), ok);
            start_block(F, ok);
            char *gp = tmp(F), *r = tmp(F);
            emit(F, "%s = getelementptr inbounds i8, ptr %s, i64 %s", gp, p, i.v);
            emit(F, "%s = load i8, ptr %s", r, gp);
            Val res = {ty_u8, r};
            return res;
        }
    }
    diag_error(call->pos, "type `%s` has no method `%s` (or it takes different arguments)", t->name, name);
    return err_val();
}

static Val gen_method_call(FnCtx *F, Node *call, Node *recv_node, const char *name, Type *expect) {
    Val recv = gen_expr(F, recv_node, NULL);
    if (recv.t == ty_never) return recv;
    if (is_err(recv.t)) {
        for (int i = 0; i < call->list.n; i++) gen_expr(F, call->list.v[i], NULL);
        return err_val();
    }
    return gen_prim_method(F, call, recv, name, expect);
}

static Val gen_call(FnCtx *F, Node *n, Type *expect) {
    Node *callee = n->a;
    Vec segs = {0}, nodes = {0};
    if (flatten_path(callee, &segs, &nodes)) {
        char *head = segs.v[0];
        bool value_head = find_local(F, head) != NULL;
        if (!value_head) {
            char **sg = (char **)segs.v;
            int used = 0;
            Decl *d = find_decl(F->g, F->file, sg, segs.n, &used, callee->pos);
            if (used < 0) return err_val();
            if (d && d->node->kind == D_FN && used == segs.n) {
                Node *last = nodes.v[nodes.n - 1];
                if (d->abi && strcmp(d->abi, "intrinsic") == 0) {
                    if (strcmp(d->name, "Printf") == 0) return gen_printf(F, n, false);
                    if (strcmp(d->name, "Eprintf") == 0) return gen_printf(F, n, true);
                    diag_error(n->pos, "unknown intrinsic `%s`", d->name);
                    return err_val();
                }
                return gen_call_decl(F, n, d, NULL, &last->list2, expect);
            }
            if (d && d->node->kind == D_FN) {
                diag_error(callee->pos, "function `%s` has no member `%s`", d->name, segs.v[used]);
                return err_val();
            }
            if (!d && segs.n == 1) {
                if (strcmp(head, "Panic") == 0) return gen_panic_builtin(F, n);
                if (strcmp(head, "Assert") == 0) return gen_assert_builtin(F, n);
            }
            if (d) {
                diag_error(callee->pos, "calling `%s` this way is not supported yet", path_join(&segs, "."));
                return err_val();
            }
            if (callee->kind == N_IDENT) {
                diag_error(callee->pos, "unknown function `%s`", head);
                return err_val();
            }
            /* a.b(...) where `a` is not a module or type: fall through to a method call on a value */
        }
    }
    if (callee->kind == N_MEMBER) return gen_method_call(F, n, callee->a, callee->name, expect);
    diag_error(callee->pos, "this expression cannot be called");
    return err_val();
}

/* =========================================================================
 * Functions and the whole program
 * ========================================================================= */

static void emit_function(Gen *g, Inst *in) {
    if (in->emitted) return;
    in->emitted = true;
    Node *fn = in->decl->node;
    FnCtx F = {0};
    F.g = g;
    F.inst = in;
    F.file = in->decl->file;
    F.env = in->env;
    F.ret = in->ret;
    F.cur = "entry";
    push_scope(&F);
    SB params = {0};
    for (int i = 0; i < in->ptypes.n; i++) {
        Node *pn = fn->list.v[i];
        Type *pt = in->ptypes.v[i];
        if (i) sb_puts(&params, ", ");
        sb_printf(&params, "%s %%p%d", type_ll(pt), i);
        Local *l = declare_local(&F, pn->name, pt, pn->flag, pn->pos);
        char *pv = zsprintf("%%p%d", i);
        Val v = {pt, pv};
        if (pt != ty_unit) store_to(&F, pt, v, l->ptr);
    }
    if (!fn->b) {
        diag_error(fn->pos, "function `%s` has no body", fn->name);
        return;
    }
    Val v = gen_block(&F, fn->b, in->ret);
    if (!F.term) {
        if (in->ret == ty_unit) {
            emit(&F, "ret void");
        } else if (in->ret == ty_never) {
            emit(&F, "unreachable");
        } else {
            Pos end = fn->b->pos;
            v = coerce(&F, end, v, in->ret, "the function result (the body must end with a value or `return`)");
            if (is_err(v.t)) emit(&F, "unreachable");
            else emit(&F, "ret %s %s", type_ll(in->ret), v.v);
        }
        F.term = true;
    }
    const char *rl = (in->ret == ty_unit || in->ret == ty_never) ? "void" : type_ll(in->ret);
    sb_printf(&g->funcs, "define %s @\"%s\"(%s) {\nentry:\n%s%s%s}\n\n", rl, in->name, sb_str(&params),
              sb_str(&F.allocas), sb_str(&F.code), sb_str(&F.cold));
}

static Decl *find_main(Gen *g) {
    Module *m = prog_find_module(g->pg, "Main");
    Decl *d = m ? module_find(m, "Main") : NULL;
    if (!d || d->node->kind != D_FN) {
        Pos none = {0};
        diag_error(none, "no entry point: a program needs `module Main` with a public `fn Main()`");
        return NULL;
    }
    return d;
}

bool gen_program(Gen *g, SB *out) {
    Decl *md = find_main(g);
    Inst *main_in = NULL;
    if (md) {
        Node *fn = md->node;
        if (fn->list2.n) diag_error(fn->pos, "`Main` cannot have type parameters");
        else if (fn->list.n != 0) diag_error(fn->pos, "`Main` with command line arguments is not supported yet; use `fn Main()`");
        else main_in = get_inst(g, md, NULL, NULL);
        if (main_in && main_in->ret != ty_unit && !is_err(main_in->ret))
            diag_error(fn->pos, "`Main` must not return a value (use `Env.Exit(code)` to set the exit code)");
    }
    /* check every non-generic function of the user's modules, even if unused */
    for (int i = 0; i < g->pg->modules.n; i++) {
        Module *m = g->pg->modules.v[i];
        if (m->is_std) continue;
        for (size_t k = 0; k < m->decls.cap; k++) {
            if (!m->decls.keys[k]) continue;
            Decl *d = m->decls.vals[k];
            if (d->node->kind == D_FN && !d->abi && d->node->list2.n == 0) get_inst(g, d, NULL, NULL);
        }
    }
    for (int i = 0; i < g->worklist.n; i++) emit_function(g, g->worklist.v[i]);
    if (zv_error_count) return false;
    sb_puts(out, "; generated by zevic0\nsource_filename = \"zevic\"\n\n");
    sb_puts(out, sb_str(&g->globals));
    sb_puts(out, "\n");
    sb_puts(out, sb_str(&g->decls));
    sb_puts(out, "declare void @zevic_init(i32, ptr)\n\n");
    sb_puts(out, sb_str(&g->funcs));
    sb_printf(out, "define i32 @main(i32 %%argc, ptr %%argv) {\n  call void @zevic_init(i32 %%argc, ptr %%argv)\n"
                   "  call void @\"%s\"()\n  ret i32 0\n}\n",
              main_in->name);
    return true;
}
