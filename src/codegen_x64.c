/*
   Native x86-64 backend.

   This is a parallel codegen path to the VM bytecode one. The lexer,
   parser, and AST are shared; everything past the AST is independent.

   Scope (first cut):
     Numbers only. All values are 64-bit doubles. Supported features:
     literals, +, -, *, /, %, unary -; ==, !=, <, <=, >, >=; &&, ||,
     !; let/assign; if/else; while; for + break/continue; print;
     named functions with parameters, return, and recursion; a handful
     of inline math intrinsics (sqrt, abs, floor, ceil, round, pow,
     min, max, pi, e).
     Strings / arrays / maps / closures / first-class fns / calclib
     are NOT supported — they need a runtime story (heap allocation,
     refcount-or-GC, helper functions for every op) that lives beyond
     this round.

   Output: Intel-syntax AT&T-compatible assembly text. Pipes through
     gcc, which assembles + links it against libc (for printf). On
     Windows that produces a PE .exe; on Linux it produces an ELF.

   Calling convention:
     - Between CalcLang functions: stack-based, cdecl-style. Caller
       pushes args RIGHT TO LEFT into 16-byte slots, calls, then
       cleans up. Arg `i` is accessed inside the callee as
       `[rbp + 16 + i*16]`. The wider slot is for stack alignment:
       every save-and-recurse keeps rsp 16-byte aligned even when the
       inner subtree makes calls of its own. The upper 8 bytes of each
       slot are unused (just padding).
     - Calls into libc (printf): Microsoft x64 ABI. Args in RCX/RDX/
       R8/R9 with the variadic float duplicated into both the integer
       slot and XMM1; 32-byte shadow space + 16-byte rsp alignment.
     - Result register: XMM0.
     - Frame layout: `push rbp; mov rbp, rsp; sub rsp, <slots>`.
       Locals at `[rbp - (slot+1)*8]`. <slots> is rounded up so rsp
       stays 16-aligned at statement boundaries; expression-level
       pushes may temporarily misalign by 8, but those are popped
       before any external call.
     - Volatile (caller-save) regs we touch freely: RAX, RCX, RDX, R10,
       R11, XMM0-XMM5. We never use callee-save regs other than RBP/RSP.

   Stack invariant during expression evaluation:
     A subtree leaves its result in XMM0. Binary ops save the left
     operand to a 16-byte stack slot (`sub rsp, 16; movsd [rsp], xmm0`),
     evaluate the right into XMM0, then `movsd xmm1, [rsp]; add rsp, 16`
     to recover the left into XMM1, and combine. The 16-byte slot
     keeps rsp aligned across nested calls inside the right subtree.

   Doubles in code:
     Encoded as their IEEE 754 bit pattern via `mov rax, <imm64>;
     movq xmm0, rax`. No constant pool. The single string constant we
     do need (the printf format) lives in .rdata.
*/

#include "codegen_x64.h"
#include "common.h"
#include <math.h>

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} Sb;

static void sb_init(Sb *s) {
    s->cap = 4096;
    s->len = 0;
    s->buf = (char *)cl_track_malloc(s->cap);
    s->buf[0] = '\0';
}

static void sb_reserve(Sb *s, size_t extra) {
    if (s->len + extra + 1 > s->cap) {
        while (s->len + extra + 1 > s->cap) s->cap *= 2;
        s->buf = (char *)cl_track_realloc(s->buf, s->cap);
    }
}

static void sb_add(Sb *s, const char *t) {
    size_t n = strlen(t);
    sb_reserve(s, n);
    memcpy(s->buf + s->len, t, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

static void sb_printf(Sb *s, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    sb_reserve(s, (size_t)n);
    memcpy(s->buf + s->len, tmp, (size_t)n);
    s->len += (size_t)n;
    s->buf[s->len] = '\0';
}

/* --- Symbol table -------------------------------------------------- */
/* A scope is a half-open range [scope_start, name_count) in `names`.
   Entering a block pushes scope_start; leaving pops names back to it.
   Each entry maps a name to an rbp offset (positive for params,
   negative for locals). */

#define CG_MAX_NAMES 256
typedef enum {
    CG_NK_PARAM,         /* [rbp + offset], raw value */
    CG_NK_LOCAL,         /* [rbp + offset], raw value */
    CG_NK_LOCAL_BOXED,   /* [rbp + offset] holds a Value*; deref to read */
    CG_NK_UPVAL          /* closure->upvals[index] -> Value*; deref to read */
} CgNameKind;

typedef struct {
    char       name[CL_MAX_TEXT];
    CgNameKind kind;
    int        offset;       /* rbp offset (locals/params) or upval index */
} CgName;

#define CG_MAX_SCOPES 64
#define CG_MAX_FNS    128

#define CG_MAX_STRS   256
/* Interned string literals. Each one becomes a CalcStr in .rdata; uses
   reference the same address. */
typedef struct {
    char bytes[CL_MAX_TEXT];
    int  len;
} CgStr;

/* Closure metadata: for each NODE_FN expression we emit, we generate
   a unique entry label and remember which outer names it captures
   (in order — the closure's upvals[i] holds the box for the i-th
   captured name). The list is consulted both when EMITTING the
   construction site (to copy upvals into the new Closure) and when
   EMITTING the closure body (to know which names resolve to upvals). */
#define CG_MAX_CLOSURES   128
#define CG_MAX_UPVALS_PER 32
typedef struct {
    int  label_id;                                /* .Lclosure_<id> */
    int  upval_count;
    char upval_names[CG_MAX_UPVALS_PER][CL_MAX_TEXT];
    AST *fn_node;                                 /* original NODE_FN */
} CgClosure;

typedef struct {
    char      name[CL_MAX_TEXT];
    int       param_count;
    TypeAnnot param_types[CL_MAX_PARAMS];
    TypeAnnot return_type;
} CgFn;

typedef struct {
    Sb     out;           /* final program output */
    Sb     body;          /* per-function buffer; flushed in flush_fn */

    char   cur_fn_name[CL_MAX_TEXT];  /* function being emitted now */

    int    next_label;    /* fresh-label counter (global) */
    int    in_fn;         /* 1 while emitting inside a user fn */

    int    local_bytes;   /* bytes allocated so far in current fn */
    int    max_locals;    /* max local_bytes seen so far in current fn */

    int    break_label;   /* current loop's break target, -1 if none */
    int    cont_label;    /* current loop's continue target, -1 if none */

    CgName names[CG_MAX_NAMES];
    int    name_count;
    int    scope_starts[CG_MAX_SCOPES];
    int    scope_depth;

    CgFn   fns[CG_MAX_FNS];
    int    fn_count;

    CgStr  strs[CG_MAX_STRS];
    int    str_count;

    /* Closures to emit at the end of the program (after main).
       Populated when we see a NODE_FN expression. */
    CgClosure closures[CG_MAX_CLOSURES];
    int       closure_count;

    /* Builtin names referenced as values (bare-name in NODE_VAR
       context). One trampoline per unique name is emitted at the end
       of the program; the trampoline bridges the cdecl-style calc
       call convention to the MS x64 ABI of the runtime function. */
    char  trampolines[64][CL_MAX_TEXT];
    int   trampoline_count;

    /* Stable label suffix per CalcClosure being emitted. */
    int    next_closure_label;

    /* Pre-computed boxing decisions for the current function's locals
       and params. Walked once at function entry, then consulted by
       add_param / NODE_LET. Names are stored in the order they will
       be encountered; the same name may appear twice if shadowing
       occurs (innermost wins on lookup). */
    char   box_set[CG_MAX_NAMES][CL_MAX_TEXT];
    int    box_count;

    /* When 1, the program is being compiled as a library: top-level
       statements outside any fn are skipped, and no `main` entry is
       emitted. Other compilation units' `main` becomes the program
       entry point. Set by the calcnat driver (`--lib`). */
    int    library_mode;

    /* Lightweight xmm register-stack for binop intermediates.
       binop_depth is the current depth of nested binop saves; at depth
       d in [0, XMM_STACK_MAX) we save xmm0 into xmm<6+d> instead of
       round-tripping via the stack. Above XMM_STACK_MAX we fall back
       to the stack. xmm_used_mask tracks which of xmm6..xmm9 we
       actually wrote to in the current function, so the function-emit
       glue can save/restore exactly those (xmm6+ are nonvolatile in
       MS x64). Both fields are reset at function entry. */
    int          binop_depth;
    unsigned int xmm_used_mask;
} Cg;

#define XMM_STACK_MAX 4   /* xmm6, xmm7, xmm8, xmm9 */

static int fresh_label(Cg *cg) { return cg->next_label++; }

static void scope_push(Cg *cg) {
    if (cg->scope_depth >= CG_MAX_SCOPES) cl_die("scope nesting too deep");
    cg->scope_starts[cg->scope_depth++] = cg->name_count;
}

static void scope_pop(Cg *cg) {
    if (cg->scope_depth <= 0) cl_die("scope pop underflow");
    cg->name_count = cg->scope_starts[--cg->scope_depth];
}

static int is_in_box_set(const Cg *cg, const char *name) {
    for (int i = 0; i < cg->box_count; i++)
        if (strcmp(cg->box_set[i], name) == 0) return 1;
    return 0;
}

/* Add a local: bumps local_bytes and records the negative offset.
   Returns the offset and (via out_kind) whether the local is boxed,
   so the caller can emit a `cl_box_new(initial)` after the rhs is
   produced. Pre-computed during the function's capture analysis. */
static int add_local(Cg *cg, const char *name, CgNameKind *out_kind) {
    if (cg->name_count >= CG_MAX_NAMES) cl_die("too many locals");
    cg->local_bytes += 8;
    if (cg->local_bytes > cg->max_locals) cg->max_locals = cg->local_bytes;
    CgName *e = &cg->names[cg->name_count++];
    cl_strncpy_z(e->name, name, CL_MAX_TEXT);
    e->kind   = is_in_box_set(cg, name) ? CG_NK_LOCAL_BOXED : CG_NK_LOCAL;
    e->offset = -cg->local_bytes;
    if (out_kind) *out_kind = e->kind;
    return e->offset;
}

/* Add a parameter: positive offset (caller's stacked arg). If the
   param name is in the box set, it'll be promoted to a boxed LOCAL
   at function entry — we ADD a separate boxed-local binding with the
   same name, allocated below the saved-r10 slot. The original arg
   slot is still readable at the positive offset but the boxed
   binding shadows it. */
static void add_param(Cg *cg, const char *name, int positive_offset) {
    if (cg->name_count >= CG_MAX_NAMES) cl_die("too many params");
    /* Always add the raw param binding first. */
    CgName *e = &cg->names[cg->name_count++];
    cl_strncpy_z(e->name, name, CL_MAX_TEXT);
    e->kind   = CG_NK_PARAM;
    e->offset = positive_offset;
    /* If captured, add a shadowing boxed-local binding. */
    if (is_in_box_set(cg, name)) {
        if (cg->name_count >= CG_MAX_NAMES) cl_die("too many locals");
        cg->local_bytes += 8;
        if (cg->local_bytes > cg->max_locals) cg->max_locals = cg->local_bytes;
        CgName *b = &cg->names[cg->name_count++];
        cl_strncpy_z(b->name, name, CL_MAX_TEXT);
        b->kind   = CG_NK_LOCAL_BOXED;
        b->offset = -cg->local_bytes;
    }
}

/* Add an upvalue binding so references to a captured name resolve to
   `closure->upvals[index]`. */
static void add_upval(Cg *cg, const char *name, int index) {
    if (cg->name_count >= CG_MAX_NAMES) cl_die("too many names");
    CgName *e = &cg->names[cg->name_count++];
    cl_strncpy_z(e->name, name, CL_MAX_TEXT);
    e->kind   = CG_NK_UPVAL;
    e->offset = index;
}

/* Resolve from innermost scope outward. Returns the CgName ptr or NULL. */
static const CgName *lookup_name(Cg *cg, const char *name) {
    for (int i = cg->name_count - 1; i >= 0; i--) {
        if (strcmp(cg->names[i].name, name) == 0) return &cg->names[i];
    }
    return NULL;
}

static CgFn *lookup_fn(Cg *cg, const char *name) {
    for (int i = 0; i < cg->fn_count; i++) {
        if (strcmp(cg->fns[i].name, name) == 0) return &cg->fns[i];
    }
    return NULL;
}

/* Pre-pass: collect all top-level function definitions (including
   `extern fn` declarations, which carry an arity but no body) so the
   codegen knows arities and link-target names. extern fns become
   bare `call calc_<name>` instructions — the system linker (gcc/ld)
   resolves them against another object file's exported symbol at
   link time. */
static void collect_fns(Cg *cg, const Program *prog) {
    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind == NODE_FN && n->as.fn_def.name[0] != '\0') {
            if (cg->fn_count >= CG_MAX_FNS) cl_die("too many functions");
            CgFn *f = &cg->fns[cg->fn_count++];
            cl_strncpy_z(f->name, n->as.fn_def.name, CL_MAX_TEXT);
            f->param_count = n->as.fn_def.param_count;
            for (int j = 0; j < n->as.fn_def.param_count; j++) {
                f->param_types[j] = n->as.fn_def.param_types[j];
            }
            f->return_type = n->as.fn_def.return_type;
        }
    }
}

/* Does `expected` accept a value of `actual`? Mirrors codegen.c so
   the VM and native pipelines reject the same programs. */
static int x64_type_accepts(TypeAnnot expected, TypeAnnot actual) {
    if (expected == TYPE_ANY)  return 1;
    if (expected == TYPE_BOOL) return actual == TYPE_NUM;
    return expected == actual;
}

/* Best-effort static type of an expression. Returns 1 + writes *out
   when statically knowable, 0 when we'd need runtime info. */
static int x64_static_arg_type(Cg *cg, AST *arg, TypeAnnot *out) {
    switch (arg->kind) {
        case NODE_NUMBER:    *out = TYPE_NUM; return 1;
        case NODE_STRING:    *out = TYPE_STR; return 1;
        case NODE_ARRAY_LIT: *out = TYPE_ARR; return 1;
        case NODE_MAP_LIT:   *out = TYPE_MAP; return 1;
        case NODE_UNOP: {
            TokenType op = arg->as.unop.op;
            if (op == TOK_BANG || op == TOK_TILDE) { *out = TYPE_NUM; return 1; }
            /* unary minus: num iff operand is num */
            TypeAnnot inner;
            if (x64_static_arg_type(cg, arg->as.unop.operand, &inner)
                && inner == TYPE_NUM) {
                *out = TYPE_NUM;
                return 1;
            }
            return 0;
        }
        case NODE_BINOP: {
            TokenType op = arg->as.binop.op;
            if (op == TOK_PLUS) return 0;  /* could be num or str */
            *out = TYPE_NUM;
            return 1;
        }
        case NODE_VAR: {
            if (arg->inferred_type != TYPE_ANY) {
                *out = (TypeAnnot)arg->inferred_type;
                return 1;
            }
            /* Bare function-name reference is a fn value. */
            CgFn *uf = lookup_fn(cg, arg->as.var);
            if (uf) { *out = TYPE_FN; return 1; }
            return 0;
        }
        case NODE_CALL: {
            if (arg->as.call.callee) return 0;
            CgFn *uf = lookup_fn(cg, arg->as.call.name);
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

/* --- Emission helpers --------------------------------------------- */

static Sb *cur(Cg *cg) { return cg->in_fn ? &cg->body : &cg->out; }

static void e(Cg *cg, const char *line) {
    sb_add(cur(cg), "    ");
    sb_add(cur(cg), line);
    sb_add(cur(cg), "\n");
}

static void ef(Cg *cg, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_add(cur(cg), "    ");
    sb_add(cur(cg), tmp);
    sb_add(cur(cg), "\n");
}

static void label(Cg *cg, int n) {
    sb_printf(cur(cg), ".L%d:\n", n);
}

/* Save xmm0 in a 16-byte stack slot. We use 16 bytes (not 8) so any
   subsequent recursive code that wants to make a call still sees an
   rsp that's 16-byte-aligned. The upper 8 bytes are unused. Callers
   restore via [rsp] / add rsp, 16. This convention also applies to
   how function arguments are pushed (each arg gets a 16-byte slot),
   so the callee reads arg i at [rbp + 16 + i*16]. */
static void push_xmm0(Cg *cg) {
    e(cg, "sub  rsp, 16");
    e(cg, "movsd [rsp], xmm0");
}

/* Save block size: 16 bytes per used xmm register. Used at function
   stitch time to extend the locals frame; the save block lives at the
   bottom of the frame (lowest rbp-relative offsets), addressed via
   `[rbp - locals_frame - 16*(slot+1)]` so the epilogue can restore
   even when invoked mid-expression by a `jmp .Lret_X` with an unknown
   rsp. */
static int xmm_save_bytes(Cg *cg) {
    if (cg->xmm_used_mask == 0) return 0;
    int n = 0;
    for (int i = 0; i < XMM_STACK_MAX; i++) {
        if (cg->xmm_used_mask & (1u << i)) n++;
    }
    return n * 16;
}

/* `locals_frame` is the prologue's `sub rsp, K` value (pure locals).
   We've extended the frame by `xmm_save_bytes(cg)` more bytes, with
   each saved xmm sitting at a fixed rbp-relative offset below the
   locals. Call right after the prologue's frame allocation. */
static void emit_xmm_save(Cg *cg, int locals_frame) {
    int slot = 1;
    for (int i = 0; i < XMM_STACK_MAX; i++) {
        if (cg->xmm_used_mask & (1u << i)) {
            sb_printf(&cg->out, "    movapd [rbp - %d], xmm%d\n",
                locals_frame + 16 * slot, 6 + i);
            slot++;
        }
    }
}

/* Call right before `mov rsp, rbp` in the epilogue. */
static void emit_xmm_restore(Cg *cg, int locals_frame) {
    int slot = 1;
    for (int i = 0; i < XMM_STACK_MAX; i++) {
        if (cg->xmm_used_mask & (1u << i)) {
            sb_printf(&cg->out, "    movapd xmm%d, [rbp - %d]\n",
                6 + i, locals_frame + 16 * slot);
            slot++;
        }
    }
}

/* If the expression we just evaluated *might* be a NaN-boxed tagged
   value (anything not statically TI_NUM), convert to 0.0/1.0 via the
   runtime's cl_truthy. This is necessary before any ucomisd-based
   truthiness test, because tagged values are themselves NaN and
   would always compare unordered. */
static void emit_truthify_if_not_num(Cg *cg, int already_num) {
    if (already_num) return;
    e(cg, "movq rcx, xmm0");
    e(cg, "sub  rsp, 32");
    e(cg, "call cl_truthy");
    e(cg, "add  rsp, 32");
    e(cg, "movq xmm0, rax");
}

/* Load the bit pattern of a double constant into xmm0 via rax. */
static void load_double_imm(Cg *cg, double v) {
    union { double d; uint64_t u; } u;
    u.d = v;
    ef(cg, "mov  rax, 0x%llx", (unsigned long long)u.u);
    e(cg, "movq xmm0, rax");
}

/* --- Math intrinsics (call by bare name, no calclib) -------------- */
/* These are inlined where the matching call appears. They are the
   only "bare-name" intrinsics the native backend recognises. */
typedef struct {
    const char *name;
    int         arity;
} Intrinsic;
static const Intrinsic INTRINSICS[] = {
    {"sqrt",  1},
    {"abs",   1},
    {"floor", 1},
    {"ceil",  1},
    {"round", 1},
    {"pow",   2},
    {"min",   2},
    {"max",   2},
    {"pi",    0},
    {"e",     0},
    {NULL, 0}
};

static const Intrinsic *find_intrinsic(const char *name) {
    for (int i = 0; INTRINSICS[i].name; i++) {
        if (strcmp(INTRINSICS[i].name, name) == 0) return &INTRINSICS[i];
    }
    return NULL;
}

/* --- String runtime builtins -------------------------------------- */
/* These are dispatched by calling cl_builtin_<name> in the runtime
   library, which takes Values (uint64_t passed in RCX/RDX/R8). */
typedef struct {
    const char *name;      /* CalcLang name */
    const char *rt_symbol; /* C symbol in src/runtime_x64.c */
    int         arity;
} RtBuiltin;
static const RtBuiltin RT_BUILTINS[] = {
    /* Math (also covered by the inline INTRINSICS table for the fast
       direct-call path; the runtime entries are what trampolines call
       and what find_rt_builtin's first-class-ref lookup consults). */
    {"sqrt",            "cl_builtin_sqrt",            1},
    {"floor",           "cl_builtin_floor",           1},
    {"ceil",            "cl_builtin_ceil",            1},
    {"abs",             "cl_builtin_abs",             1},
    {"int",             "cl_builtin_int",             1},
    {"round",           "cl_builtin_round",           1},
    {"pow",             "cl_builtin_pow",             2},
    {"min",             "cl_builtin_min",             2},
    {"max",             "cl_builtin_max",             2},
    {"sin",             "cl_builtin_sin",             1},
    {"cos",             "cl_builtin_cos",             1},
    {"tan",             "cl_builtin_tan",             1},
    {"asin",            "cl_builtin_asin",            1},
    {"acos",            "cl_builtin_acos",            1},
    {"atan",            "cl_builtin_atan",            1},
    {"atan2",           "cl_builtin_atan2",           2},
    {"exp",             "cl_builtin_exp",             1},
    {"log",             "cl_builtin_log",             1},
    {"log10",           "cl_builtin_log10",           1},
    {"random",          "cl_builtin_random",          0},
    {"pi",              "cl_builtin_pi",              0},
    {"e",               "cl_builtin_e",               0},

    /* Conversion / introspection / generic. */
    {"to_str",          "cl_builtin_to_str",          1},
    {"to_num",          "cl_builtin_to_num",          1},
    {"len",             "cl_builtin_len",             1},
    {"type_of",         "cl_builtin_type_of",         1},

    /* I/O. */
    {"read_line",       "cl_builtin_read_line",       0},
    {"write",           "cl_builtin_write",           1},

    /* Strings. */
    {"str_at",          "cl_builtin_str_at",          2},
    {"str_slice",       "cl_builtin_str_slice",       3},
    {"str_find",        "cl_builtin_str_find",        2},
    {"str_upper",       "cl_builtin_str_upper",       1},
    {"str_lower",       "cl_builtin_str_lower",       1},
    {"str_trim",        "cl_builtin_str_trim",        1},
    {"str_repeat",      "cl_builtin_str_repeat",      2},
    {"str_starts_with", "cl_builtin_str_starts_with", 2},
    {"str_ends_with",   "cl_builtin_str_ends_with",   2},
    {"str_split",       "cl_builtin_str_split",       2},
    {"str_join",        "cl_builtin_str_join",        2},

    /* Arrays. `push` and `pop` are bare names in CalcLang too. */
    {"push",            "cl_arr_push",                2},
    {"pop",             "cl_arr_pop",                 1},
    {"array_reverse",   "cl_builtin_array_reverse",   1},
    {"array_sort",      "cl_builtin_array_sort",      1},
    {"array_concat",    "cl_builtin_array_concat",    2},
    {"array_slice",     "cl_builtin_array_slice",     3},
    {"array_find",      "cl_builtin_array_find",      2},
    {"array_contains",  "cl_builtin_array_contains",  2},
    {"array_range",     "cl_builtin_array_range",     2},

    /* Maps. */
    {"keys",            "cl_builtin_keys",            1},
    {"values",          "cl_builtin_values",          1},
    {"has_key",         "cl_builtin_has_key",         2},
    {"del",             "cl_builtin_del",             2},

    /* File I/O. */
    {"file_read",       "cl_builtin_file_read",       1},
    {"file_write",      "cl_builtin_file_write",      2},
    {"file_append",     "cl_builtin_file_append",     2},
    {"file_exists",     "cl_builtin_file_exists",     1},

    /* Shell. */
    {"system",          "cl_builtin_system",          1},

    /* Terminal / interactive I/O. read_key returns -1 if no key is
       pressed, an ASCII code, or one of 1001-1004 for arrow keys. */
    {"sleep_ms",        "cl_builtin_sleep_ms",        1},
    {"read_key",        "cl_builtin_read_key",        0},
    {"time_ms",         "cl_builtin_time_ms",         0},
    /* Wall-clock date/time. epoch_ms is ms since 1970 UTC; time_make
       and time_components convert to/from broken-down components. */
    {"epoch_ms",        "cl_builtin_epoch_ms",        0},
    {"time_components", "cl_builtin_time_components", 1},
    {"time_make",       "cl_builtin_time_make",       3},
    {"time_format",     "cl_builtin_time_format",     2},

    /* SDL2-backed GUI primitives. See lib/gui.calc for the widget
       helpers built on top. */
    {"gui_init",        "cl_builtin_gui_init",        3},
    {"gui_close",       "cl_builtin_gui_close",       0},
    {"gui_should_close","cl_builtin_gui_should_close",0},
    {"gui_poll_events", "cl_builtin_gui_poll_events", 0},
    {"gui_set_color",   "cl_builtin_gui_set_color",   3},
    {"gui_clear",       "cl_builtin_gui_clear",       3},
    {"gui_rect",        "cl_builtin_gui_rect",        4},
    {"gui_rect_outline","cl_builtin_gui_rect_outline",4},
    {"gui_line",        "cl_builtin_gui_line",        4},
    {"gui_pixel",       "cl_builtin_gui_pixel",       2},
    {"gui_circle",      "cl_builtin_gui_circle",      3},
    {"gui_circle_outline","cl_builtin_gui_circle_outline",3},
    {"gui_text",        "cl_builtin_gui_text",        3},
    {"gui_set_text_scale","cl_builtin_gui_set_text_scale",1},
    {"gui_present",     "cl_builtin_gui_present",     0},
    {"gui_set_title",   "cl_builtin_gui_set_title",   1},
    {"gui_key_down",    "cl_builtin_gui_key_down",    1},
    {"gui_key_pressed", "cl_builtin_gui_key_pressed", 1},
    {"gui_mouse_x",     "cl_builtin_gui_mouse_x",     0},
    {"gui_mouse_y",     "cl_builtin_gui_mouse_y",     0},
    {"gui_mouse_down",  "cl_builtin_gui_mouse_down",  1},
    {"gui_mouse_clicked","cl_builtin_gui_mouse_clicked",1},
    {"gui_text_typed",  "cl_builtin_gui_text_typed",  0},
    {"gui_get_focus",   "cl_builtin_gui_get_focus",   0},
    {"gui_set_focus",   "cl_builtin_gui_set_focus",   1},
    /* Clipping for scrollable regions / windowed plots. */
    {"gui_set_clip",    "cl_builtin_gui_set_clip",    4},
    {"gui_clear_clip",  "cl_builtin_gui_clear_clip",  0},
    /* Modal dialogs + native file pickers. */
    {"gui_message_box", "cl_builtin_gui_message_box", 2},
    {"gui_confirm",     "cl_builtin_gui_confirm",     2},
    {"gui_open_file",   "cl_builtin_gui_open_file",   1},
    {"gui_save_file",   "cl_builtin_gui_save_file",   1},

    /* Complex numbers. */
    {"complex",         "cl_builtin_complex",         2},
    {"real",            "cl_builtin_real",            1},
    {"imag",            "cl_builtin_imag",            1},
    {"conj",            "cl_builtin_conj",            1},
    {"arg",             "cl_builtin_arg",             1},

    /* FFI / dynamic linking. */
    {"ffi_load",        "cl_builtin_ffi_load",        1},
    {"ffi_call",        "cl_builtin_ffi_call",        4},

    /* ctype helpers. */
    {"is_digit",        "cl_builtin_is_digit",        1},
    {"is_alpha",        "cl_builtin_is_alpha",        1},
    {"is_alnum",        "cl_builtin_is_alnum",        1},
    {"is_space",        "cl_builtin_is_space",        1},
    {"is_upper",        "cl_builtin_is_upper",        1},
    {"is_lower",        "cl_builtin_is_lower",        1},
    {"char_to_upper",   "cl_builtin_char_to_upper",   1},
    {"char_to_lower",   "cl_builtin_char_to_lower",   1},
    {"char_code",       "cl_builtin_char_code",       1},
    {"char_from",       "cl_builtin_char_from",       1},

    /* Format string. */
    {"fmt",             "cl_builtin_fmt",             2},
    {NULL, NULL, 0}
};

static const RtBuiltin *find_rt_builtin(const char *name) {
    for (int i = 0; RT_BUILTINS[i].name; i++) {
        if (strcmp(RT_BUILTINS[i].name, name) == 0) return &RT_BUILTINS[i];
    }
    return NULL;
}

/* --- Per-expression static type query ----------------------------- */
/* Returns 1 with *out set when the expression is statically known to
   have a single type. Falls back to TYPE_ANY (and returns 0) when the
   inference can't pin it down. Used by gen_binop and gen_print to
   decide between the numeric fast path and a runtime call. */
typedef enum { TI_UNKNOWN, TI_NUM, TI_STR, TI_OTHER } TInfer;

static TInfer infer_type(Cg *cg, AST *n);
static const RtBuiltin *find_rt_builtin(const char *name);

static TInfer infer_type(Cg *cg, AST *n) {
    if (!n) return TI_UNKNOWN;
    switch (n->kind) {
        case NODE_NUMBER: return TI_NUM;
        case NODE_STRING: return TI_STR;
        case NODE_UNOP: {
            TokenType op = n->as.unop.op;
            if (op == TOK_BANG)  return TI_NUM;  /* always 0.0 / 1.0 */
            if (op == TOK_TILDE) return TI_NUM;  /* always num (int64 cast) */
            /* TOK_MINUS: result is num iff operand is num. */
            TInfer t = infer_type(cg, n->as.unop.operand);
            return (t == TI_NUM) ? TI_NUM : TI_UNKNOWN;
        }
        case NODE_BINOP: {
            TokenType op = n->as.binop.op;
            if (op == TOK_PLUS) {
                TInfer l = infer_type(cg, n->as.binop.left);
                TInfer r = infer_type(cg, n->as.binop.right);
                if (l == TI_NUM && r == TI_NUM) return TI_NUM;
                if (l == TI_STR || r == TI_STR) return TI_STR;
                return TI_UNKNOWN;
            }
            /* `-`, `*`, `/`, `%` are numeric only when both operands
               are statically num — otherwise the result could be a
               complex Value, and the polymorphic dispatch in
               gen_binop produces a CalcCpx. Caller (e.g. gen_print)
               needs to see TI_UNKNOWN so it doesn't take the
               numeric-only printf fast path. */
            if (op == TOK_MINUS || op == TOK_STAR
             || op == TOK_SLASH || op == TOK_PERCENT) {
                TInfer l = infer_type(cg, n->as.binop.left);
                TInfer r = infer_type(cg, n->as.binop.right);
                return (l == TI_NUM && r == TI_NUM) ? TI_NUM : TI_UNKNOWN;
            }
            /* Bitwise ops always produce num (after int64 coercion). */
            if (op == TOK_AMP   || op == TOK_PIPE  || op == TOK_CARET
             || op == TOK_LSHIFT || op == TOK_RSHIFT) {
                return TI_NUM;
            }
            /* Comparisons and logical ops always produce 0.0 / 1.0. */
            return TI_NUM;
        }
        case NODE_VAR:
            if (n->inferred_type == TYPE_NUM)  return TI_NUM;
            if (n->inferred_type == TYPE_STR)  return TI_STR;
            if (n->inferred_type != TYPE_ANY)  return TI_OTHER;
            return TI_UNKNOWN;
        case NODE_CALL: {
            if (n->as.call.callee) return TI_UNKNOWN;
            if (find_intrinsic(n->as.call.name)) return TI_NUM;
            const RtBuiltin *rb = find_rt_builtin(n->as.call.name);
            if (rb) {
                /* Hard-coded per-builtin return type. Could be a
                   field on RtBuiltin; this works for now. */
                if (strcmp(rb->name, "len") == 0
                 || strcmp(rb->name, "str_find") == 0
                 || strcmp(rb->name, "str_starts_with") == 0
                 || strcmp(rb->name, "str_ends_with") == 0
                 || strcmp(rb->name, "to_num") == 0
                 || strcmp(rb->name, "push") == 0
                 || strcmp(rb->name, "array_find") == 0
                 || strcmp(rb->name, "array_contains") == 0
                 || strcmp(rb->name, "has_key") == 0
                 || strcmp(rb->name, "del") == 0
                 || strcmp(rb->name, "sqrt") == 0
                 || strcmp(rb->name, "floor") == 0
                 || strcmp(rb->name, "ceil") == 0
                 || strcmp(rb->name, "abs") == 0
                 || strcmp(rb->name, "int") == 0
                 || strcmp(rb->name, "round") == 0
                 || strcmp(rb->name, "pow") == 0
                 || strcmp(rb->name, "min") == 0
                 || strcmp(rb->name, "max") == 0
                 || strcmp(rb->name, "sin") == 0
                 || strcmp(rb->name, "cos") == 0
                 || strcmp(rb->name, "tan") == 0
                 || strcmp(rb->name, "asin") == 0
                 || strcmp(rb->name, "acos") == 0
                 || strcmp(rb->name, "atan") == 0
                 || strcmp(rb->name, "atan2") == 0
                 || strcmp(rb->name, "exp") == 0
                 || strcmp(rb->name, "log") == 0
                 || strcmp(rb->name, "log10") == 0
                 || strcmp(rb->name, "random") == 0
                 || strcmp(rb->name, "pi") == 0
                 || strcmp(rb->name, "e") == 0
                 || strcmp(rb->name, "write") == 0
                 || strcmp(rb->name, "file_write") == 0
                 || strcmp(rb->name, "file_append") == 0
                 || strcmp(rb->name, "file_exists") == 0
                 || strcmp(rb->name, "system") == 0
                 || strcmp(rb->name, "real") == 0
                 || strcmp(rb->name, "imag") == 0
                 || strcmp(rb->name, "arg") == 0
                 || strcmp(rb->name, "ffi_load") == 0) return TI_NUM;
                /* ffi_call's return depends on the sig; leave as
                   TI_UNKNOWN so the polymorphic path handles print/
                   compare correctly. */
                if (strcmp(rb->name, "to_str") == 0
                 || strcmp(rb->name, "type_of") == 0
                 || strcmp(rb->name, "str_at") == 0
                 || strcmp(rb->name, "str_slice") == 0
                 || strcmp(rb->name, "str_upper") == 0
                 || strcmp(rb->name, "str_lower") == 0
                 || strcmp(rb->name, "str_trim") == 0
                 || strcmp(rb->name, "str_repeat") == 0
                 || strcmp(rb->name, "read_line") == 0
                 || strcmp(rb->name, "file_read") == 0) return TI_STR;
                if (strcmp(rb->name, "array_reverse") == 0
                 || strcmp(rb->name, "array_sort") == 0
                 || strcmp(rb->name, "array_concat") == 0
                 || strcmp(rb->name, "array_slice") == 0
                 || strcmp(rb->name, "array_range") == 0
                 || strcmp(rb->name, "keys") == 0
                 || strcmp(rb->name, "values") == 0
                 || strcmp(rb->name, "str_split") == 0
                 || strcmp(rb->name, "complex") == 0
                 || strcmp(rb->name, "conj") == 0) return TI_OTHER;
            }
            for (int i = 0; i < cg->fn_count; i++) {
                if (strcmp(cg->fns[i].name, n->as.call.name) == 0) {
                    /* User fn return type — only useful if declared. */
                    return TI_UNKNOWN;
                }
            }
            return TI_UNKNOWN;
        }
        default: return TI_UNKNOWN;
    }
}

/* --- String literal pool ------------------------------------------ */
/* Each unique string literal becomes one CalcStr struct in .rdata.
   The format MUST match runtime.h's CalcStr: 8-byte length, then
   bytes, then a trailing NUL so libc functions can read it as a C
   string when needed. */

static int intern_string(Cg *cg, const char *bytes, int len) {
    /* Tiny linear search — strings are short and few. */
    for (int i = 0; i < cg->str_count; i++) {
        if (cg->strs[i].len == len && memcmp(cg->strs[i].bytes, bytes, (size_t)len) == 0)
            return i;
    }
    if (cg->str_count >= CG_MAX_STRS) cl_die("too many string literals");
    CgStr *s = &cg->strs[cg->str_count++];
    memcpy(s->bytes, bytes, (size_t)len);
    s->bytes[len] = '\0';
    s->len = len;
    return cg->str_count - 1;
}

/* Emit asm that loads the tagged Value for string literal `id` into
   xmm0. The Value bits are: (CL_TAG_STR << 48) | addr_of_calcstr. */
static void load_string_imm(Cg *cg, int id) {
    /* rax = &.LCstr_<id>; rax |= (CL_TAG_STR << 48); movq xmm0, rax */
    ef(cg, "lea  rax, [rip + .LCstr_%d]", id);
    e(cg,  "mov  rdx, 0xfff9000000000000");
    e(cg,  "or   rax, rdx");
    e(cg,  "movq xmm0, rax");
}

/* --- Forward decls ------------------------------------------------- */
static void gen_expr(Cg *cg, AST *n);
static void gen_stmt(Cg *cg, AST *n);
static void gen_ternary(Cg *cg, AST *n);
static void gen_do_while(Cg *cg, AST *n);
static int  ast_uses_name_recur(AST *n, const char *name, int descend_into_fns);

/* --- Expression generation ---------------------------------------- */
/* Each gen_expr leaves the result in xmm0. */

static void gen_binop(Cg *cg, AST *n) {
    /* Short-circuit && and ||. */
    if (n->as.binop.op == TOK_AND) {
        int L_false = fresh_label(cg);
        int L_end   = fresh_label(cg);
        TInfer lt = infer_type(cg, n->as.binop.left);
        TInfer rt = infer_type(cg, n->as.binop.right);
        gen_expr(cg, n->as.binop.left);
        emit_truthify_if_not_num(cg, lt == TI_NUM);
        e(cg, "xorpd xmm1, xmm1");
        e(cg, "ucomisd xmm0, xmm1");
        ef(cg, "jp   .L%d", L_false);
        ef(cg, "je   .L%d", L_false);
        gen_expr(cg, n->as.binop.right);
        emit_truthify_if_not_num(cg, rt == TI_NUM);
        e(cg, "xorpd xmm1, xmm1");
        e(cg, "ucomisd xmm0, xmm1");
        ef(cg, "jp   .L%d", L_false);
        ef(cg, "je   .L%d", L_false);
        load_double_imm(cg, 1.0);
        ef(cg, "jmp  .L%d", L_end);
        label(cg, L_false);
        load_double_imm(cg, 0.0);
        label(cg, L_end);
        return;
    }
    if (n->as.binop.op == TOK_OR) {
        int L_true = fresh_label(cg);
        int L_end  = fresh_label(cg);
        TInfer lt = infer_type(cg, n->as.binop.left);
        TInfer rt = infer_type(cg, n->as.binop.right);
        gen_expr(cg, n->as.binop.left);
        emit_truthify_if_not_num(cg, lt == TI_NUM);
        e(cg, "xorpd xmm1, xmm1");
        e(cg, "ucomisd xmm0, xmm1");
        ef(cg, "jne  .L%d", L_true);
        gen_expr(cg, n->as.binop.right);
        emit_truthify_if_not_num(cg, rt == TI_NUM);
        e(cg, "xorpd xmm1, xmm1");
        e(cg, "ucomisd xmm0, xmm1");
        ef(cg, "jne  .L%d", L_true);
        load_double_imm(cg, 0.0);
        ef(cg, "jmp  .L%d", L_end);
        label(cg, L_true);
        load_double_imm(cg, 1.0);
        label(cg, L_end);
        return;
    }

    /* Comparison ops are polymorphic: hardware ucomisd is wrong on
       tagged NaN values (always unordered). For string-string compare
       we also need a real lexicographic order. Dispatch through the
       runtime whenever the inference can't prove both num. */
    {
        TokenType op = n->as.binop.op;
        const char *helper = NULL;
        switch (op) {
            case TOK_EQEQ: helper = "cl_op_eq";  break;
            case TOK_NEQ:  helper = "cl_op_neq"; break;
            case TOK_LT:   helper = "cl_op_lt";  break;
            case TOK_LE:   helper = "cl_op_le";  break;
            case TOK_GT:   helper = "cl_op_gt";  break;
            case TOK_GE:   helper = "cl_op_ge";  break;
            default: break;
        }
        if (helper) {
            TInfer lt = infer_type(cg, n->as.binop.left);
            TInfer rt = infer_type(cg, n->as.binop.right);
            if (!(lt == TI_NUM && rt == TI_NUM)) {
                gen_expr(cg, n->as.binop.left);
                push_xmm0(cg);
                gen_expr(cg, n->as.binop.right);
                e(cg, "movq rdx, xmm0");
                e(cg, "mov  rcx, [rsp]");
                e(cg, "add  rsp, 16");
                e(cg, "sub  rsp, 32");
                ef(cg, "call %s", helper);
                e(cg, "add  rsp, 32");
                e(cg, "movq xmm0, rax");
                return;
            }
        }
    }

    /* `-`, `*`, `/`, `%` are now polymorphic too — complex numbers
       can flow into them, and a plain hardware op on a NaN-tagged
       complex would produce garbage. Dispatch to the runtime helper
       whenever the inference can't prove both operands are num. */
    {
        TokenType op = n->as.binop.op;
        const char *helper = NULL;
        switch (op) {
            case TOK_MINUS:   helper = "cl_op_minus"; break;
            case TOK_STAR:    helper = "cl_op_mul";   break;
            case TOK_SLASH:   helper = "cl_op_div";   break;
            case TOK_PERCENT: helper = "cl_op_mod";   break;
            case TOK_AMP:     helper = "cl_op_band";  break;
            case TOK_PIPE:    helper = "cl_op_bor";   break;
            case TOK_CARET:   helper = "cl_op_bxor";  break;
            case TOK_LSHIFT:  helper = "cl_op_shl";   break;
            case TOK_RSHIFT:  helper = "cl_op_shr";   break;
            default: break;
        }
        if (helper) {
            TInfer lt = infer_type(cg, n->as.binop.left);
            TInfer rt = infer_type(cg, n->as.binop.right);
            if (!(lt == TI_NUM && rt == TI_NUM)) {
                gen_expr(cg, n->as.binop.left);
                push_xmm0(cg);
                gen_expr(cg, n->as.binop.right);
                e(cg, "movq rdx, xmm0");
                e(cg, "mov  rcx, [rsp]");
                e(cg, "add  rsp, 16");
                e(cg, "sub  rsp, 32");
                ef(cg, "call %s", helper);
                e(cg, "add  rsp, 32");
                e(cg, "movq xmm0, rax");
                return;
            }
        }
    }

    /* `+` is polymorphic: num+num is hardware add, anything involving
       a string is concatenation. Decide based on the flow-sensitive
       inferred types; fall back to the runtime cl_op_plus when we
       can't be sure. */
    if (n->as.binop.op == TOK_PLUS) {
        TInfer lt = infer_type(cg, n->as.binop.left);
        TInfer rt = infer_type(cg, n->as.binop.right);
        if (lt == TI_NUM && rt == TI_NUM) {
            /* fall through to the numeric fast path below */
        } else if (lt == TI_STR || rt == TI_STR) {
            /* string concat: call runtime. Args go in RCX (left), RDX (right). */
            gen_expr(cg, n->as.binop.left);
            push_xmm0(cg);
            gen_expr(cg, n->as.binop.right);
            e(cg, "movq rdx, xmm0");
            e(cg, "mov  rcx, [rsp]");
            e(cg, "add  rsp, 16");
            e(cg, "sub  rsp, 32");
            e(cg, "call cl_concat");
            e(cg, "add  rsp, 32");
            e(cg, "movq xmm0, rax");
            return;
        } else {
            /* Unknown — dispatch through cl_op_plus at runtime. */
            gen_expr(cg, n->as.binop.left);
            push_xmm0(cg);
            gen_expr(cg, n->as.binop.right);
            e(cg, "movq rdx, xmm0");
            e(cg, "mov  rcx, [rsp]");
            e(cg, "add  rsp, 16");
            e(cg, "sub  rsp, 32");
            e(cg, "call cl_op_plus");
            e(cg, "add  rsp, 32");
            e(cg, "movq xmm0, rax");
            return;
        }
    }

    /* Eager binops: left in xmm0 -> save, right in xmm0 -> swap left
       into xmm1, then op(xmm0, xmm1) with xmm0=left, xmm1=right.
       Save uses a small register stack (xmm6..xmm9) before falling back
       to a 16-byte stack slot, avoiding the load/store roundtrip on the
       common 4-deep-or-shallower expression tree. */
    gen_expr(cg, n->as.binop.left);
    int slot = -1;
    if (cg->binop_depth < XMM_STACK_MAX) {
        slot = cg->binop_depth;
        cg->xmm_used_mask |= (1u << slot);
        ef(cg, "movapd xmm%d, xmm0", 6 + slot);
    } else {
        push_xmm0(cg);
    }
    cg->binop_depth++;
    gen_expr(cg, n->as.binop.right);
    cg->binop_depth--;
    /* swap so xmm0 = left, xmm1 = right */
    e(cg, "movapd xmm1, xmm0");          /* xmm1 = right */
    if (slot >= 0) {
        ef(cg, "movapd xmm0, xmm%d", 6 + slot);    /* xmm0 = left (from reg) */
    } else {
        e(cg, "movsd  xmm0, [rsp]");
        e(cg, "add    rsp, 16");             /* xmm0 = left (from stack) */
    }

    switch (n->as.binop.op) {
        case TOK_PLUS:  e(cg, "addsd xmm0, xmm1"); return;
        case TOK_MINUS: e(cg, "subsd xmm0, xmm1"); return;
        case TOK_STAR:  e(cg, "mulsd xmm0, xmm1"); return;
        case TOK_SLASH: e(cg, "divsd xmm0, xmm1"); return;
        case TOK_PERCENT: {
            /* fmod-style modulo. CalcLang's VM uses fmod; we match. */
            /* Call libc fmod(a, b): MS x64 — xmm0=a, xmm1=b, result xmm0.
               Need 16-byte aligned rsp + 32 shadow space. At statement
               boundaries rsp is aligned; we're mid-expression here but
               our pushes balance, so rsp is still aligned now. */
            e(cg, "sub  rsp, 32");
            e(cg, "call fmod");
            e(cg, "add  rsp, 32");
            return;
        }
        case TOK_AMP:
        case TOK_PIPE:
        case TOK_CARET:
        case TOK_LSHIFT:
        case TOK_RSHIFT: {
            /* Bitwise: convert both doubles to int64, do the op, convert
               back. cvttsd2si truncates toward zero (matching the VM's
               (int64_t) cast). */
            e(cg, "cvttsd2si rax, xmm0");        /* left  -> rax */
            e(cg, "cvttsd2si rcx, xmm1");        /* right -> rcx */
            switch (n->as.binop.op) {
                case TOK_AMP:    e(cg, "and rax, rcx"); break;
                case TOK_PIPE:   e(cg, "or  rax, rcx"); break;
                case TOK_CARET:  e(cg, "xor rax, rcx"); break;
                case TOK_LSHIFT: e(cg, "shl rax, cl");  break;  /* shift count in cl */
                case TOK_RSHIFT: e(cg, "sar rax, cl");  break;  /* arithmetic; sign-ext */
                default: cl_die("unreachable");
            }
            e(cg, "cvtsi2sd xmm0, rax");
            return;
        }
        case TOK_EQEQ:
        case TOK_NEQ:
        case TOK_LT:
        case TOK_LE:
        case TOK_GT:
        case TOK_GE: {
            /* ucomisd xmm0, xmm1 — sets ZF/PF/CF like CMP but for
               doubles. Pick the right setcc, materialise 0.0/1.0. */
            e(cg, "ucomisd xmm0, xmm1");
            const char *setcc;
            switch (n->as.binop.op) {
                case TOK_EQEQ: setcc = "sete"; break;
                case TOK_NEQ:  setcc = "setne"; break;
                case TOK_LT:   setcc = "setb"; break;   /* CF=1 */
                case TOK_LE:   setcc = "setbe"; break;  /* CF=1 or ZF=1 */
                case TOK_GT:   setcc = "seta"; break;   /* CF=0 and ZF=0 */
                case TOK_GE:   setcc = "setae"; break;  /* CF=0 */
                default: cl_die("unreachable");
            }
            ef(cg, "%s al", setcc);
            e(cg, "movzx eax, al");
            e(cg, "cvtsi2sd xmm0, eax");
            return;
        }
        default:
            fprintf(stderr, "native codegen: unsupported binop %d\n", n->as.binop.op);
            exit(1);
    }
}

static void gen_unop(Cg *cg, AST *n) {
    if (n->as.unop.op == TOK_MINUS) {
        TInfer t = infer_type(cg, n->as.unop.operand);
        gen_expr(cg, n->as.unop.operand);
        if (t == TI_NUM) {
            /* Fast path: flip sign bit. */
            e(cg, "mov  rax, 0x8000000000000000");
            e(cg, "movq xmm1, rax");
            e(cg, "xorpd xmm0, xmm1");
        } else {
            /* Polymorphic: route through cl_op_neg so complex values
               get their components negated (the inline xorpd above
               would just flip a NaN's sign bit, leaving NaN). */
            e(cg, "movq rcx, xmm0");
            e(cg, "sub  rsp, 32");
            e(cg, "call cl_op_neg");
            e(cg, "add  rsp, 32");
            e(cg, "movq xmm0, rax");
        }
        return;
    }
    if (n->as.unop.op == TOK_BANG) {
        TInfer t = infer_type(cg, n->as.unop.operand);
        gen_expr(cg, n->as.unop.operand);
        emit_truthify_if_not_num(cg, t == TI_NUM);
        /* xmm0 is now a regular number (0.0 or 1.0 in the polymorphic
           case; arbitrary num in the fast path). !x is 1.0 iff x==0. */
        e(cg, "xorpd xmm1, xmm1");
        e(cg, "ucomisd xmm0, xmm1");
        e(cg, "sete al");
        e(cg, "setnp cl");
        e(cg, "and  al, cl");
        e(cg, "movzx eax, al");
        e(cg, "cvtsi2sd xmm0, eax");
        return;
    }
    if (n->as.unop.op == TOK_TILDE) {
        TInfer t = infer_type(cg, n->as.unop.operand);
        gen_expr(cg, n->as.unop.operand);
        if (t == TI_NUM) {
            e(cg, "cvttsd2si rax, xmm0");
            e(cg, "not rax");
            e(cg, "cvtsi2sd xmm0, rax");
        } else {
            e(cg, "movq rcx, xmm0");
            e(cg, "sub  rsp, 32");
            e(cg, "call cl_op_bnot");
            e(cg, "add  rsp, 32");
            e(cg, "movq xmm0, rax");
        }
        return;
    }
    cl_die("native codegen: unsupported unary op");
}

static void gen_call_intrinsic(Cg *cg, AST *n, const Intrinsic *in) {
    if (n->as.call.arg_count != in->arity) {
        fprintf(stderr,
            "native codegen: intrinsic '%s' takes %d arg%s, got %d\n",
            in->name, in->arity, in->arity == 1 ? "" : "s", n->as.call.arg_count);
        exit(1);
    }
    if (strcmp(in->name, "pi") == 0) { load_double_imm(cg, 3.141592653589793); return; }
    if (strcmp(in->name, "e")  == 0) { load_double_imm(cg, 2.718281828459045); return; }
    if (in->arity == 1) {
        gen_expr(cg, n->as.call.args[0]);
        if (strcmp(in->name, "sqrt") == 0) {
            e(cg, "sqrtsd xmm0, xmm0");
        } else if (strcmp(in->name, "abs") == 0) {
            /* Clear sign bit via AND with 0x7FFF... */
            e(cg, "mov  rax, 0x7fffffffffffffff");
            e(cg, "movq xmm1, rax");
            e(cg, "andpd xmm0, xmm1");
        } else if (strcmp(in->name, "floor") == 0) {
            e(cg, "roundsd xmm0, xmm0, 1");
        } else if (strcmp(in->name, "ceil") == 0) {
            e(cg, "roundsd xmm0, xmm0, 2");
        } else if (strcmp(in->name, "round") == 0) {
            /* round half away from zero — roundsd mode 0 (round-to-nearest-even)
               is the closest hardware option. For exact "half away from
               zero", do: copysign(floor(abs(x) + 0.5), x). Simpler: call
               libc round(). MS x64: xmm0 -> xmm0. */
            e(cg, "sub  rsp, 32");
            e(cg, "call round");
            e(cg, "add  rsp, 32");
        }
        return;
    }
    /* arity 2 — evaluate both args; for hardware-supported ops (min,
       max) inline, otherwise call libc. */
    gen_expr(cg, n->as.call.args[0]);
    push_xmm0(cg);
    gen_expr(cg, n->as.call.args[1]);
    e(cg, "movapd xmm1, xmm0");
    e(cg, "movsd  xmm0, [rsp]");
    e(cg, "add    rsp, 16");
    if (strcmp(in->name, "min") == 0) {
        e(cg, "minsd xmm0, xmm1");
    } else if (strcmp(in->name, "max") == 0) {
        e(cg, "maxsd xmm0, xmm1");
    } else if (strcmp(in->name, "pow") == 0) {
        /* libc pow(a, b): MS x64 — xmm0=a, xmm1=b, result xmm0. */
        e(cg, "sub  rsp, 32");
        e(cg, "call pow");
        e(cg, "add  rsp, 32");
    }
}

/* Indirect call: the callee value is already in xmm0 (a tagged
   Closure). Push args, untag the callee, set R10 = closure ptr, jump
   through closure->code. */
static void emit_indirect_call_with_callee_in_xmm0(Cg *cg, int n_args, AST **args) {
    /* Save callee in a 16-byte slot. */
    push_xmm0(cg);
    /* Push args right-to-left. */
    for (int i = n_args - 1; i >= 0; i--) {
        gen_expr(cg, args[i]);
        push_xmm0(cg);
    }
    /* Load callee value from below the args. */
    ef(cg, "mov  rax, [rsp + %d]", n_args * 16);
    /* Untag into r10 (closure ptr). */
    e(cg, "mov  r10, rax");
    e(cg, "mov  rax, 0x0000ffffffffffff");
    e(cg, "and  r10, rax");
    /* Load code pointer (first field). */
    e(cg, "mov  rax, [r10]");
    /* R10 is now the closure ptr — visible to the callee, who will
       save it to [rbp - 8] in its prologue. The `call` instruction
       below preserves R10 (it's a regular indirect call). */
    e(cg, "call rax");
    /* Clean up args + callee slot. */
    ef(cg, "add  rsp, %d", (n_args + 1) * 16);
    /* Result in xmm0 by the calc calling convention. */
}

static void gen_call(Cg *cg, AST *n) {
    if (n->as.call.callee) {
        gen_expr(cg, n->as.call.callee);
        emit_indirect_call_with_callee_in_xmm0(cg, n->as.call.arg_count, n->as.call.args);
        return;
    }
    const Intrinsic *in = find_intrinsic(n->as.call.name);
    if (in) {
        /* Use the inline-intrinsic fast path only when every argument
           is statically known to be num. If a complex (or anything
           else) might flow in, the inline bit-twiddling that some
           intrinsics use (notably `abs`'s sign-bit clear) would
           silently miscompute — fall through to the RT_BUILTIN path
           in that case, which does proper type dispatch. */
        int all_num = 1;
        for (int i = 0; i < n->as.call.arg_count; i++) {
            if (infer_type(cg, n->as.call.args[i]) != TI_NUM) {
                all_num = 0;
                break;
            }
        }
        if (all_num) {
            gen_call_intrinsic(cg, n, in);
            return;
        }
        /* fall through */
    }
    const RtBuiltin *rb = find_rt_builtin(n->as.call.name);
    if (rb) {
        if (n->as.call.arg_count != rb->arity) {
            fprintf(stderr,
                "native codegen: '%s' takes %d arg%s, got %d\n",
                rb->name, rb->arity, rb->arity == 1 ? "" : "s",
                n->as.call.arg_count);
            exit(1);
        }
        /* MS x64 calling convention: args go in RCX, RDX, R8 (up to 4).
           We support up to 3 args here (max in our table is str_slice
           with arity 3). Evaluate left-to-right into stack slots,
           then load into the int regs in order. */
        for (int i = 0; i < rb->arity; i++) {
            gen_expr(cg, n->as.call.args[i]);
            push_xmm0(cg);
        }
        /* Args were pushed L-to-R into 16-byte slots, so arg0 is at
           the deepest offset. Each arg slot is 16 bytes wide, but we
           only read the low 8 bytes. MS x64 ABI: first 4 args go in
           RCX/RDX/R8/R9 (integer slots) — and for Values we use the
           integer slots because a tagged Value isn't a float. */
        const char *int_regs[4] = {"rcx", "rdx", "r8", "r9"};
        if (rb->arity > 4) cl_die("runtime builtin > 4 args not supported");
        for (int i = 0; i < rb->arity; i++) {
            ef(cg, "mov  %s, [rsp + %d]", int_regs[i], (rb->arity - 1 - i) * 16);
        }
        ef(cg, "add  rsp, %d", rb->arity * 16);
        /* Maintain 16-byte alignment for the call. Before any args
           were pushed, rsp was 16-aligned (statement boundary). Each
           push is 8 bytes; we pushed arity, popped arity — back to
           16-aligned. Add 32 shadow space (multiple of 16). */
        e(cg, "sub  rsp, 32");
        ef(cg, "call %s", rb->rt_symbol);
        e(cg, "add  rsp, 32");
        e(cg, "movq xmm0, rax");
        return;
    }
    CgFn *f = lookup_fn(cg, n->as.call.name);
    if (!f) {
        /* No matching user fn — see if it's a local variable holding
           a Closure value (e.g. an arg of type fn, or `let g = ...`).
           Evaluate the variable, then dispatch indirectly. */
        const CgName *e_ = lookup_name(cg, n->as.call.name);
        if (e_) {
            AST var_node;
            memset(&var_node, 0, sizeof(var_node));
            var_node.kind = NODE_VAR;
            cl_strncpy_z(var_node.as.var, n->as.call.name, CL_MAX_TEXT);
            gen_expr(cg, &var_node);
            emit_indirect_call_with_callee_in_xmm0(cg, n->as.call.arg_count, n->as.call.args);
            return;
        }
        fprintf(stderr, "native codegen: undefined function '%s' (or calclib symbol not supported in native mode)\n",
            n->as.call.name);
        exit(1);
    }
    if (n->as.call.arg_count != f->param_count) {
        fprintf(stderr,
            "native codegen: function '%s' takes %d arg%s, got %d\n",
            f->name, f->param_count,
            f->param_count == 1 ? "" : "s", n->as.call.arg_count);
        exit(1);
    }

    /* Compile-time per-arg type check. Anything statically knowable
       gets verified here; ambiguous args (e.g. unresolved variables)
       defer to the function-entry TYPECHECK at runtime. Mirrors the
       VM-side check in codegen.c so both pipelines reject the same
       programs. */
    for (int i = 0; i < n->as.call.arg_count; i++) {
        TypeAnnot expected = f->param_types[i];
        TypeAnnot actual;
        if (expected != TYPE_ANY
            && x64_static_arg_type(cg, n->as.call.args[i], &actual)
            && !x64_type_accepts(expected, actual)) {
            fprintf(stderr,
                "semantic error: argument %d to '%s': expected %s, got %s\n",
                i + 1, f->name,
                type_annot_name(expected),
                type_annot_name(actual));
            exit(1);
        }
    }

    /* Evaluate args left-to-right but push in reverse order so arg0
       ends up at [rbp+16] in the callee. Easiest: gen each arg in
       order, save to slot above the eventual call frame. Use a temp
       area of (arg_count * 8) bytes; once all evaluated, push from
       last to first.
       Simpler implementation: evaluate args right-to-left and push as
       we go. Side effects fire right-to-left, which is fine since the
       language doesn't promise order. */
    int n_args = n->as.call.arg_count;
    /* Each arg goes into a 16-byte slot via push_xmm0. Total bytes
       consumed = n_args * 16, always a multiple of 16, so we don't
       need a separate alignment pad. */
    for (int i = n_args - 1; i >= 0; i--) {
        gen_expr(cg, n->as.call.args[i]);
        push_xmm0(cg);
    }
    ef(cg, "call calc_%s", f->name);
    if (n_args > 0) ef(cg, "add  rsp, %d", n_args * 16);
}

static void gen_expr(Cg *cg, AST *n) {
    switch (n->kind) {
        case NODE_NUMBER:
            load_double_imm(cg, n->as.number);
            return;
        case NODE_STRING: {
            int id = intern_string(cg, n->as.string, (int)strlen(n->as.string));
            load_string_imm(cg, id);
            return;
        }
        case NODE_VAR: {
            const CgName *nm = lookup_name(cg, n->as.var);
            if (nm) {
                switch (nm->kind) {
                    case CG_NK_PARAM:
                    case CG_NK_LOCAL:
                        ef(cg, "movsd xmm0, [rbp %s %d]",
                            nm->offset < 0 ? "-" : "+",
                            nm->offset < 0 ? -nm->offset : nm->offset);
                        return;
                    case CG_NK_LOCAL_BOXED:
                        ef(cg, "mov  rax, [rbp %s %d]",
                            nm->offset < 0 ? "-" : "+",
                            nm->offset < 0 ? -nm->offset : nm->offset);
                        e(cg, "movsd xmm0, [rax]");
                        return;
                    case CG_NK_UPVAL:
                        /* closure ptr at [rbp - 8]; ->upvals at +16;
                           upvals[i] is a Value*; deref for value. */
                        e(cg, "mov  rax, [rbp - 8]");
                        e(cg, "mov  rax, [rax + 16]");
                        ef(cg, "mov  rax, [rax + %d]", nm->offset * 8);
                        e(cg, "movsd xmm0, [rax]");
                        return;
                }
            }
            /* Not a local — maybe a top-level user fn used as a
               first-class value. Wrap its code pointer in a Closure
               with 0 upvals and return the tagged value. */
            if (lookup_fn(cg, n->as.var)) {
                ef(cg, "lea  rcx, [rip + calc_%s]", n->as.var);
                e(cg, "mov  rdx, 0");
                e(cg, "sub  rsp, 32");
                e(cg, "call cl_new_closure");
                e(cg, "add  rsp, 32");
                e(cg, "movq xmm0, rax");
                return;
            }
            /* Or a calclib builtin. Emit a trampoline reference; the
               trampoline itself is generated once per unique name at
               the end of the program. */
            if (find_rt_builtin(n->as.var)) {
                int already = 0;
                for (int i = 0; i < cg->trampoline_count; i++) {
                    if (strcmp(cg->trampolines[i], n->as.var) == 0) { already = 1; break; }
                }
                if (!already) {
                    if (cg->trampoline_count >= 64) cl_die("too many builtin trampolines");
                    cl_strncpy_z(cg->trampolines[cg->trampoline_count++],
                                 n->as.var, CL_MAX_TEXT);
                }
                ef(cg, "lea  rcx, [rip + .Lbi_%s]", n->as.var);
                e(cg, "mov  rdx, 0");
                e(cg, "sub  rsp, 32");
                e(cg, "call cl_new_closure");
                e(cg, "add  rsp, 32");
                e(cg, "movq xmm0, rax");
                return;
            }
            fprintf(stderr, "native codegen: undefined variable '%s'\n", n->as.var);
            exit(1);
        }
        case NODE_BINOP:   gen_binop(cg, n);   return;
        case NODE_UNOP:    gen_unop(cg, n);    return;
        case NODE_CALL:    gen_call(cg, n);    return;
        case NODE_TERNARY: gen_ternary(cg, n); return;
        case NODE_ARRAY_LIT: {
            /* `[a, b, c]` -> cl_new_arr(); for each item, push. The
               array pointer lives in a 16-byte stack slot between
               pushes so item expressions can recurse into calls. */
            e(cg, "sub  rsp, 32");
            e(cg, "call cl_new_arr");
            e(cg, "add  rsp, 32");
            e(cg, "movq xmm0, rax");
            push_xmm0(cg);                      /* save arr in 16-byte slot */
            for (int i = 0; i < n->as.array_lit.count; i++) {
                gen_expr(cg, n->as.array_lit.items[i]);
                /* cl_arr_push(arr, val): rcx=arr, rdx=val. arr is in
                   [rsp + 16] (the next 16-byte slot up from us — the
                   current xmm0 has the item, no extra push needed yet). */
                e(cg, "movq rdx, xmm0");
                e(cg, "mov  rcx, [rsp]");       /* re-read arr */
                e(cg, "sub  rsp, 32");
                e(cg, "call cl_arr_push");
                e(cg, "add  rsp, 32");
                /* push returns new length; we ignore it. arr is still
                   on the stack, untouched. */
            }
            /* Pop the array back into xmm0 as the expression result. */
            e(cg, "movsd xmm0, [rsp]");
            e(cg, "add  rsp, 16");
            return;
        }
        case NODE_INDEX: {
            /* cl_index_get(coll, idx). coll in rcx, idx in rdx. */
            gen_expr(cg, n->as.index.target);
            push_xmm0(cg);
            gen_expr(cg, n->as.index.index);
            e(cg, "movq rdx, xmm0");
            e(cg, "mov  rcx, [rsp]");
            e(cg, "add  rsp, 16");
            e(cg, "sub  rsp, 32");
            e(cg, "call cl_index_get");
            e(cg, "add  rsp, 32");
            e(cg, "movq xmm0, rax");
            return;
        }
        case NODE_MAP_LIT: {
            /* `{k1: v1, k2: v2}` -> cl_new_map(); cl_index_set per
               entry. Map ref lives in a 16-byte slot between
               iterations so the key/value expressions can recurse. */
            e(cg, "sub  rsp, 32");
            e(cg, "call cl_new_map");
            e(cg, "add  rsp, 32");
            e(cg, "movq xmm0, rax");
            push_xmm0(cg);                          /* save map ref */
            for (int i = 0; i < n->as.map_lit.count; i++) {
                gen_expr(cg, n->as.map_lit.keys[i]);
                push_xmm0(cg);                      /* key */
                gen_expr(cg, n->as.map_lit.values[i]);
                /* Stack now: [rsp]=value-slot(empty)? No — we've just
                   computed v in xmm0; key is at [rsp], map at [rsp+16]. */
                e(cg, "movq r8, xmm0");             /* value */
                e(cg, "mov  rdx, [rsp]");           /* key */
                e(cg, "mov  rcx, [rsp + 16]");      /* map */
                e(cg, "add  rsp, 16");              /* drop key slot; map still saved */
                e(cg, "sub  rsp, 32");
                e(cg, "call cl_index_set");
                e(cg, "add  rsp, 32");
            }
            e(cg, "movsd xmm0, [rsp]");
            e(cg, "add  rsp, 16");
            return;
        }
        case NODE_FN: {
            /* Inline closure expression. Walk its body to find which
               OUTER-scope names it captures (free vars that resolve
               in cg->names but aren't bound inside this fn). Register
               a CgClosure with a fresh label; emit the construction
               site here, defer body emission to end-of-program.

               Shadowing approximation: a name appearing in this fn's
               body is treated as captured if it's in the outer scope
               and not in this fn's *param* list. Inner let-shadowing
               isn't tracked — fine for the test surface. */
            AST *fn_node = n;
            if (cg->closure_count >= CG_MAX_CLOSURES) cl_die("too many closures");
            CgClosure *cc = &cg->closures[cg->closure_count++];
            cc->label_id    = cg->next_closure_label++;
            cc->fn_node     = fn_node;
            cc->upval_count = 0;
            for (int i = 0; i < cg->name_count; i++) {
                const char *nm = cg->names[i].name;
                int already = 0;
                for (int j = 0; j < cc->upval_count; j++) {
                    if (strcmp(cc->upval_names[j], nm) == 0) { already = 1; break; }
                }
                if (already) continue;
                int inner_param = 0;
                for (int k = 0; k < fn_node->as.fn_def.param_count; k++) {
                    if (strcmp(fn_node->as.fn_def.params[k], nm) == 0) {
                        inner_param = 1; break;
                    }
                }
                if (inner_param) continue;
                if (ast_uses_name_recur(fn_node->as.fn_def.body, nm, 1)) {
                    if (cc->upval_count >= CG_MAX_UPVALS_PER) cl_die("too many upvalues");
                    cl_strncpy_z(cc->upval_names[cc->upval_count++], nm, CL_MAX_TEXT);
                }
            }
            /* Construction: cl_new_closure(code, n_upvals); then fill
               upvals from outer scope. To write into the struct we
               need the raw pointer (mask off tag bits). We keep the
               tagged Value on the stack so we can return it. */
            ef(cg, "lea  rcx, [rip + .Lclosure_%d]", cc->label_id);
            ef(cg, "mov  rdx, %d", cc->upval_count);
            e(cg, "sub  rsp, 32");
            e(cg, "call cl_new_closure");
            e(cg, "add  rsp, 32");
            /* Save tagged value (rax) in a 16-byte slot. */
            e(cg, "sub  rsp, 16");
            e(cg, "mov  [rsp], rax");
            /* Untag into r11; load upvals[] pointer into r10. */
            e(cg, "mov  r11, rax");
            e(cg, "mov  rcx, 0x0000ffffffffffff");
            e(cg, "and  r11, rcx");
            e(cg, "mov  r10, [r11 + 16]");
            for (int i = 0; i < cc->upval_count; i++) {
                const CgName *src = lookup_name(cg, cc->upval_names[i]);
                if (!src) cl_die("upval source not found");
                if (src->kind == CG_NK_LOCAL_BOXED) {
                    ef(cg, "mov  rax, [rbp %s %d]",
                        src->offset < 0 ? "-" : "+",
                        src->offset < 0 ? -src->offset : src->offset);
                } else if (src->kind == CG_NK_UPVAL) {
                    e(cg, "mov  rax, [rbp - 8]");
                    e(cg, "mov  rax, [rax + 16]");
                    ef(cg, "mov  rax, [rax + %d]", src->offset * 8);
                } else {
                    fprintf(stderr,
                        "native codegen: upvalue '%s' not boxed in outer scope\n",
                        cc->upval_names[i]);
                    exit(1);
                }
                ef(cg, "mov  [r10 + %d], rax", i * 8);
            }
            /* Reload tagged Value into xmm0. */
            e(cg, "movsd xmm0, [rsp]");
            e(cg, "add  rsp, 16");
            return;
        }
        default:
            fprintf(stderr, "native codegen: unsupported expression node %d\n", n->kind);
            exit(1);
    }
}

/* --- Statement generation ----------------------------------------- */

static void gen_print(Cg *cg, AST *n) {
    TInfer t = infer_type(cg, n->as.print_stmt.expr);
    gen_expr(cg, n->as.print_stmt.expr);
    if (t == TI_NUM) {
        /* Fast path: known number. Inline printf("%.10g\n", xmm0). */
        e(cg, "lea  rcx, [rip + .LCfmt]");
        e(cg, "movapd xmm1, xmm0");
        e(cg, "movq rdx, xmm0");
        e(cg, "sub  rsp, 32");
        e(cg, "call printf");
        e(cg, "add  rsp, 32");
        return;
    }
    /* Polymorphic path: hand the Value to the runtime, which checks
       the tag and prints appropriately. */
    e(cg, "movq rcx, xmm0");
    e(cg, "sub  rsp, 32");
    e(cg, "call cl_print");
    e(cg, "add  rsp, 32");
}

static void gen_if(Cg *cg, AST *n) {
    int L_else = fresh_label(cg);
    int L_end  = fresh_label(cg);
    TInfer t = infer_type(cg, n->as.if_stmt.cond);
    gen_expr(cg, n->as.if_stmt.cond);
    emit_truthify_if_not_num(cg, t == TI_NUM);
    e(cg, "xorpd xmm1, xmm1");
    e(cg, "ucomisd xmm0, xmm1");
    ef(cg, "jp   .L%d", L_else);   /* NaN -> falsy here */
    ef(cg, "je   .L%d", L_else);
    gen_stmt(cg, n->as.if_stmt.then_branch);
    ef(cg, "jmp  .L%d", L_end);
    label(cg, L_else);
    if (n->as.if_stmt.else_branch) gen_stmt(cg, n->as.if_stmt.else_branch);
    label(cg, L_end);
}

static void gen_while(Cg *cg, AST *n) {
    int L_top  = fresh_label(cg);
    int L_end  = fresh_label(cg);
    int saved_break = cg->break_label;
    int saved_cont  = cg->cont_label;
    cg->break_label = L_end;
    cg->cont_label  = L_top;

    label(cg, L_top);
    {
        TInfer t = infer_type(cg, n->as.while_stmt.cond);
        gen_expr(cg, n->as.while_stmt.cond);
        emit_truthify_if_not_num(cg, t == TI_NUM);
    }
    e(cg, "xorpd xmm1, xmm1");
    e(cg, "ucomisd xmm0, xmm1");
    ef(cg, "jp   .L%d", L_end);
    ef(cg, "je   .L%d", L_end);
    gen_stmt(cg, n->as.while_stmt.body);
    ef(cg, "jmp  .L%d", L_top);
    label(cg, L_end);

    cg->break_label = saved_break;
    cg->cont_label  = saved_cont;
}

static void gen_do_while(Cg *cg, AST *n) {
    int L_top  = fresh_label(cg);
    int L_cont = fresh_label(cg);
    int L_end  = fresh_label(cg);
    int saved_break = cg->break_label;
    int saved_cont  = cg->cont_label;
    cg->break_label = L_end;
    cg->cont_label  = L_cont;

    label(cg, L_top);
    gen_stmt(cg, n->as.while_stmt.body);
    label(cg, L_cont);
    {
        TInfer t = infer_type(cg, n->as.while_stmt.cond);
        gen_expr(cg, n->as.while_stmt.cond);
        emit_truthify_if_not_num(cg, t == TI_NUM);
    }
    e(cg, "xorpd xmm1, xmm1");
    e(cg, "ucomisd xmm0, xmm1");
    ef(cg, "jp   .L%d", L_end);
    ef(cg, "jne  .L%d", L_top);
    label(cg, L_end);

    cg->break_label = saved_break;
    cg->cont_label  = saved_cont;
}

static void gen_ternary(Cg *cg, AST *n) {
    int L_else = fresh_label(cg);
    int L_end  = fresh_label(cg);
    {
        TInfer t = infer_type(cg, n->as.ternary.cond);
        gen_expr(cg, n->as.ternary.cond);
        emit_truthify_if_not_num(cg, t == TI_NUM);
    }
    e(cg, "xorpd xmm1, xmm1");
    e(cg, "ucomisd xmm0, xmm1");
    ef(cg, "jp   .L%d", L_else);
    ef(cg, "je   .L%d", L_else);
    gen_expr(cg, n->as.ternary.then_expr);
    ef(cg, "jmp  .L%d", L_end);
    label(cg, L_else);
    gen_expr(cg, n->as.ternary.else_expr);
    label(cg, L_end);
}

static void gen_for(Cg *cg, AST *n) {
    /* for (init; cond; step) body
       Implemented as: { init; while (cond) { body; step; } }
       The init scope is its own block so `let i: T = ...` stays local. */
    scope_push(cg);
    int saved_local = cg->local_bytes;
    if (n->as.for_stmt.init) gen_stmt(cg, n->as.for_stmt.init);

    int L_top  = fresh_label(cg);
    int L_step = fresh_label(cg);
    int L_end  = fresh_label(cg);
    int saved_break = cg->break_label;
    int saved_cont  = cg->cont_label;
    cg->break_label = L_end;
    cg->cont_label  = L_step;

    label(cg, L_top);
    if (n->as.for_stmt.cond) {
        TInfer t = infer_type(cg, n->as.for_stmt.cond);
        gen_expr(cg, n->as.for_stmt.cond);
        emit_truthify_if_not_num(cg, t == TI_NUM);
        e(cg, "xorpd xmm1, xmm1");
        e(cg, "ucomisd xmm0, xmm1");
        ef(cg, "jp   .L%d", L_end);
        ef(cg, "je   .L%d", L_end);
    }
    gen_stmt(cg, n->as.for_stmt.body);
    label(cg, L_step);
    if (n->as.for_stmt.step) gen_stmt(cg, n->as.for_stmt.step);
    ef(cg, "jmp  .L%d", L_top);
    label(cg, L_end);

    cg->break_label = saved_break;
    cg->cont_label  = saved_cont;
    cg->local_bytes = saved_local;
    scope_pop(cg);
}

static void gen_stmt(Cg *cg, AST *n) {
    switch (n->kind) {
        case NODE_LET: {
            gen_expr(cg, n->as.let_stmt.expr);
            CgNameKind kind;
            int off = add_local(cg, n->as.let_stmt.name, &kind);
            if (kind == CG_NK_LOCAL_BOXED) {
                /* cl_box_new(initial): rcx=initial, returns Value*
                   in rax. Store that pointer at the local slot. */
                e(cg, "movq rcx, xmm0");
                e(cg, "sub  rsp, 32");
                e(cg, "call cl_box_new");
                e(cg, "add  rsp, 32");
                ef(cg, "mov  [rbp - %d], rax", -off);
            } else {
                ef(cg, "movsd [rbp - %d], xmm0", -off);
            }
            return;
        }
        case NODE_ASSIGN: {
            const CgName *nm = lookup_name(cg, n->as.assign_stmt.name);
            if (!nm) {
                fprintf(stderr, "native codegen: assign to undefined variable '%s'\n",
                    n->as.assign_stmt.name);
                exit(1);
            }
            gen_expr(cg, n->as.assign_stmt.expr);
            switch (nm->kind) {
                case CG_NK_PARAM:
                case CG_NK_LOCAL:
                    ef(cg, "movsd [rbp %s %d], xmm0",
                        nm->offset < 0 ? "-" : "+",
                        nm->offset < 0 ? -nm->offset : nm->offset);
                    return;
                case CG_NK_LOCAL_BOXED:
                    ef(cg, "mov  rax, [rbp %s %d]",
                        nm->offset < 0 ? "-" : "+",
                        nm->offset < 0 ? -nm->offset : nm->offset);
                    e(cg, "movsd [rax], xmm0");
                    return;
                case CG_NK_UPVAL:
                    e(cg, "mov  rax, [rbp - 8]");
                    e(cg, "mov  rax, [rax + 16]");
                    ef(cg, "mov  rax, [rax + %d]", nm->offset * 8);
                    e(cg, "movsd [rax], xmm0");
                    return;
            }
            return;
        }
        case NODE_PRINT:    gen_print(cg, n);    return;
        case NODE_IF:       gen_if(cg, n);       return;
        case NODE_WHILE:    gen_while(cg, n);    return;
        case NODE_DO_WHILE: gen_do_while(cg, n); return;
        case NODE_FOR:      gen_for(cg, n);      return;
        case NODE_SWITCH: {
            /* Evaluate discriminant, save to a stack slot. For each case,
               compare against the case value via cl_op_eq (polymorphic —
               so numbers and strings both work as labels). On match, jump
               to the case body; cases run sequentially and each falls
               through to the switch end (no C-style fall-through).
               `break` inside a case body jumps to the switch end too. */
            int n_cases = n->as.switch_stmt.case_count;
            int has_default = (n->as.switch_stmt.default_body != NULL);
            int L_end = fresh_label(cg);
            int *L_case = (int *)cl_track_malloc(sizeof(int) * (size_t)(n_cases > 0 ? n_cases : 1));
            for (int i = 0; i < n_cases; i++) L_case[i] = fresh_label(cg);
            int L_default = has_default ? fresh_label(cg) : L_end;

            /* Evaluate discriminant; push onto the stack (16-byte slot). */
            gen_expr(cg, n->as.switch_stmt.discriminant);
            push_xmm0(cg);

            /* Dispatch: for each case, reload disc, eval case value,
               compare, jump if equal. */
            for (int i = 0; i < n_cases; i++) {
                /* xmm0 = case value */
                gen_expr(cg, n->as.switch_stmt.case_values[i]);
                /* rdx = xmm0 (case value), rcx = [rsp] (discriminant) */
                e(cg, "movq rdx, xmm0");
                e(cg, "mov  rcx, [rsp]");
                e(cg, "sub  rsp, 32");
                e(cg, "call cl_op_eq");
                e(cg, "add  rsp, 32");
                /* rax = num 1.0 if equal, num 0.0 otherwise. */
                e(cg, "movq xmm0, rax");
                e(cg, "xorpd xmm1, xmm1");
                e(cg, "ucomisd xmm0, xmm1");
                ef(cg, "jp   .L%d_skip%d", L_end, i);   /* PF set → NaN → skip */
                ef(cg, "jne  .L%d", L_case[i]);
                ef(cg, ".L%d_skip%d:", L_end, i);
            }
            /* No case matched — go to default (or end). */
            ef(cg, "jmp  .L%d", L_default);

            /* Each case body. Use break_label so `break` works inside. */
            int saved_break = cg->break_label;
            cg->break_label = L_end;
            for (int i = 0; i < n_cases; i++) {
                label(cg, L_case[i]);
                gen_stmt(cg, n->as.switch_stmt.case_bodies[i]);
                ef(cg, "jmp  .L%d", L_end);
            }
            if (has_default) {
                label(cg, L_default);
                gen_stmt(cg, n->as.switch_stmt.default_body);
                /* fall through to L_end */
            }
            cg->break_label = saved_break;
            label(cg, L_end);
            /* Pop the discriminant slot. */
            e(cg, "add rsp, 16");
            return;
        }
        case NODE_BREAK:
            if (cg->break_label < 0) cl_die("`break` outside loop");
            ef(cg, "jmp  .L%d", cg->break_label);
            return;
        case NODE_CONTINUE:
            if (cg->cont_label < 0) cl_die("`continue` outside loop");
            ef(cg, "jmp  .L%d", cg->cont_label);
            return;
        case NODE_BLOCK: {
            scope_push(cg);
            int saved = cg->local_bytes;
            for (int i = 0; i < n->as.block.count; i++)
                gen_stmt(cg, n->as.block.stmts[i]);
            cg->local_bytes = saved;
            scope_pop(cg);
            return;
        }
        case NODE_RETURN: {
            /* Tail-call optimization (TCO). `return foo(args);` where
               foo IS the function we're currently emitting becomes a
               jump back to the body-start label after writing the new
               args into the parameter slots. This turns self-tail-
               recursion into a loop, so unbounded depth doesn't blow
               the native stack.

               Conditions: direct call (no callee expression), name
               matches `cg->cur_fn_name`, arg count matches param
               count, AND none of the params are captured by an inner
               closure (a captured param needs its box reseated; for
               now we leave that as a normal call). */
            AST *e_ = n->as.return_stmt.expr;
            if (e_ && e_->kind == NODE_CALL
                && !e_->as.call.callee
                && strcmp(e_->as.call.name, cg->cur_fn_name) == 0) {
                CgFn *self = lookup_fn(cg, e_->as.call.name);
                if (self && self->param_count == e_->as.call.arg_count) {
                    int any_boxed = 0;
                    for (int i = 0; i < cg->name_count; i++) {
                        if (cg->names[i].kind == CG_NK_LOCAL_BOXED) {
                            /* Is this name one of the params? */
                            for (int j = 0; j < self->param_count; j++) {
                                /* lookup_fn doesn't store param names,
                                   but we tracked them via add_param;
                                   any boxed-LOCAL whose offset corresponds
                                   to a param's CG_NK_PARAM is a boxed
                                   param. Simpler check: any boxed local
                                   present at all conservatively means
                                   "skip TCO" — the function may carry
                                   state that the next iteration must
                                   re-box. */
                                (void)j;
                                any_boxed = 1;
                                break;
                            }
                            if (any_boxed) break;
                        }
                    }
                    if (!any_boxed) {
                        int n_args = e_->as.call.arg_count;
                        /* Evaluate args into stack slots first. */
                        for (int i = 0; i < n_args; i++) {
                            gen_expr(cg, e_->as.call.args[i]);
                            push_xmm0(cg);
                        }
                        /* Copy each from the temp slot to the param
                           slot. Pushed L-to-R so arg i lives at
                           [rsp + (n_args - 1 - i)*16]. */
                        for (int i = 0; i < n_args; i++) {
                            ef(cg, "mov  rax, [rsp + %d]", (n_args - 1 - i) * 16);
                            ef(cg, "mov  [rbp + %d], rax", 16 + i * 16);
                        }
                        if (n_args > 0) ef(cg, "add  rsp, %d", n_args * 16);
                        ef(cg, "jmp  .Lbody_%s", cg->cur_fn_name);
                        return;
                    }
                }
            }
            if (n->as.return_stmt.expr) gen_expr(cg, n->as.return_stmt.expr);
            else load_double_imm(cg, 0.0);
            ef(cg, "jmp  .Lret_%s", cg->cur_fn_name);
            return;
        }
        case NODE_CALL:
            /* Call as statement. Result in xmm0; discarded. */
            gen_expr(cg, n);
            return;
        case NODE_INDEX_ASSIGN: {
            /* cl_index_set(target, idx, value): rcx, rdx, r8. Evaluate
               left to right, parking each result in a 16-byte stack slot,
               then load the three argument registers and call. */
            gen_expr(cg, n->as.index_assign.target);
            push_xmm0(cg);
            gen_expr(cg, n->as.index_assign.index);
            push_xmm0(cg);
            gen_expr(cg, n->as.index_assign.value);
            e(cg, "movq r8, xmm0");
            e(cg, "mov  rdx, [rsp]");
            e(cg, "mov  rcx, [rsp + 16]");
            e(cg, "add  rsp, 32");
            e(cg, "sub  rsp, 32");
            e(cg, "call cl_index_set");
            e(cg, "add  rsp, 32");
            return;
        }
        case NODE_INDEX_OPASSIGN: {
            /* `target[idx] op= value` -> evaluate target, idx, then
               read current via cl_index_get, apply op, write back via
               cl_index_set. We re-evaluate target/idx for each access
               (acceptable: codegen consistency wins over a small extra
               call). For NaN-boxed operands the op needs to dispatch
               by tag — we let cl_op_plus handle the common case and
               assume numeric for the others (matches the VM's
               behavior on '+= on a numeric slot' and falls back to a
               clean runtime error if the types disagree). */
            gen_expr(cg, n->as.index_opassign.target);
            push_xmm0(cg);
            gen_expr(cg, n->as.index_opassign.index);
            push_xmm0(cg);
            /* Now stack has: [rsp]   = idx, [rsp+16] = target.
               Read current value: cl_index_get(target, idx). */
            e(cg, "mov  rcx, [rsp + 16]");
            e(cg, "mov  rdx, [rsp]");
            e(cg, "sub  rsp, 32");
            e(cg, "call cl_index_get");
            e(cg, "add  rsp, 32");
            e(cg, "movq xmm0, rax");
            push_xmm0(cg);                       /* old value in 16-byte slot */
            gen_expr(cg, n->as.index_opassign.value);
            /* Combine old (left) and new (right). */
            if (n->as.index_opassign.op == TOK_PLUS) {
                /* Use cl_op_plus to handle both numeric and string concat. */
                e(cg, "movq rdx, xmm0");
                e(cg, "mov  rcx, [rsp]");
                e(cg, "add  rsp, 16");
                e(cg, "sub  rsp, 32");
                e(cg, "call cl_op_plus");
                e(cg, "add  rsp, 32");
                e(cg, "movq xmm0, rax");
            } else {
                /* Numeric op. xmm0 = right, [rsp] = left. */
                e(cg, "movapd xmm1, xmm0");
                e(cg, "movsd  xmm0, [rsp]");
                e(cg, "add    rsp, 16");
                switch (n->as.index_opassign.op) {
                    case TOK_MINUS:   e(cg, "subsd xmm0, xmm1"); break;
                    case TOK_STAR:    e(cg, "mulsd xmm0, xmm1"); break;
                    case TOK_SLASH:   e(cg, "divsd xmm0, xmm1"); break;
                    case TOK_PERCENT:
                        e(cg, "sub  rsp, 32");
                        e(cg, "call fmod");
                        e(cg, "add  rsp, 32");
                        break;
                    case TOK_AMP:
                    case TOK_PIPE:
                    case TOK_CARET:
                    case TOK_LSHIFT:
                    case TOK_RSHIFT:
                        e(cg, "cvttsd2si rax, xmm0");
                        e(cg, "cvttsd2si rcx, xmm1");
                        switch (n->as.index_opassign.op) {
                            case TOK_AMP:    e(cg, "and rax, rcx"); break;
                            case TOK_PIPE:   e(cg, "or  rax, rcx"); break;
                            case TOK_CARET:  e(cg, "xor rax, rcx"); break;
                            case TOK_LSHIFT: e(cg, "shl rax, cl");  break;
                            case TOK_RSHIFT: e(cg, "sar rax, cl");  break;
                            default: cl_die("unreachable");
                        }
                        e(cg, "cvtsi2sd xmm0, rax");
                        break;
                    default:
                        cl_die("unknown index-opassign op");
                }
            }
            /* Now xmm0 = new value, stack has [rsp]=idx, [rsp+16]=target. */
            e(cg, "movq r8, xmm0");
            e(cg, "mov  rdx, [rsp]");
            e(cg, "mov  rcx, [rsp + 16]");
            e(cg, "add  rsp, 32");
            e(cg, "sub  rsp, 32");
            e(cg, "call cl_index_set");
            e(cg, "add  rsp, 32");
            return;
        }
        case NODE_THROW: {
            /* Evaluate the value, hand it to cl_throw — which never
               returns; it unwinds to the topmost handler's catch label
               or, if no handler is active, prints and exits. */
            gen_expr(cg, n->as.throw_stmt.expr);
            e(cg, "movq rcx, xmm0");
            e(cg, "sub  rsp, 32");
            e(cg, "call cl_throw");
            /* No `add rsp, 32` / no fallthrough — cl_throw is _Noreturn,
               but the assembler won't complain about dead code here. */
            return;
        }
        case NODE_TRY: {
            int L_catch = fresh_label(cg);
            int L_end   = fresh_label(cg);
            /* Push handler: rsp BEFORE the shadow sub, rbp, catch addr.
               We snapshot rsp directly here so that, after a throw
               restores it, we're back at exactly this position. */
            e(cg,  "mov  rcx, rsp");
            e(cg,  "mov  rdx, rbp");
            ef(cg, "lea  r8,  [rip + .L%d]", L_catch);
            e(cg,  "sub  rsp, 32");
            e(cg,  "call cl_try_push");
            e(cg,  "add  rsp, 32");
            /* Protected body. */
            scope_push(cg);
            int saved_local = cg->local_bytes;
            gen_stmt(cg, n->as.try_stmt.try_body);
            cg->local_bytes = saved_local;
            scope_pop(cg);
            /* Normal exit: pop the handler and skip the catch block. */
            e(cg,  "sub  rsp, 32");
            e(cg,  "call cl_try_pop");
            e(cg,  "add  rsp, 32");
            ef(cg, "jmp  .L%d", L_end);
            /* Catch entry: cl_throw restored rsp/rbp to the snapshot,
               popped its own handler, and jumped here. Bind the
               exception value to the catch param as a fresh local. */
            label(cg, L_catch);
            e(cg,  "sub  rsp, 32");
            e(cg,  "call cl_get_exception");
            e(cg,  "add  rsp, 32");
            e(cg,  "movq xmm0, rax");
            scope_push(cg);
            saved_local = cg->local_bytes;
            CgNameKind kind;
            int off = add_local(cg, n->as.try_stmt.catch_name, &kind);
            if (kind == CG_NK_LOCAL_BOXED) {
                e(cg, "movq rcx, xmm0");
                e(cg, "sub  rsp, 32");
                e(cg, "call cl_box_new");
                e(cg, "add  rsp, 32");
                ef(cg, "mov  [rbp - %d], rax", -off);
            } else {
                ef(cg, "movsd [rbp - %d], xmm0", -off);
            }
            gen_stmt(cg, n->as.try_stmt.catch_body);
            cg->local_bytes = saved_local;
            scope_pop(cg);
            label(cg, L_end);
            return;
        }
        case NODE_FN:
            fprintf(stderr, "native codegen: nested functions not supported in this backend\n");
            exit(1);
        default:
            fprintf(stderr, "native codegen: unsupported statement node %d\n", n->kind);
            exit(1);
    }
}

/* --- Capture / boxing analysis ----------------------------------- */

/* Returns 1 if `name` appears as a NODE_VAR inside `n` (or anywhere
   in its descendants), but treats NODE_FN as opaque only when
   `descend_into_fns` is 0. */
static int ast_uses_name_recur(AST *n, const char *name, int descend_into_fns) {
    if (!n) return 0;
    switch (n->kind) {
        case NODE_VAR: return strcmp(n->as.var, name) == 0;
        case NODE_FN:
            if (!descend_into_fns) return 0;
            return ast_uses_name_recur(n->as.fn_def.body, name, 1);
        case NODE_BINOP:
            return ast_uses_name_recur(n->as.binop.left,  name, descend_into_fns)
                || ast_uses_name_recur(n->as.binop.right, name, descend_into_fns);
        case NODE_UNOP:
            return ast_uses_name_recur(n->as.unop.operand, name, descend_into_fns);
        case NODE_LET:
            return ast_uses_name_recur(n->as.let_stmt.expr, name, descend_into_fns);
        case NODE_ASSIGN:
            return strcmp(n->as.assign_stmt.name, name) == 0
                || ast_uses_name_recur(n->as.assign_stmt.expr, name, descend_into_fns);
        case NODE_PRINT:
            return ast_uses_name_recur(n->as.print_stmt.expr, name, descend_into_fns);
        case NODE_IF:
            return ast_uses_name_recur(n->as.if_stmt.cond, name, descend_into_fns)
                || ast_uses_name_recur(n->as.if_stmt.then_branch, name, descend_into_fns)
                || ast_uses_name_recur(n->as.if_stmt.else_branch, name, descend_into_fns);
        case NODE_WHILE:
            return ast_uses_name_recur(n->as.while_stmt.cond, name, descend_into_fns)
                || ast_uses_name_recur(n->as.while_stmt.body, name, descend_into_fns);
        case NODE_FOR:
            return ast_uses_name_recur(n->as.for_stmt.init, name, descend_into_fns)
                || ast_uses_name_recur(n->as.for_stmt.cond, name, descend_into_fns)
                || ast_uses_name_recur(n->as.for_stmt.step, name, descend_into_fns)
                || ast_uses_name_recur(n->as.for_stmt.body, name, descend_into_fns);
        case NODE_BLOCK: {
            for (int i = 0; i < n->as.block.count; i++)
                if (ast_uses_name_recur(n->as.block.stmts[i], name, descend_into_fns)) return 1;
            return 0;
        }
        case NODE_RETURN:
            return ast_uses_name_recur(n->as.return_stmt.expr, name, descend_into_fns);
        case NODE_CALL: {
            if (n->as.call.callee
                && ast_uses_name_recur(n->as.call.callee, name, descend_into_fns)) return 1;
            for (int i = 0; i < n->as.call.arg_count; i++)
                if (ast_uses_name_recur(n->as.call.args[i], name, descend_into_fns)) return 1;
            /* Also check the bare-call name (which is a name reference). */
            if (!n->as.call.callee && strcmp(n->as.call.name, name) == 0) return 1;
            return 0;
        }
        case NODE_ARRAY_LIT: {
            for (int i = 0; i < n->as.array_lit.count; i++)
                if (ast_uses_name_recur(n->as.array_lit.items[i], name, descend_into_fns)) return 1;
            return 0;
        }
        case NODE_MAP_LIT: {
            for (int i = 0; i < n->as.map_lit.count; i++) {
                if (ast_uses_name_recur(n->as.map_lit.keys[i],   name, descend_into_fns)) return 1;
                if (ast_uses_name_recur(n->as.map_lit.values[i], name, descend_into_fns)) return 1;
            }
            return 0;
        }
        case NODE_INDEX:
            return ast_uses_name_recur(n->as.index.target, name, descend_into_fns)
                || ast_uses_name_recur(n->as.index.index,  name, descend_into_fns);
        case NODE_INDEX_ASSIGN:
            return ast_uses_name_recur(n->as.index_assign.target, name, descend_into_fns)
                || ast_uses_name_recur(n->as.index_assign.index,  name, descend_into_fns)
                || ast_uses_name_recur(n->as.index_assign.value,  name, descend_into_fns);
        case NODE_INDEX_OPASSIGN:
            return ast_uses_name_recur(n->as.index_opassign.target, name, descend_into_fns)
                || ast_uses_name_recur(n->as.index_opassign.index,  name, descend_into_fns)
                || ast_uses_name_recur(n->as.index_opassign.value,  name, descend_into_fns);
        case NODE_TRY:
            return ast_uses_name_recur(n->as.try_stmt.try_body,   name, descend_into_fns)
                || ast_uses_name_recur(n->as.try_stmt.catch_body, name, descend_into_fns);
        case NODE_THROW:
            return ast_uses_name_recur(n->as.throw_stmt.expr, name, descend_into_fns);
        default: return 0;
    }
}
/* Returns 1 if `name` is referenced inside any inner NODE_FN
   descendant of `n` (but not in `n` itself outside of fns). */
static int captured_in_inner(AST *n, const char *name) {
    if (!n) return 0;
    switch (n->kind) {
        case NODE_FN: return ast_uses_name_recur(n->as.fn_def.body, name, 1);
        case NODE_BINOP:
            return captured_in_inner(n->as.binop.left,  name)
                || captured_in_inner(n->as.binop.right, name);
        case NODE_UNOP:   return captured_in_inner(n->as.unop.operand, name);
        case NODE_LET:    return captured_in_inner(n->as.let_stmt.expr, name);
        case NODE_ASSIGN: return captured_in_inner(n->as.assign_stmt.expr, name);
        case NODE_PRINT:  return captured_in_inner(n->as.print_stmt.expr, name);
        case NODE_IF:
            return captured_in_inner(n->as.if_stmt.cond, name)
                || captured_in_inner(n->as.if_stmt.then_branch, name)
                || captured_in_inner(n->as.if_stmt.else_branch, name);
        case NODE_WHILE:
            return captured_in_inner(n->as.while_stmt.cond, name)
                || captured_in_inner(n->as.while_stmt.body, name);
        case NODE_FOR:
            return captured_in_inner(n->as.for_stmt.init, name)
                || captured_in_inner(n->as.for_stmt.cond, name)
                || captured_in_inner(n->as.for_stmt.step, name)
                || captured_in_inner(n->as.for_stmt.body, name);
        case NODE_BLOCK: {
            for (int i = 0; i < n->as.block.count; i++)
                if (captured_in_inner(n->as.block.stmts[i], name)) return 1;
            return 0;
        }
        case NODE_RETURN: return captured_in_inner(n->as.return_stmt.expr, name);
        case NODE_CALL: {
            if (n->as.call.callee
                && captured_in_inner(n->as.call.callee, name)) return 1;
            for (int i = 0; i < n->as.call.arg_count; i++)
                if (captured_in_inner(n->as.call.args[i], name)) return 1;
            return 0;
        }
        case NODE_ARRAY_LIT: {
            for (int i = 0; i < n->as.array_lit.count; i++)
                if (captured_in_inner(n->as.array_lit.items[i], name)) return 1;
            return 0;
        }
        case NODE_MAP_LIT: {
            for (int i = 0; i < n->as.map_lit.count; i++) {
                if (captured_in_inner(n->as.map_lit.keys[i],   name)) return 1;
                if (captured_in_inner(n->as.map_lit.values[i], name)) return 1;
            }
            return 0;
        }
        case NODE_INDEX:
            return captured_in_inner(n->as.index.target, name)
                || captured_in_inner(n->as.index.index,  name);
        case NODE_INDEX_ASSIGN:
            return captured_in_inner(n->as.index_assign.target, name)
                || captured_in_inner(n->as.index_assign.index,  name)
                || captured_in_inner(n->as.index_assign.value,  name);
        case NODE_INDEX_OPASSIGN:
            return captured_in_inner(n->as.index_opassign.target, name)
                || captured_in_inner(n->as.index_opassign.index,  name)
                || captured_in_inner(n->as.index_opassign.value,  name);
        case NODE_TRY:
            return captured_in_inner(n->as.try_stmt.try_body,   name)
                || captured_in_inner(n->as.try_stmt.catch_body, name);
        case NODE_THROW:
            return captured_in_inner(n->as.throw_stmt.expr, name);
        default: return 0;
    }
}

/* Pre-pass for a function body: scan for every let-binding and every
   parameter; for each, decide whether it's captured by an inner
   closure. Captured names get added to cg->box_set so add_param /
   NODE_LET can emit boxed-slot code. */
static void collect_lets(AST *n, char names[][CL_MAX_TEXT], int *count) {
    if (!n) return;
    switch (n->kind) {
        case NODE_LET:
            if (*count < CG_MAX_NAMES) {
                cl_strncpy_z(names[*count], n->as.let_stmt.name, CL_MAX_TEXT);
                (*count)++;
            }
            collect_lets(n->as.let_stmt.expr, names, count);
            return;
        case NODE_FN: return;   /* don't descend into inner closures */
        case NODE_BLOCK: {
            for (int i = 0; i < n->as.block.count; i++)
                collect_lets(n->as.block.stmts[i], names, count);
            return;
        }
        case NODE_IF:
            collect_lets(n->as.if_stmt.then_branch, names, count);
            collect_lets(n->as.if_stmt.else_branch, names, count);
            return;
        case NODE_WHILE:
            collect_lets(n->as.while_stmt.body, names, count); return;
        case NODE_FOR:
            collect_lets(n->as.for_stmt.init, names, count);
            collect_lets(n->as.for_stmt.body, names, count); return;
        case NODE_TRY:
            collect_lets(n->as.try_stmt.try_body,   names, count);
            /* The catch binding is itself a `let` of the caught
               value; collect it here so it's eligible for boxing. */
            if (*count < CG_MAX_NAMES) {
                cl_strncpy_z(names[*count], n->as.try_stmt.catch_name, CL_MAX_TEXT);
                (*count)++;
            }
            collect_lets(n->as.try_stmt.catch_body, names, count);
            return;
        default: return;
    }
}

static void precompute_boxes(Cg *cg, AST *fn) {
    cg->box_count = 0;
    /* Params. */
    for (int i = 0; i < fn->as.fn_def.param_count; i++) {
        const char *name = fn->as.fn_def.params[i];
        if (captured_in_inner(fn->as.fn_def.body, name)) {
            cl_strncpy_z(cg->box_set[cg->box_count++], name, CL_MAX_TEXT);
        }
    }
    /* Lets (collected from anywhere in the body, not descending into
       inner fns). */
    char names[CG_MAX_NAMES][CL_MAX_TEXT];
    int  count = 0;
    collect_lets(fn->as.fn_def.body, names, &count);
    for (int i = 0; i < count; i++) {
        if (captured_in_inner(fn->as.fn_def.body, names[i])) {
            int already = 0;
            for (int j = 0; j < cg->box_count; j++) {
                if (strcmp(cg->box_set[j], names[i]) == 0) { already = 1; break; }
            }
            if (!already) cl_strncpy_z(cg->box_set[cg->box_count++], names[i], CL_MAX_TEXT);
        }
    }
}

/* --- Top-level emission ------------------------------------------- */

static void emit_prelude(Cg *cg) {
    sb_add(&cg->out, ".intel_syntax noprefix\n");
    sb_add(&cg->out, ".section .rdata\n");
    sb_add(&cg->out, ".LCfmt:\n");
    sb_add(&cg->out, "    .asciz \"%.10g\\n\"\n");
    sb_add(&cg->out, ".section .text\n");
}

static void emit_string_pool(Cg *cg) {
    if (cg->str_count == 0) return;
    sb_add(&cg->out, "\n.section .rdata\n");
    for (int i = 0; i < cg->str_count; i++) {
        sb_printf(&cg->out, ".p2align 3\n.LCstr_%d:\n", i);
        sb_printf(&cg->out, "    .quad %d\n", cg->strs[i].len);
        /* One .byte directive per line, each carrying up to 16 comma-
           separated bytes. Using raw byte values is escape-safe — the
           literal can contain any bytes including quotes and newlines. */
        int k = 0;
        while (k < cg->strs[i].len) {
            int end = k + 16;
            if (end > cg->strs[i].len) end = cg->strs[i].len;
            sb_add(&cg->out, "    .byte ");
            for (int j = k; j < end; j++) {
                sb_printf(&cg->out, "%d%s",
                    (unsigned char)cg->strs[i].bytes[j],
                    (j + 1 < end) ? ", " : "");
            }
            sb_add(&cg->out, "\n");
            k = end;
        }
        sb_add(&cg->out, "    .byte 0\n");
    }
}

static void emit_fn(Cg *cg, AST *fn) {
    /* Reset per-function state. [rbp - 8] is reserved for the saved
       closure pointer (R10 at call time), so locals start at -16. */
    cg->local_bytes = 8;
    cg->max_locals  = 8;
    cg->break_label = -1;
    cg->cont_label  = -1;
    sb_init(&cg->body);
    cg->name_count   = 0;
    cg->scope_depth  = 0;
    cg->in_fn        = 1;
    cg->binop_depth   = 0;
    cg->xmm_used_mask = 0;
    cl_strncpy_z(cg->cur_fn_name, fn->as.fn_def.name, CL_MAX_TEXT);

    precompute_boxes(cg, fn);

    scope_push(cg);

    /* Stash incoming closure pointer (R10) into the reserved slot. For
       direct calls R10 is undefined — harmless, since direct-called
       fns don't access upvals. */
    e(cg, "mov  [rbp - 8], r10");

    /* Each pushed argument occupies a 16-byte slot (only the low 8
       bytes used), so arg i lives at [rbp + 16 + i*16]. */
    for (int i = 0; i < fn->as.fn_def.param_count; i++) {
        add_param(cg, fn->as.fn_def.params[i], 16 + i * 16);
    }
    /* For each captured param, allocate a box and initialize it from
       the arg slot. The boxed-local binding was already added by
       add_param; find it. */
    for (int i = 0; i < fn->as.fn_def.param_count; i++) {
        const char *pname = fn->as.fn_def.params[i];
        if (!is_in_box_set(cg, pname)) continue;
        int box_off = 0, found = 0;
        for (int k = cg->name_count - 1; k >= 0; k--) {
            if (strcmp(cg->names[k].name, pname) == 0
                && cg->names[k].kind == CG_NK_LOCAL_BOXED) {
                box_off = cg->names[k].offset;
                found = 1;
                break;
            }
        }
        if (!found) cl_die("boxed param binding missing");
        ef(cg, "mov  rcx, [rbp + %d]", 16 + i * 16);
        e(cg, "sub  rsp, 32");
        e(cg, "call cl_box_new");
        e(cg, "add  rsp, 32");
        ef(cg, "mov  [rbp - %d], rax", -box_off);
    }

    /* Self-tail-call target: a `return foo(args);` inside fn foo jumps
       here with new arg values already written into the param slots.
       Re-running the body re-executes each `let`, giving fresh local
       values, while the locals' stack slots remain in place. */
    sb_printf(cur(cg), ".Lbody_%s:\n", fn->as.fn_def.name);

    for (int i = 0; i < fn->as.fn_def.body->as.block.count; i++) {
        gen_stmt(cg, fn->as.fn_def.body->as.block.stmts[i]);
    }
    /* Implicit `return 0;` if control falls off the end. */
    e(cg, "# implicit return 0");
    load_double_imm(cg, 0.0);
    ef(cg, "jmp  .Lret_%s", fn->as.fn_def.name);

    /* Frame size: round max_locals up to 16. */
    int frame = (cg->max_locals + 15) & ~15;

    /* Stitch: prologue + body + epilogue into cg->out.
       Frame layout: locals (size `frame`) followed by xmm-save area
       (size `xmm_bytes`), all in one `sub rsp, total` of the prologue.
       Saved xmm regs live at rbp-relative offsets so the epilogue can
       restore them even when reached via `jmp .Lret_X` mid-expression
       (rsp is unknown at that point). */
    int xmm_bytes = xmm_save_bytes(cg);
    int total     = frame + xmm_bytes;
    sb_printf(&cg->out, "\n.globl calc_%s\n", fn->as.fn_def.name);
    sb_printf(&cg->out, "calc_%s:\n", fn->as.fn_def.name);
    sb_add(&cg->out, "    push rbp\n");
    sb_add(&cg->out, "    mov  rbp, rsp\n");
    if (total > 0) sb_printf(&cg->out, "    sub  rsp, %d\n", total);
    emit_xmm_save(cg, frame);
    sb_add(&cg->out, cg->body.buf);
    sb_printf(&cg->out, ".Lret_%s:\n", fn->as.fn_def.name);
    emit_xmm_restore(cg, frame);
    sb_add(&cg->out, "    mov  rsp, rbp\n");
    sb_add(&cg->out, "    pop  rbp\n");
    sb_add(&cg->out, "    ret\n");

    cg->in_fn = 0;
    scope_pop(cg);
}

/* For main, run a top-level capture analysis: any top-level let
   referenced inside any top-level inline closure becomes boxed. */
static void precompute_boxes_top_level(Cg *cg, const Program *prog) {
    cg->box_count = 0;
    char names[CG_MAX_NAMES][CL_MAX_TEXT];
    int  count = 0;
    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind == NODE_FN) continue;
        collect_lets(n, names, &count);
    }
    for (int i = 0; i < count; i++) {
        int captured = 0;
        for (int j = 0; j < prog->count; j++) {
            AST *n = prog->items[j];
            if (n->kind == NODE_FN) continue;
            if (captured_in_inner(n, names[i])) { captured = 1; break; }
        }
        if (captured) {
            int already = 0;
            for (int k = 0; k < cg->box_count; k++) {
                if (strcmp(cg->box_set[k], names[i]) == 0) { already = 1; break; }
            }
            if (!already) cl_strncpy_z(cg->box_set[cg->box_count++], names[i], CL_MAX_TEXT);
        }
    }
}

static void emit_main(Cg *cg, const Program *prog) {
    /* The "main" function holds all top-level statements. [rbp - 8]
       is reserved (closure ptr slot — main doesn't have one, but the
       layout stays uniform). */
    cg->local_bytes = 8;
    cg->max_locals  = 8;
    cg->break_label = -1;
    cg->cont_label  = -1;
    sb_init(&cg->body);
    cg->name_count   = 0;
    cg->scope_depth  = 0;
    cg->in_fn        = 1;
    cg->binop_depth   = 0;
    cg->xmm_used_mask = 0;
    cl_strncpy_z(cg->cur_fn_name, "_main", CL_MAX_TEXT);

    precompute_boxes_top_level(cg, prog);

    scope_push(cg);
    /* No closure ptr to save for main, but zero the slot so anything
       that accidentally reads it gets NULL rather than garbage. */
    e(cg, "xor  rax, rax");
    e(cg, "mov  [rbp - 8], rax");
    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind == NODE_FN) continue;   /* fns emitted separately */
        gen_stmt(cg, n);
    }

    int frame = (cg->max_locals + 15) & ~15;

    int xmm_bytes = xmm_save_bytes(cg);
    int total     = frame + xmm_bytes;
    sb_add(&cg->out, "\n.globl main\n");
    sb_add(&cg->out, "main:\n");
    sb_add(&cg->out, "    push rbp\n");
    sb_add(&cg->out, "    mov  rbp, rsp\n");
    if (total > 0) sb_printf(&cg->out, "    sub  rsp, %d\n", total);
    emit_xmm_save(cg, frame);
    sb_add(&cg->out, cg->body.buf);
    sb_add(&cg->out, ".Lret__main:\n");
    emit_xmm_restore(cg, frame);
    sb_add(&cg->out, "    xor  eax, eax\n");
    sb_add(&cg->out, "    mov  rsp, rbp\n");
    sb_add(&cg->out, "    pop  rbp\n");
    sb_add(&cg->out, "    ret\n");

    cg->in_fn = 0;
    scope_pop(cg);
}

/* Emit one inline-closure body. Similar shape to emit_fn but the
   entry label is .Lclosure_<id>, and upvals are pre-registered as
   CG_NK_UPVAL bindings before params so the body's NODE_VAR lookups
   resolve correctly. */
static void emit_closure_body(Cg *cg, CgClosure *cc) {
    AST *fn = cc->fn_node;
    cg->local_bytes = 8;
    cg->max_locals  = 8;
    cg->break_label = -1;
    cg->cont_label  = -1;
    sb_init(&cg->body);
    cg->name_count   = 0;
    cg->scope_depth  = 0;
    cg->in_fn        = 1;
    cg->binop_depth   = 0;
    cg->xmm_used_mask = 0;

    char ret_name[CL_MAX_TEXT];
    snprintf(ret_name, CL_MAX_TEXT, "closure_%d", cc->label_id);
    cl_strncpy_z(cg->cur_fn_name, ret_name, CL_MAX_TEXT);

    /* For the boxing pass: we need to recognize the closure's OWN
       inner closures (transitive) — defer that since we don't support
       it yet. Just inspect this closure's body for inner-fn captures
       of its own params/lets. */
    precompute_boxes(cg, fn);

    scope_push(cg);
    e(cg, "mov  [rbp - 8], r10");

    /* Upvals come first so they shadow any lexically-outer same-name
       slot during lookup. The codegen for NODE_VAR / NODE_ASSIGN will
       hit these via lookup_name. */
    for (int i = 0; i < cc->upval_count; i++) {
        add_upval(cg, cc->upval_names[i], i);
    }

    /* Params (with shadowing-boxing if needed for inner-inner fns). */
    for (int i = 0; i < fn->as.fn_def.param_count; i++) {
        add_param(cg, fn->as.fn_def.params[i], 16 + i * 16);
    }
    for (int i = 0; i < fn->as.fn_def.param_count; i++) {
        const char *pname = fn->as.fn_def.params[i];
        if (!is_in_box_set(cg, pname)) continue;
        int box_off = 0, found = 0;
        for (int k = cg->name_count - 1; k >= 0; k--) {
            if (strcmp(cg->names[k].name, pname) == 0
                && cg->names[k].kind == CG_NK_LOCAL_BOXED) {
                box_off = cg->names[k].offset; found = 1; break;
            }
        }
        if (!found) cl_die("boxed param binding missing in closure");
        ef(cg, "mov  rcx, [rbp + %d]", 16 + i * 16);
        e(cg, "sub  rsp, 32");
        e(cg, "call cl_box_new");
        e(cg, "add  rsp, 32");
        ef(cg, "mov  [rbp - %d], rax", -box_off);
    }

    for (int i = 0; i < fn->as.fn_def.body->as.block.count; i++) {
        gen_stmt(cg, fn->as.fn_def.body->as.block.stmts[i]);
    }
    e(cg, "# implicit return 0");
    load_double_imm(cg, 0.0);
    ef(cg, "jmp  .Lret_%s", cg->cur_fn_name);

    int frame = (cg->max_locals + 15) & ~15;

    int xmm_bytes = xmm_save_bytes(cg);
    int total     = frame + xmm_bytes;
    sb_printf(&cg->out, "\n.Lclosure_%d:\n", cc->label_id);
    sb_add(&cg->out, "    push rbp\n");
    sb_add(&cg->out, "    mov  rbp, rsp\n");
    if (total > 0) sb_printf(&cg->out, "    sub  rsp, %d\n", total);
    emit_xmm_save(cg, frame);
    sb_add(&cg->out, cg->body.buf);
    sb_printf(&cg->out, ".Lret_%s:\n", cg->cur_fn_name);
    emit_xmm_restore(cg, frame);
    sb_add(&cg->out, "    mov  rsp, rbp\n");
    sb_add(&cg->out, "    pop  rbp\n");
    sb_add(&cg->out, "    ret\n");

    cg->in_fn = 0;
    scope_pop(cg);
}

/* Variant that also takes a library_mode flag. The non-flagged
   entry is kept as a thin wrapper. */
/* --- Peephole optimizer ----------------------------------------------
 *
 * Scans the emitted assembly text and applies small, locally-safe
 * rewrites. Each pattern looks at one or two adjacent non-blank lines
 * and decides if a replacement is legal. The whole thing is text-based
 * which makes it simple — and limits its power. It's the second-cheapest
 * win after constant folding.
 *
 * Patterns (all safe with the current codegen):
 *
 *   1. `add rsp, 16` followed immediately by `sub rsp, 16` (or vice
 *      versa) → drop both. These appear when a push_xmm0 ... pop_xmm0
 *      sandwich has no body, e.g. when an inner expression folded away.
 *
 *   2. `mov reg, 0` → `xor reg, reg`. Smaller encoding, same effect on
 *      the flags we care about. We only rewrite for caller-save GPRs
 *      we know aren't flag-sensitive downstream.
 *
 *   3. `jmp .Lx` followed by `.Lx:` → drop the jmp. Common when an
 *      if branch falls through to the join label.
 *
 *   4. `movq reg, xmm0` followed immediately by `movq xmm0, reg` (same
 *      reg) → drop both. Generated when the codegen passes a value
 *      through a GP register that wasn't actually used.
 *
 * Anything we can't statically prove safe, we leave alone. Anything we
 * do change must not affect any branch decision in between.
 */

static int starts_with_strip(const char *line, const char *prefix) {
    while (*line == ' ' || *line == '\t') line++;
    while (*prefix && *line == *prefix) { line++; prefix++; }
    return *prefix == '\0';
}

/* Returns 1 if `line` is the label definition matching label_name (e.g.
   line ".L42:" and label_name ".L42"). */
static int line_is_label(const char *line, const char *label_name) {
    while (*line == ' ' || *line == '\t') line++;
    size_t n = strlen(label_name);
    if (strncmp(line, label_name, n) != 0) return 0;
    return line[n] == ':';
}

/* Parse the jump label from a line of the form `    jmp .L42` —
   write into `out` of size cap. Returns 1 if matched. */
static int parse_jmp_label(const char *line, char *out, size_t cap) {
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "jmp", 3) != 0) return 0;
    line += 3;
    while (*line == ' ' || *line == '\t') line++;
    size_t i = 0;
    while (*line && *line != '\n' && *line != ' ' && *line != '\t' && i + 1 < cap) {
        out[i++] = *line++;
    }
    out[i] = '\0';
    return i > 0;
}

/* "movq REG, xmm0" → write REG to out, return 1 on match. */
static int parse_movq_from_xmm0(const char *line, char *reg_out, size_t cap) {
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "movq", 4) != 0) return 0;
    line += 4;
    while (*line == ' ' || *line == '\t') line++;
    size_t i = 0;
    while (*line && *line != ',' && *line != ' ' && i + 1 < cap) reg_out[i++] = *line++;
    reg_out[i] = '\0';
    if (i == 0 || strcmp(reg_out, "xmm0") == 0) return 0;
    while (*line == ' ' || *line == '\t' || *line == ',') line++;
    if (strncmp(line, "xmm0", 4) != 0) return 0;
    return 1;
}

/* "movq xmm0, REG" — check REG matches. */
static int parse_movq_to_xmm0_reg(const char *line, const char *reg) {
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "movq", 4) != 0) return 0;
    line += 4;
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "xmm0", 4) != 0) return 0;
    line += 4;
    while (*line == ' ' || *line == '\t' || *line == ',') line++;
    size_t n = strlen(reg);
    if (strncmp(line, reg, n) != 0) return 0;
    char after = line[n];
    return (after == '\n' || after == ' ' || after == '\t' || after == '\0');
}

/* "mov REG, 0" — write REG. We restrict to a handful of "safe" GPRs
   to avoid touching anything used as a memory base. */
static int parse_mov_zero(const char *line, char *reg_out, size_t cap) {
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "mov ", 4) != 0 && strncmp(line, "mov\t", 4) != 0) return 0;
    line += 3;
    while (*line == ' ' || *line == '\t') line++;
    size_t i = 0;
    while (*line && *line != ',' && i + 1 < cap) reg_out[i++] = *line++;
    reg_out[i] = '\0';
    if (i == 0) return 0;
    while (*line == ',' || *line == ' ' || *line == '\t') line++;
    /* Match "0" alone, or "0x0", or "0x00...", etc. */
    if (line[0] != '0') return 0;
    if (line[1] == 'x') {
        const char *p2 = line + 2;
        while (*p2 == '0') p2++;
        if (*p2 != '\n' && *p2 != ' ' && *p2 != '\t' && *p2 != '\0') return 0;
    } else if (line[1] != '\n' && line[1] != ' ' && line[1] != '\t' && line[1] != '\0') {
        return 0;
    }
    /* whitelist of caller-save scratch registers */
    static const char *safe[] = {"rax", "rcx", "rdx", "r8", "r9", "r10", "r11", NULL};
    for (int j = 0; safe[j]; j++) {
        if (strcmp(reg_out, safe[j]) == 0) return 1;
    }
    return 0;
}

/* Find the next non-blank, non-comment line in `lines[]` starting at
   index `from`. Returns the index, or `n` if none. */
static int next_real_line(char **lines, int from, int n) {
    for (int i = from; i < n; i++) {
        const char *l = lines[i];
        while (*l == ' ' || *l == '\t') l++;
        if (*l != '\0' && *l != '\n' && *l != ';') return i;
    }
    return n;
}

static char *cl_peephole_optimize(const char *asm_text) {
    if (!asm_text) return NULL;
    /* Split into lines. */
    size_t L = strlen(asm_text);
    /* Worst case: every char is its own line. */
    char **lines = (char **)cl_track_malloc(sizeof(char *) * (L + 2));
    int    n = 0;
    /* Walk and split. */
    const char *p = asm_text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *line = (char *)cl_track_malloc(len + 2);
        memcpy(line, p, len);
        line[len] = '\n';
        line[len + 1] = '\0';
        lines[n++] = line;
        if (!nl) break;
        p = nl + 1;
    }

    /* Pass 1: pattern matches. We mark lines for deletion by setting
       the pointer to "" (an empty string). A second pass at the end
       stitches the surviving lines back together. */
    char buf1[32];

    for (int i = 0; i < n; i++) {
        char *L1 = lines[i];
        if (L1[0] == '\0') continue;

        /* Pattern 2: mov reg, 0 -> xor reg, reg. (Single-line.) */
        if (parse_mov_zero(L1, buf1, sizeof buf1)) {
            char *replacement = (char *)cl_track_malloc(96);
            snprintf(replacement, 96, "    xor  %s, %s\n", buf1, buf1);
            lines[i] = replacement;
            continue;
        }

        /* Two-line patterns. */
        int j = next_real_line(lines, i + 1, n);
        if (j >= n) continue;
        char *L2 = lines[j];

        /* Pattern 1: add rsp, K immediately followed by sub rsp, K (or
           the reverse) cancels. */
        if (starts_with_strip(L1, "add  rsp, 16") && starts_with_strip(L2, "sub  rsp, 16")) {
            lines[i] = (char *)""; lines[j] = (char *)"";
            continue;
        }
        if (starts_with_strip(L1, "sub  rsp, 16") && starts_with_strip(L2, "add  rsp, 16")) {
            lines[i] = (char *)""; lines[j] = (char *)"";
            continue;
        }

        /* Pattern 4: movq REG, xmm0 ; movq xmm0, REG — same reg, no
           use of REG in between (we required adjacency via next_real_line). */
        if (parse_movq_from_xmm0(L1, buf1, sizeof buf1)
            && parse_movq_to_xmm0_reg(L2, buf1)) {
            lines[i] = (char *)""; lines[j] = (char *)"";
            continue;
        }

        /* Pattern 3: jmp .Lx followed by .Lx: — drop the jmp. */
        if (parse_jmp_label(L1, buf1, sizeof buf1) && line_is_label(L2, buf1)) {
            lines[i] = (char *)"";
            /* keep going; the label remains */
            continue;
        }
    }

    /* Stitch surviving lines back. */
    size_t total = 1;
    for (int i = 0; i < n; i++) total += strlen(lines[i]);
    char *out = (char *)cl_track_malloc(total + 16);
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        size_t k = strlen(lines[i]);
        memcpy(out + o, lines[i], k);
        o += k;
    }
    out[o] = '\0';
    return out;
}

char *codegen_x64_program_ex(const Program *prog, int library_mode) {
    Cg cg = {0};
    sb_init(&cg.out);
    cg.next_label   = 1;
    cg.break_label  = -1;
    cg.cont_label   = -1;
    cg.library_mode = library_mode;

    collect_fns(&cg, prog);
    emit_prelude(&cg);

    /* Emit each user function. Skip extern fns — they're forward
       declarations the linker will resolve. */
    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind != NODE_FN) continue;
        if (n->as.fn_def.name[0] == '\0') continue;
        if (n->as.fn_def.is_extern) continue;
        emit_fn(&cg, n);
    }

    /* In library mode, no `main` and no top-level code. The caller's
       entry-point unit owns those. */
    if (!cg.library_mode) {
        emit_main(&cg, prog);
    }

    /* Emit all registered closure bodies iteratively — emitting one
       may discover nested closures via gen_expr's NODE_FN case. */
    int emitted = 0;
    while (emitted < cg.closure_count) {
        emit_closure_body(&cg, &cg.closures[emitted]);
        emitted++;
    }

    /* Emit one trampoline per unique builtin referenced as a value.
       Each translates the cdecl-style calc call convention (args at
       [rbp + 16 + i*16], result in xmm0) to the MS x64 ABI of the
       corresponding cl_builtin_* runtime function. */
    for (int i = 0; i < cg.trampoline_count; i++) {
        const RtBuiltin *rb = find_rt_builtin(cg.trampolines[i]);
        if (!rb) cl_die("trampoline target vanished");
        sb_printf(&cg.out, "\n.Lbi_%s:\n", rb->name);
        sb_add(&cg.out, "    push rbp\n");
        sb_add(&cg.out, "    mov  rbp, rsp\n");
        /* Allocate at least the 32-byte shadow space; round up. The
           runtime call below uses it. */
        sb_add(&cg.out, "    sub  rsp, 32\n");
        /* Load args from the 16-byte cdecl slots into MS x64 int regs.
           Already wired for 4 — same registers as the main rt_builtin
           call path. */
        static const char *int_regs[4] = {"rcx", "rdx", "r8", "r9"};
        for (int a = 0; a < rb->arity && a < 4; a++) {
            sb_printf(&cg.out, "    mov  %s, [rbp + %d]\n",
                int_regs[a], 16 + a * 16);
        }
        sb_printf(&cg.out, "    call %s\n", rb->rt_symbol);
        sb_add(&cg.out, "    add  rsp, 32\n");
        sb_add(&cg.out, "    movq xmm0, rax\n");
        sb_add(&cg.out, "    pop  rbp\n");
        sb_add(&cg.out, "    ret\n");
    }

    emit_string_pool(&cg);

    /* Peephole pass — see cl_peephole_optimize below. */
    char *out = cl_peephole_optimize(cg.out.buf);
    return out ? out : cg.out.buf;
}

char *codegen_x64_program(const Program *prog) {
    return codegen_x64_program_ex(prog, 0);
}
