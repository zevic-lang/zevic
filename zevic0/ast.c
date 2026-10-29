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
