/* parser.h - recursive descent parser */
#ifndef ZEVIC_PARSER_H
#define ZEVIC_PARSER_H

#include "ast.h"

/* Parse one source file. Errors are reported through diag_error; the result
 * is never NULL but may be incomplete when zv_error_count grew. */
SourceFile *parse_file(int file, const char *text, size_t len);

#endif
