/* lexer.h - tokens and the lexer */
#ifndef ZEVIC_LEXER_H
#define ZEVIC_LEXER_H

#include "util.h"

typedef enum {
    TK_EOF,
    TK_NL, /* a statement-ending newline (or ';') */
    TK_IDENT,
    TK_INT,
    TK_FLOAT,
    TK_CHAR,
    TK_BYTE,
    TK_STRING,

    /* keywords */
    TK_MODULE, TK_IMPORT, TK_AS, TK_FN, TK_LET, TK_MUT,
    TK_CLASS, TK_STRUCT, TK_ENUM, TK_INTERFACE, TK_STATIC, TK_SELF_T,
    TK_EXTERN, TK_EXPORT,
    TK_IF, TK_ELSE, TK_WHILE, TK_LOOP, TK_FOR, TK_IN, TK_MATCH,
    TK_RETURN, TK_BREAK, TK_CONTINUE, TK_DEFER,
    TK_TRUE, TK_FALSE, TK_THIS,

    /* punctuation and operators */
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_BANG,
    TK_SHL, TK_SHR, TK_ANDAND, TK_OROR,
    TK_ASSIGN, TK_EQEQ, TK_NEQ, TK_LT, TK_GT, TK_LE, TK_GE,
    TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_PERCENTEQ,
    TK_AMPEQ, TK_PIPEEQ, TK_CARETEQ, TK_SHLEQ, TK_SHREQ,
    TK_DOT, TK_DOTDOT, TK_DOTDOTEQ, TK_ELLIPSIS,
    TK_COMMA, TK_COLON, TK_ARROW, TK_FATARROW, TK_QUESTION,
    TK_LPAREN, TK_RPAREN, TK_LBRACK, TK_RBRACK, TK_LBRACE, TK_RBRACE,
    TK_COUNT
} TokKind;

/* One piece of a string literal: literal text, or the source of a ${...} expression. */
typedef struct {
    bool is_expr;
    char *s; /* decoded bytes (literal) or raw source (expression) */
    int n;
    Pos pos; /* position of the first character */
} StrPart;

typedef struct {
    TokKind k;
    Pos pos;
    char *text;      /* identifier name */
    uint64_t ival;   /* TK_INT, TK_CHAR, TK_BYTE */
    double fval;     /* TK_FLOAT */
    const char *suffix; /* "I32", "F64", ... or NULL */
    StrPart *parts;  /* TK_STRING */
    int nparts;
    bool quoted;     /* identifier written with backticks */
    bool generic_close; /* a `>` that closes type arguments (it does not continue the line) */
} Token;

typedef struct {
    Token *v;
    int n;
} TokenList;

const char *tok_name(TokKind k);

/* Tokenize `text`. `base` is the position of the first character (used for
 * the source of ${...} expressions). Errors are reported through diag_error. */
TokenList lex(int file, const char *text, size_t len, int line, int col);

#endif
