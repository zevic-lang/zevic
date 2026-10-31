/* types.h - semantic types (interned: equal types are the same pointer) */
#ifndef ZEVIC_TYPES_H
#define ZEVIC_TYPES_H

#include "ast.h"

typedef enum {
    T_I8, T_I16, T_I32, T_I64, T_U8, T_U16, T_U32, T_U64,
    T_F32, T_F64, T_BOOL, T_CHAR, T_STRING, T_UNIT, T_NEVER,
    T_TUPLE,  /* args = element types */
    T_FN,     /* args = parameter types, ret = result */
    T_STRUCT, T_CLASS, T_ENUM, /* decl + args = type arguments */
    T_PTR,    /* args[0] = pointee (C interop) */
    T_BUF,    /* Zevic.Intrinsics.Buf<T>: a fixed-size block of T (args[0]); only the standard library uses it */
    T_CTYPE,  /* C types: name says which */
    T_PARAM   /* an unbound type parameter (only inside generic declarations) */
} TKind;

struct Decl;
struct Type;

typedef struct {
    char *name;
    struct Type *t;
} Field;

typedef struct {
    char *name;
    int tag;
    bool named;
    Vec fields;          /* Field*; positional fields are named "0", "1", ... */
    struct Type *payload; /* tuple type of the field types, NULL when there is no data */
} Variant;

typedef struct Type {
    TKind k;
    char *name;  /* display name, also the intern key */
    Vec args;    /* Type* */
    struct Type *ret;
    struct Decl *decl;
    int id;
    /* members of struct / class / enum, filled in by resolve_members */
    bool members_done;
    Vec fields;   /* Field* (struct, class) */
    Vec variants; /* Variant* (enum) */
    /* layout (lazily computed) */
    bool laid_out;
    int64_t size, align;
    char *ll;
    int payload_words; /* enum: size of the payload in 8-byte words */
} Type;

extern Type *ty_i8, *ty_i16, *ty_i32, *ty_i64, *ty_u8, *ty_u16, *ty_u32, *ty_u64;
extern Type *ty_f32, *ty_f64, *ty_bool, *ty_char, *ty_string, *ty_unit, *ty_never;

void types_init(void);
/* Set when a layout needs a type that contains itself by value (infinite size). */
extern Type *ty_cycle;

Type *ty_tuple(Vec elems);
Type *ty_fn(Vec params, Type *ret);
Type *ty_ptr(Type *pointee);
Type *ty_param(const char *name);
Type *ty_ctype(const char *name, int64_t size, bool is_signed);
Type *ty_c_by_name(const char *name); /* CInt, CString, CVoid, ... or NULL */
bool ty_is_ctype(Type *t);
Type *ty_named(TKind k, struct Decl *decl, const char *qualified, Vec args);

bool ty_is_int(Type *t);
bool ty_is_signed(Type *t);
bool ty_is_float(Type *t);
bool ty_is_numeric(Type *t);
int ty_bits(Type *t);          /* integer width */
bool ty_is_ref(Type *t);       /* class, array: handled through a pointer */
bool ty_is_value_agg(Type *t); /* struct, enum, tuple, string */
Type *ty_prim_by_name(const char *name);

const char *ty_ll(Type *t);    /* the LLVM type */
void ty_layout(Type *t);
int64_t ty_size(Type *t);
int64_t ty_align(Type *t);

#endif
