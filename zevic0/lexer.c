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
    [TK_BREAK] = "break", [TK_CONTINUE] = "continue", [TK_DEFER] = "defer",
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
    [TK_QUESTION] = "?",
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
    {"break", TK_BREAK}, {"continue", TK_CONTINUE}, {"defer", TK_DEFER},
    {"true", TK_TRUE}, {"false", TK_FALSE}, {"this", TK_THIS},
};

/* Words reserved for the future (the spec lists them). */
static const char *g_future[] = {"async", "await", "macro", "unsafe", "type",
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
    if (last == TK_NL || continues_after(last)) return;
    if (next_line_continues(lx)) return;
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
        else k = TK_LT;
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
        else if (c == '@' || c == '#')
            diag_error(pos, "`%c` is not used in Zevic", c);
        else
            diag_error(pos, "unexpected character `%c`", c);
        advance(lx);
        return;
    }
    for (int j = 0; j < len; j++) advance(lx);
    push_tok(lx, k, pos);
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
static bool lex_string(Lexer *lx, bool raw) {
    advance(lx); (void)raw;
    return false;
}
static bool lex_char(Lexer *lx, bool is_byte) {
    advance(lx); (void)is_byte;
    return false;
}
