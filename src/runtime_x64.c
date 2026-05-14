/*
   Native runtime support library — the C side of the native backend.
   Compiled separately from the user's program and linked alongside the
   generated assembly by gcc.

   See include/runtime.h for the Value model. This file implements:
     - Memory: mark-and-sweep GC with conservative stack scan
     - String allocation + concat + a dozen string calclib functions
     - Polymorphic '+', '==', '!=' dispatch helpers
     - Print
     - to_str / to_num / type_of for the tag-aware subset

   Functions that take a Value of a specific type validate the tag and
   exit with a runtime-error message on mismatch — same semantics as
   the VM's TYPECHECK trap, just expressed as a process exit.

   ---- Garbage collection ----

   Every heap allocation goes through `cl_alloc(size, kind)`, which
   prepends a `GcHdr` and links the allocation into a global list. The
   user-visible pointer points to the payload (one header behind it).

   When the bytes-since-last-GC counter exceeds GC_THRESHOLD,
   `cl_alloc` runs a stop-the-world mark-and-sweep:

     1. Mark phase — walk every 8-byte word on the native stack from
        the current rsp up to the OS-provided stack top. For each
        word, try to interpret it as either a raw heap pointer (top
        16 bits == 0) OR a NaN-boxed tagged Value (top 16 bits in our
        tag range, low 48 bits = payload pointer). If it matches a
        tracked allocation, mark it and descend (arrays mark their
        items buffer + each Value inside; closures mark upvals; etc.).
     2. Sweep phase — walk the list, free anything still unmarked,
        clear the marked bit on survivors.

   The scan is *conservative*: any 8-byte stack word that happens to
   look like a payload pointer is treated as live, which means we may
   retain a few dead objects but never collect a live one. This is
   the right trade-off for an educational runtime.

   IMPORTANT: this assumes the runtime is compiled with -O0 (the
   default for the gcc invocations in the README and test runner).
   At -O0 the compiler keeps every C local + parameter on the stack,
   so conservative scan can see them. Higher optimization levels may
   keep Value arguments only in registers, in which case live Values
   could be collected. Mark the runtime as -O0 in your build to be
   safe.

   The runtime is allocated under malloc and never relocates objects
   — pointers stay stable, so generated assembly that holds a
   payload pointer in a stack slot (a boxed local, an upval) sees it
   survive the sweep as long as something keeps it alive.
*/

#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#if defined(__linux__)
  #include <pthread.h>
  #include <dlfcn.h>
#endif

#ifdef _WIN32
  /* Forward-declared to avoid pulling in all of windows.h. The TEB
     stack-base read is via inline asm; library loading uses these. */
  __declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
  __declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
  __declspec(dllimport) void *__stdcall GetModuleHandleA(const char *);
#endif

/* --- GC infrastructure ------------------------------------------- */

typedef enum {
    GC_STR     = 1,
    GC_ARR     = 2,   /* CalcArr struct itself */
    GC_MAP     = 3,   /* CalcMap struct itself */
    GC_CLOSURE = 4,   /* CalcClosure struct itself */
    GC_BOX     = 5,   /* single Value cell used as a closure upval target */
    GC_RAW     = 6,   /* items / keys / values / upvals raw buffers */
    GC_CPX     = 7    /* CalcCpx struct — no descent needed */
} GcKind;

typedef struct GcHdr {
    struct GcHdr *next;
    uint32_t      size;     /* payload bytes — not including this header */
    uint16_t      kind;
    uint16_t      marked;
} GcHdr;

#define GC_THRESHOLD (256u * 1024u)  /* run GC when this many bytes accumulated */

static GcHdr *g_allocs = NULL;
static size_t g_bytes_since_gc = 0;
static int    g_gc_in_progress = 0;

static void cl_gc(void);

/* Wrap malloc with header + tracking. Triggers GC when the byte
   counter crosses the threshold. */
static void *cl_alloc(size_t size, GcKind kind) {
    /* Force the compiler to spill the most important caller-saved
       Value-bearing registers to this stack frame so the conservative
       scan can see them as live roots. */
    volatile uint64_t spill_r8, spill_r9, spill_xmm0, spill_xmm1;
    __asm__ __volatile__(
        "mov  %%r8,    %0\n\t"
        "mov  %%r9,    %1\n\t"
        "movq %%xmm0,  %2\n\t"
        "movq %%xmm1,  %3"
        : "=m"(spill_r8), "=m"(spill_r9),
          "=m"(spill_xmm0), "=m"(spill_xmm1)
        :
        : "memory");
    (void)spill_r8; (void)spill_r9; (void)spill_xmm0; (void)spill_xmm1;

    if (!g_gc_in_progress && g_bytes_since_gc > GC_THRESHOLD) {
        cl_gc();
    }
    void *block = malloc(sizeof(GcHdr) + size);
    if (!block) {
        fprintf(stderr, "runtime error: out of memory\n");
        exit(1);
    }
    GcHdr *hdr = (GcHdr *)block;
    hdr->next   = g_allocs;
    hdr->size   = (uint32_t)size;
    hdr->kind   = (uint16_t)kind;
    hdr->marked = 0;
    g_allocs = hdr;
    g_bytes_since_gc += size;
    return (void *)((char *)block + sizeof(GcHdr));
}

/* Tracked realloc: allocates a fresh block, copies, and lets the old
   block be reclaimed by the next sweep. Required so that the items /
   keys / values / upvals buffers grow without bypassing the GC. */
static void *cl_realloc(void *old, size_t newsize, GcKind kind) {
    if (!old) return cl_alloc(newsize, kind);
    GcHdr *hdr = (GcHdr *)((char *)old - sizeof(GcHdr));
    void *newblock = cl_alloc(newsize, kind);
    size_t copy = hdr->size < newsize ? hdr->size : newsize;
    memcpy(newblock, old, copy);
    return newblock;
}

/* --- Mark phase --------------------------------------------------- */

static void cl_mark_descend(GcHdr *hdr, void *pl);

/* Try to interpret `bits` as a pointer to one of our allocations.
   Two cases: raw pointer (top 16 bits == 0) and NaN-boxed (top 16
   bits in our tag range). */
static void cl_mark_word(uint64_t bits) {
    uint16_t tag = (uint16_t)(bits >> 48);
    void *p;
    if (tag == 0) {
        /* Raw user-space pointer or just a small integer. */
        p = (void *)(uintptr_t)bits;
    } else if (tag >= CL_TAG_STR && tag <= CL_TAG_CPX) {
        /* NaN-boxed tagged value. */
        p = (void *)(uintptr_t)(bits & 0x0000FFFFFFFFFFFFULL);
    } else {
        return;
    }
    if (!p) return;
    /* Linear search the allocation list — O(n) per probe. For our
       test scale this is fine; a real GC would index by address. */
    for (GcHdr *h = g_allocs; h; h = h->next) {
        if (h->marked) continue;
        void *pl = (void *)((char *)h + sizeof(GcHdr));
        if (pl == p) {
            h->marked = 1;
            cl_mark_descend(h, pl);
            return;
        }
    }
}

static void cl_mark_child_alloc(void *child) {
    if (!child) return;
    GcHdr *h = (GcHdr *)((char *)child - sizeof(GcHdr));
    if (h->marked) return;
    h->marked = 1;
    cl_mark_descend(h, child);
}

static void cl_mark_descend(GcHdr *hdr, void *pl) {
    switch (hdr->kind) {
        case GC_STR:
            return;                       /* payload is opaque bytes */
        case GC_RAW:
            return;                       /* parent walks contents */
        case GC_CPX:
            return;                       /* two doubles, nothing to scan */
        case GC_BOX: {
            Value v = *(Value *)pl;
            cl_mark_word((uint64_t)v);
            return;
        }
        case GC_ARR: {
            CalcArr *a = (CalcArr *)pl;
            cl_mark_child_alloc(a->items);
            for (uint64_t i = 0; i < a->len; i++)
                cl_mark_word((uint64_t)a->items[i]);
            return;
        }
        case GC_MAP: {
            CalcMap *m = (CalcMap *)pl;
            cl_mark_child_alloc(m->keys);
            cl_mark_child_alloc(m->values);
            for (uint64_t i = 0; i < m->len; i++) {
                cl_mark_word((uint64_t)m->keys[i]);
                cl_mark_word((uint64_t)m->values[i]);
            }
            return;
        }
        case GC_CLOSURE: {
            CalcClosure *c = (CalcClosure *)pl;
            cl_mark_child_alloc(c->upvals);
            for (uint32_t i = 0; i < c->n_upvals; i++) {
                Value *box = c->upvals[i];
                /* Treat the box pointer itself as a heap pointer
                   reference — cl_mark_word will both mark the box
                   allocation and (via its GC_BOX descent) recurse
                   into the Value it holds. */
                cl_mark_word((uint64_t)(uintptr_t)box);
            }
            return;
        }
    }
}

/* --- Stack scan + sweep ------------------------------------------ */

/* Returns the highest stack address — the bottom of the call stack
   (where main's frame lives), in OS terms. The current rsp is the
   top (innermost callee). Together they bound the live stack range
   the conservative scan walks. */
static uint64_t *cl_stack_top(void) {
#ifdef _WIN32
    /* Read TEB.StackBase directly. On x64 Windows the TEB lives at
       GS:[0]; StackBase is the second field (offset 8). This is the
       address one past the highest legal stack byte. */
    uint64_t base;
    __asm__ __volatile__("mov %%gs:8, %0" : "=r"(base));
    return (uint64_t *)base;
#elif defined(__linux__)
    static uint64_t *cached = NULL;
    if (!cached) {
        pthread_attr_t a;
        if (pthread_getattr_np(pthread_self(), &a) == 0) {
            void *base; size_t size;
            if (pthread_attr_getstack(&a, &base, &size) == 0) {
                cached = (uint64_t *)((char *)base + size);
            }
            pthread_attr_destroy(&a);
        }
    }
    return cached;
#else
    /* Fallback: skip stack scan (this would defeat GC; refuse). */
    return NULL;
#endif
}

static void cl_gc(void) {
    g_gc_in_progress = 1;

    uint64_t *rsp;
    __asm__ __volatile__("mov %%rsp, %0" : "=r"(rsp));
    uint64_t *top = cl_stack_top();
    if (top && top > rsp) {
        for (uint64_t *p = rsp; p < top; p++) {
            cl_mark_word(*p);
        }
    }

    /* Sweep */
    GcHdr **pp = &g_allocs;
    while (*pp) {
        GcHdr *h = *pp;
        if (h->marked) {
            h->marked = 0;
            pp = &h->next;
        } else {
            *pp = h->next;
            free(h);
        }
    }

    g_bytes_since_gc = 0;
    g_gc_in_progress = 0;
}

/* --- die ---------------------------------------------------------- */
static void cl_die_rt(const char *msg) {
    fprintf(stderr, "runtime error: %s\n", msg);
    exit(1);
}

static void require_num(Value v, const char *where) {
    if (!cl_is_num(v)) {
        fprintf(stderr, "runtime error: %s expected num, got non-num\n", where);
        exit(1);
    }
}
static void require_str(Value v, const char *where) {
    if (!cl_is_str(v)) {
        fprintf(stderr, "runtime error: %s expected str\n", where);
        exit(1);
    }
}
static void require_arr(Value v, const char *where) {
    if (!cl_is_arr(v)) {
        fprintf(stderr, "runtime error: %s expected arr\n", where);
        exit(1);
    }
}
static void require_map(Value v, const char *where) {
    if (!cl_is_map(v)) {
        fprintf(stderr, "runtime error: %s expected map\n", where);
        exit(1);
    }
}

/* --- String allocation ------------------------------------------- */

Value cl_new_str(const char *bytes, uint64_t len) {
    CalcStr *s = (CalcStr *)cl_alloc(sizeof(CalcStr) + len + 1, GC_STR);
    s->len = len;
    if (len) memcpy(s->data, bytes, (size_t)len);
    s->data[len] = '\0';
    return cl_make_tagged(CL_TAG_STR, s);
}

static Value num_to_str(double d) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%.10g", d);
    if (n < 0) n = 0;
    return cl_new_str(buf, (uint64_t)n);
}

/* --- '+' polymorphic dispatch ------------------------------------- */
Value cl_concat(Value a, Value b) {
    /* Both operands coerced to string per CalcLang's `+` semantics:
       num + str / str + num both coerce the num via %.10g. */
    Value sa = cl_is_num(a) ? num_to_str(cl_as_num(a))
             : cl_is_str(a) ? a
             : (cl_die_rt("'+' got unsupported lhs tag"), (Value)0);
    Value sb = cl_is_num(b) ? num_to_str(cl_as_num(b))
             : cl_is_str(b) ? b
             : (cl_die_rt("'+' got unsupported rhs tag"), (Value)0);
    CalcStr *A = cl_as_str(sa);
    CalcStr *B = cl_as_str(sb);
    uint64_t total = A->len + B->len;
    CalcStr *out = (CalcStr *)cl_alloc(sizeof(CalcStr) + total + 1, GC_STR);
    out->len = total;
    memcpy(out->data,           A->data, (size_t)A->len);
    memcpy(out->data + A->len,  B->data, (size_t)B->len);
    out->data[total] = '\0';
    return cl_make_tagged(CL_TAG_STR, out);
}

/* Internal: build a CalcCpx Value from raw doubles. */
static Value cpx_new(double re, double im) {
    CalcCpx *c = (CalcCpx *)cl_alloc(sizeof(CalcCpx), GC_CPX);
    c->re = re;
    c->im = im;
    return cl_make_tagged(CL_TAG_CPX, c);
}

/* Unpack a num or cpx into (re, im). Returns 0 if `v` is neither. */
static int unpack_re_im(Value v, double *re, double *im) {
    if (cl_is_num(v)) { *re = cl_as_num(v); *im = 0.0; return 1; }
    if (cl_is_cpx(v)) {
        CalcCpx *c = cl_as_cpx(v);
        *re = c->re; *im = c->im;
        return 1;
    }
    return 0;
}

Value cl_op_plus(Value a, Value b) {
    if (cl_is_num(a) && cl_is_num(b)) {
        return cl_from_num(cl_as_num(a) + cl_as_num(b));
    }
    /* Complex addition. If either operand is complex, both promote
       to complex (a real x is x + 0i). */
    if (cl_is_cpx(a) || cl_is_cpx(b)) {
        double ar, ai, br, bi;
        if (!unpack_re_im(a, &ar, &ai) || !unpack_re_im(b, &br, &bi))
            cl_die_rt("'+' got incompatible operands for complex");
        return cpx_new(ar + br, ai + bi);
    }
    return cl_concat(a, b);
}

Value cl_op_minus(Value a, Value b) {
    if (cl_is_num(a) && cl_is_num(b))
        return cl_from_num(cl_as_num(a) - cl_as_num(b));
    double ar, ai, br, bi;
    if (!unpack_re_im(a, &ar, &ai) || !unpack_re_im(b, &br, &bi))
        cl_die_rt("'-' requires num or cpx operands");
    return cpx_new(ar - br, ai - bi);
}

Value cl_op_mul(Value a, Value b) {
    if (cl_is_num(a) && cl_is_num(b))
        return cl_from_num(cl_as_num(a) * cl_as_num(b));
    double ar, ai, br, bi;
    if (!unpack_re_im(a, &ar, &ai) || !unpack_re_im(b, &br, &bi))
        cl_die_rt("'*' requires num or cpx operands");
    /* (ar + ai i)(br + bi i) = (ar*br - ai*bi) + (ar*bi + ai*br) i */
    return cpx_new(ar * br - ai * bi, ar * bi + ai * br);
}

Value cl_op_div(Value a, Value b) {
    if (cl_is_num(a) && cl_is_num(b))
        return cl_from_num(cl_as_num(a) / cl_as_num(b));
    double ar, ai, br, bi;
    if (!unpack_re_im(a, &ar, &ai) || !unpack_re_im(b, &br, &bi))
        cl_die_rt("'/' requires num or cpx operands");
    /* (ar + ai i) / (br + bi i)
       multiply numerator and denominator by conjugate of denominator:
       = ((ar*br + ai*bi) + (ai*br - ar*bi) i) / (br^2 + bi^2)        */
    double denom = br * br + bi * bi;
    if (denom == 0.0) cl_die_rt("complex division by zero");
    return cpx_new((ar * br + ai * bi) / denom,
                   (ai * br - ar * bi) / denom);
}

Value cl_op_mod(Value a, Value b) {
    /* mod on complex is undefined; numeric fmod path only. */
    if (cl_is_num(a) && cl_is_num(b))
        return cl_from_num(fmod(cl_as_num(a), cl_as_num(b)));
    cl_die_rt("'%' requires numeric operands");
    return (Value)0;
}

Value cl_op_neg(Value a) {
    if (cl_is_num(a)) return cl_from_num(-cl_as_num(a));
    if (cl_is_cpx(a)) {
        CalcCpx *c = cl_as_cpx(a);
        return cpx_new(-c->re, -c->im);
    }
    cl_die_rt("unary '-' requires num or cpx");
    return (Value)0;
}

/* Bitwise polymorphic helpers. Both operands are coerced to int64
   (truncating toward zero, matching the VM's (int64_t) cast). Non-num
   operands raise a clear runtime error. */
Value cl_op_band(Value a, Value b) {
    if (!cl_is_num(a) || !cl_is_num(b)) cl_die_rt("'&' requires numeric operands");
    return cl_from_num((double)((int64_t)cl_as_num(a) & (int64_t)cl_as_num(b)));
}
Value cl_op_bor(Value a, Value b) {
    if (!cl_is_num(a) || !cl_is_num(b)) cl_die_rt("'|' requires numeric operands");
    return cl_from_num((double)((int64_t)cl_as_num(a) | (int64_t)cl_as_num(b)));
}
Value cl_op_bxor(Value a, Value b) {
    if (!cl_is_num(a) || !cl_is_num(b)) cl_die_rt("'^' requires numeric operands");
    return cl_from_num((double)((int64_t)cl_as_num(a) ^ (int64_t)cl_as_num(b)));
}
Value cl_op_shl(Value a, Value b) {
    if (!cl_is_num(a) || !cl_is_num(b)) cl_die_rt("'<<' requires numeric operands");
    return cl_from_num((double)((int64_t)cl_as_num(a) << ((int64_t)cl_as_num(b) & 63)));
}
Value cl_op_shr(Value a, Value b) {
    if (!cl_is_num(a) || !cl_is_num(b)) cl_die_rt("'>>' requires numeric operands");
    return cl_from_num((double)((int64_t)cl_as_num(a) >> ((int64_t)cl_as_num(b) & 63)));
}
Value cl_op_bnot(Value a) {
    if (!cl_is_num(a)) cl_die_rt("'~' requires a numeric operand");
    return cl_from_num((double)(~(int64_t)cl_as_num(a)));
}

/* Forward decl: cl_throw uses format_value to print an uncaught
   exception, but format_value is defined later in the file. */
static void format_value(Value v, int nested);

/* --- Exceptions --------------------------------------------------- */

#define CL_MAX_HANDLERS 256

typedef struct {
    void *rsp;       /* native rsp at try-block entry */
    void *rbp;       /* native rbp at try-block entry */
    void *catch_pc;  /* address of the catch handler label */
} ExHandler;

static ExHandler g_handlers[CL_MAX_HANDLERS];
static int       g_handler_top = -1;       /* -1 = no active handler */
static Value     g_exception_value = 0;

/* Trampoline globals — populated by cl_throw, then loaded by inline
   asm that restores rsp/rbp/rip in one atomic-looking sequence. */
static volatile uintptr_t g_throw_rsp;
static volatile uintptr_t g_throw_rbp;
static volatile uintptr_t g_throw_pc;

void cl_try_push(void *rsp, void *rbp, void *catch_pc) {
    if (g_handler_top + 1 >= CL_MAX_HANDLERS) cl_die_rt("try handler stack overflow");
    g_handler_top++;
    g_handlers[g_handler_top].rsp      = rsp;
    g_handlers[g_handler_top].rbp      = rbp;
    g_handlers[g_handler_top].catch_pc = catch_pc;
}

void cl_try_pop(void) {
    if (g_handler_top < 0) cl_die_rt("try handler stack underflow");
    g_handler_top--;
}

Value cl_get_exception(void) {
    return g_exception_value;
}

void cl_throw(Value v) {
    if (g_handler_top < 0) {
        fprintf(stderr, "uncaught exception: ");
        format_value(v, 0);
        putchar('\n');
        exit(1);
    }
    g_exception_value = v;
    ExHandler h = g_handlers[g_handler_top];
    g_handler_top--;
    g_throw_rsp = (uintptr_t)h.rsp;
    g_throw_rbp = (uintptr_t)h.rbp;
    g_throw_pc  = (uintptr_t)h.catch_pc;
    /* Restore rsp/rbp and jump to the catch label. Load all three
       targets into volatile scratch regs first so we don't reference
       any stack-relative or rbp-relative memory after rsp/rbp move. */
    __asm__ __volatile__(
        "movq g_throw_rsp(%%rip), %%r10\n\t"
        "movq g_throw_rbp(%%rip), %%r11\n\t"
        "movq g_throw_pc(%%rip),  %%rax\n\t"
        "movq %%r10, %%rsp\n\t"
        "movq %%r11, %%rbp\n\t"
        "jmpq *%%rax\n\t"
        :: : "memory");
    __builtin_unreachable();
}

/* --- Equality dispatch ------------------------------------------- */
static int values_equal(Value a, Value b) {
    if (cl_is_num(a) && cl_is_num(b)) return cl_as_num(a) == cl_as_num(b);
    if (cl_is_str(a) && cl_is_str(b)) {
        CalcStr *A = cl_as_str(a);
        CalcStr *B = cl_as_str(b);
        return A->len == B->len && memcmp(A->data, B->data, (size_t)A->len) == 0;
    }
    if (cl_is_arr(a) && cl_is_arr(b)) {
        /* Reference identity, matching the VM. */
        return cl_as_arr(a) == cl_as_arr(b);
    }
    if (cl_is_map(a) && cl_is_map(b)) {
        return cl_as_map(a) == cl_as_map(b);
    }
    if (cl_is_closure(a) && cl_is_closure(b)) {
        /* Same underlying code => equal (matches VM behavior for
           bare-name fn references that share an entry point). */
        return cl_as_closure(a)->code == cl_as_closure(b)->code;
    }
    if (cl_is_cpx(a) && cl_is_cpx(b)) {
        CalcCpx *A = cl_as_cpx(a);
        CalcCpx *B = cl_as_cpx(b);
        return A->re == B->re && A->im == B->im;
    }
    /* Mixed types: never equal. */
    return 0;
}

Value cl_op_eq(Value a, Value b)  { return cl_from_num(values_equal(a, b) ? 1.0 : 0.0); }
Value cl_op_neq(Value a, Value b) { return cl_from_num(values_equal(a, b) ? 0.0 : 1.0); }

/* Three-way comparison. Returns -1 / 0 / 1 for both-num and both-str;
   exits on mixed-type comparisons (matches VM semantics). */
static int values_cmp(Value a, Value b, const char *where) {
    if (cl_is_num(a) && cl_is_num(b)) {
        double x = cl_as_num(a), y = cl_as_num(b);
        return (x < y) ? -1 : (x > y) ? 1 : 0;
    }
    if (cl_is_str(a) && cl_is_str(b)) {
        const CalcStr *A = cl_as_str(a);
        const CalcStr *B = cl_as_str(b);
        uint64_t n = A->len < B->len ? A->len : B->len;
        int c = memcmp(A->data, B->data, (size_t)n);
        if (c) return c < 0 ? -1 : 1;
        return (A->len < B->len) ? -1 : (A->len > B->len) ? 1 : 0;
    }
    fprintf(stderr, "runtime error: %s: comparing incompatible types\n", where);
    exit(1);
}
Value cl_op_lt(Value a, Value b) { return cl_from_num(values_cmp(a, b, "<")  <  0 ? 1.0 : 0.0); }
Value cl_op_le(Value a, Value b) { return cl_from_num(values_cmp(a, b, "<=") <= 0 ? 1.0 : 0.0); }
Value cl_op_gt(Value a, Value b) { return cl_from_num(values_cmp(a, b, ">")  >  0 ? 1.0 : 0.0); }
Value cl_op_ge(Value a, Value b) { return cl_from_num(values_cmp(a, b, ">=") >= 0 ? 1.0 : 0.0); }

/* --- Truthiness --------------------------------------------------- */
Value cl_truthy(Value v) {
    if (cl_is_num(v)) {
        double d = cl_as_num(v);
        return cl_from_num((d != 0.0 && !isnan(d)) ? 1.0 : 0.0);
    }
    if (cl_is_str(v)) return cl_from_num(cl_as_str(v)->len ? 1.0 : 0.0);
    if (cl_is_arr(v)) return cl_from_num(cl_as_arr(v)->len ? 1.0 : 0.0);
    if (cl_is_map(v)) return cl_from_num(cl_as_map(v)->len ? 1.0 : 0.0);
    if (cl_is_cpx(v)) {
        CalcCpx *c = cl_as_cpx(v);
        return cl_from_num((c->re != 0.0 || c->im != 0.0) ? 1.0 : 0.0);
    }
    /* Closures and other tagged values are always truthy. */
    return cl_from_num(1.0);
}

/* --- Arrays ------------------------------------------------------- */

Value cl_new_arr(void) {
    CalcArr *a = (CalcArr *)cl_alloc(sizeof(CalcArr), GC_ARR);
    a->len   = 0;
    a->cap   = 0;
    a->items = NULL;
    return cl_make_tagged(CL_TAG_ARR, a);
}

static void arr_grow(CalcArr *a, uint64_t want) {
    if (a->cap >= want) return;
    uint64_t newcap = a->cap == 0 ? 4 : a->cap * 2;
    while (newcap < want) newcap *= 2;
    a->items = (Value *)cl_realloc(a->items, sizeof(Value) * (size_t)newcap, GC_RAW);
    a->cap = newcap;
}

Value cl_arr_push(Value arrv, Value v) {
    require_arr(arrv, "push");
    CalcArr *a = cl_as_arr(arrv);
    arr_grow(a, a->len + 1);
    a->items[a->len++] = v;
    return cl_from_num((double)a->len);
}

Value cl_arr_pop(Value arrv) {
    require_arr(arrv, "pop");
    CalcArr *a = cl_as_arr(arrv);
    if (a->len == 0) cl_die_rt("pop from empty array");
    return a->items[--a->len];
}

/* --- Maps --------------------------------------------------------- */

Value cl_new_map(void) {
    CalcMap *m = (CalcMap *)cl_alloc(sizeof(CalcMap), GC_MAP);
    m->len = 0;
    m->cap = 0;
    m->keys = NULL;
    m->values = NULL;
    return cl_make_tagged(CL_TAG_MAP, m);
}

static int map_find(const CalcMap *m, Value key) {
    for (uint64_t i = 0; i < m->len; i++) {
        if (values_equal(m->keys[i], key)) return (int)i;
    }
    return -1;
}

static void map_grow(CalcMap *m, uint64_t want) {
    if (m->cap >= want) return;
    uint64_t newcap = m->cap == 0 ? 4 : m->cap * 2;
    while (newcap < want) newcap *= 2;
    m->keys   = (Value *)cl_realloc(m->keys,   sizeof(Value) * (size_t)newcap, GC_RAW);
    m->values = (Value *)cl_realloc(m->values, sizeof(Value) * (size_t)newcap, GC_RAW);
    m->cap = newcap;
}

static void map_set(CalcMap *m, Value key, Value v) {
    if (!cl_is_num(key) && !cl_is_str(key))
        cl_die_rt("map key must be num or str");
    int i = map_find(m, key);
    if (i >= 0) { m->values[i] = v; return; }
    map_grow(m, m->len + 1);
    m->keys[m->len]   = key;
    m->values[m->len] = v;
    m->len++;
}

/* Indexing dispatch (arrays / strings / maps). */
Value cl_index_get(Value coll, Value idx) {
    if (cl_is_arr(coll)) {
        require_num(idx, "array index");
        CalcArr *a = cl_as_arr(coll);
        int64_t i = (int64_t)cl_as_num(idx);
        if (i < 0 || (uint64_t)i >= a->len) cl_die_rt("array index out of range");
        return a->items[i];
    }
    if (cl_is_map(coll)) {
        CalcMap *m = cl_as_map(coll);
        int i = map_find(m, idx);
        if (i < 0) cl_die_rt("map key not found");
        return m->values[i];
    }
    if (cl_is_str(coll)) {
        /* str[i] is a 1-char string — same as str_at(s, i). */
        return cl_builtin_str_at(coll, idx);
    }
    cl_die_rt("index: target is not an array, map, or string");
    return (Value)0;
}

void cl_index_set(Value coll, Value idx, Value v) {
    if (cl_is_arr(coll)) {
        require_num(idx, "array index");
        CalcArr *a = cl_as_arr(coll);
        int64_t i = (int64_t)cl_as_num(idx);
        if (i < 0 || (uint64_t)i >= a->len) cl_die_rt("array index out of range");
        a->items[i] = v;
        return;
    }
    if (cl_is_map(coll)) {
        map_set(cl_as_map(coll), idx, v);
        return;
    }
    cl_die_rt("index assign: target is not an array or map");
}

/* --- print (recursive, with nested string quoting) --------------- */
static void format_value(Value v, int nested);

static void format_str(const CalcStr *s, int nested) {
    if (!nested) {
        fwrite(s->data, 1, (size_t)s->len, stdout);
        return;
    }
    putchar('"');
    for (uint64_t i = 0; i < s->len; i++) {
        char c = s->data[i];
        switch (c) {
            case '\n': fputs("\\n", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '"':  fputs("\\\"", stdout); break;
            default:   putchar(c);
        }
    }
    putchar('"');
}

static void format_arr(const CalcArr *a) {
    putchar('[');
    for (uint64_t i = 0; i < a->len; i++) {
        if (i) fputs(", ", stdout);
        format_value(a->items[i], 1);
    }
    putchar(']');
}

static void format_map(const CalcMap *m) {
    putchar('{');
    for (uint64_t i = 0; i < m->len; i++) {
        if (i) fputs(", ", stdout);
        format_value(m->keys[i],   1);
        fputs(": ", stdout);
        format_value(m->values[i], 1);
    }
    putchar('}');
}

static void format_cpx(const CalcCpx *c) {
    /* "(re, im)" — unambiguous, no special cases. */
    printf("(%.10g, %.10g)", c->re, c->im);
}

static void format_value(Value v, int nested) {
    if (cl_is_num(v))          printf("%.10g", cl_as_num(v));
    else if (cl_is_str(v))     format_str(cl_as_str(v), nested);
    else if (cl_is_arr(v))     format_arr(cl_as_arr(v));
    else if (cl_is_map(v))     format_map(cl_as_map(v));
    else if (cl_is_closure(v)) fputs("fn", stdout);
    else if (cl_is_cpx(v))     format_cpx(cl_as_cpx(v));
    else                        printf("<value tag=0x%04x>", cl_tag_of(v));
}

void cl_print(Value v) {
    format_value(v, 0);
    putchar('\n');
}

/* --- type_of ------------------------------------------------------ */
Value cl_builtin_type_of(Value v) {
    if (cl_is_num(v))               return cl_new_str("num", 3);
    if (cl_is_str(v))               return cl_new_str("str", 3);
    if (cl_is_arr(v))               return cl_new_str("arr", 3);
    if (cl_is_map(v))               return cl_new_str("map", 3);
    if (cl_is_closure(v))           return cl_new_str("fn",  2);
    if (cl_is_cpx(v))               return cl_new_str("cpx", 3);
    if (cl_tag_of(v) == CL_TAG_FN)  return cl_new_str("fn",  2);
    return cl_new_str("unknown", 7);
}

/* --- Closures ----------------------------------------------------- */

Value *cl_box_new(Value initial) {
    Value *b = (Value *)cl_alloc(sizeof(Value), GC_BOX);
    *b = initial;
    return b;
}

Value cl_new_closure(void *code, uint32_t n_upvals) {
    CalcClosure *c = (CalcClosure *)cl_alloc(sizeof(CalcClosure), GC_CLOSURE);
    c->code     = code;
    c->n_upvals = n_upvals;
    c->pad      = 0;
    c->upvals   = n_upvals ? (Value **)cl_alloc(sizeof(Value *) * n_upvals, GC_RAW)
                           : NULL;
    return cl_make_tagged(CL_TAG_CLOSURE, c);
}

/* --- to_str / to_num --------------------------------------------- */
Value cl_builtin_to_str(Value v) {
    if (cl_is_num(v)) return num_to_str(cl_as_num(v));
    if (cl_is_str(v)) return v;  /* idempotent on strings */
    cl_die_rt("to_str: unsupported value type");
    return (Value)0;
}

Value cl_builtin_to_num(Value v) {
    if (cl_is_num(v)) return v;
    require_str(v, "to_num");
    CalcStr *s = cl_as_str(v);
    if (s->len == 0) cl_die_rt("to_num: empty string");
    char *end;
    double d = strtod(s->data, &end);
    /* strtod stops at the first invalid char; if it didn't consume
       the whole string (after trimming trailing whitespace), reject. */
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    if ((uint64_t)(end - s->data) != s->len) {
        cl_die_rt("to_num: malformed number");
    }
    return cl_from_num(d);
}

/* --- len ---------------------------------------------------------- */
Value cl_builtin_len(Value v) {
    if (cl_is_str(v)) return cl_from_num((double)cl_as_str(v)->len);
    if (cl_is_arr(v)) return cl_from_num((double)cl_as_arr(v)->len);
    if (cl_is_map(v)) return cl_from_num((double)cl_as_map(v)->len);
    cl_die_rt("len: unsupported value type");
    return (Value)0;
}

/* --- str_at(s, i) ------------------------------------------------- */
Value cl_builtin_str_at(Value sv, Value iv) {
    require_str(sv, "str_at");
    require_num(iv, "str_at");
    CalcStr *s = cl_as_str(sv);
    int64_t i = (int64_t)cl_as_num(iv);
    if (i < 0 || (uint64_t)i >= s->len) cl_die_rt("str_at: index out of range");
    return cl_new_str(&s->data[i], 1);
}

/* --- str_slice(s, a, b) ------------------------------------------ */
Value cl_builtin_str_slice(Value sv, Value av, Value bv) {
    require_str(sv, "str_slice");
    require_num(av, "str_slice");
    require_num(bv, "str_slice");
    CalcStr *s = cl_as_str(sv);
    int64_t a = (int64_t)cl_as_num(av);
    int64_t b = (int64_t)cl_as_num(bv);
    if (a < 0) a = 0;
    if ((uint64_t)b > s->len) b = (int64_t)s->len;
    if (b < a) b = a;
    return cl_new_str(&s->data[a], (uint64_t)(b - a));
}

/* --- str_find(s, sub) -------------------------------------------- */
Value cl_builtin_str_find(Value sv, Value subv) {
    require_str(sv,  "str_find");
    require_str(subv, "str_find");
    CalcStr *s   = cl_as_str(sv);
    CalcStr *sub = cl_as_str(subv);
    if (sub->len == 0) return cl_from_num(0.0);
    if (sub->len > s->len) return cl_from_num(-1.0);
    for (uint64_t i = 0; i + sub->len <= s->len; i++) {
        if (memcmp(&s->data[i], sub->data, (size_t)sub->len) == 0)
            return cl_from_num((double)i);
    }
    return cl_from_num(-1.0);
}

/* --- str_upper / str_lower --------------------------------------- */
static Value str_case(Value sv, int upper) {
    require_str(sv, upper ? "str_upper" : "str_lower");
    CalcStr *s = cl_as_str(sv);
    char *buf = (char *)malloc((size_t)s->len);
    if (!buf && s->len) cl_die_rt("out of memory");
    for (uint64_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        buf[i] = (char)(upper ? toupper(c) : tolower(c));
    }
    Value r = cl_new_str(buf, s->len);
    free(buf);
    return r;
}
Value cl_builtin_str_upper(Value sv) { return str_case(sv, 1); }
Value cl_builtin_str_lower(Value sv) { return str_case(sv, 0); }

/* --- str_trim ----------------------------------------------------- */
Value cl_builtin_str_trim(Value sv) {
    require_str(sv, "str_trim");
    CalcStr *s = cl_as_str(sv);
    uint64_t a = 0;
    while (a < s->len && isspace((unsigned char)s->data[a])) a++;
    uint64_t b = s->len;
    while (b > a && isspace((unsigned char)s->data[b - 1])) b--;
    return cl_new_str(&s->data[a], b - a);
}

/* --- str_repeat(s, n) -------------------------------------------- */
Value cl_builtin_str_repeat(Value sv, Value nv) {
    require_str(sv, "str_repeat");
    require_num(nv, "str_repeat");
    CalcStr *s = cl_as_str(sv);
    int64_t n  = (int64_t)cl_as_num(nv);
    if (n <= 0) return cl_new_str("", 0);
    uint64_t total = s->len * (uint64_t)n;
    CalcStr *out = (CalcStr *)cl_alloc(sizeof(CalcStr) + total + 1, GC_STR);
    out->len = total;
    for (int64_t k = 0; k < n; k++) {
        memcpy(out->data + (uint64_t)k * s->len, s->data, (size_t)s->len);
    }
    out->data[total] = '\0';
    return cl_make_tagged(CL_TAG_STR, out);
}

/* --- str_starts_with / str_ends_with ----------------------------- */
Value cl_builtin_str_starts_with(Value sv, Value pv) {
    require_str(sv, "str_starts_with");
    require_str(pv, "str_starts_with");
    CalcStr *s = cl_as_str(sv);
    CalcStr *p = cl_as_str(pv);
    if (p->len > s->len) return cl_from_num(0.0);
    return cl_from_num(memcmp(s->data, p->data, (size_t)p->len) == 0 ? 1.0 : 0.0);
}
Value cl_builtin_str_ends_with(Value sv, Value pv) {
    require_str(sv, "str_ends_with");
    require_str(pv, "str_ends_with");
    CalcStr *s = cl_as_str(sv);
    CalcStr *p = cl_as_str(pv);
    if (p->len > s->len) return cl_from_num(0.0);
    return cl_from_num(memcmp(s->data + (s->len - p->len), p->data, (size_t)p->len) == 0 ? 1.0 : 0.0);
}

/* --- array_* calclib --------------------------------------------- */

Value cl_builtin_array_reverse(Value av) {
    require_arr(av, "array_reverse");
    CalcArr *a = cl_as_arr(av);
    for (uint64_t i = 0, j = a->len ? a->len - 1 : 0; i < j; i++, j--) {
        Value t = a->items[i]; a->items[i] = a->items[j]; a->items[j] = t;
    }
    return av;
}

/* Comparator-free sort: detect homogeneous all-num or all-str arrays
   (anything else is rejected) and use the corresponding strict
   ordering. Matches VM behavior. */
static int cmp_num(const void *x, const void *y) {
    double a = cl_as_num(*(const Value *)x);
    double b = cl_as_num(*(const Value *)y);
    return (a < b) ? -1 : (a > b) ? 1 : 0;
}
static int cmp_str(const void *x, const void *y) {
    const CalcStr *A = cl_as_str(*(const Value *)x);
    const CalcStr *B = cl_as_str(*(const Value *)y);
    uint64_t n = A->len < B->len ? A->len : B->len;
    int c = memcmp(A->data, B->data, (size_t)n);
    if (c) return c;
    return (A->len < B->len) ? -1 : (A->len > B->len) ? 1 : 0;
}
Value cl_builtin_array_sort(Value av) {
    require_arr(av, "array_sort");
    CalcArr *a = cl_as_arr(av);
    if (a->len == 0) return av;
    int all_num = 1, all_str = 1;
    for (uint64_t i = 0; i < a->len; i++) {
        if (!cl_is_num(a->items[i])) all_num = 0;
        if (!cl_is_str(a->items[i])) all_str = 0;
    }
    if (!all_num && !all_str) cl_die_rt("array_sort: mixed-type array");
    qsort(a->items, (size_t)a->len, sizeof(Value), all_num ? cmp_num : cmp_str);
    return av;
}

Value cl_builtin_array_concat(Value av, Value bv) {
    require_arr(av, "array_concat");
    require_arr(bv, "array_concat");
    CalcArr *a = cl_as_arr(av);
    CalcArr *b = cl_as_arr(bv);
    Value out = cl_new_arr();
    CalcArr *o = cl_as_arr(out);
    arr_grow(o, a->len + b->len);
    for (uint64_t i = 0; i < a->len; i++) o->items[o->len++] = a->items[i];
    for (uint64_t i = 0; i < b->len; i++) o->items[o->len++] = b->items[i];
    return out;
}

Value cl_builtin_array_slice(Value av, Value lov, Value hiv) {
    require_arr(av, "array_slice");
    require_num(lov, "array_slice");
    require_num(hiv, "array_slice");
    CalcArr *a = cl_as_arr(av);
    int64_t lo = (int64_t)cl_as_num(lov);
    int64_t hi = (int64_t)cl_as_num(hiv);
    if (lo < 0) lo = 0;
    if ((uint64_t)hi > a->len) hi = (int64_t)a->len;
    if (hi < lo) hi = lo;
    Value out = cl_new_arr();
    CalcArr *o = cl_as_arr(out);
    arr_grow(o, (uint64_t)(hi - lo));
    for (int64_t i = lo; i < hi; i++) o->items[o->len++] = a->items[i];
    return out;
}

Value cl_builtin_array_find(Value av, Value v) {
    require_arr(av, "array_find");
    CalcArr *a = cl_as_arr(av);
    for (uint64_t i = 0; i < a->len; i++) {
        if (values_equal(a->items[i], v)) return cl_from_num((double)i);
    }
    return cl_from_num(-1.0);
}

Value cl_builtin_array_contains(Value av, Value v) {
    require_arr(av, "array_contains");
    CalcArr *a = cl_as_arr(av);
    for (uint64_t i = 0; i < a->len; i++) {
        if (values_equal(a->items[i], v)) return cl_from_num(1.0);
    }
    return cl_from_num(0.0);
}

Value cl_builtin_array_range(Value lov, Value hiv) {
    require_num(lov, "array_range");
    require_num(hiv, "array_range");
    int64_t lo = (int64_t)cl_as_num(lov);
    int64_t hi = (int64_t)cl_as_num(hiv);
    if (hi < lo) hi = lo;
    Value out = cl_new_arr();
    CalcArr *o = cl_as_arr(out);
    arr_grow(o, (uint64_t)(hi - lo));
    for (int64_t i = lo; i < hi; i++) o->items[o->len++] = cl_from_num((double)i);
    return out;
}

/* --- Map calclib ------------------------------------------------- */

Value cl_builtin_keys(Value mv) {
    require_map(mv, "keys");
    CalcMap *m = cl_as_map(mv);
    Value out = cl_new_arr();
    CalcArr *a = cl_as_arr(out);
    arr_grow(a, m->len);
    for (uint64_t i = 0; i < m->len; i++) a->items[a->len++] = m->keys[i];
    return out;
}

Value cl_builtin_values(Value mv) {
    require_map(mv, "values");
    CalcMap *m = cl_as_map(mv);
    Value out = cl_new_arr();
    CalcArr *a = cl_as_arr(out);
    arr_grow(a, m->len);
    for (uint64_t i = 0; i < m->len; i++) a->items[a->len++] = m->values[i];
    return out;
}

Value cl_builtin_has_key(Value mv, Value k) {
    require_map(mv, "has_key");
    return cl_from_num(map_find(cl_as_map(mv), k) >= 0 ? 1.0 : 0.0);
}

Value cl_builtin_del(Value mv, Value k) {
    require_map(mv, "del");
    CalcMap *m = cl_as_map(mv);
    int i = map_find(m, k);
    if (i < 0) return cl_from_num(0.0);
    /* Shift later entries down to preserve insertion order. */
    for (uint64_t j = (uint64_t)i; j + 1 < m->len; j++) {
        m->keys[j]   = m->keys[j + 1];
        m->values[j] = m->values[j + 1];
    }
    m->len--;
    return cl_from_num(1.0);
}

/* --- ctype-style character helpers ------------------------------------- */

static unsigned char cl_first_byte(Value v, const char *name) {
    if (!cl_is_str(v)) cl_die_rt("ctype expects 1-char string");
    CalcStr *s = cl_as_str(v);
    if (s->len < 1) cl_die_rt("ctype expects non-empty string");
    (void)name;
    return (unsigned char)s->data[0];
}

Value cl_builtin_is_digit(Value v) {
    unsigned char c = cl_first_byte(v, "is_digit");
    return cl_from_num((c >= '0' && c <= '9') ? 1.0 : 0.0);
}
Value cl_builtin_is_alpha(Value v) {
    unsigned char c = cl_first_byte(v, "is_alpha");
    int r = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return cl_from_num(r ? 1.0 : 0.0);
}
Value cl_builtin_is_alnum(Value v) {
    unsigned char c = cl_first_byte(v, "is_alnum");
    int r = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return cl_from_num(r ? 1.0 : 0.0);
}
Value cl_builtin_is_space(Value v) {
    unsigned char c = cl_first_byte(v, "is_space");
    int r = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
    return cl_from_num(r ? 1.0 : 0.0);
}
Value cl_builtin_is_upper(Value v) {
    unsigned char c = cl_first_byte(v, "is_upper");
    return cl_from_num((c >= 'A' && c <= 'Z') ? 1.0 : 0.0);
}
Value cl_builtin_is_lower(Value v) {
    unsigned char c = cl_first_byte(v, "is_lower");
    return cl_from_num((c >= 'a' && c <= 'z') ? 1.0 : 0.0);
}
Value cl_builtin_char_to_upper(Value v) {
    unsigned char c = cl_first_byte(v, "char_to_upper");
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
    char tmp[2] = { (char)c, '\0' };
    return cl_new_str(tmp, 1);
}
Value cl_builtin_char_to_lower(Value v) {
    unsigned char c = cl_first_byte(v, "char_to_lower");
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
    char tmp[2] = { (char)c, '\0' };
    return cl_new_str(tmp, 1);
}
Value cl_builtin_char_code(Value v) {
    if (!cl_is_str(v)) cl_die_rt("char_code expects str");
    CalcStr *s = cl_as_str(v);
    if (s->len < 1) cl_die_rt("char_code expects non-empty string");
    return cl_from_num((double)(unsigned char)s->data[0]);
}
Value cl_builtin_char_from(Value v) {
    require_num(v, "char_from");
    int code = (int)cl_as_num(v);
    if (code < 0 || code > 255) cl_die_rt("char_from: code out of byte range");
    char tmp[2] = { (char)code, '\0' };
    return cl_new_str(tmp, 1);
}

/* --- printf-style formatter ------------------------------------------- */

/* Translate a Rust-style format-string fragment "{...}" into the
   equivalent C-style "%..." in `out`. `out` must have room for at least
   16 chars. Returns the number of source bytes consumed (including
   braces). The fragment starts at p[0] == '{'. Rules:
     {}           -> %s
     {:N}         -> %Ns
     {:.M}        -> %.Ms
     {:N.M}       -> %N.Ms
     {:d}/{:x}/{:o}/{:b}/{:f}/{:e}/{:g} -> %d/%x/%o/%b/%f/%e/%g
     {:Nd}/{:.Mf}/etc. combine width/precision with explicit type.
     {{ and }} are handled before this function is called.
*/
static int translate_brace(const char *p, char *out) {
    /* p[0] == '{'. Find closing brace. */
    const char *end = p + 1;
    while (*end && *end != '}') end++;
    if (*end != '}') return 0;   /* malformed; caller treats { as literal */
    char spec[16]; int slen = 0;
    if (p[1] == ':') {
        /* Copy chars between ':' and '}', up to 14. */
        const char *q = p + 2;
        while (q < end && slen < (int)sizeof(spec) - 2) spec[slen++] = *q++;
    }
    spec[slen] = '\0';
    /* Decide the conversion char: explicit if last char of spec is one
       of d/i/x/X/o/b/f/e/E/g/G; otherwise default to 's'. */
    char conv = 's';
    int  spec_end = slen;
    if (slen > 0) {
        char last = spec[slen - 1];
        if (last == 'd' || last == 'i' || last == 'x' || last == 'X'
         || last == 'o' || last == 'b' || last == 'f' || last == 'e'
         || last == 'E' || last == 'g' || last == 'G' || last == 's'
         || last == 'c') {
            conv = last;
            spec_end = slen - 1;
        }
    }
    out[0] = '%';
    int oi = 1;
    for (int i = 0; i < spec_end && oi < 14; i++) out[oi++] = spec[i];
    out[oi++] = conv;
    out[oi] = '\0';
    return (int)(end - p) + 1;   /* consumed including '}' */
}

/* Build a C-style printf format string from a possibly-Rust-style one.
   {} placeholders are translated to %; {{ and }} unescape to literals.
   Existing % specifiers pass through (`%%` is preserved). The result is
   written into a libc-malloc'd buffer; caller frees. */
static char *normalize_format(const char *src) {
    size_t cap = strlen(src) * 2 + 16;
    char *out = (char *)malloc(cap);
    if (!out) cl_die_rt("fmt: out of memory");
    size_t oi = 0;
    for (const char *p = src; *p; ) {
        if (oi + 32 >= cap) { cap *= 2; out = (char *)realloc(out, cap);
            if (!out) cl_die_rt("fmt: out of memory"); }
        if (p[0] == '{' && p[1] == '{') { out[oi++] = '{'; p += 2; continue; }
        if (p[0] == '}' && p[1] == '}') { out[oi++] = '}'; p += 2; continue; }
        if (p[0] == '{') {
            char buf[16];
            int n = translate_brace(p, buf);
            if (n == 0) { out[oi++] = *p++; continue; }   /* malformed → literal */
            for (int i = 0; buf[i]; i++) out[oi++] = buf[i];
            p += n;
            continue;
        }
        out[oi++] = *p++;
    }
    out[oi] = '\0';
    return out;
}

Value cl_builtin_fmt(Value fv, Value av) {
    if (!cl_is_str(fv)) cl_die_rt("fmt: format must be a string");
    if (!cl_is_arr(av)) cl_die_rt("fmt: args must be an array");
    CalcStr *fs   = cl_as_str(fv);
    CalcArr *args = cl_as_arr(av);
    char *normalized = normalize_format(fs->data);
    const char *fmt = normalized;
    size_t cap = 64;
    size_t len = 0;
    /* libc-malloc'd scratch buffer; copied to a CalcStr at the end. */
    char *buf = (char *)malloc(cap);
    if (!buf) cl_die_rt("fmt: out of memory");
    uint64_t arg_idx = 0;
    #define APPEND_C(c) do { \
        if (len + 1 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); if (!buf) cl_die_rt("fmt: out of memory"); } \
        buf[len++] = (char)(c); \
    } while (0)
    #define APPEND_S(s) do { const char *_p = (s); while (*_p) APPEND_C(*_p++); } while (0)

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { APPEND_C(*p); continue; }
        p++;
        if (*p == '%') { APPEND_C('%'); continue; }
        if (*p == '\0') break;
        char specbuf[32];
        int  si = 0;
        specbuf[si++] = '%';
        if (*p == '-' || *p == '0' || *p == '+' || *p == ' ' || *p == '#') specbuf[si++] = *p++;
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
        if (arg_idx >= args->len) { free(buf); cl_die_rt("fmt: not enough arguments for format string"); }
        Value v = args->items[arg_idx++];
        char tmp[256];
        switch (conv) {
            case 'd':
            case 'i':
                if (!cl_is_num(v)) { free(buf); cl_die_rt("fmt: %d expects num"); }
                specbuf[si++] = 'l'; specbuf[si++] = 'l'; specbuf[si++] = 'd'; specbuf[si] = '\0';
                snprintf(tmp, sizeof tmp, specbuf, (long long)cl_as_num(v));
                APPEND_S(tmp);
                break;
            case 'x':
            case 'X':
            case 'o':
                if (!cl_is_num(v)) { free(buf); cl_die_rt("fmt: %x/%o expects num"); }
                specbuf[si++] = 'l'; specbuf[si++] = 'l'; specbuf[si++] = conv; specbuf[si] = '\0';
                snprintf(tmp, sizeof tmp, specbuf, (long long)cl_as_num(v));
                APPEND_S(tmp);
                break;
            case 'b': {
                if (!cl_is_num(v)) { free(buf); cl_die_rt("fmt: %b expects num"); }
                unsigned long long u = (unsigned long long)(long long)cl_as_num(v);
                char bin[65];
                int bi = 0;
                if (u == 0) bin[bi++] = '0';
                while (u > 0) { bin[bi++] = (u & 1) ? '1' : '0'; u >>= 1; }
                while (bi > 0) APPEND_C(bin[--bi]);
                break;
            }
            case 'f':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
                if (!cl_is_num(v)) { free(buf); cl_die_rt("fmt: %f/%e/%g expects num"); }
                specbuf[si++] = conv; specbuf[si] = '\0';
                snprintf(tmp, sizeof tmp, specbuf, cl_as_num(v));
                APPEND_S(tmp);
                break;
            case 's': {
                const char *s = NULL;
                char numbuf[64];
                if (cl_is_str(v)) {
                    s = cl_as_str(v)->data;
                } else if (cl_is_num(v)) {
                    snprintf(numbuf, sizeof numbuf, "%.10g", cl_as_num(v));
                    s = numbuf;
                } else {
                    s = "?";
                }
                specbuf[si++] = 's'; specbuf[si] = '\0';
                size_t need = strlen(s) + 32;
                char *t2 = (char *)malloc(need);
                if (!t2) { free(buf); cl_die_rt("fmt: out of memory"); }
                snprintf(t2, need, specbuf, s);
                APPEND_S(t2);
                free(t2);
                break;
            }
            case 'c':
                if (cl_is_num(v)) APPEND_C((char)(int)cl_as_num(v));
                else if (cl_is_str(v)) {
                    CalcStr *cs = cl_as_str(v);
                    if (cs->len > 0) APPEND_C(cs->data[0]);
                } else { free(buf); cl_die_rt("fmt: %c expects num or str"); }
                break;
            default:
                free(buf);
                cl_die_rt("fmt: unknown format specifier");
        }
    }
    Value out = cl_new_str(buf, len);
    free(buf);
    free(normalized);
    return out;
    #undef APPEND_C
    #undef APPEND_S
}

/* --- Math + transcendental calclib ------------------------------ */

Value cl_builtin_sqrt(Value v) {
    require_num(v, "sqrt");
    double x = cl_as_num(v);
    if (x < 0) cl_die_rt("sqrt: negative argument");
    return cl_from_num(sqrt(x));
}
Value cl_builtin_floor(Value v) { require_num(v, "floor"); return cl_from_num(floor(cl_as_num(v))); }
Value cl_builtin_ceil (Value v) { require_num(v, "ceil");  return cl_from_num(ceil (cl_as_num(v))); }
Value cl_builtin_abs(Value v) {
    if (cl_is_cpx(v)) {
        CalcCpx *c = cl_as_cpx(v);
        return cl_from_num(hypot(c->re, c->im));
    }
    require_num(v, "abs");
    return cl_from_num(fabs(cl_as_num(v)));
}
Value cl_builtin_int  (Value v) { require_num(v, "int");   return cl_from_num(trunc(cl_as_num(v))); }
Value cl_builtin_round(Value v) { require_num(v, "round"); return cl_from_num(round(cl_as_num(v))); }

Value cl_builtin_pow(Value a, Value b) {
    require_num(a, "pow"); require_num(b, "pow");
    return cl_from_num(pow(cl_as_num(a), cl_as_num(b)));
}
Value cl_builtin_min(Value a, Value b) {
    require_num(a, "min"); require_num(b, "min");
    double x = cl_as_num(a), y = cl_as_num(b);
    return cl_from_num(x < y ? x : y);
}
Value cl_builtin_max(Value a, Value b) {
    require_num(a, "max"); require_num(b, "max");
    double x = cl_as_num(a), y = cl_as_num(b);
    return cl_from_num(x > y ? x : y);
}

Value cl_builtin_sin(Value v)  { require_num(v, "sin");  return cl_from_num(sin (cl_as_num(v))); }
Value cl_builtin_cos(Value v)  { require_num(v, "cos");  return cl_from_num(cos (cl_as_num(v))); }
Value cl_builtin_tan(Value v)  { require_num(v, "tan");  return cl_from_num(tan (cl_as_num(v))); }
Value cl_builtin_asin(Value v) {
    require_num(v, "asin");
    double x = cl_as_num(v);
    if (x < -1.0 || x > 1.0) cl_die_rt("asin: argument outside [-1, 1]");
    return cl_from_num(asin(x));
}
Value cl_builtin_acos(Value v) {
    require_num(v, "acos");
    double x = cl_as_num(v);
    if (x < -1.0 || x > 1.0) cl_die_rt("acos: argument outside [-1, 1]");
    return cl_from_num(acos(x));
}
Value cl_builtin_atan(Value v) { require_num(v, "atan"); return cl_from_num(atan(cl_as_num(v))); }
Value cl_builtin_atan2(Value y, Value x) {
    require_num(y, "atan2"); require_num(x, "atan2");
    return cl_from_num(atan2(cl_as_num(y), cl_as_num(x)));
}
Value cl_builtin_exp(Value v) { require_num(v, "exp"); return cl_from_num(exp(cl_as_num(v))); }
Value cl_builtin_log(Value v) {
    require_num(v, "log");
    double x = cl_as_num(v);
    if (x <= 0.0) cl_die_rt("log: non-positive argument");
    return cl_from_num(log(x));
}
Value cl_builtin_log10(Value v) {
    require_num(v, "log10");
    double x = cl_as_num(v);
    if (x <= 0.0) cl_die_rt("log10: non-positive argument");
    return cl_from_num(log10(x));
}

/* random() returns a uniform [0, 1). Seeded on first call. */
Value cl_builtin_random(void) {
    static int seeded = 0;
    if (!seeded) { srand((unsigned)1); seeded = 1; }
    /* rand() yields [0, RAND_MAX]; divide for [0, 1). */
    return cl_from_num((double)rand() / ((double)RAND_MAX + 1.0));
}
Value cl_builtin_pi(void) { return cl_from_num(3.141592653589793); }
Value cl_builtin_e (void) { return cl_from_num(2.718281828459045); }

/* --- I/O --------------------------------------------------------- */

Value cl_builtin_read_line(void) {
    char *buf = NULL;
    size_t cap = 0, len = 0;
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (c == '\r') continue;        /* strip CR for cross-platform parity */
        if (len + 1 >= cap) {
            cap = cap ? cap * 2 : 64;
            buf = (char *)realloc(buf, cap);
            if (!buf) cl_die_rt("out of memory");
        }
        buf[len++] = (char)c;
    }
    Value v = cl_new_str(buf ? buf : "", len);
    free(buf);
    return v;
}

/* write(x): print without newline, dispatching on tag exactly like
   cl_print but without the trailing '\n'. Returns 0. */
Value cl_builtin_write(Value v) {
    format_value(v, 0);
    return cl_from_num(0.0);
}

/* --- str_split / str_join (cross-type bridge) -------------------- */

Value cl_builtin_str_split(Value sv, Value sepv) {
    require_str(sv,  "str_split");
    require_str(sepv, "str_split");
    CalcStr *s   = cl_as_str(sv);
    CalcStr *sep = cl_as_str(sepv);
    Value out = cl_new_arr();
    CalcArr *a = cl_as_arr(out);
    if (sep->len == 0) {
        /* Edge case: empty separator -> each char its own string,
           matching the VM. */
        for (uint64_t i = 0; i < s->len; i++) {
            arr_grow(a, a->len + 1);
            a->items[a->len++] = cl_new_str(&s->data[i], 1);
        }
        return out;
    }
    uint64_t start = 0;
    for (uint64_t i = 0; i + sep->len <= s->len; ) {
        if (memcmp(&s->data[i], sep->data, (size_t)sep->len) == 0) {
            arr_grow(a, a->len + 1);
            a->items[a->len++] = cl_new_str(&s->data[start], i - start);
            i += sep->len;
            start = i;
        } else {
            i++;
        }
    }
    arr_grow(a, a->len + 1);
    a->items[a->len++] = cl_new_str(&s->data[start], s->len - start);
    return out;
}

Value cl_builtin_str_join(Value arrv, Value sepv) {
    require_arr(arrv, "str_join");
    require_str(sepv, "str_join");
    CalcArr *a   = cl_as_arr(arrv);
    CalcStr *sep = cl_as_str(sepv);
    /* Validate all items are strings, sum lengths. */
    uint64_t total = 0;
    for (uint64_t i = 0; i < a->len; i++) {
        if (!cl_is_str(a->items[i])) cl_die_rt("str_join: non-string element");
        total += cl_as_str(a->items[i])->len;
    }
    if (a->len > 1) total += sep->len * (a->len - 1);
    CalcStr *out = (CalcStr *)cl_alloc(sizeof(CalcStr) + total + 1, GC_STR);
    out->len = total;
    uint64_t p = 0;
    for (uint64_t i = 0; i < a->len; i++) {
        if (i > 0) {
            memcpy(out->data + p, sep->data, (size_t)sep->len);
            p += sep->len;
        }
        CalcStr *si = cl_as_str(a->items[i]);
        memcpy(out->data + p, si->data, (size_t)si->len);
        p += si->len;
    }
    out->data[total] = '\0';
    return cl_make_tagged(CL_TAG_STR, out);
}

/* --- File I/O ---------------------------------------------------- */

Value cl_builtin_file_read(Value pathv) {
    require_str(pathv, "file_read");
    CalcStr *p = cl_as_str(pathv);
    FILE *f = fopen(p->data, "rb");
    if (!f) {
        fprintf(stderr, "runtime error: file_read: cannot open '%s'\n", p->data);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); cl_die_rt("file_read: bad length"); }
    CalcStr *s = (CalcStr *)cl_alloc(sizeof(CalcStr) + (size_t)n + 1, GC_STR);
    s->len = (uint64_t)n;
    size_t got = fread(s->data, 1, (size_t)n, f);
    s->data[got] = '\0';
    s->len = (uint64_t)got;
    fclose(f);
    return cl_make_tagged(CL_TAG_STR, s);
}

static Value file_write_or_append(Value pathv, Value contentv, const char *mode, const char *fn) {
    require_str(pathv,    fn);
    require_str(contentv, fn);
    CalcStr *p = cl_as_str(pathv);
    CalcStr *c = cl_as_str(contentv);
    FILE *f = fopen(p->data, mode);
    if (!f) {
        fprintf(stderr, "runtime error: %s: cannot open '%s'\n", fn, p->data);
        exit(1);
    }
    if (c->len) {
        size_t w = fwrite(c->data, 1, (size_t)c->len, f);
        if (w != c->len) { fclose(f); cl_die_rt("file write: short write"); }
    }
    fclose(f);
    return cl_from_num(0.0);
}

Value cl_builtin_file_write(Value pathv, Value contentv) {
    return file_write_or_append(pathv, contentv, "wb", "file_write");
}
Value cl_builtin_file_append(Value pathv, Value contentv) {
    return file_write_or_append(pathv, contentv, "ab", "file_append");
}

Value cl_builtin_file_exists(Value pathv) {
    require_str(pathv, "file_exists");
    CalcStr *p = cl_as_str(pathv);
    FILE *f = fopen(p->data, "rb");
    if (f) { fclose(f); return cl_from_num(1.0); }
    return cl_from_num(0.0);
}

/* --- system(cmd) ------------------------------------------------- */

Value cl_builtin_system(Value cmdv) {
    require_str(cmdv, "system");
    CalcStr *c = cl_as_str(cmdv);
    /* Flush our buffered stdout so any CalcLang output already
       produced shows up before the child process's output. */
    fflush(stdout);
    fflush(stderr);
    int rc = system(c->data);
    return cl_from_num((double)rc);
}

/* --- Complex calclib --------------------------------------------- */

Value cl_builtin_complex(Value re, Value im) {
    require_num(re, "complex");
    require_num(im, "complex");
    return cpx_new(cl_as_num(re), cl_as_num(im));
}

Value cl_builtin_real(Value v) {
    if (cl_is_num(v)) return v;
    if (cl_is_cpx(v)) return cl_from_num(cl_as_cpx(v)->re);
    cl_die_rt("real: requires num or cpx");
    return (Value)0;
}

Value cl_builtin_imag(Value v) {
    if (cl_is_num(v)) return cl_from_num(0.0);
    if (cl_is_cpx(v)) return cl_from_num(cl_as_cpx(v)->im);
    cl_die_rt("imag: requires num or cpx");
    return (Value)0;
}

Value cl_builtin_conj(Value v) {
    if (cl_is_num(v)) return v;
    if (cl_is_cpx(v)) {
        CalcCpx *c = cl_as_cpx(v);
        return cpx_new(c->re, -c->im);
    }
    cl_die_rt("conj: requires num or cpx");
    return (Value)0;
}

Value cl_builtin_arg(Value v) {
    if (cl_is_num(v)) {
        double x = cl_as_num(v);
        return cl_from_num(x < 0 ? 3.141592653589793 : 0.0);
    }
    if (cl_is_cpx(v)) {
        CalcCpx *c = cl_as_cpx(v);
        return cl_from_num(atan2(c->im, c->re));
    }
    cl_die_rt("arg: requires num or cpx");
    return (Value)0;
}

/* --- FFI (dynamic linking) -------------------------------------- */

static void *ffi_platform_load(const char *path) {
#ifdef _WIN32
    /* Empty path -> the running executable. */
    if (!path || !*path) return GetModuleHandleA(NULL);
    return LoadLibraryA(path);
#else
    if (!path || !*path) return dlopen(NULL, RTLD_LAZY | RTLD_GLOBAL);
    return dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
#endif
}

static void *ffi_platform_sym(void *lib, const char *name) {
#ifdef _WIN32
    return GetProcAddress(lib, name);
#else
    return dlsym(lib, name);
#endif
}

Value cl_builtin_ffi_load(Value pathv) {
    require_str(pathv, "ffi_load");
    CalcStr *p = cl_as_str(pathv);
    void *h = ffi_platform_load(p->data);
    if (!h) {
        fprintf(stderr, "runtime error: ffi_load: cannot load '%s'\n", p->data);
        exit(1);
    }
    /* Pointer fits in a double exactly for any practical address. */
    return cl_from_num((double)(uintptr_t)h);
}

/* Marshal a single argument from a CalcLang Value to the C
   representation hinted by `kind`. The result either goes in `*as_d`
   (for `d`) or `*as_i` (for `i`/`s`/pointer-like). The two routes
   keep the integer/float register selection clean for the dispatch
   table below. */
static void ffi_marshal_in(Value v, char kind, double *as_d, uintptr_t *as_i) {
    switch (kind) {
        case 'd':
            if (!cl_is_num(v)) cl_die_rt("ffi_call: arg type 'd' expects num");
            *as_d = cl_as_num(v);
            return;
        case 'i':
            if (!cl_is_num(v)) cl_die_rt("ffi_call: arg type 'i' expects num");
            *as_i = (uintptr_t)(int64_t)cl_as_num(v);
            return;
        case 's':
            if (!cl_is_str(v)) cl_die_rt("ffi_call: arg type 's' expects str");
            *as_i = (uintptr_t)cl_as_str(v)->data;
            return;
        default:
            cl_die_rt("ffi_call: unsupported arg kind in signature");
    }
}

/* For a given `sig` like "d:dd", separate into return-kind and args. */
static int ffi_parse_sig(const char *sig, char *ret_out, char arg_kinds[8], int *n_args) {
    if (!sig || !sig[0] || sig[1] != ':') return 0;
    *ret_out = sig[0];
    const char *p = sig + 2;
    int n = 0;
    while (*p) {
        if (n >= 7) return 0;
        arg_kinds[n++] = *p++;
    }
    *n_args = n;
    return 1;
}

Value cl_builtin_ffi_call(Value libv, Value namev, Value sigv, Value argsv) {
    require_num(libv,  "ffi_call");
    require_str(namev, "ffi_call");
    require_str(sigv,  "ffi_call");
    require_arr(argsv, "ffi_call");
    void *lib = (void *)(uintptr_t)cl_as_num(libv);
    CalcArr *args = cl_as_arr(argsv);

    char ret_kind;
    char arg_kinds[8];
    int  n_args = 0;
    if (!ffi_parse_sig(cl_as_str(sigv)->data, &ret_kind, arg_kinds, &n_args)) {
        cl_die_rt("ffi_call: malformed signature");
    }
    if ((int)args->len != n_args) {
        cl_die_rt("ffi_call: arg count doesn't match signature");
    }

    void *fn = ffi_platform_sym(lib, cl_as_str(namev)->data);
    if (!fn) {
        fprintf(stderr, "runtime error: ffi_call: cannot find '%s'\n",
            cl_as_str(namev)->data);
        exit(1);
    }

    /* Marshal up to 4 args. The MS x64 ABI passes the first 4 args
       in RCX/RDX/R8/R9 for ints/pointers and XMM0-3 for floats. We
       precompute both register slots and pass the same values into
       all signatures below — the compiler picks whichever the
       function-pointer prototype expects. */
    double d0=0, d1=0, d2=0, d3=0;
    uintptr_t i0=0, i1=0, i2=0, i3=0;
    for (int k = 0; k < n_args; k++) {
        double *dp;
        uintptr_t *ip;
        switch (k) {
            case 0: dp = &d0; ip = &i0; break;
            case 1: dp = &d1; ip = &i1; break;
            case 2: dp = &d2; ip = &i2; break;
            case 3: dp = &d3; ip = &i3; break;
            default: cl_die_rt("ffi_call: more than 4 args not supported");
        }
        ffi_marshal_in(args->items[k], arg_kinds[k], dp, ip);
    }

    /* Dispatch table. We enumerate the common signatures explicitly
       rather than dragging in libffi. Each branch types the fn
       pointer correctly so the compiler emits proper arg setup. */
    /* Build a compact key: e.g. "d:dd" -> ret_kind='d', arg_kinds="dd". */

    #define SIG_IS(r, args_str)  (ret_kind == (r) && n_args == (int)strlen(args_str) && \
                                  memcmp(arg_kinds, args_str, strlen(args_str)) == 0)

    if (SIG_IS('d', "")) {
        typedef double (*F)(void);
        return cl_from_num(((F)fn)());
    }
    if (SIG_IS('d', "d")) {
        typedef double (*F)(double);
        return cl_from_num(((F)fn)(d0));
    }
    if (SIG_IS('d', "dd")) {
        typedef double (*F)(double, double);
        return cl_from_num(((F)fn)(d0, d1));
    }
    if (SIG_IS('d', "ddd")) {
        typedef double (*F)(double, double, double);
        return cl_from_num(((F)fn)(d0, d1, d2));
    }
    if (SIG_IS('i', "")) {
        typedef int (*F)(void);
        return cl_from_num((double)((F)fn)());
    }
    if (SIG_IS('i', "i")) {
        typedef int (*F)(int);
        return cl_from_num((double)((F)fn)((int)i0));
    }
    if (SIG_IS('i', "s")) {
        typedef int (*F)(const char *);
        return cl_from_num((double)((F)fn)((const char *)i0));
    }
    if (SIG_IS('i', "ss")) {
        typedef int (*F)(const char *, const char *);
        return cl_from_num((double)((F)fn)((const char *)i0, (const char *)i1));
    }
    if (SIG_IS('s', "")) {
        typedef const char *(*F)(void);
        const char *r = ((F)fn)();
        return cl_new_str(r ? r : "", r ? strlen(r) : 0);
    }
    if (SIG_IS('s', "s")) {
        typedef const char *(*F)(const char *);
        const char *r = ((F)fn)((const char *)i0);
        return cl_new_str(r ? r : "", r ? strlen(r) : 0);
    }
    if (SIG_IS('s', "i")) {
        typedef const char *(*F)(int);
        const char *r = ((F)fn)((int)i0);
        return cl_new_str(r ? r : "", r ? strlen(r) : 0);
    }
    if (SIG_IS('v', "")) {
        typedef void (*F)(void);
        ((F)fn)();
        return cl_from_num(0.0);
    }
    if (SIG_IS('v', "s")) {
        typedef void (*F)(const char *);
        ((F)fn)((const char *)i0);
        return cl_from_num(0.0);
    }
    if (SIG_IS('v', "i")) {
        typedef void (*F)(int);
        ((F)fn)((int)i0);
        return cl_from_num(0.0);
    }

    fprintf(stderr, "runtime error: ffi_call: unsupported signature '%s'\n",
        cl_as_str(sigv)->data);
    exit(1);
    #undef SIG_IS
}
