/* gen.h - type checking and LLVM IR generation (one pass per function instance) */
#ifndef ZEVIC_GEN_H
#define ZEVIC_GEN_H

#include "module.h"

typedef struct Gen Gen;

Gen *gen_new(Program *pg);
/* Check the whole program and write the LLVM IR into `out`.
 * Returns false when errors were reported. */
bool gen_program(Gen *g, SB *out);
/* Build the test runner (every `test` block) instead of the program's `Main`. */
void gen_set_test_mode(Gen *g);
/* Build a library: there is no `Main`, and the exports are the interface. */
void gen_set_lib_mode(Gen *g);
/* The C header for the `export \"C\"` functions (call after gen_program succeeded). */
void gen_header(Gen *g, const char *guard, SB *out);

/* `--dump-types`: instantiate every non-generic struct, class and enum and print them (tests the Zevic port). */
void gen_dump_types(Gen *g);

#endif
