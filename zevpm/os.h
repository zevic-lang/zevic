/* os.h - files, directories and processes for zevpm (POSIX and Windows) */
#ifndef ZEVPM_OS_H
#define ZEVPM_OS_H

#include "../zevic0/util.h"

char *path_join(const char *a, const char *b);
const char *path_base(const char *path);       /* the last element */
char *path_dir(const char *path);              /* everything before the last element ("." if none) */

bool os_is_dir(const char *path);
bool os_is_file(const char *path);
bool os_mkdirs(const char *path);              /* mkdir -p */
bool os_write_file(const char *path, const char *text);
bool os_rmtree(const char *path);              /* rm -rf */
bool os_rename(const char *from, const char *to);

/* The files below `dir` (recursively) whose names end in one of `exts` (e.g. ".zev"), sorted. */
void os_walk(const char *dir, const char *const *exts, Vec *out);

char *os_exe_dir(void);                        /* the directory of the running program */
char *os_home_dir(void);                       /* the user's home directory, or NULL */
bool os_find_in_path(const char *name);        /* is `name` an executable on PATH? */

/* Run a program (argv, NULL-terminated vector of char*) and wait. Returns its exit code, or -1 when it
 * cannot be started. `dir` (may be NULL) is the working directory. */
int os_run(Vec *argv, const char *dir);
/* Like os_run, but the standard output is collected in *out (without the trailing newline). */
int os_run_capture(Vec *argv, const char *dir, char **out);

#endif
