/* deps.h - dependencies: fetching with git, the cache in ~/.zevpm, zevic.lock */
#ifndef ZEVPM_DEPS_H
#define ZEVPM_DEPS_H

#include "toml.h"

typedef struct {
    Manifest *m; /* the dependency's own zevic.toml */
    char *dir;   /* where its files are */
    char *id;    /* what was chosen (git url and commit, or the directory): for finding conflicts */
} Pkg;

/* Finds every dependency of `root`, and theirs, and fetches what is missing.
 * Commits come from zevic.lock when it has them. `update` is NULL (keep the lock), "" (update all) or the
 * name of one package to update. zevic.lock is written when something changed. Exits on errors. */
void deps_resolve(Manifest *root, const char *update, Vec *pkgs);

/* `zevpm get`: fetch the repository, add it to zevic.toml and zevic.lock, return its package name. */
char *deps_get(Manifest *root, const char *spec);

/* `zevpm remove`: drop the dependency from zevic.toml and zevic.lock. */
bool deps_remove(Manifest *root, const char *name);

#endif
