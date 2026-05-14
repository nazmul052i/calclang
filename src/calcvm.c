#include "common.h"
#include "insn.h"
#include "value.h"
#include "calclib.h"
#include <math.h>
#include <time.h>

/* TypeAnnot codes kept in sync with include/ast.h. Duplicated here so
   the VM does not have to link against ast.c (which transitively pulls
   in the lexer and parser). The relationship is one-way: codegen emits
   these numbers into TYPECHECK instructions; the VM reads them back. */
enum {
    VM_TYPE_ANY  = 0,
    VM_TYPE_NUM  = 1,
    VM_TYPE_STR  = 2,
    VM_TYPE_ARR  = 3,
    VM_TYPE_BOOL = 4,
    VM_TYPE_MAP  = 5,
    VM_TYPE_FN   = 6
};

static const char *value_tag_name(ValueTag t) {
    switch (t) {
        case VAL_NUM: return "num";
        case VAL_STR: return "str";
        case VAL_ARR: return "arr";
        case VAL_FN:  return "fn";
        case VAL_MAP: return "map";
    }
    return "?";
}

static const char *vm_type_name(int t) {
    switch (t) {
        case VM_TYPE_NUM:  return "num";
        case VM_TYPE_STR:  return "str";
        case VM_TYPE_ARR:  return "arr";
        case VM_TYPE_BOOL: return "bool";
        case VM_TYPE_MAP:  return "map";
        case VM_TYPE_FN:   return "fn";
        case VM_TYPE_ANY:  return "any";
    }
    return "?";
}

static int typecheck_accepts(int expected, ValueTag actual) {
    switch (expected) {
        case VM_TYPE_ANY:  return 1;
        case VM_TYPE_NUM:  return actual == VAL_NUM;
        case VM_TYPE_BOOL: return actual == VAL_NUM;   /* alias */
        case VM_TYPE_STR:  return actual == VAL_STR;
        case VM_TYPE_ARR:  return actual == VAL_ARR;
        case VM_TYPE_MAP:  return actual == VAL_MAP;
        case VM_TYPE_FN:   return actual == VAL_FN;
    }
    return 0;
}

/* qsort comparators for array_sort. Switch on the element tag we
   already verified is uniform across the array. */
static int sort_cmp_num(const void *a, const void *b) {
    double da = ((const Value *)a)->as.num;
    double db = ((const Value *)b)->as.num;
    return (da > db) - (da < db);
}
static int sort_cmp_str(const void *a, const void *b) {
    return strcmp(((const Value *)a)->as.str, ((const Value *)b)->as.str);
}

static _Noreturn void type_error(const char *op_name) {
    fprintf(stderr, "error: type error in '%s' (operands must be numbers; strings only support print, ==, !=, and truthiness)\n",
            op_name);
    exit(1);
}

/* Lightweight printf-style formatter for the `fmt(format, [args])` builtin.
   Supports %d (int), %f / %.Nf (float), %e (scientific), %g (default),
   %s (string), %x (hex int), %o (octal int), %b (binary int), %% (literal),
   plus optional width and zero-padding (%5d, %05d, %-5d, %8.2f).
   Args is an array of Values. Returns a heap-allocated string. */
static char *cl_vm_format(const char *fmt, Array *args) {
    char *buf = (char *)cl_track_malloc(64);
    size_t cap = 64;
    size_t len = 0;
    int    arg_idx = 0;
    #define APPEND(c) do { \
        if (len + 1 >= cap) { cap *= 2; buf = (char *)cl_track_realloc(buf, cap); } \
        buf[len++] = (char)(c); \
    } while (0)
    #define APPEND_STR(s) do { \
        const char *_p = (s); while (*_p) APPEND(*_p++); \
    } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { APPEND(*p); continue; }
        p++;
        if (*p == '%') { APPEND('%'); continue; }
        if (*p == '\0') break;
        /* parse flags / width / precision */
        char specbuf[32];
        int  si = 0;
        specbuf[si++] = '%';
        if (*p == '-' || *p == '0' || *p == '+' || *p == ' ' || *p == '#') {
            specbuf[si++] = *p++;
        }
        while (*p >= '0' && *p <= '9') {
            if (si < (int)sizeof(specbuf) - 4) specbuf[si++] = *p;
            p++;
        }
        if (*p == '.') {
            if (si < (int)sizeof(specbuf) - 4) specbuf[si++] = *p;
            p++;
            while (*p >= '0' && *p <= '9') {
                if (si < (int)sizeof(specbuf) - 4) specbuf[si++] = *p;
                p++;
            }
        }
        char conv = *p;
        if (arg_idx >= (args ? args->count : 0)) {
            cl_die("fmt: not enough arguments for format string");
        }
        Value v = args->items[arg_idx++];
        char tmp[128];
        switch (conv) {
            case 'd':
            case 'i': {
                if (v.tag != VAL_NUM) cl_die("fmt: %d expects num");
                specbuf[si++] = 'l'; specbuf[si++] = 'l';
                specbuf[si++] = 'd'; specbuf[si] = '\0';
                snprintf(tmp, sizeof tmp, specbuf, (long long)v.as.num);
                APPEND_STR(tmp);
                break;
            }
            case 'x':
            case 'X':
            case 'o': {
                if (v.tag != VAL_NUM) cl_die("fmt: %x/%o expects num");
                specbuf[si++] = 'l'; specbuf[si++] = 'l';
                specbuf[si++] = conv; specbuf[si] = '\0';
                snprintf(tmp, sizeof tmp, specbuf, (long long)v.as.num);
                APPEND_STR(tmp);
                break;
            }
            case 'b': {
                if (v.tag != VAL_NUM) cl_die("fmt: %b expects num");
                /* No libc %b — render manually. */
                unsigned long long u = (unsigned long long)(long long)v.as.num;
                char bin[65];
                int  bi = 0;
                if (u == 0) bin[bi++] = '0';
                while (u > 0) { bin[bi++] = (u & 1) ? '1' : '0'; u >>= 1; }
                while (bi > 0) APPEND(bin[--bi]);
                break;
            }
            case 'f':
            case 'e':
            case 'E':
            case 'g':
            case 'G': {
                if (v.tag != VAL_NUM) cl_die("fmt: %f/%e/%g expects num");
                specbuf[si++] = conv; specbuf[si] = '\0';
                snprintf(tmp, sizeof tmp, specbuf, v.as.num);
                APPEND_STR(tmp);
                break;
            }
            case 's': {
                const char *s = NULL;
                if (v.tag == VAL_STR) s = v.as.str;
                else {
                    /* coerce non-strings via the standard %.10g rendering */
                    if (v.tag == VAL_NUM) {
                        snprintf(tmp, sizeof tmp, "%.10g", v.as.num);
                        s = tmp;
                    } else {
                        s = "?";
                    }
                }
                specbuf[si++] = 's'; specbuf[si] = '\0';
                /* let snprintf handle width/precision */
                size_t need = strlen(s) + 32;
                char *t2 = (char *)cl_track_malloc(need);
                snprintf(t2, need, specbuf, s);
                APPEND_STR(t2);
                break;
            }
            case 'c': {
                if (v.tag == VAL_NUM) { APPEND((char)(int)v.as.num); }
                else if (v.tag == VAL_STR && v.as.str) { APPEND(v.as.str[0]); }
                else cl_die("fmt: %c expects num or str");
                break;
            }
            default:
                cl_die("fmt: unknown format specifier");
        }
    }
    if (len + 1 >= cap) { cap = len + 1; buf = (char *)cl_track_realloc(buf, cap); }
    buf[len] = '\0';
    return buf;
    #undef APPEND
    #undef APPEND_STR
}

/* Render an array element (used inside [...] formatting): numbers are
   plain, strings get quoted so [1, "two"] is distinguishable from
   [1, two]. Depth limit prevents pathological recursion. */
static void print_value_elem(Value v, int depth);

static void print_value_elem(Value v, int depth) {
    if (depth > 8) { fputs("...", stdout); return; }
    if (v.tag == VAL_NUM) {
        printf("%.10g", v.as.num);
    } else if (v.tag == VAL_STR) {
        printf("\"%s\"", v.as.str ? v.as.str : "");
    } else if (v.tag == VAL_ARR) {
        Array *a = v.as.arr;
        putchar('[');
        if (a) {
            for (int i = 0; i < a->count; i++) {
                if (i > 0) fputs(", ", stdout);
                print_value_elem(a->items[i], depth + 1);
            }
        }
        putchar(']');
    } else if (v.tag == VAL_FN) {
        printf("<fn @%d>", v.as.fn ? v.as.fn->pc : -1);
    } else if (v.tag == VAL_MAP) {
        Map *m = v.as.map;
        putchar('{');
        if (m) {
            for (int i = 0; i < m->count; i++) {
                if (i > 0) fputs(", ", stdout);
                print_value_elem(m->entries[i].key,   depth + 1);
                fputs(": ", stdout);
                print_value_elem(m->entries[i].value, depth + 1);
            }
        }
        putchar('}');
    }
}

/* Top-level print: bare for numbers/strings, bracketed for arrays,
   braced for maps. */
static void print_value_top(Value v) {
    if (v.tag == VAL_NUM) {
        printf("%.10g\n", v.as.num);
    } else if (v.tag == VAL_STR) {
        printf("%s\n", v.as.str ? v.as.str : "");
    } else if (v.tag == VAL_ARR) {
        Array *a = v.as.arr;
        putchar('[');
        if (a) {
            for (int i = 0; i < a->count; i++) {
                if (i > 0) fputs(", ", stdout);
                print_value_elem(a->items[i], 1);
            }
        }
        puts("]");
    } else if (v.tag == VAL_FN) {
        printf("<fn @%d>\n", v.as.fn ? v.as.fn->pc : -1);
    } else if (v.tag == VAL_MAP) {
        Map *m = v.as.map;
        putchar('{');
        if (m) {
            for (int i = 0; i < m->count; i++) {
                if (i > 0) fputs(", ", stdout);
                print_value_elem(m->entries[i].key,   1);
                fputs(": ", stdout);
                print_value_elem(m->entries[i].value, 1);
            }
        }
        puts("}");
    }
}

/* Linear lookup in a map. Returns the index of the matching entry
   or -1 if the key isn't present. */
static int map_find(Map *m, Value key) {
    if (!m) return -1;
    for (int i = 0; i < m->count; i++) {
        if (val_equal(m->entries[i].key, key)) return i;
    }
    return -1;
}

typedef struct {
    int      ret_pc;
    int      saved_fp;
    Closure *saved_closure;
} CallFrame;

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: calcvm program.cexe\n");
        return 1;
    }

    /* Seed rand() once at startup so calclib's random() is unpredictable
       across runs without needing the program to call a seed function. */
    srand((unsigned)time(NULL));

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }

    char tag[64];
    int var_count = 0, code_count = 0, string_count = 0;

    if (fscanf(f, "%63s", tag) != 1 || strcmp(tag, "CEXE3") != 0) {
        fclose(f);
        cl_die("not a CEXE3 file (regenerate with the current linker)");
    }
    if (fscanf(f, "%63s %d", tag, &var_count)    != 2 || strcmp(tag, "VARCOUNT") != 0) { fclose(f); cl_die("bad executable VARCOUNT"); }
    if (fscanf(f, "%63s %d", tag, &string_count) != 2 || strcmp(tag, "STRINGS")  != 0) { fclose(f); cl_die("bad executable STRINGS");  }
    if (var_count    < 0 || var_count    > CL_MAX_SYMBOLS) { fclose(f); cl_die("executable var_count out of range"); }
    if (string_count < 0 || string_count > CL_MAX_STRINGS) { fclose(f); cl_die("executable string_count out of range"); }

    char **strings = NULL;
    if (string_count > 0) {
        strings = (char **)cl_track_calloc(sizeof(char *) * (size_t)string_count);
        for (int i = 0; i < string_count; i++) {
            if (fscanf(f, "%63s", tag) != 1 || strcmp(tag, "S") != 0) {
                fclose(f);
                cl_die("bad executable string record");
            }
            char buf[CL_MAX_TEXT];
            cl_fread_quoted(f, buf, sizeof(buf));
            strings[i] = cl_track_strdup(buf);
        }
    }

    if (fscanf(f, "%63s %d", tag, &code_count) != 2 || strcmp(tag, "CODE") != 0) {
        fclose(f);
        cl_die("bad executable CODE");
    }
    if (code_count < 0 || code_count > CL_MAX_CODE) {
        fclose(f);
        cl_die("executable code_count out of range");
    }

    Insn *code = NULL;
    if (code_count > 0) {
        code = (Insn *)cl_track_calloc(sizeof(Insn) * (size_t)code_count);
        for (int i = 0; i < code_count; i++) {
            int got = fscanf(f, "%63s %d %lf %d",
                tag, &code[i].op, &code[i].farg, &code[i].iarg);
            if (got != 4 || strcmp(tag, "I") != 0) {
                fclose(f);
                cl_die("bad executable instruction");
            }
        }
    }
    fclose(f);

    size_t mem_count = var_count > 0 ? (size_t)var_count : 1;
    Value *mem    = (Value *)cl_track_calloc(sizeof(Value) * mem_count);
    Value *stack  = (Value *)cl_track_calloc(sizeof(Value) * CL_MAX_CODE);
    /* Paired return-PC and saved-fp stack: each CALL pushes one entry,
       each RET pops one. fp itself is a separate register. */
    enum { CALL_STACK_MAX = 1024 };
    CallFrame *call_stack = (CallFrame *)cl_track_calloc(sizeof(CallFrame) * CALL_STACK_MAX);
    Closure *current_closure = NULL;   /* set on CALL_VAL; LOAD_UPVAL reads from it */
    int csp = 0;
    int sp  = 0;
    int fp  = 0;
    int pc  = 0;
    int rc  = 0;

    while (pc >= 0 && pc < code_count) {
        Insn in = code[pc++];
        switch (in.op) {
            case OP_PUSH:
                if (sp >= CL_MAX_CODE) cl_die("stack overflow");
                stack[sp++] = val_num(in.farg);
                break;
            case OP_PUSH_STR:
                if (sp >= CL_MAX_CODE) cl_die("stack overflow");
                if (in.iarg < 0 || in.iarg >= string_count) cl_die("PUSH_STR index out of range");
                stack[sp++] = val_str(strings[in.iarg]);
                break;
            case OP_LOAD:
                if (in.iarg < 0 || in.iarg >= var_count) cl_die("LOAD slot out of range");
                if (sp >= CL_MAX_CODE) cl_die("stack overflow");
                stack[sp++] = mem[in.iarg];
                break;
            case OP_STORE:
                if (in.iarg < 0 || in.iarg >= var_count) cl_die("STORE slot out of range");
                if (sp <= 0) cl_die("stack underflow");
                mem[in.iarg] = stack[--sp];
                break;
            case OP_LOAD_LOCAL: {
                int idx = fp + in.iarg;
                if (idx < 0 || idx >= sp) cl_die("LOAD_LOCAL out of frame");
                if (sp >= CL_MAX_CODE) cl_die("stack overflow");
                stack[sp++] = stack[idx];
                break;
            }
            case OP_STORE_LOCAL: {
                int idx = fp + in.iarg;
                if (sp <= 0) cl_die("stack underflow");
                if (idx < 0 || idx >= sp - 1) cl_die("STORE_LOCAL out of frame");
                stack[idx] = stack[--sp];
                break;
            }
            case OP_ENTER: {
                if (in.iarg < 0) cl_die("ENTER with negative size");
                if (sp + in.iarg > CL_MAX_CODE) cl_die("stack overflow on ENTER");
                /* Zero-initialise the new slots so undefined reads
                   produce a numeric 0 rather than uninitialised junk. */
                for (int i = 0; i < in.iarg; i++) stack[sp + i] = val_num(0);
                sp += in.iarg;
                break;
            }
            case OP_ADD:
                if (sp < 2) cl_die("stack underflow");
                {
                    Value lhs = stack[sp - 2];
                    Value rhs = stack[sp - 1];
                    if (lhs.tag == VAL_NUM && rhs.tag == VAL_NUM) {
                        stack[sp - 2] = val_num(lhs.as.num + rhs.as.num);
                    } else if ((lhs.tag == VAL_STR || lhs.tag == VAL_NUM)
                            && (rhs.tag == VAL_STR || rhs.tag == VAL_NUM)
                            && (lhs.tag == VAL_STR || rhs.tag == VAL_STR)) {
                        /* At least one operand is a string: coerce
                           the other to its %.10g form and concatenate.
                           Result is a fresh tracked allocation. */
                        char num_buf[64];
                        const char *lstr;
                        const char *rstr;
                        if (lhs.tag == VAL_NUM) {
                            snprintf(num_buf, sizeof(num_buf), "%.10g", lhs.as.num);
                            lstr = num_buf;
                            rstr = rhs.as.str;
                        } else if (rhs.tag == VAL_NUM) {
                            lstr = lhs.as.str;
                            snprintf(num_buf, sizeof(num_buf), "%.10g", rhs.as.num);
                            rstr = num_buf;
                        } else {
                            lstr = lhs.as.str;
                            rstr = rhs.as.str;
                        }
                        size_t la = strlen(lstr);
                        size_t lb = strlen(rstr);
                        char *buf = (char *)cl_track_malloc(la + lb + 1);
                        memcpy(buf,      lstr, la);
                        memcpy(buf + la, rstr, lb);
                        buf[la + lb] = '\0';
                        stack[sp - 2] = val_str(buf);
                    } else {
                        type_error("+");
                    }
                    sp--;
                }
                break;
            case OP_SUB:
                if (sp < 2) cl_die("stack underflow");
                if (stack[sp-2].tag != VAL_NUM || stack[sp-1].tag != VAL_NUM) type_error("-");
                stack[sp - 2] = val_num(stack[sp-2].as.num - stack[sp-1].as.num); sp--;
                break;
            case OP_MUL:
                if (sp < 2) cl_die("stack underflow");
                if (stack[sp-2].tag != VAL_NUM || stack[sp-1].tag != VAL_NUM) type_error("*");
                stack[sp - 2] = val_num(stack[sp-2].as.num * stack[sp-1].as.num); sp--;
                break;
            case OP_DIV:
                if (sp < 2) cl_die("stack underflow");
                if (stack[sp-2].tag != VAL_NUM || stack[sp-1].tag != VAL_NUM) type_error("/");
                if (stack[sp-1].as.num == 0) cl_die("division by zero");
                stack[sp - 2] = val_num(stack[sp-2].as.num / stack[sp-1].as.num); sp--;
                break;
            case OP_MOD:
                if (sp < 2) cl_die("stack underflow");
                if (stack[sp-2].tag != VAL_NUM || stack[sp-1].tag != VAL_NUM) type_error("%");
                if (stack[sp-1].as.num == 0) cl_die("modulo by zero");
                stack[sp - 2] = val_num(fmod(stack[sp-2].as.num, stack[sp-1].as.num)); sp--;
                break;
            case OP_NEG:
                if (sp <= 0) cl_die("stack underflow");
                if (stack[sp-1].tag != VAL_NUM) type_error("unary -");
                stack[sp - 1] = val_num(-stack[sp-1].as.num);
                break;
            case OP_EQ:
                if (sp < 2) cl_die("stack underflow");
                stack[sp - 2] = val_num(val_equal(stack[sp-2], stack[sp-1]) ? 1.0 : 0.0); sp--;
                break;
            case OP_NEQ:
                if (sp < 2) cl_die("stack underflow");
                stack[sp - 2] = val_num(val_equal(stack[sp-2], stack[sp-1]) ? 0.0 : 1.0); sp--;
                break;
            case OP_LT:
            case OP_LE:
            case OP_GT:
            case OP_GE: {
                if (sp < 2) cl_die("stack underflow");
                Value lhs = stack[sp - 2];
                Value rhs = stack[sp - 1];
                int r = 0;
                /* Numbers and strings (lexicographic) compare with
                   themselves; mixed-type ordering is a type error. */
                if (lhs.tag == VAL_NUM && rhs.tag == VAL_NUM) {
                    double a = lhs.as.num, b = rhs.as.num;
                    switch (in.op) {
                        case OP_LT: r = a <  b; break;
                        case OP_LE: r = a <= b; break;
                        case OP_GT: r = a >  b; break;
                        case OP_GE: r = a >= b; break;
                    }
                } else if (lhs.tag == VAL_STR && rhs.tag == VAL_STR) {
                    int c = strcmp(lhs.as.str, rhs.as.str);
                    switch (in.op) {
                        case OP_LT: r = c <  0; break;
                        case OP_LE: r = c <= 0; break;
                        case OP_GT: r = c >  0; break;
                        case OP_GE: r = c >= 0; break;
                    }
                } else {
                    const char *name = "<";
                    switch (in.op) {
                        case OP_LE: name = "<="; break;
                        case OP_GT: name = ">";  break;
                        case OP_GE: name = ">="; break;
                    }
                    type_error(name);
                }
                stack[sp - 2] = val_num(r ? 1.0 : 0.0);
                sp--;
                break;
            }
            case OP_AND:
                if (sp < 2) cl_die("stack underflow");
                stack[sp - 2] = val_num((val_truthy(stack[sp-2]) && val_truthy(stack[sp-1])) ? 1.0 : 0.0); sp--;
                break;
            case OP_OR:
                if (sp < 2) cl_die("stack underflow");
                stack[sp - 2] = val_num((val_truthy(stack[sp-2]) || val_truthy(stack[sp-1])) ? 1.0 : 0.0); sp--;
                break;
            case OP_NOT:
                if (sp <= 0) cl_die("stack underflow");
                stack[sp - 1] = val_num(val_truthy(stack[sp-1]) ? 0.0 : 1.0);
                break;
            case OP_BAND:
            case OP_BOR:
            case OP_BXOR:
            case OP_SHL:
            case OP_SHR: {
                if (sp < 2) cl_die("stack underflow");
                const char *name = "&";
                if (in.op == OP_BOR)  name = "|";
                if (in.op == OP_BXOR) name = "^";
                if (in.op == OP_SHL)  name = "<<";
                if (in.op == OP_SHR)  name = ">>";
                if (stack[sp-2].tag != VAL_NUM || stack[sp-1].tag != VAL_NUM) type_error(name);
                int64_t a = (int64_t)stack[sp-2].as.num;
                int64_t b = (int64_t)stack[sp-1].as.num;
                int64_t r = 0;
                switch (in.op) {
                    case OP_BAND: r = a & b;  break;
                    case OP_BOR:  r = a | b;  break;
                    case OP_BXOR: r = a ^ b;  break;
                    /* Shifts: mask the count to [0, 63] to match the
                       hardware behaviour and avoid UB. */
                    case OP_SHL:  r = a << (b & 63); break;
                    case OP_SHR:  r = a >> (b & 63); break;  /* arithmetic — sign-extends */
                }
                stack[sp - 2] = val_num((double)r); sp--;
                break;
            }
            case OP_BNOT:
                if (sp <= 0) cl_die("stack underflow");
                if (stack[sp-1].tag != VAL_NUM) type_error("~");
                stack[sp - 1] = val_num((double)(~(int64_t)stack[sp-1].as.num));
                break;
            case OP_PRINT:
                if (sp <= 0) cl_die("stack underflow");
                print_value_top(stack[--sp]);
                break;
            case OP_JMP:
                if (in.iarg < 0 || in.iarg >= code_count) cl_die("jump target out of range");
                pc = in.iarg;
                break;
            case OP_JZ:
                if (sp <= 0) cl_die("stack underflow");
                {
                    Value v = stack[--sp];
                    if (!val_truthy(v)) {
                        if (in.iarg < 0 || in.iarg >= code_count) cl_die("jump target out of range");
                        pc = in.iarg;
                    }
                }
                break;
            case OP_JNZ:
                if (sp <= 0) cl_die("stack underflow");
                {
                    Value v = stack[--sp];
                    if (val_truthy(v)) {
                        if (in.iarg < 0 || in.iarg >= code_count) cl_die("jump target out of range");
                        pc = in.iarg;
                    }
                }
                break;
            case OP_CALL: {
                if (csp >= CALL_STACK_MAX) cl_die("call stack overflow");
                if (in.iarg < 0 || in.iarg >= code_count) cl_die("CALL target out of range");
                int n_args = (int)in.farg;
                if (n_args < 0 || sp < n_args) cl_die("CALL with bad arg count");
                call_stack[csp].ret_pc        = pc;
                call_stack[csp].saved_fp      = fp;
                call_stack[csp].saved_closure = current_closure;
                csp++;
                /* OP_CALL targets a label, not a closure value, so there
                   are no upvalues to inherit. LOAD_UPVAL inside the
                   callee would be a programmer error. */
                current_closure = NULL;
                fp = sp - n_args;
                pc = in.iarg;
                break;
            }
            case OP_RET: {
                if (csp <= 0) cl_die("call stack underflow");
                if (sp <= 0)  cl_die("stack underflow at RET");
                Value retval = stack[--sp];
                /* Drop the entire frame (args + locals) and restore. */
                sp = fp;
                csp--;
                fp              = call_stack[csp].saved_fp;
                pc              = call_stack[csp].ret_pc;
                current_closure = call_stack[csp].saved_closure;
                if (sp >= CL_MAX_CODE) cl_die("stack overflow on RET");
                stack[sp++] = retval;
                break;
            }
            case OP_BUILTIN:
                switch (in.iarg) {
                    case BI_SQRT: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("sqrt");
                        if (v.as.num < 0) cl_die("sqrt of negative number");
                        stack[sp - 1] = val_num(sqrt(v.as.num));
                        break;
                    }
                    case BI_FLOOR: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("floor");
                        stack[sp - 1] = val_num(floor(v.as.num));
                        break;
                    }
                    case BI_CEIL: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("ceil");
                        stack[sp - 1] = val_num(ceil(v.as.num));
                        break;
                    }
                    case BI_ABS: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("abs");
                        stack[sp - 1] = val_num(fabs(v.as.num));
                        break;
                    }
                    case BI_POW: {
                        if (sp < 2) cl_die("stack underflow");
                        Value b = stack[sp - 2];
                        Value e = stack[sp - 1];
                        if (b.tag != VAL_NUM || e.tag != VAL_NUM) type_error("pow");
                        stack[sp - 2] = val_num(pow(b.as.num, e.as.num));
                        sp--;
                        break;
                    }
                    case BI_TO_STR: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag == VAL_STR) break;   /* already a string */
                        char tmp[64];
                        int n = snprintf(tmp, sizeof(tmp), "%.10g", v.as.num);
                        if (n < 0 || n >= (int)sizeof(tmp)) cl_die("to_str: formatting failed");
                        stack[sp - 1] = val_str(cl_track_strdup(tmp));
                        break;
                    }
                    case BI_TO_NUM: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag == VAL_NUM) break;   /* already a number */
                        const char *s = v.as.str ? v.as.str : "";
                        while (isspace((unsigned char)*s)) s++;
                        if (!*s) cl_die("to_num: empty string");
                        char *endptr;
                        double d = strtod(s, &endptr);
                        if (endptr == s) cl_die("to_num: not a number");
                        while (isspace((unsigned char)*endptr)) endptr++;
                        if (*endptr != '\0') cl_die("to_num: trailing garbage");
                        stack[sp - 1] = val_num(d);
                        break;
                    }
                    case BI_LEN: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag == VAL_STR) {
                            stack[sp - 1] = val_num((double)strlen(v.as.str));
                        } else if (v.tag == VAL_ARR) {
                            stack[sp - 1] = val_num((double)(v.as.arr ? v.as.arr->count : 0));
                        } else if (v.tag == VAL_MAP) {
                            stack[sp - 1] = val_num((double)(v.as.map ? v.as.map->count : 0));
                        } else {
                            type_error("len");
                        }
                        break;
                    }
                    case BI_PUSH: {
                        if (sp < 2) cl_die("stack underflow");
                        Value valv = stack[sp - 1];
                        Value tgt  = stack[sp - 2];
                        if (tgt.tag != VAL_ARR || !tgt.as.arr) type_error("push");
                        Array *a = tgt.as.arr;
                        if (a->count >= a->cap) {
                            int new_cap = a->cap == 0 ? 4 : a->cap * 2;
                            a->items = (Value *)cl_track_realloc(a->items, sizeof(Value) * (size_t)new_cap);
                            a->cap = new_cap;
                        }
                        a->items[a->count++] = valv;
                        stack[sp - 2] = val_num((double)a->count);
                        sp--;
                        break;
                    }
                    case BI_READ_LINE: {
                        if (sp >= CL_MAX_CODE) cl_die("stack overflow");
                        char buf[1024];
                        if (!fgets(buf, sizeof(buf), stdin)) {
                            /* EOF or error -> empty string. */
                            stack[sp++] = val_str(cl_track_strdup(""));
                        } else {
                            size_t len = strlen(buf);
                            if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
                            if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';
                            stack[sp++] = val_str(cl_track_strdup(buf));
                        }
                        break;
                    }

                    /* ----- additional math ----- */
                    case BI_MIN: {
                        if (sp < 2) cl_die("stack underflow");
                        Value a = stack[sp - 2], b = stack[sp - 1];
                        if (a.tag != VAL_NUM || b.tag != VAL_NUM) type_error("min");
                        stack[sp - 2] = val_num(a.as.num < b.as.num ? a.as.num : b.as.num);
                        sp--;
                        break;
                    }
                    case BI_MAX: {
                        if (sp < 2) cl_die("stack underflow");
                        Value a = stack[sp - 2], b = stack[sp - 1];
                        if (a.tag != VAL_NUM || b.tag != VAL_NUM) type_error("max");
                        stack[sp - 2] = val_num(a.as.num > b.as.num ? a.as.num : b.as.num);
                        sp--;
                        break;
                    }
                    case BI_INT: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("int");
                        stack[sp - 1] = val_num(trunc(v.as.num));
                        break;
                    }
                    case BI_ROUND: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("round");
                        stack[sp - 1] = val_num(round(v.as.num));
                        break;
                    }
                    case BI_SIN: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("sin");
                        stack[sp - 1] = val_num(sin(v.as.num));
                        break;
                    }
                    case BI_COS: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("cos");
                        stack[sp - 1] = val_num(cos(v.as.num));
                        break;
                    }
                    case BI_TAN: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("tan");
                        stack[sp - 1] = val_num(tan(v.as.num));
                        break;
                    }
                    case BI_ASIN: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("asin");
                        if (v.as.num < -1 || v.as.num > 1) cl_die("asin: argument out of [-1, 1]");
                        stack[sp - 1] = val_num(asin(v.as.num));
                        break;
                    }
                    case BI_ACOS: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("acos");
                        if (v.as.num < -1 || v.as.num > 1) cl_die("acos: argument out of [-1, 1]");
                        stack[sp - 1] = val_num(acos(v.as.num));
                        break;
                    }
                    case BI_ATAN: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("atan");
                        stack[sp - 1] = val_num(atan(v.as.num));
                        break;
                    }
                    case BI_ATAN2: {
                        if (sp < 2) cl_die("stack underflow");
                        Value y = stack[sp - 2], x = stack[sp - 1];
                        if (y.tag != VAL_NUM || x.tag != VAL_NUM) type_error("atan2");
                        stack[sp - 2] = val_num(atan2(y.as.num, x.as.num));
                        sp--;
                        break;
                    }
                    case BI_EXP: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("exp");
                        stack[sp - 1] = val_num(exp(v.as.num));
                        break;
                    }
                    case BI_LOG: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("log");
                        if (v.as.num <= 0) cl_die("log: argument must be > 0");
                        stack[sp - 1] = val_num(log(v.as.num));
                        break;
                    }
                    case BI_LOG10: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_NUM) type_error("log10");
                        if (v.as.num <= 0) cl_die("log10: argument must be > 0");
                        stack[sp - 1] = val_num(log10(v.as.num));
                        break;
                    }
                    case BI_RANDOM:
                        if (sp >= CL_MAX_CODE) cl_die("stack overflow");
                        stack[sp++] = val_num((double)rand() / ((double)RAND_MAX + 1.0));
                        break;
                    case BI_PI:
                        if (sp >= CL_MAX_CODE) cl_die("stack overflow");
                        stack[sp++] = val_num(3.141592653589793);
                        break;
                    case BI_E:
                        if (sp >= CL_MAX_CODE) cl_die("stack overflow");
                        stack[sp++] = val_num(2.718281828459045);
                        break;

                    /* ----- strings ----- */
                    case BI_STR_AT: {
                        if (sp < 2) cl_die("stack underflow");
                        Value sv = stack[sp - 2], iv = stack[sp - 1];
                        if (sv.tag != VAL_STR || iv.tag != VAL_NUM) type_error("str_at");
                        size_t l = strlen(sv.as.str);
                        int i = (int)iv.as.num;
                        if (i < 0 || (size_t)i >= l) cl_die("str_at: index out of range");
                        char *buf = (char *)cl_track_malloc(2);
                        buf[0] = sv.as.str[i];
                        buf[1] = '\0';
                        stack[sp - 2] = val_str(buf);
                        sp--;
                        break;
                    }
                    case BI_STR_SLICE: {
                        if (sp < 3) cl_die("stack underflow");
                        Value sv  = stack[sp - 3];
                        Value siv = stack[sp - 2];
                        Value eiv = stack[sp - 1];
                        if (sv.tag != VAL_STR || siv.tag != VAL_NUM || eiv.tag != VAL_NUM) type_error("str_slice");
                        size_t l = strlen(sv.as.str);
                        int s = (int)siv.as.num;
                        int e = (int)eiv.as.num;
                        if (s < 0 || e < s || (size_t)e > l) cl_die("str_slice: range out of bounds");
                        size_t out_len = (size_t)(e - s);
                        char *buf = (char *)cl_track_malloc(out_len + 1);
                        memcpy(buf, sv.as.str + s, out_len);
                        buf[out_len] = '\0';
                        stack[sp - 3] = val_str(buf);
                        sp -= 2;
                        break;
                    }
                    case BI_STR_FIND: {
                        if (sp < 2) cl_die("stack underflow");
                        Value sv = stack[sp - 2], pv = stack[sp - 1];
                        if (sv.tag != VAL_STR || pv.tag != VAL_STR) type_error("str_find");
                        const char *found = strstr(sv.as.str, pv.as.str);
                        stack[sp - 2] = val_num(found ? (double)(found - sv.as.str) : -1.0);
                        sp--;
                        break;
                    }
                    case BI_STR_UPPER: {
                        if (sp < 1) cl_die("stack underflow");
                        Value sv = stack[sp - 1];
                        if (sv.tag != VAL_STR) type_error("str_upper");
                        size_t l = strlen(sv.as.str);
                        char *buf = (char *)cl_track_malloc(l + 1);
                        for (size_t i = 0; i < l; i++) buf[i] = (char)toupper((unsigned char)sv.as.str[i]);
                        buf[l] = '\0';
                        stack[sp - 1] = val_str(buf);
                        break;
                    }
                    case BI_STR_LOWER: {
                        if (sp < 1) cl_die("stack underflow");
                        Value sv = stack[sp - 1];
                        if (sv.tag != VAL_STR) type_error("str_lower");
                        size_t l = strlen(sv.as.str);
                        char *buf = (char *)cl_track_malloc(l + 1);
                        for (size_t i = 0; i < l; i++) buf[i] = (char)tolower((unsigned char)sv.as.str[i]);
                        buf[l] = '\0';
                        stack[sp - 1] = val_str(buf);
                        break;
                    }
                    case BI_STR_TRIM: {
                        if (sp < 1) cl_die("stack underflow");
                        Value sv = stack[sp - 1];
                        if (sv.tag != VAL_STR) type_error("str_trim");
                        const char *s = sv.as.str;
                        while (isspace((unsigned char)*s)) s++;
                        const char *e = s + strlen(s);
                        while (e > s && isspace((unsigned char)e[-1])) e--;
                        size_t l = (size_t)(e - s);
                        char *buf = (char *)cl_track_malloc(l + 1);
                        memcpy(buf, s, l);
                        buf[l] = '\0';
                        stack[sp - 1] = val_str(buf);
                        break;
                    }
                    case BI_STR_REPEAT: {
                        if (sp < 2) cl_die("stack underflow");
                        Value sv = stack[sp - 2], nv = stack[sp - 1];
                        if (sv.tag != VAL_STR || nv.tag != VAL_NUM) type_error("str_repeat");
                        int n = (int)nv.as.num;
                        if (n < 0) cl_die("str_repeat: negative count");
                        size_t l = strlen(sv.as.str);
                        size_t total = l * (size_t)n;
                        char *buf = (char *)cl_track_malloc(total + 1);
                        for (int i = 0; i < n; i++) memcpy(buf + (size_t)i * l, sv.as.str, l);
                        buf[total] = '\0';
                        stack[sp - 2] = val_str(buf);
                        sp--;
                        break;
                    }
                    case BI_STR_STARTS_WITH: {
                        if (sp < 2) cl_die("stack underflow");
                        Value sv = stack[sp - 2], pv = stack[sp - 1];
                        if (sv.tag != VAL_STR || pv.tag != VAL_STR) type_error("str_starts_with");
                        size_t sl = strlen(sv.as.str), pl = strlen(pv.as.str);
                        int r = pl <= sl && memcmp(sv.as.str, pv.as.str, pl) == 0;
                        stack[sp - 2] = val_num(r ? 1.0 : 0.0);
                        sp--;
                        break;
                    }
                    case BI_STR_ENDS_WITH: {
                        if (sp < 2) cl_die("stack underflow");
                        Value sv = stack[sp - 2], pv = stack[sp - 1];
                        if (sv.tag != VAL_STR || pv.tag != VAL_STR) type_error("str_ends_with");
                        size_t sl = strlen(sv.as.str), pl = strlen(pv.as.str);
                        int r = pl <= sl && memcmp(sv.as.str + sl - pl, pv.as.str, pl) == 0;
                        stack[sp - 2] = val_num(r ? 1.0 : 0.0);
                        sp--;
                        break;
                    }
                    case BI_STR_SPLIT: {
                        if (sp < 2) cl_die("stack underflow");
                        Value sv = stack[sp - 2], pv = stack[sp - 1];
                        if (sv.tag != VAL_STR || pv.tag != VAL_STR) type_error("str_split");
                        const char *s   = sv.as.str;
                        const char *sep = pv.as.str;
                        size_t sep_len = strlen(sep);
                        if (sep_len == 0) cl_die("str_split: empty separator");

                        Array *arr = (Array *)cl_track_malloc(sizeof(Array));
                        arr->items = NULL;
                        arr->count = 0;
                        arr->cap   = 0;
                        const char *p = s;
                        for (;;) {
                            const char *nxt = strstr(p, sep);
                            const char *end = nxt ? nxt : p + strlen(p);
                            size_t part_len = (size_t)(end - p);
                            char *buf = (char *)cl_track_malloc(part_len + 1);
                            memcpy(buf, p, part_len);
                            buf[part_len] = '\0';
                            if (arr->count >= arr->cap) {
                                int nc = arr->cap == 0 ? 4 : arr->cap * 2;
                                arr->items = (Value *)cl_track_realloc(arr->items, sizeof(Value) * (size_t)nc);
                                arr->cap = nc;
                            }
                            arr->items[arr->count++] = val_str(buf);
                            if (!nxt) break;
                            p = nxt + sep_len;
                        }
                        stack[sp - 2] = val_arr(arr);
                        sp--;
                        break;
                    }
                    case BI_STR_JOIN: {
                        if (sp < 2) cl_die("stack underflow");
                        Value av = stack[sp - 2], sv = stack[sp - 1];
                        if (av.tag != VAL_ARR || sv.tag != VAL_STR) type_error("str_join");
                        Array *arr = av.as.arr;
                        if (!arr || arr->count == 0) {
                            char *buf = (char *)cl_track_malloc(1);
                            buf[0] = '\0';
                            stack[sp - 2] = val_str(buf);
                            sp--;
                            break;
                        }
                        size_t sep_len = strlen(sv.as.str);
                        size_t total = 0;
                        for (int i = 0; i < arr->count; i++) {
                            if (arr->items[i].tag != VAL_STR)
                                cl_die("str_join: array elements must be strings");
                            total += strlen(arr->items[i].as.str);
                        }
                        total += sep_len * (size_t)(arr->count - 1);
                        char *buf = (char *)cl_track_malloc(total + 1);
                        char *p = buf;
                        for (int i = 0; i < arr->count; i++) {
                            if (i > 0) { memcpy(p, sv.as.str, sep_len); p += sep_len; }
                            size_t l = strlen(arr->items[i].as.str);
                            memcpy(p, arr->items[i].as.str, l);
                            p += l;
                        }
                        *p = '\0';
                        stack[sp - 2] = val_str(buf);
                        sp--;
                        break;
                    }

                    /* ----- arrays ----- */
                    case BI_POP: {
                        if (sp < 1) cl_die("stack underflow");
                        Value av = stack[sp - 1];
                        if (av.tag != VAL_ARR || !av.as.arr) type_error("pop");
                        Array *arr = av.as.arr;
                        if (arr->count == 0) cl_die("pop: empty array");
                        stack[sp - 1] = arr->items[--arr->count];
                        break;
                    }
                    case BI_ARRAY_REVERSE: {
                        if (sp < 1) cl_die("stack underflow");
                        Value av = stack[sp - 1];
                        if (av.tag != VAL_ARR || !av.as.arr) type_error("array_reverse");
                        Array *arr = av.as.arr;
                        for (int i = 0, j = arr->count - 1; i < j; i++, j--) {
                            Value tmp = arr->items[i];
                            arr->items[i] = arr->items[j];
                            arr->items[j] = tmp;
                        }
                        /* result is the same array reference */
                        break;
                    }
                    case BI_ARRAY_SORT: {
                        if (sp < 1) cl_die("stack underflow");
                        Value av = stack[sp - 1];
                        if (av.tag != VAL_ARR || !av.as.arr) type_error("array_sort");
                        Array *arr = av.as.arr;
                        if (arr->count > 1) {
                            ValueTag t = arr->items[0].tag;
                            for (int i = 1; i < arr->count; i++) {
                                if (arr->items[i].tag != t) cl_die("array_sort: mixed types");
                            }
                            if      (t == VAL_NUM) qsort(arr->items, (size_t)arr->count, sizeof(Value), sort_cmp_num);
                            else if (t == VAL_STR) qsort(arr->items, (size_t)arr->count, sizeof(Value), sort_cmp_str);
                            else cl_die("array_sort: unsupported element type");
                        }
                        break;
                    }
                    case BI_ARRAY_CONCAT: {
                        if (sp < 2) cl_die("stack underflow");
                        Value av = stack[sp - 2], bv = stack[sp - 1];
                        if (av.tag != VAL_ARR || bv.tag != VAL_ARR) type_error("array_concat");
                        Array *a = av.as.arr;
                        Array *b = bv.as.arr;
                        int total = (a ? a->count : 0) + (b ? b->count : 0);
                        Array *arr = (Array *)cl_track_malloc(sizeof(Array));
                        arr->count = total;
                        arr->cap   = total;
                        arr->items = total > 0 ? (Value *)cl_track_malloc(sizeof(Value) * (size_t)total) : NULL;
                        int k = 0;
                        if (a) for (int i = 0; i < a->count; i++) arr->items[k++] = a->items[i];
                        if (b) for (int i = 0; i < b->count; i++) arr->items[k++] = b->items[i];
                        stack[sp - 2] = val_arr(arr);
                        sp--;
                        break;
                    }
                    case BI_ARRAY_SLICE: {
                        if (sp < 3) cl_die("stack underflow");
                        Value av  = stack[sp - 3];
                        Value siv = stack[sp - 2];
                        Value eiv = stack[sp - 1];
                        if (av.tag != VAL_ARR || siv.tag != VAL_NUM || eiv.tag != VAL_NUM) type_error("array_slice");
                        Array *src = av.as.arr;
                        int count = src ? src->count : 0;
                        int s = (int)siv.as.num;
                        int e = (int)eiv.as.num;
                        if (s < 0 || e < s || e > count) cl_die("array_slice: range out of bounds");
                        int out_count = e - s;
                        Array *arr = (Array *)cl_track_malloc(sizeof(Array));
                        arr->count = out_count;
                        arr->cap   = out_count;
                        arr->items = out_count > 0 ? (Value *)cl_track_malloc(sizeof(Value) * (size_t)out_count) : NULL;
                        for (int i = 0; i < out_count; i++) arr->items[i] = src->items[s + i];
                        stack[sp - 3] = val_arr(arr);
                        sp -= 2;
                        break;
                    }
                    case BI_ARRAY_FIND: {
                        if (sp < 2) cl_die("stack underflow");
                        Value av = stack[sp - 2], v = stack[sp - 1];
                        if (av.tag != VAL_ARR) type_error("array_find");
                        Array *arr = av.as.arr;
                        int idx = -1;
                        if (arr) for (int i = 0; i < arr->count; i++) {
                            if (val_equal(arr->items[i], v)) { idx = i; break; }
                        }
                        stack[sp - 2] = val_num((double)idx);
                        sp--;
                        break;
                    }
                    case BI_ARRAY_CONTAINS: {
                        if (sp < 2) cl_die("stack underflow");
                        Value av = stack[sp - 2], v = stack[sp - 1];
                        if (av.tag != VAL_ARR) type_error("array_contains");
                        Array *arr = av.as.arr;
                        int found = 0;
                        if (arr) for (int i = 0; i < arr->count; i++) {
                            if (val_equal(arr->items[i], v)) { found = 1; break; }
                        }
                        stack[sp - 2] = val_num(found ? 1.0 : 0.0);
                        sp--;
                        break;
                    }
                    case BI_ARRAY_RANGE: {
                        if (sp < 2) cl_die("stack underflow");
                        Value sv = stack[sp - 2], ev = stack[sp - 1];
                        if (sv.tag != VAL_NUM || ev.tag != VAL_NUM) type_error("array_range");
                        int s = (int)sv.as.num;
                        int e = (int)ev.as.num;
                        int count = e - s;
                        if (count < 0) count = 0;
                        Array *arr = (Array *)cl_track_malloc(sizeof(Array));
                        arr->count = count;
                        arr->cap   = count;
                        arr->items = count > 0 ? (Value *)cl_track_malloc(sizeof(Value) * (size_t)count) : NULL;
                        for (int i = 0; i < count; i++) arr->items[i] = val_num((double)(s + i));
                        stack[sp - 2] = val_arr(arr);
                        sp--;
                        break;
                    }

                    /* ----- misc ----- */
                    case BI_WRITE: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag == VAL_NUM)      printf("%.10g", v.as.num);
                        else if (v.tag == VAL_STR) printf("%s", v.as.str ? v.as.str : "");
                        else type_error("write");
                        /* Builtins must leave exactly one Value on the
                           stack; write returns 0 (rarely used). */
                        stack[sp - 1] = val_num(0);
                        break;
                    }
                    case BI_TYPE_OF: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        const char *t = "?";
                        if      (v.tag == VAL_NUM) t = "num";
                        else if (v.tag == VAL_STR) t = "str";
                        else if (v.tag == VAL_ARR) t = "arr";
                        else if (v.tag == VAL_FN)  t = "fn";
                        else if (v.tag == VAL_MAP) t = "map";
                        /* Literal strings live for the whole program
                           and never need to be tracked. */
                        stack[sp - 1] = val_str(t);
                        break;
                    }

                    /* ----- maps ----- */
                    case BI_KEYS: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_MAP || !v.as.map) type_error("keys");
                        Map *m = v.as.map;
                        Array *arr = (Array *)cl_track_malloc(sizeof(Array));
                        arr->count = m->count;
                        arr->cap   = m->count;
                        arr->items = m->count > 0
                                     ? (Value *)cl_track_malloc(sizeof(Value) * (size_t)m->count)
                                     : NULL;
                        for (int i = 0; i < m->count; i++) arr->items[i] = m->entries[i].key;
                        stack[sp - 1] = val_arr(arr);
                        break;
                    }
                    case BI_VALUES: {
                        if (sp < 1) cl_die("stack underflow");
                        Value v = stack[sp - 1];
                        if (v.tag != VAL_MAP || !v.as.map) type_error("values");
                        Map *m = v.as.map;
                        Array *arr = (Array *)cl_track_malloc(sizeof(Array));
                        arr->count = m->count;
                        arr->cap   = m->count;
                        arr->items = m->count > 0
                                     ? (Value *)cl_track_malloc(sizeof(Value) * (size_t)m->count)
                                     : NULL;
                        for (int i = 0; i < m->count; i++) arr->items[i] = m->entries[i].value;
                        stack[sp - 1] = val_arr(arr);
                        break;
                    }
                    case BI_HAS_KEY: {
                        if (sp < 2) cl_die("stack underflow");
                        Value mv = stack[sp - 2], kv = stack[sp - 1];
                        if (mv.tag != VAL_MAP) type_error("has_key");
                        int found = map_find(mv.as.map, kv) >= 0;
                        stack[sp - 2] = val_num(found ? 1.0 : 0.0);
                        sp--;
                        break;
                    }
                    case BI_DEL: {
                        if (sp < 2) cl_die("stack underflow");
                        Value mv = stack[sp - 2], kv = stack[sp - 1];
                        if (mv.tag != VAL_MAP || !mv.as.map) type_error("del");
                        Map *m = mv.as.map;
                        int idx = map_find(m, kv);
                        if (idx < 0) {
                            stack[sp - 2] = val_num(0);
                        } else {
                            /* Shift the rest down one slot. */
                            for (int i = idx; i < m->count - 1; i++) {
                                m->entries[i] = m->entries[i + 1];
                            }
                            m->count--;
                            stack[sp - 2] = val_num(1);
                        }
                        sp--;
                        break;
                    }

                    case BI_IS_DIGIT:
                    case BI_IS_ALPHA:
                    case BI_IS_ALNUM:
                    case BI_IS_SPACE:
                    case BI_IS_UPPER:
                    case BI_IS_LOWER: {
                        if (sp < 1) cl_die("stack underflow");
                        Value sv = stack[sp - 1];
                        if (sv.tag != VAL_STR || !sv.as.str || strlen(sv.as.str) != 1) {
                            type_error("ctype check (expects 1-char string)");
                        }
                        unsigned char c = (unsigned char)sv.as.str[0];
                        int r = 0;
                        switch (in.iarg) {
                            case BI_IS_DIGIT: r = (c >= '0' && c <= '9'); break;
                            case BI_IS_ALPHA: r = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); break;
                            case BI_IS_ALNUM: r = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); break;
                            case BI_IS_SPACE: r = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'); break;
                            case BI_IS_UPPER: r = (c >= 'A' && c <= 'Z'); break;
                            case BI_IS_LOWER: r = (c >= 'a' && c <= 'z'); break;
                        }
                        stack[sp - 1] = val_num(r ? 1.0 : 0.0);
                        break;
                    }
                    case BI_CHAR_TO_UPPER:
                    case BI_CHAR_TO_LOWER: {
                        if (sp < 1) cl_die("stack underflow");
                        Value sv = stack[sp - 1];
                        if (sv.tag != VAL_STR || !sv.as.str || strlen(sv.as.str) != 1) {
                            type_error("char_to_upper/lower (expects 1-char string)");
                        }
                        char c = sv.as.str[0];
                        if (in.iarg == BI_CHAR_TO_UPPER) {
                            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
                        } else {
                            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                        }
                        char *out = (char *)cl_track_malloc(2);
                        out[0] = c; out[1] = '\0';
                        stack[sp - 1] = val_str(out);
                        break;
                    }
                    case BI_CHAR_CODE: {
                        if (sp < 1) cl_die("stack underflow");
                        Value sv = stack[sp - 1];
                        if (sv.tag != VAL_STR || !sv.as.str || strlen(sv.as.str) < 1) {
                            type_error("char_code (expects non-empty string)");
                        }
                        stack[sp - 1] = val_num((double)(unsigned char)sv.as.str[0]);
                        break;
                    }
                    case BI_CHAR_FROM: {
                        if (sp < 1) cl_die("stack underflow");
                        Value cv = stack[sp - 1];
                        if (cv.tag != VAL_NUM) type_error("char_from (expects num)");
                        int code = (int)cv.as.num;
                        if (code < 0 || code > 255) cl_die("char_from: code out of byte range");
                        char *out = (char *)cl_track_malloc(2);
                        out[0] = (char)code; out[1] = '\0';
                        stack[sp - 1] = val_str(out);
                        break;
                    }
                    case BI_FMT: {
                        /* fmt(format: str, args: arr) -> str. Implemented
                           via the shared cl_fmt helper to keep VM and
                           native in sync. */
                        if (sp < 2) cl_die("stack underflow");
                        Value fv = stack[sp - 2];
                        Value av = stack[sp - 1];
                        if (fv.tag != VAL_STR) type_error("fmt (format must be string)");
                        if (av.tag != VAL_ARR) type_error("fmt (args must be array)");
                        char *out = cl_vm_format(fv.as.str, av.as.arr);
                        stack[sp - 2] = val_str(out);
                        sp--;
                        break;
                    }

                    default:
                        cl_die("unknown calclib builtin");
                }
                break;
            case OP_NEW_ARRAY: {
                int count = in.iarg;
                if (count < 0) cl_die("NEW_ARRAY with negative count");
                if (sp < count) cl_die("stack underflow on NEW_ARRAY");
                Array *arr  = (Array *)cl_track_malloc(sizeof(Array));
                arr->count  = count;
                arr->cap    = count;
                arr->items  = count > 0
                              ? (Value *)cl_track_malloc(sizeof(Value) * (size_t)count)
                              : NULL;
                for (int i = 0; i < count; i++) {
                    arr->items[i] = stack[sp - count + i];
                }
                sp -= count;
                if (sp >= CL_MAX_CODE) cl_die("stack overflow");
                stack[sp++] = val_arr(arr);
                break;
            }
            case OP_TYPECHECK: {
                int offset   = in.iarg;
                int expected = (int)in.farg;
                int idx = fp + offset;
                if (idx < 0 || idx >= sp) cl_die("TYPECHECK out of frame");
                Value v = stack[idx];
                if (!typecheck_accepts(expected, v.tag)) {
                    fprintf(stderr,
                        "runtime type error: parameter at offset %d expected %s, got %s\n",
                        offset, vm_type_name(expected), value_tag_name(v.tag));
                    exit(1);
                }
                break;
            }
            case OP_TYPECHECK_TOP: {
                int expected = (int)in.farg;
                if (sp <= 0) cl_die("TYPECHECK_TOP on empty stack");
                Value v = stack[sp - 1];
                if (!typecheck_accepts(expected, v.tag)) {
                    fprintf(stderr,
                        "runtime type error: value being bound expected %s, got %s\n",
                        vm_type_name(expected), value_tag_name(v.tag));
                    exit(1);
                }
                break;
            }
            case OP_NEW_MAP: {
                int count = in.iarg;
                if (count < 0) cl_die("NEW_MAP with negative count");
                if (sp < 2 * count) cl_die("stack underflow on NEW_MAP");
                Map *m  = (Map *)cl_track_malloc(sizeof(Map));
                m->count   = 0;
                m->cap     = count;
                m->entries = count > 0
                             ? (MapEntry *)cl_track_malloc(sizeof(MapEntry) * (size_t)count)
                             : NULL;
                int base = sp - 2 * count;
                /* Insert pairs in order. A duplicate key overwrites the
                   earlier value (later-wins, like a desugaring of two
                   sequential `m[k] = v;` assignments). */
                for (int i = 0; i < count; i++) {
                    Value k = stack[base + 2 * i];
                    Value v = stack[base + 2 * i + 1];
                    if (k.tag != VAL_NUM && k.tag != VAL_STR)
                        cl_die("map literal: keys must be numbers or strings");
                    int prev = map_find(m, k);
                    if (prev >= 0) {
                        m->entries[prev].value = v;
                    } else {
                        m->entries[m->count].key   = k;
                        m->entries[m->count].value = v;
                        m->count++;
                    }
                }
                sp -= 2 * count;
                if (sp >= CL_MAX_CODE) cl_die("stack overflow");
                stack[sp++] = val_map(m);
                break;
            }
            case OP_INDEX_GET: {
                if (sp < 2) cl_die("stack underflow");
                Value idxv = stack[sp - 1];
                Value tgt  = stack[sp - 2];
                if (tgt.tag == VAL_ARR) {
                    if (idxv.tag != VAL_NUM) cl_die("array index must be a number");
                    int i = (int)idxv.as.num;
                    if (!tgt.as.arr) cl_die("indexed value is not an array");
                    if (i < 0 || i >= tgt.as.arr->count) cl_die("array index out of range");
                    stack[sp - 2] = tgt.as.arr->items[i];
                } else if (tgt.tag == VAL_MAP) {
                    if (idxv.tag != VAL_NUM && idxv.tag != VAL_STR)
                        cl_die("map key must be a number or string");
                    int i = map_find(tgt.as.map, idxv);
                    if (i < 0) cl_die("map: key not found");
                    stack[sp - 2] = tgt.as.map->entries[i].value;
                } else {
                    cl_die("indexed value is not an array or map");
                }
                sp--;
                break;
            }
            case OP_INDEX_SET: {
                if (sp < 3) cl_die("stack underflow");
                Value valv = stack[sp - 1];
                Value idxv = stack[sp - 2];
                Value tgt  = stack[sp - 3];
                if (tgt.tag == VAL_ARR) {
                    if (idxv.tag != VAL_NUM) cl_die("array index must be a number");
                    int i = (int)idxv.as.num;
                    if (!tgt.as.arr) cl_die("indexed value is not an array");
                    if (i < 0 || i >= tgt.as.arr->count) cl_die("array index out of range");
                    tgt.as.arr->items[i] = valv;
                } else if (tgt.tag == VAL_MAP) {
                    if (idxv.tag != VAL_NUM && idxv.tag != VAL_STR)
                        cl_die("map key must be a number or string");
                    Map *m = tgt.as.map;
                    if (!m) cl_die("map is null");
                    int i = map_find(m, idxv);
                    if (i >= 0) {
                        m->entries[i].value = valv;
                    } else {
                        if (m->count >= m->cap) {
                            int nc = m->cap == 0 ? 4 : m->cap * 2;
                            m->entries = (MapEntry *)cl_track_realloc(
                                m->entries, sizeof(MapEntry) * (size_t)nc);
                            m->cap = nc;
                        }
                        m->entries[m->count].key   = idxv;
                        m->entries[m->count].value = valv;
                        m->count++;
                    }
                } else {
                    cl_die("indexed value is not an array or map");
                }
                sp -= 3;
                break;
            }
            case OP_DROP:
                if (sp <= 0) cl_die("stack underflow");
                sp--;
                break;
            case OP_DUP2:
                if (sp < 2) cl_die("stack underflow on DUP2");
                if (sp + 2 > CL_MAX_CODE) cl_die("stack overflow on DUP2");
                stack[sp]     = stack[sp - 2];
                stack[sp + 1] = stack[sp - 1];
                sp += 2;
                break;
            case OP_PUSH_FN:
                /* Linker has already resolved iarg to the function's
                   entry PC. We just wrap it in a VAL_FN. */
                if (sp >= CL_MAX_CODE) cl_die("stack overflow");
                if (in.iarg < 0 || in.iarg >= code_count) cl_die("PUSH_FN target out of range");
                stack[sp++] = val_fn(in.iarg);
                break;
            case OP_CALL_VAL: {
                /* Indirect call: function value sits on top, n args
                   underneath. Pop the function, save state, set the
                   current closure so LOAD_UPVAL inside the callee
                   reads from the right captured array. */
                if (sp <= 0) cl_die("stack underflow");
                Value f = stack[--sp];
                if (f.tag != VAL_FN || !f.as.fn) cl_die("CALL_VAL: target is not a function value");
                int n_args = in.iarg;
                if (n_args < 0 || sp < n_args) cl_die("CALL_VAL: bad arg count");
                if (csp >= CALL_STACK_MAX) cl_die("call stack overflow");
                if (f.as.fn->pc < 0 || f.as.fn->pc >= code_count) cl_die("CALL_VAL target out of range");
                call_stack[csp].ret_pc        = pc;
                call_stack[csp].saved_fp      = fp;
                call_stack[csp].saved_closure = current_closure;
                csp++;
                current_closure = f.as.fn;
                fp = sp - n_args;
                pc = f.as.fn->pc;
                break;
            }
            case OP_MAKE_CLOSURE: {
                int n_upvals = (int)in.farg;
                if (in.iarg < 0 || in.iarg >= code_count) cl_die("MAKE_CLOSURE target out of range");
                if (n_upvals < 0) cl_die("MAKE_CLOSURE with negative upvalue count");
                if (sp < n_upvals) cl_die("MAKE_CLOSURE: stack underflow");
                Value cap_buf[64];
                if (n_upvals > 64) cl_die("MAKE_CLOSURE: too many upvalues");
                for (int i = 0; i < n_upvals; i++) {
                    cap_buf[i] = stack[sp - n_upvals + i];
                }
                sp -= n_upvals;
                if (sp >= CL_MAX_CODE) cl_die("stack overflow on MAKE_CLOSURE");
                stack[sp++] = val_closure(in.iarg, cap_buf, n_upvals);
                break;
            }
            case OP_LOAD_UPVAL: {
                if (!current_closure) cl_die("LOAD_UPVAL with no active closure context");
                int idx = in.iarg;
                if (idx < 0 || idx >= current_closure->n_upvals) cl_die("LOAD_UPVAL index out of range");
                if (sp >= CL_MAX_CODE) cl_die("stack overflow");
                stack[sp++] = current_closure->upvals[idx];
                break;
            }
            case OP_HALT:
                goto done;
            default:
                cl_die("unknown opcode");
        }
    }
done:
    cl_track_free(call_stack);
    cl_track_free(stack);
    cl_track_free(mem);
    cl_track_free(code);
    if (strings) {
        for (int i = 0; i < string_count; i++) cl_track_free(strings[i]);
        cl_track_free(strings);
    }
    return rc;
}
