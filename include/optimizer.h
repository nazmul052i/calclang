#ifndef CALCLANG_OPTIMIZER_H
#define CALCLANG_OPTIMIZER_H

#include "ast.h"

/* Run the AST-rewriting optimizer over an entire program. Applies
   constant folding, algebraic simplification, and dead-branch
   elimination in a single post-order walk. Each pass is conservative —
   nothing changes observable behaviour. Safe to call multiple times. */
void cl_optimize_program(Program *p);

#endif
