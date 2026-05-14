#ifndef CALCLANG_CALCLIB_H
#define CALCLANG_CALCLIB_H

/* calclib: the set of functions baked into the VM. They are called
   from CalcLang source using bare names (`sqrt(x)`, `len(s)`,
   `str_split(s, ",")`, ...) and lowered by the code generator to a
   single `BUILTIN <id>` opcode that the VM dispatches on. */

typedef enum {
    /* original ten */
    BI_SQRT      = 0,
    BI_FLOOR     = 1,
    BI_CEIL      = 2,
    BI_ABS       = 3,
    BI_POW       = 4,
    BI_TO_STR    = 5,
    BI_TO_NUM    = 6,
    BI_LEN       = 7,
    BI_READ_LINE = 8,
    BI_PUSH      = 9,

    /* additional math */
    BI_MIN       = 10,
    BI_MAX       = 11,
    BI_INT       = 12,
    BI_ROUND     = 13,
    BI_SIN       = 14,
    BI_COS       = 15,
    BI_TAN       = 16,
    BI_ASIN      = 17,
    BI_ACOS      = 18,
    BI_ATAN      = 19,
    BI_ATAN2     = 20,
    BI_EXP       = 21,
    BI_LOG       = 22,
    BI_LOG10     = 23,
    BI_RANDOM    = 24,
    BI_PI        = 25,
    BI_E         = 26,

    /* strings */
    BI_STR_AT          = 27,
    BI_STR_SLICE       = 28,
    BI_STR_FIND        = 29,
    BI_STR_UPPER       = 30,
    BI_STR_LOWER       = 31,
    BI_STR_TRIM        = 32,
    BI_STR_REPEAT      = 33,
    BI_STR_STARTS_WITH = 34,
    BI_STR_ENDS_WITH   = 35,
    BI_STR_SPLIT       = 36,
    BI_STR_JOIN        = 37,

    /* arrays */
    BI_POP             = 38,
    BI_ARRAY_REVERSE   = 39,
    BI_ARRAY_SORT      = 40,
    BI_ARRAY_CONCAT    = 41,
    BI_ARRAY_SLICE     = 42,
    BI_ARRAY_FIND      = 43,
    BI_ARRAY_CONTAINS  = 44,
    BI_ARRAY_RANGE     = 45,

    /* misc */
    BI_WRITE           = 46,
    BI_TYPE_OF         = 47,

    /* map operations */
    BI_KEYS            = 48,
    BI_VALUES          = 49,
    BI_HAS_KEY         = 50,
    BI_DEL             = 51,

    /* ctype-style character helpers */
    BI_IS_DIGIT        = 52,
    BI_IS_ALPHA        = 53,
    BI_IS_ALNUM        = 54,
    BI_IS_SPACE        = 55,
    BI_IS_UPPER        = 56,
    BI_IS_LOWER        = 57,
    BI_CHAR_TO_UPPER   = 58,
    BI_CHAR_TO_LOWER   = 59,
    BI_CHAR_CODE       = 60,
    BI_CHAR_FROM       = 61,

    /* format strings */
    BI_FMT             = 62
} BuiltinId;

typedef struct {
    const char *name;
    int         arg_count;
    int         id;
    int         return_type;   /* TypeAnnot code, 0 (TYPE_ANY) if unknown */
} BuiltinDef;

/* Returns NULL if `name` is not a known builtin. */
const BuiltinDef *cl_find_builtin(const char *name);

#endif
