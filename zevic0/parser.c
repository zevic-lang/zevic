#include "parser.h"

#include <setjmp.h>

typedef struct {
    int file;
    Token *t;
    int n, i;
    jmp_buf *jb;
    bool quiet; /* speculative parse: do not print errors */
    int depth;  /* recursion guard */
} Parser;

/* ---- helpers ---- */

static Token *peek(Parser *p) { return &p->t[p->i]; }
static Token *peek_at(Parser *p, int k) {
    int j = p->i + k;
    if (j >= p->n) j = p->n - 1;
    return &p->t[j];
}
static TokKind kind(Parser *p) { return p->t[p->i].k; }
static Token *next(Parser *p) {
    Token *t = &p->t[p->i];
    if (p->i < p->n - 1) p->i++;
    return t;
}

static void perr(Parser *p, Pos pos, const char *fmt, ...) __attribute__((noreturn, format(printf, 3, 4)));
static void perr(Parser *p, Pos pos, const char *fmt, ...) {
    if (!p->quiet) {
        va_list ap;
        va_start(ap, fmt);
        char *msg = zvsprintf(fmt, ap);
        va_end(ap);
        diag_error(pos, "%s", msg);
    }
    longjmp(*p->jb, 1);
}

static const char *describe(Token *t) {
    switch (t->k) {
    case TK_IDENT: return zsprintf("`%s`", t->text);
    case TK_INT: case TK_FLOAT: return "a number";
    case TK_STRING: return "a string";
    case TK_CHAR: case TK_BYTE: return "a character";
    case TK_NL: return "the end of the line";
    case TK_EOF: return "the end of the file";
    default: return zsprintf("`%s`", tok_name(t->k));
    }
}

static void skip_nl(Parser *p) {
    while (kind(p) == TK_NL) next(p);
}

static bool accept(Parser *p, TokKind k) {
    if (kind(p) == k) {
        next(p);
        return true;
    }
    return false;
}

static Token *expect(Parser *p, TokKind k, const char *what) {
    if (kind(p) != k) perr(p, peek(p)->pos, "expected %s, found %s", what, describe(peek(p)));
    return next(p);
}

static char *expect_ident(Parser *p, const char *what) {
    Token *t = expect(p, TK_IDENT, what);
    return t->text;
}

/* `>>` closing two generic lists: split the token and use its first half. */
static void expect_gt(Parser *p) {
    if (kind(p) == TK_GT) {
        next(p);
    } else if (kind(p) == TK_SHR) {
        Token *t = peek(p);
        t->k = TK_GT;
        t->pos.col++;
    } else if (kind(p) == TK_GE) {
        Token *t = peek(p);
        t->k = TK_ASSIGN;
        t->pos.col++;
    } else {
        perr(p, peek(p)->pos, "expected `>`, found %s", describe(peek(p)));
    }
}

static bool is_upper(const char *s) {
    return s[0] >= 'A' && s[0] <= 'Z';
}

/* ---- forward declarations ---- */

static Node *parse_expr(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_pattern(Parser *p);
static Node *parse_decl_fn(Parser *p, bool allow_nobody);


/* ---- types ---- */

static Node *parse_type(Parser *p);

static void parse_type_list(Parser *p, Vec *out, TokKind close) {
    skip_nl(p);
    while (kind(p) != close) {
        vec_push(out, parse_type(p));
        skip_nl(p);
        if (!accept(p, TK_COMMA)) break;
        skip_nl(p);
    }
}

static Node *parse_type(Parser *p) {
    Token *t = peek(p);
    Pos pos = t->pos;
    if (++p->depth > 200) perr(p, pos, "type is nested too deeply");
    Node *res;
    if (t->k == TK_LPAREN) {
        next(p);
        Node *n = node_new(N_TYPE_TUPLE, pos);
        parse_type_list(p, &n->list, TK_RPAREN);
        expect(p, TK_RPAREN, "`)`");
        if (n->list.n == 0) {
            n = node_new(N_TYPE_NAME, pos);
            vec_push(&n->path, "Unit");
        } else if (n->list.n == 1) {
            n = n->list.v[0];
        }
        res = n;
    } else if (t->k == TK_FN) {
        next(p);
        Node *n = node_new(N_TYPE_FN, pos);
        expect(p, TK_LPAREN, "`(`");
        parse_type_list(p, &n->list, TK_RPAREN);
        expect(p, TK_RPAREN, "`)`");
        if (accept(p, TK_ARROW)) {
            n->a = parse_type(p);
        } else {
            Node *u = node_new(N_TYPE_NAME, pos);
            vec_push(&u->path, "Unit");
            n->a = u;
        }
        res = n;
    } else if (t->k == TK_STAR) {
        next(p);
        Node *n = node_new(N_TYPE_PTR, pos);
        n->a = parse_type(p);
        res = n;
    } else if (t->k == TK_SELF_T) {
        next(p);
        Node *n = node_new(N_TYPE_NAME, pos);
        vec_push(&n->path, "Self");
        res = n;
    } else if (t->k == TK_IDENT) {
        Node *n = node_new(N_TYPE_NAME, pos);
        vec_push(&n->path, next(p)->text);
        while (kind(p) == TK_DOT && peek_at(p, 1)->k == TK_IDENT) {
            next(p);
            vec_push(&n->path, next(p)->text);
        }
        if (kind(p) == TK_LT) {
            next(p);
            parse_type_list(p, &n->list, TK_GT);
            expect_gt(p);
        }
        res = n;
    } else {
        perr(p, pos, "expected a type, found %s", describe(t));
    }
    p->depth--;
    return res;
}

/* <T, K: Hashable + Comparable> */
static void parse_tparams(Parser *p, Vec *out) {
    if (kind(p) != TK_LT) return;
    next(p);
    for (;;) {
        Token *t = expect(p, TK_IDENT, "a type parameter name");
        Node *tp = node_new(N_TPARAM, t->pos);
        tp->name = t->text;
        if (accept(p, TK_COLON)) {
            vec_push(&tp->list, parse_type(p));
            while (accept(p, TK_PLUS)) vec_push(&tp->list, parse_type(p));
        }
        vec_push(out, tp);
        if (!accept(p, TK_COMMA)) break;
    }
    expect_gt(p);
}

/* ---- module header ---- */

static void parse_dotted(Parser *p, Vec *path, const char *what) {
    vec_push(path, expect_ident(p, what));
    while (kind(p) == TK_DOT) {
        next(p);
        vec_push(path, expect_ident(p, what));
    }
}

static Node *stub_decl(Parser *p);

SourceFile *parse_file(int file, const char *text, size_t len) {
    TokenList tl = lex(file, text, len, 1, 1);
    Parser ps = {0};
    ps.file = file;
    ps.t = tl.v;
    ps.n = tl.n;
    Parser *p = &ps;
    SourceFile *sf = zalloc(sizeof *sf);
    sf->file = file;
    jmp_buf top;
    p->jb = &top;
    skip_nl(p);
    if (setjmp(top)) {
        return sf; /* an error in the header: nothing sensible can follow */
    }
    if (kind(p) != TK_MODULE) perr(p, peek(p)->pos, "a Zevic file must start with `module Name`");
    sf->pos = next(p)->pos;
    sf->module_pos = peek(p)->pos;
    Vec path = {0};
    parse_dotted(p, &path, "a module name");
    sf->module = path_join(&path, ".");
    skip_nl(p);
    while (kind(p) == TK_IMPORT) {
        Node *im = node_new(N_IMPORT, next(p)->pos);
        parse_dotted(p, &im->path, "a module name");
        if (accept(p, TK_AS)) im->name = expect_ident(p, "an alias name");
        vec_push(&sf->imports, im);
        skip_nl(p);
    }
    while (kind(p) != TK_EOF) {
        jmp_buf here;
        jmp_buf *prev = p->jb;
        p->jb = &here;
        if (setjmp(here) == 0) {
            Node *d = stub_decl(p);
            if (d) vec_push(&sf->decls, d);
        } else {
            /* recover: skip to the next top-level declaration keyword */
            int depth = 0;
            while (kind(p) != TK_EOF) {
                TokKind k = kind(p);
                if (k == TK_LBRACE) depth++;
                else if (k == TK_RBRACE) { if (depth > 0) depth--; }
                else if (depth == 0 && (k == TK_FN || k == TK_STRUCT || k == TK_CLASS || k == TK_ENUM ||
                                        k == TK_INTERFACE || k == TK_EXTERN || k == TK_EXPORT))
                    break;
                next(p);
            }
        }
        p->jb = prev;
        skip_nl(p);
    }
    return sf;
}

static Node *stub_decl(Parser *p) {
    perr(p, peek(p)->pos, "unexpected %s at the top level", describe(peek(p)));
}
