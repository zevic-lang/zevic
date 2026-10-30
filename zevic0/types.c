#include "types.h"

Type *ty_i8, *ty_i16, *ty_i32, *ty_i64, *ty_u8, *ty_u16, *ty_u32, *ty_u64;
Type *ty_f32, *ty_f64, *ty_bool, *ty_char, *ty_string, *ty_unit, *ty_never;

Type *ty_cycle;
static Dict g_types;
static int g_next_id;

static Type *intern(Type *t) {
    Type *old = dict_get(&g_types, t->name);
    if (old) return old;
    t->id = g_next_id++;
    dict_put(&g_types, t->name, t);
    return t;
}

static Type *prim(TKind k, const char *name) {
    Type *t = zalloc(sizeof *t);
    t->k = k;
    t->name = zstrdup(name);
    return intern(t);
}

void types_init(void) {
    if (ty_i8) return;
    ty_i8 = prim(T_I8, "I8");
    ty_i16 = prim(T_I16, "I16");
    ty_i32 = prim(T_I32, "I32");
    ty_i64 = prim(T_I64, "I64");
    ty_u8 = prim(T_U8, "U8");
    ty_u16 = prim(T_U16, "U16");
    ty_u32 = prim(T_U32, "U32");
    ty_u64 = prim(T_U64, "U64");
    ty_f32 = prim(T_F32, "F32");
    ty_f64 = prim(T_F64, "F64");
    ty_bool = prim(T_BOOL, "Bool");
    ty_char = prim(T_CHAR, "Char");
    ty_string = prim(T_STRING, "String");
    ty_unit = prim(T_UNIT, "Unit");
    ty_never = prim(T_NEVER, "Never");
}

Type *ty_prim_by_name(const char *n) {
    static const char *names[] = {"I8", "I16", "I32", "I64", "U8", "U16", "U32", "U64", "F32", "F64",
                                  "Bool", "Char", "String", "Unit", NULL};
    for (int i = 0; names[i]; i++)
        if (strcmp(names[i], n) == 0) return dict_get(&g_types, n);
    return NULL;
}

Type *ty_tuple(Vec elems) {
    SB sb = {0};
    sb_putc(&sb, '(');
    for (int i = 0; i < elems.n; i++) {
        if (i) sb_puts(&sb, ", ");
        sb_puts(&sb, ((Type *)elems.v[i])->name);
    }
    sb_putc(&sb, ')');
    Type *t = zalloc(sizeof *t);
    t->k = T_TUPLE;
    t->name = sb_str(&sb);
    t->args = elems;
    return intern(t);
}

Type *ty_fn(Vec params, Type *ret) {
    SB sb = {0};
    sb_puts(&sb, "fn(");
    for (int i = 0; i < params.n; i++) {
        if (i) sb_puts(&sb, ", ");
        sb_puts(&sb, ((Type *)params.v[i])->name);
    }
    sb_printf(&sb, ") -> %s", ret->name);
    Type *t = zalloc(sizeof *t);
    t->k = T_FN;
    t->name = sb_str(&sb);
    t->args = params;
    t->ret = ret;
    return intern(t);
}

Type *ty_ptr(Type *pointee) {
    Type *t = zalloc(sizeof *t);
    t->k = T_PTR;
    t->name = zsprintf("*%s", pointee->name);
    vec_push(&t->args, pointee);
    return intern(t);
}

Type *ty_param(const char *name) {
    Type *t = zalloc(sizeof *t);
    t->k = T_PARAM;
    t->name = zsprintf("'%s", name);
    return intern(t);
}

Type *ty_ctype(const char *name, int64_t size, bool is_signed) {
    Type *t = zalloc(sizeof *t);
    t->k = T_CTYPE;
    t->name = zstrdup(name);
    t->size = size;
    t->align = size;
    t->laid_out = true;
    t->payload_words = is_signed ? 1 : 0; /* reused as the signedness flag */
    t->ll = zsprintf("i%lld", (long long)(size * 8));
    return intern(t);
}

Type *ty_named(TKind k, struct Decl *decl, const char *qualified, Vec args) {
    SB sb = {0};
    sb_puts(&sb, qualified);
    if (args.n) {
        sb_putc(&sb, '<');
        for (int i = 0; i < args.n; i++) {
            if (i) sb_puts(&sb, ", ");
            sb_puts(&sb, ((Type *)args.v[i])->name);
        }
        sb_putc(&sb, '>');
    }
    Type *t = zalloc(sizeof *t);
    t->k = k;
    t->name = sb_str(&sb);
    t->args = args;
    t->decl = decl;
    return intern(t);
}

bool ty_is_int(Type *t) { return t->k >= T_I8 && t->k <= T_U64; }
bool ty_is_signed(Type *t) { return t->k >= T_I8 && t->k <= T_I64; }
bool ty_is_float(Type *t) { return t->k == T_F32 || t->k == T_F64; }
bool ty_is_numeric(Type *t) { return ty_is_int(t) || ty_is_float(t); }

int ty_bits(Type *t) {
    switch (t->k) {
    case T_I8: case T_U8: return 8;
    case T_I16: case T_U16: return 16;
    case T_I32: case T_U32: return 32;
    case T_I64: case T_U64: return 64;
    default: return 0;
    }
}

bool ty_is_ref(Type *t) { return t->k == T_CLASS; }
bool ty_is_value_agg(Type *t) {
    return t->k == T_STRUCT || t->k == T_ENUM || t->k == T_TUPLE || t->k == T_STRING;
}

/* ---- layout ---- */

static int64_t round_up(int64_t n, int64_t a) {
    return (n + a - 1) / a * a;
}

static void struct_layout(Vec *types, int64_t *size, int64_t *align) {
    int64_t off = 0, al = 1;
    for (int i = 0; i < types->n; i++) {
        Type *ft = types->v[i];
        if (ft->laid_out && !ft->ll) {
            ty_cycle = ft;
            continue;
        }
        ty_layout(ft);
        off = round_up(off, ft->align);
        off += ft->size;
        if (ft->align > al) al = ft->align;
    }
    *size = round_up(off, al);
    *align = al;
}

void ty_layout(Type *t) {
    if (t->laid_out) return;
    t->laid_out = true;
    switch (t->k) {
    case T_I8: case T_U8: t->size = t->align = 1; t->ll = "i8"; break;
    case T_I16: case T_U16: t->size = t->align = 2; t->ll = "i16"; break;
    case T_I32: case T_U32: t->size = t->align = 4; t->ll = "i32"; break;
    case T_I64: case T_U64: t->size = t->align = 8; t->ll = "i64"; break;
    case T_F32: t->size = t->align = 4; t->ll = "float"; break;
    case T_F64: t->size = t->align = 8; t->ll = "double"; break;
    case T_BOOL: t->size = t->align = 1; t->ll = "i1"; break;
    case T_CHAR: t->size = t->align = 4; t->ll = "i32"; break;
    case T_STRING: t->size = 16; t->align = 8; t->ll = "{ ptr, i64 }"; break;
    case T_UNIT: case T_NEVER: t->size = 0; t->align = 1; t->ll = "{}"; break;
    case T_CLASS: case T_PTR: case T_BUF: t->size = t->align = 8; t->ll = "ptr"; break;
    case T_FN: t->size = 16; t->align = 8; t->ll = "{ ptr, ptr }"; break;
    case T_TUPLE: {
        SB sb = {0};
        sb_puts(&sb, "{ ");
        for (int i = 0; i < t->args.n; i++) {
            if (i) sb_puts(&sb, ", ");
            Type *e = t->args.v[i];
            if (e->laid_out && !e->ll) {
                ty_cycle = e;
                sb_puts(&sb, "{}");
                continue;
            }
            ty_layout(e);
            sb_puts(&sb, e->ll);
        }
        sb_puts(&sb, t->args.n ? " }" : "}");
        t->ll = sb_str(&sb);
        struct_layout(&t->args, &t->size, &t->align);
        break;
    }
    case T_STRUCT: {
        Vec ts = {0};
        SB sb = {0};
        sb_puts(&sb, "{ ");
        for (int i = 0; i < t->fields.n; i++) {
            Field *f = t->fields.v[i];
            if (f->t->laid_out && !f->t->ll) {
                ty_cycle = f->t;
                continue;
            }
            ty_layout(f->t);
            vec_push(&ts, f->t);
            if (ts.n > 1) sb_puts(&sb, ", ");
            sb_puts(&sb, f->t->ll);
        }
        sb_puts(&sb, t->fields.n ? " }" : "}");
        t->ll = sb_str(&sb);
        struct_layout(&ts, &t->size, &t->align);
        break;
    }
    case T_ENUM: {
        int64_t max = 0;
        for (int i = 0; i < t->variants.n; i++) {
            Variant *v = t->variants.v[i];
            if (v->payload) {
                if (v->payload->laid_out && !v->payload->ll) {
                    ty_cycle = t;
                    continue;
                }
                ty_layout(v->payload);
                if (v->payload->size > max) max = v->payload->size;
            }
        }
        t->payload_words = (int)((max + 7) / 8);
        if (t->payload_words == 0) {
            t->ll = "{ i32 }";
            t->size = 4;
            t->align = 4;
        } else {
            t->ll = zsprintf("{ i32, [%d x i64] }", t->payload_words);
            t->size = 8 + 8 * t->payload_words;
            t->align = 8;
        }
        break;
    }
    default:
        t->size = 8;
        t->align = 8;
        t->ll = "ptr";
        break;
    }
}

const char *ty_ll(Type *t) {
    ty_layout(t);
    return t->ll;
}
int64_t ty_size(Type *t) {
    ty_layout(t);
    return t->size;
}
int64_t ty_align(Type *t) {
    ty_layout(t);
    return t->align;
}
