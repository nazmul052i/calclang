#ifndef CALCLANG_RUNTIME_H
#define CALCLANG_RUNTIME_H
/*
   Native runtime support library.

   This header defines the Value representation used by the native
   backend. It is the C-side counterpart to the assembly that the
   native codegen emits — every CalcLang value at runtime is a 64-bit
   word with this layout:

     - Regular IEEE-754 double: stored directly. The bit pattern of any
       non-NaN double (or of +/-Inf, or of "ordinary" NaN with high 13
       bits != 0xFFF8...0xFFFF) IS the value. Numeric programs pay
       zero overhead — addsd/subsd/etc still work.

     - Tagged heap value: a quiet NaN with negative sign whose top 16
       bits select the type and whose bottom 48 bits hold a pointer:

           0xFFF9 xxxx xxxxxxxx   CalcStr *
           0xFFFA xxxx xxxxxxxx   CalcArr *   (reserved for next chunk)
           0xFFFB xxxx xxxxxxxx   CalcMap *   (reserved)
           0xFFFC xxxx xxxxxxxx   CalcClosure *  (reserved)
           0xFFFD xxxx xxxxxxxx   raw fn entry (reserved)

       User-space pointers on x86-64 fit in 48 bits, so we lose
       nothing.

   The runtime functions in src/runtime_x64.c are the helpers the
   native codegen calls into. They use Microsoft x64 ABI (RCX/RDX/R8/
   R9 for the first four args; return in RAX). Codegen emits the call
   sequence; the runtime never has to know about the asm.

   Memory model
   ------------
   First cut: leak. `malloc` everything, never free. CalcLang programs
   that don't allocate forever are fine. A real GC (refcount or
   tracing) is the next chunk.
*/

#include <stdint.h>
#include <stddef.h>

typedef uint64_t Value;

#define CL_TAG_STR     0xFFF9u
#define CL_TAG_ARR     0xFFFAu
#define CL_TAG_MAP     0xFFFBu
#define CL_TAG_CLOSURE 0xFFFCu
#define CL_TAG_FN      0xFFFDu
#define CL_TAG_CPX     0xFFFEu

/* Length-prefixed UTF-8-ish byte sequence. Immutable. We do NOT rely
   on a trailing NUL terminator; runtime functions go off `len`. We DO
   append a NUL byte after `data` when allocating so libc functions
   that demand a C string (e.g. printf %s on a literal printout) work
   too — `data[len] == '\0'` is always safe to read. */
typedef struct CalcStr {
    uint64_t len;
    char     data[];
} CalcStr;

/* Dynamic array of Values. Grows on push; reference semantics — two
   variables holding the same array share the same struct, so a write
   through one is visible through the other (matches VM behavior). */
typedef struct CalcArr {
    uint64_t len;
    uint64_t cap;
    Value   *items;
} CalcArr;

/* Map = parallel-array (keys, values) ordered by insertion. Linear-
   probe equality on key reads/writes keeps the implementation tiny
   and preserves insertion order naturally. Keys may be num or str;
   anything else is rejected at runtime. Reference semantics like
   arrays. */
typedef struct CalcMap {
    uint64_t len;
    uint64_t cap;
    Value   *keys;
    Value   *values;
} CalcMap;

/* Closure = code pointer + captured upvalues. Each upval is a
   pointer to a heap-allocated Value-sized "box" — a layer of
   indirection that lets multiple closures (and the original local in
   the enclosing scope) write through to the same cell. Top-level
   user functions, when referenced by bare name as a value, are
   wrapped in a Closure with n_upvals=0. */
typedef struct CalcClosure {
    void    *code;
    uint32_t n_upvals;
    uint32_t pad;       /* alignment: keep struct multiple of 8 */
    Value  **upvals;
} CalcClosure;

/* Complex number: real + imaginary, both double. Arithmetic
   operators dispatch through the runtime when either operand might
   be complex (the inference treats `complex(...)` as non-num). */
typedef struct CalcCpx {
    double re;
    double im;
} CalcCpx;

/* --- Bit-level helpers (header-only so codegen + runtime agree) --- */
static inline uint16_t cl_tag_of(Value v)  { return (uint16_t)(v >> 48); }
static inline void    *cl_ptr_of(Value v)  { return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL); }
static inline Value    cl_make_tagged(uint16_t tag, void *p) {
    return ((uint64_t)tag << 48) | ((uintptr_t)p & 0x0000FFFFFFFFFFFFULL);
}
static inline int      cl_is_num(Value v)  {
    uint16_t t = cl_tag_of(v);
    return t < CL_TAG_STR || t > CL_TAG_CPX;
}
static inline int      cl_is_str(Value v)  { return cl_tag_of(v) == CL_TAG_STR; }
static inline double   cl_as_num(Value v)  {
    union { uint64_t u; double d; } u; u.u = v; return u.d;
}
static inline Value    cl_from_num(double d) {
    union { uint64_t u; double d; } u; u.d = d; return u.u;
}
static inline CalcStr *cl_as_str(Value v)  { return (CalcStr *)cl_ptr_of(v); }
static inline int      cl_is_arr(Value v)  { return cl_tag_of(v) == CL_TAG_ARR; }
static inline CalcArr *cl_as_arr(Value v)  { return (CalcArr *)cl_ptr_of(v); }
static inline int      cl_is_map(Value v)      { return cl_tag_of(v) == CL_TAG_MAP; }
static inline CalcMap *cl_as_map(Value v)      { return (CalcMap *)cl_ptr_of(v); }
static inline int      cl_is_closure(Value v)  { return cl_tag_of(v) == CL_TAG_CLOSURE; }
static inline CalcClosure *cl_as_closure(Value v) { return (CalcClosure *)cl_ptr_of(v); }
static inline int      cl_is_cpx(Value v)      { return cl_tag_of(v) == CL_TAG_CPX; }
static inline CalcCpx *cl_as_cpx(Value v)      { return (CalcCpx *)cl_ptr_of(v); }

/* --- Runtime entry points (called from generated assembly) --- */
/* MS x64 ABI. C compiler picks up these symbol names; the codegen
   emits `call cl_print`, `call cl_concat`, etc. */
Value cl_new_str(const char *bytes, uint64_t len);
Value cl_concat(Value a, Value b);

/* Print one value followed by a newline. Dispatches on tag: numbers
   use "%.10g", strings print their bytes verbatim. */
void  cl_print(Value v);

/* calclib equivalents (numeric/string subset). Each takes/returns
   Value. Mismatched argument types raise a runtime error and exit. */
Value cl_builtin_to_str(Value v);
Value cl_builtin_to_num(Value v);
Value cl_builtin_len(Value v);
Value cl_builtin_str_at(Value s, Value i);
Value cl_builtin_str_slice(Value s, Value a, Value b);
Value cl_builtin_str_find(Value s, Value sub);
Value cl_builtin_str_upper(Value s);
Value cl_builtin_str_lower(Value s);
Value cl_builtin_str_trim(Value s);
Value cl_builtin_str_repeat(Value s, Value n);
Value cl_builtin_str_starts_with(Value s, Value p);
Value cl_builtin_str_ends_with(Value s, Value p);
Value cl_builtin_type_of(Value v);

/* Polymorphic '+' (dispatches: num+num is add; either str is concat).
   This is the slow path; codegen only emits a call to it when at
   least one operand isn't statically known to be num. */
Value cl_op_plus(Value a, Value b);

/* Array operations. cl_new_arr() returns an empty array. cl_arr_push
   appends and returns the new length (as a Value/num). cl_index_get
   and cl_index_set dispatch on the collection's tag; for now they
   support arrays. They'll grow map handling when that chunk lands. */
Value cl_new_arr(void);
Value cl_arr_push(Value arr, Value v);
Value cl_arr_pop(Value arr);
Value cl_index_get(Value coll, Value idx);
void  cl_index_set(Value coll, Value idx, Value v);

/* array_* calclib. Mismatched types raise a runtime error and exit. */
Value cl_builtin_array_reverse(Value a);
Value cl_builtin_array_sort(Value a);
Value cl_builtin_array_concat(Value a, Value b);
Value cl_builtin_array_slice(Value a, Value lo, Value hi);
Value cl_builtin_array_find(Value a, Value v);
Value cl_builtin_array_contains(Value a, Value v);
Value cl_builtin_array_range(Value lo, Value hi);

/* Maps. cl_new_map returns an empty map. map_set inserts or updates
   in place; map_get errors on missing key (use has_key to guard).
   The cl_index_* helpers already dispatch arrays vs strings; with
   maps in the picture they also handle map keys. */
Value cl_new_map(void);
Value cl_builtin_keys(Value m);
Value cl_builtin_values(Value m);
Value cl_builtin_has_key(Value m, Value k);
Value cl_builtin_del(Value m, Value k);

/* Closure / box helpers. cl_box_new allocates a single-Value cell,
   stores `initial`, and returns a raw `Value *` pointer (not a Value
   — the box is internal). cl_new_closure allocates a CalcClosure
   with the given code pointer and uninitialized upvals; the codegen
   then fills slots via direct memory writes (no helper needed for
   set/get). */
Value  *cl_box_new(Value initial);
Value   cl_new_closure(void *code, uint32_t n_upvals);

/* The rest of calclib (math intrinsics, trig, I/O). Each takes/returns
   Value via MS x64 ABI. Errors exit the process. */
Value cl_builtin_sqrt(Value v);
Value cl_builtin_floor(Value v);
Value cl_builtin_ceil(Value v);
Value cl_builtin_abs(Value v);
Value cl_builtin_int(Value v);
Value cl_builtin_round(Value v);
Value cl_builtin_pow(Value a, Value b);
Value cl_builtin_min(Value a, Value b);
Value cl_builtin_max(Value a, Value b);
Value cl_builtin_sin(Value v);
Value cl_builtin_cos(Value v);
Value cl_builtin_tan(Value v);
Value cl_builtin_asin(Value v);
Value cl_builtin_acos(Value v);
Value cl_builtin_atan(Value v);
Value cl_builtin_atan2(Value y, Value x);
Value cl_builtin_exp(Value v);
Value cl_builtin_log(Value v);
Value cl_builtin_log10(Value v);
Value cl_builtin_random(void);
Value cl_builtin_pi(void);
Value cl_builtin_e(void);
Value cl_builtin_read_line(void);
Value cl_builtin_write(Value v);
Value cl_builtin_str_split(Value s, Value sep);
Value cl_builtin_str_join(Value arr, Value sep);

/* File I/O. file_read returns whole file contents as a string;
   file_write/append return 0 on success; file_exists returns 1/0.
   Errors (path missing, permission denied, etc.) on read/write/append
   exit with a runtime error message. */
Value cl_builtin_file_read(Value path);
Value cl_builtin_file_write(Value path, Value content);
Value cl_builtin_file_append(Value path, Value content);
Value cl_builtin_file_exists(Value path);

/* system(cmd) — invokes the OS shell. Returns the exit code as a num. */
Value cl_builtin_system(Value cmd);

/* Complex-number constructor + accessors. complex(re, im) makes a new
   complex value. real/imag/conj/arg accept either num (treating it as
   re+0i) or cpx. abs is polymorphic too — magnitude for cpx, |x| for
   num. The four arithmetic ops dispatch through cl_op_*. */
Value cl_builtin_complex(Value re, Value im);
Value cl_builtin_real(Value v);
Value cl_builtin_imag(Value v);
Value cl_builtin_conj(Value v);
Value cl_builtin_arg(Value v);

/* Polymorphic arithmetic (codegen falls back to these when types
   aren't statically num+num). cl_op_plus already exists. */
Value cl_op_minus(Value a, Value b);
Value cl_op_mul  (Value a, Value b);
Value cl_op_div  (Value a, Value b);
Value cl_op_mod  (Value a, Value b);
Value cl_op_neg  (Value a);

/* Exceptions. The generated code calls cl_try_push at the start of a
   `try` block with the native rsp/rbp at try-entry and the address
   of the catch label. cl_try_pop is called on normal exit. cl_throw
   walks the handler stack, restores the topmost handler's rsp/rbp,
   and jumps to the catch label — non-local control transfer that
   unwinds whatever frames are in between.
   Uncaught exceptions print a diagnostic and exit.
   cl_get_exception fetches the most recently thrown value for the
   catch handler to bind. */
void   cl_try_push(void *rsp, void *rbp, void *catch_pc);
void   cl_try_pop(void);
void   cl_throw(Value v) __attribute__((noreturn));
Value  cl_get_exception(void);

/* Foreign-function interface. ffi_load returns the library handle as
   a num (the OS pointer cast to a double; round-trips exactly for
   any practical address). ffi_call resolves `name` in `lib`,
   marshals the array `args` according to `sig`, calls the function,
   and marshals the return.

   Signature format: "<ret>:<argkinds>" where ret is one of v/i/d/s
   and each argkind is i/d/s.
     v = void return
     i = int (passed as / returned as num)
     d = double (passed as / returned as num)
     s = const char* (passed as / returned as str; NULL becomes "")

   Up to 4 args per call. Larger arities require libffi or a custom
   thunk and aren't in this round. */
Value cl_builtin_ffi_load(Value path);
Value cl_builtin_ffi_call(Value lib, Value name, Value sig, Value args);

/* Runtime equality for non-numeric tagged values. Numeric == numeric
   stays on the hardware fast path. */
Value cl_op_eq(Value a, Value b);
Value cl_op_neq(Value a, Value b);
Value cl_op_lt(Value a, Value b);
Value cl_op_le(Value a, Value b);
Value cl_op_gt(Value a, Value b);
Value cl_op_ge(Value a, Value b);

/* Truthiness coercion. 0 is falsy, "" is falsy, everything else
   truthy. Returns 1.0 or 0.0 as a Value (always a number). */
Value cl_truthy(Value v);

#endif
