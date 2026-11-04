#define _POSIX_C_SOURCE 200809L
#include "deps.h"

#include <stdint.h>

#include <ctype.h>
#include <time.h>

/* ---- zevic.lock ---- */

typedef struct {
    char *name, *git, *ref, *rev;
} LockEntry;

static Vec g_lock; /* LockEntry* */
static bool g_lock_dirty;

static char *lock_path(Manifest *root) {
    return path_join(root->dir, "zevic.lock");
}

static char *unquote(const char *v) {
    while (*v == ' ') v++;
    if (*v != '"') return NULL;
    const char *e = strrchr(v, '"');
    if (e == v) return NULL;
    return zstrndup(v + 1, (size_t)(e - v - 1));
}

static void lock_load(Manifest *root) {
    g_lock = (Vec){0};
    g_lock_dirty = false;
    size_t len;
    char *text = read_file(lock_path(root), &len);
    if (!text) return;
    LockEntry *cur = NULL;
    char *p = text;
    while (*p) {
        char *eol = strchr(p, '\n');
        char *line = eol ? zstrndup(p, (size_t)(eol - p)) : zstrdup(p);
        p = eol ? eol + 1 : p + strlen(p);
        if (strncmp(line, "[[package]]", 11) == 0) {
            cur = zalloc(sizeof *cur);
            vec_push(&g_lock, cur);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!cur || !eq) continue;
        char *key = zstrndup(line, (size_t)(eq - line));
        size_t kn = strlen(key);
        while (kn > 0 && key[kn - 1] == ' ') key[--kn] = 0;
        char *val = unquote(eq + 1);
        if (!val) continue;
        if (strcmp(key, "name") == 0) cur->name = val;
        else if (strcmp(key, "git") == 0) cur->git = val;
        else if (strcmp(key, "ref") == 0) cur->ref = val;
        else if (strcmp(key, "rev") == 0) cur->rev = val;
    }
}

static LockEntry *lock_find(const char *name) {
    for (int i = 0; i < g_lock.n; i++) {
        LockEntry *e = g_lock.v[i];
        if (e->name && strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

static void lock_set(const char *name, const char *git, const char *ref, const char *rev) {
    LockEntry *e = lock_find(name);
    if (e && e->git && e->ref && e->rev && !strcmp(e->git, git) && !strcmp(e->ref, ref) && !strcmp(e->rev, rev)) return;
    if (!e) {
        e = zalloc(sizeof *e);
        vec_push(&g_lock, e);
    }
    e->name = zstrdup(name);
    e->git = zstrdup(git);
    e->ref = zstrdup(ref);
    e->rev = zstrdup(rev);
    g_lock_dirty = true;
}

static int cmp_entry(const void *a, const void *b) {
    return strcmp((*(LockEntry *const *)a)->name, (*(LockEntry *const *)b)->name);
}

static void lock_save(Manifest *root) {
    if (g_lock.n > 1) qsort(g_lock.v, (size_t)g_lock.n, sizeof(void *), cmp_entry);
    char *path = lock_path(root);
    if (g_lock.n == 0) {
        remove(path);
        return;
    }
    SB sb = {0};
    sb_puts(&sb, "# zevic.lock: the exact commits of the dependencies. Written by zevpm; commit this file.\n");
    for (int i = 0; i < g_lock.n; i++) {
        LockEntry *e = g_lock.v[i];
        sb_printf(&sb, "\n[[package]]\nname = \"%s\"\ngit = \"%s\"\nref = \"%s\"\nrev = \"%s\"\n", e->name, e->git, e->ref, e->rev);
    }
    if (!os_write_file(path, sb_str(&sb))) {
        fprintf(stderr, "error: cannot write `%s`\n", path);
        exit(2);
    }
}

/* ---- git ---- */

static void need_git(void) {
    static bool checked;
    if (checked) return;
    if (!os_find_in_path("git")) {
        fprintf(stderr, "error: git dependencies need `git`, but it was not found on PATH\n"
                        "  install it (apt install git / brew install git / winget install Git.Git) and try again\n");
        exit(2);
    }
    checked = true;
}

static int git(Vec *args, const char *dir) {
    Vec argv = {0};
    vec_push(&argv, "git");
    for (int i = 0; i < args->n; i++) vec_push(&argv, args->v[i]);
    return os_run(&argv, dir);
}

static char *cache_root(void) {
    const char *env = getenv("ZEVPM_HOME");
    if (env && *env) return zstrdup(env);
    char *home = os_home_dir();
    if (!home) {
        fprintf(stderr, "error: cannot find the home directory (set ZEVPM_HOME to choose where to keep downloads)\n");
        exit(2);
    }
    return path_join(home, ".zevpm");
}

/* The name of a repository's directory in the cache. Long addresses are cut and given a hash (32-bit FNV-1a):
 * paths are limited to 260 characters on Windows, and git adds a long tail of its own. */
static char *sanitize(const char *url) {
    SB sb = {0};
    for (const char *p = url; *p; p++) sb_putc(&sb, isalnum((unsigned char)*p) || *p == '.' || *p == '-' ? *p : '_');
    char *full = sb_str(&sb);
    size_t n = strlen(full);
    if (n <= 40) return full;
    uint32_t h = 2166136261u;
    for (const char *p = url; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return zsprintf("%s-%u", full + n - 32, (unsigned)h);
}

static bool is_full_hash(const char *s) {
    if (strlen(s) != 40) return false;
    for (; *s; s++)
        if (!isxdigit((unsigned char)*s)) return false;
    return true;
}

/* How the dependency asks for its version, as text: "tag v1.0", "branch main", "rev <hash>" or "default". */
static char *ref_of(const Dep *d) {
    if (d->tag) return zsprintf("tag %s", d->tag);
    if (d->branch) return zsprintf("branch %s", d->branch);
    if (d->rev) return zsprintf("rev %s", d->rev);
    return zstrdup("default");
}

static void fetch_fail(const Dep *d, const char *what) {
    fprintf(stderr, "error: cannot %s `%s`%s%s\n", what, d->git, d->tag ? " at tag " : d->branch ? " at branch " : "",
            d->tag ? d->tag : d->branch ? d->branch : "");
    fprintf(stderr, "  check the address and your network connection, then try again\n");
    exit(2);
}

/* Get the files of a git dependency into the cache and return the directory.
 * `rev` is the commit to use when known (from the lock); otherwise the newest commit of the ref is taken.
 * *rev_out receives the commit. */
static char *fetch_git(const Dep *d, const char *rev, char **rev_out) {
    need_git();
    char *root = cache_root();
    char *repo_dir = path_join(path_join(root, "git"), sanitize(d->git));
    if (d->rev) {
        if (!is_full_hash(d->rev)) {
            fprintf(stderr, "error: dependency `%s`: `rev` must be the full 40-character commit hash\n", d->name);
            exit(1);
        }
        rev = d->rev;
    }
    if (rev) {
        char *target = path_join(repo_dir, rev);
        if (os_is_dir(target)) {
            *rev_out = zstrdup(rev);
            return target;
        }
    }
    os_mkdirs(repo_dir);
    static int counter;
    char *tmp = path_join(repo_dir, zsprintf("tmp-%ld-%d", (long)time(NULL), counter++));
    os_rmtree(tmp);
    Vec a = {0};
    if (rev) {
        /* a known commit: ask for exactly that one */
        os_mkdirs(tmp);
        vec_push(&a, "init"), vec_push(&a, "-q");
        if (git(&a, tmp) != 0) fetch_fail(d, "prepare a download for");
        a = (Vec){0};
        vec_push(&a, "fetch"), vec_push(&a, "-q"), vec_push(&a, "--depth"), vec_push(&a, "1");
        vec_push(&a, d->git), vec_push(&a, (void *)rev);
        if (git(&a, tmp) != 0) {
            os_rmtree(tmp);
            fetch_fail(d, "fetch the commit from");
        }
        a = (Vec){0};
        vec_push(&a, "checkout"), vec_push(&a, "-q"), vec_push(&a, "FETCH_HEAD");
        if (git(&a, tmp) != 0) {
            os_rmtree(tmp);
            fetch_fail(d, "check out the commit of");
        }
    } else {
        vec_push(&a, "clone"), vec_push(&a, "-q"), vec_push(&a, "--depth"), vec_push(&a, "1");
        if (d->tag || d->branch) {
            vec_push(&a, "--branch");
            vec_push(&a, (void *)(d->tag ? d->tag : d->branch));
        }
        vec_push(&a, (void *)d->git), vec_push(&a, tmp);
        if (git(&a, NULL) != 0) {
            os_rmtree(tmp);
            fetch_fail(d, "clone");
        }
    }
    char *head = NULL;
    Vec r = {0};
    vec_push(&r, "rev-parse"), vec_push(&r, "HEAD");
    Vec argv = {0};
    vec_push(&argv, "git");
    for (int i = 0; i < r.n; i++) vec_push(&argv, r.v[i]);
    if (os_run_capture(&argv, tmp, &head) != 0 || !head || !is_full_hash(head)) {
        os_rmtree(tmp);
        fetch_fail(d, "read the commit of");
    }
    os_rmtree(path_join(tmp, ".git"));
    char *target = path_join(repo_dir, head);
    if (os_is_dir(target)) os_rmtree(tmp);
    else if (!os_rename(tmp, target)) {
        fprintf(stderr, "error: cannot move the download to `%s`\n", target);
        exit(2);
    }
    *rev_out = head;
    return target;
}

/* ---- resolving ---- */

typedef struct {
    Vec pkgs;   /* Pkg* */
    Dict seen;  /* name -> Pkg* */
    const char *update;
    char *root_name;
} Walk;

static bool is_update_target(Walk *w, const char *name) {
    return w->update && (w->update[0] == 0 || strcmp(w->update, name) == 0);
}

static void walk_deps(Walk *w, Manifest *owner) {
    for (int i = 0; i < owner->deps.n; i++) {
        Dep *d = owner->deps.v[i];
        char *dir, *id, *rev = NULL;
        if (d->path) {
            dir = path_join(owner->dir, d->path);
            if (!os_is_dir(dir)) {
                fprintf(stderr, "error: dependency `%s` (in %s): the directory `%s` does not exist\n", d->name, owner->file, dir);
                exit(1);
            }
            id = zsprintf("path %s", dir);
        } else {
            LockEntry *le = lock_find(d->name);
            char *ref = ref_of(d);
            const char *want = NULL;
            if (le && le->git && le->ref && le->rev && !strcmp(le->git, d->git) && !strcmp(le->ref, ref) &&
                !is_update_target(w, d->name))
                want = le->rev;
            dir = fetch_git(d, want, &rev);
            lock_set(d->name, d->git, ref, rev);
            id = zsprintf("git %s @ %s", d->git, rev);
        }
        Pkg *old = dict_get(&w->seen, d->name);
        if (old) {
            if (strcmp(old->id, id) != 0) {
                fprintf(stderr, "error: the package `%s` is needed in two different versions:\n  %s\n  %s\n"
                                "  zevpm does not choose between versions: edit zevic.toml so that all agree\n",
                        d->name, old->id, id);
                exit(1);
            }
            continue;
        }
        Pkg *p = zalloc(sizeof *p);
        p->dir = dir;
        p->id = id;
        p->m = manifest_load(dir);
        if (strcmp(p->m->name, d->name) != 0) {
            fprintf(stderr, "error: `%s` lists the dependency as `%s`, but its package is named `%s`\n", owner->file, d->name,
                    p->m->name);
            exit(1);
        }
        dict_put(&w->seen, d->name, p);
        vec_push(&w->pkgs, p);
        walk_deps(w, p->m);
    }
}

/* Drop lock entries that no dependency uses any more. */
static void lock_prune(Walk *w) {
    Vec keep = {0};
    for (int i = 0; i < g_lock.n; i++) {
        LockEntry *e = g_lock.v[i];
        if (e->name && dict_get(&w->seen, e->name)) vec_push(&keep, e);
        else g_lock_dirty = true;
    }
    g_lock = keep;
}

void deps_resolve(Manifest *root, const char *update, Vec *pkgs) {
    lock_load(root);
    Walk w = {0};
    w.update = update;
    walk_deps(&w, root);
    lock_prune(&w);
    if (g_lock_dirty) lock_save(root);
    *pkgs = w.pkgs;
}

/* ---- get and remove ---- */

static bool looks_like_version(const char *s) {
    if (*s == 'v') s++;
    return isdigit((unsigned char)*s) != 0;
}

char *deps_get(Manifest *root, const char *spec) {
    Dep *d = zalloc(sizeof *d);
    char *s = zstrdup(spec);
    /* name@ref: the `@` after the last `/` */
    char *slash = strrchr(s, '/');
    char *at = strrchr(s, '@');
    if (at && (!slash || at > slash)) {
        *at++ = 0;
        if (!*at) {
            fprintf(stderr, "error: `%s`: nothing after `@`\n", spec);
            exit(1);
        }
        if (is_full_hash(at)) d->rev = at;
        else if (looks_like_version(at)) d->tag = at;
        else d->branch = at;
    }
    if (strstr(s, "://") || strncmp(s, "git@", 4) == 0) {
        d->git = s;
    } else if (strncmp(s, "github.com/", 11) == 0) {
        d->git = zsprintf("https://%s", s);
    } else if (strchr(s, '/') && strchr(s, '/') == strrchr(s, '/')) {
        d->git = zsprintf("https://github.com/%s", s);
    } else {
        fprintf(stderr, "error: `%s` is not a repository: write owner/name, github.com/owner/name or a full git address\n", spec);
        exit(1);
    }
    d->name = "Dependency";
    lock_load(root);
    char *rev = NULL;
    char *dir = fetch_git(d, NULL, &rev);
    Manifest *dm = manifest_load(dir);
    d->name = dm->name;
    if (manifest_find_dep(root, d->name)) {
        fprintf(stderr, "note: `%s` was already a dependency; it is replaced\n", d->name);
    }
    lock_set(d->name, d->git, ref_of(d), rev);
    manifest_add_dep(root, d);
    lock_save(root);
    return d->name;
}

bool deps_remove(Manifest *root, const char *name) {
    if (!manifest_remove_dep(root, name)) return false;
    lock_load(root);
    Vec keep = {0};
    for (int i = 0; i < g_lock.n; i++) {
        LockEntry *e = g_lock.v[i];
        if (!e->name || strcmp(e->name, name) != 0) vec_push(&keep, e);
    }
    g_lock = keep;
    lock_save(root);
    return true;
}
