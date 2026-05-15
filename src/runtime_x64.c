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
  __declspec(dllimport) void   __stdcall Sleep(unsigned long);
  /* Console for terminal-mode (VT escapes + raw key reads). */
  __declspec(dllimport) void *__stdcall GetStdHandle(unsigned long);
  __declspec(dllimport) int    __stdcall GetConsoleMode(void *, unsigned long *);
  __declspec(dllimport) int    __stdcall SetConsoleMode(void *, unsigned long);
  #define CL_STD_OUTPUT_HANDLE         ((unsigned long)-11)
  #define CL_ENABLE_VIRTUAL_TERMINAL   0x0004
  /* conio.h prototypes — pulled in directly so we don't need the header. */
  int _kbhit(void);
  int _getch(void);
#else
  #include <unistd.h>     /* usleep */
  #include <termios.h>
  #include <sys/select.h>
  #include <fcntl.h>
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
/* Externally visible so addon runtime modules (e.g. runtime_gui_sdl2.c)
   can use the same error-reporting + type-checking helpers without
   duplicating them. */
void cl_die_rt(const char *msg) {
    fprintf(stderr, "runtime error: %s\n", msg);
    exit(1);
}

void require_num(Value v, const char *where) {
    if (!cl_is_num(v)) {
        fprintf(stderr, "runtime error: %s expected num, got non-num\n", where);
        exit(1);
    }
}
void require_str(Value v, const char *where) {
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

/* --- Terminal-mode helpers (sleep_ms, read_key, time_ms) --------- */

/* time_ms(): wall-clock milliseconds since program start. Useful for
   game-loop pacing and benchmarks. Returns a num. The reference is
   captured on first call so subsequent values fit comfortably in a
   double's precision range. */
#ifdef _WIN32
  __declspec(dllimport) unsigned long __stdcall GetTickCount(void);
  static unsigned long cl_t0_ms = 0;
  static int cl_t0_init = 0;
#else
  #include <sys/time.h>
  static double cl_t0_ms = 0;
  static int    cl_t0_init = 0;
#endif

Value cl_builtin_time_ms(void) {
#ifdef _WIN32
    unsigned long now = GetTickCount();
    if (!cl_t0_init) { cl_t0_ms = now; cl_t0_init = 1; }
    return cl_from_num((double)(now - cl_t0_ms));
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    double now = (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
    if (!cl_t0_init) { cl_t0_ms = now; cl_t0_init = 1; }
    return cl_from_num(now - cl_t0_ms);
#endif
}

/* --- Wall-clock date/time --------------------------------------- */

/* epoch_ms(): wall-clock milliseconds since the Unix epoch
   (1970-01-01 00:00:00 UTC). Distinct from time_ms (which is a
   process-start-relative monotonic counter). */
#ifdef _WIN32
__declspec(dllimport) void __stdcall GetSystemTimeAsFileTime(void *);
#endif

Value cl_builtin_epoch_ms(void) {
#ifdef _WIN32
    /* FILETIME = 100-ns intervals since 1601-01-01 UTC. Unix epoch
       is 11644473600 s = 116444736000000000 100-ns ticks later. */
    union { unsigned long long u; struct { unsigned long lo, hi; } p; } ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t100ns = ft.u;
    unsigned long long ms = (t100ns - 116444736000000000ULL) / 10000ULL;
    return cl_from_num((double)ms);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return cl_from_num((double)tv.tv_sec * 1000.0 +
                       (double)tv.tv_usec / 1000.0);
#endif
}

#include <time.h>

/* Cross-platform UTC component extraction. Fills *out with broken-down
   time from a millisecond-precision epoch value. Returns 0 on success. */
static int components_from_ms(double ms, struct tm *out) {
    time_t seconds = (time_t)(ms / 1000.0);
#ifdef _WIN32
    /* gmtime_s has reversed arg order on Windows. */
    if (gmtime_s(out, &seconds) != 0) return -1;
#else
    if (gmtime_r(&seconds, out) == NULL) return -1;
#endif
    return 0;
}

/* time_components(ms) — break a wall-clock ms into broken-down UTC
   fields. Returns an array of 7 numbers:
     [year, month (1-12), day (1-31), hour (0-23), minute (0-59),
      second (0-59), weekday (0=Sun..6=Sat)]
   Sub-second precision (ms within the second) is preserved by the
   caller — we just floor to whole seconds for component extraction. */
Value cl_builtin_time_components(Value ms_v) {
    require_num(ms_v, "time_components");
    double ms = cl_as_num(ms_v);
    struct tm t;
    if (components_from_ms(ms, &t) != 0) {
        cl_die_rt("time_components: invalid timestamp");
    }
    Value a = cl_new_arr();
    cl_arr_push(a, cl_from_num((double)(t.tm_year + 1900)));
    cl_arr_push(a, cl_from_num((double)(t.tm_mon + 1)));
    cl_arr_push(a, cl_from_num((double)t.tm_mday));
    cl_arr_push(a, cl_from_num((double)t.tm_hour));
    cl_arr_push(a, cl_from_num((double)t.tm_min));
    cl_arr_push(a, cl_from_num((double)t.tm_sec));
    cl_arr_push(a, cl_from_num((double)t.tm_wday));
    return a;
}

/* time_make(year, month, day) — UTC midnight of the given date, as
   epoch milliseconds. month is 1-12, day is 1-31. */
Value cl_builtin_time_make(Value yv, Value mv, Value dv) {
    require_num(yv, "time_make");
    require_num(mv, "time_make");
    require_num(dv, "time_make");
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = (int)cl_as_num(yv) - 1900;
    t.tm_mon  = (int)cl_as_num(mv) - 1;
    t.tm_mday = (int)cl_as_num(dv);
    t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0;
#ifdef _WIN32
    time_t s = _mkgmtime(&t);
#else
    /* timegm is GNU-extension. The portable workaround is to set the
       TZ env to UTC and use mktime, but that's invasive — most libcs
       expose timegm under glibc and BSD. Fall back to a manual day
       count if timegm isn't available. */
    time_t s = timegm(&t);
#endif
    if (s == (time_t)-1) {
        cl_die_rt("time_make: invalid date");
    }
    return cl_from_num((double)s * 1000.0);
}

/* time_format(ms, fmt) — format an epoch_ms value as a string using
   strftime. The format string follows the standard:
     %Y year (4 digits)   %m month (01-12)   %d day (01-31)
     %H hour (00-23)      %M minute (00-59)  %S second (00-59)
     %a abbrev weekday    %A full weekday
     %B full month        %b abbrev month
     %j day of year       %Z timezone name
   See strftime(3) for the full set. Everything renders in UTC. */
Value cl_builtin_time_format(Value ms_v, Value fmt_v) {
    require_num(ms_v, "time_format");
    require_str(fmt_v, "time_format");
    double ms = cl_as_num(ms_v);
    struct tm t;
    if (components_from_ms(ms, &t) != 0) {
        cl_die_rt("time_format: invalid timestamp");
    }
    char buf[512];
    size_t n = strftime(buf, sizeof buf, cl_as_str(fmt_v)->data, &t);
    if (n == 0 && cl_as_str(fmt_v)->len > 0) {
        /* strftime returns 0 on buffer-too-small OR an empty format.
           For our purposes treat both as success with empty output. */
        n = 0;
    }
    return cl_new_str(buf, (uint64_t)n);
}

/* sleep_ms(n): pause for n milliseconds. Returns 0. */
Value cl_builtin_sleep_ms(Value v) {
    require_num(v, "sleep_ms");
    double ms = cl_as_num(v);
    if (ms < 0) ms = 0;
    /* Flush output so the user sees the previous frame before we sleep. */
    fflush(stdout);
#ifdef _WIN32
    Sleep((unsigned long)ms);
#else
    /* usleep takes microseconds; clamp to its range. */
    if (ms > 1000000.0 * 60.0) ms = 1000000.0 * 60.0;
    usleep((unsigned int)(ms * 1000));
#endif
    return cl_from_num(0.0);
}

#ifdef _WIN32
/* One-time VT-mode enable so ANSI escapes ("\e[2J", "\e[H", colors)
   actually take effect on Windows Terminal / cmd.exe. Modern Windows
   terminals support it but it's off by default. */
static void cl_enable_vt_mode_once(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    void *h = GetStdHandle(CL_STD_OUTPUT_HANDLE);
    unsigned long mode = 0;
    if (!GetConsoleMode(h, &mode)) return;   /* not a console — leave alone */
    SetConsoleMode(h, mode | CL_ENABLE_VIRTUAL_TERMINAL);
}
#else
/* On POSIX we set the terminal to raw mode the first time read_key runs,
   and restore it via atexit so the user's shell isn't left broken. */
static struct termios cl_saved_termios;
static int cl_termios_saved = 0;
static void cl_restore_termios(void) {
    if (cl_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &cl_saved_termios);
    }
}
static void cl_enable_raw_once(void) {
    if (cl_termios_saved) return;
    if (tcgetattr(STDIN_FILENO, &cl_saved_termios) != 0) return;
    cl_termios_saved = 1;
    atexit(cl_restore_termios);
    struct termios raw = cl_saved_termios;
    raw.c_lflag &= ~(unsigned long)(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}
#endif

/* read_key(): non-blocking single-key read.
   Returns:
     -1   if no key is pressed
     0-255 for normal ASCII keys ('a', ' ', '\n', etc.)
     1001  Arrow Up
     1002  Arrow Down
     1003  Arrow Left
     1004  Arrow Right
     2000+ for other "special" Windows keys (F-keys etc.); avoid
           relying on the exact code. */
Value cl_builtin_read_key(void) {
#ifdef _WIN32
    cl_enable_vt_mode_once();
    if (!_kbhit()) return cl_from_num(-1.0);
    int c = _getch();
    if (c == 0 || c == 0xE0) {
        /* Special key: next byte is the actual scan code. */
        int c2 = _getch();
        switch (c2) {
            case 72: return cl_from_num(1001.0);   /* up */
            case 80: return cl_from_num(1002.0);   /* down */
            case 75: return cl_from_num(1003.0);   /* left */
            case 77: return cl_from_num(1004.0);   /* right */
            default: return cl_from_num(2000.0 + (double)c2);
        }
    }
    return cl_from_num((double)c);
#else
    cl_enable_raw_once();
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return cl_from_num(-1.0);
    if (c == 0x1B) {
        /* Escape sequence: ESC [ X. Read with short timeout. */
        unsigned char b1, b2;
        ssize_t n1 = read(STDIN_FILENO, &b1, 1);
        if (n1 <= 0) return cl_from_num(27.0);    /* bare ESC */
        if (b1 != '[') return cl_from_num(27.0);
        ssize_t n2 = read(STDIN_FILENO, &b2, 1);
        if (n2 <= 0) return cl_from_num(27.0);
        switch (b2) {
            case 'A': return cl_from_num(1001.0); /* up */
            case 'B': return cl_from_num(1002.0); /* down */
            case 'D': return cl_from_num(1003.0); /* left */
            case 'C': return cl_from_num(1004.0); /* right */
            default:  return cl_from_num(2000.0 + (double)b2);
        }
    }
    return cl_from_num((double)c);
#endif
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

/* --- Regex builtins --------------------------------------------- */

#include "regex.h"

/* For each pattern we compile we cache it keyed on (pattern, flags)
   so the same pattern used repeatedly (find_all-in-a-loop) doesn't
   recompile every call. Cache evicts oldest when full. */
typedef struct {
    char       *pat;        /* heap copy of the source pattern */
    int         flags;
    CalcRegex  *re;
} RegexCacheEntry;

#define RE_CACHE_MAX 32
static RegexCacheEntry re_cache[RE_CACHE_MAX];
static int             re_cache_n = 0;
static int             re_cache_next_evict = 0;

static CalcRegex *re_get_or_compile(const char *pat, int flags) {
    for (int i = 0; i < re_cache_n; i++) {
        if (re_cache[i].flags == flags && strcmp(re_cache[i].pat, pat) == 0) {
            return re_cache[i].re;
        }
    }
    CalcRegex *r = cl_regex_compile(pat, flags);
    if (!r) {
        cl_die_rt(cl_regex_error());
    }
    if (re_cache_n < RE_CACHE_MAX) {
        re_cache[re_cache_n].pat   = strdup(pat);
        re_cache[re_cache_n].flags = flags;
        re_cache[re_cache_n].re    = r;
        re_cache_n++;
    } else {
        /* Evict oldest. */
        int i = re_cache_next_evict;
        re_cache_next_evict = (re_cache_next_evict + 1) % RE_CACHE_MAX;
        free(re_cache[i].pat);
        cl_regex_free(re_cache[i].re);
        re_cache[i].pat   = strdup(pat);
        re_cache[i].flags = flags;
        re_cache[i].re    = r;
    }
    return r;
}

/* regex_match(pattern, text) — returns 1 if any match exists in text,
   0 otherwise. Equivalent to `re.search` in Python (NOT anchored to
   the start; use ^ if you want that). */
Value cl_builtin_regex_match(Value pat_v, Value text_v) {
    require_str(pat_v,  "regex_match");
    require_str(text_v, "regex_match");
    CalcStr *p = cl_as_str(pat_v);
    CalcStr *t = cl_as_str(text_v);
    CalcRegex *r = re_get_or_compile(p->data, 0);
    int ms, me;
    int matched = cl_regex_match(r, t->data, (int)t->len, 0, &ms, &me,
                                  NULL, NULL, 0, NULL);
    return cl_from_num(matched ? 1.0 : 0.0);
}

/* regex_find(pattern, text) — returns a map describing the first match:
     {"start": int, "end": int, "match": str, "groups": [str, str, ...]}
   Or returns the empty string "" if no match. (Returning a Value-less
   "null" is awkward without a tag; using "" is a common idiom in
   CalcLang scripts.) */
Value cl_builtin_regex_find(Value pat_v, Value text_v) {
    require_str(pat_v,  "regex_find");
    require_str(text_v, "regex_find");
    CalcStr *p = cl_as_str(pat_v);
    CalcStr *t = cl_as_str(text_v);
    CalcRegex *r = re_get_or_compile(p->data, 0);
    int ms, me;
    int cs[RE_CACHE_MAX], ce[RE_CACHE_MAX], nc = 0;
    /* (RE_CACHE_MAX is unrelated — just reusing a large enough constant.) */
    if (!cl_regex_match(r, t->data, (int)t->len, 0, &ms, &me, cs, ce, RE_CACHE_MAX, &nc)) {
        return cl_new_str("", 0);
    }
    Value m = cl_new_map();
    cl_index_set(m, cl_new_str("start", 5), cl_from_num((double)ms));
    cl_index_set(m, cl_new_str("end",   3), cl_from_num((double)me));
    cl_index_set(m, cl_new_str("match", 5), cl_new_str(t->data + ms, (uint64_t)(me - ms)));
    Value groups = cl_new_arr();
    for (int i = 1; i < nc; i++) {
        if (cs[i] >= 0) {
            cl_arr_push(groups, cl_new_str(t->data + cs[i], (uint64_t)(ce[i] - cs[i])));
        } else {
            cl_arr_push(groups, cl_new_str("", 0));
        }
    }
    cl_index_set(m, cl_new_str("groups", 6), groups);
    return m;
}

/* regex_find_all(pattern, text) — returns an array of all
   non-overlapping matches. Each entry is the same map shape as
   regex_find. Empty array on no matches. */
Value cl_builtin_regex_find_all(Value pat_v, Value text_v) {
    require_str(pat_v,  "regex_find_all");
    require_str(text_v, "regex_find_all");
    CalcStr *p = cl_as_str(pat_v);
    CalcStr *t = cl_as_str(text_v);
    CalcRegex *r = re_get_or_compile(p->data, 0);
    Value out = cl_new_arr();
    int pos = 0;
    while (pos <= (int)t->len) {
        int ms, me;
        int cs[32], ce[32], nc = 0;
        if (!cl_regex_match(r, t->data, (int)t->len, pos, &ms, &me, cs, ce, 32, &nc)) break;
        Value m = cl_new_map();
        cl_index_set(m, cl_new_str("start", 5), cl_from_num((double)ms));
        cl_index_set(m, cl_new_str("end",   3), cl_from_num((double)me));
        cl_index_set(m, cl_new_str("match", 5), cl_new_str(t->data + ms, (uint64_t)(me - ms)));
        Value groups = cl_new_arr();
        for (int i = 1; i < nc; i++) {
            if (cs[i] >= 0) {
                cl_arr_push(groups, cl_new_str(t->data + cs[i], (uint64_t)(ce[i] - cs[i])));
            } else {
                cl_arr_push(groups, cl_new_str("", 0));
            }
        }
        cl_index_set(m, cl_new_str("groups", 6), groups);
        cl_arr_push(out, m);
        /* Advance past the match; if match was zero-width, step by 1
           to avoid an infinite loop. */
        pos = (me > ms) ? me : ms + 1;
    }
    return out;
}

/* regex_replace(pattern, text, replacement) — replace every match
   with `replacement`. The replacement string may contain $0 (whole
   match) and $1..$N (captured groups) as substitution references.
   $$ produces a literal $. */
Value cl_builtin_regex_replace(Value pat_v, Value text_v, Value rep_v) {
    require_str(pat_v,  "regex_replace");
    require_str(text_v, "regex_replace");
    require_str(rep_v,  "regex_replace");
    CalcStr *p = cl_as_str(pat_v);
    CalcStr *t = cl_as_str(text_v);
    CalcStr *rep = cl_as_str(rep_v);
    CalcRegex *r = re_get_or_compile(p->data, 0);

    /* Build into a growable buffer. */
    size_t cap = t->len + 64;
    char  *buf = (char *)malloc(cap);
    size_t blen = 0;
    if (!buf) cl_die_rt("regex_replace: oom");

    int pos = 0;
    int cs[32], ce[32], nc = 0;
    while (pos <= (int)t->len) {
        int ms, me;
        if (!cl_regex_match(r, t->data, (int)t->len, pos, &ms, &me, cs, ce, 32, &nc)) break;
        /* Append everything before the match. */
        size_t pre = (size_t)(ms - pos);
        if (blen + pre + 1 > cap) { cap = (blen + pre) * 2 + 64; buf = realloc(buf, cap); }
        memcpy(buf + blen, t->data + pos, pre); blen += pre;
        /* Expand replacement with $N substitutions. */
        for (uint64_t i = 0; i < rep->len; i++) {
            char rc = rep->data[i];
            if (rc == '$' && i + 1 < rep->len) {
                char nc1 = rep->data[i + 1];
                if (nc1 == '$') {
                    if (blen + 1 + 1 > cap) { cap = blen * 2 + 64; buf = realloc(buf, cap); }
                    buf[blen++] = '$';
                    i++;
                    continue;
                }
                if (nc1 >= '0' && nc1 <= '9') {
                    int idx = nc1 - '0';
                    if (idx < nc && cs[idx] >= 0) {
                        int n = ce[idx] - cs[idx];
                        if (blen + (size_t)n + 1 > cap) { cap = (blen + (size_t)n) * 2 + 64; buf = realloc(buf, cap); }
                        memcpy(buf + blen, t->data + cs[idx], (size_t)n);
                        blen += (size_t)n;
                    }
                    i++;
                    continue;
                }
            }
            if (blen + 1 + 1 > cap) { cap = blen * 2 + 64; buf = realloc(buf, cap); }
            buf[blen++] = rc;
        }
        pos = (me > ms) ? me : ms + 1;
    }
    /* Append the rest. */
    size_t tail = (size_t)((int)t->len - pos);
    if (blen + tail + 1 > cap) { cap = (blen + tail) * 2 + 64; buf = realloc(buf, cap); }
    memcpy(buf + blen, t->data + pos, tail); blen += tail;
    buf[blen] = '\0';

    Value v = cl_new_str(buf, (uint64_t)blen);
    free(buf);
    return v;
}

/* regex_split(pattern, text) — split text on every match of pattern.
   Returns array of strings (the segments BETWEEN matches). */
Value cl_builtin_regex_split(Value pat_v, Value text_v) {
    require_str(pat_v,  "regex_split");
    require_str(text_v, "regex_split");
    CalcStr *p = cl_as_str(pat_v);
    CalcStr *t = cl_as_str(text_v);
    CalcRegex *r = re_get_or_compile(p->data, 0);
    Value out = cl_new_arr();
    int pos = 0;
    int ms, me;
    while (pos <= (int)t->len
        && cl_regex_match(r, t->data, (int)t->len, pos, &ms, &me, NULL, NULL, 0, NULL)) {
        cl_arr_push(out, cl_new_str(t->data + pos, (uint64_t)(ms - pos)));
        pos = (me > ms) ? me : ms + 1;
    }
    cl_arr_push(out, cl_new_str(t->data + pos, (uint64_t)((int)t->len - pos)));
    return out;
}
