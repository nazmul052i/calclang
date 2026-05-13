#ifndef CALCLANG_CODEGEN_X64_H
#define CALCLANG_CODEGEN_X64_H
#include "ast.h"

/* Emit x86-64 assembly (Intel syntax, GAS-compatible) for `prog` into
   a newly-allocated NUL-terminated string. Caller owns the result
   (the tracked allocator will reclaim it at process exit anyway). */
char *codegen_x64_program(const Program *prog);

/* Same, but with library_mode = 1 the compilation unit emits only
   its function bodies — no `main` and no top-level code. Use this
   for multi-file builds where another unit owns the entry point. */
char *codegen_x64_program_ex(const Program *prog, int library_mode);

#endif
