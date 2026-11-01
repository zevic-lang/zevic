/* toml.h - zevic.toml: the small subset of TOML that the manifest uses */
#ifndef ZEVPM_TOML_H
#define ZEVPM_TOML_H

#include "os.h"

typedef struct {
    char *name;
    char *git, *tag, *branch, *rev; /* a git dependency (at most one of tag, branch, rev) */
    char *path;                     /* or a dependency in a local directory */
} Dep;

typedef struct {
    char *dir;  /* the package directory */
    char *file; /* .../zevic.toml */
    char *name, *version, *kind, *zevic_req;
    Vec deps; /* Dep* */
    Vec libs; /* char*: [native] libs */
} Manifest;

/* Reads `dir`/zevic.toml. Reports the problem and exits with 1 when it is missing or wrong. */
Manifest *manifest_load(const char *dir);

Dep *manifest_find_dep(Manifest *m, const char *name);
/* The line `Name = { git = "...", tag = "..." }` that describes a dependency. */
char *dep_line(const Dep *d);
/* Add (or replace) a dependency in the file, keeping the rest of the file as it is. */
void manifest_add_dep(Manifest *m, const Dep *d);
/* Remove a dependency from the file. Returns false when there is none by that name. */
bool manifest_remove_dep(Manifest *m, const char *name);

bool is_pascal_case(const char *s);

#endif
