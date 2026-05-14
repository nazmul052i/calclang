#include "calclib.h"
#include <string.h>

/* Type codes mirror ast.h's TypeAnnot enum:
   1=num  2=str  3=arr  4=bool  5=map  6=fn  0=any/unknown. */
#define T_NUM 1
#define T_STR 2
#define T_ARR 3
#define T_MAP 5
#define T_ANY 0

static const BuiltinDef table[] = {
    /* original ten */
    { "sqrt",      1, BI_SQRT,      T_NUM },
    { "floor",     1, BI_FLOOR,     T_NUM },
    { "ceil",      1, BI_CEIL,      T_NUM },
    { "abs",       1, BI_ABS,       T_NUM },
    { "pow",       2, BI_POW,       T_NUM },
    { "to_str",    1, BI_TO_STR,    T_STR },
    { "to_num",    1, BI_TO_NUM,    T_NUM },
    { "len",       1, BI_LEN,       T_NUM },
    { "read_line", 0, BI_READ_LINE, T_STR },
    { "push",      2, BI_PUSH,      T_NUM },

    /* additional math */
    { "min",       2, BI_MIN,       T_NUM },
    { "max",       2, BI_MAX,       T_NUM },
    { "int",       1, BI_INT,       T_NUM },
    { "round",     1, BI_ROUND,     T_NUM },
    { "sin",       1, BI_SIN,       T_NUM },
    { "cos",       1, BI_COS,       T_NUM },
    { "tan",       1, BI_TAN,       T_NUM },
    { "asin",      1, BI_ASIN,      T_NUM },
    { "acos",      1, BI_ACOS,      T_NUM },
    { "atan",      1, BI_ATAN,      T_NUM },
    { "atan2",     2, BI_ATAN2,     T_NUM },
    { "exp",       1, BI_EXP,       T_NUM },
    { "log",       1, BI_LOG,       T_NUM },
    { "log10",     1, BI_LOG10,     T_NUM },
    { "random",    0, BI_RANDOM,    T_NUM },
    { "pi",        0, BI_PI,        T_NUM },
    { "e",         0, BI_E,         T_NUM },

    /* strings */
    { "str_at",          2, BI_STR_AT,          T_STR },
    { "str_slice",       3, BI_STR_SLICE,       T_STR },
    { "str_find",        2, BI_STR_FIND,        T_NUM },
    { "str_upper",       1, BI_STR_UPPER,       T_STR },
    { "str_lower",       1, BI_STR_LOWER,       T_STR },
    { "str_trim",        1, BI_STR_TRIM,        T_STR },
    { "str_repeat",      2, BI_STR_REPEAT,      T_STR },
    { "str_starts_with", 2, BI_STR_STARTS_WITH, T_NUM },
    { "str_ends_with",   2, BI_STR_ENDS_WITH,   T_NUM },
    { "str_split",       2, BI_STR_SPLIT,       T_ARR },
    { "str_join",        2, BI_STR_JOIN,        T_STR },

    /* arrays */
    { "pop",             1, BI_POP,             T_ANY },
    { "array_reverse",   1, BI_ARRAY_REVERSE,   T_ARR },
    { "array_sort",      1, BI_ARRAY_SORT,      T_ARR },
    { "array_concat",    2, BI_ARRAY_CONCAT,    T_ARR },
    { "array_slice",     3, BI_ARRAY_SLICE,     T_ARR },
    { "array_find",      2, BI_ARRAY_FIND,      T_NUM },
    { "array_contains",  2, BI_ARRAY_CONTAINS,  T_NUM },
    { "array_range",     2, BI_ARRAY_RANGE,     T_ARR },

    /* misc */
    { "write",           1, BI_WRITE,           T_NUM },
    { "type_of",         1, BI_TYPE_OF,         T_STR },

    /* map operations */
    { "keys",            1, BI_KEYS,            T_ARR },
    { "values",          1, BI_VALUES,          T_ARR },
    { "has_key",         2, BI_HAS_KEY,         T_NUM },
    { "del",             2, BI_DEL,             T_NUM },

    /* ctype helpers — operate on single-character strings */
    { "is_digit",        1, BI_IS_DIGIT,        T_NUM },
    { "is_alpha",        1, BI_IS_ALPHA,        T_NUM },
    { "is_alnum",        1, BI_IS_ALNUM,        T_NUM },
    { "is_space",        1, BI_IS_SPACE,        T_NUM },
    { "is_upper",        1, BI_IS_UPPER,        T_NUM },
    { "is_lower",        1, BI_IS_LOWER,        T_NUM },
    { "char_to_upper",   1, BI_CHAR_TO_UPPER,   T_STR },
    { "char_to_lower",   1, BI_CHAR_TO_LOWER,   T_STR },
    { "char_code",       1, BI_CHAR_CODE,       T_NUM },
    { "char_from",       1, BI_CHAR_FROM,       T_STR },

    /* printf-style formatting: fmt(format, [args]) -> str */
    { "fmt",             2, BI_FMT,             T_STR },

    { NULL, 0, 0, 0 }
};

const BuiltinDef *cl_find_builtin(const char *name) {
    for (const BuiltinDef *b = table; b->name; b++) {
        if (strcmp(b->name, name) == 0) return b;
    }
    return NULL;
}
