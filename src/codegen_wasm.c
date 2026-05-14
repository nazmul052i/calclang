/*
   WebAssembly text-format (.wat) backend — Stage 1: numeric subset.

   Emits a .wat module covering CalcLang's numeric subset:
     - f64 number literals
     - arithmetic, comparison, logical, bitwise operators
     - let / assign / variable read (locals inside fns, globals at top level)
     - if/else, while, for, do-while, break, continue
     - function definition with f64 params + f64 return
     - print (numbers only) via a host import
     - direct calls + math-builtin imports (sin, cos, sqrt, ...)

   Strings, arrays, maps, classes, closures, structs, imports, FFI,
   read_key/sleep_ms — all reserved for later stages.

   The output runs in any Wasm engine: wasmtime/wasmer (CLI), Node.js,
   browsers, or embedded Wasm runtimes. The host provides `print_num`
   plus the math intrinsics via the "env" namespace.

   Wasm is a stack machine: operands are pushed, ops consume them and
   push results. Comparison ops produce i32 0/1 and we convert back to
   f64 to keep CalcLang's "everything is a num" model. Bitwise ops do
   the f64 -> i64 -> f64 round-trip the runtime does too.
*/

#include "ast.h"
#include "parser.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* --- String builder -------------------------------------------------- */
typedef struct {
    char  *buf;
    size_t len, cap;
} Sb;

static void sb_init(Sb *s) {
    s->cap = 4096;
    s->len = 0;
    s->buf = (char *)cl_track_malloc(s->cap);
    s->buf[0] = '\0';
}
static void sb_grow(Sb *s, size_t need) {
    if (s->len + need + 1 <= s->cap) return;
    while (s->len + need + 1 > s->cap) s->cap *= 2;
    s->buf = (char *)cl_track_realloc(s->buf, s->cap);
}
static void sb_putc(Sb *s, char c) { sb_grow(s, 1); s->buf[s->len++] = c; s->buf[s->len] = '\0'; }
static void sb_puts(Sb *s, const char *str) {
    size_t n = strlen(str);
    sb_grow(s, n);
    memcpy(s->buf + s->len, str, n);
    s->len += n;
    s->buf[s->len] = '\0';
}
__attribute__((format(printf, 2, 3)))
static void sb_printf(Sb *s, const char *fmt, ...) {
    char tmp[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    sb_grow(s, (size_t)n);
    memcpy(s->buf + s->len, tmp, (size_t)n);
    s->len += (size_t)n;
    s->buf[s->len] = '\0';
}

/* --- Codegen state --------------------------------------------------- */
#define WCG_MAX_FNS    256
#define WCG_MAX_LOCALS 256
#define WCG_MAX_DEPTH  64

typedef struct {
    char name[CL_MAX_TEXT];
    int  param_count;
} WFn;

typedef struct {
    char name[CL_MAX_TEXT];
} WGlobal;

/* Math intrinsics we import from the host. Anything not in this table
   that the user calls and that isn't a user fn becomes a hard error.
   These map 1:1 to host-provided JS / wasmtime functions. */
typedef struct {
    const char *calc_name;
    const char *wasm_name;     /* what we import under */
    int         arity;
    int         is_native;     /* 1 = use Wasm's native op (e.g. f64.sqrt) */
} WIntrinsic;
static const WIntrinsic INTRINSICS[] = {
    {"sqrt",  "sqrt",  1, 1},
    {"abs",   "abs",   1, 1},
    {"floor", "floor", 1, 1},
    {"ceil",  "ceil",  1, 1},
    {"min",   "min",   2, 1},
    {"max",   "max",   2, 1},
    /* These need host imports — no Wasm built-in. */
    {"sin",   "sin",   1, 0},
    {"cos",   "cos",   1, 0},
    {"tan",   "tan",   1, 0},
    {"asin",  "asin",  1, 0},
    {"acos",  "acos",  1, 0},
    {"atan",  "atan",  1, 0},
    {"atan2", "atan2", 2, 0},
    {"exp",   "exp",   1, 0},
    {"log",   "log",   1, 0},
    {"log10", "log10", 1, 0},
    {"pow",   "pow",   2, 0},
    {"round", "round", 1, 0},
    {"random","random",0, 0},
    {"pi",    "pi",    0, 0},
    {"e",     "e",     0, 0},
    {NULL, NULL, 0, 0}
};
static const WIntrinsic *find_intrinsic(const char *name) {
    for (int i = 0; INTRINSICS[i].calc_name; i++) {
        if (strcmp(INTRINSICS[i].calc_name, name) == 0) return &INTRINSICS[i];
    }
    return NULL;
}

typedef struct {
    Sb     out;          /* full module text */
    Sb     fns_buf;      /* function bodies emitted into here, flushed later */

    WFn      fns[WCG_MAX_FNS];
    int      fn_count;
    WGlobal  globals[WCG_MAX_FNS];
    int      global_count;

    /* Per-function state. */
    char  locals[WCG_MAX_LOCALS][CL_MAX_TEXT];
    int   local_count;
    int   declared_local_count;      /* locals declared at the top of the current fn */
    int   in_fn;                     /* 1 inside a user fn, 0 in _start */

    /* Loop label stack — break jumps to brk[depth], continue to cont[depth]. */
    int   loop_depth;
    int   brk_label[WCG_MAX_DEPTH];
    int   cont_label[WCG_MAX_DEPTH];
    int   next_label;
} WCg;

static void wcg_init(WCg *cg) {
    memset(cg, 0, sizeof *cg);
    sb_init(&cg->out);
    sb_init(&cg->fns_buf);
}

/* --- Symbol resolution ---------------------------------------------- */

static WFn *find_user_fn(WCg *cg, const char *name) {
    for (int i = 0; i < cg->fn_count; i++) {
        if (strcmp(cg->fns[i].name, name) == 0) return &cg->fns[i];
    }
    return NULL;
}

static int is_local(WCg *cg, const char *name) {
    for (int i = 0; i < cg->local_count; i++) {
        if (strcmp(cg->locals[i], name) == 0) return 1;
    }
    return 0;
}

static void add_local(WCg *cg, const char *name) {
    if (cg->local_count >= WCG_MAX_LOCALS) cl_die("too many locals in fn");
    cl_strncpy_z(cg->locals[cg->local_count++], name, CL_MAX_TEXT);
}

/* --- Pre-passes ----------------------------------------------------- */

/* Register every user fn so calls can forward-reference them. Doesn't
   emit code; locals are collected separately per function body and per
   top-level _start. Stage 1 keeps every variable as a Wasm local —
   top-level fns can't see top-level lets anyway (same as the native
   backend), so we don't need globals yet. */
static void scan_top_level(WCg *cg, const Program *prog) {
    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind == NODE_FN && n->as.fn_def.name[0] != '\0') {
            if (cg->fn_count >= WCG_MAX_FNS) cl_die("too many functions");
            cl_strncpy_z(cg->fns[cg->fn_count].name,
                         n->as.fn_def.name, CL_MAX_TEXT);
            cg->fns[cg->fn_count].param_count = n->as.fn_def.param_count;
            cg->fn_count++;
        }
    }
}

/* Scan a function body for `let` declarations so we can declare them
   at the top of the Wasm fn (locals must be declared before any code).
   Recurses into blocks/control structures. Doesn't recurse into nested
   `fn` definitions — Stage 1 doesn't support those. */
static void collect_locals(WCg *cg, AST *n) {
    if (!n) return;
    switch (n->kind) {
        case NODE_LET:
            if (!is_local(cg, n->as.let_stmt.name)) {
                add_local(cg, n->as.let_stmt.name);
            }
            return;
        case NODE_BLOCK:
            for (int i = 0; i < n->as.block.count; i++)
                collect_locals(cg, n->as.block.stmts[i]);
            return;
        case NODE_IF:
            collect_locals(cg, n->as.if_stmt.then_branch);
            collect_locals(cg, n->as.if_stmt.else_branch);
            return;
        case NODE_WHILE:    collect_locals(cg, n->as.while_stmt.body); return;
        case NODE_DO_WHILE: collect_locals(cg, n->as.while_stmt.body); return;
        case NODE_FOR:
            collect_locals(cg, n->as.for_stmt.init);
            collect_locals(cg, n->as.for_stmt.body);
            return;
        case NODE_SWITCH:
            for (int i = 0; i < n->as.switch_stmt.case_count; i++)
                collect_locals(cg, n->as.switch_stmt.case_bodies[i]);
            collect_locals(cg, n->as.switch_stmt.default_body);
            return;
        default:
            return;
    }
}

/* --- Expression emission -------------------------------------------- */

static void gen_expr(WCg *cg, Sb *out, AST *n);
static void gen_stmt(WCg *cg, Sb *out, AST *n);

/* Push the condition's truthiness as an i32 0/1. CalcLang booleans are
   "0.0 is false, anything else true", so we emit `f64 0 cmp f64.ne`. */
static void gen_truthy(WCg *cg, Sb *out, AST *cond) {
    gen_expr(cg, out, cond);
    sb_puts(out, "f64.const 0\nf64.ne\n");
}

static void gen_binop(WCg *cg, Sb *out, AST *n) {
    TokenType op = n->as.binop.op;
    /* Logical && / || short-circuit. We emit a fresh `if` that decides
       whether to evaluate the right side. The result lives on the
       stack as f64 (0.0 or 1.0). */
    if (op == TOK_AND) {
        gen_truthy(cg, out, n->as.binop.left);
        sb_puts(out, "(if (result f64) (then\n");
        gen_truthy(cg, out, n->as.binop.right);
        sb_puts(out, "f64.convert_i32_u\n) (else f64.const 0))\n");
        return;
    }
    if (op == TOK_OR) {
        gen_truthy(cg, out, n->as.binop.left);
        sb_puts(out, "(if (result f64) (then f64.const 1) (else\n");
        gen_truthy(cg, out, n->as.binop.right);
        sb_puts(out, "f64.convert_i32_u\n))\n");
        return;
    }

    gen_expr(cg, out, n->as.binop.left);
    gen_expr(cg, out, n->as.binop.right);
    switch (op) {
        case TOK_PLUS:   sb_puts(out, "f64.add\n"); return;
        case TOK_MINUS:  sb_puts(out, "f64.sub\n"); return;
        case TOK_STAR:   sb_puts(out, "f64.mul\n"); return;
        case TOK_SLASH:  sb_puts(out, "f64.div\n"); return;
        /* % needs i64 round-trip — Wasm has no f64.rem. Use the operand
           order that matches CalcLang: trunc both to i64 and rem_s. */
        case TOK_PERCENT:
            sb_puts(out,
                "(local.set $__wcg_b)\n"
                "(local.set $__wcg_a)\n"
                "local.get $__wcg_a\n"
                "i64.trunc_f64_s\n"
                "local.get $__wcg_b\n"
                "i64.trunc_f64_s\n"
                "i64.rem_s\n"
                "f64.convert_i64_s\n");
            return;
        /* Comparisons: f64.* produce i32; convert back to f64. */
        case TOK_EQEQ:  sb_puts(out, "f64.eq\nf64.convert_i32_u\n"); return;
        case TOK_NEQ:   sb_puts(out, "f64.ne\nf64.convert_i32_u\n"); return;
        case TOK_LT:    sb_puts(out, "f64.lt\nf64.convert_i32_u\n"); return;
        case TOK_LE:    sb_puts(out, "f64.le\nf64.convert_i32_u\n"); return;
        case TOK_GT:    sb_puts(out, "f64.gt\nf64.convert_i32_u\n"); return;
        case TOK_GE:    sb_puts(out, "f64.ge\nf64.convert_i32_u\n"); return;
        /* Bitwise: i64 round-trip. */
        case TOK_AMP:
        case TOK_PIPE:
        case TOK_CARET:
        case TOK_LSHIFT:
        case TOK_RSHIFT: {
            sb_puts(out,
                "(local.set $__wcg_b)\n"
                "(local.set $__wcg_a)\n"
                "local.get $__wcg_a\n"
                "i64.trunc_f64_s\n"
                "local.get $__wcg_b\n"
                "i64.trunc_f64_s\n");
            if (op == TOK_AMP)    sb_puts(out, "i64.and\n");
            if (op == TOK_PIPE)   sb_puts(out, "i64.or\n");
            if (op == TOK_CARET)  sb_puts(out, "i64.xor\n");
            if (op == TOK_LSHIFT) sb_puts(out, "i64.shl\n");
            if (op == TOK_RSHIFT) sb_puts(out, "i64.shr_s\n");
            sb_puts(out, "f64.convert_i64_s\n");
            return;
        }
        default:
            cl_die("codegen_wasm: unsupported binop");
    }
}

static void gen_unop(WCg *cg, Sb *out, AST *n) {
    gen_expr(cg, out, n->as.unop.operand);
    switch (n->as.unop.op) {
        case TOK_MINUS:  sb_puts(out, "f64.neg\n"); return;
        case TOK_BANG:   /* !x = x == 0 */
            sb_puts(out, "f64.const 0\nf64.eq\nf64.convert_i32_u\n");
            return;
        case TOK_TILDE:  /* ~x — i64 round-trip */
            sb_puts(out,
                "i64.trunc_f64_s\n"
                "i64.const -1\n"
                "i64.xor\n"
                "f64.convert_i64_s\n");
            return;
        default:
            cl_die("codegen_wasm: unsupported unop");
    }
}

static void gen_call(WCg *cg, Sb *out, AST *n) {
    if (n->as.call.callee) {
        cl_die("codegen_wasm Stage 1: indirect calls not supported");
    }
    const WIntrinsic *ic = find_intrinsic(n->as.call.name);
    if (ic) {
        if (n->as.call.arg_count != ic->arity) {
            fprintf(stderr,
                "codegen_wasm: '%s' takes %d arg%s, got %d\n",
                ic->calc_name, ic->arity, ic->arity == 1 ? "" : "s",
                n->as.call.arg_count);
            exit(1);
        }
        for (int i = 0; i < n->as.call.arg_count; i++)
            gen_expr(cg, out, n->as.call.args[i]);
        if (ic->is_native) {
            /* Native Wasm op: f64.sqrt, f64.abs, ... */
            sb_printf(out, "f64.%s\n", ic->wasm_name);
        } else {
            sb_printf(out, "call $%s\n", ic->wasm_name);
        }
        return;
    }
    WFn *f = find_user_fn(cg, n->as.call.name);
    if (!f) {
        fprintf(stderr,
            "codegen_wasm: undefined function '%s'. Stage 1 supports "
            "the numeric subset only — strings, arrays, maps, classes, "
            "FFI etc. need later stages.\n", n->as.call.name);
        exit(1);
    }
    if (f->param_count != n->as.call.arg_count) {
        fprintf(stderr,
            "codegen_wasm: '%s' takes %d arg%s, got %d\n",
            f->name, f->param_count,
            f->param_count == 1 ? "" : "s", n->as.call.arg_count);
        exit(1);
    }
    for (int i = 0; i < n->as.call.arg_count; i++)
        gen_expr(cg, out, n->as.call.args[i]);
    sb_printf(out, "call $calc_%s\n", f->name);
}

static void gen_var_read(WCg *cg, Sb *out, AST *n) {
    if (is_local(cg, n->as.var)) {
        sb_printf(out, "local.get $%s\n", n->as.var);
        return;
    }
    fprintf(stderr, "codegen_wasm: undefined variable '%s'\n", n->as.var);
    exit(1);
}

static void gen_ternary(WCg *cg, Sb *out, AST *n) {
    gen_truthy(cg, out, n->as.ternary.cond);
    sb_puts(out, "(if (result f64) (then\n");
    gen_expr(cg, out, n->as.ternary.then_expr);
    sb_puts(out, ") (else\n");
    gen_expr(cg, out, n->as.ternary.else_expr);
    sb_puts(out, "))\n");
}

static void gen_expr(WCg *cg, Sb *out, AST *n) {
    switch (n->kind) {
        case NODE_NUMBER:
            sb_printf(out, "f64.const %.17g\n", n->as.number);
            return;
        case NODE_BINOP:   gen_binop(cg, out, n); return;
        case NODE_UNOP:    gen_unop(cg, out, n);  return;
        case NODE_VAR:     gen_var_read(cg, out, n); return;
        case NODE_CALL:    gen_call(cg, out, n);  return;
        case NODE_TERNARY: gen_ternary(cg, out, n); return;
        default:
            fprintf(stderr, "codegen_wasm: unsupported expression node %d "
                "(strings/arrays/maps/closures are not in Stage 1)\n", n->kind);
            exit(1);
    }
}

/* --- Statement emission --------------------------------------------- */

static void gen_block(WCg *cg, Sb *out, AST *n) {
    for (int i = 0; i < n->as.block.count; i++)
        gen_stmt(cg, out, n->as.block.stmts[i]);
}

static void gen_if(WCg *cg, Sb *out, AST *n) {
    gen_truthy(cg, out, n->as.if_stmt.cond);
    sb_puts(out, "(if\n(then\n");
    if (n->as.if_stmt.then_branch) gen_stmt(cg, out, n->as.if_stmt.then_branch);
    sb_puts(out, ")\n");
    if (n->as.if_stmt.else_branch) {
        sb_puts(out, "(else\n");
        gen_stmt(cg, out, n->as.if_stmt.else_branch);
        sb_puts(out, ")\n");
    }
    sb_puts(out, ")\n");
}

/* Common shape: a labeled block enclosing a labeled loop, with the
   loop condition first. break -> br $brk; continue -> br $cont. */
static void emit_loop(WCg *cg, Sb *out, AST *cond, AST *step, AST *body) {
    int id = cg->next_label++;
    char brk[32], cont[32];
    snprintf(brk,  sizeof brk,  "wcg_brk_%d",  id);
    snprintf(cont, sizeof cont, "wcg_cont_%d", id);

    if (cg->loop_depth >= WCG_MAX_DEPTH) cl_die("loop nesting too deep");
    cg->brk_label[cg->loop_depth]  = id;
    cg->cont_label[cg->loop_depth] = id;
    cg->loop_depth++;

    sb_printf(out, "(block $%s\n  (loop $%s\n", brk, cont);
    if (cond) {
        gen_truthy(cg, out, cond);
        sb_puts(out, "i32.eqz\n");
        sb_printf(out, "br_if $%s\n", brk);
    }
    if (body) gen_stmt(cg, out, body);
    if (step) gen_stmt(cg, out, step);
    sb_printf(out, "br $%s\n", cont);
    sb_puts(out, "  )\n)\n");

    cg->loop_depth--;
}

static void gen_while(WCg *cg, Sb *out, AST *n) {
    emit_loop(cg, out, n->as.while_stmt.cond, NULL, n->as.while_stmt.body);
}

static void gen_do_while(WCg *cg, Sb *out, AST *n) {
    /* do { body } while (cond) — run body at least once. Emit body
       first, then conditional br to top. */
    int id = cg->next_label++;
    char brk[32], cont[32];
    snprintf(brk,  sizeof brk,  "wcg_brk_%d",  id);
    snprintf(cont, sizeof cont, "wcg_cont_%d", id);

    if (cg->loop_depth >= WCG_MAX_DEPTH) cl_die("loop nesting too deep");
    cg->brk_label[cg->loop_depth]  = id;
    cg->cont_label[cg->loop_depth] = id;
    cg->loop_depth++;

    sb_printf(out, "(block $%s\n  (loop $%s\n", brk, cont);
    if (n->as.while_stmt.body) gen_stmt(cg, out, n->as.while_stmt.body);
    gen_truthy(cg, out, n->as.while_stmt.cond);
    sb_printf(out, "br_if $%s\n  )\n)\n", cont);

    cg->loop_depth--;
}

static void gen_for(WCg *cg, Sb *out, AST *n) {
    if (n->as.for_stmt.init) gen_stmt(cg, out, n->as.for_stmt.init);
    emit_loop(cg, out, n->as.for_stmt.cond, n->as.for_stmt.step, n->as.for_stmt.body);
}

static void gen_break(WCg *cg, Sb *out) {
    if (cg->loop_depth == 0) cl_die("break outside of loop");
    int id = cg->brk_label[cg->loop_depth - 1];
    sb_printf(out, "br $wcg_brk_%d\n", id);
}

static void gen_continue(WCg *cg, Sb *out) {
    if (cg->loop_depth == 0) cl_die("continue outside of loop");
    int id = cg->cont_label[cg->loop_depth - 1];
    sb_printf(out, "br $wcg_cont_%d\n", id);
}

static void gen_let_stmt(WCg *cg, Sb *out, AST *n) {
    /* Every `let` (both top-level and inside fns) is a Wasm local. The
       declaration was emitted at the top of the enclosing function;
       here we just assign. */
    gen_expr(cg, out, n->as.let_stmt.expr);
    sb_printf(out, "local.set $%s\n", n->as.let_stmt.name);
}

static void gen_assign(WCg *cg, Sb *out, AST *n) {
    gen_expr(cg, out, n->as.assign_stmt.expr);
    if (is_local(cg, n->as.assign_stmt.name)) {
        sb_printf(out, "local.set $%s\n", n->as.assign_stmt.name);
    } else {
        fprintf(stderr, "codegen_wasm: assigning to undefined variable '%s'\n",
            n->as.assign_stmt.name);
        exit(1);
    }
}

static void gen_print(WCg *cg, Sb *out, AST *n) {
    gen_expr(cg, out, n->as.print_stmt.expr);
    sb_puts(out, "call $print_num\n");
}

static void gen_return(WCg *cg, Sb *out, AST *n) {
    if (n->as.return_stmt.expr) {
        gen_expr(cg, out, n->as.return_stmt.expr);
    } else {
        sb_puts(out, "f64.const 0\n");   /* implicit return value */
    }
    sb_puts(out, "return\n");
}

static void gen_stmt(WCg *cg, Sb *out, AST *n) {
    switch (n->kind) {
        case NODE_LET:      gen_let_stmt(cg, out, n); return;
        case NODE_ASSIGN:   gen_assign(cg, out, n); return;
        case NODE_PRINT:    gen_print(cg, out, n); return;
        case NODE_IF:       gen_if(cg, out, n); return;
        case NODE_WHILE:    gen_while(cg, out, n); return;
        case NODE_DO_WHILE: gen_do_while(cg, out, n); return;
        case NODE_FOR:      gen_for(cg, out, n); return;
        case NODE_BREAK:    gen_break(cg, out); return;
        case NODE_CONTINUE: gen_continue(cg, out); return;
        case NODE_BLOCK:    gen_block(cg, out, n); return;
        case NODE_RETURN:   gen_return(cg, out, n); return;
        case NODE_CALL:
            /* Expression-statement call: emit, then drop the result. */
            gen_expr(cg, out, n);
            sb_puts(out, "drop\n");
            return;
        default:
            fprintf(stderr, "codegen_wasm: unsupported statement node %d\n", n->kind);
            exit(1);
    }
}

/* --- Function emission ---------------------------------------------- */

static void emit_fn(WCg *cg, AST *fn) {
    /* Reset per-fn state. */
    cg->local_count = 0;
    cg->in_fn = 1;
    cg->loop_depth = 0;

    /* Params are locals 0..param_count-1 by Wasm convention. */
    for (int i = 0; i < fn->as.fn_def.param_count; i++) {
        add_local(cg, fn->as.fn_def.params[i]);
    }
    int param_local_count = cg->local_count;
    /* Walk body for `let` declarations. */
    if (fn->as.fn_def.body) collect_locals(cg, fn->as.fn_def.body);

    Sb *body = &cg->fns_buf;
    sb_printf(body, "(func $calc_%s", fn->as.fn_def.name);
    for (int i = 0; i < fn->as.fn_def.param_count; i++) {
        sb_printf(body, " (param $%s f64)", fn->as.fn_def.params[i]);
    }
    sb_puts(body, " (result f64)\n");

    /* Declare non-param locals + two scratch slots for binop helpers. */
    for (int i = param_local_count; i < cg->local_count; i++) {
        sb_printf(body, "  (local $%s f64)\n", cg->locals[i]);
    }
    sb_puts(body, "  (local $__wcg_a f64) (local $__wcg_b f64)\n");

    if (fn->as.fn_def.body) gen_block(cg, body, fn->as.fn_def.body);

    /* Implicit final return (zero) so the f64 result slot is always
       filled, even when the user forgot a `return`. */
    sb_puts(body, "f64.const 0\nreturn\n)\n\n");

    cg->in_fn = 0;
}

static void emit_start(WCg *cg, const Program *prog) {
    cg->local_count = 0;
    cg->in_fn = 0;
    cg->loop_depth = 0;

    /* Walk all top-level statements (other than fn defs) for their
       `let` declarations. Wasm requires every local to be declared at
       the top of the function before any code. */
    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind == NODE_FN) continue;
        collect_locals(cg, n);
    }

    Sb *body = &cg->fns_buf;
    sb_puts(body, "(func $_start (export \"_start\")\n");
    for (int i = 0; i < cg->local_count; i++) {
        sb_printf(body, "  (local $%s f64)\n", cg->locals[i]);
    }
    sb_puts(body, "  (local $__wcg_a f64) (local $__wcg_b f64)\n");

    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind == NODE_FN) continue;   /* fns emitted separately */
        gen_stmt(cg, body, n);
    }
    sb_puts(body, ")\n");
}

/* --- Module assembly ------------------------------------------------ */

static void emit_module(WCg *cg, const Program *prog) {
    (void)prog;
    /* Header + imports. */
    sb_puts(&cg->out,
        "(module\n"
        "  ;; --- Host imports ---\n"
        "  (import \"env\" \"print_num\" (func $print_num (param f64)))\n");
    /* Math intrinsics that need host help. */
    for (int i = 0; INTRINSICS[i].calc_name; i++) {
        if (INTRINSICS[i].is_native) continue;
        const WIntrinsic *ic = &INTRINSICS[i];
        sb_printf(&cg->out, "  (import \"env\" \"%s\" (func $%s",
            ic->wasm_name, ic->wasm_name);
        for (int k = 0; k < ic->arity; k++) sb_puts(&cg->out, " (param f64)");
        sb_puts(&cg->out, " (result f64)))\n");
    }
    sb_puts(&cg->out, "\n");

    /* User function bodies + _start. */
    sb_puts(&cg->out, cg->fns_buf.buf);

    sb_puts(&cg->out, ")\n");
}

/* --- Entry point ---------------------------------------------------- */

const char *codegen_wasm(const Program *prog) {
    WCg cg;
    wcg_init(&cg);

    scan_top_level(&cg, prog);

    /* Emit user fns into fns_buf. */
    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind == NODE_FN && n->as.fn_def.name[0] != '\0'
            && n->as.fn_def.body) {
            emit_fn(&cg, n);
        }
    }
    /* Top-level code becomes the _start fn. */
    emit_start(&cg, prog);

    /* Final assembly. */
    emit_module(&cg, prog);
    return cg.out.buf;
}

/* Quiet the compiler about unused helpers we keep around for future
   stages (string-allocator / heap-writer etc. will use sb_putc). */
__attribute__((used)) static void wcg_keepalive(void) {
    (void)sb_putc;
}
