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
    p->source_dir[0] = '\0';
    next(p);
}

void parser_init_with_path(Parser *p, const char *src, const char *file_path) {
    lexer_init(&p->lexer, src);
    p->source_dir[0] = '\0';
    if (file_path) {
        /* Find the rightmost path separator and copy everything before it. */
        const char *last = file_path;
        for (const char *q = file_path; *q; q++) {
            if (*q == '/' || *q == '\\') last = q + 1;
        }
        if (last > file_path) {
            size_t n = (size_t)(last - file_path);
            if (n >= sizeof p->source_dir) n = sizeof p->source_dir - 1;
            memcpy(p->source_dir, file_path, n);
            p->source_dir[n] = '\0';
        }
    }
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
        /* Array literal `[a, b, c]` or empty `[]`. A trailing comma
           before `]` is allowed (matches struct-field-list style). */
        next(p);
        AST *lit = ast_array_lit();
        if (p->current.type != TOK_RBRACKET) {
            for (;;) {
                ast_array_lit_add(lit, parse_expr(p));
                if (p->current.type != TOK_COMMA) break;
                next(p);
                if (p->current.type == TOK_RBRACKET) break;
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
                if (p->current.type == TOK_RBRACE) break;   /* trailing comma */
            }
        }
        expect(p, TOK_RBRACE);
        return m;
    }
    fprintf(stderr, "parse error at %d:%d: expected number, variable, '(', '[', or '{' but got %s\n",
        t.line, t.col, token_type_name(t.type));
    exit(1);
}

/* Postfix chain. Accepts `[expr]`, `.identifier`, `(args)`, and
   the NumPy-style extensions `[i, j]` (chained indexing) and
   `[lo:hi]` / `[:hi]` / `[lo:]` / `[:]` (1-D slicing). The dot form
   is sugar for `["identifier"]` (struct-like field access on maps).
   The parenthesised form is an indirect call — `obj.method(a)`
   parses as `(obj["method"])(a)`. */
static AST *parse_postfix(Parser *p) {
    AST *n = parse_primary(p);
    while (p->current.type == TOK_LBRACKET
        || p->current.type == TOK_DOT
        || p->current.type == TOK_LPAREN) {
        if (p->current.type == TOK_LBRACKET) {
            next(p);
            /* Two opening cases: `[expr ...` or `[: ...`. The latter
               means slice-from-start. */
            AST *first = NULL;
            if (p->current.type != TOK_COLON) {
                first = parse_expr(p);
            }
            if (p->current.type == TOK_COLON) {
                /* Slice: `[lo:hi]`, `[:hi]`, `[lo:]`, or `[:]`. Desugar
                   to a call to array_slice(target, lo, hi). When lo is
                   omitted it defaults to 0; when hi is omitted, len(target).
                   Note: target is evaluated twice when hi is omitted —
                   acceptable for the common variable-read case. */
                next(p);
                AST *hi = NULL;
                if (p->current.type != TOK_RBRACKET && p->current.type != TOK_COMMA) {
                    hi = parse_expr(p);
                }
                /* 2-D slice forms like `m[1:5, :]` aren't supported yet;
                   if a comma follows, drop everything through to the `]`. */
                if (p->current.type == TOK_COMMA) {
                    while (p->current.type != TOK_RBRACKET && p->current.type != TOK_EOF) {
                        next(p);
                    }
                }
                expect(p, TOK_RBRACKET);
                AST *lo_v = first ? first : ast_number(0);
                AST *hi_v = hi;
                if (!hi_v) {
                    hi_v = ast_call("len");
                    ast_call_add_arg(hi_v, n);
                }
                AST *call = ast_call("array_slice");
                ast_call_add_arg(call, n);
                ast_call_add_arg(call, lo_v);
                ast_call_add_arg(call, hi_v);
                n = call;
            } else if (p->current.type == TOK_COMMA) {
                /* Multi-dim indexing: m[i, j, k] -> ((m[i])[j])[k]. */
                n = ast_index(n, first);
                while (p->current.type == TOK_COMMA) {
                    next(p);
                    AST *more = parse_expr(p);
                    n = ast_index(n, more);
                }
                expect(p, TOK_RBRACKET);
            } else {
                /* Plain single-index m[i] — what we've always done. */
                expect(p, TOK_RBRACKET);
                n = ast_index(n, first);
            }
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

/* Ternary `cond ? a : b` — lowest precedence in expressions, right-associative.
   We parse `cond` at logical-or precedence; then if a `?` follows, we parse
   the `then` branch at expression (= ternary) precedence so chains like
   `a ? b : c ? d : e` parse as `a ? b : (c ? d : e)`. */
static AST *parse_ternary(Parser *p) {
    AST *cond = parse_logical_or(p);
    if (p->current.type == TOK_QMARK) {
        next(p);
        AST *then_expr = parse_ternary(p);
        expect(p, TOK_COLON);
        AST *else_expr = parse_ternary(p);
        return ast_ternary(cond, then_expr, else_expr);
    }
    return cond;
}

static AST *parse_expr(Parser *p) {
    return parse_ternary(p);
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

/* Build a fresh helper name like "__for3_it" so nested loops don't
   alias their temporaries. The names are illegal-looking on purpose:
   double-underscore + counter, so they won't collide with user vars. */
static int g_for_in_counter = 0;
static void fresh_name(char *buf, size_t buflen, int id, const char *suffix) {
    snprintf(buf, buflen, "__for%d_%s", id, suffix);
}

/* Desugar a `for x in iter { body }` into nested standard constructs.
   `iter` is one of:
     - a range expression  (start..end / start..=end) — handled by the
       caller, which passes start, end, and `inclusive`.
     - an arr/str expression — pass start=NULL.
   The result is a NODE_BLOCK that introduces helper bindings in a
   private scope. */
static AST *build_for_in_range(const char *var, AST *start, AST *end, int inclusive, AST *body) {
    int id = ++g_for_in_counter;
    char end_name[64];
    fresh_name(end_name, sizeof end_name, id, "end");

    AST *block = ast_block();
    /* let __forN_end = end; */
    ast_block_add(block, ast_let(end_name, TYPE_ANY, end));
    /* for (let var = start; var <{=} __forN_end; var = var + 1) body */
    AST *init = ast_let(var, TYPE_ANY, start);
    AST *cond = ast_binop(inclusive ? TOK_LE : TOK_LT, ast_var(var), ast_var(end_name));
    AST *step = ast_assign(var, ast_binop(TOK_PLUS, ast_var(var), ast_number(1)));
    ast_block_add(block, ast_for(init, cond, step, body));
    return block;
}

static AST *build_for_in_iter(const char *var, AST *iter, AST *body) {
    int id = ++g_for_in_counter;
    char it_name[64], n_name[64], i_name[64];
    fresh_name(it_name, sizeof it_name, id, "it");
    fresh_name(n_name,  sizeof n_name,  id, "n");
    fresh_name(i_name,  sizeof i_name,  id, "i");

    AST *outer = ast_block();
    /* let __forN_it = iter; */
    ast_block_add(outer, ast_let(it_name, TYPE_ANY, iter));
    /* let __forN_n  = len(__forN_it); */
    AST *call_len = ast_call("len");
    ast_call_add_arg(call_len, ast_var(it_name));
    ast_block_add(outer, ast_let(n_name, TYPE_ANY, call_len));

    /* for (let __forN_i = 0; __forN_i < __forN_n; __forN_i = __forN_i + 1) {
           let var = __forN_it[__forN_i];
           body...
       } */
    AST *inner = ast_block();
    ast_block_add(inner, ast_let(var, TYPE_ANY,
        ast_index(ast_var(it_name), ast_var(i_name))));
    /* Splice the body in. If it's already a block, fold its statements
       into `inner` so we don't create a redundant nesting level. */
    if (body->kind == NODE_BLOCK) {
        for (int k = 0; k < body->as.block.count; k++) {
            ast_block_add(inner, body->as.block.stmts[k]);
        }
    } else {
        ast_block_add(inner, body);
    }

    AST *init = ast_let(i_name, TYPE_ANY, ast_number(0));
    AST *cond = ast_binop(TOK_LT, ast_var(i_name), ast_var(n_name));
    AST *step = ast_assign(i_name,
        ast_binop(TOK_PLUS, ast_var(i_name), ast_number(1)));
    ast_block_add(outer, ast_for(init, cond, step, inner));
    return outer;
}

/* Parse a for-in header *after* the loop variable identifier has been
   consumed. Caller passes the variable name and whether the head was
   wrapped in parens (so we know whether to expect `)` at the end). */
static AST *parse_for_in_after_name(Parser *p, const char *var, int has_parens) {
    expect(p, TOK_IN);
    AST *first = parse_expr(p);
    AST *iter_or_start = first;
    AST *range_end = NULL;
    int inclusive = 0;
    if (p->current.type == TOK_DOTDOT || p->current.type == TOK_DOTDOTEQ) {
        inclusive = (p->current.type == TOK_DOTDOTEQ);
        next(p);
        range_end = parse_expr(p);
    }
    if (has_parens) expect(p, TOK_RPAREN);
    AST *body = parse_stmt(p);
    if (range_end) {
        return build_for_in_range(var, iter_or_start, range_end, inclusive, body);
    }
    return build_for_in_iter(var, iter_or_start, body);
}

/* Parse the header and body of a `for`. Supports three forms:
     for (let i = 0; i < n; i = i + 1) body         // classic C-style
     for (let? x in iter) body                       // for-in, with parens
     for x in iter body                              // Rust-style, no parens
   `iter` may be a range (start..end / start..=end) or any array / string
   expression. Maps don't work directly — use `for k in keys(m)`. */
static AST *parse_for_header_and_body(Parser *p) {
    /* No-paren Rust-style: `for x in xs { body }`. The loop variable
       can't be a `let` here (`let` outside parens reads ambiguously),
       so we just expect an identifier directly. */
    if (p->current.type != TOK_LPAREN) {
        Token name = expect(p, TOK_IDENTIFIER);
        return parse_for_in_after_name(p, name.text, /*has_parens=*/0);
    }

    expect(p, TOK_LPAREN);

    /* Inside parens: distinguish for-in (`[let] IDENT in ...`) from
       classic (`[let] IDENT = ... ; ... ; ...`). We always need to
       look at what comes after the optional `let` + ident. */
    int has_let = 0;
    if (p->current.type == TOK_LET) {
        has_let = 1;
        next(p);
        Token name = expect(p, TOK_IDENTIFIER);
        /* Optional type annotation, only valid in classic form. */
        TypeAnnot t = TYPE_ANY;
        if (p->current.type == TOK_COLON) {
            next(p);
            t = parse_type(p);
        }
        if (p->current.type == TOK_IN) {
            /* for-in with `let`: `for (let x in iter) body`. */
            (void)t;
            return parse_for_in_after_name(p, name.text, /*has_parens=*/1);
        }
        /* Classic: continue parsing `= expr; cond; step`. */
        expect(p, TOK_EQUAL);
        AST *e = parse_expr(p);
        AST *init = ast_let(name.text, t, e);
        expect(p, TOK_SEMICOLON);

        AST *cond = NULL;
        if (p->current.type != TOK_SEMICOLON) cond = parse_expr(p);
        expect(p, TOK_SEMICOLON);

        AST *step = NULL;
        if (p->current.type != TOK_RPAREN) {
            if (p->current.type == TOK_IDENTIFIER) {
                Token n2 = p->current;
                next(p);
                step = parse_assignment_after_ident(p, n2);
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
    (void)has_let;

    AST *init = NULL;
    if (p->current.type != TOK_SEMICOLON) {
        if (p->current.type == TOK_IDENTIFIER) {
            Token name = p->current;
            next(p);
            if (p->current.type == TOK_IN) {
                /* for-in without `let`: `for (x in iter) body`. */
                return parse_for_in_after_name(p, name.text, /*has_parens=*/1);
            }
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
    /* printf / println: convenience statements that desugar to a
       fmt(...)-then-output call. Form:
           printf  FORMAT, expr1, expr2, ...;     -> write(fmt(FORMAT, [args]))
           println FORMAT, expr1, expr2, ...;     -> print fmt(FORMAT, [args])
       The args are collected into an array literal so we don't need
       variadic builtin support — fmt's existing (str, arr) signature
       handles them. */
    if (p->current.type == TOK_PRINTF || p->current.type == TOK_PRINTLN) {
        int is_println = (p->current.type == TOK_PRINTLN);
        next(p);
        AST *format = parse_expr(p);
        AST *args = ast_array_lit();
        while (p->current.type == TOK_COMMA) {
            next(p);
            ast_array_lit_add(args, parse_expr(p));
        }
        expect(p, TOK_SEMICOLON);
        AST *fmt_call = ast_call("fmt");
        ast_call_add_arg(fmt_call, format);
        ast_call_add_arg(fmt_call, args);
        if (is_println) {
            return ast_print(fmt_call);
        }
        /* printf: write the string with no trailing newline.
           Build write(fmt(...)) — write is a calclib builtin. */
        AST *write_call = ast_call("write");
        ast_call_add_arg(write_call, fmt_call);
        return write_call;
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
    if (p->current.type == TOK_DO) {
        next(p);
        AST *body = parse_stmt(p);
        expect(p, TOK_WHILE);
        expect(p, TOK_LPAREN);
        AST *cond = parse_expr(p);
        expect(p, TOK_RPAREN);
        expect(p, TOK_SEMICOLON);
        return ast_do_while(body, cond);
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

/* Parse a struct definition: a typed record with fields, an
   auto-generated positional constructor, and no methods.

       struct Point { x: num, y: num }   ->   roughly...
       pub fn Point(x: num, y: num): map {
           let this = {};
           this.x = x;
           this.y = y;
           return this;
       }

   This is Stage 1 of the typed-records project: the runtime is still
   the same map-with-string-keys we use for classes, so a struct is
   only different from a class with all-public fields in *syntax*
   (declarative — no `fn init` boilerplate) and *intent* (a record,
   not an object with methods). Later stages will lower structs to a
   fixed flat layout in memory and unboxed numeric fields. Until then,
   getting the surface syntax right is the prerequisite. */
static AST *parse_struct_def(Parser *p) {
    expect(p, TOK_STRUCT);
    Token name = expect(p, TOK_IDENTIFIER);
    expect(p, TOK_LBRACE);

    char fnames[CL_MAX_PARAMS][CL_MAX_TEXT];
    TypeAnnot ftypes[CL_MAX_PARAMS];
    int field_count = 0;

    while (p->current.type != TOK_RBRACE && p->current.type != TOK_EOF) {
        if (field_count >= CL_MAX_PARAMS) {
            fprintf(stderr, "parse error at %d:%d: struct '%s' has too many fields (max %d)\n",
                p->current.line, p->current.col, name.text, CL_MAX_PARAMS);
            exit(1);
        }
        Token fname = expect(p, TOK_IDENTIFIER);
        cl_strncpy_z(fnames[field_count], fname.text, CL_MAX_TEXT);

        /* Field type annotation: `name: type` is the canonical form;
           bare `name` is also accepted (defaults to any). */
        TypeAnnot ft = TYPE_ANY;
        if (p->current.type == TOK_COLON) {
            next(p);
            ft = parse_type(p);
        }
        ftypes[field_count] = ft;
        field_count++;

        /* Comma between fields; the last one may omit it. */
        if (p->current.type == TOK_COMMA) {
            next(p);
        } else if (p->current.type != TOK_RBRACE) {
            fprintf(stderr, "parse error at %d:%d: expected ',' or '}' after struct field, got '%s'\n",
                p->current.line, p->current.col, p->current.text);
            exit(1);
        }
    }
    expect(p, TOK_RBRACE);

    /* Build the constructor function. It takes the fields as positional
       parameters and returns a map populated with them. */
    AST *ctor = ast_fn(name.text);
    for (int i = 0; i < field_count; i++) {
        ast_fn_add_param(ctor, fnames[i], ftypes[i]);
    }

    AST *body = ast_block();
    ast_block_add(body, ast_let("this", TYPE_ANY, ast_map_lit()));
    for (int i = 0; i < field_count; i++) {
        /* this.<field> = <field-param> */
        ast_block_add(body,
            ast_index_assign(ast_var("this"),
                             ast_string(fnames[i]),
                             ast_var(fnames[i])));
    }
    ast_block_add(body, ast_return(ast_var("this")));
    ast_fn_set_body(ctor, body);
    return ctor;
}

/* Cycle-detection set for imports. A static array is fine — the
   compiler runs once per invocation, and CalcLang programs don't
   import hundreds of files. Reset only at process exit. Both the
   parser's main loop and the recursive parse_import consult it. */
#define IMPORT_SET_MAX 256
static char import_set[IMPORT_SET_MAX][512];
static int  import_set_count = 0;

static int import_set_has(const char *path) {
    for (int i = 0; i < import_set_count; i++) {
        if (strcmp(import_set[i], path) == 0) return 1;
    }
    return 0;
}

static void import_set_add(const char *path) {
    if (import_set_count >= IMPORT_SET_MAX) return;
    cl_strncpy_z(import_set[import_set_count++], path, 512);
}

/* Handle a top-level `import "path";`. Reads the named file, parses
   it, and INLINES every `pub fn` (and `pub class` — the parser
   already lowers a class to a NODE_FN whose params come from `init`)
   into the current Program with body intact. No `--lib` build step is
   needed: the library's bodies become part of the consumer's binary.

   Cycle detection: a canonical path that has already been imported in
   this compilation is silently skipped (transitive imports of the
   same library all share the one inlined copy). */
static void parse_import(Parser *p, Program *out) {
    expect(p, TOK_IMPORT);
    Token path_tok = expect(p, TOK_STRING);
    expect(p, TOK_SEMICOLON);

    /* Path resolution. In order:
         - Absolute path:                used as-is.
         - "./..." or "../...":          importer-relative.
         - Path ending in ".clc":        used as a literal path (cwd then
                                         importer-relative fallback).
         - Anything else:                module spec — rewritten to
                                         "lib/<spec>.clc". Works for both
                                         flat names ("math") and nested
                                         libs ("nr/poly").

       Examples:
         import "math";          ->  lib/math.clc
         import "nr/poly";        ->  lib/nr/poly.clc
         import "./helper.clc";   ->  importer-dir/helper.clc
         import "lib/math.clc";   ->  as a literal path */
    const char *raw = path_tok.text;
    int is_abs = raw[0] == '/' || raw[0] == '\\' ||
                 (raw[0] != '\0' && raw[1] == ':');
    int is_dot = raw[0] == '.' && (raw[1] == '/' || raw[1] == '\\'
                 || (raw[1] == '.' && (raw[2] == '/' || raw[2] == '\\')));
    /* Path-like if it ends in .clc OR starts with lib/. */
    size_t rlen = strlen(raw);
    int ends_clc = rlen >= 4 && strcmp(raw + rlen - 4, ".clc") == 0;
    int starts_lib = (rlen >= 4 && (strncmp(raw, "lib/", 4) == 0 || strncmp(raw, "lib\\", 4) == 0));

    char rewritten[512];
    const char *path = raw;
    if (!is_abs && !is_dot && !ends_clc && !starts_lib) {
        /* Module spec — flat ("math"), nested with slash ("nr/poly"),
           or nested with dot ("nr.poly"). Both separators are accepted;
           dots get translated to slashes Python-style before joining.
           Final form: lib/<spec>.clc. */
        char tmp[512];
        size_t i = 0;
        for (const char *q = raw; *q && i + 1 < sizeof tmp; q++) {
            tmp[i++] = (*q == '.') ? '/' : *q;
        }
        tmp[i] = '\0';
        snprintf(rewritten, sizeof rewritten, "lib/%s.clc", tmp);
        path = rewritten;
    }

    /* Build the candidate list in resolution order, then take the first
       existing file. Order: cwd-relative > importer-relative > stdlib
       (from CALC_LIB_PATH, only for "lib/..." paths). Absolute paths
       skip probing — used as-is. Dot-prefixed paths are importer-rel
       when there's an importer, cwd-rel otherwise. */
    char full_path[1024];
    if (is_abs) {
        cl_strncpy_z(full_path, path, sizeof full_path);
    } else {
        const char *cands[4];
        int  ncands = 0;
        char importer_rel[1024];
        char stdlib_path[1024];

        if (is_dot) {
            if (p->source_dir[0] != '\0') {
                snprintf(importer_rel, sizeof importer_rel, "%s%s", p->source_dir, path);
                cands[ncands++] = importer_rel;
            } else {
                cands[ncands++] = path;
            }
        } else {
            cands[ncands++] = path;                 /* cwd-relative */
            if (p->source_dir[0] != '\0') {
                snprintf(importer_rel, sizeof importer_rel, "%s%s", p->source_dir, path);
                cands[ncands++] = importer_rel;
            }
            /* Stdlib + project-deps fallbacks: only for paths that
               started life as a module spec (which the rewrite gives a
               "lib/" prefix), or an explicit "lib/foo.clc". */
            int has_lib_prefix = (strncmp(path, "lib/", 4) == 0 || strncmp(path, "lib\\", 4) == 0);
            if (has_lib_prefix) {
                /* `rest` is what the user wrote after the implicit `lib/`
                   — e.g. for `import "vec-extras"` rest = "vec-extras.clc",
                   stripping ".clc" gives the module name. */
                const char *rest = path + 4;

                /* Project deps: probe deps/<name>/<name>.clc for the
                   leaf (non-nested) case. v0.1 only resolves flat names
                   through deps/; nested module specs ("nr.poly" → "nr/poly")
                   still target stdlib only. */
                char deps_path[1024];
                if (!strchr(rest, '/') && !strchr(rest, '\\')) {
                    char name[256];
                    size_t rl = strlen(rest);
                    if (rl >= 4 && strcmp(rest + rl - 4, ".clc") == 0) rl -= 4;
                    if (rl > 0 && rl < sizeof name) {
                        memcpy(name, rest, rl); name[rl] = '\0';
                        snprintf(deps_path, sizeof deps_path, "deps/%s/%s.clc", name, name);
                        cands[ncands++] = deps_path;
                    }
                }

                /* Stdlib search root (from CALC_LIB_PATH, set by
                   cl_init_install_paths). Always last so deps shadow. */
                const char *lib_env = getenv("CALC_LIB_PATH");
                if (lib_env) {
                    snprintf(stdlib_path, sizeof stdlib_path, "%s/%s", lib_env, rest);
                    cands[ncands++] = stdlib_path;
                }
            }
        }

        int found = 0;
        for (int i = 0; i < ncands; i++) {
            FILE *t = fopen(cands[i], "rb");
            if (t) { fclose(t); cl_strncpy_z(full_path, cands[i], sizeof full_path); found = 1; break; }
        }
        /* Not found — use the last candidate so the error from cl_read_file
           points at the most-specific attempted location (stdlib if probed,
           importer-rel otherwise). */
        if (!found) cl_strncpy_z(full_path, cands[ncands - 1], sizeof full_path);
    }

    /* Cycle detection. A library that's already been imported (directly
       or transitively) is skipped — its definitions are already in the
       containing Program. */
    if (import_set_has(full_path)) return;
    import_set_add(full_path);

    char *src = cl_read_file(full_path);   /* exits on failure with a clear path */
    Parser sub;
    parser_init_with_path(&sub, src, full_path);
    Program sub_prog = parser_parse_program(&sub);

    /* Inline ALL fn definitions (pub AND priv) into the current
       Program. The pub fns expose the library's API; the priv fns are
       internal helpers the pub bodies call. Both must be compiled into
       the consumer's binary or the pub bodies won't link. Top-level
       statements in the library (anything that's not a fn definition)
       are dropped, matching the `--lib` model. Privacy is preserved
       socially: priv names are conventionally underscore-prefixed and
       not part of the documented API, but the import system can't
       enforce that without per-symbol visibility checks at the call
       site (a future addition). */
    for (int i = 0; i < sub_prog.count; i++) {
        AST *item = sub_prog.items[i];
        if (item->kind != NODE_FN) continue;
        if (out->count >= CL_MAX_NODES) cl_die("too many top-level items after import");
        out->items[out->count++] = item;
    }
}

Program parser_parse_program(Parser *p) {
    Program prog;
    prog.count = 0;
    while (p->current.type != TOK_EOF) {
        if (prog.count >= CL_MAX_NODES) cl_die("too many top-level items");
        if (p->current.type == TOK_IMPORT) {
            parse_import(p, &prog);
            continue;
        }
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
            } else if (p->current.type == TOK_STRUCT) {
                AST *st = parse_struct_def(p);
                ast_fn_set_public(st, 1);
                prog.items[prog.count++] = st;
            } else {
                fprintf(stderr, "parse error at %d:%d: 'pub' must be followed by 'fn', 'class', or 'struct'\n",
                    p->current.line, p->current.col);
                exit(1);
            }
        } else if (p->current.type == TOK_CLASS) {
            prog.items[prog.count++] = parse_class_def(p);
        } else if (p->current.type == TOK_STRUCT) {
            prog.items[prog.count++] = parse_struct_def(p);
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
