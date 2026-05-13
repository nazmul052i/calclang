#ifndef CALCLANG_AST_H
#define CALCLANG_AST_H
#include "common.h"
#include "lexer.h"

#define CL_MAX_PARAMS 16

/* Optional type annotations on function parameters and return values.
   Recorded by the parser, emitted as `.casm` signature comments, but
   not enforced at runtime — they are documentation today and an
   anchor a future type checker can hang off of. */
typedef enum {
    TYPE_ANY  = 0,
    TYPE_NUM  = 1,
    TYPE_STR  = 2,
    TYPE_ARR  = 3,
    TYPE_BOOL = 4,   /* alias for num — there is no separate boolean */
    TYPE_MAP  = 5,
    TYPE_FN   = 6
} TypeAnnot;

const char *type_annot_name(TypeAnnot t);

typedef enum {
    NODE_NUMBER,
    NODE_STRING,
    NODE_VAR,
    NODE_BINOP,
    NODE_UNOP,
    NODE_LET,
    NODE_ASSIGN,
    NODE_PRINT,
    NODE_IF,
    NODE_WHILE,
    NODE_FOR,
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_BLOCK,
    NODE_FN,
    NODE_CALL,
    NODE_RETURN,
    NODE_ARRAY_LIT,
    NODE_INDEX,
    NODE_INDEX_ASSIGN,
    NODE_INDEX_OPASSIGN,
    NODE_MAP_LIT,
    NODE_TRY,
    NODE_THROW
} NodeKind;

typedef struct AST AST;
struct AST {
    NodeKind kind;
    /* Per-use inferred type, filled in by the flow-sensitive type
       inference pass. Only meaningful on NODE_VAR (and is otherwise
       0/TYPE_ANY). When non-zero, the inference engine has proven the
       variable has exactly this type at this program point, even if
       other uses elsewhere see a different type. */
    int inferred_type;
    union {
        double number;
        char   string[CL_MAX_TEXT];
        char   var[CL_MAX_TEXT];
        struct { TokenType op; AST *left; AST *right; } binop;
        struct { TokenType op; AST *operand; }          unop;
        struct { char name[CL_MAX_TEXT]; TypeAnnot type; AST *expr; } let_stmt;
        struct { char name[CL_MAX_TEXT]; AST *expr; }   assign_stmt;
        struct { AST *expr; }                           print_stmt;
        struct { AST *cond; AST *then_branch; AST *else_branch; } if_stmt;
        struct { AST *cond; AST *body; }                while_stmt;
        struct { AST *init; AST *cond; AST *step; AST *body; } for_stmt;
        struct { AST **stmts; int count; int cap; }     block;
        struct {
            char      name[CL_MAX_TEXT];
            char      params[CL_MAX_PARAMS][CL_MAX_TEXT];
            TypeAnnot param_types[CL_MAX_PARAMS];
            int       param_count;
            TypeAnnot return_type;
            int       is_public;   /* 1 if declared with `pub fn ...` */
            int       is_extern;   /* 1 if declared with `extern fn ...;` (no body) */
            AST      *body;
        } fn_def;
        struct {
            /* When `callee` is NULL the call is dispatched by `name`
               (calclib builtin, named user fn, or a variable holding a
               function value — resolved in that order at codegen).
               When `callee` is non-NULL the call is indirect: codegen
               evaluates `callee` to a function value and CALL_VALs it.
               This is what `obj.method(args)` and `arr[i](args)` lower
               to — there is no name to look up. */
            char  name[CL_MAX_TEXT];
            AST  *callee;
            AST **args;
            int   arg_count;
            int   arg_cap;
        } call;
        struct { AST *expr; }                           return_stmt; /* expr may be NULL */
        struct { AST **items; int count; int cap; }     array_lit;
        struct { AST *target; AST *index; }             index;
        struct { AST *target; AST *index; AST *value; } index_assign;
        struct { AST *target; AST *index; TokenType op; AST *value; } index_opassign;
        struct {
            AST **keys;
            AST **values;
            int   count;
            int   cap;
        } map_lit;
        struct {
            AST *try_body;
            char catch_name[CL_MAX_TEXT];
            AST *catch_body;
        } try_stmt;
        struct { AST *expr; } throw_stmt;
    } as;
};

typedef struct {
    AST *items[CL_MAX_NODES];
    int  count;
} Program;

AST *ast_number(double v);
AST *ast_string(const char *s);
AST *ast_var(const char *name);
AST *ast_binop(TokenType op, AST *left, AST *right);
AST *ast_unop(TokenType op, AST *operand);
AST *ast_let(const char *name, TypeAnnot type, AST *expr);
AST *ast_assign(const char *name, AST *expr);
AST *ast_print(AST *expr);
AST *ast_if(AST *cond, AST *then_branch, AST *else_branch);
AST *ast_while(AST *cond, AST *body);
AST *ast_for(AST *init, AST *cond, AST *step, AST *body);
AST *ast_break(void);
AST *ast_continue(void);
AST *ast_block(void);
void ast_block_add(AST *block, AST *stmt);
AST *ast_fn(const char *name);
void ast_fn_add_param(AST *fn, const char *param, TypeAnnot type);
void ast_fn_set_return_type(AST *fn, TypeAnnot type);
void ast_fn_set_public(AST *fn, int is_public);
void ast_fn_set_extern(AST *fn, int is_extern);
void ast_fn_set_body(AST *fn, AST *body);
AST *ast_call(const char *name);
AST *ast_call_indirect(AST *callee);
void ast_call_add_arg(AST *call, AST *arg);
AST *ast_return(AST *expr);
AST *ast_array_lit(void);
void ast_array_lit_add(AST *lit, AST *item);
AST *ast_index(AST *target, AST *index);
AST *ast_index_assign(AST *target, AST *index, AST *value);
AST *ast_index_opassign(AST *target, AST *index, TokenType op, AST *value);
AST *ast_map_lit(void);
void ast_map_lit_add(AST *m, AST *key, AST *value);
AST *ast_try(AST *try_body, const char *catch_name, AST *catch_body);
AST *ast_throw(AST *expr);

void ast_print_debug(AST *n, int indent);
void program_free(Program *p);

#endif
