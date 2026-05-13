#ifndef CALCLANG_PARSER_H
#define CALCLANG_PARSER_H
#include "ast.h"

typedef struct {
    Lexer lexer;
    Token current;
} Parser;

void parser_init(Parser *p, const char *src);
Program parser_parse_program(Parser *p);

#endif
