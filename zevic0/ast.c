#include "ast.h"

Node *node_new(NodeKind k, Pos pos) {
    Node *n = zalloc(sizeof *n);
    n->kind = k;
    n->pos = pos;
    return n;
}

char *path_join(const Vec *path, const char *sep) {
    SB sb = {0};
    for (int i = 0; i < path->n; i++) {
        if (i) sb_puts(&sb, sep);
        sb_puts(&sb, (const char *)path->v[i]);
    }
    return sb_str(&sb);
}

/* ---- dump (for --dump-ast and parser tests) ---- */

static void dump(FILE *o, Node *n, int ind);

static void indent(FILE *o, int n) {
    for (int i = 0; i < n; i++) fputs("  ", o);
}

static void dump_list(FILE *o, const char *label, Vec *v, int ind) {
    if (v->n == 0) return;
    indent(o, ind);
    fprintf(o, "%s:\n", label);
    for (int i = 0; i < v->n; i++) dump(o, v->v[i], ind + 1);
}

static void dump_child(FILE *o, const char *label, Node *n, int ind) {
    if (!n) return;
    indent(o, ind);
    fprintf(o, "%s:\n", label);
    dump(o, n, ind + 1);
}

static void dump_str(FILE *o, const char *s, int n) {
    fputc('"', o);
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') fputs("\\n", o);
        else if (c == '"') fputs("\\\"", o);
        else if (c < 32) fprintf(o, "\\x%02x", c);
        else fputc(c, o);
    }
    fputc('"', o);
}

static void dump(FILE *o, Node *n, int ind) {
    indent(o, ind);
    if (!n) {
        fputs("(null)\n", o);
        return;
    }
    fprintf(o, "%s", node_kind_name(n->kind));
    if (n->path.n) fprintf(o, " %s", path_join(&n->path, "."));
    switch (n->kind) {
    case N_INT: fprintf(o, " %llu%s%s", (unsigned long long)n->ival, n->suffix ? "_" : "", n->suffix ? n->suffix : ""); break;
    case N_FLOAT: fprintf(o, " %g%s%s", n->fval, n->suffix ? "_" : "", n->suffix ? n->suffix : ""); break;
    case N_BOOL: case N_CHAR: case N_BYTE: fprintf(o, " %llu", (unsigned long long)n->ival); break;
    case N_STR_LIT: fputc(' ', o); dump_str(o, n->name, n->slen); break;
    case N_UNARY: case N_BINARY: case N_ASSIGN: fprintf(o, " %s", tok_name(n->op)); break;
    case N_RANGE: case P_RANGE: fprintf(o, "%s", n->flag ? " (inclusive)" : ""); break;
    case N_LET: fprintf(o, "%s", n->flag ? " mut" : " let"); break;
    default:
        if (n->name) fprintf(o, " %s", n->name);
        break;
    }
    if (n->kind == D_FN) {
        if (n->is_static) fputs(" static", o);
        if (n->is_mut_fn) fputs(" mut", o);
        if (n->is_export) fputs(" export", o);
    }
    if (n->kind == N_PARAM) {
        if (n->flag) fputs(" mut", o);
        if (n->flag2) fputs(" params", o);
        if (n->flag3) fputs(" ...", o);
    }
    fputc('\n', o);
    if (n->kind >= D_FN && n->kind <= D_INTERFACE) dump_list(o, "tparams", &n->list2, ind + 1);
    switch (n->kind) {
    case D_FN:
        dump_list(o, "params", &n->list, ind + 1);
        dump_child(o, "ret", n->a, ind + 1);
        dump_child(o, "body", n->b, ind + 1);
        break;
    case N_IF:
        dump_child(o, "cond", n->a, ind + 1);
        dump_child(o, "then", n->b, ind + 1);
        dump_child(o, "else", n->c, ind + 1);
        break;
    case N_LET:
        dump_child(o, "pattern", n->a, ind + 1);
        dump_child(o, "type", n->b, ind + 1);
        dump_child(o, "init", n->c, ind + 1);
        break;
    case N_FOR:
        dump_child(o, "index", n->c, ind + 1);
        dump_child(o, "pattern", n->a, ind + 1);
        dump_child(o, "iter", n->d, ind + 1);
        dump_child(o, "body", n->b, ind + 1);
        break;
    case N_BLOCK:
        dump_list(o, "stmts", &n->list, ind + 1);
        dump_child(o, "tail", n->a, ind + 1);
        break;
    case N_ARM:
        dump_child(o, "pattern", n->a, ind + 1);
        dump_child(o, "guard", n->b, ind + 1);
        dump_child(o, "body", n->c, ind + 1);
        break;
    default:
        dump_child(o, "a", n->a, ind + 1);
        dump_child(o, "b", n->b, ind + 1);
        dump_child(o, "c", n->c, ind + 1);
        dump_child(o, "d", n->d, ind + 1);
        dump_list(o, "list", &n->list, ind + 1);
        break;
    }
    if (n->kind != D_FN) dump_list(o, "implements", &n->list3, ind + 1);
    if (n->kind == P_VARIANT || n->kind == N_VARIANT) dump_list(o, "fields", &n->list2, ind + 1);
}

void ast_dump(SourceFile *f, FILE *out) {
    fprintf(out, "module %s\n", f->module ? f->module : "?");
    for (int i = 0; i < f->imports.n; i++) {
        Node *im = f->imports.v[i];
        fprintf(out, "import %s%s%s\n", path_join(&im->path, "."), im->name ? " as " : "", im->name ? im->name : "");
    }
    for (int i = 0; i < f->decls.n; i++) dump(out, f->decls.v[i], 0);
}

const char *node_kind_name(NodeKind k) {
    static const char *names[] = {
        [N_TYPE_NAME] = "TypeName", [N_TYPE_TUPLE] = "TypeTuple", [N_TYPE_FN] = "TypeFn",
        [N_TYPE_PTR] = "TypePtr", [N_INT] = "Int", [N_FLOAT] = "Float", [N_BOOL] = "Bool",
        [N_CHAR] = "Char", [N_BYTE] = "Byte", [N_STRING] = "String", [N_STR_LIT] = "Lit",
        [N_UNIT] = "Unit", [N_IDENT] = "Ident", [N_THIS] = "This", [N_MEMBER] = "Member",
        [N_CALL] = "Call", [N_INDEX] = "Index", [N_TRY] = "Try", [N_UNARY] = "Unary",
        [N_BINARY] = "Binary", [N_RANGE] = "Range", [N_IF] = "If", [N_MATCH] = "Match",
        [N_ARM] = "Arm", [N_BLOCK] = "Block", [N_LAMBDA] = "Lambda", [N_ARRAY] = "Array",
        [N_MAPLIT] = "MapLit", [N_PAIR] = "Pair", [N_TUPLE] = "Tuple", [N_STRUCTLIT] = "StructLit", [N_LET] = "Let",
        [N_ASSIGN] = "Assign", [N_EXPRSTMT] = "ExprStmt", [N_RETURN] = "Return",
        [N_BREAK] = "Break", [N_CONTINUE] = "Continue", [N_DEFER] = "Defer", [N_WHILE] = "While",
        [N_LOOP] = "Loop", [N_FOR] = "For", [P_WILD] = "PWild", [P_BIND] = "PBind",
        [P_LIT] = "PLit", [P_RANGE] = "PRange", [P_TUPLE] = "PTuple", [P_VARIANT] = "PVariant",
        [P_FIELD] = "PField", [P_OR] = "POr", [D_FN] = "Fn", [D_STRUCT] = "Struct",
        [D_CLASS] = "Class", [D_ENUM] = "Enum", [D_INTERFACE] = "Interface", [D_EXTERN] = "Extern",
        [D_CONST] = "Const", [D_TEST] = "Test", [N_FIELD] = "Field", [N_VARIANT] = "Variant",
        [N_PARAM] = "Param", [N_TPARAM] = "TParam", [N_IMPORT] = "Import",
    };
    return names[k] ? names[k] : "?";
}
