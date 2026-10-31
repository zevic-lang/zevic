/* ast.h - syntax tree */
#ifndef ZEVIC_AST_H
#define ZEVIC_AST_H

#include "lexer.h"
#include "util.h"

typedef enum {
    /* ---- types ---- */
    N_TYPE_NAME,  /* path (Vec of char*), list = type arguments */
    N_TYPE_TUPLE, /* list = element types */
    N_TYPE_FN,    /* list = parameter types, a = return type */
    N_TYPE_PTR,   /* a = pointee */

    /* ---- expressions ---- */
    N_INT,     /* ival, suffix */
    N_FLOAT,   /* fval, suffix */
    N_BOOL,    /* ival = 0 / 1 */
    N_CHAR,    /* ival = code point */
    N_BYTE,    /* ival */
    N_STRING,  /* list = parts: N_STR_LIT or an expression */
    N_STR_LIT, /* name/slen = decoded bytes */
    N_UNIT,    /* () */
    N_IDENT,   /* name */
    N_THIS,
    N_MEMBER,  /* a . name */
    N_CALL,    /* a = callee, list = arguments, list2 = explicit type arguments */
    N_INDEX,   /* a [ b ] */
    N_TRY,     /* a ? */
    N_UNARY,   /* op (TK_MINUS, TK_BANG, TK_TILDE), a */
    N_BINARY,  /* op, a, b */
    N_RANGE,   /* a .. b (flag = inclusive) */
    N_IF,      /* a = condition, b = then block, c = else (block / if) or NULL */
    N_MATCH,   /* a = scrutinee, list = N_ARM */
    N_ARM,     /* a = pattern, b = guard or NULL, c = body */
    N_BLOCK,   /* list = statements; a = tail expression or NULL */
    N_LAMBDA,  /* list = N_PARAM, b = body */
    N_ARRAY,   /* list = elements */
    N_MAPLIT,  /* list = N_PAIR */
    N_PAIR,    /* a, b */
    N_TUPLE,   /* list */
    N_STRUCTLIT, /* a = path (Shape.Named), list = N_FIELD (name, b = value): Shape.Named { Name: "x" } */

    /* ---- statements ---- */
    N_LET,      /* flag = mut; a = pattern, b = type, c = initializer */
    N_ASSIGN,   /* op (TK_ASSIGN or an op-assign token); a = target, b = value */
    N_EXPRSTMT, /* a */
    N_RETURN,   /* a or NULL */
    N_BREAK,
    N_CONTINUE,
    N_DEFER,    /* a = statement or block */
    N_WHILE,    /* a = condition, b = body */
    N_LOOP,     /* b = body */
    N_FOR,      /* a = pattern (value), c = index pattern or NULL, b = body, d = iterable */

    /* ---- patterns ---- */
    P_WILD,
    P_BIND,    /* name */
    P_LIT,     /* a = literal expression (possibly negated) */
    P_RANGE,   /* a, b (flag = inclusive) */
    P_TUPLE,   /* list */
    P_VARIANT, /* path = [Shape, Circle] or [Some]; list = sub-patterns; flag = has parens;
                  list2 = N_FIELDPAT for { Name: pat, .. }; flag2 = has braces; flag3 = has `..` */
    P_FIELD,   /* name, a = pattern */
    P_OR,      /* list */

    /* ---- declarations ---- */
    D_FN,
    D_STRUCT,
    D_CLASS,
    D_ENUM,
    D_INTERFACE,
    D_EXTERN,  /* name = ABI string; list = D_FN / D_STRUCT declarations */
    D_CONST,   /* name, a = type, b = value */
    D_TEST,    /* name = test name, b = body */
    N_FIELD,   /* name, a = type, b = default or NULL, flag = static */
    N_VARIANT, /* name, list = payload types (tuple form) or list2 = N_FIELD (named form); flag = named */
    N_PARAM,   /* name, a = type, flag = mut, flag2 = params, flag3 = variadic `...` */
    N_TPARAM,  /* name, list = bounds */
    N_IMPORT   /* path, name = alias or NULL */
} NodeKind;

typedef struct Node Node;

struct Node {
    NodeKind kind;
    Pos pos;
    char *name;     /* identifier, string bytes, alias */
    int slen;       /* length of `name` when it holds string bytes */
    char *name2;    /* extern alias (C name) */
    TokKind op;
    uint64_t ival;
    double fval;
    const char *suffix;
    bool flag, flag2, flag3;
    bool is_static, is_mut_fn, is_export;
    Node *a, *b, *c, *d;
    Vec list, list2, list3;
    Vec path; /* char* segments */
    void *aux; /* scratch for later passes (e.g. the Decl wrapper of a method) */
};

typedef struct {
    int file;
    Pos pos;
    char *module; /* dotted module name */
    Pos module_pos;
    Vec imports;  /* N_IMPORT */
    Vec decls;
    bool is_std;  /* loaded from the standard library */
    struct Module *mod;
    Dict import_map; /* name used in the source -> Module* */
} SourceFile;

Node *node_new(NodeKind k, Pos pos);
const char *node_kind_name(NodeKind k);
void ast_dump(SourceFile *f, FILE *out);
char *path_join(const Vec *path, const char *sep);

#endif
