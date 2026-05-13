#ifndef CALCLANG_LEXER_H
#define CALCLANG_LEXER_H
#include "common.h"

typedef enum {
    TOK_EOF = 0,
    TOK_NUMBER,
    TOK_STRING,
    TOK_IDENTIFIER,

    /* keywords */
    TOK_LET,
    TOK_PRINT,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_FN,
    TOK_RETURN,
    TOK_PUB,
    TOK_PRIV,
    TOK_EXTERN,
    TOK_CLASS,
    TOK_TRY,
    TOK_CATCH,
    TOK_THROW,

    /* arithmetic */
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,

    /* assignment and comparison */
    TOK_EQUAL,    /* =  */
    TOK_EQEQ,     /* == */
    TOK_NEQ,      /* != */
    TOK_LT,       /* <  */
    TOK_LE,       /* <= */
    TOK_GT,       /* >  */
    TOK_GE,       /* >= */

    /* compound assignment */
    TOK_PLUS_EQ,    /* += */
    TOK_MINUS_EQ,   /* -= */
    TOK_STAR_EQ,    /* *= */
    TOK_SLASH_EQ,   /* /= */
    TOK_PERCENT_EQ, /* %= */
    TOK_PLUSPLUS,   /* ++ */
    TOK_MINUSMINUS, /* -- */

    /* logical */
    TOK_AND,      /* && */
    TOK_OR,       /* || */
    TOK_BANG,     /* !  */

    /* punctuation */
    TOK_SEMICOLON,
    TOK_COLON,
    TOK_COMMA,
    TOK_DOT,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET
} TokenType;

typedef struct {
    TokenType type;
    char text[CL_MAX_TEXT];
    int  line;
    int  col;
} Token;

typedef struct {
    const char *src;
    size_t pos;
    int    line;
    int    col;
    Token  current;
} Lexer;

void lexer_init(Lexer *lx, const char *src);
Token lexer_next(Lexer *lx);
const char *token_type_name(TokenType t);

#endif
