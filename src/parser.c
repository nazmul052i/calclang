#include "parser.h"

static void next(Parser *p) { p->current = lexer_next(&p->lexer); }

static Token expect(Parser *p, TokenType t) {
    if (p->current.type != t) {
        fprintf(stderr, "parse error at %d:%d: expected %s but got %s ('%s')\n",
            p->current.line, p->current.col,
            token_type_name(t), token_type_name(p->current.type), p->current.text);
        exit(1);
    }
    Token got = p->current;
    next(p);
    return got;
}

void parser_init(Parser *p, const char *src) {
    lexer_init(&p->lexer, src);
    next(p);
}

static AST *parse_expr(Parser *p);
static AST *parse_stmt(Parser *p);
static TypeAnnot parse_type(Parser *p);
static void parse_fn_rest(Parser *p, AST *fn);

static AST *parse_call_after_name(Parser *p, const char *name) {
    /* Caller has already consumed the IDENT; we are sitting on `(`. */
    expect(p, TOK_LPAREN);
    AST *call = ast_call(name);
    if (p->current.type != TOK_RPAREN) {
        for (;;) {
            ast_call_add_arg(call, parse_expr(p));
            if (p->current.type != TOK_COMMA) break;
            next(p);
        }
    }
    expect(p, TOK_RPAREN);
    return call;
}

static AST *parse_primary(Parser *p) {
    Token t = p->current;
    if (t.type == TOK_NUMBER) {
        next(p);
        return ast_number(strtod(t.text, NULL));
    }
    if (t.type == TOK_STRING) {
        next(p);
        return ast_string(t.text);
    }
    if (t.type == TOK_IDENTIFIER) {
        next(p);
        if (p->current.type == TOK_LPAREN) {
            return parse_call_after_name(p, t.text);
        }
        return ast_var(t.text);
    }
    if (t.type == TOK_LPAREN) {
        next(p);
        AST *e = parse_expr(p);
        expect(p, TOK_RPAREN);
        return e;
    }
    if (t.type == TOK_LBRACKET) {
        /* Array literal `[a, b, c]` or empty `[]`. */
        next(p);
        AST *lit = ast_array_lit();
        if (p->current.type != TOK_RBRACKET) {
            for (;;) {
                ast_array_lit_add(lit, parse_expr(p));
                if (p->current.type != TOK_COMMA) break;
                next(p);
            }
        }
        expect(p, TOK_RBRACKET);
        return lit;
    }
    if (t.type == TOK_FN) {
        /* Anonymous closure expression: `fn(params) { body }`. The
           result is a NODE_FN value with empty name; codegen recognises
           the empty name as "inline closure, not a top-level fn". */
        next(p);
        AST *fn = ast_fn("");
        parse_fn_rest(p, fn);
        return fn;
    }
    if (t.type == TOK_LBRACE) {
        /* Map literal `{ key: value, ... }` or empty `{}`. `{` is only
           interpreted as a map here, in expression position; in
           statement position the parser sees it as a block before
           ever reaching parse_primary, so the two never collide. */
        next(p);
        AST *m = ast_map_lit();
        if (p->current.type != TOK_RBRACE) {
            for (;;) {
                AST *key = parse_expr(p);
                expect(p, TOK_COLON);
                AST *val = parse_expr(p);
                ast_map_lit_add(m, key, val);
                if (p->current.type != TOK_COMMA) break;
                next(p);
            }
        }
        expect(p, TOK_RBRACE);
        return m;
    }
    fprintf(stderr, "parse error at %d:%d: expected number, variable, '(', '[', or '{' but got %s\n",
        t.line, t.col, token_type_name(t.type));
    exit(1);
}

/* Postfix chain. Accepts `[expr]`, `.identifier`, and `(args)`. The
   dot form is sugar for `["identifier"]` (struct-like field access on
   maps). The parenthesised form is an indirect call — `obj.method(a)`
   parses as `(obj["method"])(a)`. */
static AST *parse_postfix(Parser *p) {
    AST *n = parse_primary(p);
    while (p->current.type == TOK_LBRACKET
        || p->current.type == TOK_DOT
        || p->current.type == TOK_LPAREN) {
        if (p->current.type == TOK_LBRACKET) {
            next(p);
            AST *idx = parse_expr(p);
            expect(p, TOK_RBRACKET);
            n = ast_index(n, idx);
        } else if (p->current.type == TOK_DOT) {
            next(p);
            Token field = expect(p, TOK_IDENTIFIER);
            n = ast_index(n, ast_string(field.text));
        } else {
            next(p);
            AST *call = ast_call_indirect(n);
            if (p->current.type != TOK_RPAREN) {
                for (;;) {
                    ast_call_add_arg(call, parse_expr(p));
                    if (p->current.type != TOK_COMMA) break;
                    next(p);
                }
            }
            expect(p, TOK_RPAREN);
            n = call;
        }
    }
    return n;
}

static AST *parse_unary(Parser *p) {
    if (p->current.type == TOK_MINUS) {
        next(p);
        return ast_unop(TOK_MINUS, parse_unary(p));
    }
    if (p->current.type == TOK_BANG) {
        next(p);
        return ast_unop(TOK_BANG, parse_unary(p));
    }
    if (p->current.type == TOK_TILDE) {
        next(p);
        return ast_unop(TOK_TILDE, parse_unary(p));
    }
    return parse_postfix(p);
}

static AST *parse_multiplicative(Parser *p) {
    AST *n = parse_unary(p);
    while (p->current.type == TOK_STAR ||
           p->current.type == TOK_SLASH ||
           p->current.type == TOK_PERCENT) {
        TokenType op = p->current.type;
        next(p);
        n = ast_binop(op, n, parse_unary(p));
    }
    return n;
}

static AST *parse_additive(Parser *p) {
    AST *n = parse_multiplicative(p);
    while (p->current.type == TOK_PLUS || p->current.type == TOK_MINUS) {
        TokenType op = p->current.type;
        next(p);
        n = ast_binop(op, n, parse_multiplicative(p));
    }
    return n;
}

/* Bitwise shifts: bind tighter than comparison, looser than +/-. */
static AST *parse_shift(Parser *p) {
    AST *n = parse_additive(p);
    while (p->current.type == TOK_LSHIFT || p->current.type == TOK_RSHIFT) {
        TokenType op = p->current.type;
        next(p);
        n = ast_binop(op, n, parse_additive(p));
    }
    return n;
}

static AST *parse_comparison(Parser *p) {
    AST *n = parse_shift(p);
    while (p->current.type == TOK_LT || p->current.type == TOK_LE ||
           p->current.type == TOK_GT || p->current.type == TOK_GE) {
        TokenType op = p->current.type;
        next(p);
        n = ast_binop(op, n, parse_shift(p));
    }
    return n;
}

static AST *parse_equality(Parser *p) {
    AST *n = parse_comparison(p);
    while (p->current.type == TOK_EQEQ || p->current.type == TOK_NEQ) {
        TokenType op = p->current.type;
        next(p);
        n = ast_binop(op, n, parse_comparison(p));
    }
    return n;
}

/* C-style precedence: equality < bit-and < bit-xor < bit-or < logical-and < logical-or. */
static AST *parse_bitwise_and(Parser *p) {
    AST *n = parse_equality(p);
    while (p->current.type == TOK_AMP) {
        next(p);
        n = ast_binop(TOK_AMP, n, parse_equality(p));
    }
    return n;
}

static AST *parse_bitwise_xor(Parser *p) {
    AST *n = parse_bitwise_and(p);
    while (p->current.type == TOK_CARET) {
        next(p);
        n = ast_binop(TOK_CARET, n, parse_bitwise_and(p));
    }
    return n;
}

static AST *parse_bitwise_or(Parser *p) {
    AST *n = parse_bitwise_xor(p);
    while (p->current.type == TOK_PIPE) {
        next(p);
        n = ast_binop(TOK_PIPE, n, parse_bitwise_xor(p));
    }
    return n;
}

static AST *parse_logical_and(Parser *p) {
    AST *n = parse_bitwise_or(p);
    while (p->current.type == TOK_AND) {
        next(p);
        n = ast_binop(TOK_AND, n, parse_bitwise_or(p));
    }
    return n;
}

static AST *parse_logical_or(Parser *p) {
    AST *n = parse_logical_and(p);
    while (p->current.type == TOK_OR) {
        next(p);
        n = ast_binop(TOK_OR, n, parse_logical_and(p));
    }
    return n;
}

static AST *parse_expr(Parser *p) {
    return parse_logical_or(p);
}

static AST *parse_block(Parser *p) {
    expect(p, TOK_LBRACE);
    AST *block = ast_block();
    while (p->current.type != TOK_RBRACE && p->current.type != TOK_EOF) {
        ast_block_add(block, parse_stmt(p));
    }
    expect(p, TOK_RBRACE);
    return block;
}

/* All assignment-like forms after an identifier desugar to plain
   NODE_ASSIGN:
     x  =  e   →  x = e
     x += e    →  x = x + e
     x -= e    →  x = x - e
     x *= e    →  x = x * e
     x /= e    →  x = x / e
     x %= e    →  x = x % e
     x++       →  x = x + 1
     x--       →  x = x - 1
   The caller has already consumed the identifier and we are sitting on
   the operator. This is used both in statement context (caller adds
   the trailing `;`) and in for-init / for-step (no semicolon). */
static AST *parse_assignment_after_ident(Parser *p, Token name) {
    TokenType t = p->current.type;

    if (t == TOK_EQUAL) {
        next(p);
        return ast_assign(name.text, parse_expr(p));
    }
    if (t == TOK_PLUSPLUS) {
        next(p);
        return ast_assign(name.text,
            ast_binop(TOK_PLUS, ast_var(name.text), ast_number(1)));
    }
    if (t == TOK_MINUSMINUS) {
        next(p);
        return ast_assign(name.text,
            ast_binop(TOK_MINUS, ast_var(name.text), ast_number(1)));
    }

    TokenType binop;
    switch (t) {
        case TOK_PLUS_EQ:    binop = TOK_PLUS;    break;
        case TOK_MINUS_EQ:   binop = TOK_MINUS;   break;
        case TOK_STAR_EQ:    binop = TOK_STAR;    break;
        case TOK_SLASH_EQ:   binop = TOK_SLASH;   break;
        case TOK_PERCENT_EQ: binop = TOK_PERCENT; break;
        case TOK_AMP_EQ:     binop = TOK_AMP;     break;
        case TOK_PIPE_EQ:    binop = TOK_PIPE;    break;
        case TOK_CARET_EQ:   binop = TOK_CARET;   break;
        case TOK_LSHIFT_EQ:  binop = TOK_LSHIFT;  break;
        case TOK_RSHIFT_EQ:  binop = TOK_RSHIFT;  break;
        default:
            fprintf(stderr,
                "parse error at %d:%d: expected an assignment operator after identifier '%s'\n",
                p->current.line, p->current.col, name.text);
            exit(1);
    }
    next(p);
    AST *rhs = parse_expr(p);
    return ast_assign(name.text,
        ast_binop(binop, ast_var(name.text), rhs));
}

/* Parse the (init; cond; step) part of a `for` plus its body. Each of
   init, cond, step may be omitted (empty), which is encoded as a NULL
   child in the AST so codegen can skip it. */
static AST *parse_for_header_and_body(Parser *p) {
    expect(p, TOK_LPAREN);

    AST *init = NULL;
    if (p->current.type != TOK_SEMICOLON) {
        if (p->current.type == TOK_LET) {
            next(p);
            Token name = expect(p, TOK_IDENTIFIER);
            /* `for (let i: num = 0; ...)` — same annotation grammar
               as plain `let`. */
            TypeAnnot t = TYPE_ANY;
            if (p->current.type == TOK_COLON) {
                next(p);
                t = parse_type(p);
            }
            expect(p, TOK_EQUAL);
            AST *e = parse_expr(p);
            init = ast_let(name.text, t, e);
        } else if (p->current.type == TOK_IDENTIFIER) {
            Token name = p->current;
            next(p);
            init = parse_assignment_after_ident(p, name);
        } else {
            fprintf(stderr, "parse error at %d:%d: expected 'let' or assignment in for-init\n",
                p->current.line, p->current.col);
            exit(1);
        }
    }
    expect(p, TOK_SEMICOLON);

    AST *cond = NULL;
    if (p->current.type != TOK_SEMICOLON) {
        cond = parse_expr(p);
    }
    expect(p, TOK_SEMICOLON);

    AST *step = NULL;
    if (p->current.type != TOK_RPAREN) {
        if (p->current.type == TOK_IDENTIFIER) {
            Token name = p->current;
            next(p);
            step = parse_assignment_after_ident(p, name);
        } else {
            fprintf(stderr, "parse error at %d:%d: expected assignment in for-step\n",
                p->current.line, p->current.col);
            exit(1);
        }
    }
    expect(p, TOK_RPAREN);

    AST *body = parse_stmt(p);
    return ast_for(init, cond, step, body);
}

static AST *parse_stmt(Parser *p) {
    if (p->current.type == TOK_LET) {
        next(p);
        Token name = expect(p, TOK_IDENTIFIER);
        /* Optional type annotation: `let x: T = expr;`. */
        TypeAnnot t = TYPE_ANY;
        if (p->current.type == TOK_COLON) {
            next(p);
            t = parse_type(p);
        }
        expect(p, TOK_EQUAL);
        AST *e = parse_expr(p);
        expect(p, TOK_SEMICOLON);
        return ast_let(name.text, t, e);
    }
    if (p->current.type == TOK_PRINT) {
        next(p);
        AST *e = parse_expr(p);
        expect(p, TOK_SEMICOLON);
        return ast_print(e);
    }
    if (p->current.type == TOK_IF) {
        next(p);
        expect(p, TOK_LPAREN);
        AST *cond = parse_expr(p);
        expect(p, TOK_RPAREN);
        AST *then_branch = parse_stmt(p);
        AST *else_branch = NULL;
        if (p->current.type == TOK_ELSE) {
            next(p);
            else_branch = parse_stmt(p);
        }
        return ast_if(cond, then_branch, else_branch);
    }
    if (p->current.type == TOK_WHILE) {
        next(p);
        expect(p, TOK_LPAREN);
        AST *cond = parse_expr(p);
        expect(p, TOK_RPAREN);
        AST *body = parse_stmt(p);
        return ast_while(cond, body);
    }
    if (p->current.type == TOK_FOR) {
        next(p);
        return parse_for_header_and_body(p);
    }
    if (p->current.type == TOK_BREAK) {
        next(p);
        expect(p, TOK_SEMICOLON);
        return ast_break();
    }
    if (p->current.type == TOK_CONTINUE) {
        next(p);
        expect(p, TOK_SEMICOLON);
        return ast_continue();
    }
    if (p->current.type == TOK_RETURN) {
        next(p);
        AST *e = NULL;
        if (p->current.type != TOK_SEMICOLON) {
            e = parse_expr(p);
        }
        expect(p, TOK_SEMICOLON);
        return ast_return(e);
    }
    if (p->current.type == TOK_THROW) {
        next(p);
        AST *e = parse_expr(p);
        expect(p, TOK_SEMICOLON);
        return ast_throw(e);
    }
    if (p->current.type == TOK_TRY) {
        next(p);
        AST *body = parse_block(p);
        expect(p, TOK_CATCH);
        expect(p, TOK_LPAREN);
        Token name = expect(p, TOK_IDENTIFIER);
        expect(p, TOK_RPAREN);
        AST *handler = parse_block(p);
        return ast_try(body, name.text, handler);
    }
    if (p->current.type == TOK_SWITCH) {
        /* switch (expr) {
               case v1:
                   stmts*
               case v2:
                   stmts*
               default:
                   stmts*
           }
           Cases run top to bottom; `break;` exits the switch. There is
           no fall-through (each case body is terminated by the start of
           the next case label, the `default` keyword, or the closing
           brace). */
        next(p);
        expect(p, TOK_LPAREN);
        AST *disc = parse_expr(p);
        expect(p, TOK_RPAREN);
        expect(p, TOK_LBRACE);
        AST *sw = ast_switch(disc);
        while (p->current.type == TOK_CASE || p->current.type == TOK_DEFAULT) {
            int is_default = (p->current.type == TOK_DEFAULT);
            next(p);
            AST *val = NULL;
            if (!is_default) {
                val = parse_expr(p);
            }
            expect(p, TOK_COLON);
            /* Collect statements until the next case/default/} into a block. */
            AST *body = ast_block();
            while (p->current.type != TOK_CASE
                && p->current.type != TOK_DEFAULT
                && p->current.type != TOK_RBRACE
                && p->current.type != TOK_EOF) {
                ast_block_add(body, parse_stmt(p));
            }
            if (is_default) {
                ast_switch_set_default(sw, body);
            } else {
                ast_switch_add_case(sw, val, body);
            }
        }
        expect(p, TOK_RBRACE);
        return sw;
    }
    if (p->current.type == TOK_LBRACE) {
        return parse_block(p);
    }
    if (p->current.type == TOK_FN) {
        /* Nested named fn inside a block — desugar to a let. The
           function becomes an inline closure (NODE_FN expression) bound
           to a local of the enclosing scope. */
        next(p);
        Token name = expect(p, TOK_IDENTIFIER);
        AST *fn = ast_fn(name.text);
        parse_fn_rest(p, fn);
        return ast_let(name.text, TYPE_ANY, fn);
    }
    if (p->current.type == TOK_IDENTIFIER) {
        Token name = p->current;
        next(p);
        if (p->current.type == TOK_LPAREN) {
            /* Call-as-statement: `f(...);`. Codegen will emit the
               normal call expression and then DROP the result. */
            AST *call = parse_call_after_name(p, name.text);
            expect(p, TOK_SEMICOLON);
            return call;
        }
        if (p->current.type == TOK_LBRACKET || p->current.type == TOK_DOT) {
            /* Index-assignment OR method-call statement. The chain can
               mix [..] and .name freely:
                 arr[i] = v
                 m.field = v
                 grid[r][c] = v
                 obj.users[i].name = v
                 obj.method(args)            // method call as statement
                 things[i].action(a, b)      // ditto, chained
               All inner ops build a NODE_INDEX chain. Reaching `(`
               ends the chain as a call; reaching `=`/`+=`/`...` ends
               it as an assignment. */
            AST *target = ast_var(name.text);
            AST *final_index = NULL;
            while (p->current.type == TOK_LBRACKET || p->current.type == TOK_DOT) {
                AST *idx;
                if (p->current.type == TOK_LBRACKET) {
                    next(p);
                    idx = parse_expr(p);
                    expect(p, TOK_RBRACKET);
                } else {
                    next(p);
                    Token field = expect(p, TOK_IDENTIFIER);
                    idx = ast_string(field.text);
                }
                if (p->current.type == TOK_LPAREN) {
                    /* Method-call statement: parse the remaining
                       `(args);` and emit an indirect call on the full
                       postfix chain. */
                    AST *callee = ast_index(target, idx);
                    next(p);
                    AST *call = ast_call_indirect(callee);
                    if (p->current.type != TOK_RPAREN) {
                        for (;;) {
                            ast_call_add_arg(call, parse_expr(p));
                            if (p->current.type != TOK_COMMA) break;
                            next(p);
                        }
                    }
                    expect(p, TOK_RPAREN);
                    /* Allow further chained postfix ops on the call
                       result (e.g. `factory().init();`). */
                    while (p->current.type == TOK_LBRACKET
                        || p->current.type == TOK_DOT
                        || p->current.type == TOK_LPAREN) {
                        if (p->current.type == TOK_LBRACKET) {
                            next(p);
                            AST *e = parse_expr(p);
                            expect(p, TOK_RBRACKET);
                            call = ast_index(call, e);
                        } else if (p->current.type == TOK_DOT) {
                            next(p);
                            Token fld = expect(p, TOK_IDENTIFIER);
                            call = ast_index(call, ast_string(fld.text));
                        } else {
                            next(p);
                            AST *c2 = ast_call_indirect(call);
                            if (p->current.type != TOK_RPAREN) {
                                for (;;) {
                                    ast_call_add_arg(c2, parse_expr(p));
                                    if (p->current.type != TOK_COMMA) break;
                                    next(p);
                                }
                            }
                            expect(p, TOK_RPAREN);
                            call = c2;
                        }
                    }
                    expect(p, TOK_SEMICOLON);
                    return call;
                }
                if (p->current.type == TOK_LBRACKET || p->current.type == TOK_DOT) {
                    target = ast_index(target, idx);
                } else {
                    final_index = idx;
                    break;
                }
            }
            /* After the final `[expr]` we accept any of:
                 = value;
                 += value;   -= value;   *= value;   /= value;   %= value;
                 ++;          --;
               All compound forms are lowered to NODE_INDEX_OPASSIGN
               which gets a DUP2 + INDEX_GET + binop + INDEX_SET. */
            TokenType opt = p->current.type;
            if (opt == TOK_EQUAL) {
                next(p);
                AST *value = parse_expr(p);
                expect(p, TOK_SEMICOLON);
                return ast_index_assign(target, final_index, value);
            }
            if (opt == TOK_PLUSPLUS || opt == TOK_MINUSMINUS) {
                next(p);
                expect(p, TOK_SEMICOLON);
                TokenType bop = (opt == TOK_PLUSPLUS) ? TOK_PLUS : TOK_MINUS;
                return ast_index_opassign(target, final_index, bop, ast_number(1));
            }
            TokenType bop;
            switch (opt) {
                case TOK_PLUS_EQ:    bop = TOK_PLUS;    break;
                case TOK_MINUS_EQ:   bop = TOK_MINUS;   break;
                case TOK_STAR_EQ:    bop = TOK_STAR;    break;
                case TOK_SLASH_EQ:   bop = TOK_SLASH;   break;
                case TOK_PERCENT_EQ: bop = TOK_PERCENT; break;
                case TOK_AMP_EQ:     bop = TOK_AMP;     break;
                case TOK_PIPE_EQ:    bop = TOK_PIPE;    break;
                case TOK_CARET_EQ:   bop = TOK_CARET;   break;
                case TOK_LSHIFT_EQ:  bop = TOK_LSHIFT;  break;
                case TOK_RSHIFT_EQ:  bop = TOK_RSHIFT;  break;
                default:
                    fprintf(stderr, "parse error at %d:%d: array index expression as a statement requires an assignment operator\n",
                        p->current.line, p->current.col);
                    exit(1);
            }
            next(p);
            AST *value = parse_expr(p);
            expect(p, TOK_SEMICOLON);
            return ast_index_opassign(target, final_index, bop, value);
        }
        AST *stmt = parse_assignment_after_ident(p, name);
        expect(p, TOK_SEMICOLON);
        return stmt;
    }
    fprintf(stderr, "parse error at %d:%d: expected statement but got %s\n",
        p->current.line, p->current.col, token_type_name(p->current.type));
    exit(1);
}

/* A type annotation is a bare identifier matching one of the known
   type-keywords. Recognising it only in type position keeps the
   keywords from stealing those names as ordinary identifiers. */
static TypeAnnot parse_type(Parser *p) {
    /* `fn` is a keyword, not an identifier, so we accept its token
       form here as the function type. All other type names come
       through as identifiers. */
    if (p->current.type == TOK_FN) {
        next(p);
        return TYPE_FN;
    }
    if (p->current.type != TOK_IDENTIFIER) {
        fprintf(stderr, "parse error at %d:%d: expected type name (num/str/arr/map/fn/bool/any), got %s\n",
            p->current.line, p->current.col, token_type_name(p->current.type));
        exit(1);
    }
    const char *s = p->current.text;
    TypeAnnot t;
    if      (strcmp(s, "num")  == 0) t = TYPE_NUM;
    else if (strcmp(s, "str")  == 0) t = TYPE_STR;
    else if (strcmp(s, "arr")  == 0) t = TYPE_ARR;
    else if (strcmp(s, "map")  == 0) t = TYPE_MAP;
    else if (strcmp(s, "fn")   == 0) t = TYPE_FN;
    else if (strcmp(s, "bool") == 0) t = TYPE_BOOL;
    else if (strcmp(s, "any")  == 0) t = TYPE_ANY;
    else {
        fprintf(stderr, "parse error at %d:%d: unknown type '%s' (expected num/str/arr/map/fn/bool/any)\n",
            p->current.line, p->current.col, s);
        exit(1);
    }
    next(p);
    return t;
}

/* Parse the `(params) [: T] { body }` portion of a function after the
   `fn` keyword and any optional name have been consumed. Used by:
     - top-level named `fn foo(...) { ... }`
     - top-level `pub fn foo(...) { ... }`
     - nested named `fn foo(...) { ... }` (sugar for `let foo = fn(...)`)
     - anonymous expression `fn(...) { ... }` */
static void parse_fn_rest(Parser *p, AST *fn) {
    expect(p, TOK_LPAREN);
    if (p->current.type != TOK_RPAREN) {
        for (;;) {
            Token param = expect(p, TOK_IDENTIFIER);
            TypeAnnot pt = TYPE_ANY;
            if (p->current.type == TOK_COLON) {
                next(p);
                pt = parse_type(p);
            }
            ast_fn_add_param(fn, param.text, pt);
            if (p->current.type != TOK_COMMA) break;
            next(p);
        }
    }
    expect(p, TOK_RPAREN);
    if (p->current.type == TOK_COLON) {
        next(p);
        ast_fn_set_return_type(fn, parse_type(p));
    }
    ast_fn_set_body(fn, parse_block(p));
}

static AST *parse_fn_def(Parser *p) {
    expect(p, TOK_FN);
    Token name = expect(p, TOK_IDENTIFIER);
    AST *fn = ast_fn(name.text);
    parse_fn_rest(p, fn);
    return fn;
}

/* `extern fn name(params): T;` — forward declaration of a function
   defined in another compilation unit. Implicitly public. */
static AST *parse_extern_fn(Parser *p) {
    expect(p, TOK_EXTERN);
    expect(p, TOK_FN);
    Token name = expect(p, TOK_IDENTIFIER);
    AST *fn = ast_fn(name.text);
    ast_fn_set_public(fn, 1);
    ast_fn_set_extern(fn, 1);
    expect(p, TOK_LPAREN);
    if (p->current.type != TOK_RPAREN) {
        for (;;) {
            Token param = expect(p, TOK_IDENTIFIER);
            TypeAnnot pt = TYPE_ANY;
            if (p->current.type == TOK_COLON) {
                next(p);
                pt = parse_type(p);
            }
            ast_fn_add_param(fn, param.text, pt);
            if (p->current.type != TOK_COMMA) break;
            next(p);
        }
    }
    expect(p, TOK_RPAREN);
    if (p->current.type == TOK_COLON) {
        next(p);
        ast_fn_set_return_type(fn, parse_type(p));
    }
    expect(p, TOK_SEMICOLON);
    return fn;
}

/* `class Name { fn init(...) {...} fn m(...) {...} ... }`
   desugars to a constructor function. The constructor takes init's
   parameters; its body builds an instance map (`this`), attaches each
   non-init method as a closure that captures `this`, then inlines the
   init body. Methods refer to `this` as a free name — the existing
   mutable + transitive closure machinery does the rest. */
static AST *parse_class_def(Parser *p) {
    expect(p, TOK_CLASS);
    Token name = expect(p, TOK_IDENTIFIER);
    expect(p, TOK_LBRACE);

    AST *methods[CL_MAX_PARAMS];
    int  method_count = 0;
    AST *init_method  = NULL;

    while (p->current.type != TOK_RBRACE && p->current.type != TOK_EOF) {
        if (p->current.type != TOK_FN) {
            fprintf(stderr, "parse error at %d:%d: class body may contain only `fn` method declarations\n",
                p->current.line, p->current.col);
            exit(1);
        }
        next(p);
        Token mname = expect(p, TOK_IDENTIFIER);
        AST *m = ast_fn(mname.text);
        parse_fn_rest(p, m);
        if (strcmp(mname.text, "init") == 0) {
            if (init_method) {
                fprintf(stderr, "parse error at %d:%d: class '%s' has more than one `init`\n",
                    mname.line, mname.col, name.text);
                exit(1);
            }
            init_method = m;
        } else {
            if (method_count >= CL_MAX_PARAMS) {
                fprintf(stderr, "parse error at %d:%d: class '%s' has too many methods (max %d)\n",
                    mname.line, mname.col, name.text, CL_MAX_PARAMS);
                exit(1);
            }
            methods[method_count++] = m;
        }
    }
    expect(p, TOK_RBRACE);

    AST *ctor = ast_fn(name.text);
    if (init_method) {
        for (int i = 0; i < init_method->as.fn_def.param_count; i++) {
            ast_fn_add_param(ctor,
                init_method->as.fn_def.params[i],
                init_method->as.fn_def.param_types[i]);
        }
    }

    AST *body = ast_block();
    ast_block_add(body, ast_let("this", TYPE_ANY, ast_map_lit()));

    /* Attach each non-init method as `this["m"] = fn(...) { body };`.
       The method's NODE_FN started life as a named decl; switch it to
       an inline closure by clearing the name (codegen treats empty
       name as anonymous). */
    for (int i = 0; i < method_count; i++) {
        AST *m = methods[i];
        char mname_saved[CL_MAX_TEXT];
        cl_strncpy_z(mname_saved, m->as.fn_def.name, CL_MAX_TEXT);
        m->as.fn_def.name[0] = '\0';
        ast_block_add(body,
            ast_index_assign(ast_var("this"), ast_string(mname_saved), m));
    }

    /* Inline init's statements directly into the constructor body so
       they execute in the constructor's scope (where `this` and the
       constructor's params are bound). */
    if (init_method && init_method->as.fn_def.body) {
        AST *ib = init_method->as.fn_def.body;
        for (int i = 0; i < ib->as.block.count; i++) {
            ast_block_add(body, ib->as.block.stmts[i]);
        }
        ib->as.block.count = 0;  /* statements moved; avoid double-free */
    }

    ast_block_add(body, ast_return(ast_var("this")));
    ast_fn_set_body(ctor, body);
    return ctor;
}

Program parser_parse_program(Parser *p) {
    Program prog;
    prog.count = 0;
    while (p->current.type != TOK_EOF) {
        if (prog.count >= CL_MAX_NODES) cl_die("too many top-level items");
        if (p->current.type == TOK_PUB) {
            next(p);
            if (p->current.type == TOK_FN) {
                AST *fn = parse_fn_def(p);
                ast_fn_set_public(fn, 1);
                prog.items[prog.count++] = fn;
            } else if (p->current.type == TOK_CLASS) {
                AST *cls = parse_class_def(p);
                ast_fn_set_public(cls, 1);
                prog.items[prog.count++] = cls;
            } else {
                fprintf(stderr, "parse error at %d:%d: 'pub' must be followed by 'fn' or 'class'\n",
                    p->current.line, p->current.col);
                exit(1);
            }
        } else if (p->current.type == TOK_CLASS) {
            prog.items[prog.count++] = parse_class_def(p);
        } else if (p->current.type == TOK_PRIV) {
            /* `priv` is explicit private — same effect as bare `fn`,
               but lets the visibility be stated for clarity. */
            next(p);
            if (p->current.type != TOK_FN) {
                fprintf(stderr, "parse error at %d:%d: 'priv' must be followed by 'fn'\n",
                    p->current.line, p->current.col);
                exit(1);
            }
            prog.items[prog.count++] = parse_fn_def(p);
        } else if (p->current.type == TOK_EXTERN) {
            prog.items[prog.count++] = parse_extern_fn(p);
        } else if (p->current.type == TOK_FN) {
            prog.items[prog.count++] = parse_fn_def(p);
        } else {
            prog.items[prog.count++] = parse_stmt(p);
        }
    }
    return prog;
}
