#include "ast.h"

const char *type_annot_name(TypeAnnot t) {
    switch (t) {
        case TYPE_NUM:  return "num";
        case TYPE_STR:  return "str";
        case TYPE_ARR:  return "arr";
        case TYPE_BOOL: return "bool";
        case TYPE_MAP:  return "map";
        case TYPE_FN:   return "fn";
        case TYPE_ANY:  return "any";
    }
    return "?";
}

static AST *make(NodeKind k) {
    AST *n = (AST *)cl_track_calloc(sizeof(AST));
    n->kind = k;
    return n;
}

AST *ast_number(double v) {
    AST *n = make(NODE_NUMBER);
    n->as.number = v;
    return n;
}

AST *ast_string(const char *s) {
    AST *n = make(NODE_STRING);
    cl_strncpy_z(n->as.string, s, CL_MAX_TEXT);
    return n;
}

AST *ast_var(const char *name) {
    AST *n = make(NODE_VAR);
    cl_strncpy_z(n->as.var, name, CL_MAX_TEXT);
    return n;
}

AST *ast_binop(TokenType op, AST *l, AST *r) {
    AST *n = make(NODE_BINOP);
    n->as.binop.op    = op;
    n->as.binop.left  = l;
    n->as.binop.right = r;
    return n;
}

AST *ast_unop(TokenType op, AST *operand) {
    AST *n = make(NODE_UNOP);
    n->as.unop.op      = op;
    n->as.unop.operand = operand;
    return n;
}

AST *ast_let(const char *name, TypeAnnot type, AST *expr) {
    AST *n = make(NODE_LET);
    cl_strncpy_z(n->as.let_stmt.name, name, CL_MAX_TEXT);
    n->as.let_stmt.type = type;
    n->as.let_stmt.expr = expr;
    return n;
}

AST *ast_assign(const char *name, AST *expr) {
    AST *n = make(NODE_ASSIGN);
    cl_strncpy_z(n->as.assign_stmt.name, name, CL_MAX_TEXT);
    n->as.assign_stmt.expr = expr;
    return n;
}

AST *ast_print(AST *expr) {
    AST *n = make(NODE_PRINT);
    n->as.print_stmt.expr = expr;
    return n;
}

AST *ast_if(AST *cond, AST *then_branch, AST *else_branch) {
    AST *n = make(NODE_IF);
    n->as.if_stmt.cond         = cond;
    n->as.if_stmt.then_branch  = then_branch;
    n->as.if_stmt.else_branch  = else_branch;
    return n;
}

AST *ast_while(AST *cond, AST *body) {
    AST *n = make(NODE_WHILE);
    n->as.while_stmt.cond = cond;
    n->as.while_stmt.body = body;
    return n;
}

AST *ast_for(AST *init, AST *cond, AST *step, AST *body) {
    AST *n = make(NODE_FOR);
    n->as.for_stmt.init = init;
    n->as.for_stmt.cond = cond;
    n->as.for_stmt.step = step;
    n->as.for_stmt.body = body;
    return n;
}

AST *ast_break(void)    { return make(NODE_BREAK); }
AST *ast_continue(void) { return make(NODE_CONTINUE); }

AST *ast_block(void) {
    AST *n = make(NODE_BLOCK);
    n->as.block.stmts = NULL;
    n->as.block.count = 0;
    n->as.block.cap   = 0;
    return n;
}

void ast_block_add(AST *block, AST *stmt) {
    if (block->as.block.count == block->as.block.cap) {
        int new_cap = block->as.block.cap == 0 ? 8 : block->as.block.cap * 2;
        block->as.block.stmts = (AST **)cl_track_realloc(
            block->as.block.stmts, sizeof(AST *) * (size_t)new_cap);
        block->as.block.cap = new_cap;
    }
    block->as.block.stmts[block->as.block.count++] = stmt;
}

AST *ast_fn(const char *name) {
    AST *n = make(NODE_FN);
    cl_strncpy_z(n->as.fn_def.name, name, CL_MAX_TEXT);
    n->as.fn_def.param_count = 0;
    n->as.fn_def.return_type = TYPE_ANY;
    n->as.fn_def.is_public   = 0;   /* private unless promoted via `pub`   */
    n->as.fn_def.is_extern   = 0;   /* set to 1 by parser for `extern fn`. */
    n->as.fn_def.body = NULL;
    return n;
}

void ast_fn_set_public(AST *fn, int is_public) {
    fn->as.fn_def.is_public = is_public ? 1 : 0;
}

void ast_fn_set_extern(AST *fn, int is_extern) {
    fn->as.fn_def.is_extern = is_extern ? 1 : 0;
}

void ast_fn_add_param(AST *fn, const char *param, TypeAnnot type) {
    if (fn->as.fn_def.param_count >= CL_MAX_PARAMS) cl_die("too many function parameters");
    int i = fn->as.fn_def.param_count;
    cl_strncpy_z(fn->as.fn_def.params[i], param, CL_MAX_TEXT);
    fn->as.fn_def.param_types[i] = type;
    fn->as.fn_def.param_count++;
}

void ast_fn_set_return_type(AST *fn, TypeAnnot type) {
    fn->as.fn_def.return_type = type;
}

void ast_fn_set_body(AST *fn, AST *body) {
    fn->as.fn_def.body = body;
}

AST *ast_call(const char *name) {
    AST *n = make(NODE_CALL);
    cl_strncpy_z(n->as.call.name, name, CL_MAX_TEXT);
    n->as.call.callee    = NULL;
    n->as.call.args      = NULL;
    n->as.call.arg_count = 0;
    n->as.call.arg_cap   = 0;
    return n;
}

AST *ast_call_indirect(AST *callee) {
    AST *n = make(NODE_CALL);
    n->as.call.name[0]   = '\0';
    n->as.call.callee    = callee;
    n->as.call.args      = NULL;
    n->as.call.arg_count = 0;
    n->as.call.arg_cap   = 0;
    return n;
}

void ast_call_add_arg(AST *call, AST *arg) {
    if (call->as.call.arg_count == call->as.call.arg_cap) {
        int new_cap = call->as.call.arg_cap == 0 ? 4 : call->as.call.arg_cap * 2;
        call->as.call.args = (AST **)cl_track_realloc(
            call->as.call.args, sizeof(AST *) * (size_t)new_cap);
        call->as.call.arg_cap = new_cap;
    }
    call->as.call.args[call->as.call.arg_count++] = arg;
}

AST *ast_return(AST *expr) {
    AST *n = make(NODE_RETURN);
    n->as.return_stmt.expr = expr;
    return n;
}

AST *ast_array_lit(void) {
    AST *n = make(NODE_ARRAY_LIT);
    n->as.array_lit.items = NULL;
    n->as.array_lit.count = 0;
    n->as.array_lit.cap   = 0;
    return n;
}

void ast_array_lit_add(AST *lit, AST *item) {
    if (lit->as.array_lit.count == lit->as.array_lit.cap) {
        int new_cap = lit->as.array_lit.cap == 0 ? 4 : lit->as.array_lit.cap * 2;
        lit->as.array_lit.items = (AST **)cl_track_realloc(
            lit->as.array_lit.items, sizeof(AST *) * (size_t)new_cap);
        lit->as.array_lit.cap = new_cap;
    }
    lit->as.array_lit.items[lit->as.array_lit.count++] = item;
}

AST *ast_index(AST *target, AST *index) {
    AST *n = make(NODE_INDEX);
    n->as.index.target = target;
    n->as.index.index  = index;
    return n;
}

AST *ast_index_assign(AST *target, AST *index, AST *value) {
    AST *n = make(NODE_INDEX_ASSIGN);
    n->as.index_assign.target = target;
    n->as.index_assign.index  = index;
    n->as.index_assign.value  = value;
    return n;
}

AST *ast_index_opassign(AST *target, AST *index, TokenType op, AST *value) {
    AST *n = make(NODE_INDEX_OPASSIGN);
    n->as.index_opassign.target = target;
    n->as.index_opassign.index  = index;
    n->as.index_opassign.op     = op;
    n->as.index_opassign.value  = value;
    return n;
}

AST *ast_map_lit(void) {
    AST *n = make(NODE_MAP_LIT);
    n->as.map_lit.keys   = NULL;
    n->as.map_lit.values = NULL;
    n->as.map_lit.count  = 0;
    n->as.map_lit.cap    = 0;
    return n;
}

AST *ast_try(AST *try_body, const char *catch_name, AST *catch_body) {
    AST *n = make(NODE_TRY);
    n->as.try_stmt.try_body = try_body;
    cl_strncpy_z(n->as.try_stmt.catch_name, catch_name, CL_MAX_TEXT);
    n->as.try_stmt.catch_body = catch_body;
    return n;
}

AST *ast_throw(AST *expr) {
    AST *n = make(NODE_THROW);
    n->as.throw_stmt.expr = expr;
    return n;
}

void ast_map_lit_add(AST *m, AST *key, AST *value) {
    if (m->as.map_lit.count >= m->as.map_lit.cap) {
        int nc = m->as.map_lit.cap == 0 ? 4 : m->as.map_lit.cap * 2;
        m->as.map_lit.keys   = (AST **)cl_track_realloc(m->as.map_lit.keys,   sizeof(AST *) * (size_t)nc);
        m->as.map_lit.values = (AST **)cl_track_realloc(m->as.map_lit.values, sizeof(AST *) * (size_t)nc);
        m->as.map_lit.cap = nc;
    }
    m->as.map_lit.keys[m->as.map_lit.count]   = key;
    m->as.map_lit.values[m->as.map_lit.count] = value;
    m->as.map_lit.count++;
}

static void ind(int n) { for (int i = 0; i < n; i++) putchar(' '); }

void ast_print_debug(AST *n, int indent) {
    if (!n) return;
    ind(indent);
    switch (n->kind) {
        case NODE_NUMBER:   printf("Number(%g)\n", n->as.number); break;
        case NODE_STRING:   printf("String(%s)\n", n->as.string); break;
        case NODE_VAR:      printf("Var(%s)\n",    n->as.var);    break;
        case NODE_BINOP:
            printf("BinOp(%s)\n", token_type_name(n->as.binop.op));
            ast_print_debug(n->as.binop.left,  indent + 2);
            ast_print_debug(n->as.binop.right, indent + 2);
            break;
        case NODE_UNOP:
            printf("UnOp(%s)\n", token_type_name(n->as.unop.op));
            ast_print_debug(n->as.unop.operand, indent + 2);
            break;
        case NODE_LET:
            if (n->as.let_stmt.type != TYPE_ANY) {
                printf("Let(%s: %s)\n",
                    n->as.let_stmt.name,
                    type_annot_name(n->as.let_stmt.type));
            } else {
                printf("Let(%s)\n", n->as.let_stmt.name);
            }
            ast_print_debug(n->as.let_stmt.expr, indent + 2);
            break;
        case NODE_ASSIGN:
            printf("Assign(%s)\n", n->as.assign_stmt.name);
            ast_print_debug(n->as.assign_stmt.expr, indent + 2);
            break;
        case NODE_PRINT:
            printf("Print\n");
            ast_print_debug(n->as.print_stmt.expr, indent + 2);
            break;
        case NODE_IF:
            printf("If\n");
            ast_print_debug(n->as.if_stmt.cond,        indent + 2);
            ast_print_debug(n->as.if_stmt.then_branch, indent + 2);
            if (n->as.if_stmt.else_branch)
                ast_print_debug(n->as.if_stmt.else_branch, indent + 2);
            break;
        case NODE_WHILE:
            printf("While\n");
            ast_print_debug(n->as.while_stmt.cond, indent + 2);
            ast_print_debug(n->as.while_stmt.body, indent + 2);
            break;
        case NODE_FOR:
            printf("For\n");
            ast_print_debug(n->as.for_stmt.init, indent + 2);
            ast_print_debug(n->as.for_stmt.cond, indent + 2);
            ast_print_debug(n->as.for_stmt.step, indent + 2);
            ast_print_debug(n->as.for_stmt.body, indent + 2);
            break;
        case NODE_BREAK:    printf("Break\n");    break;
        case NODE_CONTINUE: printf("Continue\n"); break;
        case NODE_BLOCK:
            printf("Block\n");
            for (int i = 0; i < n->as.block.count; i++)
                ast_print_debug(n->as.block.stmts[i], indent + 2);
            break;
        case NODE_FN:
            printf("Fn(%s%s%s, (",
                n->as.fn_def.is_public ? "pub " : "",
                n->as.fn_def.is_extern ? "extern " : "",
                n->as.fn_def.name);
            for (int i = 0; i < n->as.fn_def.param_count; i++) {
                if (i > 0) printf(", ");
                printf("%s: %s",
                    n->as.fn_def.params[i],
                    type_annot_name(n->as.fn_def.param_types[i]));
            }
            printf("): %s)\n", type_annot_name(n->as.fn_def.return_type));
            ast_print_debug(n->as.fn_def.body, indent + 2);
            break;
        case NODE_CALL:
            if (n->as.call.callee) {
                printf("CallIndirect\n");
                ast_print_debug(n->as.call.callee, indent + 2);
            } else {
                printf("Call(%s)\n", n->as.call.name);
            }
            for (int i = 0; i < n->as.call.arg_count; i++)
                ast_print_debug(n->as.call.args[i], indent + 2);
            break;
        case NODE_RETURN:
            printf("Return\n");
            if (n->as.return_stmt.expr)
                ast_print_debug(n->as.return_stmt.expr, indent + 2);
            break;
        case NODE_ARRAY_LIT:
            printf("Array(%d items)\n", n->as.array_lit.count);
            for (int i = 0; i < n->as.array_lit.count; i++)
                ast_print_debug(n->as.array_lit.items[i], indent + 2);
            break;
        case NODE_INDEX:
            printf("Index\n");
            ast_print_debug(n->as.index.target, indent + 2);
            ast_print_debug(n->as.index.index,  indent + 2);
            break;
        case NODE_INDEX_ASSIGN:
            printf("IndexAssign\n");
            ast_print_debug(n->as.index_assign.target, indent + 2);
            ast_print_debug(n->as.index_assign.index,  indent + 2);
            ast_print_debug(n->as.index_assign.value,  indent + 2);
            break;
        case NODE_INDEX_OPASSIGN:
            printf("IndexOpAssign(%s)\n", token_type_name(n->as.index_opassign.op));
            ast_print_debug(n->as.index_opassign.target, indent + 2);
            ast_print_debug(n->as.index_opassign.index,  indent + 2);
            ast_print_debug(n->as.index_opassign.value,  indent + 2);
            break;
        case NODE_MAP_LIT:
            printf("Map(%d entries)\n", n->as.map_lit.count);
            for (int i = 0; i < n->as.map_lit.count; i++) {
                ast_print_debug(n->as.map_lit.keys[i],   indent + 2);
                ast_print_debug(n->as.map_lit.values[i], indent + 2);
            }
            break;
        case NODE_TRY:
            printf("Try(catch %s)\n", n->as.try_stmt.catch_name);
            ast_print_debug(n->as.try_stmt.try_body,   indent + 2);
            ast_print_debug(n->as.try_stmt.catch_body, indent + 2);
            break;
        case NODE_THROW:
            printf("Throw\n");
            ast_print_debug(n->as.throw_stmt.expr, indent + 2);
            break;
    }
}

static void free_node(AST *n) {
    if (!n) return;
    switch (n->kind) {
        case NODE_BINOP:
            free_node(n->as.binop.left);
            free_node(n->as.binop.right);
            break;
        case NODE_UNOP:
            free_node(n->as.unop.operand);
            break;
        case NODE_LET:
            free_node(n->as.let_stmt.expr);
            break;
        case NODE_ASSIGN:
            free_node(n->as.assign_stmt.expr);
            break;
        case NODE_PRINT:
            free_node(n->as.print_stmt.expr);
            break;
        case NODE_IF:
            free_node(n->as.if_stmt.cond);
            free_node(n->as.if_stmt.then_branch);
            free_node(n->as.if_stmt.else_branch);
            break;
        case NODE_WHILE:
            free_node(n->as.while_stmt.cond);
            free_node(n->as.while_stmt.body);
            break;
        case NODE_FOR:
            free_node(n->as.for_stmt.init);
            free_node(n->as.for_stmt.cond);
            free_node(n->as.for_stmt.step);
            free_node(n->as.for_stmt.body);
            break;
        case NODE_BLOCK:
            for (int i = 0; i < n->as.block.count; i++)
                free_node(n->as.block.stmts[i]);
            if (n->as.block.stmts) cl_track_free(n->as.block.stmts);
            break;
        case NODE_FN:
            free_node(n->as.fn_def.body);
            break;
        case NODE_CALL:
            if (n->as.call.callee) free_node(n->as.call.callee);
            for (int i = 0; i < n->as.call.arg_count; i++)
                free_node(n->as.call.args[i]);
            if (n->as.call.args) cl_track_free(n->as.call.args);
            break;
        case NODE_RETURN:
            free_node(n->as.return_stmt.expr);
            break;
        case NODE_ARRAY_LIT:
            for (int i = 0; i < n->as.array_lit.count; i++)
                free_node(n->as.array_lit.items[i]);
            if (n->as.array_lit.items) cl_track_free(n->as.array_lit.items);
            break;
        case NODE_INDEX:
            free_node(n->as.index.target);
            free_node(n->as.index.index);
            break;
        case NODE_INDEX_ASSIGN:
            free_node(n->as.index_assign.target);
            free_node(n->as.index_assign.index);
            free_node(n->as.index_assign.value);
            break;
        case NODE_INDEX_OPASSIGN:
            free_node(n->as.index_opassign.target);
            free_node(n->as.index_opassign.index);
            free_node(n->as.index_opassign.value);
            break;
        case NODE_MAP_LIT:
            for (int i = 0; i < n->as.map_lit.count; i++) {
                free_node(n->as.map_lit.keys[i]);
                free_node(n->as.map_lit.values[i]);
            }
            if (n->as.map_lit.keys)   cl_track_free(n->as.map_lit.keys);
            if (n->as.map_lit.values) cl_track_free(n->as.map_lit.values);
            break;
        case NODE_TRY:
            free_node(n->as.try_stmt.try_body);
            free_node(n->as.try_stmt.catch_body);
            break;
        case NODE_THROW:
            free_node(n->as.throw_stmt.expr);
            break;
        case NODE_NUMBER:
        case NODE_STRING:
        case NODE_VAR:
        case NODE_BREAK:
        case NODE_CONTINUE:
            break;
    }
    cl_track_free(n);
}

void program_free(Program *p) {
    for (int i = 0; i < p->count; i++) free_node(p->items[i]);
    p->count = 0;
}
