/* main.c - command line entry point of zevic0 */
#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include "util.h"

static void dump_tokens(const char *path) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) die("cannot read %s", path);
    int file = src_add(path, text);
    TokenList tl = lex(file, text, len, 1, 1);
    for (int i = 0; i < tl.n; i++) {
        Token *t = &tl.v[i];
        printf("%d:%d %s", t->pos.line, t->pos.col, tok_name(t->k));
        switch (t->k) {
        case TK_IDENT: printf(" %s%s", t->text, t->quoted ? " (quoted)" : ""); break;
        case TK_INT: case TK_CHAR: case TK_BYTE:
            printf(" %llu", (unsigned long long)t->ival);
            if (t->suffix) printf(" _%s", t->suffix);
            break;
        case TK_FLOAT:
            printf(" %g", t->fval);
            if (t->suffix) printf(" _%s", t->suffix);
            break;
        case TK_STRING:
            for (int k = 0; k < t->nparts; k++) {
                StrPart *p = &t->parts[k];
                printf(" %s[", p->is_expr ? "expr" : "text");
                for (int j = 0; j < p->n; j++) {
                    unsigned char c = (unsigned char)p->s[j];
                    if (c == '\n') printf("\\n");
                    else if (c < 32) printf("\\x%02x", c);
                    else putchar(c);
                }
                printf("]");
            }
            break;
        default: break;
        }
        printf("\n");
    }
}

static void dump_ast(const char *path) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) die("cannot read %s", path);
    int file = src_add(path, text);
    SourceFile *sf = parse_file(file, text, len);
    if (!zv_error_count) ast_dump(sf, stdout);
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--dump-ast") == 0) {
        dump_ast(argv[2]);
        return zv_error_count ? 1 : 0;
    }
    if (argc == 3 && strcmp(argv[1], "--dump-tokens") == 0) {
        dump_tokens(argv[2]);
        return zv_error_count ? 1 : 0;
    }
    fprintf(stderr, "usage: zevic <file.zev> [-o out]\n");
    return 1;
}
