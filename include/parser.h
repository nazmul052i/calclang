#ifndef CALCLANG_PARSER_H
#define CALCLANG_PARSER_H
#include "ast.h"

typedef struct {
    Lexer lexer;
    Token current;
    /* Directory of the source file, used to resolve `import "..."`
       paths. Empty string means imports resolve from the current
       working directory. */
    char  source_dir[512];
} Parser;

void parser_init(Parser *p, const char *src);
/* Variant that records the source file's containing directory so
   relative imports resolve from there. Call this instead of
   parser_init when you have a file path. */
void parser_init_with_path(Parser *p, const char *src, const char *file_path);
Program parser_parse_program(Parser *p);

#endif
