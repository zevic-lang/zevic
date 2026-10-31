/* gen.h - type checking and LLVM IR generation (one pass per function instance) */
#ifndef ZEVIC_GEN_H
#define ZEVIC_GEN_H

#include "module.h"

typedef struct Gen Gen;

Gen *gen_new(Program *pg);
/* Check the whole program and write the LLVM IR into `out`.
 * Returns false when errors were reported. */
bool gen_program(Gen *g, SB *out);
/* The C header for the `export \"C\"` functions (call after gen_program succeeded). */
void gen_header(Gen *g, const char *guard, SB *out);

#endif
