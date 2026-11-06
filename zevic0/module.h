/* module.h - loading files and resolving modules and imports */
#ifndef ZEVIC_MODULE_H
#define ZEVIC_MODULE_H

#include "ast.h"

typedef struct Module Module;

struct Type;

typedef struct Decl {
    Node *node;     /* D_FN, D_STRUCT, D_CLASS, D_ENUM, D_INTERFACE or D_CONST */
    Module *mod;
    SourceFile *file;
    char *name;
    char *abi;      /* for functions declared in an extern block: "C" or "zevic" */
    bool is_pub;
    struct Type *iface; /* a default method taken from this interface type (with its type arguments) */
    char *test_name;    /* for the hidden function of a `test "name" { }` block: the name */
} Decl;

struct Module {
    char *name;
    Vec files; /* SourceFile* */
    Dict decls;
    Vec tests; /* Decl*: one hidden function per `test` block, in source order */
    Vec deps;  /* Module* */
    bool is_std;
    int visit; /* cycle detection */
};

typedef struct {
    Vec modules;
    Dict mod_by_name;
    Vec files;
    char *std_dir;  /* directory holding the standard library, e.g. .../packages/Zevic/src */
    Vec search_dirs;
} Program;

Program *prog_new(const char *std_dir);
/* Parse and register a file. Returns NULL when it cannot be read. */
SourceFile *prog_load_file(Program *pg, const char *path, bool is_std);
/* Load every module the registered files import, then check for cycles. */
void prog_resolve(Program *pg);
Module *prog_find_module(Program *pg, const char *name);
Decl *module_find(Module *m, const char *name);

#endif
