/* driver.h - finding the toolchain and running clang */
#ifndef ZEVIC_DRIVER_H
#define ZEVIC_DRIVER_H

#include "util.h"

typedef struct {
    const char *cc;      /* --cc */
    const char *cxx;     /* --cxx */
    const char *opt;     /* "-O1" */
    bool debug;          /* -g */
    Vec inputs;          /* extra files for the link: .c .cpp .o .a ... */
    Vec libs, libdirs, incdirs, defines;
    bool keep_ll;
    bool pic;            /* --pic: position independent code (for a shared library) */
} BuildOpts;

/* The directory that holds `packages/Zevic/` and `runtime/`, or NULL. */
const char *driver_home_report(void);
char *driver_find_home(const char *argv0);
/* A usable clang (version 15 or newer), or NULL after printing install instructions. */
char *driver_find_clang(const char *cli_cc);
/* Compile `ll_path` together with the runtime and extra inputs into `out_path`. Returns clang's exit code. */
int driver_build(const char *clang, const char *home, const char *ll_path, const char *out_path, BuildOpts *o);
/* Compile `ll_path`, the extra C/C++ inputs, the runtime and the collector into one static library. */
int driver_build_lib(const char *clang, const char *home, const char *ll_path, const char *out_path, BuildOpts *o);
/* Remove a file (best effort). */
void driver_remove(const char *path);

#endif
