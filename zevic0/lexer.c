#include "lexer.h"

typedef struct {
    int file;
    const char *s;
    size_t n, i;
    int line, col;
    Token *v;
    int nv, cap;
    char stack[256]; /* open brackets: ( [ { */
    int sp;
    int angle; /* open `<` of type arguments, e.g. Array<I32> */
} Lexer;

static const char *g_tok_names[TK_COUNT] = {
    [TK_EOF] = "end of file", [TK_NL] = "newline", [TK_IDENT] = "identifier",
    [TK_INT] = "integer", [TK_FLOAT] = "number", [TK_CHAR] = "character",
    [TK_BYTE] = "byte", [TK_STRING] = "string",
    [TK_MODULE] = "module", [TK_IMPORT] = "import", [TK_AS] = "as", [TK_FN] = "fn",
    [TK_LET] = "let", [TK_MUT] = "mut", [TK_CLASS] = "class", [TK_STRUCT] = "struct",
    [TK_ENUM] = "enum", [TK_INTERFACE] = "interface", [TK_STATIC] = "static",
    [TK_SELF_T] = "Self", [TK_EXTERN] = "extern", [TK_EXPORT] = "export",
    [TK_IF] = "if", [TK_ELSE] = "else", [TK_WHILE] = "while", [TK_LOOP] = "loop",
    [TK_FOR] = "for", [TK_IN] = "in", [TK_MATCH] = "match", [TK_RETURN] = "return",
    [TK_BREAK] = "break", [TK_CONTINUE] = "continue", [TK_DEFER] = "defer", [TK_ASYNC] = "async", [TK_AWAIT] = "await",
    [TK_TRUE] = "true", [TK_FALSE] = "false", [TK_THIS] = "this",
    [TK_PLUS] = "+", [TK_MINUS] = "-", [TK_STAR] = "*", [TK_SLASH] = "/", [TK_PERCENT] = "%",
    [TK_AMP] = "&", [TK_PIPE] = "|", [TK_CARET] = "^", [TK_TILDE] = "~", [TK_BANG] = "!",
    [TK_SHL] = "<<", [TK_SHR] = ">>", [TK_ANDAND] = "&&", [TK_OROR] = "||",
    [TK_ASSIGN] = "=", [TK_EQEQ] = "==", [TK_NEQ] = "!=", [TK_LT] = "<", [TK_GT] = ">",
    [TK_LE] = "<=", [TK_GE] = ">=",
    [TK_PLUSEQ] = "+=", [TK_MINUSEQ] = "-=", [TK_STAREQ] = "*=", [TK_SLASHEQ] = "/=",
    [TK_PERCENTEQ] = "%=", [TK_AMPEQ] = "&=", [TK_PIPEEQ] = "|=", [TK_CARETEQ] = "^=",
    [TK_SHLEQ] = "<<=", [TK_SHREQ] = ">>=",
    [TK_DOT] = ".", [TK_DOTDOT] = "..", [TK_DOTDOTEQ] = "..=", [TK_ELLIPSIS] = "...",
    [TK_COMMA] = ",", [TK_COLON] = ":", [TK_ARROW] = "->", [TK_FATARROW] = "=>",
    [TK_QUESTION] = "?", [TK_AT] = "@",
    [TK_LPAREN] = "(", [TK_RPAREN] = ")", [TK_LBRACK] = "[", [TK_RBRACK] = "]",
    [TK_LBRACE] = "{", [TK_RBRACE] = "}",
};

const char *tok_name(TokKind k) {
    return g_tok_names[k] ? g_tok_names[k] : "?";
}

static const struct {
    const char *name;
    TokKind k;
} g_keywords[] = {
    {"module", TK_MODULE}, {"import", TK_IMPORT}, {"as", TK_AS}, {"fn", TK_FN},
    {"let", TK_LET}, {"mut", TK_MUT}, {"class", TK_CLASS}, {"struct", TK_STRUCT},
    {"enum", TK_ENUM}, {"interface", TK_INTERFACE}, {"static", TK_STATIC},
    {"Self", TK_SELF_T}, {"extern", TK_EXTERN}, {"export", TK_EXPORT},
    {"if", TK_IF}, {"else", TK_ELSE}, {"while", TK_WHILE}, {"loop", TK_LOOP},
    {"for", TK_FOR}, {"in", TK_IN}, {"match", TK_MATCH}, {"return", TK_RETURN},
    {"break", TK_BREAK}, {"continue", TK_CONTINUE}, {"defer", TK_DEFER}, {"async", TK_ASYNC}, {"await", TK_AWAIT},
    {"true", TK_TRUE}, {"false", TK_FALSE}, {"this", TK_THIS},
};

/* Words reserved for the future (the spec lists them). */
static const char *g_future[] = {"macro", "unsafe", "type",
                                 "where", "const", "yield", "impl", "null", NULL};

/* ---- cursor helpers ---- */

static Pos here(Lexer *lx) {
    Pos p = {lx->file, lx->line, lx->col};
    return p;
}

static int peekc(Lexer *lx, size_t off) {
    return lx->i + off < lx->n ? (unsigned char)lx->s[lx->i + off] : -1;
}

static int advance(Lexer *lx) {
    if (lx->i >= lx->n) return -1;
    unsigned char c = (unsigned char)lx->s[lx->i++];
    if (c == '\n') {
        lx->line++;
        lx->col = 1;
    } else if ((c & 0xC0) != 0x80) {
        lx->col++;
    }
    return c;
}

static bool is_ident_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_digit(int c) {
    return c >= '0' && c <= '9';
}
static bool is_ident_char(int c) {
    return is_ident_start(c) || is_digit(c);
}

static Token *push_tok(Lexer *lx, TokKind k, Pos pos) {
    if (lx->nv == lx->cap) {
        int nc = lx->cap ? lx->cap * 2 : 256;
        lx->v = zrealloc(lx->v, sizeof(Token) * (size_t)lx->cap, sizeof(Token) * (size_t)nc);
        lx->cap = nc;
    }
    Token *t = &lx->v[lx->nv++];
    memset(t, 0, sizeof *t);
    t->k = k;
    t->pos = pos;
    return t;
}

/* ---- newline handling ---- */

/* Does a token of this kind at the end of a line mean the statement continues? */
static bool continues_after(TokKind k) {
    switch (k) {
    case TK_PLUS: case TK_MINUS: case TK_STAR: case TK_SLASH: case TK_PERCENT:
    case TK_AMP: case TK_PIPE: case TK_CARET: case TK_SHL: case TK_SHR:
    case TK_ANDAND: case TK_OROR: case TK_EQEQ: case TK_NEQ:
    case TK_LT: case TK_GT: case TK_LE: case TK_GE:
    case TK_COMMA: case TK_LPAREN: case TK_LBRACK: case TK_LBRACE:
    case TK_ASSIGN: case TK_FATARROW: case TK_ARROW: case TK_DOT:
    case TK_PLUSEQ: case TK_MINUSEQ: case TK_STAREQ: case TK_SLASHEQ: case TK_PERCENTEQ:
    case TK_AMPEQ: case TK_PIPEEQ: case TK_CARETEQ: case TK_SHLEQ: case TK_SHREQ:
    case TK_COLON:
        return true;
    default:
        return false;
    }
}

/* After a newline: look ahead (skipping blanks, newlines and comments) to see
 * whether the next line starts with `.`, `&&` or `||`. */
static bool next_line_continues(Lexer *lx) {
    size_t i = lx->i;
    const char *s = lx->s;
    while (i < lx->n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            i++;
        } else if (c == '/' && i + 1 < lx->n && s[i + 1] == '/') {
            while (i < lx->n && s[i] != '\n') i++;
        } else if (c == '/' && i + 1 < lx->n && s[i + 1] == '*') {
            int depth = 1;
            i += 2;
            while (i < lx->n && depth > 0) {
                if (s[i] == '/' && i + 1 < lx->n && s[i + 1] == '*') { depth++; i += 2; }
                else if (s[i] == '*' && i + 1 < lx->n && s[i + 1] == '/') { depth--; i += 2; }
                else i++;
            }
        } else {
            break;
        }
    }
    if (i >= lx->n) return false;
    if (s[i] == '.' && !(i + 1 < lx->n && s[i + 1] == '.')) return true;
    if (s[i] == '&' && i + 1 < lx->n && s[i + 1] == '&') return true;
    if (s[i] == '|' && i + 1 < lx->n && s[i + 1] == '|') return true;
    return false;
}

static void newline(Lexer *lx, Pos pos) {
    if (lx->sp > 0 && lx->stack[lx->sp - 1] != '{') return; /* inside ( ) or [ ] */
    if (lx->nv == 0) return;
    TokKind last = lx->v[lx->nv - 1].k;
    if (lx->v[lx->nv - 1].generic_close) last = TK_IDENT; /* `Array<I32>` ends a type, not an expression */
    if (last == TK_NL || continues_after(last)) return;
    if (next_line_continues(lx)) return;
    lx->angle = 0;
    push_tok(lx, TK_NL, pos);
}

/* ---- main loop ---- */

static void skip_block_comment(Lexer *lx) {
    Pos start = here(lx);
    advance(lx);
    advance(lx); /* the opening slash-star */
    int depth = 1;
    while (depth > 0) {
        if (lx->i >= lx->n) {
            diag_error(start, "unterminated block comment");
            return;
        }
        if (peekc(lx, 0) == '/' && peekc(lx, 1) == '*') {
            advance(lx); advance(lx);
            depth++;
        } else if (peekc(lx, 0) == '*' && peekc(lx, 1) == '/') {
            advance(lx); advance(lx);
            depth--;
        } else {
            int c = advance(lx);
            if (c == '\n') newline(lx, start);
        }
    }
}

static bool lex_number(Lexer *lx);
static bool lex_string(Lexer *lx, bool raw);
static bool lex_char(Lexer *lx, bool is_byte);

static bool lex_ident(Lexer *lx) {
    Pos pos = here(lx);
    size_t start = lx->i;
    while (is_ident_char(peekc(lx, 0))) advance(lx);
    size_t n = lx->i - start;
    const char *s = lx->s + start;
    /* r"..." and b'...' prefixes */
    if (n == 1 && s[0] == 'r' && (peekc(lx, 0) == '"')) return lex_string(lx, true);
    if (n == 1 && s[0] == 'b' && peekc(lx, 0) == '\'') return lex_char(lx, true);
    for (size_t k = 0; k < sizeof g_keywords / sizeof g_keywords[0]; k++) {
        if (strlen(g_keywords[k].name) == n && memcmp(g_keywords[k].name, s, n) == 0) {
            push_tok(lx, g_keywords[k].k, pos);
            return true;
        }
    }
    for (int k = 0; g_future[k]; k++) {
        if (strlen(g_future[k]) == n && memcmp(g_future[k], s, n) == 0) {
            diag_error(pos, "`%s` is reserved for the future and cannot be used as a name (use `%s` in backticks)",
                       g_future[k], g_future[k]);
            break;
        }
    }
    Token *t = push_tok(lx, TK_IDENT, pos);
    t->text = zstrndup(s, n);
    return true;
}

static void lex_backtick(Lexer *lx) {
    Pos pos = here(lx);
    advance(lx);
    size_t start = lx->i;
    while (is_ident_char(peekc(lx, 0))) advance(lx);
    size_t n = lx->i - start;
    if (peekc(lx, 0) != '`' || n == 0) {
        diag_error(pos, "malformed `identifier`: expected a name followed by a closing backtick");
        return;
    }
    Token *t = push_tok(lx, TK_IDENT, pos);
    t->text = zstrndup(lx->s + start, n);
    t->quoted = true;
    advance(lx);
}

static void lex_op(Lexer *lx) {
    Pos pos = here(lx);
    int c = peekc(lx, 0), d = peekc(lx, 1), e = peekc(lx, 2);
    TokKind k = TK_EOF;
    int len = 1;
#define OP1(ch, kind) case ch: k = kind; break;
    switch (c) {
    case '+': if (d == '=') { k = TK_PLUSEQ; len = 2; } else k = TK_PLUS; break;
    case '-':
        if (d == '=') { k = TK_MINUSEQ; len = 2; }
        else if (d == '>') { k = TK_ARROW; len = 2; }
        else k = TK_MINUS;
        break;
    case '*': if (d == '=') { k = TK_STAREQ; len = 2; } else k = TK_STAR; break;
    case '/': if (d == '=') { k = TK_SLASHEQ; len = 2; } else k = TK_SLASH; break;
    case '%': if (d == '=') { k = TK_PERCENTEQ; len = 2; } else k = TK_PERCENT; break;
    case '&':
        if (d == '&') { k = TK_ANDAND; len = 2; }
        else if (d == '=') { k = TK_AMPEQ; len = 2; }
        else k = TK_AMP;
        break;
    case '|':
        if (d == '|') { k = TK_OROR; len = 2; }
        else if (d == '=') { k = TK_PIPEEQ; len = 2; }
        else k = TK_PIPE;
        break;
    case '^': if (d == '=') { k = TK_CARETEQ; len = 2; } else k = TK_CARET; break;
    OP1('~', TK_TILDE)
    case '!': if (d == '=') { k = TK_NEQ; len = 2; } else k = TK_BANG; break;
    case '=':
        if (d == '=') { k = TK_EQEQ; len = 2; }
        else if (d == '>') { k = TK_FATARROW; len = 2; }
        else k = TK_ASSIGN;
        break;
    case '<':
        if (d == '<') {
            if (e == '=') { k = TK_SHLEQ; len = 3; } else { k = TK_SHL; len = 2; }
        } else if (d == '=') { k = TK_LE; len = 2; }
        else {
            k = TK_LT;
            /* `Name<...`: glued to a name and not followed by a space, so it opens type arguments */
            if (lx->i > 0 && is_ident_char((unsigned char)lx->s[lx->i - 1]) && d != ' ' && d != '\t' && d != '\n' && d != -1)
                lx->angle++;
        }
        break;
    case '>':
        /* `>>` is only a shift token when it cannot close two generic lists;
         * the parser splits TK_SHR when it needs two TK_GT. */
        if (d == '>') {
            if (e == '=') { k = TK_SHREQ; len = 3; } else { k = TK_SHR; len = 2; }
        } else if (d == '=') { k = TK_GE; len = 2; }
        else k = TK_GT;
        break;
    case '.':
        if (d == '.' && e == '.') { k = TK_ELLIPSIS; len = 3; }
        else if (d == '.' && e == '=') { k = TK_DOTDOTEQ; len = 3; }
        else if (d == '.') { k = TK_DOTDOT; len = 2; }
        else k = TK_DOT;
        break;
    OP1(',', TK_COMMA)
    OP1(':', TK_COLON)
    OP1('?', TK_QUESTION)
    OP1('@', TK_AT)
    case '(': k = TK_LPAREN; if (lx->sp < 255) lx->stack[lx->sp++] = '('; break;
    case '[': k = TK_LBRACK; if (lx->sp < 255) lx->stack[lx->sp++] = '['; break;
    case '{': k = TK_LBRACE; if (lx->sp < 255) lx->stack[lx->sp++] = '{'; break;
    case ')': k = TK_RPAREN; if (lx->sp > 0) lx->sp--; break;
    case ']': k = TK_RBRACK; if (lx->sp > 0) lx->sp--; break;
    case '}': k = TK_RBRACE; if (lx->sp > 0) lx->sp--; break;
    default:
        break;
    }
#undef OP1
    if (k == TK_EOF) {
        if (c >= 0x80)
            diag_error(pos, "unexpected non-ASCII character outside a string or comment");
        else if (c == '#')
            diag_error(pos, "`%c` is not used in Zevic", c);
        else
            diag_error(pos, "unexpected character `%c`", c);
        advance(lx);
        return;
    }
    for (int j = 0; j < len; j++) advance(lx);
    Token *t = push_tok(lx, k, pos);
    if (k == TK_GT && lx->angle > 0) {
        lx->angle--;
        t->generic_close = true;
    } else if (k == TK_SHR && lx->angle >= 2) {
        lx->angle -= 2;
        t->generic_close = true;
    }
}

TokenList lex(int file, const char *text, size_t len, int line, int col) {
    Lexer lx = {0};
    lx.file = file;
    lx.s = text;
    lx.n = len;
    lx.line = line;
    lx.col = col;
    /* skip a UTF-8 BOM */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        lx.i = 3;
    }
    while (lx.i < lx.n) {
        int c = peekc(&lx, 0);
        Pos pos = here(&lx);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(&lx);
        } else if (c == '\n') {
            advance(&lx);
            newline(&lx, pos);
        } else if (c == ';') {
            advance(&lx);
            if (lx.nv > 0 && lx.v[lx.nv - 1].k != TK_NL) push_tok(&lx, TK_NL, pos);
        } else if (c == '/' && peekc(&lx, 1) == '/') {
            while (lx.i < lx.n && peekc(&lx, 0) != '\n') advance(&lx);
        } else if (c == '/' && peekc(&lx, 1) == '*') {
            skip_block_comment(&lx);
        } else if (is_ident_start(c)) {
            lex_ident(&lx);
        } else if (is_digit(c)) {
            lex_number(&lx);
        } else if (c == '"') {
            lex_string(&lx, false);
        } else if (c == '\'') {
            lex_char(&lx, false);
        } else if (c == '`') {
            lex_backtick(&lx);
        } else {
            lex_op(&lx);
        }
    }
    if (lx.nv > 0 && lx.v[lx.nv - 1].k != TK_NL) push_tok(&lx, TK_NL, here(&lx));
    push_tok(&lx, TK_EOF, here(&lx));
    TokenList out = {lx.v, lx.nv};
    return out;
}

/* ---- literals (filled in by later commits) ---- */

static const char *g_suffixes[] = {"I8", "I16", "I32", "I64", "U8", "U16", "U32", "U64",
                                   "F32", "F64", NULL};

/* If the text at offset `off` is `_` followed by a type suffix, return it. */
static const char *suffix_at(Lexer *lx, size_t off) {
    if (peekc(lx, off) != '_') return NULL;
    for (int k = 0; g_suffixes[k]; k++) {
        size_t m = strlen(g_suffixes[k]);
        if (lx->i + off + 1 + m <= lx->n && memcmp(lx->s + lx->i + off + 1, g_suffixes[k], m) == 0 &&
            !is_ident_char(peekc(lx, off + 1 + m))) {
            return g_suffixes[k];
        }
    }
    return NULL;
}

static int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

/* Read digits of `base` with single underscores between digits into `out`. */
static void read_digits(Lexer *lx, int base, SB *out, Pos pos) {
    for (;;) {
        int c = peekc(lx, 0);
        if (c == '_') {
            if (suffix_at(lx, 0)) return;
            int d = peekc(lx, 1);
            if (out->len == 0 || digit_value(d) >= base) {
                diag_error(pos, "`_` in a number must sit between two digits");
                advance(lx);
                continue;
            }
            advance(lx);
        } else if (c >= 0 && digit_value(c) < base && !(base <= 10 && !is_digit(c))) {
            sb_putc(out, (char)c);
            advance(lx);
        } else {
            return;
        }
    }
}

static bool lex_number(Lexer *lx) {
    Pos pos = here(lx);
    SB digits = {0};
    int base = 10;
    bool is_float = false;
    if (peekc(lx, 0) == '0' && (peekc(lx, 1) == 'x' || peekc(lx, 1) == 'b' || peekc(lx, 1) == 'o')) {
        base = peekc(lx, 1) == 'x' ? 16 : peekc(lx, 1) == 'b' ? 2 : 8;
        advance(lx);
        advance(lx);
        read_digits(lx, base, &digits, pos);
        if (digits.len == 0) diag_error(pos, "expected digits after the number prefix");
    } else {
        read_digits(lx, 10, &digits, pos);
        /* fraction: `.` must be followed by a digit (so `1..10` and `1.ToString()` work) */
        if (peekc(lx, 0) == '.' && is_digit(peekc(lx, 1))) {
            is_float = true;
            sb_putc(&digits, '.');
            advance(lx);
            SB frac = {0};
            read_digits(lx, 10, &frac, pos);
            sb_putn(&digits, sb_str(&frac), frac.len);
        } else if (peekc(lx, 0) == '.' && !is_ident_start(peekc(lx, 1)) && peekc(lx, 1) != '.') {
            diag_error(pos, "a number needs digits on both sides of `.` (write `1.0`, not `1.`)");
        }
        int c = peekc(lx, 0);
        if (c == 'e' || c == 'E') {
            int d = peekc(lx, 1);
            int sign = (d == '+' || d == '-') ? 1 : 0;
            if (is_digit(peekc(lx, 1 + (size_t)sign))) {
                is_float = true;
                sb_putc(&digits, 'e');
                advance(lx);
                if (sign) {
                    sb_putc(&digits, (char)peekc(lx, 0));
                    advance(lx);
                }
                while (is_digit(peekc(lx, 0))) {
                    sb_putc(&digits, (char)peekc(lx, 0));
                    advance(lx);
                }
            }
        }
    }
    const char *suffix = suffix_at(lx, 0);
    if (suffix) {
        for (size_t k = 0; k < strlen(suffix) + 1; k++) advance(lx);
    }
    if (is_ident_char(peekc(lx, 0))) {
        diag_error(pos, "unexpected `%c` after a number (a type is written like `255_U8`)", peekc(lx, 0));
        while (is_ident_char(peekc(lx, 0))) advance(lx);
    }
    char *txt = sb_str(&digits);
    if (suffix && suffix[0] == 'F') is_float = true;
    if (is_float) {
        Token *t = push_tok(lx, TK_FLOAT, pos);
        t->fval = strtod(txt, NULL);
        t->suffix = suffix;
        if (suffix && suffix[0] != 'F') diag_error(pos, "a decimal number cannot have the integer type `%s`", suffix);
        return true;
    }
    if (base == 10 && digits.len > 1 && txt[0] == '0')
        diag_error(pos, "a number cannot start with 0 (octal is written `0o17`)");
    uint64_t v = 0;
    bool overflow = false;
    for (size_t k = 0; k < digits.len; k++) {
        uint64_t d = (uint64_t)digit_value((unsigned char)txt[k]);
        if (v > (UINT64_MAX - d) / (uint64_t)base) overflow = true;
        v = v * (uint64_t)base + d;
    }
    if (overflow) diag_error(pos, "integer literal is too large (the maximum is 18446744073709551615)");
    Token *t = push_tok(lx, TK_INT, pos);
    t->ival = v;
    t->suffix = suffix;
    return true;
}
/* ---- characters and strings ---- */

/* Decode one UTF-8 sequence. Returns its length, or 0 when invalid. */
static int utf8_decode(const char *s, size_t n, uint32_t *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int len = (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (!len || (size_t)len > n) return 0;
    uint32_t v = len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (int k = 1; k < len; k++) {
        if (((unsigned char)s[k] & 0xC0) != 0x80) return 0;
        v = (v << 6) | ((unsigned char)s[k] & 0x3Fu);
    }
    if ((len == 2 && v < 0x80) || (len == 3 && v < 0x800) || (len == 4 && v < 0x10000) ||
        v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
        return 0;
    *cp = v;
    return len;
}

static void utf8_put(SB *sb, uint32_t cp) {
    if (cp < 0x80) {
        sb_putc(sb, (char)cp);
    } else if (cp < 0x800) {
        sb_putc(sb, (char)(0xC0 | (cp >> 6)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_putc(sb, (char)(0xE0 | (cp >> 12)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(sb, (char)(0xF0 | (cp >> 18)));
        sb_putc(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

/* Decode the escape whose backslash was just consumed; s[*i] is the letter.
 * `max_x` is the largest \xNN value allowed. Returns NULL on success or an error text. */
static const char *read_escape(const char *s, size_t n, size_t *i, uint32_t *cp, uint32_t max_x) {
    if (*i >= n) return "unfinished escape sequence";
    char c = s[(*i)++];
    switch (c) {
    case 'n': *cp = '\n'; return NULL;
    case 'r': *cp = '\r'; return NULL;
    case 't': *cp = '\t'; return NULL;
    case '\\': *cp = '\\'; return NULL;
    case '"': *cp = '"'; return NULL;
    case '\'': *cp = '\''; return NULL;
    case '0': *cp = 0; return NULL;
    case '$': *cp = '$'; return NULL;
    case 'x': {
        if (*i + 2 > n || digit_value((unsigned char)s[*i]) > 15 || digit_value((unsigned char)s[*i + 1]) > 15)
            return "`\\x` must be followed by two hex digits";
        *cp = (uint32_t)(digit_value((unsigned char)s[*i]) * 16 + digit_value((unsigned char)s[*i + 1]));
        *i += 2;
        if (*cp > max_x) return max_x == 0x7F ? "`\\xNN` in a string must be 00 to 7F (use `\\u{...}` for other characters)"
                                               : "`\\xNN` is out of range";
        return NULL;
    }
    case 'u': {
        if (*i >= n || s[*i] != '{') return "`\\u` must be followed by `{hex digits}`";
        (*i)++;
        uint32_t v = 0;
        int nd = 0;
        while (*i < n && s[*i] != '}') {
            int d = digit_value((unsigned char)s[*i]);
            if (d > 15 || nd >= 6) return "invalid `\\u{...}` escape";
            v = v * 16 + (uint32_t)d;
            nd++;
            (*i)++;
        }
        if (*i >= n || nd == 0) return "invalid `\\u{...}` escape";
        (*i)++;
        if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return "`\\u{...}` is not a valid Unicode scalar value";
        *cp = v;
        return NULL;
    }
    default:
        return "unknown escape sequence";
    }
}

/* Cursor is on the opening quote (a preceding `b` was already consumed). */
static bool lex_char(Lexer *lx, bool is_byte) {
    Pos pos = here(lx);
    if (is_byte) pos.col--; /* point at the `b` */
    advance(lx);
    uint32_t cp = 0;
    const char *s = lx->s;
    size_t i = lx->i, n = lx->n;
    if (i >= n || s[i] == '\n') {
        diag_error(pos, "unterminated character literal");
        return false;
    }
    if (s[i] == '\'') {
        diag_error(pos, "empty character literal");
        advance(lx);
        return false;
    }
    size_t start = i;
    if (s[i] == '\\') {
        i++;
        const char *err = read_escape(s, n, &i, &cp, is_byte ? 0xFF : 0x10FFFF);
        if (err) {
            diag_error(pos, "%s", err);
            return false;
        }
    } else {
        int len = utf8_decode(s + i, n - i, &cp);
        if (!len) {
            diag_error(pos, "invalid UTF-8 in character literal");
            return false;
        }
        i += (size_t)len;
        if (is_byte && cp >= 0x80) diag_error(pos, "a byte literal must be ASCII (use `b'\\xNN'` for other values)");
    }
    if (i >= n || s[i] != '\'') {
        diag_error(pos, "a character literal holds exactly one character (use a string for more)");
        /* resync: skip to the closing quote on this line */
        while (lx->i < lx->n && peekc(lx, 0) != '\'' && peekc(lx, 0) != '\n') advance(lx);
        if (peekc(lx, 0) == '\'') advance(lx);
        return false;
    }
    i++;
    while (lx->i < i) advance(lx);
    (void)start;
    Token *t = push_tok(lx, is_byte ? TK_BYTE : TK_CHAR, pos);
    t->ival = cp;
    return true;
}

/* Skip over `${ ... }` (i is just after the `${`). Returns the index of the matching `}`. */
static size_t skip_interp(const char *s, size_t n, size_t i);

/* Skip a string body; i is just after the opening quote. Returns the index just after the closing quote. */
static size_t skip_nested_string(const char *s, size_t n, size_t i) {
    while (i < n) {
        char c = s[i];
        if (c == '\\') {
            i += 2;
        } else if (c == '"') {
            return i + 1;
        } else if (c == '$' && i + 1 < n && s[i + 1] == '{') {
            i = skip_interp(s, n, i + 2) + 1;
        } else {
            i++;
        }
    }
    return n;
}

static size_t skip_interp(const char *s, size_t n, size_t i) {
    int depth = 1;
    while (i < n) {
        char c = s[i];
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            if (--depth == 0) return i;
        } else if (c == '"') {
            i = skip_nested_string(s, n, i + 1);
            continue;
        } else if (c == '\'') {
            i++;
            if (i < n && s[i] == '\\') i++;
            while (i < n && s[i] != '\'' && s[i] != '\n') i++;
        }
        i++;
    }
    return n;
}

typedef struct {
    StrPart *v;
    int n, cap;
} Parts;

static void parts_add(Parts *ps, bool is_expr, const char *s, size_t n, Pos pos) {
    if (ps->n == ps->cap) {
        int nc = ps->cap ? ps->cap * 2 : 4;
        ps->v = zrealloc(ps->v, sizeof(StrPart) * (size_t)ps->cap, sizeof(StrPart) * (size_t)nc);
        ps->cap = nc;
    }
    StrPart *p = &ps->v[ps->n++];
    p->is_expr = is_expr;
    p->s = zstrndup(s, n);
    p->n = (int)n;
    p->pos = pos;
}

/* Turn the body of a string literal into literal and `${}` parts. */
static void split_body(const char *s, size_t n, bool raw, Pos pos, Parts *ps) {
    SB cur = {0};
    size_t i = 0;
    int line = pos.line, col = pos.col;
    Pos cur_pos = pos;
    while (i < n) {
        char c = s[i];
        if (!raw && c == '\\') {
            size_t j = i + 1;
            uint32_t cp;
            const char *err = read_escape(s, n, &j, &cp, 0x7F);
            if (err) {
                Pos ep = {pos.file, line, col};
                diag_error(ep, "%s", err);
                j = i + 2 > n ? n : i + 2;
            } else {
                utf8_put(&cur, cp);
            }
            col += (int)(j - i);
            i = j;
        } else if (!raw && c == '$' && i + 1 < n && s[i + 1] == '{') {
            size_t end = skip_interp(s, n, i + 2);
            if (end >= n) {
                Pos ep = {pos.file, line, col};
                diag_error(ep, "unterminated `${` in a string");
                return;
            }
            if (cur.len) {
                parts_add(ps, false, sb_str(&cur), cur.len, cur_pos);
                cur.len = 0;
            }
            Pos ep = {pos.file, line, col + 2};
            parts_add(ps, true, s + i + 2, end - (i + 2), ep);
            for (size_t k = i; k <= end; k++) {
                if (s[k] == '\n') { line++; col = 1; } else if (((unsigned char)s[k] & 0xC0) != 0x80) col++;
            }
            i = end + 1;
            cur_pos.line = line;
            cur_pos.col = col;
        } else {
            if (cur.len == 0) { cur_pos.line = line; cur_pos.col = col; }
            sb_putc(&cur, c);
            if (c == '\n') { line++; col = 1; } else if (((unsigned char)c & 0xC0) != 0x80) col++;
            i++;
        }
    }
    if (cur.len) parts_add(ps, false, sb_str(&cur), cur.len, cur_pos);
}

static void check_utf8(const char *s, size_t n, Pos pos) {
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        int len = utf8_decode(s + i, n - i, &cp);
        if (!len) {
            diag_error(pos, "invalid UTF-8 in a string literal");
            return;
        }
        i += (size_t)len;
    }
}

/* Cursor is on the opening quote. */
static bool lex_string(Lexer *lx, bool raw) {
    Pos pos = here(lx);
    if (raw) pos.col--; /* the `r` */
    const char *s = lx->s;
    size_t n = lx->n, i = lx->i;
    bool triple = i + 2 < n && s[i + 1] == '"' && s[i + 2] == '"';
    Parts ps = {0};
    size_t body_start, body_end;
    if (triple) {
        i += 3;
        /* the opening quotes must be followed by a newline */
        size_t j = i;
        while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r')) j++;
        if (j >= n || s[j] != '\n') {
            diag_error(pos, "a `\"\"\"` string must start on a new line (put the text after the line break)");
        } else {
            i = j + 1;
        }
        body_start = i;
        size_t k = i;
        for (;;) {
            if (k >= n) {
                diag_error(pos, "unterminated multi-line string");
                while (lx->i < n) advance(lx);
                return false;
            }
            if (!raw && s[k] == '\\') { k += 2; continue; }
            if (!raw && s[k] == '$' && k + 1 < n && s[k + 1] == '{') { k = skip_interp(s, n, k + 2) + 1; continue; }
            if (s[k] == '"' && k + 2 < n && s[k + 1] == '"' && s[k + 2] == '"') break;
            k++;
        }
        body_end = k;
        size_t total = k + 3;
        /* dedent: the closing line's indentation is removed from every line */
        size_t ls = body_end;
        while (ls > body_start && s[ls - 1] != '\n') ls--;
        size_t indent = body_end - ls;
        bool blank = true;
        for (size_t q = ls; q < body_end; q++)
            if (s[q] != ' ' && s[q] != '\t') blank = false;
        SB text = {0};
        if (!blank) {
            diag_error(pos, "the closing `\"\"\"` must be on its own line");
            sb_putn(&text, s + body_start, body_end - body_start);
        } else {
            size_t p = body_start;
            size_t content_end = ls == body_start ? body_start : ls - 1; /* drop the last newline */
            bool first = true;
            while (p <= content_end && ls > body_start) {
                size_t e = p;
                while (e < content_end && s[e] != '\n') e++;
                size_t lstart = p;
                size_t skip = 0;
                while (skip < indent && lstart + skip < e && (s[lstart + skip] == ' ' || s[lstart + skip] == '\t')) skip++;
                if (skip < indent && e > lstart + skip) {
                    Pos lp = {pos.file, pos.line, pos.col};
                    diag_error(lp, "a line in a multi-line string is indented less than the closing `\"\"\"`");
                }
                if (!first) sb_putc(&text, '\n');
                first = false;
                size_t cut = e;
                if (cut > lstart && s[cut - 1] == '\r') cut--;
                sb_putn(&text, s + lstart + skip, cut - (lstart + skip));
                if (e >= content_end) break;
                p = e + 1;
            }
        }
        Pos bp = {pos.file, pos.line + 1, 1};
        check_utf8(text.p ? text.p : "", text.len, pos);
        split_body(text.p ? text.p : "", text.len, raw, bp, &ps);
        while (lx->i < total) advance(lx);
    } else {
        i += 1;
        body_start = i;
        size_t k = i;
        for (;;) {
            if (k >= n || s[k] == '\n') {
                diag_error(pos, "unterminated string (a string cannot span lines; use `\"\"\"` for that)");
                while (lx->i < k) advance(lx);
                return false;
            }
            if (!raw && s[k] == '\\') { k += 2; continue; }
            if (!raw && s[k] == '$' && k + 1 < n && s[k + 1] == '{') { k = skip_interp(s, n, k + 2) + 1; continue; }
            if (s[k] == '"') break;
            k++;
        }
        body_end = k;
        check_utf8(s + body_start, body_end - body_start, pos);
        Pos bp = {pos.file, pos.line, pos.col + (raw ? 2 : 1)};
        split_body(s + body_start, body_end - body_start, raw, bp, &ps);
        while (lx->i < body_end + 1) advance(lx);
    }
    Token *t = push_tok(lx, TK_STRING, pos);
    t->parts = ps.v;
    t->nparts = ps.n;
    return true;
}
