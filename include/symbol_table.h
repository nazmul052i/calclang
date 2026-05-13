#ifndef CALCLANG_SYMBOL_TABLE_H
#define CALCLANG_SYMBOL_TABLE_H
#include "common.h"

#define CL_MAX_SCOPES 64

typedef enum {
    SYM_GLOBAL = 0,
    SYM_LOCAL  = 1
} SymbolKind;

/* What codegen needs to know about a name. */
typedef struct {
    SymbolKind kind;
    int        slot;
    int        function_depth;   /* 0 = global, 1+ = inside some function */
    int        inferred_type;    /* TypeAnnot code; 0 = TYPE_ANY */
    int        is_boxed;         /* 1 if this local is stored as a 1-elem array (captured by some inner closure) */
} SymInfo;

typedef struct {
    char       name[CL_MAX_TEXT];
    SymbolKind kind;
    int        slot;
    int        function_depth;
    int        inferred_type;
    int        is_boxed;
} Symbol;

typedef struct {
    Symbol items[CL_MAX_SYMBOLS];
    int    count;
    int    scope_starts[CL_MAX_SCOPES];
    int    scope_local_starts[CL_MAX_SCOPES];
    int    scope_count;
    int    next_global_slot;
    /* Stack of function frame state, indexed by function_frame_depth.
       Depth 0 is top-level (uses next_global_slot instead). */
    int    function_frame_depth;
    int    next_local_offset_stack[CL_MAX_SCOPES];
    int    max_local_offset_stack[CL_MAX_SCOPES];
} SymbolTable;

void symtab_init(SymbolTable *st);
void symtab_push_scope(SymbolTable *st);
void symtab_pop_scope(SymbolTable *st);

/* Push a new function frame. Nested functions push frames on top of
   their enclosing function's frame; each frame has its own local slot
   space. */
void symtab_enter_function(SymbolTable *st);
int  symtab_exit_function(SymbolTable *st);  /* returns max_local_offset of the popped frame */

SymInfo symtab_declare(SymbolTable *st, const char *name, int inferred_type);
void    symtab_set_inferred_type(SymbolTable *st, const char *name, int inferred_type);
void    symtab_mark_boxed(SymbolTable *st, const char *name);
SymInfo symtab_lookup(SymbolTable *st, const char *name);
SymInfo symtab_require(SymbolTable *st, const char *name);

#endif
