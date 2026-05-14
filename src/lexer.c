#include "lexer.h"
#include <stdint.h>

static char cur(Lexer *lx) { return lx->src[lx->pos]; }

static char peek(Lexer *lx) {
    if (lx->src[lx->pos] == '\0') return '\0';
    return lx->src[lx->pos + 1];
}

static void adv(Lexer *lx) {
    char c = lx->src[lx->pos];
    if (c == '\0') return;
    if (c == '\n') { lx->line++; lx->col = 1; }
    else           { lx->col++; }
    lx->pos++;
}

static Token tok(TokenType type, const char *text, int line, int col) {
    Token t;
    t.type = type;
    t.line = line;
    t.col  = col;
    cl_strncpy_z(t.text, text, CL_MAX_TEXT);
    return t;
}

void lexer_init(Lexer *lx, const char *src) {
    lx->src = src;
    lx->pos = 0;
    lx->line = 1;
    lx->col  = 1;
    lx->current = tok(TOK_EOF, "", 1, 1);
}

const char *token_type_name(TokenType t) {
    switch (t) {
        case TOK_EOF:        return "EOF";
        case TOK_NUMBER:     return "NUMBER";
        case TOK_STRING:     return "STRING";
        case TOK_IDENTIFIER: return "IDENTIFIER";
        case TOK_LET:        return "LET";
        case TOK_PRINT:      return "PRINT";
        case TOK_IF:         return "IF";
        case TOK_ELSE:       return "ELSE";
        case TOK_WHILE:      return "WHILE";
        case TOK_FOR:        return "FOR";
        case TOK_BREAK:      return "BREAK";
        case TOK_CONTINUE:   return "CONTINUE";
        case TOK_FN:         return "FN";
        case TOK_RETURN:     return "RETURN";
        case TOK_PUB:        return "PUB";
        case TOK_PRIV:       return "PRIV";
        case TOK_EXTERN:     return "EXTERN";
        case TOK_CLASS:      return "CLASS";
        case TOK_TRY:        return "TRY";
        case TOK_CATCH:      return "CATCH";
        case TOK_THROW:      return "THROW";
        case TOK_PLUS:       return "PLUS";
        case TOK_MINUS:      return "MINUS";
        case TOK_STAR:       return "STAR";
        case TOK_SLASH:      return "SLASH";
        case TOK_PERCENT:    return "PERCENT";
        case TOK_EQUAL:      return "EQUAL";
        case TOK_EQEQ:       return "EQEQ";
        case TOK_NEQ:        return "NEQ";
        case TOK_LT:         return "LT";
        case TOK_LE:         return "LE";
        case TOK_GT:         return "GT";
        case TOK_GE:         return "GE";
        case TOK_PLUS_EQ:    return "PLUS_EQ";
        case TOK_MINUS_EQ:   return "MINUS_EQ";
        case TOK_STAR_EQ:    return "STAR_EQ";
        case TOK_SLASH_EQ:   return "SLASH_EQ";
        case TOK_PERCENT_EQ: return "PERCENT_EQ";
        case TOK_PLUSPLUS:   return "PLUSPLUS";
        case TOK_MINUSMINUS: return "MINUSMINUS";
        case TOK_AND:        return "AND";
        case TOK_OR:         return "OR";
        case TOK_BANG:       return "BANG";
        case TOK_SEMICOLON:  return "SEMICOLON";
        case TOK_COLON:      return "COLON";
        case TOK_COMMA:      return "COMMA";
        case TOK_DOT:        return "DOT";
        case TOK_LPAREN:     return "LPAREN";
        case TOK_RPAREN:     return "RPAREN";
        case TOK_LBRACE:     return "LBRACE";
        case TOK_RBRACE:     return "RBRACE";
        case TOK_LBRACKET:   return "LBRACKET";
        case TOK_RBRACKET:   return "RBRACKET";
        default:             return "UNKNOWN";
    }
}

static void skip_ws_and_comments(Lexer *lx) {
    for (;;) {
        while (isspace((unsigned char)cur(lx))) adv(lx);
        if (cur(lx) == '/' && peek(lx) == '/') {
            while (cur(lx) != '\0' && cur(lx) != '\n') adv(lx);
            continue;
        }
        if (cur(lx) == '/' && peek(lx) == '*') {
            /* Block comment. C-style: does NOT nest. The first close
               marker terminates the comment regardless of any inner
               open markers. */
            int start_line = lx->line;
            int start_col  = lx->col;
            adv(lx); adv(lx);
            int closed = 0;
            while (cur(lx) != '\0') {
                if (cur(lx) == '*' && peek(lx) == '/') {
                    adv(lx); adv(lx);
                    closed = 1;
                    break;
                }
                adv(lx);
            }
            if (!closed) cl_die_at(start_line, start_col, "unterminated /* block comment");
            continue;
        }
        break;
    }
}

Token lexer_next(Lexer *lx) {
    skip_ws_and_comments(lx);
    int line = lx->line, col = lx->col;
    char c = cur(lx);
    if (c == '\0') return tok(TOK_EOF, "", line, col);

    if (c == '"') {
        /* String literal. Token text holds the *decoded* bytes — escape
           sequences are resolved here so downstream layers never have
           to think about them. */
        adv(lx);
        char b[CL_MAX_TEXT];
        int  i = 0;
        while (cur(lx) != '"' && cur(lx) != '\0') {
            char ch;
            if (cur(lx) == '\\') {
                adv(lx);
                char esc = cur(lx);
                if (esc == '\0') cl_die_at(line, col, "unterminated string literal");
                switch (esc) {
                    case 'n':  ch = '\n'; break;
                    case 't':  ch = '\t'; break;
                    case 'r':  ch = '\r'; break;
                    case '\\': ch = '\\'; break;
                    case '"':  ch = '"';  break;
                    default:
                        cl_die_at(line, col, "unknown string escape '\\%c'", esc);
                }
                adv(lx);
            } else {
                ch = cur(lx);
                adv(lx);
            }
            if (i >= CL_MAX_TEXT - 1)
                cl_die_at(line, col, "string literal longer than %d chars", CL_MAX_TEXT - 1);
            b[i++] = ch;
        }
        if (cur(lx) != '"') cl_die_at(line, col, "unterminated string literal");
        adv(lx);
        b[i] = '\0';
        return tok(TOK_STRING, b, line, col);
    }

    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)peek(lx)))) {
        char b[CL_MAX_TEXT];
        int  i = 0, dots = 0;

        /* Hex (0x...) and binary (0b...) literals. Consume digits up to
           CL_MAX_TEXT-1, then convert to a decimal string for the rest
           of the pipeline. */
        if (c == '0' && (peek(lx) == 'x' || peek(lx) == 'X')) {
            adv(lx);  /* 0 */
            adv(lx);  /* x */
            uint64_t v = 0;
            int any = 0;
            while (1) {
                char ch = cur(lx);
                int d = -1;
                if (ch >= '0' && ch <= '9') d = ch - '0';
                else if (ch >= 'a' && ch <= 'f') d = 10 + (ch - 'a');
                else if (ch >= 'A' && ch <= 'F') d = 10 + (ch - 'A');
                else if (ch == '_') { adv(lx); continue; }   /* digit separator */
                else break;
                v = (v << 4) | (uint64_t)d;
                any = 1;
                adv(lx);
            }
            if (!any) cl_die_at(line, col, "hex literal needs at least one digit after 0x");
            snprintf(b, sizeof b, "%lld", (long long)(int64_t)v);
            return tok(TOK_NUMBER, b, line, col);
        }
        if (c == '0' && (peek(lx) == 'b' || peek(lx) == 'B')) {
            adv(lx);  /* 0 */
            adv(lx);  /* b */
            uint64_t v = 0;
            int any = 0;
            while (1) {
                char ch = cur(lx);
                if (ch == '0' || ch == '1') {
                    v = (v << 1) | (uint64_t)(ch - '0');
                    any = 1;
                    adv(lx);
                } else if (ch == '_') {
                    adv(lx);
                } else break;
            }
            if (!any) cl_die_at(line, col, "binary literal needs at least one digit after 0b");
            snprintf(b, sizeof b, "%lld", (long long)(int64_t)v);
            return tok(TOK_NUMBER, b, line, col);
        }
        while (isdigit((unsigned char)cur(lx)) || cur(lx) == '.') {
            if (cur(lx) == '.') dots++;
            if (dots > 1) cl_die_at(line, col, "invalid number with multiple decimal points");
            if (i >= CL_MAX_TEXT - 1) cl_die_at(line, col, "number too long");
            b[i++] = cur(lx);
            adv(lx);
        }
        /* Optional exponent: `e` or `E`, optional sign, then digits.
           Only consume the `e` if it's followed by a digit (or a
           sign + digit) — otherwise leave it for the identifier
           tokenizer (so the calclib `e()` builtin still tokenizes
           correctly when it appears after a number-like sequence). */
        if (cur(lx) == 'e' || cur(lx) == 'E') {
            char n1 = peek(lx);
            int  is_exp = isdigit((unsigned char)n1);
            if (!is_exp && (n1 == '+' || n1 == '-')) {
                /* Need one more char of lookahead — peek only sees
                   pos+1. Use the underlying buffer directly. */
                char n2 = lx->src[lx->pos + 1] == '\0'
                            ? '\0' : lx->src[lx->pos + 2];
                if (isdigit((unsigned char)n2)) is_exp = 1;
            }
            if (is_exp) {
                if (i >= CL_MAX_TEXT - 1) cl_die_at(line, col, "number too long");
                b[i++] = cur(lx);    /* 'e' or 'E' */
                adv(lx);
                if (cur(lx) == '+' || cur(lx) == '-') {
                    if (i >= CL_MAX_TEXT - 1) cl_die_at(line, col, "number too long");
                    b[i++] = cur(lx);
                    adv(lx);
                }
                while (isdigit((unsigned char)cur(lx))) {
                    if (i >= CL_MAX_TEXT - 1) cl_die_at(line, col, "number too long");
                    b[i++] = cur(lx);
                    adv(lx);
                }
            }
        }
        b[i] = '\0';
        return tok(TOK_NUMBER, b, line, col);
    }

    if (isalpha((unsigned char)c) || c == '_') {
        char b[CL_MAX_TEXT];
        int  i = 0;
        while (isalnum((unsigned char)cur(lx)) || cur(lx) == '_') {
            if (i >= CL_MAX_TEXT - 1) cl_die_at(line, col, "identifier too long");
            b[i++] = cur(lx);
            adv(lx);
        }
        b[i] = '\0';
        if (strcmp(b, "let")      == 0) return tok(TOK_LET,      b, line, col);
        if (strcmp(b, "print")    == 0) return tok(TOK_PRINT,    b, line, col);
        if (strcmp(b, "if")       == 0) return tok(TOK_IF,       b, line, col);
        if (strcmp(b, "else")     == 0) return tok(TOK_ELSE,     b, line, col);
        if (strcmp(b, "while")    == 0) return tok(TOK_WHILE,    b, line, col);
        if (strcmp(b, "for")      == 0) return tok(TOK_FOR,      b, line, col);
        if (strcmp(b, "break")    == 0) return tok(TOK_BREAK,    b, line, col);
        if (strcmp(b, "continue") == 0) return tok(TOK_CONTINUE, b, line, col);
        if (strcmp(b, "fn")       == 0) return tok(TOK_FN,       b, line, col);
        if (strcmp(b, "return")   == 0) return tok(TOK_RETURN,   b, line, col);
        if (strcmp(b, "pub")      == 0) return tok(TOK_PUB,      b, line, col);
        if (strcmp(b, "priv")     == 0) return tok(TOK_PRIV,     b, line, col);
        if (strcmp(b, "extern")   == 0) return tok(TOK_EXTERN,   b, line, col);
        if (strcmp(b, "class")    == 0) return tok(TOK_CLASS,    b, line, col);
        if (strcmp(b, "try")      == 0) return tok(TOK_TRY,      b, line, col);
        if (strcmp(b, "catch")    == 0) return tok(TOK_CATCH,    b, line, col);
        if (strcmp(b, "throw")    == 0) return tok(TOK_THROW,    b, line, col);
        return tok(TOK_IDENTIFIER, b, line, col);
    }

    /* Multi-character operators must be tested before consuming the
       first character so we can include both in the token text. */
    if (c == '=' && peek(lx) == '=') { adv(lx); adv(lx); return tok(TOK_EQEQ,       "==", line, col); }
    if (c == '!' && peek(lx) == '=') { adv(lx); adv(lx); return tok(TOK_NEQ,        "!=", line, col); }
    if (c == '<' && peek(lx) == '=') { adv(lx); adv(lx); return tok(TOK_LE,         "<=", line, col); }
    if (c == '>' && peek(lx) == '=') { adv(lx); adv(lx); return tok(TOK_GE,         ">=", line, col); }
    if (c == '&' && peek(lx) == '&') { adv(lx); adv(lx); return tok(TOK_AND,        "&&", line, col); }
    if (c == '|' && peek(lx) == '|') { adv(lx); adv(lx); return tok(TOK_OR,         "||", line, col); }
    if (c == '+' && peek(lx) == '+') { adv(lx); adv(lx); return tok(TOK_PLUSPLUS,   "++", line, col); }
    if (c == '+' && peek(lx) == '=') { adv(lx); adv(lx); return tok(TOK_PLUS_EQ,    "+=", line, col); }
    if (c == '-' && peek(lx) == '-') { adv(lx); adv(lx); return tok(TOK_MINUSMINUS, "--", line, col); }
    if (c == '-' && peek(lx) == '=') { adv(lx); adv(lx); return tok(TOK_MINUS_EQ,   "-=", line, col); }
    if (c == '*' && peek(lx) == '=') { adv(lx); adv(lx); return tok(TOK_STAR_EQ,    "*=", line, col); }
    if (c == '/' && peek(lx) == '=') { adv(lx); adv(lx); return tok(TOK_SLASH_EQ,   "/=", line, col); }
    if (c == '%' && peek(lx) == '=') { adv(lx); adv(lx); return tok(TOK_PERCENT_EQ, "%=", line, col); }

    /* Bitwise shifts and compound-shifts: <<, <<=, >>, >>=. Must come
       before the <= / >= checks above? No — <= and >= already required
       single-char follow. Here we check the doubled forms. */
    if (c == '<' && peek(lx) == '<') {
        adv(lx); adv(lx);
        if (cur(lx) == '=') { adv(lx); return tok(TOK_LSHIFT_EQ, "<<=", line, col); }
        return tok(TOK_LSHIFT, "<<", line, col);
    }
    if (c == '>' && peek(lx) == '>') {
        adv(lx); adv(lx);
        if (cur(lx) == '=') { adv(lx); return tok(TOK_RSHIFT_EQ, ">>=", line, col); }
        return tok(TOK_RSHIFT, ">>", line, col);
    }
    if (c == '&' && peek(lx) == '=') { adv(lx); adv(lx); return tok(TOK_AMP_EQ,    "&=", line, col); }
    if (c == '|' && peek(lx) == '=') { adv(lx); adv(lx); return tok(TOK_PIPE_EQ,   "|=", line, col); }
    if (c == '^' && peek(lx) == '=') { adv(lx); adv(lx); return tok(TOK_CARET_EQ,  "^=", line, col); }

    adv(lx);
    switch (c) {
        case '+': return tok(TOK_PLUS,      "+", line, col);
        case '-': return tok(TOK_MINUS,     "-", line, col);
        case '*': return tok(TOK_STAR,      "*", line, col);
        case '/': return tok(TOK_SLASH,     "/", line, col);
        case '%': return tok(TOK_PERCENT,   "%", line, col);
        case '=': return tok(TOK_EQUAL,     "=", line, col);
        case '<': return tok(TOK_LT,        "<", line, col);
        case '>': return tok(TOK_GT,        ">", line, col);
        case '!': return tok(TOK_BANG,      "!", line, col);
        case ';': return tok(TOK_SEMICOLON, ";", line, col);
        case ':': return tok(TOK_COLON,     ":", line, col);
        case ',': return tok(TOK_COMMA,     ",", line, col);
        case '.': return tok(TOK_DOT,       ".", line, col);
        case '(': return tok(TOK_LPAREN,    "(", line, col);
        case ')': return tok(TOK_RPAREN,    ")", line, col);
        case '{': return tok(TOK_LBRACE,    "{", line, col);
        case '}': return tok(TOK_RBRACE,    "}", line, col);
        case '[': return tok(TOK_LBRACKET,  "[", line, col);
        case ']': return tok(TOK_RBRACKET,  "]", line, col);
        case '&': return tok(TOK_AMP,       "&", line, col);
        case '|': return tok(TOK_PIPE,      "|", line, col);
        case '^': return tok(TOK_CARET,     "^", line, col);
        case '~': return tok(TOK_TILDE,     "~", line, col);
        default:  cl_die_at(line, col, "unknown character '%c'", c);
    }
    return tok(TOK_EOF, "", line, col); /* unreachable */
}
