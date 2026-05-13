#include "codegen.h"
#include "calclib.h"

/* User-defined top-level functions in the current program. Populated
   once at the start of codegen_program so a call to an undefined name
   fails here instead of at link time, so call sites know whether to
   use the global `fn_<name>` label or the file-local `.fn_<name>`,
   and so we can validate arg counts and argument types at call sites. */
#define CG_MAX_FUNCTIONS 256
typedef struct {
    char      name[CL_MAX_TEXT];
    int       is_public;
    int       param_count;
    TypeAnnot param_types[CL_MAX_PARAMS];
    TypeAnnot return_type;
} CgUserFn;

static CgUserFn g_user_fns[CG_MAX_FUNCTIONS];
static int      g_user_fn_count = 0;

static const CgUserFn *find_user_function(const char *name) {
    for (int i = 0; i < g_user_fn_count; i++) {
        if (strcmp(g_user_fns[i].name, name) == 0) return &g_user_fns[i];
    }
    return NULL;
}

/* Prefix used for a function's entry label and for every CALL targeting
   it: `fn_` for public, `.fn_` for private (file-local). */
static const char *fn_label_prefix(int is_public) {
    return is_public ? "fn_" : ".fn_";
}

/* When a calclib builtin is referenced as a *value* (a bare name, no
   parens — e.g. `let f = sqrt;` or `map(arr, sqrt)`), the codegen
   records a request to emit a tiny trampoline function for it. The
   trampoline has the standard CALL/RET convention but its body is
   just LOAD_LOCAL the params + BUILTIN <id> + RET. This lets every
   higher-order call site treat builtins uniformly with user functions. */
typedef struct {
    char name[CL_MAX_TEXT];
    int  builtin_id;
    int  arg_count;
} CgTrampoline;

static CgTrampoline g_trampolines[CG_MAX_FUNCTIONS];
static int          g_trampoline_count = 0;

static void need_builtin_trampoline(const BuiltinDef *bi) {
    for (int i = 0; i < g_trampoline_count; i++) {
        if (g_trampolines[i].builtin_id == bi->id) return;
    }
    if (g_trampoline_count >= CG_MAX_FUNCTIONS) cl_die("too many builtin trampolines");
    cl_strncpy_z(g_trampolines[g_trampoline_count].name, bi->name, CL_MAX_TEXT);
    g_trampolines[g_trampoline_count].builtin_id = bi->id;
    g_trampolines[g_trampoline_count].arg_count  = bi->arg_count;
    g_trampoline_count++;
}

/* Does `expected` accept a value of `actual`? `any` accepts anything;
   `bool` is an alias for `num`; everything else is exact. */
static int type_accepts(TypeAnnot expected, TypeAnnot actual) {
    if (expected == TYPE_ANY)  return 1;
    if (expected == TYPE_BOOL) return actual == TYPE_NUM;
    return expected == actual;
}

/* Best-effort static type of an expression node. Returns 1 and writes
   `*out` when the type is unambiguous at compile time; returns 0 when
   the answer needs runtime info and the call site should defer to the
   function-entry TYPECHECK. */
static int static_arg_type(SymbolTable *st, AST *arg, TypeAnnot *out) {
    switch (arg->kind) {
        case NODE_NUMBER:    *out = TYPE_NUM; return 1;
        case NODE_STRING:    *out = TYPE_STR; return 1;
        case NODE_ARRAY_LIT: *out = TYPE_ARR; return 1;
        case NODE_MAP_LIT:   *out = TYPE_MAP; return 1;
        case NODE_UNOP:      *out = TYPE_NUM; return 1;
        case NODE_BINOP: {
            /* Everything except `+` is unambiguously numeric. `+` can
               also concat strings, so we leave it for runtime. */
            TokenType op = arg->as.binop.op;
            if (op == TOK_PLUS) return 0;
            *out = TYPE_NUM;
            return 1;
        }
        case NODE_VAR: {
            /* Prefer the per-use type recorded by the flow-sensitive
               inference pass: it knows the type at THIS program point
               even when other uses see a different type. Falls back to
               the symbol table's coarser "last-write" inference when
               the per-use type is unknown. */
            if (arg->inferred_type != TYPE_ANY) {
                *out = (TypeAnnot)arg->inferred_type;
                return 1;
            }
            SymInfo s = symtab_lookup(st, arg->as.var);
            if (s.slot >= 0 && s.inferred_type != TYPE_ANY) {
                *out = (TypeAnnot)s.inferred_type;
                return 1;
            }
            /* Bare function-name reference produces a fn value. */
            const CgUserFn *uf = find_user_function(arg->as.var);
            if (uf) { *out = TYPE_FN; return 1; }
            const BuiltinDef *bi = cl_find_builtin(arg->as.var);
            if (bi) { *out = TYPE_FN; return 1; }
            return 0;
        }
        case NODE_CALL: {
            /* Use the callee's declared (or table-recorded) return type.
               Indirect calls have no name to look up — return unknown. */
            if (arg->as.call.callee) return 0;
            const BuiltinDef *bi = cl_find_builtin(arg->as.call.name);
            if (bi && bi->return_type != 0) {
                *out = (TypeAnnot)bi->return_type;
                return 1;
            }
            const CgUserFn *uf = find_user_function(arg->as.call.name);
            if (uf && uf->return_type != TYPE_ANY) {
                *out = uf->return_type;
                return 1;
            }
            return 0;
        }
        default:
            return 0;
    }
}

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *s) {
    s->cap = 4096;
    s->len = 0;
    s->buf = (char *)cl_track_malloc(s->cap);
    s->buf[0] = '\0';
}

static void sb_reserve(StrBuf *s, size_t extra) {
    if (s->len + extra + 1 <= s->cap) return;
    while (s->len + extra + 1 > s->cap) s->cap *= 2;
    s->buf = (char *)cl_track_realloc(s->buf, s->cap);
}

static void sb_add(StrBuf *s, const char *txt) {
    size_t n = strlen(txt);
    sb_reserve(s, n);
    memcpy(s->buf + s->len, txt, n + 1);
    s->len += n;
}

static void gen_inline_closure(AST *fn_node, SymbolTable *st, StrBuf *parent_out);

static void sb_printf(StrBuf *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) {
        va_end(ap2);
        cl_die("vsnprintf failed in codegen");
    }
    sb_reserve(s, (size_t)needed);
    vsnprintf(s->buf + s->len, (size_t)needed + 1, fmt, ap2);
    va_end(ap2);
    s->len += (size_t)needed;
}

/* `.L<n>` labels are scoped to the object file by the assembler — they
   never appear in the global symbol table, so two compilation units can
   coexist at link time without colliding. The counter is monotonic
   across the entire compilation; it does not reset per function. */
static int g_label_counter = 0;

static void make_label(char *buf, size_t buf_size) {
    snprintf(buf, buf_size, ".L%d", g_label_counter++);
}

typedef struct {
    char continue_label[32];
    char break_label[32];
} LoopCtx;

#define CG_MAX_LOOP_DEPTH 64
static LoopCtx g_loops[CG_MAX_LOOP_DEPTH];
static int     g_loop_depth = 0;

static void push_loop(const char *cont_lbl, const char *brk_lbl) {
    if (g_loop_depth >= CG_MAX_LOOP_DEPTH) cl_die("loop nesting too deep");
    cl_strncpy_z(g_loops[g_loop_depth].continue_label, cont_lbl, sizeof(g_loops[g_loop_depth].continue_label));
    cl_strncpy_z(g_loops[g_loop_depth].break_label,    brk_lbl,  sizeof(g_loops[g_loop_depth].break_label));
    g_loop_depth++;
}

static void pop_loop(void) { g_loop_depth--; }

/* Compile-time string pool — keyed by content, indexed into by PUSH_STR. */
typedef struct {
    char *items[CL_MAX_STRINGS];
    int   count;
} StringPool;

static StringPool g_strings;

static int strpool_intern(const char *s) {
    for (int i = 0; i < g_strings.count; i++) {
        if (strcmp(g_strings.items[i], s) == 0) return i;
    }
    if (g_strings.count >= CL_MAX_STRINGS) cl_die("too many string literals");
    g_strings.items[g_strings.count] = cl_track_strdup(s);
    return g_strings.count++;
}

static void sb_append_quoted(StrBuf *sb, const char *s) {
    sb_add(sb, "\"");
    char esc[3] = { '\\', 0, 0 };
    char one[2] = { 0, 0 };
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '\n': sb_add(sb, "\\n");  break;
            case '\t': sb_add(sb, "\\t");  break;
            case '\r': sb_add(sb, "\\r");  break;
            case '\\': esc[1] = '\\'; sb_add(sb, esc); break;
            case '"':  esc[1] = '"';  sb_add(sb, esc); break;
            default:   one[0] = *p; sb_add(sb, one);
        }
    }
    sb_add(sb, "\"");
}

/* Returns 1 if `name` appears as a NODE_VAR or as the target of an
   assignment anywhere in the subtree. Walks into inner NODE_FNs too
   (which is exactly what we want when asking "does some inner closure
   reference this name"). Conservative — does not honour shadowing,
   so a closure that locally rebinds the name still counts as a use.
   The cost is one unnecessary box per false positive. */
static int contains_var_use(AST *n, const char *name) {
    if (!n) return 0;
    switch (n->kind) {
        case NODE_VAR:    return strcmp(n->as.var, name) == 0;
        case NODE_ASSIGN:
            return strcmp(n->as.assign_stmt.name, name) == 0
                || contains_var_use(n->as.assign_stmt.expr, name);
        case NODE_BINOP:
            return contains_var_use(n->as.binop.left,  name)
                || contains_var_use(n->as.binop.right, name);
        case NODE_UNOP:   return contains_var_use(n->as.unop.operand, name);
        case NODE_LET:    return contains_var_use(n->as.let_stmt.expr, name);
        case NODE_PRINT:  return contains_var_use(n->as.print_stmt.expr, name);
        case NODE_IF:
            return contains_var_use(n->as.if_stmt.cond, name)
                || contains_var_use(n->as.if_stmt.then_branch, name)
                || contains_var_use(n->as.if_stmt.else_branch, name);
        case NODE_WHILE:
            return contains_var_use(n->as.while_stmt.cond, name)
                || contains_var_use(n->as.while_stmt.body, name);
        case NODE_FOR:
            return contains_var_use(n->as.for_stmt.init, name)
                || contains_var_use(n->as.for_stmt.cond, name)
                || contains_var_use(n->as.for_stmt.step, name)
                || contains_var_use(n->as.for_stmt.body, name);
        case NODE_BLOCK: {
            for (int i = 0; i < n->as.block.count; i++)
                if (contains_var_use(n->as.block.stmts[i], name)) return 1;
            return 0;
        }
        case NODE_FN:     return contains_var_use(n->as.fn_def.body, name);
        case NODE_CALL: {
            if (n->as.call.callee
                && contains_var_use(n->as.call.callee, name)) return 1;
            for (int i = 0; i < n->as.call.arg_count; i++)
                if (contains_var_use(n->as.call.args[i], name)) return 1;
            return 0;
        }
        case NODE_RETURN: return contains_var_use(n->as.return_stmt.expr, name);
        case NODE_ARRAY_LIT: {
            for (int i = 0; i < n->as.array_lit.count; i++)
                if (contains_var_use(n->as.array_lit.items[i], name)) return 1;
            return 0;
        }
        case NODE_INDEX:
            return contains_var_use(n->as.index.target, name)
                || contains_var_use(n->as.index.index,  name);
        case NODE_INDEX_ASSIGN:
            return contains_var_use(n->as.index_assign.target, name)
                || contains_var_use(n->as.index_assign.index,  name)
                || contains_var_use(n->as.index_assign.value,  name);
        case NODE_INDEX_OPASSIGN:
            return contains_var_use(n->as.index_opassign.target, name)
                || contains_var_use(n->as.index_opassign.index,  name)
                || contains_var_use(n->as.index_opassign.value,  name);
        case NODE_MAP_LIT: {
            for (int i = 0; i < n->as.map_lit.count; i++) {
                if (contains_var_use(n->as.map_lit.keys[i],   name)) return 1;
                if (contains_var_use(n->as.map_lit.values[i], name)) return 1;
            }
            return 0;
        }
        default: return 0;
    }
}

/* Walks the subtree looking only at INNER closures (NODE_FN nodes).
   Returns 1 if any one of them references `name`. Used by codegen to
   decide whether a local needs to be boxed. */
static int captured_in_inner_fn(AST *n, const char *name) {
    if (!n) return 0;
    switch (n->kind) {
        case NODE_FN:
            return contains_var_use(n->as.fn_def.body, name);
        case NODE_BLOCK: {
            for (int i = 0; i < n->as.block.count; i++)
                if (captured_in_inner_fn(n->as.block.stmts[i], name)) return 1;
            return 0;
        }
        case NODE_IF:
            return captured_in_inner_fn(n->as.if_stmt.cond, name)
                || captured_in_inner_fn(n->as.if_stmt.then_branch, name)
                || captured_in_inner_fn(n->as.if_stmt.else_branch, name);
        case NODE_WHILE:
            return captured_in_inner_fn(n->as.while_stmt.cond, name)
                || captured_in_inner_fn(n->as.while_stmt.body, name);
        case NODE_FOR:
            return captured_in_inner_fn(n->as.for_stmt.init, name)
                || captured_in_inner_fn(n->as.for_stmt.cond, name)
                || captured_in_inner_fn(n->as.for_stmt.step, name)
                || captured_in_inner_fn(n->as.for_stmt.body, name);
        case NODE_LET:    return captured_in_inner_fn(n->as.let_stmt.expr, name);
        case NODE_ASSIGN: return captured_in_inner_fn(n->as.assign_stmt.expr, name);
        case NODE_PRINT:  return captured_in_inner_fn(n->as.print_stmt.expr, name);
        case NODE_RETURN: return captured_in_inner_fn(n->as.return_stmt.expr, name);
        case NODE_BINOP:
            return captured_in_inner_fn(n->as.binop.left, name)
                || captured_in_inner_fn(n->as.binop.right, name);
        case NODE_UNOP:   return captured_in_inner_fn(n->as.unop.operand, name);
        case NODE_CALL: {
            if (n->as.call.callee
                && captured_in_inner_fn(n->as.call.callee, name)) return 1;
            for (int i = 0; i < n->as.call.arg_count; i++)
                if (captured_in_inner_fn(n->as.call.args[i], name)) return 1;
            return 0;
        }
        case NODE_ARRAY_LIT: {
            for (int i = 0; i < n->as.array_lit.count; i++)
                if (captured_in_inner_fn(n->as.array_lit.items[i], name)) return 1;
            return 0;
        }
        case NODE_INDEX:
            return captured_in_inner_fn(n->as.index.target, name)
                || captured_in_inner_fn(n->as.index.index,  name);
        case NODE_INDEX_ASSIGN:
            return captured_in_inner_fn(n->as.index_assign.target, name)
                || captured_in_inner_fn(n->as.index_assign.index,  name)
                || captured_in_inner_fn(n->as.index_assign.value,  name);
        case NODE_INDEX_OPASSIGN:
            return captured_in_inner_fn(n->as.index_opassign.target, name)
                || captured_in_inner_fn(n->as.index_opassign.index,  name)
                || captured_in_inner_fn(n->as.index_opassign.value,  name);
        case NODE_MAP_LIT: {
            for (int i = 0; i < n->as.map_lit.count; i++) {
                if (captured_in_inner_fn(n->as.map_lit.keys[i],   name)) return 1;
                if (captured_in_inner_fn(n->as.map_lit.values[i], name)) return 1;
            }
            return 0;
        }
        default: return 0;
    }
}

/* Upvalue context per function depth. g_upval_stack[d-1] is the
   context for the function currently being emitted at depth d. Depth
   0 (top-level main code) has no upvalues. */
#define CG_MAX_UPVALS 16

typedef enum {
    CG_UPVAL_FROM_LOCAL = 0,   /* source is a local of the parent function */
    CG_UPVAL_FROM_UPVAL = 1    /* source is the parent function's upval[index] (transitive) */
} CgUpvalSource;

typedef struct {
    char          name[CL_MAX_TEXT];
    CgUpvalSource source_kind;
    int           source_index;   /* parent-side local slot OR parent-side upval index */
} CgUpvalEntry;

typedef struct {
    CgUpvalEntry items[CG_MAX_UPVALS];
    int count;
    int active;                   /* 1 while we are inside this function's body */
} CgUpvalCtx;

static CgUpvalCtx g_upval_stack[CL_MAX_SCOPES];   /* index by depth - 1 */

/* The body AST of the function currently being emitted (top-level fn
   or inline closure). Used by NODE_LET to decide whether the new
   local needs boxing — i.e. whether any inner closure references it. */
static AST *g_current_fn_body = NULL;

static int upvals_find(CgUpvalCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->items[i].name, name) == 0) return i;
    }
    return -1;
}

static int upvals_register(CgUpvalCtx *ctx, const char *name,
                           CgUpvalSource src_kind, int src_index) {
    int existing = upvals_find(ctx, name);
    if (existing >= 0) return existing;
    if (ctx->count >= CG_MAX_UPVALS) cl_die("too many captured variables in one closure");
    cl_strncpy_z(ctx->items[ctx->count].name, name, CL_MAX_TEXT);
    ctx->items[ctx->count].source_kind  = src_kind;
    ctx->items[ctx->count].source_index = src_index;
    return ctx->count++;
}

/* Resolve a reference to `name` that's at depth `sym_depth` from
   inside a function at depth `current_depth`. Registers the name as
   an upvalue at every level from `sym_depth + 1` up to `current_depth`,
   chaining sources so each level reads from its parent. Returns the
   upvalue index at the current level. */
static int resolve_upvalue(const char *name, int sym_depth, int sym_slot, int current_depth) {
    int last_idx = -1;
    for (int d = sym_depth + 1; d <= current_depth; d++) {
        CgUpvalCtx *ctx = &g_upval_stack[d - 1];
        if (d == sym_depth + 1) {
            last_idx = upvals_register(ctx, name, CG_UPVAL_FROM_LOCAL, sym_slot);
        } else {
            last_idx = upvals_register(ctx, name, CG_UPVAL_FROM_UPVAL, last_idx);
        }
    }
    return last_idx;
}

/* Load a variable's value onto the stack. Handles:
     - globals (LOAD <slot>)
     - non-boxed locals of the current fn (LOAD_LOCAL <slot>)
     - boxed locals of the current fn (LOAD_LOCAL + PUSH 0 + INDEX_GET)
     - upvalues captured from an enclosing fn — possibly transitive.
       Every captured local is stored as a 1-element array (a "box")
       so the closure can read AND write through it; LOAD_UPVAL gives
       the box reference and INDEX_GET pulls the value out. */
static void emit_load(SymbolTable *st, StrBuf *out, SymInfo sym, const char *name) {
    if (sym.function_depth == 0) {
        sb_printf(out, "LOAD %d ; %s\n", sym.slot, name);
        return;
    }
    if (sym.function_depth == st->function_frame_depth) {
        if (sym.is_boxed) {
            sb_printf(out, "LOAD_LOCAL %d ; %s (box)\n", sym.slot, name);
            sb_add(out, "PUSH 0\n");
            sb_add(out, "INDEX_GET\n");
        } else {
            sb_printf(out, "LOAD_LOCAL %d ; %s\n", sym.slot, name);
        }
        return;
    }
    /* sym.function_depth < current — captured (possibly transitively). */
    int idx = resolve_upvalue(name, sym.function_depth, sym.slot, st->function_frame_depth);
    sb_printf(out, "LOAD_UPVAL %d ; %s (captured)\n", idx, name);
    sb_add(out, "PUSH 0\n");
    sb_add(out, "INDEX_GET\n");
}

/* Store the top-of-stack value into a variable. For boxed targets the
   target's box reference plus index 0 must be pushed BEFORE the
   right-hand side. This helper assumes the caller has not yet pushed
   the RHS; for non-boxed paths it pops the existing top via STORE. */
static void emit_store(SymbolTable *st, StrBuf *out, SymInfo sym, const char *name) {
    if (sym.function_depth == 0) {
        sb_printf(out, "STORE %d ; %s\n", sym.slot, name);
        return;
    }
    if (sym.function_depth == st->function_frame_depth) {
        if (sym.is_boxed) {
            /* This path is only used when the value is already on the
               stack but the helper is doing the writes itself. For boxed
               we need the box ref + index BELOW the value, so callers
               that need to store into a boxed slot use emit_store_boxed
               (see below) which arranges the stack correctly. */
            cl_die("internal: emit_store on boxed local must use emit_store_boxed");
        }
        sb_printf(out, "STORE_LOCAL %d ; %s\n", sym.slot, name);
        return;
    }
    /* Boxed upvalue write: same shape mismatch — use emit_store_via_*. */
    cl_die("internal: emit_store on upvalue must use the boxed path");
}

/* Emit the full assignment pattern for a boxed variable: target the
   box, push the index 0, gen the RHS, then INDEX_SET. The RHS is
   generated by the caller via the `gen_rhs` callback so we control
   the stack layout. */
typedef void (*GenExprFn)(AST *n, SymbolTable *st, StrBuf *out);
static void gen_expr(AST *n, SymbolTable *st, StrBuf *out);

static void emit_store_boxed(SymbolTable *st, StrBuf *out, SymInfo sym, const char *name, AST *rhs) {
    if (sym.function_depth == st->function_frame_depth) {
        sb_printf(out, "LOAD_LOCAL %d ; %s (box)\n", sym.slot, name);
    } else {
        int idx = resolve_upvalue(name, sym.function_depth, sym.slot, st->function_frame_depth);
        sb_printf(out, "LOAD_UPVAL %d ; %s (captured box)\n", idx, name);
    }
    sb_add(out, "PUSH 0\n");
    gen_expr(rhs, st, out);
    sb_add(out, "INDEX_SET\n");
}

static void gen_stmt(AST *n, SymbolTable *st, StrBuf *out);

static void gen_expr(AST *n, SymbolTable *st, StrBuf *out) {
    switch (n->kind) {
        case NODE_NUMBER:
            sb_printf(out, "PUSH %0.17g\n", n->as.number);
            break;
        case NODE_STRING: {
            int idx = strpool_intern(n->as.string);
            sb_printf(out, "PUSH_STR %d\n", idx);
            break;
        }
        case NODE_VAR: {
            /* Resolution order: local/global scope → user function →
               calclib builtin (via trampoline). Variables shadow
               function names; functions shadow builtins of the same
               name (which is rejected during the pre-scan anyway). */
            SymInfo s = symtab_lookup(st, n->as.var);
            if (s.slot >= 0) {
                emit_load(st, out, s, n->as.var);
                break;
            }
            const CgUserFn *uf = find_user_function(n->as.var);
            if (uf) {
                sb_printf(out, "PUSH_FN %s%s\n",
                    fn_label_prefix(uf->is_public), n->as.var);
                break;
            }
            const BuiltinDef *bi = cl_find_builtin(n->as.var);
            if (bi) {
                need_builtin_trampoline(bi);
                sb_printf(out, "PUSH_FN .bi_%s\n", bi->name);
                break;
            }
            fprintf(stderr, "semantic error: variable '%s' used before declaration\n", n->as.var);
            exit(1);
        }
        case NODE_UNOP:
            gen_expr(n->as.unop.operand, st, out);
            switch (n->as.unop.op) {
                case TOK_MINUS: sb_add(out, "NEG\n"); break;
                case TOK_BANG:  sb_add(out, "NOT\n"); break;
                default:        cl_die("unknown unary op");
            }
            break;
        case NODE_BINOP:
            gen_expr(n->as.binop.left,  st, out);
            gen_expr(n->as.binop.right, st, out);
            switch (n->as.binop.op) {
                case TOK_PLUS:    sb_add(out, "ADD\n"); break;
                case TOK_MINUS:   sb_add(out, "SUB\n"); break;
                case TOK_STAR:    sb_add(out, "MUL\n"); break;
                case TOK_SLASH:   sb_add(out, "DIV\n"); break;
                case TOK_PERCENT: sb_add(out, "MOD\n"); break;
                case TOK_LT:      sb_add(out, "LT\n");  break;
                case TOK_LE:      sb_add(out, "LE\n");  break;
                case TOK_GT:      sb_add(out, "GT\n");  break;
                case TOK_GE:      sb_add(out, "GE\n");  break;
                case TOK_EQEQ:    sb_add(out, "EQ\n");  break;
                case TOK_NEQ:     sb_add(out, "NEQ\n"); break;
                case TOK_AND:     sb_add(out, "AND\n"); break;
                case TOK_OR:      sb_add(out, "OR\n");  break;
                default:          cl_die("unknown binary op");
            }
            break;
        case NODE_CALL: {
            if (n->as.call.callee) {
                /* Indirect call: `(expr)(args)`, `obj.method(args)`,
                   `arr[i](args)`. No static arity or type check is
                   possible — those become runtime checks. */
                for (int i = 0; i < n->as.call.arg_count; i++) {
                    gen_expr(n->as.call.args[i], st, out);
                }
                gen_expr(n->as.call.callee, st, out);
                sb_printf(out, "CALL_VAL %d\n", n->as.call.arg_count);
                break;
            }
            const BuiltinDef *bi = cl_find_builtin(n->as.call.name);
            if (bi) {
                if (n->as.call.arg_count != bi->arg_count) {
                    fprintf(stderr,
                        "semantic error: calclib '%s' takes %d arg%s, got %d\n",
                        bi->name, bi->arg_count, bi->arg_count == 1 ? "" : "s",
                        n->as.call.arg_count);
                    exit(1);
                }
                for (int i = 0; i < n->as.call.arg_count; i++) {
                    gen_expr(n->as.call.args[i], st, out);
                }
                sb_printf(out, "BUILTIN %d ; %s\n", bi->id, bi->name);
            } else if (find_user_function(n->as.call.name)) {
                const CgUserFn *uf = find_user_function(n->as.call.name);
                /* Arg-count check. Runtime would also catch a stack
                   underflow, but a clear compile-time error is friendlier. */
                if (uf->param_count != n->as.call.arg_count) {
                    fprintf(stderr,
                        "semantic error: function '%s' takes %d arg%s, got %d\n",
                        uf->name, uf->param_count,
                        uf->param_count == 1 ? "" : "s",
                        n->as.call.arg_count);
                    exit(1);
                }
                /* Per-arg type check for arguments whose type is
                   knowable statically (literals, unary ops, most
                   binops). Anything ambiguous is left for the
                   function's entry-time TYPECHECK opcode. */
                for (int i = 0; i < n->as.call.arg_count; i++) {
                    TypeAnnot expected = uf->param_types[i];
                    TypeAnnot actual;
                    if (expected != TYPE_ANY
                        && static_arg_type(st, n->as.call.args[i], &actual)
                        && !type_accepts(expected, actual)) {
                        fprintf(stderr,
                            "semantic error: argument %d to '%s': expected %s, got %s\n",
                            i + 1, uf->name,
                            type_annot_name(expected),
                            type_annot_name(actual));
                        exit(1);
                    }
                    gen_expr(n->as.call.args[i], st, out);
                }
                /* Direct call. CALL carries the label and the arg
                   count so the VM can set fp = sp - n_args. */
                sb_printf(out, "CALL %s%s %d\n",
                    fn_label_prefix(uf->is_public),
                    n->as.call.name, n->as.call.arg_count);
            } else {
                /* Last shot: maybe a variable holding a function
                   value. Emit an indirect call. */
                SymInfo s = symtab_lookup(st, n->as.call.name);
                if (s.slot < 0) {
                    fprintf(stderr,
                        "semantic error: undefined function '%s'\n",
                        n->as.call.name);
                    exit(1);
                }
                for (int i = 0; i < n->as.call.arg_count; i++) {
                    gen_expr(n->as.call.args[i], st, out);
                }
                emit_load(st, out, s, n->as.call.name);
                sb_printf(out, "CALL_VAL %d\n", n->as.call.arg_count);
            }
            break;
        }
        case NODE_ARRAY_LIT:
            for (int i = 0; i < n->as.array_lit.count; i++) {
                gen_expr(n->as.array_lit.items[i], st, out);
            }
            sb_printf(out, "NEW_ARRAY %d\n", n->as.array_lit.count);
            break;
        case NODE_MAP_LIT:
            /* Push every (key, value) pair so NEW_MAP can pop 2*count
               values in alternating order. */
            for (int i = 0; i < n->as.map_lit.count; i++) {
                gen_expr(n->as.map_lit.keys[i],   st, out);
                gen_expr(n->as.map_lit.values[i], st, out);
            }
            sb_printf(out, "NEW_MAP %d\n", n->as.map_lit.count);
            break;
        case NODE_INDEX:
            gen_expr(n->as.index.target, st, out);
            gen_expr(n->as.index.index,  st, out);
            sb_add(out, "INDEX_GET\n");
            break;
        case NODE_FN:
            /* Anonymous closure expression or nested named fn (which
               the parser desugars to a let with this on the RHS). */
            gen_inline_closure(n, st, out);
            break;
        default:
            cl_die("invalid expression node");
    }
}

static void gen_stmt(AST *n, SymbolTable *st, StrBuf *out) {
    switch (n->kind) {
        case NODE_LET: {
            /* Reject top-level `let X = ...` if X is also a user
               function. Without this check the global silently shadows
               the function for every later body lookup, which usually
               wasn't what the user meant. Local lets inside a function
               are fine — they're scoped and disappear on return. */
            if (st->function_frame_depth == 0
                && find_user_function(n->as.let_stmt.name)) {
                fprintf(stderr,
                    "semantic error: top-level 'let %s' conflicts with a function of the same name\n",
                    n->as.let_stmt.name);
                exit(1);
            }
            TypeAnnot annot = n->as.let_stmt.type;
            /* Compile-time literal-type check for `let x: T = literal`.
               Dynamic RHS is left for the TYPECHECK_TOP runtime check. */
            TypeAnnot rhs_type = TYPE_ANY;
            int rhs_known = static_arg_type(st, n->as.let_stmt.expr, &rhs_type);
            if (annot != TYPE_ANY && rhs_known
                && !type_accepts(annot, rhs_type)) {
                fprintf(stderr,
                    "semantic error: let %s: %s = ... initializer has type %s\n",
                    n->as.let_stmt.name,
                    type_annot_name(annot),
                    type_annot_name(rhs_type));
                exit(1);
            }
            gen_expr(n->as.let_stmt.expr, st, out);
            /* Runtime check just before STORE. Works for globals and
               locals uniformly because TYPECHECK_TOP inspects sp-1. */
            if (annot != TYPE_ANY) {
                sb_printf(out, "TYPECHECK_TOP %d  ; %s: %s\n",
                    (int)annot, n->as.let_stmt.name, type_annot_name(annot));
            }
            /* Type-inference rule: an explicit annotation is the most
               authoritative source. Otherwise we adopt the RHS's
               statically-known type (if any). Unknown stays TYPE_ANY. */
            int recorded_type = annot != TYPE_ANY
                ? (int)annot
                : (rhs_known ? (int)rhs_type : 0);
            SymInfo sym = symtab_declare(st, n->as.let_stmt.name, recorded_type);
            /* If we're inside a function and some inner closure
               references this local, box it at declaration: wrap the
               value just pushed in a 1-element array before storing. */
            if (sym.function_depth > 0 && g_current_fn_body
                && captured_in_inner_fn(g_current_fn_body, n->as.let_stmt.name)) {
                sb_add(out, "NEW_ARRAY 1\n");
                symtab_mark_boxed(st, n->as.let_stmt.name);
                /* emit_store goes through the regular path — the slot
                   now holds the box reference, which is what we want. */
                sb_printf(out, "STORE_LOCAL %d ; %s (box)\n", sym.slot, n->as.let_stmt.name);
            } else {
                emit_store(st, out, sym, n->as.let_stmt.name);
            }
            break;
        }
        case NODE_ASSIGN: {
            /* Track the assigned type. */
            TypeAnnot rhs_type;
            int rhs_known = static_arg_type(st, n->as.assign_stmt.expr, &rhs_type);
            SymInfo sym = symtab_require(st, n->as.assign_stmt.name);
            if (rhs_known) {
                symtab_set_inferred_type(st, n->as.assign_stmt.name, (int)rhs_type);
            } else {
                symtab_set_inferred_type(st, n->as.assign_stmt.name, 0);
            }
            /* Two boxed paths: local boxed slot OR captured upvalue.
               Both flow through emit_store_boxed which sets up the
               (box, index, value) ordering INDEX_SET wants. The
               unboxed path is the original gen_expr + STORE pattern. */
            if (sym.function_depth > 0
                && sym.function_depth != st->function_frame_depth) {
                /* upvalue write — always boxed */
                emit_store_boxed(st, out, sym, n->as.assign_stmt.name,
                                 n->as.assign_stmt.expr);
            } else if (sym.is_boxed) {
                emit_store_boxed(st, out, sym, n->as.assign_stmt.name,
                                 n->as.assign_stmt.expr);
            } else {
                gen_expr(n->as.assign_stmt.expr, st, out);
                emit_store(st, out, sym, n->as.assign_stmt.name);
            }
            break;
        }
        case NODE_PRINT:
            gen_expr(n->as.print_stmt.expr, st, out);
            sb_add(out, "PRINT\n");
            break;
        case NODE_IF: {
            char lelse[32], lend[32];
            make_label(lelse, sizeof(lelse));
            make_label(lend,  sizeof(lend));
            gen_expr(n->as.if_stmt.cond, st, out);
            if (n->as.if_stmt.else_branch) {
                sb_printf(out, "JZ %s\n", lelse);
                gen_stmt(n->as.if_stmt.then_branch, st, out);
                sb_printf(out, "JMP %s\n", lend);
                sb_printf(out, "%s:\n", lelse);
                gen_stmt(n->as.if_stmt.else_branch, st, out);
                sb_printf(out, "%s:\n", lend);
            } else {
                sb_printf(out, "JZ %s\n", lend);
                gen_stmt(n->as.if_stmt.then_branch, st, out);
                sb_printf(out, "%s:\n", lend);
            }
            break;
        }
        case NODE_WHILE: {
            char lstart[32], lend[32];
            make_label(lstart, sizeof(lstart));
            make_label(lend,   sizeof(lend));
            sb_printf(out, "%s:\n", lstart);
            gen_expr(n->as.while_stmt.cond, st, out);
            sb_printf(out, "JZ %s\n", lend);
            push_loop(lstart, lend);
            gen_stmt(n->as.while_stmt.body, st, out);
            pop_loop();
            sb_printf(out, "JMP %s\n", lstart);
            sb_printf(out, "%s:\n", lend);
            break;
        }
        case NODE_FOR: {
            char lstart[32], lstep[32], lend[32];
            make_label(lstart, sizeof(lstart));
            make_label(lstep,  sizeof(lstep));
            make_label(lend,   sizeof(lend));
            symtab_push_scope(st);
            if (n->as.for_stmt.init) gen_stmt(n->as.for_stmt.init, st, out);
            sb_printf(out, "%s:\n", lstart);
            if (n->as.for_stmt.cond) {
                gen_expr(n->as.for_stmt.cond, st, out);
                sb_printf(out, "JZ %s\n", lend);
            }
            push_loop(lstep, lend);
            gen_stmt(n->as.for_stmt.body, st, out);
            pop_loop();
            sb_printf(out, "%s:\n", lstep);
            if (n->as.for_stmt.step) gen_stmt(n->as.for_stmt.step, st, out);
            sb_printf(out, "JMP %s\n", lstart);
            sb_printf(out, "%s:\n", lend);
            symtab_pop_scope(st);
            break;
        }
        case NODE_BREAK:
            if (g_loop_depth == 0) cl_die("'break' outside a loop");
            sb_printf(out, "JMP %s\n", g_loops[g_loop_depth - 1].break_label);
            break;
        case NODE_CONTINUE:
            if (g_loop_depth == 0) cl_die("'continue' outside a loop");
            sb_printf(out, "JMP %s\n", g_loops[g_loop_depth - 1].continue_label);
            break;
        case NODE_BLOCK:
            symtab_push_scope(st);
            for (int i = 0; i < n->as.block.count; i++) {
                gen_stmt(n->as.block.stmts[i], st, out);
            }
            symtab_pop_scope(st);
            break;
        case NODE_RETURN:
            if (n->as.return_stmt.expr) {
                gen_expr(n->as.return_stmt.expr, st, out);
            } else {
                sb_add(out, "PUSH 0\n");
            }
            sb_add(out, "RET\n");
            break;
        case NODE_INDEX_ASSIGN: {
            /* Target is now any expression — typically an identifier
               or a chained NODE_INDEX. INDEX_SET mutates the
               underlying container in place; references stay shared. */
            gen_expr(n->as.index_assign.target, st, out);
            gen_expr(n->as.index_assign.index,  st, out);
            gen_expr(n->as.index_assign.value,  st, out);
            sb_add(out, "INDEX_SET\n");
            break;
        }
        case NODE_INDEX_OPASSIGN: {
            /* arr[i] += v  →  evaluate target+index ONCE, DUP2 so the
               same pair feeds both the INDEX_GET (read old value) and
               the INDEX_SET (write new value). Indices with side
               effects fire exactly once. */
            gen_expr(n->as.index_opassign.target, st, out);
            gen_expr(n->as.index_opassign.index,  st, out);
            sb_add(out, "DUP2\n");
            sb_add(out, "INDEX_GET\n");
            gen_expr(n->as.index_opassign.value, st, out);
            switch (n->as.index_opassign.op) {
                case TOK_PLUS:    sb_add(out, "ADD\n"); break;
                case TOK_MINUS:   sb_add(out, "SUB\n"); break;
                case TOK_STAR:    sb_add(out, "MUL\n"); break;
                case TOK_SLASH:   sb_add(out, "DIV\n"); break;
                case TOK_PERCENT: sb_add(out, "MOD\n"); break;
                default:          cl_die("unknown compound op");
            }
            sb_add(out, "INDEX_SET\n");
            break;
        }
        case NODE_CALL:
            /* Call-as-statement: evaluate the call expression, then
               drop the return value off the stack. */
            gen_expr(n, st, out);
            sb_add(out, "DROP\n");
            break;
        default:
            cl_die("invalid statement node");
    }
}

/* Closures queued during NODE_FN expression emission and appended to
   the main output after top-level fn bodies. */
typedef struct {
    char  *body_buf;   /* tracked allocation containing the closure's full assembly */
} CgPendingClosure;

static CgPendingClosure g_pending_closures[CG_MAX_FUNCTIONS];
static int g_pending_closure_count = 0;
static int g_closure_id_counter    = 0;

static void gen_inline_closure(AST *fn_node, SymbolTable *st, StrBuf *parent_out) {
    if (g_pending_closure_count >= CG_MAX_FUNCTIONS) cl_die("too many closures in one program");

    char label[64];
    snprintf(label, sizeof(label), ".cl_%d", g_closure_id_counter++);

    symtab_enter_function(st);
    symtab_push_scope(st);

    int my_depth = st->function_frame_depth;
    CgUpvalCtx *my_ctx = &g_upval_stack[my_depth - 1];
    my_ctx->count  = 0;
    my_ctx->active = 1;

    AST *saved_fn_body = g_current_fn_body;
    g_current_fn_body = fn_node->as.fn_def.body;

    /* Declare params. Each captured-by-inner param gets boxed at
       function entry. */
    for (int i = 0; i < fn_node->as.fn_def.param_count; i++) {
        symtab_declare(st, fn_node->as.fn_def.params[i],
                       (int)fn_node->as.fn_def.param_types[i]);
        if (captured_in_inner_fn(fn_node->as.fn_def.body, fn_node->as.fn_def.params[i])) {
            symtab_mark_boxed(st, fn_node->as.fn_def.params[i]);
        }
    }

    /* Build the body into a temp buffer so the upvalue list is fully
       populated by the time we emit MAKE_CLOSURE in the parent. */
    StrBuf inner;
    sb_init(&inner);
    gen_stmt(fn_node->as.fn_def.body, st, &inner);
    sb_add(&inner, "PUSH 0\n");
    sb_add(&inner, "RET\n");

    symtab_pop_scope(st);
    int frame_total = symtab_exit_function(st);
    int n_params = fn_node->as.fn_def.param_count;
    int extra = frame_total - n_params;
    if (extra < 0) extra = 0;

    /* Assemble the final body: label, TYPECHECKs, ENTER, param boxes,
       inner code. */
    StrBuf body;
    sb_init(&body);
    sb_printf(&body, "%s:  ; closure (%d param%s, %d upval%s)\n",
        label, n_params, n_params == 1 ? "" : "s",
        my_ctx->count, my_ctx->count == 1 ? "" : "s");
    for (int i = 0; i < n_params; i++) {
        TypeAnnot t = fn_node->as.fn_def.param_types[i];
        if (t != TYPE_ANY) {
            sb_printf(&body, "TYPECHECK %d %d  ; %s: %s\n",
                i, (int)t, fn_node->as.fn_def.params[i], type_annot_name(t));
        }
    }
    if (extra > 0) sb_printf(&body, "ENTER %d\n", extra);
    /* Box any params that get captured by inner closures inside this
       body. The frame slot already holds the original value, so we
       load it, wrap in a 1-element array, and put the box back. */
    for (int i = 0; i < n_params; i++) {
        if (captured_in_inner_fn(fn_node->as.fn_def.body, fn_node->as.fn_def.params[i])) {
            sb_printf(&body, "LOAD_LOCAL %d  ; box %s\n", i, fn_node->as.fn_def.params[i]);
            sb_add(&body, "NEW_ARRAY 1\n");
            sb_printf(&body, "STORE_LOCAL %d\n", i);
        }
    }
    sb_add(&body, inner.buf);
    cl_track_free(inner.buf);

    g_pending_closures[g_pending_closure_count].body_buf = body.buf;
    g_pending_closure_count++;

    /* Parent context: for each captured upvalue, push its source
       (either a parent local slot or a parent upvalue index), then
       MAKE_CLOSURE bundles them into the new closure value. */
    for (int i = 0; i < my_ctx->count; i++) {
        const CgUpvalEntry *e = &my_ctx->items[i];
        if (e->source_kind == CG_UPVAL_FROM_LOCAL) {
            sb_printf(parent_out, "LOAD_LOCAL %d  ; capture %s (parent local)\n",
                e->source_index, e->name);
        } else {
            sb_printf(parent_out, "LOAD_UPVAL %d  ; capture %s (parent upval)\n",
                e->source_index, e->name);
        }
    }
    sb_printf(parent_out, "MAKE_CLOSURE %s %d\n", label, my_ctx->count);

    my_ctx->active = 0;
    g_current_fn_body = saved_fn_body;
}

static void gen_function(AST *fn, SymbolTable *st, StrBuf *out) {
    /* Compile the body into a temp buffer so we can measure its peak
       local-offset use and prepend the right ENTER. Params occupy the
       lowest offsets — they are already on the data stack when CALL
       sets fp, so the function only needs to allocate space for the
       *additional* locals declared in the body. */
    symtab_enter_function(st);
    symtab_push_scope(st);

    int my_depth = st->function_frame_depth;
    g_upval_stack[my_depth - 1].count  = 0;
    g_upval_stack[my_depth - 1].active = 1;

    AST *saved_fn_body = g_current_fn_body;
    g_current_fn_body = fn->as.fn_def.body;

    for (int i = 0; i < fn->as.fn_def.param_count; i++) {
        /* Seed the parameter's inferred type from its annotation so
           uses inside the body benefit from the same inference rules
           as `let` bindings. */
        symtab_declare(st, fn->as.fn_def.params[i],
                       (int)fn->as.fn_def.param_types[i]);
        /* If any inner closure references this param, mark it boxed
           so reads/writes go through a 1-element array. */
        if (captured_in_inner_fn(fn->as.fn_def.body, fn->as.fn_def.params[i])) {
            symtab_mark_boxed(st, fn->as.fn_def.params[i]);
        }
    }

    StrBuf body;
    sb_init(&body);
    gen_stmt(fn->as.fn_def.body, st, &body);
    /* Default `return 0` for fall-through. If the body already RETted,
       these instructions are unreachable. */
    sb_add(&body, "PUSH 0\n");
    sb_add(&body, "RET\n");

    symtab_pop_scope(st);
    int frame_total  = symtab_exit_function(st);
    int extra_locals = frame_total - fn->as.fn_def.param_count;
    if (extra_locals < 0) extra_locals = 0;

    /* Emit a human-readable signature comment on the label line.
       Comments after `;` are stripped by the assembler, so this is
       pure documentation visible in the generated .casm. The label
       prefix tracks the function's visibility. */
    sb_printf(out, "%s%s:  ; %s(",
        fn_label_prefix(fn->as.fn_def.is_public),
        fn->as.fn_def.name,
        fn->as.fn_def.is_public ? "pub " : "");
    for (int i = 0; i < fn->as.fn_def.param_count; i++) {
        if (i > 0) sb_add(out, ", ");
        sb_printf(out, "%s: %s",
            fn->as.fn_def.params[i],
            type_annot_name(fn->as.fn_def.param_types[i]));
    }
    sb_printf(out, "): %s\n", type_annot_name(fn->as.fn_def.return_type));
    /* Runtime type checks on typed parameters. `any`-typed (the
       default for unannotated params) skips the check, so the cost
       is zero for fully-dynamic functions. */
    for (int i = 0; i < fn->as.fn_def.param_count; i++) {
        TypeAnnot t = fn->as.fn_def.param_types[i];
        if (t != TYPE_ANY) {
            sb_printf(out, "TYPECHECK %d %d  ; %s: %s\n",
                i, (int)t, fn->as.fn_def.params[i], type_annot_name(t));
        }
    }
    if (extra_locals > 0) sb_printf(out, "ENTER %d\n", extra_locals);
    /* Wrap any captured params in 1-element arrays so inner closures
       can see and mutate them through the captured box. */
    for (int i = 0; i < fn->as.fn_def.param_count; i++) {
        if (captured_in_inner_fn(fn->as.fn_def.body, fn->as.fn_def.params[i])) {
            sb_printf(out, "LOAD_LOCAL %d  ; box %s\n", i, fn->as.fn_def.params[i]);
            sb_add(out, "NEW_ARRAY 1\n");
            sb_printf(out, "STORE_LOCAL %d\n", i);
        }
    }
    sb_add(out, body.buf);
    cl_track_free(body.buf);

    g_upval_stack[my_depth - 1].active = 0;
    g_current_fn_body = saved_fn_body;
}

char *codegen_program(Program *prog) {
    g_label_counter         = 0;
    g_loop_depth            = 0;
    g_strings.count         = 0;
    g_trampoline_count      = 0;
    g_pending_closure_count = 0;
    g_closure_id_counter    = 0;
    g_current_fn_body       = NULL;
    for (int i = 0; i < CL_MAX_SCOPES; i++) {
        g_upval_stack[i].count  = 0;
        g_upval_stack[i].active = 0;
    }
    /* Pre-scan: record every top-level function name + visibility +
       signature so call sites can validate arg count, type-check each
       arg against the declared parameter type, and emit the right
       label prefix for the callee's visibility. */
    g_user_fn_count = 0;
    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind != NODE_FN) continue;
        if (cl_find_builtin(n->as.fn_def.name)) {
            fprintf(stderr,
                "semantic error: function name '%s' shadows a calclib builtin\n",
                n->as.fn_def.name);
            exit(1);
        }
        if (find_user_function(n->as.fn_def.name)) {
            fprintf(stderr,
                "semantic error: function '%s' declared more than once\n",
                n->as.fn_def.name);
            exit(1);
        }
        if (g_user_fn_count >= CG_MAX_FUNCTIONS) cl_die("too many user functions");
        CgUserFn *u = &g_user_fns[g_user_fn_count++];
        cl_strncpy_z(u->name, n->as.fn_def.name, CL_MAX_TEXT);
        u->is_public   = n->as.fn_def.is_public;
        u->param_count = n->as.fn_def.param_count;
        for (int p = 0; p < n->as.fn_def.param_count; p++) {
            u->param_types[p] = n->as.fn_def.param_types[p];
        }
        u->return_type = n->as.fn_def.return_type;
    }

    SymbolTable st;
    symtab_init(&st);
    StrBuf body;
    sb_init(&body);
    sb_add(&body, "; generated Calc assembly\n");

    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind == NODE_FN) continue;
        gen_stmt(n, &st, &body);
    }
    sb_add(&body, "HALT\n");

    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind != NODE_FN) continue;
        /* Extern functions are forward declarations — their body lives
           in another compilation unit. We register the name during the
           pre-scan so calls compile, but we emit no body here. */
        if (n->as.fn_def.is_extern) continue;
        gen_function(n, &st, &body);
    }

    /* Emit each inline closure's body. gen_inline_closure already
       built the full body (label + ENTER + body + default return)
       and queued it here. Iterate by index because emitting a closure
       body that itself contains another inline closure will append
       to the same list. */
    for (int i = 0; i < g_pending_closure_count; i++) {
        sb_add(&body, g_pending_closures[i].body_buf);
        cl_track_free(g_pending_closures[i].body_buf);
    }

    /* Emit one trampoline per builtin that was used as a value. Each
       just loads its parameters from the call frame, dispatches via
       BUILTIN <id>, and returns. The label is local (`.bi_<name>`)
       so it doesn't pollute the global symbol table. */
    for (int i = 0; i < g_trampoline_count; i++) {
        const CgTrampoline *t = &g_trampolines[i];
        sb_printf(&body, ".bi_%s:  ; calclib trampoline\n", t->name);
        for (int p = 0; p < t->arg_count; p++) {
            sb_printf(&body, "LOAD_LOCAL %d\n", p);
        }
        sb_printf(&body, "BUILTIN %d  ; %s\n", t->builtin_id, t->name);
        sb_add(&body, "RET\n");
    }

    StrBuf out;
    sb_init(&out);
    sb_printf(&out, ".var_count %d\n", st.next_global_slot);
    for (int i = 0; i < g_strings.count; i++) {
        sb_printf(&out, ".string %d ", i);
        sb_append_quoted(&out, g_strings.items[i]);
        sb_add(&out, "\n");
    }
    sb_add(&out, body.buf);
    cl_track_free(body.buf);
    return out.buf;
}
