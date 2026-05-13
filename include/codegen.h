#ifndef CALCLANG_CODEGEN_H
#define CALCLANG_CODEGEN_H
#include "parser.h"
#include "symbol_table.h"

char *codegen_program(Program *prog);

#endif
