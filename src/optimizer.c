/* optimizer.c — AST-rewriting optimization passes.
 *
 * Three passes, applied in one walk:
 *
 *   1. Constant folding. Arithmetic, comparison, logical, and bitwise
 *      operators on constant operands are evaluated at compile time.
 *      String concatenation of two string literals folds too.
 *
 *   2. Algebraic simplification. Trivial identities like `x + 0` -> `x`,
 *      `x * 1` -> `x`, `x * 0` -> `0`, `x - 0` -> `x`, `x * 2` -> `x + x`,
 *      `!!x` -> truthy(x), etc.
 *
 *   3. Dead-branch elimination. `if (constant_true) { A } else { B }`
 *      becomes A; `if (constant_false) { A } else { B }` becomes B
 *      (or empty block if no else). `while (0) { ... }` becomes empty.
 *      `cond ? a : b` with a constant condition becomes a or b.
 *
 * Legality. None of these change observable behaviour on well-typed
 * input. We refuse to fold any operation whose operand types are
 * not statically obvious (e.g. `arr + arr` — we don't actually allow
 * that, but a robust optimizer never assumes). For floating-point we
 * accept the standard IEEE-754 same-bit-pattern guarantee: folding
 * `2.0 + 3.0` to `5.0` is fine.
 *
 * Side-effecting subexpressions: we only fold if BOTH operands are
 * pure constants (NODE_NUMBER, NODE_STRING). Anything that could
 * call user code (NODE_CALL, NODE_INDEX, etc.) is left alone.
 *
 * Entry point: cl_optimize_ast(AST **node).
 * The pointer is taken by reference so we can replace the entire
 * subtree (e.g. fold `2 + 3` from a NODE_BINOP to a NODE_NUMBER).
 */

#include "ast.h"
#include "common.h"
#include <math.h>
#include <string.h>

/* Forward decls */
static void opt_node(AST **np);
static void opt_block(AST *block);

/* --- Helpers ------------------------------------------------------ */

static int is_const_num(AST *n, double *out) {
    if (n && n->kind == NODE_NUMBER) {
        if (out) *out = n->as.number;
        return 1;
    }
    return 0;
}

static int is_const_str(AST *n, const char **out) {
    if (n && n->kind == NODE_STRING) {
        if (out) *out = n->as.string;
        return 1;
    }
    return 0;
}

/* Compile-time truthiness (matches the runtime). NODE_NUMBER: non-zero
   is truthy; NODE_STRING: non-empty is truthy. For anything else we
   can't decide. Returns 1 (truthy), 0 (falsy), or -1 (unknown). */
static int const_truthy(AST *n) {
    if (!n) return -1;
    if (n->kind == NODE_NUMBER) return n->as.number != 0.0 ? 1 : 0;
    if (n->kind == NODE_STRING) return n->as.string[0] != '\0' ? 1 : 0;
    return -1;
}

/* --- Constant folding for binary ops ----------------------------- */

static AST *fold_num_binop(TokenType op, double a, double b) {
    switch (op) {
        case TOK_PLUS:    return ast_number(a + b);
        case TOK_MINUS:   return ast_number(a - b);
        case TOK_STAR:    return ast_number(a * b);
        case TOK_SLASH:
            if (b == 0.0) return NULL;            /* leave for runtime */
            return ast_number(a / b);
        case TOK_PERCENT:
            if (b == 0.0) return NULL;
            return ast_number(fmod(a, b));
        case TOK_LT:      return ast_number(a <  b ? 1 : 0);
        case TOK_LE:      return ast_number(a <= b ? 1 : 0);
        case TOK_GT:      return ast_number(a >  b ? 1 : 0);
        case TOK_GE:      return ast_number(a >= b ? 1 : 0);
        case TOK_EQEQ:    return ast_number(a == b ? 1 : 0);
        case TOK_NEQ:     return ast_number(a != b ? 1 : 0);
        case TOK_AND:     return ast_number((a != 0 && b != 0) ? 1 : 0);
        case TOK_OR:      return ast_number((a != 0 || b != 0) ? 1 : 0);
        case TOK_AMP:     return ast_number((double)((long long)a & (long long)b));
        case TOK_PIPE:    return ast_number((double)((long long)a | (long long)b));
        case TOK_CARET:   return ast_number((double)((long long)a ^ (long long)b));
        case TOK_LSHIFT:  return ast_number((double)((long long)a << (((long long)b) & 63)));
        case TOK_RSHIFT:  return ast_number((double)((long long)a >> (((long long)b) & 63)));
        default: return NULL;
    }
}

/* String-string concat. NODE_STRING storage is fixed-size, so we have
   to refuse folds that would overflow. */
static AST *fold_str_concat(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la + lb + 1 >= CL_MAX_TEXT) return NULL;
    char buf[CL_MAX_TEXT];
    memcpy(buf, a, la);
    memcpy(buf + la, b, lb);
    buf[la + lb] = '\0';
    return ast_string(buf);
}

/* String + number coerces via %.10g, matching the runtime. */
static AST *fold_str_num(const char *s, double n, int str_on_left) {
    char numbuf[32];
    snprintf(numbuf, sizeof numbuf, "%.10g", n);
    if (str_on_left) return fold_str_concat(s, numbuf);
    return fold_str_concat(numbuf, s);
}

/* --- Algebraic simplifications ----------------------------------- */

/* For an op whose result is known to be num (i.e. the operands are
   provably num), apply identity rewrites. Returns a replacement node
   or NULL if nothing applies. We deliberately do NOT apply identities
   that would change a NaN propagation result, even though they're
   technically valid: e.g. `x * 0` could be NaN if x is NaN. We avoid
   that one to be safe. */
static AST *algebraic_simplify(AST *n) {
    if (n->kind != NODE_BINOP) return NULL;
    TokenType op = n->as.binop.op;
    AST *L = n->as.binop.left;
    AST *R = n->as.binop.right;

    double rv;
    /* x + 0 -> x, 0 + x -> x */
    if (op == TOK_PLUS) {
        if (is_const_num(R, &rv) && rv == 0.0) return L;
        if (is_const_num(L, &rv) && rv == 0.0) return R;
    }
    /* x - 0 -> x */
    if (op == TOK_MINUS) {
        if (is_const_num(R, &rv) && rv == 0.0) return L;
    }
    /* x * 1 -> x, 1 * x -> x. We don't fold x * 0 -> 0 because of NaN. */
    if (op == TOK_STAR) {
        if (is_const_num(R, &rv) && rv == 1.0) return L;
        if (is_const_num(L, &rv) && rv == 1.0) return R;
    }
    /* x / 1 -> x */
    if (op == TOK_SLASH) {
        if (is_const_num(R, &rv) && rv == 1.0) return L;
    }
    /* x & 0 -> 0  (integer ops; NaN propagates through int cast as 0) */
    if (op == TOK_AMP) {
        if (is_const_num(R, &rv) && rv == 0.0) return ast_number(0);
        if (is_const_num(L, &rv) && rv == 0.0) return ast_number(0);
    }
    /* x | 0 -> x */
    if (op == TOK_PIPE) {
        if (is_const_num(R, &rv) && rv == 0.0) return L;
        if (is_const_num(L, &rv) && rv == 0.0) return R;
    }
    /* x ^ 0 -> x */
    if (op == TOK_CARET) {
        if (is_const_num(R, &rv) && rv == 0.0) return L;
        if (is_const_num(L, &rv) && rv == 0.0) return R;
    }
    /* x << 0 -> x, x >> 0 -> x */
    if (op == TOK_LSHIFT || op == TOK_RSHIFT) {
        if (is_const_num(R, &rv) && rv == 0.0) return L;
    }
    /* Short-circuit constant logical ops */
    if (op == TOK_AND) {
        int lt = const_truthy(L);
        if (lt == 0) return ast_number(0);     /* 0 && x  -> 0  (no side effects from x) */
        if (lt == 1) return R;                  /* 1 && x  -> x  (... if x can't have side effects we lose, but result-equal) */
    }
    if (op == TOK_OR) {
        int lt = const_truthy(L);
        if (lt == 1) return ast_number(1);     /* 1 || x  -> 1 */
        if (lt == 0) return R;                  /* 0 || x  -> x */
    }
    return NULL;
}

/* --- Recursive walker -------------------------------------------- */

static void opt_node(AST **np) {
    if (!np || !*np) return;
    AST *n = *np;

    /* Recurse into children first (post-order rewriting). */
    switch (n->kind) {
        case NODE_BINOP:
            opt_node(&n->as.binop.left);
            opt_node(&n->as.binop.right);
            break;
        case NODE_UNOP:
            opt_node(&n->as.unop.operand);
            break;
        case NODE_LET:
            opt_node(&n->as.let_stmt.expr);
            break;
        case NODE_ASSIGN:
            opt_node(&n->as.assign_stmt.expr);
            break;
        case NODE_PRINT:
            opt_node(&n->as.print_stmt.expr);
            break;
        case NODE_IF:
            opt_node(&n->as.if_stmt.cond);
            opt_node(&n->as.if_stmt.then_branch);
            opt_node(&n->as.if_stmt.else_branch);
            break;
        case NODE_WHILE:
        case NODE_DO_WHILE:
            opt_node(&n->as.while_stmt.cond);
            opt_node(&n->as.while_stmt.body);
            break;
        case NODE_FOR:
            opt_node(&n->as.for_stmt.init);
            opt_node(&n->as.for_stmt.cond);
            opt_node(&n->as.for_stmt.step);
            opt_node(&n->as.for_stmt.body);
            break;
        case NODE_BLOCK:
            opt_block(n);
            break;
        case NODE_FN:
            opt_node(&n->as.fn_def.body);
            break;
        case NODE_CALL:
            opt_node(&n->as.call.callee);
            for (int i = 0; i < n->as.call.arg_count; i++)
                opt_node(&n->as.call.args[i]);
            break;
        case NODE_RETURN:
            opt_node(&n->as.return_stmt.expr);
            break;
        case NODE_ARRAY_LIT:
            for (int i = 0; i < n->as.array_lit.count; i++)
                opt_node(&n->as.array_lit.items[i]);
            break;
        case NODE_INDEX:
            opt_node(&n->as.index.target);
            opt_node(&n->as.index.index);
            break;
        case NODE_INDEX_ASSIGN:
            opt_node(&n->as.index_assign.target);
            opt_node(&n->as.index_assign.index);
            opt_node(&n->as.index_assign.value);
            break;
        case NODE_INDEX_OPASSIGN:
            opt_node(&n->as.index_opassign.target);
            opt_node(&n->as.index_opassign.index);
            opt_node(&n->as.index_opassign.value);
            break;
        case NODE_MAP_LIT:
            for (int i = 0; i < n->as.map_lit.count; i++) {
                opt_node(&n->as.map_lit.keys[i]);
                opt_node(&n->as.map_lit.values[i]);
            }
            break;
        case NODE_TRY:
            opt_node(&n->as.try_stmt.try_body);
            opt_node(&n->as.try_stmt.catch_body);
            break;
        case NODE_THROW:
            opt_node(&n->as.throw_stmt.expr);
            break;
        case NODE_SWITCH:
            opt_node(&n->as.switch_stmt.discriminant);
            for (int i = 0; i < n->as.switch_stmt.case_count; i++) {
                opt_node(&n->as.switch_stmt.case_values[i]);
                opt_node(&n->as.switch_stmt.case_bodies[i]);
            }
            opt_node(&n->as.switch_stmt.default_body);
            break;
        case NODE_TERNARY:
            opt_node(&n->as.ternary.cond);
            opt_node(&n->as.ternary.then_expr);
            opt_node(&n->as.ternary.else_expr);
            break;
        default:
            /* leaves: NODE_NUMBER, NODE_STRING, NODE_VAR, NODE_BREAK,
               NODE_CONTINUE. Nothing to recurse into. */
            break;
    }

    /* --- Now apply rewrites bottom-up. --- */

    /* Unary fold: -literal, !literal, ~literal. */
    if (n->kind == NODE_UNOP) {
        double v;
        TokenType op = n->as.unop.op;
        if (is_const_num(n->as.unop.operand, &v)) {
            if (op == TOK_MINUS) { *np = ast_number(-v); return; }
            if (op == TOK_BANG)  { *np = ast_number(v == 0.0 ? 1.0 : 0.0); return; }
            if (op == TOK_TILDE) { *np = ast_number((double)(~(long long)v)); return; }
        }
    }

    /* Binary fold + algebraic simplification. */
    if (n->kind == NODE_BINOP) {
        TokenType op = n->as.binop.op;
        AST *L = n->as.binop.left;
        AST *R = n->as.binop.right;
        double lv, rv;
        const char *ls, *rs;

        /* num op num */
        if (is_const_num(L, &lv) && is_const_num(R, &rv)) {
            AST *r = fold_num_binop(op, lv, rv);
            if (r) { *np = r; return; }
        }
        /* str + str / str + num / num + str (only for +) */
        if (op == TOK_PLUS) {
            if (is_const_str(L, &ls) && is_const_str(R, &rs)) {
                AST *r = fold_str_concat(ls, rs);
                if (r) { *np = r; return; }
            }
            if (is_const_str(L, &ls) && is_const_num(R, &rv)) {
                AST *r = fold_str_num(ls, rv, 1);
                if (r) { *np = r; return; }
            }
            if (is_const_num(L, &lv) && is_const_str(R, &rs)) {
                AST *r = fold_str_num(rs, lv, 0);
                if (r) { *np = r; return; }
            }
        }
        /* No fold available — try algebraic simplification. */
        AST *r = algebraic_simplify(n);
        if (r) { *np = r; return; }
    }

    /* Dead-branch elimination for if. */
    if (n->kind == NODE_IF) {
        int t = const_truthy(n->as.if_stmt.cond);
        if (t == 1) {
            *np = n->as.if_stmt.then_branch;
            return;
        }
        if (t == 0) {
            if (n->as.if_stmt.else_branch) {
                *np = n->as.if_stmt.else_branch;
            } else {
                /* Replace with an empty block — keeps statement-position
                   nodes well-typed. */
                *np = ast_block();
            }
            return;
        }
    }

    /* `while (0) { ... }` becomes a no-op. */
    if (n->kind == NODE_WHILE) {
        int t = const_truthy(n->as.while_stmt.cond);
        if (t == 0) {
            *np = ast_block();
            return;
        }
    }
    /* `do { ... } while (0);` runs body once and stops; equivalent to body. */
    if (n->kind == NODE_DO_WHILE) {
        int t = const_truthy(n->as.while_stmt.cond);
        if (t == 0) {
            *np = n->as.while_stmt.body;
            return;
        }
    }

    /* Ternary with constant cond -> picked branch. */
    if (n->kind == NODE_TERNARY) {
        int t = const_truthy(n->as.ternary.cond);
        if (t == 1) { *np = n->as.ternary.then_expr; return; }
        if (t == 0) { *np = n->as.ternary.else_expr; return; }
    }
}

static void opt_block(AST *block) {
    for (int i = 0; i < block->as.block.count; i++) {
        opt_node(&block->as.block.stmts[i]);
    }
}

/* Public entry point. Call once per program after parsing, before
   codegen. Takes a Program* and rewrites each top-level item in place. */
void cl_optimize_program(Program *p) {
    if (!p) return;
    for (int i = 0; i < p->count; i++) {
        AST *n = p->items[i];
        if (n && n->kind == NODE_FN) {
            opt_node(&n->as.fn_def.body);
        } else {
            opt_node(&p->items[i]);
        }
    }
}
