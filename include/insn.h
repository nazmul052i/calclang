#ifndef CALCLANG_INSN_H
#define CALCLANG_INSN_H
#include "common.h"

/* Shared opcode set and instruction record used by the assembler,
   linker, and VM. Keeping these in one place avoids the three-way
   drift the previous duplicated definitions invited. */

typedef enum {
    OP_PUSH  = 1,
    OP_LOAD  = 2,
    OP_STORE = 3,
    OP_ADD   = 4,
    OP_SUB   = 5,
    OP_MUL   = 6,
    OP_DIV   = 7,
    OP_PRINT = 8,
    OP_HALT  = 9,

    /* control flow: all three carry a label operand in the .casm and
       are relocated by the linker into an iarg PC target. */
    OP_JMP   = 10,
    OP_JZ    = 11,  /* pop, jump if zero */
    OP_JNZ   = 12,  /* pop, jump if non-zero */

    /* comparison: pop two, push 1.0 if true else 0.0 */
    OP_EQ    = 13,
    OP_NEQ   = 14,
    OP_LT    = 15,
    OP_LE    = 16,
    OP_GT    = 17,
    OP_GE    = 18,

    /* logical: 0.0 is false, anything non-zero is true */
    OP_AND   = 19,
    OP_OR    = 20,
    OP_NOT   = 21,

    /* unary minus and modulo */
    OP_NEG   = 22,
    OP_MOD   = 23,

    /* function calls. OP_CALL is relocatable like OP_JMP and pushes
       the return PC onto a dedicated return stack inside the VM.
       OP_RET pops and resumes there. */
    OP_CALL  = 24,
    OP_RET   = 25,

    /* push a string by its index into the program's strings table. */
    OP_PUSH_STR     = 26,

    /* Per-call activation record opcodes. Locals live on the data
       stack relative to the frame pointer (fp). ENTER advances sp by
       `iarg` slots (zero-initialized) to make room for non-parameter
       locals; LOAD_LOCAL/STORE_LOCAL address frame[fp + iarg]. */
    OP_ENTER        = 27,
    OP_LOAD_LOCAL   = 28,
    OP_STORE_LOCAL  = 29,

    /* Dispatch to a calclib builtin (sqrt, to_str, len, ...). iarg is
       the BuiltinId; args are already on the stack like a normal CALL. */
    OP_BUILTIN      = 30,

    /* Array operations. NEW_ARRAY pops `iarg` elements off the stack
       and pushes a fresh Array Value. INDEX_GET pops (target, index)
       and pushes target[index]. INDEX_SET pops (target, index, value)
       and writes target[index] = value without pushing anything. */
    OP_NEW_ARRAY    = 31,
    OP_INDEX_GET    = 32,
    OP_INDEX_SET    = 33,

    /* Pop one value and discard. Used after a call-as-statement to
       drop its return value off the stack. */
    OP_DROP         = 34,

    /* First-class function values. PUSH_FN behaves like a CALL operand
       at link time (carries a label that resolves to the function's
       entry PC) but pushes a VAL_FN value instead of calling. CALL_VAL
       pops a function value off the stack and dispatches to it,
       analogous to CALL but with the target taken from the stack
       rather than from a fixed label. iarg is the arg count. */
    OP_PUSH_FN      = 35,
    OP_CALL_VAL     = 36,

    /* Build a map. iarg = number of (key, value) pairs; 2*iarg values
       are popped off the stack in key/value alternating order. */
    OP_NEW_MAP      = 37,

    /* Runtime type check on a single frame slot. iarg = frame offset,
       farg = expected TypeAnnot. Aborts with a clear error if the
       value's tag does not match. Used at function entry to enforce
       parameter type annotations. */
    OP_TYPECHECK    = 38,

    /* Like OP_TYPECHECK but examines the top of the data stack without
       popping. iarg is unused; farg = expected TypeAnnot. Used right
       after evaluating the RHS of a typed `let x: T = expr;` so the
       check fires regardless of whether the binding is a global or
       a local. */
    OP_TYPECHECK_TOP = 39,

    /* Stack: [..., A, B] -> [..., A, B, A, B]. Used by compound
       assignment on indexed lvalues so target+index can be evaluated
       once and reused for both INDEX_GET (read old value) and
       INDEX_SET (write new value). */
    OP_DUP2          = 40,

    /* Build a closure with captured values. iarg = entry PC (resolved
       by the linker from a label, same as PUSH_FN). farg = number of
       captured upvalues. The values are popped off the stack from
       lowest index to highest, so the caller pushes upvalue 0 first,
       then 1, etc. */
    OP_MAKE_CLOSURE  = 41,

    /* Read from the current closure's captured array. iarg = index. */
    OP_LOAD_UPVAL    = 42,

    /* Bitwise. Both operands are coerced to int64 internally; result
       is a regular num. Non-num operands abort the VM with a clear
       error. >> is arithmetic (sign-extending), matching C semantics. */
    OP_BAND          = 43,
    OP_BOR           = 44,
    OP_BXOR          = 45,
    OP_SHL           = 46,
    OP_SHR           = 47,
    OP_BNOT          = 48
} OpCode;

typedef struct {
    int    op;
    double farg;
    int    iarg;
    int    has_sym;
    char   sym[CL_MAX_TEXT];
} Insn;

#endif
