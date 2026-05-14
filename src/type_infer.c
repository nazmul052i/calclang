/*
   Flow-sensitive type inference for CalcLang.

   Conceptually this is a classic forward dataflow analysis. We walk
   each function body once (with fixpoint iteration on loops) and at
   every program point we track, per variable in scope, the set of
   types that variable could hold given everything we've seen so far.
   At branch joins the per-branch sets are unioned. At a use site (a
   NODE_VAR read), if the set has collapsed to a single concrete type
   we stamp that type onto the node's `inferred_type` field; codegen
   then has more precise static info than the old "last write wins,
   widen on disagreement" symbol-level inference could give it.

   Type representation
   -------------------
   A `TypeSet` is a small bitmask. Bit i is set iff TypeAnnot value i
   is a possible type at this program point. The set is "exact" iff
   exactly one bit is set; otherwise the variable is either unknown
   (no bits) or polymorphic (multiple bits), and we record TYPE_ANY on
   the use site to defer to runtime checks.

   BOOL is folded into NUM (they share runtime representation), so the
   inference uses TYPE_NUM as the canonical numeric tag.

   Loops
   -----
   For while/for we run the body, merge the post-body env with the
   pre-loop env, and re-run if anything changed. The lattice is finite
   (TypeSet is one byte) so this terminates quickly — in practice
   after one or two iterations.

   What this DOESN'T do
   --------------------
   - Path-sensitive narrowing via `type_of(x) == "num"` guards. The
     guard expression is evaluated to a number, not consumed as a
     refinement.
   - Function specialization per call-site signature. Each function is
     analyzed once with its declared parameter types.
   - Cross-function flow into closures' free variables. A closure's
     body is analyzed with its declared parameters; free variables are
     looked up in the enclosing analyzer state and used as best-effort
     starting types, but they aren't refined through the closure body
     because the closure may run at arbitrary points later.
*/

#include "type_infer.h"
#include "common.h"
#include "calclib.h"

/* --- TypeSet ------------------------------------------------------- */
typedef uint8_t TypeSet;

#define TS_NUM_BIT  (1u << TYPE_NUM)
#define TS_STR_BIT  (1u << TYPE_STR)
#define TS_ARR_BIT  (1u << TYPE_ARR)
#define TS_MAP_BIT  (1u << TYPE_MAP)
#define TS_FN_BIT   (1u << TYPE_FN)
#define TS_ANY_MASK (TS_NUM_BIT | TS_STR_BIT | TS_ARR_BIT | TS_MAP_BIT | TS_FN_BIT)

static TypeSet ts_from_annot(TypeAnnot t) {
    /* TYPE_BOOL collapses to NUM; TYPE_ANY is the full set. */
    if (t == TYPE_ANY)  return TS_ANY_MASK;
    if (t == TYPE_BOOL) return TS_NUM_BIT;
    return (TypeSet)(1u << t);
}

/* If exactly one TypeAnnot is in the set, return its code via *out and
   return 1. Otherwise return 0 (multiple types OR no information). */
static int ts_to_single(TypeSet s, TypeAnnot *out) {
    /* Count bits — pop-count for a one-byte mask. */
    if (s == 0)        return 0;
    if (s & (s - 1))   return 0;   /* more than one bit */
    for (int i = 1; i < 8; i++) {
        if (s == (1u << i)) { *out = (TypeAnnot)i; return 1; }
    }
    return 0;
}

/* --- Type environment --------------------------------------------- */
/* Flat array of bindings with scope markers. Scope push records the
   current binding count; scope pop truncates back to it. Lookup walks
   from the end (innermost) outward. */

#define TI_MAX_BINDINGS 256
#define TI_MAX_SCOPES   64

typedef struct {
    char    name[CL_MAX_TEXT];
    TypeSet types;
} TiBinding;

typedef struct {
    TiBinding bindings[TI_MAX_BINDINGS];
    int       count;
    int       scope_starts[TI_MAX_SCOPES];
    int       scope_depth;
} TiEnv;

static void env_init(TiEnv *e) {
    e->count = 0;
    e->scope_depth = 0;
}

static void env_push_scope(TiEnv *e) {
    if (e->scope_depth >= TI_MAX_SCOPES) cl_die("type-infer: scope nesting too deep");
    e->scope_starts[e->scope_depth++] = e->count;
}

static void env_pop_scope(TiEnv *e) {
    if (e->scope_depth <= 0) cl_die("type-infer: scope pop underflow");
    e->count = e->scope_starts[--e->scope_depth];
}

static void env_bind(TiEnv *e, const char *name, TypeSet t) {
    if (e->count >= TI_MAX_BINDINGS) cl_die("type-infer: too many bindings");
    TiBinding *b = &e->bindings[e->count++];
    cl_strncpy_z(b->name, name, CL_MAX_TEXT);
    b->types = t;
}

/* Find the innermost binding for `name`. Returns -1 if not found. */
static int env_index(TiEnv *e, const char *name) {
    for (int i = e->count - 1; i >= 0; i--) {
        if (strcmp(e->bindings[i].name, name) == 0) return i;
    }
    return -1;
}

static int env_lookup(TiEnv *e, const char *name, TypeSet *out) {
    int i = env_index(e, name);
    if (i < 0) return 0;
    *out = e->bindings[i].types;
    return 1;
}

/* Refine (overwrite) the innermost binding's type. If the variable
   isn't bound in any active scope, create a binding in the outermost
   active scope — this models top-level globals that get reassigned
   after their first `let`. */
static void env_set(TiEnv *e, const char *name, TypeSet t) {
    int i = env_index(e, name);
    if (i >= 0) { e->bindings[i].types = t; return; }
    env_bind(e, name, t);
}

/* Snapshot / restore: snapshot stores the binding count and the
   per-binding types as they are now. Restore truncates and overwrites
   binding types from the snapshot. Used at if/while boundaries. */
typedef struct {
    TypeSet types[TI_MAX_BINDINGS];
    int     count;
} TiSnapshot;

static void env_snapshot(const TiEnv *e, TiSnapshot *s) {
    s->count = e->count;
    for (int i = 0; i < e->count; i++) s->types[i] = e->bindings[i].types;
}

static void env_restore(TiEnv *e, const TiSnapshot *s) {
    e->count = s->count;
    for (int i = 0; i < e->count; i++) e->bindings[i].types = s->types[i];
}

/* Merge `other` into `e`. For every binding present at the same
   position in both, union the types. Bindings that exist only in `e`
   (introduced inside the branch that "other" represents the
   alternative of) are widened to TS_ANY_MASK — they may be
   uninitialized along that path. */
static int env_merge(TiEnv *e, const TiSnapshot *other) {
    int changed = 0;
    int common = e->count < other->count ? e->count : other->count;
    for (int i = 0; i < common; i++) {
        TypeSet u = e->bindings[i].types | other->types[i];
        if (u != e->bindings[i].types) {
            e->bindings[i].types = u;
            changed = 1;
        }
    }
    for (int i = common; i < e->count; i++) {
        if (e->bindings[i].types != TS_ANY_MASK) {
            e->bindings[i].types = TS_ANY_MASK;
            changed = 1;
        }
    }
    return changed;
}

/* Compare current env's types to a snapshot. Returns 1 if anything
   differs (used for loop fixpoint detection). */
static int env_changed_since(const TiEnv *e, const TiSnapshot *s) {
    if (e->count != s->count) return 1;
    for (int i = 0; i < e->count; i++) {
        if (e->bindings[i].types != s->types[i]) return 1;
    }
    return 0;
}

/* --- Known function return types ---------------------------------- */
/* Mirror of codegen.c's CgUserFn table: we need the return types so a
   NODE_CALL can be typed during analysis. Built up by collect_fns. */

#define TI_MAX_FNS 256
typedef struct {
    char      name[CL_MAX_TEXT];
    TypeAnnot return_type;
    TypeAnnot param_types[CL_MAX_PARAMS];
    int       param_count;
} TiFn;

static TiFn g_fns[TI_MAX_FNS];
static int  g_fn_count = 0;

static const TiFn *find_fn(const char *name) {
    for (int i = 0; i < g_fn_count; i++) {
        if (strcmp(g_fns[i].name, name) == 0) return &g_fns[i];
    }
    return NULL;
}

static void collect_fns(const Program *prog) {
    g_fn_count = 0;
    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind != NODE_FN) continue;
        if (n->as.fn_def.name[0] == '\0') continue;
        if (g_fn_count >= TI_MAX_FNS) cl_die("type-infer: too many top-level fns");
        TiFn *f = &g_fns[g_fn_count++];
        cl_strncpy_z(f->name, n->as.fn_def.name, CL_MAX_TEXT);
        f->return_type = n->as.fn_def.return_type;
        f->param_count = n->as.fn_def.param_count;
        for (int j = 0; j < n->as.fn_def.param_count; j++) {
            f->param_types[j] = n->as.fn_def.param_types[j];
        }
    }
}

/* --- Expression type inference ------------------------------------ */
/* Returns the TypeSet of `expr`. As a side effect, walks `expr` and
   annotates every NODE_VAR descendant with a concrete TypeAnnot when
   the environment proves it. */

static TypeSet infer_expr(AST *expr, TiEnv *env);

static TypeSet infer_call(AST *n, TiEnv *env) {
    if (n->as.call.callee) {
        /* Indirect call: we have no name to look up the return type by.
           Still walk the args so their NODE_VARs get annotated. */
        infer_expr(n->as.call.callee, env);
        for (int i = 0; i < n->as.call.arg_count; i++)
            infer_expr(n->as.call.args[i], env);
        return TS_ANY_MASK;
    }
    for (int i = 0; i < n->as.call.arg_count; i++)
        infer_expr(n->as.call.args[i], env);

    const BuiltinDef *bi = cl_find_builtin(n->as.call.name);
    if (bi && bi->return_type != 0) return ts_from_annot((TypeAnnot)bi->return_type);

    const TiFn *uf = find_fn(n->as.call.name);
    if (uf && uf->return_type != TYPE_ANY) return ts_from_annot(uf->return_type);

    /* Maybe a variable holding a fn value. We don't know its return
       type without specialization. */
    TypeSet vt;
    if (env_lookup(env, n->as.call.name, &vt)) return TS_ANY_MASK;
    return TS_ANY_MASK;
}

static TypeSet infer_binop(AST *n, TiEnv *env) {
    TypeSet l = infer_expr(n->as.binop.left,  env);
    TypeSet r = infer_expr(n->as.binop.right, env);
    TokenType op = n->as.binop.op;
    /* + is special: num+num=num, str+anything=str, anything+str=str. */
    if (op == TOK_PLUS) {
        if (l == TS_STR_BIT || r == TS_STR_BIT) return TS_STR_BIT;
        if (l == TS_NUM_BIT && r == TS_NUM_BIT) return TS_NUM_BIT;
        /* Any other combination — could be either depending on runtime. */
        return TS_NUM_BIT | TS_STR_BIT;
    }
    /* Comparison ops always produce 0/1. */
    if (op == TOK_EQEQ || op == TOK_NEQ || op == TOK_LT
     || op == TOK_LE   || op == TOK_GT  || op == TOK_GE
     || op == TOK_AND  || op == TOK_OR) {
        return TS_NUM_BIT;
    }
    /* Arithmetic (-, *, /, %): result type follows the operand type set.
       num op num -> num; cpx anywhere -> cpx (which lives outside the
       inference lattice and shows up as TS_ANY_MASK). If either operand
       isn't statically proven num, widen to ANY so downstream code
       dispatches through the polymorphic runtime path instead of
       emitting a hardware FP op on possibly-NaN-tagged complex bits. */
    if (l == TS_NUM_BIT && r == TS_NUM_BIT) return TS_NUM_BIT;
    return TS_ANY_MASK;
}

static TypeSet infer_index(AST *n, TiEnv *env) {
    infer_expr(n->as.index.target, env);
    infer_expr(n->as.index.index,  env);
    /* arr[i] / map[k] — element type isn't tracked; widen to any. */
    return TS_ANY_MASK;
}

static TypeSet infer_expr(AST *expr, TiEnv *env) {
    if (!expr) return TS_ANY_MASK;
    switch (expr->kind) {
        case NODE_NUMBER:    return TS_NUM_BIT;
        case NODE_STRING:    return TS_STR_BIT;
        case NODE_ARRAY_LIT: {
            for (int i = 0; i < expr->as.array_lit.count; i++)
                infer_expr(expr->as.array_lit.items[i], env);
            return TS_ARR_BIT;
        }
        case NODE_MAP_LIT: {
            for (int i = 0; i < expr->as.map_lit.count; i++) {
                infer_expr(expr->as.map_lit.keys[i],   env);
                infer_expr(expr->as.map_lit.values[i], env);
            }
            return TS_MAP_BIT;
        }
        case NODE_VAR: {
            TypeSet t;
            if (env_lookup(env, expr->as.var, &t)) {
                TypeAnnot single;
                if (ts_to_single(t, &single)) {
                    expr->inferred_type = (int)single;
                } else {
                    expr->inferred_type = TYPE_ANY;
                }
                return t;
            }
            /* Free reference — maybe a function value or a builtin. */
            if (find_fn(expr->as.var) != NULL)         return TS_FN_BIT;
            if (cl_find_builtin(expr->as.var) != NULL) return TS_FN_BIT;
            expr->inferred_type = TYPE_ANY;
            return TS_ANY_MASK;
        }
        case NODE_UNOP:
            infer_expr(expr->as.unop.operand, env);
            /* Unary - is numeric; ! is numeric (0/1). */
            return TS_NUM_BIT;
        case NODE_BINOP: return infer_binop(expr, env);
        case NODE_CALL:  return infer_call(expr, env);
        case NODE_INDEX: return infer_index(expr, env);
        case NODE_FN:
            /* Inline closure — value is a function. We don't walk the
               body during expression inference; it gets analyzed by
               the top-level driver in its own scope. */
            return TS_FN_BIT;
        default:
            return TS_ANY_MASK;
    }
}

/* --- Statement inference ----------------------------------------- */

static void infer_stmt(AST *n, TiEnv *env);

static void infer_block(AST *n, TiEnv *env) {
    env_push_scope(env);
    for (int i = 0; i < n->as.block.count; i++)
        infer_stmt(n->as.block.stmts[i], env);
    env_pop_scope(env);
}

static void infer_if(AST *n, TiEnv *env) {
    infer_expr(n->as.if_stmt.cond, env);
    TiSnapshot before;
    env_snapshot(env, &before);
    infer_stmt(n->as.if_stmt.then_branch, env);
    TiSnapshot after_then;
    env_snapshot(env, &after_then);
    env_restore(env, &before);
    if (n->as.if_stmt.else_branch) {
        infer_stmt(n->as.if_stmt.else_branch, env);
    }
    /* `else` may be absent — that path leaves env at `before`, which
       we just restored to. Merging the post-then snapshot folds in
       all the then-branch's refinements. */
    env_merge(env, &after_then);
}

static void infer_loop_body(AST *body, AST *step, TiEnv *env) {
    /* Iterate to fixpoint. TypeSet is finite so this terminates fast. */
    for (int iter = 0; iter < 8; iter++) {
        TiSnapshot before;
        env_snapshot(env, &before);
        if (body) infer_stmt(body, env);
        if (step) infer_stmt(step, env);
        env_merge(env, &before);
        if (!env_changed_since(env, &before)) break;
    }
}

static void infer_while(AST *n, TiEnv *env) {
    /* Cond evaluates with the pre-loop env; subsequent iterations'
       cond evaluations are conservative (the merged env covers them). */
    infer_expr(n->as.while_stmt.cond, env);
    infer_loop_body(n->as.while_stmt.body, NULL, env);
    /* Re-walk cond once at fixpoint so its NODE_VARs get the merged
       types. */
    infer_expr(n->as.while_stmt.cond, env);
}

static void infer_for(AST *n, TiEnv *env) {
    env_push_scope(env);
    if (n->as.for_stmt.init) infer_stmt(n->as.for_stmt.init, env);
    if (n->as.for_stmt.cond) infer_expr(n->as.for_stmt.cond, env);
    infer_loop_body(n->as.for_stmt.body, n->as.for_stmt.step, env);
    if (n->as.for_stmt.cond) infer_expr(n->as.for_stmt.cond, env);
    env_pop_scope(env);
}

static void infer_stmt(AST *n, TiEnv *env) {
    if (!n) return;
    switch (n->kind) {
        case NODE_LET: {
            TypeSet t = infer_expr(n->as.let_stmt.expr, env);
            TypeAnnot ann = n->as.let_stmt.type;
            /* If an explicit annotation is present, take its intersection
               with the inferred RHS. If they're disjoint we trust the
               annotation (the runtime check will catch mismatches). */
            if (ann != TYPE_ANY) {
                TypeSet declared = ts_from_annot(ann);
                TypeSet narrowed = t & declared;
                t = narrowed ? narrowed : declared;
            }
            env_bind(env, n->as.let_stmt.name, t);
            return;
        }
        case NODE_ASSIGN: {
            TypeSet t = infer_expr(n->as.assign_stmt.expr, env);
            env_set(env, n->as.assign_stmt.name, t);
            return;
        }
        case NODE_PRINT:
            infer_expr(n->as.print_stmt.expr, env);
            return;
        case NODE_IF:    infer_if(n, env);    return;
        case NODE_WHILE: infer_while(n, env); return;
        case NODE_FOR:   infer_for(n, env);   return;
        case NODE_BLOCK: infer_block(n, env); return;
        case NODE_RETURN:
            if (n->as.return_stmt.expr) infer_expr(n->as.return_stmt.expr, env);
            return;
        case NODE_BREAK:
        case NODE_CONTINUE:
            return;
        case NODE_CALL:
            infer_expr(n, env);
            return;
        case NODE_INDEX_ASSIGN:
            infer_expr(n->as.index_assign.target, env);
            infer_expr(n->as.index_assign.index,  env);
            infer_expr(n->as.index_assign.value,  env);
            return;
        case NODE_INDEX_OPASSIGN:
            infer_expr(n->as.index_opassign.target, env);
            infer_expr(n->as.index_opassign.index,  env);
            infer_expr(n->as.index_opassign.value,  env);
            return;
        case NODE_FN:
            /* Nested named fn inside a block — sugar for `let name = fn(...)`.
               Treat as binding name -> fn. */
            if (n->as.fn_def.name[0] != '\0')
                env_bind(env, n->as.fn_def.name, TS_FN_BIT);
            return;
        default:
            return;
    }
}

/* --- Function-body analysis --------------------------------------- */

static void infer_function(AST *fn) {
    TiEnv env;
    env_init(&env);
    env_push_scope(&env);
    for (int i = 0; i < fn->as.fn_def.param_count; i++) {
        env_bind(&env, fn->as.fn_def.params[i],
            ts_from_annot(fn->as.fn_def.param_types[i]));
    }
    if (fn->as.fn_def.body) {
        for (int i = 0; i < fn->as.fn_def.body->as.block.count; i++)
            infer_stmt(fn->as.fn_def.body->as.block.stmts[i], &env);
    }
    env_pop_scope(&env);
}

/* Find every NODE_FN in `n` and analyze it. Recurses through all
   statement / expression positions so nested closures get covered
   too. The recursion descends through bodies, so a top-level fn's
   inner closures are reached. */
static void walk_for_inner_fns(AST *n);

static void walk_inner_fn(AST *fn) {
    infer_function(fn);
    if (fn->as.fn_def.body) walk_for_inner_fns(fn->as.fn_def.body);
}

static void walk_for_inner_fns(AST *n) {
    if (!n) return;
    switch (n->kind) {
        case NODE_FN: walk_inner_fn(n); return;
        case NODE_BLOCK:
            for (int i = 0; i < n->as.block.count; i++)
                walk_for_inner_fns(n->as.block.stmts[i]);
            return;
        case NODE_IF:
            walk_for_inner_fns(n->as.if_stmt.cond);
            walk_for_inner_fns(n->as.if_stmt.then_branch);
            walk_for_inner_fns(n->as.if_stmt.else_branch);
            return;
        case NODE_WHILE:
            walk_for_inner_fns(n->as.while_stmt.cond);
            walk_for_inner_fns(n->as.while_stmt.body);
            return;
        case NODE_FOR:
            walk_for_inner_fns(n->as.for_stmt.init);
            walk_for_inner_fns(n->as.for_stmt.cond);
            walk_for_inner_fns(n->as.for_stmt.step);
            walk_for_inner_fns(n->as.for_stmt.body);
            return;
        case NODE_LET:    walk_for_inner_fns(n->as.let_stmt.expr); return;
        case NODE_ASSIGN: walk_for_inner_fns(n->as.assign_stmt.expr); return;
        case NODE_PRINT:  walk_for_inner_fns(n->as.print_stmt.expr); return;
        case NODE_RETURN: walk_for_inner_fns(n->as.return_stmt.expr); return;
        case NODE_BINOP:
            walk_for_inner_fns(n->as.binop.left);
            walk_for_inner_fns(n->as.binop.right);
            return;
        case NODE_UNOP:   walk_for_inner_fns(n->as.unop.operand); return;
        case NODE_CALL: {
            if (n->as.call.callee) walk_for_inner_fns(n->as.call.callee);
            for (int i = 0; i < n->as.call.arg_count; i++)
                walk_for_inner_fns(n->as.call.args[i]);
            return;
        }
        case NODE_ARRAY_LIT:
            for (int i = 0; i < n->as.array_lit.count; i++)
                walk_for_inner_fns(n->as.array_lit.items[i]);
            return;
        case NODE_MAP_LIT:
            for (int i = 0; i < n->as.map_lit.count; i++) {
                walk_for_inner_fns(n->as.map_lit.keys[i]);
                walk_for_inner_fns(n->as.map_lit.values[i]);
            }
            return;
        case NODE_INDEX:
            walk_for_inner_fns(n->as.index.target);
            walk_for_inner_fns(n->as.index.index);
            return;
        case NODE_INDEX_ASSIGN:
            walk_for_inner_fns(n->as.index_assign.target);
            walk_for_inner_fns(n->as.index_assign.index);
            walk_for_inner_fns(n->as.index_assign.value);
            return;
        case NODE_INDEX_OPASSIGN:
            walk_for_inner_fns(n->as.index_opassign.target);
            walk_for_inner_fns(n->as.index_opassign.index);
            walk_for_inner_fns(n->as.index_opassign.value);
            return;
        default: return;
    }
}

/* --- Top-level driver --------------------------------------------- */

void infer_program_types(const Program *prog) {
    collect_fns(prog);

    /* Analyze each top-level function. */
    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind == NODE_FN && n->as.fn_def.name[0] != '\0') {
            infer_function(n);
            if (n->as.fn_def.body) walk_for_inner_fns(n->as.fn_def.body);
        }
    }

    /* Analyze the top-level statement sequence as one implicit
       function body — same flow rules, separate env. */
    TiEnv env;
    env_init(&env);
    env_push_scope(&env);
    for (int i = 0; i < prog->count; i++) {
        AST *n = prog->items[i];
        if (n->kind == NODE_FN) {
            if (n->as.fn_def.name[0] != '\0')
                env_bind(&env, n->as.fn_def.name, TS_FN_BIT);
            continue;
        }
        infer_stmt(n, &env);
        walk_for_inner_fns(n);
    }
    env_pop_scope(&env);
}
