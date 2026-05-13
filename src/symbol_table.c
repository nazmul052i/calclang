#include "symbol_table.h"

static SymInfo make_info(SymbolKind kind, int slot, int depth, int inferred_type, int is_boxed) {
    SymInfo r;
    r.kind = kind;
    r.slot = slot;
    r.function_depth = depth;
    r.inferred_type = inferred_type;
    r.is_boxed = is_boxed;
    return r;
}

void symtab_init(SymbolTable *st) {
    st->count = 0;
    st->scope_count = 1;
    st->scope_starts[0]       = 0;
    st->scope_local_starts[0] = 0;
    st->next_global_slot      = 0;
    st->function_frame_depth  = 0;
}

void symtab_push_scope(SymbolTable *st) {
    if (st->scope_count >= CL_MAX_SCOPES) cl_die("scope nesting too deep");
    st->scope_starts[st->scope_count] = st->count;
    /* Snapshot the current frame's next_local_offset so popping the
       scope can restore it. For globals (depth 0) we record 0 — it's
       unused. */
    if (st->function_frame_depth > 0) {
        int d = st->function_frame_depth - 1;
        st->scope_local_starts[st->scope_count] = st->next_local_offset_stack[d];
    } else {
        st->scope_local_starts[st->scope_count] = 0;
    }
    st->scope_count++;
}

void symtab_pop_scope(SymbolTable *st) {
    if (st->scope_count <= 1) cl_die("scope underflow (popping global scope)");
    st->scope_count--;
    st->count = st->scope_starts[st->scope_count];
    if (st->function_frame_depth > 0) {
        int d = st->function_frame_depth - 1;
        st->next_local_offset_stack[d] = st->scope_local_starts[st->scope_count];
    }
}

void symtab_enter_function(SymbolTable *st) {
    if (st->function_frame_depth >= CL_MAX_SCOPES) cl_die("function nesting too deep");
    st->function_frame_depth++;
    int d = st->function_frame_depth - 1;
    st->next_local_offset_stack[d] = 0;
    st->max_local_offset_stack[d]  = 0;
}

int symtab_exit_function(SymbolTable *st) {
    if (st->function_frame_depth <= 0) cl_die("exit_function with no function frame");
    int d = st->function_frame_depth - 1;
    int max = st->max_local_offset_stack[d];
    st->function_frame_depth--;
    return max;
}

SymInfo symtab_declare(SymbolTable *st, const char *name, int inferred_type) {
    /* Same-scope hit becomes a reassignment. Widen the inferred type
       to TYPE_ANY if the new type disagrees with the recorded one. */
    int scope_start = st->scope_starts[st->scope_count - 1];
    for (int i = scope_start; i < st->count; i++) {
        if (strcmp(st->items[i].name, name) == 0) {
            Symbol *s = &st->items[i];
            if (s->inferred_type != inferred_type) s->inferred_type = 0;
            return make_info(s->kind, s->slot, s->function_depth, s->inferred_type, s->is_boxed);
        }
    }
    if (st->count >= CL_MAX_SYMBOLS) cl_die("too many live variables");

    Symbol *s = &st->items[st->count++];
    cl_strncpy_z(s->name, name, CL_MAX_TEXT);
    s->inferred_type   = inferred_type;
    s->function_depth  = st->function_frame_depth;
    s->is_boxed        = 0;

    if (st->function_frame_depth > 0) {
        int d = st->function_frame_depth - 1;
        if (st->next_local_offset_stack[d] >= CL_MAX_SYMBOLS) cl_die("too many locals in one function");
        s->kind = SYM_LOCAL;
        s->slot = st->next_local_offset_stack[d]++;
        if (st->next_local_offset_stack[d] > st->max_local_offset_stack[d]) {
            st->max_local_offset_stack[d] = st->next_local_offset_stack[d];
        }
    } else {
        if (st->next_global_slot >= CL_MAX_SYMBOLS) cl_die("too many global variables");
        s->kind = SYM_GLOBAL;
        s->slot = st->next_global_slot++;
    }
    return make_info(s->kind, s->slot, s->function_depth, s->inferred_type, s->is_boxed);
}

void symtab_set_inferred_type(SymbolTable *st, const char *name, int inferred_type) {
    for (int i = st->count - 1; i >= 0; i--) {
        if (strcmp(st->items[i].name, name) == 0) {
            if (st->items[i].inferred_type != inferred_type) st->items[i].inferred_type = 0;
            return;
        }
    }
}

void symtab_mark_boxed(SymbolTable *st, const char *name) {
    /* Find the innermost matching symbol and flag it as boxed. */
    for (int i = st->count - 1; i >= 0; i--) {
        if (strcmp(st->items[i].name, name) == 0) {
            st->items[i].is_boxed = 1;
            return;
        }
    }
}

SymInfo symtab_lookup(SymbolTable *st, const char *name) {
    for (int i = st->count - 1; i >= 0; i--) {
        if (strcmp(st->items[i].name, name) == 0) {
            Symbol *s = &st->items[i];
            return make_info(s->kind, s->slot, s->function_depth, s->inferred_type, s->is_boxed);
        }
    }
    return make_info(SYM_GLOBAL, -1, 0, 0, 0);
}

SymInfo symtab_require(SymbolTable *st, const char *name) {
    SymInfo r = symtab_lookup(st, name);
    if (r.slot < 0) {
        fprintf(stderr, "semantic error: variable '%s' used before declaration\n", name);
        exit(1);
    }
    return r;
}
