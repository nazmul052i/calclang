#include "common.h"
#include "insn.h"

typedef struct {
    char name[CL_MAX_TEXT];
    int  addr;
} Label;

typedef struct {
    Insn  code[CL_MAX_CODE];
    int   code_count;
    /* Two label tables: globals are exported in the object's SYMS
       section so the linker can resolve cross-object references;
       locals (names starting with '.') stay private to this object. */
    Label glabels[CL_MAX_SYMBOLS];
    int   glabel_count;
    Label llabels[CL_MAX_SYMBOLS];
    int   llabel_count;
    int   var_count;
    /* String literal pool, populated from `.string` directives. */
    char *strings[CL_MAX_STRINGS];
    int   string_count;
} AsmState;

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void strip_comment(char *s) {
    char *p = strchr(s, ';');
    if (p) *p = '\0';
}

static int op_from_name(const char *s) {
    if (!strcmp(s, "PUSH"))  return OP_PUSH;
    if (!strcmp(s, "LOAD"))  return OP_LOAD;
    if (!strcmp(s, "STORE")) return OP_STORE;
    if (!strcmp(s, "ADD"))   return OP_ADD;
    if (!strcmp(s, "SUB"))   return OP_SUB;
    if (!strcmp(s, "MUL"))   return OP_MUL;
    if (!strcmp(s, "DIV"))   return OP_DIV;
    if (!strcmp(s, "MOD"))   return OP_MOD;
    if (!strcmp(s, "NEG"))   return OP_NEG;
    if (!strcmp(s, "PRINT")) return OP_PRINT;
    if (!strcmp(s, "HALT"))  return OP_HALT;
    if (!strcmp(s, "JMP"))   return OP_JMP;
    if (!strcmp(s, "JZ"))    return OP_JZ;
    if (!strcmp(s, "JNZ"))   return OP_JNZ;
    if (!strcmp(s, "EQ"))    return OP_EQ;
    if (!strcmp(s, "NEQ"))   return OP_NEQ;
    if (!strcmp(s, "LT"))    return OP_LT;
    if (!strcmp(s, "LE"))    return OP_LE;
    if (!strcmp(s, "GT"))    return OP_GT;
    if (!strcmp(s, "GE"))    return OP_GE;
    if (!strcmp(s, "AND"))   return OP_AND;
    if (!strcmp(s, "OR"))    return OP_OR;
    if (!strcmp(s, "NOT"))   return OP_NOT;
    if (!strcmp(s, "CALL"))         return OP_CALL;
    if (!strcmp(s, "RET"))          return OP_RET;
    if (!strcmp(s, "PUSH_STR"))     return OP_PUSH_STR;
    if (!strcmp(s, "ENTER"))        return OP_ENTER;
    if (!strcmp(s, "LOAD_LOCAL"))   return OP_LOAD_LOCAL;
    if (!strcmp(s, "STORE_LOCAL"))  return OP_STORE_LOCAL;
    if (!strcmp(s, "BUILTIN"))      return OP_BUILTIN;
    if (!strcmp(s, "NEW_ARRAY"))    return OP_NEW_ARRAY;
    if (!strcmp(s, "INDEX_GET"))    return OP_INDEX_GET;
    if (!strcmp(s, "INDEX_SET"))    return OP_INDEX_SET;
    if (!strcmp(s, "DROP"))         return OP_DROP;
    if (!strcmp(s, "PUSH_FN"))      return OP_PUSH_FN;
    if (!strcmp(s, "CALL_VAL"))     return OP_CALL_VAL;
    if (!strcmp(s, "NEW_MAP"))      return OP_NEW_MAP;
    if (!strcmp(s, "TYPECHECK"))     return OP_TYPECHECK;
    if (!strcmp(s, "TYPECHECK_TOP")) return OP_TYPECHECK_TOP;
    if (!strcmp(s, "DUP2"))          return OP_DUP2;
    if (!strcmp(s, "MAKE_CLOSURE"))   return OP_MAKE_CLOSURE;
    if (!strcmp(s, "LOAD_UPVAL"))     return OP_LOAD_UPVAL;
    if (!strcmp(s, "BAND"))           return OP_BAND;
    if (!strcmp(s, "BOR"))            return OP_BOR;
    if (!strcmp(s, "BXOR"))           return OP_BXOR;
    if (!strcmp(s, "SHL"))            return OP_SHL;
    if (!strcmp(s, "SHR"))            return OP_SHR;
    if (!strcmp(s, "BNOT"))           return OP_BNOT;
    return 0;
}

static int op_takes_label(int op) {
    /* CALL and MAKE_CLOSURE also take a label, but they are parsed
       separately because they carry a second operand. */
    return op == OP_JMP || op == OP_JZ || op == OP_JNZ || op == OP_PUSH_FN;
}

static void add_label(AsmState *st, const char *name, int addr, int lineno) {
    int is_local = (name[0] == '.');
    Label *table  = is_local ? st->llabels : st->glabels;
    int   *count  = is_local ? &st->llabel_count : &st->glabel_count;
    for (int i = 0; i < *count; i++) {
        if (!strcmp(table[i].name, name)) {
            fprintf(stderr, "assembler error line %d: duplicate label '%s'\n", lineno, name);
            exit(1);
        }
    }
    if (*count >= CL_MAX_SYMBOLS) cl_die("too many labels");
    cl_strncpy_z(table[*count].name, name, CL_MAX_TEXT);
    table[*count].addr = addr;
    (*count)++;
}

static void add_insn(AsmState *st, Insn in) {
    if (st->code_count >= CL_MAX_CODE) cl_die("too many instructions");
    st->code[st->code_count++] = in;
}

static int starts_with_word(const char *s, const char *word) {
    size_t n = strlen(word);
    if (strncmp(s, word, n) != 0) return 0;
    char next = s[n];
    if (next == '\0') return 1;
    return isspace((unsigned char)next) != 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: calcasm input.casm output.co\n");
        return 1;
    }

    char *text = cl_read_file(argv[1]);
    AsmState *st = (AsmState *)cl_track_calloc(sizeof(AsmState));

    char *cursor = text;
    int   lineno = 0;
    for (;;) {
        lineno++;
        char *eol = strchr(cursor, '\n');
        if (eol) *eol = '\0';

        strip_comment(cursor);
        char *s = trim(cursor);

        if (*s != '\0') {
            if (starts_with_word(s, ".var_count")) {
                st->var_count = atoi(trim(s + (int)strlen(".var_count")));
            } else if (starts_with_word(s, ".string")) {
                /* `.string <idx> "value"` — index is provided so we
                   can validate the pool was emitted contiguously. */
                const char *p = s + strlen(".string");
                while (*p == ' ' || *p == '\t') p++;
                char *end;
                long idx = strtol(p, &end, 10);
                if (end == p || idx < 0 || idx >= CL_MAX_STRINGS) {
                    fprintf(stderr, "assembler error line %d: invalid .string index\n", lineno);
                    return 1;
                }
                if ((int)idx != st->string_count) {
                    fprintf(stderr, "assembler error line %d: .string indices out of order (expected %d, got %ld)\n",
                            lineno, st->string_count, idx);
                    return 1;
                }
                p = end;
                char buf[CL_MAX_TEXT];
                cl_parse_quoted(&p, buf, sizeof(buf));
                st->strings[st->string_count++] = cl_track_strdup(buf);
            } else {
                char *colon = strchr(s, ':');
                if (colon) {
                    *colon = '\0';
                    add_label(st, trim(s), st->code_count, lineno);
                    s = trim(colon + 1);
                }
                if (*s != '\0') {
                    char opbuf[CL_MAX_TEXT]  = {0};
                    char argbuf[CL_MAX_TEXT] = {0};
                    char arg2buf[CL_MAX_TEXT] = {0};
                    sscanf(s, "%127s %127s %127s", opbuf, argbuf, arg2buf);
                    int op = op_from_name(opbuf);
                    if (!op) {
                        fprintf(stderr, "assembler error line %d: unknown opcode '%s'\n", lineno, opbuf);
                        return 1;
                    }

                    Insn in;
                    memset(&in, 0, sizeof(in));
                    in.op = op;
                    if (op == OP_PUSH) {
                        if (!*argbuf) {
                            fprintf(stderr, "assembler error line %d: PUSH needs a number operand\n", lineno);
                            return 1;
                        }
                        in.farg = strtod(argbuf, NULL);
                    } else if (op == OP_LOAD || op == OP_STORE || op == OP_PUSH_STR
                               || op == OP_ENTER || op == OP_LOAD_LOCAL || op == OP_STORE_LOCAL
                               || op == OP_BUILTIN || op == OP_NEW_ARRAY || op == OP_CALL_VAL
                               || op == OP_NEW_MAP || op == OP_LOAD_UPVAL) {
                        if (!*argbuf) {
                            fprintf(stderr, "assembler error line %d: %s needs an integer operand\n", lineno, opbuf);
                            return 1;
                        }
                        in.iarg = atoi(argbuf);
                    } else if (op_takes_label(op)) {
                        if (!*argbuf) {
                            fprintf(stderr, "assembler error line %d: %s needs a label operand\n", lineno, opbuf);
                            return 1;
                        }
                        in.has_sym = 1;
                        cl_strncpy_z(in.sym, argbuf, CL_MAX_TEXT);
                    } else if (op == OP_CALL) {
                        if (!*argbuf || !*arg2buf) {
                            fprintf(stderr, "assembler error line %d: CALL needs `<label> <n_args>` operands\n", lineno);
                            return 1;
                        }
                        in.has_sym = 1;
                        cl_strncpy_z(in.sym, argbuf, CL_MAX_TEXT);
                        in.farg = strtod(arg2buf, NULL);
                    } else if (op == OP_TYPECHECK) {
                        if (!*argbuf || !*arg2buf) {
                            fprintf(stderr, "assembler error line %d: TYPECHECK needs `<offset> <type_id>` operands\n", lineno);
                            return 1;
                        }
                        in.iarg = atoi(argbuf);
                        in.farg = strtod(arg2buf, NULL);
                    } else if (op == OP_TYPECHECK_TOP) {
                        if (!*argbuf) {
                            fprintf(stderr, "assembler error line %d: TYPECHECK_TOP needs `<type_id>` operand\n", lineno);
                            return 1;
                        }
                        in.farg = strtod(argbuf, NULL);
                    } else if (op == OP_MAKE_CLOSURE) {
                        if (!*argbuf || !*arg2buf) {
                            fprintf(stderr, "assembler error line %d: MAKE_CLOSURE needs `<label> <n_upvals>` operands\n", lineno);
                            return 1;
                        }
                        in.has_sym = 1;
                        cl_strncpy_z(in.sym, argbuf, CL_MAX_TEXT);
                        in.farg = strtod(arg2buf, NULL);
                    }
                    add_insn(st, in);
                }
            }
        }

        if (!eol) break;
        cursor = eol + 1;
    }

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fprintf(f, "COBJ4\n");
    fprintf(f, "VARCOUNT %d\n", st->var_count);
    fprintf(f, "STRINGS %d\n", st->string_count);
    for (int i = 0; i < st->string_count; i++) {
        fputs("S ", f);
        cl_fputs_quoted(f, st->strings[i]);
        fputc('\n', f);
    }
    fprintf(f, "CODE %d\n", st->code_count);
    for (int i = 0; i < st->code_count; i++) {
        Insn *in = &st->code[i];
        fprintf(f, "I %d %.17g %d %d %s\n",
            in->op, in->farg, in->iarg, in->has_sym,
            in->has_sym ? in->sym : "_");
    }
    fprintf(f, "SYMS %d\n", st->glabel_count);
    for (int i = 0; i < st->glabel_count; i++) {
        fprintf(f, "S %s %d\n", st->glabels[i].name, st->glabels[i].addr);
    }
    fprintf(f, "LOCALSYMS %d\n", st->llabel_count);
    for (int i = 0; i < st->llabel_count; i++) {
        fprintf(f, "L %s %d\n", st->llabels[i].name, st->llabels[i].addr);
    }
    if (fclose(f) != 0) cl_die("failed to close output file");

    cl_track_free(st);
    cl_track_free(text);
    return 0;
}
