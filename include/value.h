#ifndef CALCLANG_VALUE_H
#define CALCLANG_VALUE_H
#include "common.h"

/* Runtime value seen by the VM. Variables and stack slots hold one of:
   - a number,
   - an immutable string (pointer into the program's strings pool),
   - a mutable array (reference),
   - a function value (reference to a Closure: entry PC + captured
     upvalues; closures with zero upvalues are just function pointers),
   - a mutable map (reference, string-or-number keys).
   Strings, arrays, function refs, and maps all use reference semantics. */

typedef enum {
    VAL_NUM = 0,
    VAL_STR = 1,
    VAL_ARR = 2,
    VAL_FN  = 3,
    VAL_MAP = 4
} ValueTag;

struct Array;
struct Map;
struct Closure;

typedef struct {
    ValueTag tag;
    union {
        double          num;
        const char     *str;
        struct Array   *arr;
        struct Closure *fn;
        struct Map     *map;
    } as;
} Value;

typedef struct Array {
    Value *items;
    int    count;
    int    cap;
} Array;

typedef struct {
    Value key;
    Value value;
} MapEntry;

typedef struct Map {
    MapEntry *entries;
    int       count;
    int       cap;
} Map;

typedef struct Closure {
    int    pc;          /* entry instruction in the linked code   */
    int    n_upvals;    /* number of captured values              */
    Value *upvals;      /* heap-allocated, owned by this closure  */
} Closure;

static inline Value val_num(double n) {
    Value v; v.tag = VAL_NUM; v.as.num = n; return v;
}
static inline Value val_str(const char *s) {
    Value v; v.tag = VAL_STR; v.as.str = s; return v;
}
static inline Value val_arr(Array *a) {
    Value v; v.tag = VAL_ARR; v.as.arr = a; return v;
}
static inline Value val_map(Map *m) {
    Value v; v.tag = VAL_MAP; v.as.map = m; return v;
}

/* val_fn(pc) allocates a fresh Closure with no captures. Tracked
   so the atexit cleanup reaps it; the cost is one small heap block
   per OP_PUSH_FN. Hot loops that repeatedly push the same function
   value pay this cost but the language has no hot loop pattern that
   actually does so. */
static inline Value val_fn(int pc) {
    Closure *c = (Closure *)cl_track_malloc(sizeof(Closure));
    c->pc = pc;
    c->n_upvals = 0;
    c->upvals = NULL;
    Value v;
    v.tag = VAL_FN;
    v.as.fn = c;
    return v;
}

/* val_closure(pc, upvals, n) — copies the upvalue array onto the heap
   and bundles it with the entry PC. Used by OP_MAKE_CLOSURE. */
static inline Value val_closure(int pc, const Value *upvals, int n) {
    Closure *c = (Closure *)cl_track_malloc(sizeof(Closure));
    c->pc = pc;
    c->n_upvals = n;
    if (n > 0) {
        c->upvals = (Value *)cl_track_malloc(sizeof(Value) * (size_t)n);
        for (int i = 0; i < n; i++) c->upvals[i] = upvals[i];
    } else {
        c->upvals = NULL;
    }
    Value v;
    v.tag = VAL_FN;
    v.as.fn = c;
    return v;
}

static inline int val_truthy(Value v) {
    if (v.tag == VAL_NUM) return v.as.num != 0;
    if (v.tag == VAL_STR) return v.as.str && v.as.str[0] != '\0';
    if (v.tag == VAL_ARR) return v.as.arr && v.as.arr->count > 0;
    if (v.tag == VAL_FN)  return v.as.fn != NULL;
    if (v.tag == VAL_MAP) return v.as.map && v.as.map->count > 0;
    return 0;
}

static inline int val_equal(Value a, Value b) {
    if (a.tag != b.tag) return 0;
    if (a.tag == VAL_NUM) return a.as.num == b.as.num;
    if (a.tag == VAL_STR) return strcmp(a.as.str, b.as.str) == 0;
    /* Arrays and maps compare by reference identity (typical of
       mutable containers). Function values compare by their entry
       PC alone — two references to the same function are equal even
       though each PUSH_FN allocates a fresh Closure header. Closures
       with captures are not distinguished from one another at this
       level; callers that care about identity should compare against
       a stored reference. */
    if (a.tag == VAL_ARR) return a.as.arr == b.as.arr;
    if (a.tag == VAL_FN) {
        if (a.as.fn == b.as.fn) return 1;
        if (!a.as.fn || !b.as.fn) return 0;
        return a.as.fn->pc == b.as.fn->pc
            && a.as.fn->n_upvals == 0
            && b.as.fn->n_upvals == 0;
    }
    if (a.tag == VAL_MAP) return a.as.map == b.as.map;
    return 0;
}

#endif
