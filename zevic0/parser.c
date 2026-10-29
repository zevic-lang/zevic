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

/* ---- expressions ---- */

static Node *parse_unary(Parser *p);
static Node *parse_range_expr(Parser *p);

static Node *parse_string_node(Parser *p, Token *t) {
    Node *n = node_new(N_STRING, t->pos);
    for (int k = 0; k < t->nparts; k++) {
        StrPart *sp = &t->parts[k];
        if (!sp->is_expr) {
            Node *lit = node_new(N_STR_LIT, sp->pos);
            lit->name = sp->s;
            lit->slen = sp->n;
            vec_push(&n->list, lit);
            continue;
        }
        /* parse the ${...} source with a sub-parser */
        TokenList tl = lex(p->file, sp->s, (size_t)sp->n, sp->pos.line, sp->pos.col);
        Parser sub = {0};
        sub.file = p->file;
        sub.t = tl.v;
        sub.n = tl.n;
        sub.quiet = p->quiet;
        jmp_buf jb;
        sub.jb = &jb;
        Node *e = NULL;
        if (setjmp(jb) == 0) {
            skip_nl(&sub);
            if (kind(&sub) == TK_EOF) perr(&sub, sp->pos, "empty `${}` in a string");
            e = parse_expr(&sub);
            skip_nl(&sub);
            if (kind(&sub) != TK_EOF)
                perr(&sub, peek(&sub)->pos, "unexpected %s in `${...}`", describe(peek(&sub)));
        }
        if (!e) {
            if (p->quiet) longjmp(*p->jb, 1);
            e = node_new(N_UNIT, sp->pos);
        }
        vec_push(&n->list, e);
    }
    return n;
}

static bool is_lambda_ahead(Parser *p) {
    int depth = 0;
    for (int j = p->i; j < p->n; j++) {
        TokKind k = p->t[j].k;
        if (k == TK_LPAREN || k == TK_LBRACK || k == TK_LBRACE) depth++;
        else if (k == TK_RPAREN || k == TK_RBRACK || k == TK_RBRACE) {
            depth--;
            if (depth == 0) return j + 1 < p->n && p->t[j + 1].k == TK_FATARROW;
            if (depth < 0) return false;
        } else if (k == TK_EOF) return false;
    }
    return false;
}

static Node *parse_lambda(Parser *p) {
    Node *n = node_new(N_LAMBDA, peek(p)->pos);
    expect(p, TK_LPAREN, "`(`");
    while (kind(p) != TK_RPAREN) {
        Token *t = expect(p, TK_IDENT, "a parameter name");
        Node *pm = node_new(N_PARAM, t->pos);
        pm->name = t->text;
        expect(p, TK_COLON, "`:` and the parameter's type (lambda parameter types are required)");
        pm->a = parse_type(p);
        vec_push(&n->list, pm);
        if (!accept(p, TK_COMMA)) break;
    }
    expect(p, TK_RPAREN, "`)`");
    expect(p, TK_FATARROW, "`=>`");
    if (kind(p) == TK_LBRACE) n->b = parse_block(p);
    else n->b = parse_expr(p);
    return n;
}

static Node *parse_if(Parser *p) {
    Node *n = node_new(N_IF, next(p)->pos);
    expect(p, TK_LPAREN, "`(` after `if` (conditions are written in parentheses)");
    n->a = parse_expr(p);
    expect(p, TK_RPAREN, "`)`");
    if (kind(p) != TK_LBRACE) perr(p, peek(p)->pos, "expected `{` (the body of an `if` must be in braces)");
    n->b = parse_block(p);
    /* `else` may follow on the next line */
    int save = p->i;
    skip_nl(p);
    if (kind(p) == TK_ELSE) {
        next(p);
        if (kind(p) == TK_IF) n->c = parse_if(p);
        else if (kind(p) == TK_LBRACE) n->c = parse_block(p);
        else perr(p, peek(p)->pos, "expected `if` or `{` after `else`");
    } else {
        p->i = save;
    }
    return n;
}

static Node *parse_match(Parser *p) {
    Node *n = node_new(N_MATCH, next(p)->pos);
    expect(p, TK_LPAREN, "`(` after `match` (the scrutinee is written in parentheses)");
    n->a = parse_expr(p);
    expect(p, TK_RPAREN, "`)`");
    expect(p, TK_LBRACE, "`{`");
    skip_nl(p);
    while (kind(p) != TK_RBRACE) {
        if (kind(p) == TK_EOF) perr(p, peek(p)->pos, "unterminated `match`: expected `}`");
        Node *arm = node_new(N_ARM, peek(p)->pos);
        arm->a = parse_pattern(p);
        if (accept(p, TK_IF)) {
            expect(p, TK_LPAREN, "`(` (a guard condition is written in parentheses)");
            arm->b = parse_expr(p);
            expect(p, TK_RPAREN, "`)`");
        }
        expect(p, TK_FATARROW, "`=>`");
        if (kind(p) == TK_LBRACE) arm->c = parse_block(p);
        else arm->c = parse_expr(p);
        vec_push(&n->list, arm);
        bool sep = false;
        while (kind(p) == TK_NL || kind(p) == TK_COMMA) {
            sep = true;
            next(p);
        }
        if (!sep && kind(p) != TK_RBRACE)
            perr(p, peek(p)->pos, "expected `,` or a new line after a match arm, found %s", describe(peek(p)));
    }
    next(p);
    return n;
}

static bool starts_expr(TokKind k) {
    switch (k) {
    case TK_NL: case TK_EOF: case TK_COMMA: case TK_RBRACE: case TK_RPAREN: case TK_RBRACK:
    case TK_FATARROW: case TK_ELSE: case TK_COLON:
        return false;
    default:
        return true;
    }
}

static Node *parse_primary(Parser *p) {
    Token *t = peek(p);
    Pos pos = t->pos;
    switch (t->k) {
    case TK_INT: {
        next(p);
        Node *n = node_new(N_INT, pos);
        n->ival = t->ival;
        n->suffix = t->suffix;
        return n;
    }
    case TK_FLOAT: {
        next(p);
        Node *n = node_new(N_FLOAT, pos);
        n->fval = t->fval;
        n->suffix = t->suffix;
        return n;
    }
    case TK_CHAR: case TK_BYTE: {
        next(p);
        Node *n = node_new(t->k == TK_CHAR ? N_CHAR : N_BYTE, pos);
        n->ival = t->ival;
        return n;
    }
    case TK_STRING:
        next(p);
        return parse_string_node(p, t);
    case TK_TRUE: case TK_FALSE: {
        next(p);
        Node *n = node_new(N_BOOL, pos);
        n->ival = t->k == TK_TRUE;
        return n;
    }
    case TK_THIS:
        next(p);
        return node_new(N_THIS, pos);
    case TK_IDENT: {
        next(p);
        Node *n = node_new(N_IDENT, pos);
        n->name = t->text;
        return n;
    }
    case TK_SELF_T: {
        next(p);
        Node *n = node_new(N_IDENT, pos);
        n->name = "Self";
        return n;
    }
    case TK_LPAREN: {
        if (is_lambda_ahead(p)) return parse_lambda(p);
        next(p);
        if (accept(p, TK_RPAREN)) return node_new(N_UNIT, pos);
        Node *e = parse_expr(p);
        if (kind(p) == TK_COMMA) {
            Node *tup = node_new(N_TUPLE, pos);
            vec_push(&tup->list, e);
            while (accept(p, TK_COMMA)) {
                if (kind(p) == TK_RPAREN) break;
                vec_push(&tup->list, parse_expr(p));
            }
            expect(p, TK_RPAREN, "`)`");
            return tup;
        }
        expect(p, TK_RPAREN, "`)`");
        e->flag3 = e->flag3 || e->kind == N_INT || e->kind == N_FLOAT; /* parenthesised literal */
        return e;
    }
    case TK_LBRACK: {
        next(p);
        Node *n = node_new(N_ARRAY, pos);
        while (kind(p) != TK_RBRACK) {
            vec_push(&n->list, parse_expr(p));
            if (!accept(p, TK_COMMA)) break;
        }
        expect(p, TK_RBRACK, "`]`");
        return n;
    }
    case TK_LBRACE: {
        next(p);
        Node *n = node_new(N_MAPLIT, pos);
        while (kind(p) != TK_RBRACE) {
            Node *pr = node_new(N_PAIR, peek(p)->pos);
            pr->a = parse_expr(p);
            expect(p, TK_COLON, "`:` in a map literal");
            pr->b = parse_expr(p);
            vec_push(&n->list, pr);
            if (!accept(p, TK_COMMA)) break;
        }
        expect(p, TK_RBRACE, "`}`");
        return n;
    }
    case TK_IF:
        return parse_if(p);
    case TK_MATCH:
        return parse_match(p);
    case TK_RETURN: {
        next(p);
        Node *n = node_new(N_RETURN, pos);
        if (starts_expr(kind(p))) n->a = parse_expr(p);
        return n;
    }
    case TK_BREAK:
        next(p);
        return node_new(N_BREAK, pos);
    case TK_CONTINUE:
        next(p);
        return node_new(N_CONTINUE, pos);
    default:
        perr(p, pos, "expected an expression, found %s", describe(t));
    }
}

/* Speculatively parse `<T, U>` after a name; succeeds only if `(` or `.` follows. */
static bool try_type_args(Parser *p, Vec *out) {
    int save = p->i;
    bool was_quiet = p->quiet;
    jmp_buf jb;
    jmp_buf *prev = p->jb;
    p->jb = &jb;
    p->quiet = true;
    bool ok = false;
    Vec args = {0};
    if (setjmp(jb) == 0) {
        expect(p, TK_LT, "`<`");
        parse_type_list(p, &args, TK_GT);
        expect_gt(p);
        ok = (kind(p) == TK_LPAREN || kind(p) == TK_DOT) && args.n > 0;
    }
    p->quiet = was_quiet;
    p->jb = prev;
    if (!ok) {
        p->i = save;
        return false;
    }
    *out = args;
    return true;
}

static Node *parse_postfix(Parser *p) {
    Node *e = parse_primary(p);
    for (;;) {
        Pos pos = peek(p)->pos;
        switch (kind(p)) {
        case TK_DOT: {
            next(p);
            Node *m = node_new(N_MEMBER, pos);
            m->a = e;
            if (kind(p) == TK_IDENT) {
                m->name = next(p)->text;
            } else if (kind(p) == TK_INT && !peek(p)->suffix) {
                m->name = zsprintf("%llu", (unsigned long long)next(p)->ival);
            } else {
                perr(p, peek(p)->pos, "expected a member name after `.`, found %s", describe(peek(p)));
            }
            e = m;
            break;
        }
        case TK_LPAREN: {
            next(p);
            Node *c = node_new(N_CALL, pos);
            c->a = e;
            while (kind(p) != TK_RPAREN) {
                vec_push(&c->list, parse_expr(p));
                if (!accept(p, TK_COMMA)) break;
            }
            expect(p, TK_RPAREN, "`)` after the arguments");
            e = c;
            break;
        }
        case TK_LBRACK: {
            next(p);
            Node *ix = node_new(N_INDEX, pos);
            ix->a = e;
            ix->b = parse_expr(p);
            expect(p, TK_RBRACK, "`]`");
            e = ix;
            break;
        }
        case TK_QUESTION: {
            next(p);
            Node *t = node_new(N_TRY, pos);
            t->a = e;
            e = t;
            break;
        }
        case TK_LT:
            if ((e->kind == N_IDENT || e->kind == N_MEMBER) && try_type_args(p, &e->list2)) break;
            return e;
        default:
            return e;
        }
    }
}

static Node *parse_unary(Parser *p) {
    Token *t = peek(p);
    if (t->k == TK_MINUS || t->k == TK_BANG || t->k == TK_TILDE) {
        if (++p->depth > 200) perr(p, t->pos, "expression is nested too deeply");
        next(p);
        Node *n = node_new(N_UNARY, t->pos);
        n->op = t->k;
        n->a = parse_unary(p);
        p->depth--;
        return n;
    }
    return parse_postfix(p);
}

static int binprec(TokKind k) {
    switch (k) {
    case TK_OROR: return 1;
    case TK_ANDAND: return 2;
    case TK_EQEQ: case TK_NEQ: case TK_LT: case TK_GT: case TK_LE: case TK_GE: return 3;
    case TK_PIPE: return 4;
    case TK_CARET: return 5;
    case TK_AMP: return 6;
    case TK_SHL: case TK_SHR: return 7;
    case TK_PLUS: case TK_MINUS: return 8;
    case TK_STAR: case TK_SLASH: case TK_PERCENT: return 9;
    default: return 0;
    }
}

static Node *parse_binary(Parser *p, int minprec) {
    Node *left = parse_unary(p);
    for (;;) {
        TokKind k = kind(p);
        int prec = binprec(k);
        if (prec == 0 || prec < minprec) return left;
        Pos pos = next(p)->pos;
        Node *right = parse_binary(p, prec + 1);
        Node *n = node_new(N_BINARY, pos);
        n->op = k;
        n->a = left;
        n->b = right;
        left = n;
        if (prec == 3 && binprec(kind(p)) == 3)
            perr(p, peek(p)->pos, "comparison operators cannot be chained (write `a < b && b < c`)");
    }
}

static Node *parse_range_expr(Parser *p) {
    Node *lhs = parse_binary(p, 1);
    if (kind(p) == TK_DOTDOT || kind(p) == TK_DOTDOTEQ) {
        Token *op = next(p);
        Node *r = node_new(N_RANGE, op->pos);
        r->flag = op->k == TK_DOTDOTEQ;
        r->a = lhs;
        r->b = parse_binary(p, 1);
        if (kind(p) == TK_DOTDOT || kind(p) == TK_DOTDOTEQ)
            perr(p, peek(p)->pos, "ranges cannot be chained");
        return r;
    }
    return lhs;
}

static Node *parse_expr(Parser *p) {
    if (++p->depth > 200) perr(p, peek(p)->pos, "expression is nested too deeply");
    Node *n = parse_range_expr(p);
    p->depth--;
    return n;
}

/* ---- patterns ---- */

static Node *parse_pattern_atom(Parser *p);

static Node *parse_literal_pattern(Parser *p) {
    Token *t = peek(p);
    Node *lit;
    if (t->k == TK_MINUS) {
        next(p);
        Node *inner = parse_primary(p);
        if (inner->kind != N_INT && inner->kind != N_FLOAT)
            perr(p, t->pos, "`-` in a pattern must be followed by a number");
        lit = node_new(N_UNARY, t->pos);
        lit->op = TK_MINUS;
        lit->a = inner;
    } else {
        lit = parse_primary(p);
    }
    return lit;
}

static Node *parse_pattern_atom(Parser *p) {
    Token *t = peek(p);
    Pos pos = t->pos;
    switch (t->k) {
    case TK_INT: case TK_FLOAT: case TK_CHAR: case TK_BYTE: case TK_STRING:
    case TK_TRUE: case TK_FALSE: case TK_MINUS: {
        Node *lit = parse_literal_pattern(p);
        if (kind(p) == TK_DOTDOT || kind(p) == TK_DOTDOTEQ) {
            Token *op = next(p);
            Node *r = node_new(P_RANGE, pos);
            r->flag = op->k == TK_DOTDOTEQ;
            r->a = lit;
            r->b = parse_literal_pattern(p);
            return r;
        }
        Node *n = node_new(P_LIT, pos);
        n->a = lit;
        return n;
    }
    case TK_LPAREN: {
        next(p);
        Node *n = node_new(P_TUPLE, pos);
        while (kind(p) != TK_RPAREN) {
            vec_push(&n->list, parse_pattern(p));
            if (!accept(p, TK_COMMA)) break;
        }
        expect(p, TK_RPAREN, "`)`");
        if (n->list.n == 1) return n->list.v[0];
        return n;
    }
    case TK_IDENT: {
        next(p);
        if (strcmp(t->text, "_") == 0 && !t->quoted) return node_new(P_WILD, pos);
        bool qualified = kind(p) == TK_DOT && peek_at(p, 1)->k == TK_IDENT;
        bool args = kind(p) == TK_LPAREN;
        bool fields = kind(p) == TK_LBRACE && is_upper(t->text);
        if (!qualified && !args && !fields && !is_upper(t->text)) {
            Node *n = node_new(P_BIND, pos);
            n->name = t->text;
            return n;
        }
        Node *n = node_new(P_VARIANT, pos);
        vec_push(&n->path, t->text);
        while (kind(p) == TK_DOT && peek_at(p, 1)->k == TK_IDENT) {
            next(p);
            vec_push(&n->path, next(p)->text);
        }
        if (kind(p) == TK_LPAREN) {
            next(p);
            n->flag = true;
            while (kind(p) != TK_RPAREN) {
                vec_push(&n->list, parse_pattern(p));
                if (!accept(p, TK_COMMA)) break;
            }
            expect(p, TK_RPAREN, "`)`");
        } else if (kind(p) == TK_LBRACE) {
            next(p);
            n->flag2 = true;
            skip_nl(p);
            while (kind(p) != TK_RBRACE) {
                if (accept(p, TK_DOTDOT)) {
                    n->flag3 = true;
                } else {
                    Token *ft = expect(p, TK_IDENT, "a field name");
                    Node *f = node_new(P_FIELD, ft->pos);
                    f->name = ft->text;
                    expect(p, TK_COLON, "`:` after the field name in a pattern");
                    f->a = parse_pattern(p);
                    vec_push(&n->list2, f);
                }
                skip_nl(p);
                if (!accept(p, TK_COMMA)) break;
                skip_nl(p);
            }
            skip_nl(p);
            expect(p, TK_RBRACE, "`}`");
        }
        return n;
    }
    default:
        perr(p, pos, "expected a pattern, found %s", describe(t));
    }
}

static Node *parse_pattern(Parser *p) {
    Node *first = parse_pattern_atom(p);
    if (kind(p) != TK_PIPE) return first;
    Node *or = node_new(P_OR, first->pos);
    vec_push(&or->list, first);
    while (accept(p, TK_PIPE)) vec_push(&or->list, parse_pattern_atom(p));
    return or;
}

/* ---- statements and blocks ---- */

static bool is_assign_op(TokKind k) {
    switch (k) {
    case TK_ASSIGN: case TK_PLUSEQ: case TK_MINUSEQ: case TK_STAREQ: case TK_SLASHEQ:
    case TK_PERCENTEQ: case TK_AMPEQ: case TK_PIPEEQ: case TK_CARETEQ: case TK_SHLEQ: case TK_SHREQ:
        return true;
    default:
        return false;
    }
}

static Node *parse_stmt(Parser *p);

static Node *parse_simple_stmt(Parser *p) {
    Pos pos = peek(p)->pos;
    Node *e = parse_expr(p);
    if (is_assign_op(kind(p))) {
        Token *op = next(p);
        Node *a = node_new(N_ASSIGN, op->pos);
        a->op = op->k;
        a->a = e;
        a->b = parse_expr(p);
        return a;
    }
    Node *s = node_new(N_EXPRSTMT, pos);
    s->a = e;
    return s;
}

static Node *parse_stmt(Parser *p) {
    Token *t = peek(p);
    Pos pos = t->pos;
    switch (t->k) {
    case TK_LET: case TK_MUT: {
        next(p);
        Node *n = node_new(N_LET, pos);
        n->flag = t->k == TK_MUT;
        n->a = parse_pattern(p);
        if (!accept(p, TK_COLON))
            perr(p, peek(p)->pos, "a variable needs a type: write `%s name: Type = value`", t->k == TK_MUT ? "mut" : "let");
        n->b = parse_type(p);
        expect(p, TK_ASSIGN, "`=` and an initial value (a variable must be initialised)");
        n->c = parse_expr(p);
        return n;
    }
    case TK_DEFER: {
        next(p);
        Node *n = node_new(N_DEFER, pos);
        if (kind(p) == TK_LBRACE) n->a = parse_block(p);
        else n->a = parse_simple_stmt(p);
        return n;
    }
    case TK_WHILE: {
        next(p);
        Node *n = node_new(N_WHILE, pos);
        expect(p, TK_LPAREN, "`(` after `while` (conditions are written in parentheses)");
        n->a = parse_expr(p);
        expect(p, TK_RPAREN, "`)`");
        if (kind(p) != TK_LBRACE) perr(p, peek(p)->pos, "expected `{` (the body of a loop must be in braces)");
        n->b = parse_block(p);
        return n;
    }
    case TK_LOOP: {
        next(p);
        Node *n = node_new(N_LOOP, pos);
        if (kind(p) != TK_LBRACE) perr(p, peek(p)->pos, "expected `{` after `loop`");
        n->b = parse_block(p);
        return n;
    }
    case TK_FOR: {
        next(p);
        Node *n = node_new(N_FOR, pos);
        expect(p, TK_LPAREN, "`(` after `for`");
        n->a = parse_pattern(p);
        if (accept(p, TK_COMMA)) {
            n->c = n->a; /* index pattern */
            n->a = parse_pattern(p);
        }
        expect(p, TK_IN, "`in`");
        n->d = parse_expr(p);
        expect(p, TK_RPAREN, "`)`");
        if (kind(p) != TK_LBRACE) perr(p, peek(p)->pos, "expected `{` (the body of a loop must be in braces)");
        n->b = parse_block(p);
        return n;
    }
    case TK_FN: case TK_STRUCT: case TK_CLASS: case TK_ENUM: case TK_INTERFACE:
        perr(p, pos, "declarations are only allowed at the top level (use a lambda for a local function)");
    default:
        return parse_simple_stmt(p);
    }
}

/* Skip to the end of the statement after an error. */
static void sync_stmt(Parser *p) {
    int depth = 0;
    while (kind(p) != TK_EOF) {
        TokKind k = kind(p);
        if (k == TK_LBRACE) depth++;
        else if (k == TK_RBRACE) {
            if (depth == 0) return;
            depth--;
        } else if (k == TK_NL && depth == 0) {
            return;
        }
        next(p);
    }
}

static Node *parse_block(Parser *p) {
    Token *open = expect(p, TK_LBRACE, "`{`");
    Node *b = node_new(N_BLOCK, open->pos);
    skip_nl(p);
    while (kind(p) != TK_RBRACE) {
        if (kind(p) == TK_EOF) {
            Pos pos = open->pos;
            perr(p, pos, "this `{` is never closed");
        }
        jmp_buf here;
        jmp_buf *prev = p->jb;
        p->jb = &here;
        if (setjmp(here) == 0) {
            Node *s = parse_stmt(p);
            vec_push(&b->list, s);
            if (kind(p) != TK_NL && kind(p) != TK_RBRACE)
                perr(p, peek(p)->pos, "expected a new line or `}` after the statement, found %s", describe(peek(p)));
        } else {
            p->jb = prev;
            if (p->quiet) longjmp(*p->jb, 1);
            sync_stmt(p);
        }
        p->jb = prev;
        skip_nl(p);
    }
    next(p);
    if (b->list.n > 0) {
        Node *last = b->list.v[b->list.n - 1];
        if (last->kind == N_EXPRSTMT) {
            b->a = last->a;
            b->list.n--;
        }
    }
    return b;
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

