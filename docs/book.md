# The CalcLang Programming Language

*A tutorial introduction to CalcLang, in the style of K&R's "The C Programming Language."*

This book teaches CalcLang from scratch. CalcLang is a small dynamically-typed programming language that compiles to either a custom bytecode (runs on `calcvm`) or to a real native executable (`.exe` on Windows, ELF on Linux) via a tiny x86-64 backend. The book starts with hello-world and works up to closures, classes, multi-file programs, complex numbers, FFI, exceptions, and an engineering-flavored standard library that includes everything from FFT to Levenberg-Marquardt.

If you've written C, the syntax will feel familiar — semicolons, curly braces, `let`/`fn`/`if`/`while`/`for`. The semantics are closer to a scripting language: types are dynamic, strings and arrays are garbage-collected and have reference semantics, and functions are first-class values.

The compiler is named `calcc` (bytecode) or `calcnat` (native). The bytecode VM is `calcvm`. Through this book you'll mostly use `calcnat` — it's the simpler workflow and produces real `.exe` / ELF binaries.

### How to read this book

Each chapter introduces a feature and then drills into it with worked examples. Code blocks marked `calc` are CalcLang source; `bash` blocks are shell commands you run after building the toolchain. Comments at end of `calc` lines (`// 5`) show what the line would print, so you can read top to bottom and see the program's behaviour without running it.

Chapters 1–7 are the core language. Chapter 8 covers multi-file builds. Chapter 9 covers the type system. Chapter 9.5 covers the engineering-oriented additions (file I/O, shell, complex numbers, FFI, exceptions, TCO). Chapter 10 is the calclib reference. Chapter 10.5 walks through the CalcLang engineering library. Chapters 11–12 build complete programs and explain the compiler. Appendices cover idioms, debugging, performance, and limitations.

---

## Chapter 0 — Getting started

### Build the toolchain

You'll need a working C compiler on your `PATH`:

- **Windows**: MinGW-w64 (`gcc`) is the standard choice. WSL or MSYS2 also work. Native MSVC `cl.exe` isn't supported — the assembly emitted by `calcnat` is in GCC-style Intel syntax and links against the included `runtime_x64.c`.
- **Linux / macOS**: system `gcc` or `clang` (the Makefile uses `gcc` by default; override with `make CC=clang`).

A POSIX shell (`sh` or `bash`) is also needed to drive `make` and the test runner. Git Bash, WSL, or MSYS2 cover this on Windows.

From the repository root:

```bash
make
```

That produces five binaries under `build/`:

| Tool       | What it does                                                  |
|------------|---------------------------------------------------------------|
| `calcc`    | Source `.calc` → assembly-like `.casm` (bytecode pipeline)    |
| `calcasm`  | `.casm` → object `.co`                                        |
| `calcld`   | One or more `.co` files → linked `.cexe` bytecode             |
| `calcvm`   | Bytecode interpreter that executes `.cexe`                    |
| `calcnat`  | Source `.calc` → x86-64 assembly `.s` → linked OS executable  |

`calcnat` is the one-stop tool: hand it a `.calc` and it produces an `.exe` (or ELF on Linux) directly, invoking `gcc` under the hood. The other four tools exist for the bytecode pipeline, which the test runner uses for verification.

If you don't have `make`, the manual build commands are in the project README. They reduce to compiling the C sources under `src/` into the five binaries above.

#### Troubleshooting

- `gcc: command not found` — gcc isn't on `PATH`. On Windows, ensure MinGW's `bin/` directory is in `%PATH%`. On Linux, install `build-essential` (Debian/Ubuntu) or the equivalent.
- `make: command not found` on Windows — install Git for Windows (provides `sh`), then either use `mingw32-make` or just build from the README's manual command list.
- Permission errors writing to `build/` — Make creates the directory; if it pre-exists with the wrong permissions, `rm -rf build` and rebuild.
- Test failures with `\r` mismatches — the test runner uses `strip_cr` to normalise Windows-style line endings. If you see them, check that `tests/run_tests.sh` is intact.

### Hello, world

Create a file `hello.calc`:

```calc
print "hello, world";
```

Compile and run:

```bash
build/calcnat hello.calc
./hello.exe        # Windows
./hello            # Linux/macOS (no .exe by default — see below)
```

That's it. `calcnat` takes one `.calc` file and produces an executable. The default output filename is the input minus `.calc` plus `.exe` on every platform — the same name regardless of OS. To pick a different name use `-o`:

```bash
build/calcnat hello.calc -o greeter.exe
```

Behind the scenes, `calcnat` performs three steps:

1. Lexes and parses `hello.calc` into an AST.
2. Runs the x86-64 codegen, producing a `.s` assembly file in a temp directory.
3. Invokes `gcc` to assemble and link the `.s` with the bundled runtime (`src/runtime_x64.c`), producing the final executable.

If you want to see the intermediate assembly, pass `-S`:

```bash
build/calcnat -S hello.calc hello.s
cat hello.s        # human-readable Intel-syntax assembly
```

### Two pipelines

CalcLang has two backends. Both come from the same lexer, parser, AST, and type inferrer:

```
hello.calc ─┬─► .casm ─► .co ─► .cexe ─► calcvm           (bytecode VM)
            └─► .s ──────────── gcc ──► hello.exe / hello   (native)
```

The **native path** (`calcnat`) is what you'll use day-to-day. It runs faster (no interpreter loop) and produces real OS executables. CalcLang's engineering library, complex numbers, FFI, and file I/O all live here.

The **bytecode path** (`calcc` → `calcasm` → `calcld` → `calcvm`) is the original implementation. It's byte-identical in output for the features it supports, and it's what the test runner cross-checks the native backend against. The VM is a useful reference implementation but lacks several native-only features (complex numbers, FFI, file I/O, exceptions, TCO).

You don't have to pick: a single `.calc` file builds and runs through either pipeline without modification. The book uses the native path throughout. For the VM workflow, see the project README.

### A first program with multiple statements

```calc
let name = "world";
let n = 3;
let i = 0;
while (i < n) {
    print "hello, " + name + " (" + i + ")";
    i = i + 1;
}
```

Save as `hi.calc`, then:

```bash
build/calcnat hi.calc && ./hi.exe
# hello, world (0)
# hello, world (1)
# hello, world (2)
```

You've already seen most of CalcLang's syntax: `let` declares a variable, `+` concatenates strings (numbers are auto-converted), `while` loops, `print` writes a line. The next chapters develop each piece in detail.

### Comments

`//` to end-of-line, `/* ... */` for block comments (don't nest):

```calc
// single-line comment

/* multi-line
   comment */

let x = 1 /* inline */ + 2;     // 3
print x;
```

---

## Chapter 1 — Numbers and variables

### Arithmetic

CalcLang has the operators you'd expect from C-family languages:

```calc
print 2 + 3;       // 5
print 10 - 4;      // 6
print 6 * 7;       // 42
print 20 / 4;      // 5
print 17 % 5;      // 2    — modulo, like C's % but works on doubles
print -7;          // -7
print (2 + 3) * 4; // 20
```

Numbers are 64-bit IEEE-754 doubles. **There is no separate integer type** — integers are just doubles whose fractional part happens to be zero. This has a few consequences:

- Division returns a real number, never truncates: `7 / 2` is `3.5`, not `3`. Use `floor(7 / 2)` for integer-division behaviour.
- The maximum exactly-representable integer is `2^53`, around 9 × 10¹⁵. Larger integers lose precision silently.
- `%` works on non-integer operands using `fmod` semantics: `5.5 % 2` is `1.5`.

```calc
print 7 / 2;        // 3.5
print floor(7 / 2); // 3
print 5.5 % 2;      // 1.5
print 1e16 + 1 == 1e16;   // 1 — the +1 was rounded away
```

`print` formats numbers with `%.10g` — about 10 significant digits, no trailing zeros, scientific notation when the magnitude is extreme:

```calc
print 1.0;             // 1
print 0.1 + 0.2;       // 0.3   (rounded; the exact float is 0.30000000000000004)
print 1e6;             // 1000000
print 1.5e-7;          // 1.5e-07
print 1 / 0;           // inf
print 0 / 0;           // -nan
```

Floating-point edge cases (`inf`, `-inf`, `nan`) propagate through arithmetic as IEEE-754 prescribes; they're not exceptions. The `<` / `>` comparisons with `nan` always return false, so a `nan` in your data won't accidentally win a max/min comparison.

#### Scientific-notation literals

You can write numbers in scientific form:

```calc
print 1e6;        // 1000000
print 1.5e-7;     // 1.5e-07
print 2.3e+4;     // 23000
print .5e2;       // 50         — leading decimal point is allowed
print 6.022e23;   // 6.022e+23
```

The lexer is careful: `e` is only consumed as an exponent marker if followed by a digit (or `+`/`-` and a digit). The calclib constant `e()` (Euler's number) still works in normal expressions like `let z = e();`.

### `let` and assignment

Bind a value to a name with `let`:

```calc
let x = 10;
let y = 5;
print x + y;       // 15
```

Once bound, change the value with plain assignment (no `let`):

```calc
let x = 10;
x = x + 1;
print x;           // 11
```

The variable's value can become any type — CalcLang is dynamically typed:

```calc
let z = 42;
print z;           // 42
z = "hello";
print z;           // hello
z = [1, 2, 3];
print z;           // [1, 2, 3]
```

Trying to assign to a name that was never `let`-bound is a compile error:

```calc
y = 5;             // error: y is not in scope
```

**`let` vs `=` — shadowing vs reassignment.** Re-using `let` inside a new scope creates a fresh binding (it *shadows*); using `=` modifies the existing one. We'll see scope rules in Chapter 2.

#### Multiple variables, one expression

CalcLang doesn't have multiple-assignment syntax (`a, b = b, a` from Python). You write it out:

```calc
let a = 10;
let b = 20;
let tmp = a;
a = b;
b = tmp;
print a;    // 20
print b;    // 10
```

Or use an array if you do this often:

```calc
fn swap_to_array(a, b) { return [b, a]; }
let result = swap_to_array(10, 20);
print result[0];     // 20
print result[1];     // 10
```

### Compound assignment and increment

The compound forms work like in C:

```calc
let n = 10;
n += 5;            // n = 15        (same as n = n + 5)
n -= 3;            // n = 12
n *= 2;            // n = 24
n /= 4;            // n = 6
n %= 4;            // n = 2
n++;               // n = 3         (post-increment statement; not an expression)
n--;               // n = 2         (post-decrement statement)
```

These are pure syntactic sugar — `n += 5` is exactly `n = n + 5`. `n++` and `n--` are **statements**, not expressions: you can't write `let m = n++;` or `if (n++ > 5) ...`. Use the explicit form for those (`let m = n; n = n + 1;`).

`+=` on a string concatenates:

```calc
let s = "hello";
s += ", world";
print s;           // hello, world
```

### Comparison and logical operators

```calc
print 5 < 6;       // 1     — true is 1, false is 0
print 5 == 5;      // 1
print 5 != 6;      // 1
print 5 >= 5;      // 1

print 1 && 1;      // 1     — short-circuit AND
print 1 || 0;      // 1     — short-circuit OR
print !0;          // 1     — logical NOT
print !1;          // 0
print !"";         // 1     — empty string is falsy
print !"x";        // 0
```

Booleans aren't a distinct type — `true` is the number `1.0` and `false` is `0.0`. Every comparison and logical op returns a number, so `if (a < b) { ... }` and `let positive = (x > 0); print positive + 1;` both work.

**Truthiness rules** (used by `if`, `while`, `&&`, `||`, `!`):

| Value type | Falsy        | Truthy           |
|------------|--------------|------------------|
| num        | `0`, `-0`    | everything else (incl. `nan`) |
| str        | `""`         | any non-empty    |
| arr        | (none)       | always — even `[]` |
| map        | (none)       | always — even `{}` |
| fn         | (none)       | always           |

Two surprises worth highlighting:

- **Empty array / map are truthy.** Use `len(a) == 0` to check emptiness — there's no `is_empty` shortcut.
- **`nan` is truthy** (it's nonzero), but `nan == nan` is *false* per IEEE-754. So `let x = 0/0; if (x) { print "truthy"; }` prints "truthy", but `if (x == x)` does not.

**Short-circuit semantics.** `&&` and `||` evaluate their right operand only if needed:

```calc
fn boom() { throw "side effect"; }

print 0 && boom();   // 0      — boom() never called
print 1 || boom();   // 1      — boom() never called
```

This is useful for guarding expensive or partial operations:

```calc
let cfg = {"verbose": 1};
if (cfg["verbose"] && len(messages) > 0) {
    print messages;
}
```

### Ternary operator

`cond ? a : b` is an expression that evaluates `cond` and yields `a` if truthy, `b` otherwise. Only the chosen branch runs:

```calc
let x = 5;
let y = 10;
print x > y ? "x wins" : "y wins";    // y wins

// Chain for if-else-if expression form:
fn grade(s) {
    return s >= 90 ? "A"
         : s >= 80 ? "B"
         : s >= 70 ? "C" : "F";
}
```

Right-associative: `a ? b : c ? d : e` parses as `a ? b : (c ? d : e)`. Use parentheses if you want the other grouping.

The two branches can have different types — the result type is the union. For numeric `?` chains in tight loops you can annotate the surrounding function as `: num` to keep the codegen on the hardware fast path.

### Bitwise operators

CalcLang has the full C set of bitwise operators: `&` (AND), `|` (OR), `^` (XOR), `~` (NOT), `<<` (left shift), `>>` (right shift — arithmetic, sign-extending). The compound forms `&=`, `|=`, `^=`, `<<=`, `>>=` work too.

```calc
print 5 & 3;        // 1
print 5 | 3;        // 7
print 5 ^ 3;        // 6
print ~5;           // -6
print 1 << 4;       // 16
print 256 >> 2;     // 64
print -8 >> 1;      // -4  (arithmetic shift)
```

Operands are converted to 64-bit signed integers (truncating toward zero), the op runs, and the result is converted back to a num. Non-numeric operands raise at runtime. Shift counts are masked to the low 6 bits to match hardware behaviour (no UB for `1 << 100`).

#### Hex and binary literals

Numeric literals can be written in hex (`0x...`) or binary (`0b...`) form. Underscores anywhere in the digits are treated as separators for readability:

```calc
print 0xFF;             // 255
print 0xCAFE_BABE;      // 3405691582
print 0b1010;           // 10
print 0b1111_0000;      // 240
```

These are pure lexical forms — once parsed they're indistinguishable from a decimal literal of the same value.

#### Bitwise precedence

CalcLang follows C-style precedence. From tightest to loosest in the relevant range:

```
*  /  %                      (multiplicative)
+  -                         (additive)
<<  >>                       (shift)
<  <=  >  >=                 (relational)
==  !=                       (equality)
&                            (bitwise AND)
^                            (bitwise XOR)
|                            (bitwise OR)
&&                           (logical AND)
||                           (logical OR)
```

Worth memorizing: `&` binds tighter than `^`, which binds tighter than `|`. So `a | b & c` is `a | (b & c)`. Use parentheses freely when the intent isn't obvious.

#### Classic bit-twiddling patterns

```calc
// Set bit n
let flags = 0;
flags |= 1 << 3;            // set bit 3
flags |= 1 << 5;            // set bit 5

// Test bit n
if ((flags & (1 << 3)) != 0) { print "bit 3 is set"; }

// Clear bit n
flags &= ~(1 << 3);

// Toggle bit n
flags ^= 1 << 5;

// Mask out low byte
let low = value & 0xFF;

// Pack two 16-bit numbers into 32 bits
let packed = (hi << 16) | lo;
let hi2 = (packed >> 16) & 0xFFFF;
let lo2 = packed & 0xFFFF;
```

#### Popcount (count set bits)

```calc
fn popcount(n) {
    let c = 0;
    while (n != 0) {
        c = c + (n & 1);
        n = n >> 1;
    }
    return c;
}
print popcount(0xFF);   // 8
print popcount(0x55);   // 4
```

### Order of evaluation and precedence

CalcLang uses C-style precedence. From tightest binding to loosest:

```
1.  ()        function call, indexing, .field
2.  ! -       unary
3.  * / %
4.  + -       (binary)
5.  < <= > >= 
6.  == !=
7.  &&
8.  ||
9.  =
```

When in doubt, parenthesize:

```calc
print 1 + 2 * 3;        // 7   — not 9
print (1 + 2) * 3;      // 9
print !0 == 1;          // 1   — !0 first, then ==
print !(0 == 1);        // 1   — explicit grouping
```

### A worked example: prime check

Putting it together — a function that decides primality:

```calc
fn is_prime(n) {
    if (n < 2)   { return 0; }
    if (n == 2)  { return 1; }
    if (n % 2 == 0) { return 0; }
    let i = 3;
    while (i * i <= n) {
        if (n % i == 0) { return 0; }
        i += 2;
    }
    return 1;
}

let count = 0;
for (let k = 0; k < 100; k = k + 1) {
    if (is_prime(k)) { count += 1; }
}
print "primes < 100: " + count;    // primes < 100: 25
```

This program uses everything from this chapter: `let`, `if`/`else`, comparison and modulo, compound `+=`, `while`, `for`, function definitions, and `return`. The next chapter develops control flow systematically.

---

## Chapter 2 — Control flow

### `if` / `else`

```calc
let age = 18;
if (age >= 18) {
    print "adult";
} else {
    print "minor";
}
```

`else if` chains work too:

```calc
let score = 85;
if (score >= 90) {
    print "A";
} else if (score >= 80) {
    print "B";
} else if (score >= 70) {
    print "C";
} else {
    print "F";
}
```

The condition is any expression — it doesn't have to be a comparison. Truthiness follows the table in Chapter 1: `0` and `""` are falsy; every other value (including `[]`, `{}`, `nan`, and any function) is truthy.

```calc
if ("yes") { print "truthy"; }   // truthy
if ("")    { print "never"; }    // never reached
if (0)     { print "never"; }    // never reached
if ([])    { print "still truthy"; }
```

#### Braces are mandatory

Unlike C, CalcLang **requires braces** even for single-statement branches. There's no `if (x) print x;` shortcut:

```calc
if (x > 0) { print x; }            // OK
if (x > 0) print x;                // parse error
```

This keeps the dangling-else question from ever coming up.

#### Common conditional patterns

**Min/max in line.** Use a ternary expression or the calclib helper:

```calc
let m = a < b ? a : b;             // ternary
let m2 = min(a, b);                // calclib (also handles arrays in lib/stats)
```

```calc
let m = min(a, b);
```

**Clamping a value to a range:**

```calc
fn clamp(x, lo, hi) {
    if (x < lo) { return lo; }
    if (x > hi) { return hi; }
    return x;
}
print clamp(15, 0, 10);    // 10
print clamp(-3, 0, 10);    // 0
print clamp(5, 0, 10);     // 5
```

**Guard clauses** (early exits) keep nesting shallow:

```calc
fn process(x) {
    if (x < 0) { return "negative"; }
    if (x == 0) { return "zero"; }
    if (x > 100) { return "huge"; }
    return "normal";
}
```

### `while`

A `while` loop runs as long as the condition is truthy:

```calc
let i = 0;
let sum = 0;
while (i <= 10) {
    sum = sum + i;
    i = i + 1;
}
print sum;         // 55
```

For a loop that must run the body at least once, `do { … } while (cond);` is the natural choice (see below). The `while (1) { … if (done) break; }` idiom also works when the exit condition is awkward to express up front:

```calc
let line = "";
while (1) {
    line = read_line();
    if (line == "") { break; }     // EOF
    print "got: " + line;
}
```

`while (1)` is the idiomatic infinite loop. Some languages prefer `while (true)`, but `true` isn't a CalcLang keyword — use `1`.

#### Newton's method via `while`

```calc
// Find the square root of N by Newton iteration.
fn newton_sqrt(N) {
    let x = N;
    while (1) {
        let next = 0.5 * (x + N / x);
        if (abs(next - x) < 1e-12) { return next; }
        x = next;
    }
}
print newton_sqrt(2);    // 1.414213562
```

This is the standard "loop until converged" pattern: the loop body decides when to exit via `break` or `return`, rather than encoding the test in the loop header.

### `for`

C-style `for` with init, condition, and step:

```calc
let total = 0;
for (let i = 1; i <= 10; i = i + 1) {
    total += i;
}
print total;       // 55
```

The `init` clause may declare a fresh variable with `let`, or just assign to one already in scope. The variable declared inside the `for` is **scoped to the loop** — it doesn't outlive it:

```calc
for (let i = 0; i < 3; i = i + 1) { print i; }
// print i;   // error: i is not in scope here
```

If you want the index visible afterwards, declare it before the loop:

```calc
let i = 0;
for (i = 0; i < 10; i = i + 1) {
    if (i == 5) { break; }
}
print i;           // 5
```

#### `for-in` over arrays, strings, and ranges

CalcLang also has the Rust-style `for x in iter` form. The iterable can be an array, a string (one character per step), or a range expression. Braces are required on the body.

```calc
for x in [10, 20, 30, 40] {
    print x;
}

for ch in "hello" {
    print ch;          // h, e, l, l, o
}
```

Ranges use `start..end` (exclusive) or `start..=end` (inclusive). The bounds can be any numeric expression:

```calc
for i in 0..5    { print i; }     // 0, 1, 2, 3, 4
for i in 1..=3   { print i; }     // 1, 2, 3

let n = 10;
for i in 0..n    { ... }
```

Maps don't iterate directly — wrap them in `keys(m)`:

```calc
let scores = {"alice": 90, "bob": 75};
for name in keys(scores) {
    print name + " -> " + scores[name];
}
```

The parens-around-the-head form works too if you prefer the C-style framing — `for (x in xs) { ... }` and `for (let x in xs) { ... }` are accepted equivalents. Pick whichever reads better.

#### Common loop patterns

**Generating a range:**

```calc
let xs = [];
for i in 0..100 { push(xs, i * 0.1); }
// xs is now [0, 0.1, 0.2, ..., 9.9]
```

Or use the calclib helper:

```calc
let xs = array_range(0, 100);    // [0, 1, ..., 99]
```

**Summing / averaging:**

```calc
fn average(xs) {
    let s = 0;
    for x in xs { s += x; }
    return s / len(xs);
}
```

**Counting:**

```calc
fn count_if(xs, pred) {
    let c = 0;
    for x in xs {
        if (pred(x)) { c += 1; }
    }
    return c;
}
print count_if([1, -2, 3, -4, 5], fn(x) { return x > 0; });  // 3
```

**Reverse iteration** (use the C-style form since `for-in` is forward-only):

```calc
let xs = [10, 20, 30];
for (let i = len(xs) - 1; i >= 0; i = i - 1) {
    print xs[i];
}
// 30, 20, 10
```

**Nested loops** (matrix walk):

```calc
let m = [[1, 2, 3], [4, 5, 6]];
for row in m {
    for v in row {
        print v;
    }
}
```

When you need the index *and* the value, mix the forms:

```calc
let xs = ["a", "b", "c"];
for i in 0..len(xs) {
    print i + ": " + xs[i];
}
```

### `do { ... } while (cond);`

A loop that **runs the body at least once**, then re-runs while `cond` is truthy:

```calc
let answer = "";
do {
    answer = read_line();
} while (answer != "quit");
```

Compared to `while (cond) { ... }`, the difference is just where the test runs. `do/while` is the natural fit when the very first iteration computes the value the test inspects (the read-loop above, a "do work; check if done" pattern, etc.). Semicolon after the closing `)` is required.

### `break` and `continue`

`break` exits the nearest enclosing loop. `continue` jumps to the next iteration — to the step clause in a `for`, or back to the condition in a `while`:

```calc
// Print the first odd number greater than 100.
let n = 100;
while (1) {
    n = n + 1;
    if (n % 2 == 0) { continue; }
    print n;
    break;
}
// 101
```

Inside nested loops, `break` and `continue` only affect the **innermost** loop. CalcLang has no labeled break. If you need to escape from a deeply nested loop, the idiomatic solutions are:

1. Pull the loops into a function and `return`.
2. Use a sentinel flag.

```calc
// Option 1: extract into a function.
fn find(haystack, needle) {
    for (let r = 0; r < len(haystack); r = r + 1) {
        for (let c = 0; c < len(haystack[r]); c = c + 1) {
            if (haystack[r][c] == needle) { return [r, c]; }
        }
    }
    return [-1, -1];
}

// Option 2: sentinel.
let found = 0;
let r = 0;
while (r < len(haystack) && !found) {
    let c = 0;
    while (c < len(haystack[r]) && !found) {
        if (haystack[r][c] == 42) { found = 1; }
        c += 1;
    }
    r += 1;
}
```

Option 1 is almost always clearer.

### `switch`

Multi-way dispatch on a value, with **no C-style fall-through** — each case is implicitly terminated, so you never need a trailing `break;` just to stop. (You *can* use `break` inside a multi-statement case body for early exit.)

```calc
fn day_name(d) {
    switch (d) {
        case 0: return "Sun";
        case 1: return "Mon";
        case 2: return "Tue";
        case 3: return "Wed";
        case 4: return "Thu";
        case 5: return "Fri";
        case 6: return "Sat";
        default: return "?";
    }
}
print day_name(3);     // Wed
print day_name(10);    // ?
```

Rules:

- The discriminant can be any expression. Case values can be too (they're not restricted to literals, though that's the common case).
- Equality is checked with the same semantics as `==` — numbers, strings, and complex values all work.
- `default:` is optional. If no case matches and no default is given, the switch does nothing and execution continues after.
- Cases match in order (top-to-bottom). The first match wins.
- Each case body is its own scope; `let` declarations inside a case don't leak out.
- `break` inside a case body exits the switch immediately. Inside an enclosing loop, `break` continues to mean "break the innermost loop or switch."

#### Switch on strings

```calc
fn op_arity(op) {
    switch (op) {
        case "+": return 2;
        case "-": return 2;
        case "neg": return 1;
        case "if": return 3;
        default: return 0;
    }
}
print op_arity("+");      // 2
```

That's about three times more readable than the chained `if (op == "+") ... else if (op == "-") ...` form.

#### When to use switch vs. if-chain

- `switch` is the right choice when you're dispatching on the value of *one* expression to *equal* cases.
- `if`/`else if` is the right choice when each branch tests a different condition (`if (x > 0)`, `else if (s == "foo")`, etc.).

If your cases need ranges (`case 0..9`), pattern matching, or guard conditions, fall back to `if`/`else if` — CalcLang doesn't have those features yet.

### Block scope

Every `{ ... }` is a fresh scope. Inner `let` declarations shadow outer ones; assignment without `let` reaches outward to the nearest binding:

```calc
let x = 1;
{
    let x = 999;
    print x;       // 999  — new binding shadows the outer
}
print x;           // 1    — outer unchanged
```

vs. reassignment, which finds the outer binding:

```calc
let x = 1;
{
    x = 999;       // no `let` — reassigns the existing outer x
}
print x;           // 999
```

This is identical to C's block-scope behaviour. The rule of thumb: `let` for new variables, plain `=` for updates.

#### Bare blocks for local scoping

A standalone `{ ... }` works as a scope-limiter for temporary state:

```calc
let result = 0;
{
    let lots_of_temp = build_big_array();
    let summary = summarize(lots_of_temp);
    result = summary["mean"];
    // lots_of_temp and summary go out of scope here
}
print result;
```

After the block, `lots_of_temp` and `summary` are unreachable and eligible for GC.

#### Loop body scope

Each iteration of a `for` or `while` runs the body in a fresh scope:

```calc
for (let i = 0; i < 3; i = i + 1) {
    let snapshot = i * 10;       // fresh `snapshot` each iteration
    print snapshot;
}
// 0
// 10
// 20
```

This matters most when the body **captures variables in a closure** — see Chapter 6 for the implications.

---

## Chapter 3 — Functions

### Definition and call

```calc
fn square(n) {
    return n * n;
}
print square(7);   // 49
```

`fn name(p1, p2, ...) { body }` defines a named function. Parentheses are required even for zero parameters:

```calc
fn now_ish() { return 42; }
print now_ish();      // 42
```

#### Return rules

- `return expr;` exits the function with `expr`'s value.
- Bare `return;` returns the number `0`.
- Falling off the end of the body implicitly returns `0`.

```calc
fn maybe(b) {
    if (b) { return "yes"; }
    // falls off → returns 0
}
print maybe(1);     // yes
print maybe(0);     // 0
```

That implicit-zero behaviour matters when you mix return types: a function whose "no result" branch leaks `0` into a downstream string concat will silently produce `"label: 0"`. Be deliberate: either return a sentinel like `""` or a map (`{"ok": 0}`), or throw an exception (Chapter 9.5).

#### Recursion

Recursion works as you'd expect:

```calc
fn fact(n) {
    if (n <= 1) { return 1; }
    return n * fact(n - 1);
}
print fact(10);    // 3628800
print fact(20);    // 2.43e+18 — exact, since 20! fits in 53 bits
print fact(21);    // 5.109094217e+19 — loses precision past 2^53
```

Mutual recursion is also fine — the parser does a forward-declaration pass before binding bodies, so the order you write the functions doesn't matter:

```calc
fn is_even(n) {
    if (n == 0) { return 1; }
    return is_odd(n - 1);
}
fn is_odd(n) {
    if (n == 0) { return 0; }
    return is_even(n - 1);
}
print is_even(13);   // 0
print is_even(28);   // 1
```

Plain recursion blows the stack at a few thousand frames. For tail-recursive iteration over millions of items, see "Tail-call optimization" in Chapter 9.5.

#### Multiple arguments and side effects

Arguments are evaluated **left to right**, then the function runs. Side effects in argument expressions happen in that order:

```calc
fn touch(label, x) { print "touched " + label; return x; }
fn add(a, b) { return a + b; }
print add(touch("first", 10), touch("second", 20));
// touched first
// touched second
// 30
```

Arguments are passed by value for numbers and strings (the function gets its own copy), and **by reference** for arrays, maps, and complex numbers (the function sees the same underlying object — see Chapter 5).

```calc
fn bump_number(x) { x = x + 1; print "inside: " + x; }
let n = 10;
bump_number(n);
print "outside: " + n;       // outside: 10  — `n` unchanged
```

```calc
fn bump_array(a) { a[0] = 999; }
let a = [1, 2, 3];
bump_array(a);
print a;                     // [999, 2, 3]  — mutated in place
```

### Optional type annotations

You can annotate parameters and the return type. Annotations are optional — bare `fn foo(x) { ... }` accepts anything (equivalent to `: any` everywhere).

```calc
fn add(a: num, b: num): num {
    return a + b;
}
```

Types:

| Annotation | Means                                            |
|------------|--------------------------------------------------|
| `num`      | A number (or boolean — booleans are nums)        |
| `str`      | A string                                         |
| `arr`      | An array                                         |
| `map`      | A map (or class instance — instances are maps)   |
| `fn`       | A function (any signature)                       |
| `bool`     | Alias for `num`                                  |
| `any`      | Anything; no check                               |

#### When the check fires

- **Compile time** if the call site has a literal of the wrong type, or the type inferrer can prove a mismatch:
  ```calc
  fn need_num(n: num): num { return n + 1; }
  need_num("hi");          // compile error: argument 1: expected num, got str
  ```
- **Runtime** otherwise — passing a value whose type can't be determined statically:
  ```calc
  fn anything() { return "hi"; }
  print need_num(anything());     // runtime type error at call site
  ```

The compile-time check is fast and free; the runtime check costs one tag comparison.

#### Why annotate?

1. **Documentation** that the compiler actually enforces.
2. **Performance.** The native backend uses the annotations to skip type-dispatch on hot paths — a function declared `fn dist(x: num, y: num): num` compiles to a direct hardware `addsd` rather than the polymorphic runtime `cl_op_plus`. For tight numerical inner loops, annotating can cut runtime in half.
3. **Cross-file safety.** When you import a `pub fn` in another file via `extern fn`, the annotations let the compiler verify the link.

### Parameter rules

- Up to 16 parameters per function.
- Calling with the wrong number of arguments is a compile error:
  ```calc
  fn add(a, b) { return a + b; }
  print add(3);            // compile error: takes 2 args, got 1
  ```
- No default parameter values, no rest/varargs syntax. To accept variable arity, take an array:
  ```calc
  fn sum_all(xs) {
      let s = 0;
      for (let i = 0; i < len(xs); i = i + 1) { s += xs[i]; }
      return s;
  }
  print sum_all([1, 2, 3, 4]);   // 10
  ```

### Visibility

Top-level functions are private (file-local) by default. Mark them `pub` to export across compilation units — see [Chapter 8 — Multi-file programs](#chapter-8--multi-file-programs).

```calc
pub fn area(w, h) { return w * h; }
priv fn helper(x) { return x * 2; }   // explicit private (same as bare `fn`)
fn other(x) { return x; }             // also private — `priv` is just explicit
```

`priv` and bare `fn` mean the same thing; `priv` exists for readers who want the visibility called out.

### Naming conventions

Idiomatic CalcLang uses **snake_case** for functions and variables, **PascalCase** for classes:

```calc
fn pythagorean_distance(x, y) { return sqrt(x*x + y*y); }
class CircleArea { fn init(r) { this.r = r; } }
```

This is just a convention — the language treats all identifiers uniformly.

### A small worked example: tiny calculator

A function that takes an operator code and two operands:

```calc
fn calc(op: str, a: num, b: num): num {
    if (op == "+") { return a + b; }
    if (op == "-") { return a - b; }
    if (op == "*") { return a * b; }
    if (op == "/") {
        if (b == 0) { throw "division by zero"; }
        return a / b;
    }
    throw "unknown op: " + op;
}

print calc("+", 3, 4);     // 7
print calc("*", 6, 7);     // 42

try {
    print calc("/", 10, 0);
} catch (e) {
    print "caught: " + e;  // caught: division by zero
}
```

`throw` and `try` / `catch` come back in Chapter 9.5. For now, treat them as "raise and handle errors."

---

## Chapter 4 — Strings

Strings are double-quoted, with the usual escape sequences:

```calc
print "hello, world";
print "with \"quotes\"";
print "tab\there";          // tab + a real tab character
print "two\nlines";         // two lines — \n is a newline
print "backslash: \\";       // single backslash
```

Single quotes are NOT supported — `'x'` is a parse error. Triple-quoted or multi-line raw strings aren't supported either; write multi-line text via concatenation of `"\n"` or by reading a file (Chapter 9.5).

#### Supported escapes

| Escape | Meaning            |
|--------|--------------------|
| `\\`   | backslash          |
| `\"`   | double-quote       |
| `\n`   | newline            |
| `\t`   | tab                |
| `\r`   | carriage return    |
| `\0`   | NUL byte           |

Other escape sequences (`\u00XX` for Unicode, `\xNN` for hex) aren't recognised — use UTF-8 byte sequences directly in the source if you need non-ASCII characters.

Strings hold raw bytes, not characters. `len("héllo")` returns the number of bytes (which depends on encoding), not the number of code points. For ASCII the distinction doesn't matter.

### Concatenation

`+` concatenates when **either** operand is a string. Numbers are coerced via `%.10g`:

```calc
print "hello" + " " + "world";   // hello world
print "x = " + 5;                // x = 5
print 7 + " items";              // 7 items
print "pi = " + 3.14159;         // pi = 3.14159
print "n = " + 1e6;              // n = 1000000
```

`+` is **left-associative**. Mixed expressions chain as you'd expect:

```calc
print "x" + 1 + 2;          // x12   — left-to-right; "x"+1 → "x1", then "x1"+2 → "x12"
print 1 + 2 + "x";          // 3x    — 1+2 → 3 (numeric), then 3+"x" → "3x"
```

That second case is a common gotcha: the numeric `+` runs as long as both sides are numbers; the moment a string enters, every subsequent `+` becomes concatenation.

`+=` works for strings too:

```calc
let msg = "hello";
msg += ", ";
msg += "world";
print msg;       // hello, world
```

Building a long string by repeated `+=` is O(n²) — each concat copies the prefix. For tight loops generating big strings (large CSV / SVG output), stream to a file with `file_append` instead (Chapter 9.5).

#### Other operators on strings

- `==` and `!=` compare by value: `"foo" == "foo"` is `1` even if the two strings were built separately.
- `<`, `<=`, `>`, `>=` compare lexicographically (byte by byte):
  ```calc
  print "apple" < "banana";    // 1
  print "ab" < "abc";          // 1   — shorter prefix wins
  print "B" < "a";             // 1   — uppercase 'B' (0x42) < lowercase 'a' (0x61)
  ```
- `*`, `/`, `-`, `%` don't apply — only `+` works on strings.

### String library

The most useful string functions in calclib:

```calc
print len("calclang");                  // 8       — byte length
print str_upper("hello");               // HELLO
print str_lower("HELLO");               // hello
print str_at("abcde", 2);               // c       — 0-indexed, single-byte
print str_slice("hello", 1, 4);         // ell     — half-open [a, b)
print str_find("foobar", "bar");        // 3       — first index, -1 if absent
print str_starts_with("foobar", "foo"); // 1
print str_ends_with("foobar", "bar");   // 1
print str_repeat("ab", 3);              // ababab
print str_trim("   hi   ");             // hi      — trim leading/trailing whitespace
print str_split("a,b,c", ",");          // ["a", "b", "c"]
print str_join(["a", "b", "c"], "-");   // a-b-c
print to_str(3.14);                     // 3.14
print to_num("42") + 1;                 // 43      — string → num
print type_of("hi");                    // str
```

Edge cases worth knowing:

- `str_at` returns a single-character string, not a number. There's no character-as-int type.
- `str_slice(s, a, b)` is **half-open**: `str_slice("hello", 1, 4) = "ell"` (positions 1, 2, 3). Negative indices aren't supported; for "from the end," compute `len(s) - n`.
- `str_find` returns `-1` when the substring is missing — check before slicing.
- `to_num` is permissive: it accepts a leading sign and a decimal point. Scientific notation works. Trailing garbage after the number returns `0` silently.
- `to_str` of a number uses `%.10g` (same as `print`); of a complex value, it returns `"(re, im)"`.

#### Iterating characters

There's no `for ch in str` form. Walk by index, slicing one character at a time:

```calc
fn count_vowels(s) {
    let n = 0;
    for (let i = 0; i < len(s); i = i + 1) {
        let ch = str_at(s, i);
        if (ch == "a" || ch == "e" || ch == "i" || ch == "o" || ch == "u") {
            n += 1;
        }
    }
    return n;
}
print count_vowels("calclang");    // 2
```

#### Reversing a string

```calc
fn str_reverse(s) {
    let out = "";
    for (let i = len(s) - 1; i >= 0; i = i - 1) {
        out += str_at(s, i);
    }
    return out;
}
print str_reverse("hello");        // olleh
```

This is O(n²) due to string immutability — fine for short strings, slow for very long ones.

#### Checking a prefix or suffix

```calc
fn is_url(s) { return str_starts_with(s, "http://") || str_starts_with(s, "https://"); }
fn is_image(s) {
    return str_ends_with(s, ".png")
        || str_ends_with(s, ".jpg")
        || str_ends_with(s, ".svg");
}
```

#### Splitting and re-joining

`str_split` and `str_join` are inverses on well-formed input:

```calc
let csv = "alice,30,NYC";
let parts = str_split(csv, ",");
print parts;                       // ["alice", "30", "NYC"]
print str_join(parts, " | ");      // alice | 30 | NYC
```

Splitting on a multi-character separator works:

```calc
print str_split("a => b => c", " => ");  // ["a", "b", "c"]
```

Splitting on the empty string is *not* supported.

#### Number formatting

CalcLang's built-in `+` uses `%.10g`. If you need different formatting (fixed decimals, padding), build it yourself:

```calc
fn fixed2(x) {
    let n = floor(x * 100 + 0.5);          // round to 2 decimals
    let whole = floor(n / 100);
    let frac = n - whole * 100;
    let fs = "" + frac;
    if (frac < 10) { fs = "0" + fs; }      // zero-pad
    return "" + whole + "." + fs;
}
print fixed2(3.14159);    // 3.14
print fixed2(2);          // 2.00
```

Or, for richer formatting, just embed gnuplot / Python via `system` (Chapter 9.5).

#### Putting it together: caesar cipher

```calc
fn caesar(s, shift) {
    let out = "";
    for (let i = 0; i < len(s); i = i + 1) {
        let ch = str_at(s, i);
        // Only shift ASCII letters; leave others alone.
        if (ch >= "a" && ch <= "z") {
            let n = to_num("" + ch);    // doesn't work directly; need ord/chr
            // CalcLang doesn't expose ord/chr — use a lookup string instead.
            let alpha = "abcdefghijklmnopqrstuvwxyz";
            let pos = str_find(alpha, ch);
            let new_pos = (pos + shift) % 26;
            if (new_pos < 0) { new_pos += 26; }
            out += str_at(alpha, new_pos);
        } else {
            out += ch;
        }
    }
    return out;
}

print caesar("hello", 3);      // khoor
print caesar("khoor", -3);     // hello
```

The `str_find(alpha, ch)` trick gets us a 0–25 index without an `ord` builtin. (CalcLang has no character-code accessors; this lookup-table pattern works around the gap cleanly.)

---

## Chapter 5 — Arrays and maps

### Arrays

```calc
let a = [1, 2, 3];
print a;              // [1, 2, 3]
print len(a);         // 3
print a[0];           // 1     — 0-indexed
print a[2];           // 3

a[1] = 20;            // mutating an element
print a;              // [1, 20, 3]
```

Arrays are **heterogeneous** (any element can be any type) and **mutable**:

```calc
let mixed = [1, "two", [3, 4], {"five": 5}, fn(x) { return x; }];
print mixed[0];       // 1
print mixed[1];       // two
print mixed[3].five;  // 5
print mixed[4](99);   // 99
```

#### Reference semantics

Arrays have **reference semantics**: assigning one variable to another, or passing one to a function, does not copy. Both variables reference the same array:

```calc
let a = [1, 2, 3];
let b = a;
b[0] = 99;
print a[0];           // 99      — same underlying array
print a;              // [99, 2, 3]
```

This makes it cheap to pass big arrays around, but you have to think when you want a fresh copy. To copy, build a new array explicitly:

```calc
fn array_copy(a) {
    let out = [];
    for (let i = 0; i < len(a); i = i + 1) { push(out, a[i]); }
    return out;
}

let a = [1, 2, 3];
let b = array_copy(a);
b[0] = 99;
print a[0];           // 1       — unaffected
```

That helper does a **shallow** copy — if the elements are themselves arrays or maps, the inner references are still shared. For full deep copies, recurse.

#### Indexing rules

- 0-indexed.
- Reading an out-of-range index throws a runtime error:
  ```calc
  let a = [1, 2, 3];
  print a[10];      // runtime error: index out of range
  ```
- Negative indices aren't supported. To access the last element, use `a[len(a) - 1]`.
- Writing to `a[len(a)]` is also out of range — use `push(a, x)` to grow.

### Array library

Mutating ops:

```calc
let xs = [];
push(xs, 10);                 // append; returns the new length
push(xs, 20);
push(xs, 30);
print xs;                     // [10, 20, 30]
print pop(xs);                // 30; xs becomes [10, 20]
print xs;                     // [10, 20]
```

Non-mutating ops (return a new array):

```calc
print array_reverse([1, 2, 3]);            // [3, 2, 1]
print array_sort([3, 1, 2]);               // [1, 2, 3]
print array_concat([1, 2], [3, 4]);        // [1, 2, 3, 4]
print array_slice([10, 20, 30, 40], 1, 3); // [20, 30]   — half-open [a, b)
print array_find([10, 20, 30], 20);        // 1          — first index of value, -1 if absent
print array_contains([10, 20, 30], 99);    // 0
print array_range(0, 5);                   // [0, 1, 2, 3, 4]
print array_range(3, 7);                   // [3, 4, 5, 6]
```

`array_sort` sorts numerically when all elements are numbers; lexically when all are strings. Mixed-type arrays fall back to a stable comparison by type tag.

#### Common array patterns

**Map / filter / reduce** (CalcLang doesn't include these as builtins; write them once):

```calc
fn map(xs, f) {
    let out = [];
    for (let i = 0; i < len(xs); i = i + 1) { push(out, f(xs[i])); }
    return out;
}

fn filter(xs, pred) {
    let out = [];
    for (let i = 0; i < len(xs); i = i + 1) {
        if (pred(xs[i])) { push(out, xs[i]); }
    }
    return out;
}

fn reduce(xs, init, f) {
    let acc = init;
    for (let i = 0; i < len(xs); i = i + 1) { acc = f(acc, xs[i]); }
    return acc;
}

print map([1, 2, 3], fn(x) { return x * x; });             // [1, 4, 9]
print filter([1, 2, 3, 4, 5], fn(x) { return x % 2 == 0; }); // [2, 4]
print reduce([1, 2, 3, 4], 0, fn(a, b) { return a + b; });   // 10
```

**2-D arrays (matrices):**

```calc
fn zeros(rows, cols) {
    let m = [];
    for (let r = 0; r < rows; r = r + 1) {
        let row = [];
        for (let c = 0; c < cols; c = c + 1) { push(row, 0); }
        push(m, row);
    }
    return m;
}

let M = zeros(3, 4);
M[1][2] = 99;
print M;
// [[0, 0, 0, 0], [0, 0, 99, 0], [0, 0, 0, 0]]
```

The Achilles' heel: if you build a matrix with shared inner-row references by accident, mutating one row mutates all of them.

```calc
let row = [0, 0, 0];
let bad = [row, row, row];          // three references to the SAME row
bad[0][0] = 99;
print bad;
// [[99, 0, 0], [99, 0, 0], [99, 0, 0]]    — yikes
```

The `zeros` function above avoids this by `let row = [];` inside the outer loop — each row is fresh.

**Stack / queue:**

```calc
// Stack: push/pop are O(1).
let stack = [];
push(stack, "a");
push(stack, "b");
print pop(stack);     // b
print pop(stack);     // a

// Queue: push is O(1), but dequeueing front is O(n) since there's no shift().
// For perf-critical queues, use a circular buffer or two stacks. For most
// engineering code, the simple shift() pattern is fine:
fn shift(xs) {
    if (len(xs) == 0) { throw "shift: empty"; }
    let v = xs[0];
    let next = [];
    for (let i = 1; i < len(xs); i = i + 1) { push(next, xs[i]); }
    return {"value": v, "rest": next};   // doesn't mutate xs; returns the tail
}
```

### Maps

Maps are key/value tables. Keys may be strings or numbers; values may be anything:

```calc
let person = {"name": "Alice", "age": 30};
print person["name"];         // Alice
person["age"] = 31;
print person["age"];          // 31

let words = {1: "one", 2: "two", 3: "three"};
print words[2];               // two
```

Empty literal: `{}`.

#### Print format

Maps print as JSON-ish text. Strings get quoted; numbers, complex, and nested arrays/maps print recursively:

```calc
print {"name": "Bob", "scores": [90, 85], "active": 1};
// {"name": "Bob", "scores": [90, 85], "active": 1}

print {"z": complex(1, 2), "v": [{"x": 1}, {"x": 2}]};
// {"z": (1, 2), "v": [{"x": 1}, {"x": 2}]}
```

Iteration order is **insertion order**: keys come back in the order they were first added.

#### Reference semantics

Like arrays, maps are references — assigning or passing doesn't copy:

```calc
let a = {"count": 0};
let b = a;
b["count"] = 99;
print a["count"];           // 99   — same map
```

#### Missing keys

Reading a missing key throws a runtime error:

```calc
let m = {"x": 1};
print m["y"];          // runtime error: key not found: y
```

Guard with `has_key` first, or wrap in `try`/`catch`:

```calc
if (has_key(m, "y")) { print m["y"]; } else { print "absent"; }
```

Writing to a missing key adds it:

```calc
let m = {};
m["a"] = 10;
m["b"] = 20;
print m;       // {"a": 10, "b": 20}
```

### Field access shorthand

`obj.field` is shorthand for `obj["field"]`. It works on both reads and writes, and composes with `[]` indexing:

```calc
let user = {"name": "Alice", "city": "NYC"};
print user.name;                         // Alice
user.name = "Alicia";
print user.name;                         // Alicia

let team = {"members": [{"name": "Bob"}]};
print team.members[0].name;              // Bob
team.members[0].name = "Bobby";
print team.members[0].name;              // Bobby
```

`.field` only works with identifier-like keys (letters, digits, underscores, no spaces or punctuation). For arbitrary string keys, use `["..."]`:

```calc
let cfg = {};
cfg["db.host"] = "localhost";     // dot-in-key requires bracket form
cfg["api key"] = "abc123";
print cfg["db.host"];
```

### Map library

```calc
let scores = {"alice": 90, "bob": 75};
print len(scores);               // 2
print keys(scores);              // ["alice", "bob"]
print values(scores);            // [90, 75]
print has_key(scores, "alice");  // 1
print has_key(scores, "carol");  // 0
print del(scores, "alice");      // 1   — was present
print del(scores, "alice");      // 0   — already removed
print scores;                    // {"bob": 75}
```

`keys` and `values` return fresh arrays (mutating them doesn't affect the map). `del` returns 1 if the key was present, 0 if not.

#### Iteration

There's no `for k, v in map` form. Walk the keys:

```calc
let prices = {"apple": 1.20, "banana": 0.50, "cherry": 3.00};
let ks = keys(prices);
let total = 0;
for (let i = 0; i < len(ks); i = i + 1) {
    let k = ks[i];
    print k + ": $" + prices[k];
    total += prices[k];
}
print "total: $" + total;
```

#### When to use a map vs. an array

- **Map** when access is by name/key (`scores["alice"]`, `config["timeout"]`).
- **Array** when access is by position (`pixels[100]`, `time_series[t]`).
- For records (a fixed set of named fields), either works. Maps win on readability; arrays win on memory when you have many records of the same shape.

#### Inventory example

```calc
let inv = {};

fn add_item(inv, name, qty) {
    if (has_key(inv, name)) {
        inv[name] = inv[name] + qty;
    } else {
        inv[name] = qty;
    }
}

add_item(inv, "apple", 3);
add_item(inv, "banana", 5);
add_item(inv, "apple", 2);
print inv;        // {"apple": 5, "banana": 5}
```

The mutation in `add_item` works because maps are references — the function modifies the caller's map directly.

#### Word frequency

```calc
fn word_freq(text) {
    let counts = {};
    let words = str_split(text, " ");
    for (let i = 0; i < len(words); i = i + 1) {
        let w = words[i];
        if (has_key(counts, w)) { counts[w] = counts[w] + 1; }
        else                    { counts[w] = 1; }
    }
    return counts;
}

print word_freq("the quick brown fox jumps over the lazy dog the end");
// {"the": 3, "quick": 1, "brown": 1, "fox": 1, "jumps": 1, "over": 1, "lazy": 1, "dog": 1, "end": 1}
```

---

## Chapter 6 — Closures and first-class functions

Functions are values. You can store them in variables, pass them as arguments, return them from other functions, and put them in arrays or maps. This chapter introduces all five.

### Functions as values

A named function's name is just a regular binding to a function value:

```calc
fn add(a, b) { return a + b; }

let f = add;          // f now holds the SAME function value
print f(2, 3);        // 5
print add(2, 3);      // 5 — both work
```

Builtins are values too. `abs`, `sqrt`, `len`, etc. can be referenced by bare name:

```calc
let s = sqrt;
print s(16);          // 4
print type_of(s);     // fn
```

### Anonymous functions

`fn(params) { body }` as an expression is a function literal — useful for one-off callbacks:

```calc
let sq = fn(x) { return x * x; };
print sq(5);          // 25
```

The semicolon after `};` is required when the literal terminates a statement (just like `let x = 5;`).

Anonymous functions also work inline:

```calc
print (fn(a, b) { return a + b; })(3, 4);   // 7
```

— though the named-binding form is usually clearer.

### Higher-order functions

Functions can take other functions as arguments. This is the foundation of `map` / `filter` / `reduce` and of CalcLang's library design (root finders take an `f`, ODE solvers take a vector field, etc.):

```calc
fn map(arr, f) {
    let out = [];
    for (let i = 0; i < len(arr); i = i + 1) {
        push(out, f(arr[i]));
    }
    return out;
}

print map([1, 2, 3, 4], fn(x) { return x * x; });  // [1, 4, 9, 16]
print map([-3, -1, 2], abs);                        // [3, 1, 2] — calclib fn as value
print map([1, 4, 9, 16, 25], sqrt);                 // [1, 2, 3, 4, 5]
```

A function can also **return** a function:

```calc
fn make_multiplier(k) {
    return fn(x) { return k * x; };
}

let triple = make_multiplier(3);
let half   = make_multiplier(0.5);
print triple(7);     // 21
print half(10);      // 5
```

That `make_multiplier` is a textbook closure — the returned function captures `k` from its enclosing scope. We'll come back to capture semantics in a moment.

### Closures

A function defined inside another function captures the surrounding locals. The capture is **by reference**: the inner function sees the *current* value of an outer variable, including changes after the inner function was created.

```calc
fn make_counter() {
    let n = 0;
    fn inc() {
        n = n + 1;       // reads and writes outer `n`
        return n;
    }
    return inc;
}

let c = make_counter();
print c();            // 1
print c();            // 2
print c();            // 3
```

Each `make_counter` call creates a **fresh** `n` and a fresh closure capturing it:

```calc
let a = make_counter();
let b = make_counter();
print a();            // 1
print a();            // 2
print b();            // 1   — independent counter
print a();            // 3
```

The GC keeps the captured cell alive as long as the closure that references it is reachable. Once the last reference is dropped, both go.

Closures can capture **multiple variables**:

```calc
fn make_clamped_counter(lo, hi) {
    let n = lo;
    fn step() {
        n = n + 1;
        if (n > hi) { n = lo; }     // wrap
        return n;
    }
    return step;
}

let s = make_clamped_counter(0, 2);
print s();    // 1
print s();    // 2
print s();    // 0
print s();    // 1
```

Closures nest:

```calc
fn outer(x) {
    fn middle() {
        fn inner() {
            return x;     // captured from outer — two levels up
        }
        return inner;
    }
    return middle;
}
let m = outer(42);
let i = m();
print i();            // 42
```

### The classic loop-capture pitfall

A subtle gotcha when closures capture a loop variable:

```calc
let funcs = [];
for (let i = 0; i < 3; i = i + 1) {
    push(funcs, fn() { return i; });
}
print funcs[0]();    // ?
print funcs[1]();    // ?
print funcs[2]();    // ?
```

In CalcLang each iteration's `let i` is fresh (Chapter 2's loop-body scope rule), so each closure captures its own `i`. The output is `0, 1, 2`. In languages with C-style "one binding per for header," you'd get `3, 3, 3` — make sure you remember which world you're in when porting code.

To capture the **same** outer variable across iterations, declare `i` outside the loop:

```calc
let i = 0;
let funcs = [];
for (i = 0; i < 3; i = i + 1) {
    push(funcs, fn() { return i; });
}
// Now ALL closures share the one `i`.
print funcs[0]();   // 3
print funcs[1]();   // 3
print funcs[2]();   // 3
```

That's rarely what you want, but it's worth knowing the rule.

### Closures over mutable state

A common idiom is to build a small object out of a closure plus some captured private state:

```calc
fn make_accumulator(start) {
    let total = start;
    return {
        "add":   fn(x) { total = total + x; return total; },
        "value": fn()  { return total; },
        "reset": fn()  { total = start; }
    };
}

let acc = make_accumulator(0);
acc["add"](10);
acc["add"](5);
print acc["value"]();    // 15
acc["reset"]();
print acc["value"]();    // 0
```

This is essentially an object literal — same idea as the `class` feature in Chapter 7, just spelled out manually.

### Nested named fns

Inside a function body, `fn name(params) { ... }` is shorthand for `let name = fn(params) { ... };` — same closure machinery, named binding:

```calc
fn make_pair_fn(prefix) {
    fn render(name) {
        return prefix + ": " + name;     // captures `prefix`
    }
    return render;
}
let info = make_pair_fn("INFO");
let warn = make_pair_fn("WARN");
print info("startup");   // INFO: startup
print warn("disk full"); // WARN: disk full
```

### Top-level fn vs. let-bound fn — a caveat

In the **native backend**, top-level `fn name(...)` definitions are compiled to standalone functions that **don't** see top-level `let`s. So this doesn't work:

```calc
let scale = 10;

fn boost(x) {
    return x * scale;       // ERROR: `scale` is not visible here
}
```

But this does:

```calc
let scale = 10;

let boost = fn(x) {
    return x * scale;       // captured via the closure
};

print boost(7);             // 70
```

The rule: if you need a top-level function to read or update module-level state, use a let-bound closure (`let foo = fn(...) { ... };`). Bare top-level `fn` only works for pure functions or ones that take all the state they need via parameters.

This restriction comes from how the native backend allocates symbols. The bytecode VM has slightly different rules; for portability between the two, prefer let-bound closures whenever capturing is involved.

### A worked example: a tiny test harness

Higher-order functions and closures together let you build a domain-specific test runner in a few lines:

```calc
fn run_test(name, body) {
    let failed = 0;
    let assert_eq = fn(actual, expected, label) {
        if (actual != expected) {
            print "  FAIL " + label + ": expected " + expected + ", got " + actual;
            failed = 1;
        }
    };
    body(assert_eq);
    if (failed == 0) { print "PASS " + name; }
    else             { print "FAIL " + name; }
}

run_test("arithmetic", fn(eq) {
    eq(2 + 2, 4, "add");
    eq(7 % 3, 1, "mod");
    eq(2 * 3, 6, "mul");
});

run_test("strings", fn(eq) {
    eq(len("hi"), 2, "len");
    eq(str_upper("abc"), "ABC", "upper");
    eq(str_split("a,b", ",")[1], "b", "split");
});
```

`assert_eq` is a closure over `failed` — calling it from inside `body` flips the flag the runner watches. The pattern shows up over and over in CalcLang code: a "context" object passed to a callback as a closure-over-state.

---

## Chapter 7 — Classes

CalcLang classes are syntactic sugar over the "map + closures" pattern from Chapter 6. The compiler generates the closure boilerplate; you write the methods.

### Defining a class

```calc
class Point {
    fn init(x, y) {
        this.x = x;
        this.y = y;
    }
    fn dist() {
        return sqrt(this.x * this.x + this.y * this.y);
    }
    fn move(dx, dy) {
        this.x += dx;
        this.y += dy;
    }
}
```

The rules:

- `class Name { ... }` declares a constructor. The class itself **becomes a function value** — calling `Name(args)` runs `init` and returns the instance.
- `fn init(params)` is the constructor. It's optional; if absent, `Name()` returns an empty instance.
- Other `fn name(params)` blocks inside the class body are methods.
- Inside any method (including `init`), the keyword `this` refers to the receiver.
- Fields are introduced by **assignment to `this.field`** — usually inside `init`. There's no separate field-declaration syntax.

### Using a class

```calc
let p = Point(3, 4);
print p.x;            // 3
print p.y;            // 4
print type_of(p);     // map     — instances are maps with bound closures
print p.dist();       // 5
p.move(7, 0);
print p.x;            // 10
print p.dist();       // 10.77032961
```

You can mutate fields directly:

```calc
p.x = 100;
print p.dist();       // 100.0799680...
```

And you can pass `Point(...)` around like any other function:

```calc
fn quadrant(make_point, x, y) {
    let p = make_point(x, y);
    if (p.x >= 0 && p.y >= 0) { return "Q1"; }
    if (p.x <  0 && p.y >= 0) { return "Q2"; }
    if (p.x <  0 && p.y <  0) { return "Q3"; }
    return "Q4";
}

print quadrant(Point, 3, 4);     // Q1
print quadrant(Point, -1, -1);   // Q3
```

### Methods are first-class

Extracting a method captures the receiver — you can call it later with no argument and it still knows its `this`:

```calc
let p = Point(3, 4);
let bound = p.dist;
print bound();         // 5     — `bound` remembers it came from `p`

fn apply(f) { return f(); }
print apply(p.dist);   // 5
```

This is how higher-order code naturally consumes class methods — pass `p.dist` straight to `map`, `array_sort`'s comparator, an ODE integrator, etc., without an explicit lambda wrapping it.

### Method bodies are closures

Methods can refer to other methods on `this`:

```calc
class Calculator {
    fn init() {
        this.value = 0;
    }
    fn add(x) { this.value = this.value + x; return this; }
    fn mul(x) { this.value = this.value * x; return this; }
    fn show() { return "value = " + this.value; }
}

let c = Calculator();
c.add(5);
c.mul(3);
c.add(2);
print c.show();        // value = 17
```

Returning `this` from a chainable method enables fluent-style usage:

```calc
print Calculator().add(5).mul(3).add(2).show();   // value = 17
```

### Adding fields outside `init`

There's no required-fields declaration, so a method can introduce new fields any time:

```calc
class Logger {
    fn init() {
        this.lines = [];
    }
    fn log(msg) {
        push(this.lines, msg);
        if (!has_key(this, "first_logged_at")) {
            this.first_logged_at = "now";       // appears on first log call
        }
    }
}

let lg = Logger();
print keys(lg);        // ["lines"]
lg.log("hello");
print keys(lg);        // ["lines", "first_logged_at"]
```

This is flexible but loose — there's no compiler check that every instance has the same shape. Treat it as a feature for prototyping; for production code, set every field you care about in `init` so instances are uniform.

### Public and private

`pub class Foo { ... }` exports the constructor for cross-file linking (Chapter 8). Bare `class` is private:

```calc
pub class Vehicle { fn init(wheels) { this.wheels = wheels; } }
class _LocalHelper { fn init() { this.x = 0; } }   // file-local
```

Method visibility isn't separately controlled — once you have an instance, all methods are callable. Convention: prefix internal-use-only methods with an underscore (`fn _normalize() { ... }`) so callers know they're not part of the API.

### A worked example: a 2-D vector class

```calc
class Vec2 {
    fn init(x, y) {
        this.x = x;
        this.y = y;
    }
    fn add(other)  { return Vec2(this.x + other.x, this.y + other.y); }
    fn sub(other)  { return Vec2(this.x - other.x, this.y - other.y); }
    fn scale(k)    { return Vec2(this.x * k, this.y * k); }
    fn dot(other)  { return this.x * other.x + this.y * other.y; }
    fn norm()      { return sqrt(this.dot(this)); }
    fn normalized() {
        let n = this.norm();
        if (n == 0) { return Vec2(0, 0); }
        return this.scale(1 / n);
    }
    fn to_str() {
        return "(" + this.x + ", " + this.y + ")";
    }
}

let v = Vec2(3, 4);
let w = Vec2(1, 2);
print v.to_str();              // (3, 4)
print v.norm();                // 5
print v.add(w).to_str();       // (4, 6)
print v.normalized().to_str(); // (0.6, 0.8)
print v.dot(w);                // 11
```

Notice this is **immutable** — every `add` / `sub` / `scale` returns a new `Vec2`. That's a stylistic choice (mutation would also work); immutability avoids surprises when vectors are shared between callers.

### A worked example: a polynomial class

```calc
class Poly {
    fn init(coefs) {
        // coefs[i] is the coefficient of x^i.
        this.coefs = coefs;
    }
    fn degree() { return len(this.coefs) - 1; }
    fn eval(x) {
        // Horner's method.
        let n = this.degree();
        let acc = this.coefs[n];
        for (let i = n - 1; i >= 0; i = i - 1) {
            acc = acc * x + this.coefs[i];
        }
        return acc;
    }
    fn add(other) {
        let n = max(this.degree(), other.degree()) + 1;
        let out = [];
        for (let i = 0; i < n; i = i + 1) {
            let a = 0;  if (i < len(this.coefs))  { a = this.coefs[i]; }
            let b = 0;  if (i < len(other.coefs)) { b = other.coefs[i]; }
            push(out, a + b);
        }
        return Poly(out);
    }
    fn deriv() {
        if (this.degree() == 0) { return Poly([0]); }
        let out = [];
        for (let i = 1; i < len(this.coefs); i = i + 1) {
            push(out, i * this.coefs[i]);
        }
        return Poly(out);
    }
}

// p(x) = 1 + 2x + 3x^2
let p = Poly([1, 2, 3]);
print p.eval(0);          // 1
print p.eval(2);          // 17 (= 1 + 4 + 12)
print p.deriv().eval(2);  // 14 (p'(x) = 2 + 6x, p'(2) = 14)

let q = Poly([10, -1, 0, 4]);     // 10 - x + 4 x^3
let r = p.add(q);                 // 11 + x + 3x^2 + 4x^3
print r.coefs;                    // [11, 1, 3, 4]
```

Each method is reused: `eval`, `deriv`, and `add` compose naturally because each operation returns a fresh `Poly`. This is the kind of design CalcLang classes are well suited to — a value-like data type with a small algebra of operations.

### Limitations

CalcLang's class feature is intentionally minimal:

- **No inheritance.** You can't `class Subclass extends Base`. Reuse comes from composition (methods that delegate to other instances) rather than subclassing.
- **No method overloading.** A class has at most one method per name.
- **No static / class-level methods.** Helper functions that don't need `this` are just top-level `fn`s, not class members.
- **No private fields.** Fields are visible to anyone with the instance; convention (`_underscore` for internal use) is what you have.
- **No operator overloading.** `a + b` is concatenation or numeric addition; it can't call a user-defined `add` method.

If any of these matter, build the equivalent on top of the closure pattern in Chapter 6 — that's what `class` does anyway.

### `struct` — typed records without methods

When you want a typed record — a fixed shape, named fields, no methods — `struct` is the dedicated form. It's a class with all-public fields, an auto-generated positional constructor, and no `init` boilerplate:

```calc
struct Point {
    x: num,
    y: num,
}

let p = Point(3, 4);       // positional ctor in declaration order
print p.x;                  // 3
print p.y;                  // 4

p.x = 10;                   // fields are mutable
print p.x;                  // 10
```

Field type annotations are optional — omit them and the field defaults to `any`:

```calc
struct Bag { label, value }       // label: any, value: any
let b = Bag("answer", 42);
print b.label + " = " + b.value;  // answer = 42
```

Structs work just like classes in terms of value passing: an instance is a (boxed) map at the moment, so passing it to a function shares the same instance — mutations in the callee are visible to the caller. Future stages of the typed-record project will lower structs to a flat unboxed memory layout; the surface syntax won't change.

#### When to use `struct` vs. `class`

- **`struct`** when you want a *record*: data with named fields and no behavior. Library result types (`LU`, `QR`, `LinReg`), small composite values (`Point`, `Range`, `Sample`), DTO-ish wrappers around a few numbers.
- **`class`** when you want behavior — methods, `this`-state-with-operations, anything constructor-heavy. `Rng`, `Vec2` with `add`/`dot`/`norm`, polynomial classes.

You can mostly tell which fits by asking: "do I write `fn methodname()` here?" If yes, class. If no, struct.

#### Returning structs from library functions

CalcLang's standard library uses structs for multi-field results so callers can write `fit.slope` instead of `fit["slope"]`. From `lib/stats.calc`:

```calc
pub struct LinReg { slope: num, intercept: num, r2: num }

pub fn linreg(xs, ys) {
    // ... compute slope, intercept, r2 ...
    return LinReg(slope, intercept, r2);
}
```

And the caller:

```calc
import "stats";

let fit = linreg(xs, ys);
print "y = " + fit.slope + " x + " + fit.intercept;
print "R^2 = " + fit.r2;
```

A struct's underlying storage is currently a map, so legacy `fit["slope"]` still works if you have older code — but `.slope` is the documented form.

#### Looking ahead: unboxed structs

This is **Stage 1** of CalcLang's typed-records story. Today, every struct field is a boxed CalcLang Value (a NaN-tagged 64-bit slot) in a map keyed by field name. That's good for ergonomics but bad for tight numerical kernels — a `Point { x: num, y: num }` ought to be 16 bytes of contiguous doubles, not a map header plus two hashed entries.

Stage 2+ will lower struct declarations to a fixed flat layout: each field gets a known byte offset, `p.x` compiles to a `movsd` from `[base + 0]`, and a numeric struct is cache-friendly and SIMD-vectorizable. The same `struct` declaration syntax you write today will produce that layout automatically — you don't have to rewrite anything when the codegen catches up.

---

## Chapter 8 — Multi-file programs

A single `.calc` file works for hundreds of lines. Past that, splitting into modules helps. CalcLang's module system is the simplest possible: one statement, one command, no separate library-build step.

### `import`

Put `import "name";` at the top of any file. The parser inlines the named library's definitions into your program. One command builds the whole thing.

**lib.calc:**

```calc
pub fn add(a, b) {
    return a + b;
}

pub fn greet(name) {
    return "hello, " + name;
}

// Bare `fn` (or `priv fn`) is a file-local helper. Imports inline it
// too — pub bodies often call private helpers — but conventionally it
// stays out of the consumer's working vocabulary.
priv fn _internal_helper(x) {
    return x * 2;
}
```

**main.calc:**

```calc
import "./lib.calc";     // co-located helper (./ + .calc = sibling file)

print add(2, 3);            // 5
print greet("world");       // hello, world
```

Build and run:

```bash
build/calcnat main.calc -o app.exe
./app.exe
# 5
# hello, world
```

No `make libs`, no `--lib`, no `.s` files on the command line. The same model works for the standard engineering libraries:

```calc
import "math";              // lib/math.calc — sq, cube, hypot, lerp, ...
import "stats";             // lib/stats.calc — mean, stddev, linreg, ...
import "nr.brent";          // lib/nr/brent.calc — root finding
```

#### How it works

When the parser sees `import "name";`, it:

1. **Resolves the path** (table below).
2. **Reads and parses the file** in a fresh sub-parser.
3. **Inlines every fn definition** (pub *and* priv) into the current program's AST.
4. **Recursively processes nested imports** with cycle detection.

Then the regular codegen compiles everything as one program. The library's functions get the same `calc_<name>` symbols they'd have as a standalone build; they just live in the consumer's binary.

#### Path resolution

| Form                  | Resolved as                                            |
|-----------------------|--------------------------------------------------------|
| `"math"`              | module spec → `lib/math.calc`                          |
| `"nr.brent"`          | dot-syntax module spec → `lib/nr/brent.calc`           |
| `"nr/brent"`          | slash-syntax (equivalent) → `lib/nr/brent.calc`        |
| `"./helper.calc"`     | importer-relative                                      |
| `"/abs/path.calc"`    | absolute — used as given                               |
| `"lib/math.calc"`     | literal `.calc` path — cwd-relative, then importer-relative |

The "module spec" rule (anything not absolute, not dot-prefixed, not ending in `.calc`, not starting with `lib/`) is the common case: `import "math"` Just Works from anywhere in the project, and nested libraries use Python-style dots (`nr.brent`) that translate to slashes (`nr/brent`) before joining with `lib/`.

#### Cycle detection

Each canonical file path is imported at most once. If `nr/svd.calc` imports `nr.eigen`, and your program also imports `nr.eigen` directly, the eigen module is included once.

```calc
// my_app.calc
import "nr.svd";        // svd.calc itself does `import "nr.eigen";`
import "nr.eigen";      // already in the import set — silently skipped
```

Direct circular imports (`a` imports `b` imports `a`) are also handled.

#### Visibility — `pub` vs. `priv`

A library's `pub` items are its documented API. `priv` items are internal helpers — the import system still inlines them (pub bodies depend on them), but they're not part of what callers should rely on. Conventionally, prefix private helpers with `_`:

```calc
// lib/nr/svd.calc
priv fn _svd_matmul(A, B) { ... }    // internal — don't call from outside
pub  fn svd(A) { ... }                // API — what consumers use
```

If two imports both define the same symbol name, codegen rejects the program at compile time. Pick names that don't collide.

### Top-level state in libraries

A library can declare top-level `let`s for shared constants:

```calc
// lib.calc
let SCALE = 1.5;     // module-level constant

pub fn boost(x) {
    return x * SCALE;     // works
}
```

But there's a catch: a top-level `pub fn` in the native backend doesn't see top-level `let`s. So this won't work the way you might hope:

```calc
// lib.calc — BROKEN PATTERN
let counter = 0;
pub fn bump() {
    counter = counter + 1;     // top-level `fn` can't reach top-level `let`
    return counter;
}
```

For stateful libraries, wrap the state in a class:

```calc
pub class Counter {
    fn init(start) { this.n = start; }
    fn bump() { this.n = this.n + 1; return this.n; }
}
```

Callers do `let c = Counter(0); c.bump();` — the state rides along with the instance.

### Using a class from another file

Classes export the same way as fns:

**shapes.calc:**

```calc
pub class Circle {
    fn init(r) { this.r = r; }
    fn area() { return pi() * this.r * this.r; }
}
```

**main.calc:**

```calc
import "./shapes.calc";

let c = Circle(5);
print c.area();                        // 78.53981634
```

### `extern fn` — the lower-level alternative

`import` is the default. For specialized cases there's also `extern fn`, which declares a single symbol from another compilation unit and links against a pre-built `.s` file:

```calc
// main.calc — explicit declarations, manual link step
extern fn mean(xs: arr): num;
extern fn stddev(xs: arr): num;
```

```bash
build/calcnat --lib lib/stats.calc -o build/stats.s
build/calcnat main.calc build/stats.s -o app.exe
```

Use `extern fn` when you want to:

- **Subset a library's API** (only declare what you name — useful if a lib has 200 fns and you want 3).
- **Use a pre-compiled `.s`** (e.g. a closed-source library shipped as assembly).
- **Share one library binary across many programs** (avoids inlining the lib N times).

For day-to-day work, `import` is what you want.

### Current limitations

- **No selective import** like Python's `from X import Y, Z`. Everything from the imported file comes in.
- **No name aliasing**. If two imports both export `mean`, codegen rejects the program. Pick names that don't collide.
- **No package system**. `import "math"` resolves to `lib/math.calc` under cwd; there's no install-able registry.
- **Library code is inlined per consumer**. If 10 programs all `import "math"`, each binary contains its own copy. Cheap for small libs, matters for very large ones — use `extern fn` + `--lib` to share if it matters.

### A bigger example — splitting an app

Suppose you want to build a small numerical experiment:

**stats.calc:**

```calc
pub fn mean(xs) {
    let s = 0;
    for (let i = 0; i < len(xs); i = i + 1) { s += xs[i]; }
    return s / len(xs);
}

pub fn stddev(xs) {
    let m = mean(xs);
    let s = 0;
    for (let i = 0; i < len(xs); i = i + 1) {
        let d = xs[i] - m;
        s += d * d;
    }
    return sqrt(s / (len(xs) - 1));
}
```

**random.calc:**

```calc
pub class Rng {
    fn init(seed) { this.state = seed; }
    fn next_u32() {
        this.state = (this.state * 1664525 + 1013904223) % 4294967296;
        return this.state;
    }
    fn uniform01() { return this.next_u32() / 4294967296; }
    fn normal(mu, sigma) {
        let u1 = this.uniform01();
        let u2 = this.uniform01();
        if (u1 < 1e-10) { u1 = 1e-10; }
        let r = sqrt(-2 * log(u1));
        return mu + sigma * r * cos(2 * pi() * u2);
    }
}
```

**main.calc:**

```calc
import "./stats.calc";
import "./random.calc";

let rng = Rng(2026);
let samples = [];
for (let i = 0; i < 10000; i = i + 1) {
    push(samples, rng.normal(0, 1));
}
print "mean   = " + mean(samples);        // ~ 0
print "stddev = " + stddev(samples);      // ~ 1
```

Build:

```bash
build/calcnat main.calc -o sim.exe
./sim.exe
# mean   = -0.001234...
# stddev = 0.998765...
```

One command, no per-library `--lib` step. The leading `./` on each import says "look next to main.calc"; without it the parser would search `lib/stats.calc` first.

### Same source, both pipelines

Multi-file programs also work on the bytecode VM, but with the older `extern fn`/`--lib`/per-file `.co` model (the VM's frontend doesn't expand `import` directives across compilation units). For the VM you still write:

```bash
build/calcc   lib.calc  lib.casm
build/calcc   main.calc main.casm
build/calcasm lib.casm  lib.co
build/calcasm main.casm main.co
build/calcld  main.co lib.co  app.cexe     # main MUST come first — its top-level code is the entry
build/calcvm  app.cexe
```

The two pipelines produce byte-identical output for the features they share. For new code, prefer the native backend + `import`.

---

## Chapter 9 — Flow-sensitive types

CalcLang has a **flow-sensitive type inference**: the compiler walks the program once (with fixpoint iteration on loops) and, at each program point, tracks the set of types each in-scope variable could hold. Each *use* of a variable is typed based on its state at that program point, not just its declaration. This catches errors that simpler "last write wins" inference would miss.

### The basics

```calc
let x = 5;                    // x: num
print x + 10;                 // x is num here  — fast numeric add
x = "hello";                  // x: str now
print x + " world";           // x is str here  — string concat
print x + 10;                 // x is str here  — concat with coerced "10"
```

The same source name has different types at different points. The compiler tracks this and uses it to pick the right code path (and skip unnecessary runtime tag checks).

### Branch joins

Each branch of an `if` is analysed separately. At the merge point, the per-branch types are unioned:

```calc
let x = 5;
if (some_cond) {
    x = "hello";              // branch 1: x is str
} else {
    x = 99;                   // branch 2: x is num
}
// after the if: x could be str or num — type is "any"
print type_of(x);             // "str" or "num", depending on which branch ran
```

When two branches set the same variable to different types, the variable's type after the join is the **union**. Further uses go through the polymorphic runtime path instead of the fast type-specialized one.

A narrower form: when both branches agree, the post-join type is precise:

```calc
let x = 0;
if (some_cond) { x = 10; } else { x = 20; }
print x + 1;                  // x is still `num` after — fast path
```

### Loops

`while` and `for` bodies iterate to a fixpoint:

```calc
let acc = 0;                  // acc: num
let i = 0;
while (i < 10) {
    acc = acc + i;            // each iteration: still num
    i = i + 1;
}
print acc;                    // num throughout — fast path
```

If a loop body mutates a variable to a new type, the inference widens accordingly and the post-loop type is the union of pre-loop and per-iteration types. In practice, this matters most when you accidentally mix types in a loop — the compiler will conservatively widen and you'll lose the fast numeric path.

### The `any` escape hatch

A variable declared with `: any` (or with no annotation, in some positions) skips the static type check. The value is still type-tagged at runtime; calls into typed functions get a runtime guard:

```calc
fn need_num(n: num): num { return n + 1; }
fn flex(x): any { return x; }

print need_num(flex(42));       // runtime check: 42 is num → OK
print need_num(flex("hi"));     // runtime error: expected num, got str
```

`any` is useful at API boundaries where you don't know the caller's type. Inside performance-critical inner loops, annotate.

### What the inference does NOT do

- **Type narrowing via `type_of` guards.** Writing `if (type_of(x) == "num") { ... }` does not refine `x`'s static type inside the branch. The check happens at runtime but isn't fed back into the inference.
- **Specialization per call-site.** A polymorphic function is analyzed once with the parameter types you declared (or `any` if you didn't). It isn't recompiled per caller.
- **Tracking element types of arrays / maps.** `arr` and `map` are opaque containers; the inference doesn't track what's inside.

### Why this matters

The inference exists for two reasons:

1. **Catch bugs early.** Passing a string where a num is required is a compile error (if the compiler can tell) rather than a mysterious runtime crash.
2. **Performance.** When the compiler proves `x: num` and `y: num` at the point of `x + y`, the native backend emits a single hardware `addsd` instruction. When it can't, it emits a call to the runtime helper `cl_op_plus`, which type-tag-checks and dispatches. The difference can be 5×–10× on tight inner loops.

For most code you don't need to think about it — the inference works in the background. Annotate hot functions for the performance win; use `any` at the edges where flexibility matters.

---

## Chapter 9.5 — Engineering features

Six additions take CalcLang from "language tutorial" to "thing you can actually do work with": file I/O, shell commands, complex numbers, FFI, exceptions, and tail-call optimization. They're grouped here because they all share two properties: **native-only** (the bytecode VM doesn't support them yet) and **engineering-focused** (most "small language" tutorials don't include them).

### File I/O

```calc
// Write a string to disk (overwrites).
file_write("data.txt", "hello, world\n");

// Append more lines.
file_append("data.txt", "second line\n");

// Read the whole file back.
let content = file_read("data.txt");
print content;

// Check existence before reading.
if (file_exists("config.txt")) {
    let cfg = file_read("config.txt");
    // ...
}
```

`file_write` and `file_append` open the file in binary mode and write the raw bytes — no newline translation, no encoding conversion. `file_read` returns the entire file as a single string. For large files, prefer line-by-line patterns by splitting on `"\n"`:

```calc
let lines = str_split(file_read("input.txt"), "\n");
for (let i = 0; i < len(lines); i = i + 1) {
    print "line " + i + ": " + lines[i];
}
```

When generating large output (a 10 MB SVG, say), don't build the string in memory and write at the end — that's O(n²) due to string immutability. Instead, `file_append` each piece as you produce it:

```calc
file_write("plot.svg", "<svg ...>");
for (let i = 0; i < 10000; i = i + 1) {
    file_append("plot.svg", "<circle cx=\"" + i + "\" .../>");
}
file_append("plot.svg", "</svg>");
```

#### Practical patterns

**Read configuration file into a map:**

```calc
fn read_config(path) {
    let cfg = {};
    let lines = str_split(file_read(path), "\n");
    for (let i = 0; i < len(lines); i = i + 1) {
        let line = str_trim(lines[i]);
        if (line == "" || str_starts_with(line, "#")) { continue; }
        let eq = str_find(line, "=");
        if (eq < 0) { continue; }
        let key = str_trim(str_slice(line, 0, eq));
        let val = str_trim(str_slice(line, eq + 1, len(line)));
        cfg[key] = val;
    }
    return cfg;
}
```

**Append to a log file with a timestamp:**

```calc
fn log(path, msg) {
    file_append(path, msg + "\n");
}

log("app.log", "starting up");
log("app.log", "loaded 42 records");
```

**Atomic file write** (write to temp file, then rename via shell):

```calc
fn atomic_write(path, content) {
    let tmp = path + ".tmp";
    file_write(tmp, content);
    system("move " + tmp + " " + path);    // Windows
    // On Linux: system("mv " + tmp + " " + path);
}
```

### Shell commands

`system(cmd)` runs the command through the OS shell and returns its exit code:

```calc
let rc = system("dir build");
if (rc != 0) {
    print "command failed with exit code " + rc;
}
```

Useful for invoking `gnuplot`, `python`, image converters, or anything else that already exists. CalcLang generates the data file; the external tool reads it.

A typical pattern: write CSV, call gnuplot via a small script:

```calc
file_write("data.csv", "x,y\n");
for (let i = 0; i < 100; i = i + 1) {
    let x = i / 10.0;
    let y = sin(x);
    file_append("data.csv", x + "," + y + "\n");
}
file_write("plot.gp", "set datafile separator ','\nplot 'data.csv' with lines\npause -1\n");
system("gnuplot plot.gp");
```

`system` is a coarse tool — you only get back the exit code, not the stdout / stderr. For "run a command and read its output," write the output to a temp file:

```calc
system("git rev-parse HEAD > /tmp/sha.txt");
let sha = str_trim(file_read("/tmp/sha.txt"));
print "current commit: " + sha;
```

That's clumsy but unambiguous. For more elaborate IPC, use FFI (next section after complex numbers) to call libc's `popen`.

#### Cross-platform concerns

- Path separators: Windows accepts both `/` and `\`. Use `/` for portability.
- Command syntax differs: `dir` vs `ls`, `move` vs `mv`, `type` vs `cat`. Detect at the top of your script (e.g. check for the existence of `/bin/sh`) and branch.
- Quoting: `system` passes the string straight to the OS shell. Spaces in paths need quotes. Be cautious with user-supplied input — `system("ls " + user_path)` is a classic injection bug.

### Terminal I/O — building interactive programs

Three builtins plus the `\e` string escape are enough to write real-time terminal apps — games, dashboards, REPLs that don't want line buffering. All are native-only.

```calc
sleep_ms(n)       // pause for n milliseconds
time_ms()         // wall-clock ms since program start (for game-loop pacing)
read_key()        // non-blocking single-key read — see codes below
```

`read_key()` returns:

- `-1` if no key is currently pressed (so a tight loop won't spin on stdin).
- `0..255` for normal ASCII keys: `'a'` → 97, `' '` → 32, `'\n'` → 10, `Esc` → 27.
- `1001` Arrow Up, `1002` Down, `1003` Left, `1004` Right.
- `2000+` for other special keys (F-keys etc.); avoid the exact codes — they're platform-specific.

The string escape `\e` produces the ANSI escape character (`0x1B`), so you can write color and cursor-control sequences inline:

```calc
print "\e[2J\e[H";                          // clear screen + cursor home
print "\e[31mred\e[0m and \e[42mgreen-bg\e[0m";
print "\e[10;5HHello at row 10 col 5";      // absolute cursor positioning
```

On Windows, `read_key()` automatically enables Virtual Terminal mode the first time it runs, so the same ANSI sequences work in cmd.exe and Windows Terminal. On POSIX, the terminal is put into raw, no-echo mode for the lifetime of the program (and restored at exit).

#### Game-loop pattern

A standard real-time loop on top of these three primitives:

```calc
fn loop() {
    let last_tick = time_ms();
    while (1) {
        // Drain queued input — multiple keypresses can arrive between frames.
        let k = read_key();
        while (k != -1) {
            handle_key(k);
            k = read_key();
        }
        let now = time_ms();
        if (now - last_tick >= 100) {        // physics tick every 100ms
            advance_world();
            last_tick = now;
        }
        render();                             // print frame
        sleep_ms(16);                         // ~60 fps cap
    }
}
```

#### Worked example: Tetris

`examples/tetris.calc` is a complete terminal Tetris built with `read_key`, `sleep_ms`, `time_ms`, and ANSI escapes. About 300 lines total: a `struct Piece` for the seven tetrominoes, a `struct Game` for board + piece state + RNG + score, collision/rotation/line-clear logic, and a render path that builds each frame as one big ANSI string before writing it (single `write` per frame avoids visible tearing).

```bash
build/calcnat examples/tetris.calc -o build/tetris.exe
build/tetris.exe
```

Controls are arrow keys to move/rotate, space to hard-drop, `q` to quit. The "ghost" preview (where the piece would land), the next-piece panel, and the standard level-up speedup are all there.

### Windowed GUI via SDL2

For an actual *windowed* program — pixels, mouse, keyboard, smooth animation — CalcLang ships an SDL2-backed set of builtins. Same pattern as the terminal builtins: pure CalcLang code on top, a small C runtime module (`src/runtime_gui_sdl2.c`) that wraps SDL2's window/renderer/event API.

#### One-time setup

```bash
tools/setup_sdl2.sh        # downloads + unpacks SDL2 into third_party/
make                        # rebuild calcnat to pick up the GUI runtime path
```

The setup script fetches the official SDL2 mingw dev tarball (~13 MB) from `github.com/libsdl-org/SDL`, extracts to `third_party/SDL2-X.X.X/`, and that's it. The vendored tree is gitignored. After this, calcnat auto-detects SDL2 and links every GUI program against it (non-GUI programs aren't affected — the dead-code stripper drops the unused builtins).

When you build a GUI program, calcnat copies `SDL2.dll` next to the output `.exe`, so the resulting executable runs anywhere — no PATH setup needed.

#### Builtins

The whole API is ~17 functions, all named `gui_*`:

| Function                              | What it does                                  |
|---------------------------------------|-----------------------------------------------|
| `gui_init(w, h, title)`               | open the window; returns 1 on success         |
| `gui_close()`                         | destroy window + clean up SDL                 |
| `gui_should_close()`                  | 1 if user closed window or pressed Esc        |
| `gui_poll_events()`                   | pump SDL events; call once per frame          |
| `gui_set_color(r, g, b)`              | set current draw color (each 0..255)          |
| `gui_clear(r, g, b)`                  | fill window with color                        |
| `gui_rect(x, y, w, h)`                | filled rect, current color                    |
| `gui_rect_outline(x, y, w, h)`        | outline rect, current color                   |
| `gui_line(x1, y1, x2, y2)`            | line, current color                           |
| `gui_pixel(x, y)`                     | single pixel, current color                   |
| `gui_present()`                       | swap buffers (display what you drew)          |
| `gui_set_title(s)`                    | change window title (e.g. for score display)  |
| `gui_key_down(name)`                  | 1 if key currently held                       |
| `gui_key_pressed(name)`               | 1 if pressed this frame (one-shot)            |
| `gui_mouse_x() / gui_mouse_y()`       | mouse position in window                      |
| `gui_mouse_down(idx)`                 | mouse button currently held? 0=L, 1=M, 2=R    |
| `gui_mouse_clicked(idx)`              | clicked this frame? (one-shot)                |

Key names for `gui_key_down` / `gui_key_pressed`: `"left"`, `"right"`, `"up"`, `"down"`, `"space"`, `"enter"`, `"esc"`, `"tab"`, `"shift"`, `"ctrl"`, `"alt"`, plus any single character (`"a"`, `" "`, `"7"`, ...).

#### Standard game-loop shape

```calc
import "gui";

gui_init(640, 480, "My program");
let x = 100;

while (!gui_should_close()) {
    gui_poll_events();

    // Update state.
    if (gui_key_down("right")) { x = x + 5; }
    if (gui_key_down("left"))  { x = x - 5; }

    // Render this frame.
    gui_clear(20, 25, 40);                   // dark background
    gui_set_color(220, 80, 120);
    gui_rect(x, 200, 50, 50);
    gui_present();

    sleep_ms(16);                            // ~60 fps cap
}
gui_close();
```

`gui_poll_events()` *must* be called every frame — without it the OS will mark the window unresponsive within a second or two.

#### Widget kit

`lib/gui.calc` builds an immediate-mode widget kit on top of the primitives — buttons, checkboxes, sliders, colored panels:

```calc
import "gui";

gui_init(400, 300, "Widgets demo");
let counter = 0;
let dark = 0;
let volume = 50;

while (!gui_should_close()) {
    gui_poll_events();
    gui_clear_c(col_dark());

    if (button(20, 20, 140, 32, "Click me")) {
        counter = counter + 1;
    }
    dark = checkbox(20, 70, 24, dark);
    volume = slider(60, 130, 200, 24, 0, 100, volume);

    gui_set_title("clicks=" + counter + "  vol=" + floor(volume));
    gui_present();
    sleep_ms(16);
}
gui_close();
```

The kit is intentionally small — `button`, `checkbox`, `slider`, plus color helpers (`col_dark`, `col_panel`, `col_accent`, `rgb(r, g, b)`). Building more widgets on top is straightforward: each is just a function that checks `gui_mouse_*` against its hit rect and draws with `gui_rect`. Look at `lib/gui.calc` for the template.

#### Worked example: GUI Tetris

`examples/tetris_gui.calc` is the same Tetris game logic as `examples/tetris.calc`, but the render pass uses `gui_rect`/`gui_rect_outline` instead of ANSI block printing, and input comes from `gui_key_pressed` instead of `read_key`. Score / lines / level go in the window title bar — Stage 1 of the GUI runtime doesn't render text yet (a bitmap font is a future stage).

```bash
build/calcnat examples/tetris_gui.calc -o build/tetris_gui.exe
build/tetris_gui.exe
```

#### Cross-platform note

SDL2 itself is cross-platform — Windows, macOS, Linux, even mobile and the web (via Emscripten). What's currently Windows-only is the vendored mingw-built SDL2 tree under `third_party/`. To use the GUI on macOS or Linux, run `brew install sdl2` / `apt install libsdl2-dev`, then point `calcnat` at the system SDL2 by editing the `sdl_root` path in `src/calcnat.c` (or set `CALC_SDL2_ROOT`). A proper auto-detect path is a small future cleanup; the bones are in place.

### Complex numbers

A first-class type: `complex(re, im)` builds one; arithmetic operators do the right thing.

```calc
let z = complex(3, 4);
print z;                  // (3, 4)
print real(z);            // 3
print imag(z);            // 4
print abs(z);             // 5            — magnitude
print arg(z);             // 0.9272952180  — phase, in radians
print conj(z);            // (3, -4)
print type_of(z);         // cpx
```

The operators are polymorphic: `+ - * /` work on any combination of `num` and `cpx`, auto-promoting reals to `re + 0i`:

```calc
let z = complex(3, 4);
let w = complex(1, 2);
print z + w;              // (4, 6)
print z - w;              // (2, 2)
print z * w;              // (-5, 10)
print z / w;              // (2.2, -0.4)
print 2 * z;              // (6, 8)
print z + 1;              // (4, 4)
print -z;                 // (-3, -4)
print complex(0, 1) * complex(0, 1);   // (-1, 0)   — i² = -1
```

Equality compares by components; ordering (`<`, `>`, etc.) on complex values raises a runtime error (complex numbers don't have a natural total order).

`abs` and `conj` accept either `num` or `cpx`, so generic code that treats a number as "just a complex with zero imaginary" works uniformly.

```calc
// Roots of unity (the nth roots of 1).
fn roots_of_unity(n) {
    let out = [];
    let two_pi = 2 * pi();
    for (let k = 0; k < n; k = k + 1) {
        let theta = two_pi * k / n;
        push(out, complex(cos(theta), sin(theta)));
    }
    return out;
}

let r = roots_of_unity(4);
print r;        // [(1, 0), (6.12e-17, 1), (-1, 1.22e-16), (-1.83e-16, -1)]
                //                              (close enough to 1, i, -1, -i)
```

#### When complex matters

CalcLang's complex support is what makes FFT, polynomial root finding, and frequency-domain filtering possible without hand-packing `{re, im}` pairs into maps. The standard library's `lib/fft.calc` evaluates the Cooley-Tukey radix-2 FFT directly using `complex(...)` and the polymorphic arithmetic operators — the code reads like the math.

```calc
// Snippet from lib/fft.calc (paraphrased):
fn fft_step(x) {
    let n = len(x);
    if (n == 1) { return x; }
    let even = fft_step(get_evens(x));
    let odd  = fft_step(get_odds(x));
    let out = ...;
    for (let k = 0; k < n / 2; k = k + 1) {
        let theta = -2 * pi() * k / n;
        let w = complex(cos(theta), sin(theta));       // twiddle factor
        out[k]         = even[k] + w * odd[k];
        out[k + n / 2] = even[k] - w * odd[k];
    }
    return out;
}
```

The same calclib `+`, `*`, `-` work on real and complex operands; nothing in the algorithm has to special-case the type. That's the design payoff.

#### A type-inference gotcha (now fixed)

Earlier versions of CalcLang's type inferrer wrongly marked any `complex / complex` result as `num`, which made the native backend emit a hardware divide on NaN-tagged complex bits and silently produce `nan`. This is fixed (see CHANGELOG); the inferrer now widens to `any` whenever either operand isn't statically `num`, dispatching through the polymorphic runtime helper. Worth knowing because **any** library doing complex arithmetic in tight inner loops benefits from the fix — and any custom code you write probably will too.

### FFI — calling C libraries

`ffi_load(path)` loads a shared library (`.dll` on Windows, `.so` on Linux, `.dylib` on macOS) and returns a handle. `ffi_call(lib, name, sig, args)` resolves a symbol and invokes it after marshalling arguments according to `sig`.

```calc
let lib = ffi_load("msvcrt.dll");          // libc on Windows; "libm.so.6" on Linux
print ffi_call(lib, "sqrt", "d:d", [16]);  // 4
print ffi_call(lib, "pow",  "d:dd", [2, 10]);  // 1024
print ffi_call(lib, "strlen", "i:s", ["hello"]);  // 5
```

The signature format is `"<ret>:<args>"`:

| code | C type        | CalcLang value     |
|------|---------------|--------------------|
| `v`  | `void`        | (return only)      |
| `d`  | `double`      | num                |
| `i`  | `int`         | num (truncated)    |
| `s`  | `const char*` | str (or returned)  |

So `"d:dd"` is "double-returning function taking two doubles", `"i:s"` is "int-returning function taking a C string", `"v:i"` is "void function taking an int". The supported signatures cover up to 4 arguments and the common math/string/system patterns. More exotic shapes (pointer-to-array, structs by value, variadic) would need libffi or a custom thunk; not in this round.

#### First-class FFI

Wrap an `ffi_call` in a closure and you get a regular CalcLang function value — passable to higher-order code, storable in maps, etc.:

```calc
let lib = ffi_load("msvcrt.dll");
let csqrt = fn(x) { return ffi_call(lib, "sqrt", "d:d", [x]); };
let cpow  = fn(b, e) { return ffi_call(lib, "pow", "d:dd", [b, e]); };

fn map(arr, f) {
    let out = [];
    for (let i = 0; i < len(arr); i = i + 1) {
        push(out, f(arr[i]));
    }
    return out;
}

print map([1, 4, 9, 16, 25], csqrt);  // [1, 2, 3, 4, 5]

let math = {"sqrt": csqrt, "pow": cpow};
print math["sqrt"](64);                // 8
```

A note on scoping: a top-level `fn foo(...)` does NOT see top-level `let`s — top-level fns are compiled as standalone units. Use a let-bound closure (`let foo = fn(...) { ... };`) when you need to capture top-level state, including library handles.

#### Engineering math example

```calc
let m = ffi_load("msvcrt.dll");

// Wrap each libc fn as a first-class CalcLang fn.
let cs = {
    "sqrt":  fn(x)    { return ffi_call(m, "sqrt",  "d:d",  [x]); },
    "sin":   fn(x)    { return ffi_call(m, "sin",   "d:d",  [x]); },
    "cos":   fn(x)    { return ffi_call(m, "cos",   "d:d",  [x]); },
    "atan2": fn(y, x) { return ffi_call(m, "atan2", "d:dd", [y, x]); }
};

// Use them like native functions.
print cs["sqrt"](2);                     // 1.414213562
print cs["atan2"](1, 1);                 // 0.7853981634
```

#### Limitations

- Only basic types: `d` (double), `i` (int), `s` (C string), `v` (void).
- Maximum 4 arguments per call.
- No struct-by-value, no variadic, no pointer-to-array, no callback-passing.
- The library handle from `ffi_load` is not closed automatically; the OS reclaims it on process exit.

For the small set of "call a C math/string function" use cases this covers everything you need. Anything more elaborate would need libffi or a custom shim DLL; that's a future-phase project.

#### Cross-platform FFI

- **Windows**: `msvcrt.dll` for libc, `user32.dll`/`kernel32.dll` for Win32 APIs.
- **Linux**: `libm.so.6` for math, `libc.so.6` for the rest.
- **macOS**: `libSystem.dylib` for both.

Wrap the load in a helper and branch on `system("uname")` if you need portability.

### Exceptions

`throw expr;` raises an exception carrying the value of `expr` (any Value — string, number, map, complex, anything). `try { ... } catch (name) { ... }` installs a handler: if the body throws, the catch block runs with `name` bound to the thrown value. If no handler is active, the program prints `"uncaught exception: <value>"` and exits with code 1.

```calc
try {
    throw "boom";
} catch (e) {
    print "caught: " + e;        // caught: boom
}

// Throwing across function call boundaries — frames in between are unwound.
fn safe_div(a, b) {
    if (b == 0) {
        throw {"code": "DIV_ZERO", "lhs": a};
    }
    return a / b;
}

try {
    print safe_div(10, 2);       // 5
    print safe_div(7, 0);        // never returns; throws past safe_div's frame
    print "unreachable";
} catch (err) {
    print "failed with code " + err["code"];   // failed with code DIV_ZERO
}
```

Nested `try`/`catch` works. A `throw` inside a catch handler is caught by the next outer handler (or escapes the program if none).

```calc
try {
    try {
        throw "inner";
    } catch (e) {
        print "inner: " + e;
        throw "rethrown";        // surfaces to the outer handler
    }
} catch (e) {
    print "outer: " + e;         // outer: rethrown
}
```

Notes:

- The thrown value can be anything — use a string for simple errors, a map for structured ones (`{"code": ..., "message": ...}`), a complex if that's what you have. Whatever you throw is what `catch` binds.
- Unwinding restores `rsp` and `rbp` to their values at the start of the matching `try`. Local variables in the unwound frames simply become unreachable; the GC reclaims them at the next cycle.
- Like the other native-only features in this round, exceptions don't exist in the VM yet.
- TCO ([next section](#tail-call-optimization)) and exceptions interact correctly: a tail-recursive function that throws is unwound the same way as any other.

#### When to use exceptions vs. return values

Exceptions are for **exceptional** conditions — things you don't expect on the happy path. The CalcLang engineering library uses them for:

- Numerical failures: `chol_decompose` throws on a non-SPD matrix, `lu_decompose` throws on a singular matrix.
- Programmer errors: `array_pop` of an empty array throws.
- Resource problems: file open failures, FFI library not found.

For normal flow control (a parser that didn't match this rule but might match the next), use return values or a result map (`{"ok": 0, "error": "..."}`). Exceptions are heavy: each `throw` unwinds the stack and incurs overhead the regular path doesn't pay.

#### Worked example: a small JSON-ish parser with backtracking

```calc
fn parse_int(s, pos) {
    let n = 0;
    let start = pos;
    while (pos < len(s) && str_at(s, pos) >= "0" && str_at(s, pos) <= "9") {
        n = n * 10 + to_num(str_at(s, pos));
        pos = pos + 1;
    }
    if (pos == start) { throw "no digits at position " + start; }
    return {"value": n, "pos": pos};
}

fn try_parse(s, pos) {
    try {
        let r = parse_int(s, pos);
        return {"ok": 1, "value": r["value"], "pos": r["pos"]};
    } catch (e) {
        return {"ok": 0, "error": e};
    }
}

print try_parse("42 hello", 0);     // {"ok": 1, "value": 42, "pos": 2}
print try_parse("hello", 0);        // {"ok": 0, "error": "no digits at position 0"}
```

The `parse_int` function uses `throw` for "didn't match"; the caller catches that and turns it back into a structured "ok/error" result. That's a clean way to compose recoverable failures.

### Tail-call optimization

A function that ends with `return f(args);` where `f` is the function we're already inside doesn't allocate a new stack frame. The codegen recognises the pattern and turns the call into a jump back to the start of the function's body, after rewriting the parameter slots with the new argument values.

```calc
fn count_down(n) {
    if (n <= 0) { return 0; }
    return count_down(n - 1);    // self-tail call — compiled as a jump
}
print count_down(1000000);       // 0   (one million iterations, flat stack)

fn fact_acc(n, acc) {
    if (n <= 1) { return acc; }
    return fact_acc(n - 1, n * acc);
}
print fact_acc(20, 1);           // 2.43e+18
```

What counts as a tail call:
- The expression has to be a *direct* call to the current function — `return foo(...)` inside `fn foo(...) { ... }`.
- All the arg expressions are evaluated first (into temporary stack slots), then copied to the parameter slots in one pass, so an arg can safely reference the current parameter value.
- If the current function captures any of its parameters in an inner closure, TCO is suppressed — re-using the same boxes across "iterations" would break the closure-per-call semantics. In that case the call goes through the normal call/return path.

What's **not** a tail call:
- `return x + foo(...)` — the result of `foo` feeds into `+`, so the call isn't in tail position.
- `return foo(...) + 1` — same.
- `return other_fn(...)` where `other_fn` is a different function — mutual tail recursion isn't optimized (yet).
- `return f(...)` where `f` is a variable holding a function value — indirect tail calls aren't optimized (yet).
- Calls anywhere other than directly under a `return`.

For now the optimization covers the most common case (iterative algorithms expressed as self-recursion); mutual and indirect tail calls are deferred to a future phase.

#### Worked example: iterative summation as tail recursion

```calc
fn sum_to(n, acc) {
    if (n <= 0) { return acc; }
    return sum_to(n - 1, acc + n);   // tail call — flat stack
}
print sum_to(1000000, 0);            // 500000500000
```

A million-deep recursion would overflow without TCO. With it, the function runs in constant stack space — equivalent to writing the loop explicitly. Use this when an iterative algorithm naturally has more than one piece of state (current value, accumulator, …) and the tail-recursive form is clearer than a `while` loop with manual state.

#### Mental model: "TCO turns return-of-call into goto-start"

```
return f(args);                         becomes:                    set params = args;
                                                                    goto fn-entry;
```

This works whether `f` is a self-call or a different function — but the current backend only optimizes self-calls. Mutual recursion still allocates a frame per call.

### Why this matters for engineering work

With these three additions, you can:

- Write a `linalg.calc` library: matrices as arrays-of-arrays, operations defined on top.
- Write a `plot.calc` library: stream SVG to a file as you generate it, or write CSV and shell out to gnuplot.
- Do signal-processing math directly: complex arithmetic without manually packing `{re, im}` pairs.

The next phases of the language (tail-call optimization, exceptions, dynamic linking / FFI) extend what you can express; these three are about *interoperating* with the outside world.

---

## Chapter 10 — calclib reference

Every builtin is a regular function value. It can be called directly (`sqrt(x)`), stored in a variable (`let f = sqrt;`), or passed as a higher-order argument (`map(xs, sqrt)`).

### Math

| Function       | What it does                                              |
|----------------|-----------------------------------------------------------|
| `sqrt(x)`      | √x                                                        |
| `floor(x)`     | round toward −∞                                           |
| `ceil(x)`      | round toward +∞                                           |
| `abs(x)`       | magnitude (works on num AND cpx)                          |
| `pow(b, e)`    | b<sup>e</sup>                                             |
| `min(a, b)`    | numeric min (two args, not array)                         |
| `max(a, b)`    | numeric max                                               |
| `int(x)`       | truncate toward zero (drop fractional part)               |
| `round(x)`     | round to nearest integer (banker's rounding via libc)     |
| `sin(x)`       | sin (radians)                                             |
| `cos(x)`       | cos (radians)                                             |
| `tan(x)`       | tan (radians)                                             |
| `asin(x)`      | inverse sin                                               |
| `acos(x)`      | inverse cos                                               |
| `atan(x)`      | inverse tan                                               |
| `atan2(y, x)`  | quadrant-aware inverse tan                                |
| `exp(x)`       | e<sup>x</sup>                                             |
| `log(x)`       | natural log                                               |
| `log10(x)`     | base-10 log                                               |
| `random()`     | uniform [0, 1) — uses `rand()`; not reproducible          |
| `pi()`         | π                                                         |
| `e()`          | Euler's number                                            |

**Examples:**

```calc
print sqrt(2);              // 1.414213562
print pow(2, 10);           // 1024
print floor(3.7);           // 3
print floor(-3.7);          // -4   (NOT -3 — truncation toward -inf)
print int(-3.7);            // -3   (truncation toward zero)
print min(5, 3);            // 3
print abs(-7);              // 7
print abs(complex(3, 4));   // 5    (same name — works on cpx)
print atan2(1, 1) * 4;      // 3.141592654    (== pi)
print log(e());             // 1
```

For reproducible random numbers, use `lib/random.calc`'s `Rng` class instead of the bare `random()` builtin.

### Strings

| Function                  | What it does                                             |
|---------------------------|----------------------------------------------------------|
| `len(s)`                  | byte length                                              |
| `str_at(s, i)`            | single-byte substring at index i                         |
| `str_slice(s, a, b)`      | half-open slice [a, b)                                   |
| `str_find(s, sub)`        | first index of sub, or -1                                |
| `str_upper(s)`            | uppercase (ASCII)                                        |
| `str_lower(s)`            | lowercase (ASCII)                                        |
| `str_trim(s)`             | strip leading/trailing whitespace                        |
| `str_repeat(s, n)`        | n concatenations of s                                    |
| `str_starts_with(s, p)`   | 1 if s starts with p                                     |
| `str_ends_with(s, p)`     | 1 if s ends with p                                       |
| `str_split(s, sep)`       | array of substrings                                      |
| `str_join(arr, sep)`      | concatenate with sep between                             |
| `to_str(x)`               | any value → string (uses print formatting)               |
| `to_num(s)`               | string → number (0 on failure)                           |

**Examples:**

```calc
let path = "/var/log/app.log";
print str_split(path, "/");                  // ["", "var", "log", "app.log"]

let name = "  Alice  ";
print "[" + str_trim(name) + "]";            // [Alice]

print str_repeat("-=", 5);                   // -=-=-=-=-=

let csv = "alice,30,NYC";
let row = str_split(csv, ",");
let m = {"name": row[0], "age": to_num(row[1]), "city": row[2]};
print m;                                     // {"name": "alice", "age": 30, "city": "NYC"}
```

### Arrays

| Function                       | What it does                                        |
|--------------------------------|-----------------------------------------------------|
| `len(arr)`                     | element count                                       |
| `push(arr, v)`                 | append v; returns new length                        |
| `pop(arr)`                     | remove and return last element                      |
| `array_reverse(arr)`           | new array, reversed                                 |
| `array_sort(arr)`              | new array, sorted (numeric or lexical)              |
| `array_concat(a, b)`           | new array = a ++ b                                  |
| `array_slice(arr, a, b)`       | half-open slice [a, b)                              |
| `array_find(arr, v)`           | first index of v, or -1                             |
| `array_contains(arr, v)`       | 1 if v ∈ arr, else 0                                |
| `array_range(lo, hi)`          | [lo, lo+1, ..., hi-1]                               |

**Examples:**

```calc
let xs = [3, 1, 4, 1, 5, 9, 2, 6];
print array_sort(xs);                        // [1, 1, 2, 3, 4, 5, 6, 9]
print array_reverse(xs);                     // [6, 2, 9, 5, 1, 4, 1, 3]
print array_slice(xs, 2, 5);                 // [4, 1, 5]
print array_find(xs, 9);                     // 5
print array_contains(xs, 99);                // 0

let evens = [];
for (let i = 0; i < 20; i = i + 2) { push(evens, i); }
print evens;                                 // [0, 2, 4, ..., 18]
```

Note: `array_sort` doesn't accept a custom comparator. For non-default sort orders, use `lib/nr/sort.calc`'s `heapsort_idx` to get a permutation, or sort a derived key array.

### Maps

| Function          | What it does                                                |
|-------------------|-------------------------------------------------------------|
| `len(map)`        | number of keys                                              |
| `keys(m)`         | array of keys (insertion order)                             |
| `values(m)`       | array of values (insertion order)                           |
| `has_key(m, k)`   | 1 if k present, else 0                                      |
| `del(m, k)`       | remove k; returns 1 if was present, 0 if not                |

**Examples:**

```calc
let m = {"a": 1, "b": 2, "c": 3};
print keys(m);                  // ["a", "b", "c"]
print values(m);                // [1, 2, 3]
print has_key(m, "b");          // 1
print del(m, "b");              // 1
print m;                        // {"a": 1, "c": 3}
print del(m, "x");              // 0

// Iterate with keys.
let ks = keys(m);
for (let i = 0; i < len(ks); i = i + 1) {
    print ks[i] + " -> " + m[ks[i]];
}
```

### I/O and introspection

| Function           | What it does                                                |
|--------------------|-------------------------------------------------------------|
| `read_line()`      | read a line from stdin; returns "" on EOF                   |
| `write(x)`         | print without newline                                       |
| `type_of(x)`       | type tag string: `"num"`, `"str"`, `"arr"`, `"map"`, `"fn"`, `"cpx"` |

**Examples:**

```calc
// Echo loop.
while (1) {
    let line = read_line();
    if (line == "") { break; }
    print "got: " + line;
}

// Dispatch on type.
fn show(v) {
    let t = type_of(v);
    if (t == "num") { print "number " + v; }
    else if (t == "str") { print "string '" + v + "'"; }
    else if (t == "arr") { print "array of " + len(v); }
    else { print t + " value"; }
}
show(42);                 // number 42
show("hi");               // string 'hi'
show([1, 2, 3]);          // array of 3
show({"a": 1});           // map value
```

### File I/O and shell *(native-only)*

| Function                       | What it does                                        |
|--------------------------------|-----------------------------------------------------|
| `file_read(path)`              | whole file → string                                 |
| `file_write(path, content)`    | overwrite file with content                         |
| `file_append(path, content)`   | append content to file                              |
| `file_exists(path)`            | 1 if openable, 0 otherwise                          |
| `system(cmd)`                  | run shell command; return exit code                 |
| `sleep_ms(n)`                  | pause for n milliseconds                            |
| `time_ms()`                    | ms since program start (for game-loop pacing)       |
| `read_key()`                   | non-blocking key read; -1 if none, 1001-1004 = arrows |

**Examples:**

```calc
// Round-trip.
file_write("greet.txt", "hello\nworld\n");
print file_read("greet.txt");

// Streaming write.
file_write("out.csv", "x,y\n");
for (let i = 0; i < 100; i = i + 1) {
    file_append("out.csv", i + "," + (i * i) + "\n");
}

// External tool.
let rc = system("gcc --version");
print "gcc returned " + rc;
```

### Character helpers (`ctype`-style)

CalcLang has byte-oriented strings, so it's natural to have the C-style character predicates:

| Function              | What it does                                |
|-----------------------|---------------------------------------------|
| `is_digit(s)`         | 1 if `s[0]` is `0–9`                        |
| `is_alpha(s)`         | 1 if `s[0]` is a letter                     |
| `is_alnum(s)`         | 1 if letter or digit                        |
| `is_space(s)`         | 1 if whitespace (space, tab, \n, \r, \f, \v)|
| `is_upper(s)` / `is_lower(s)` | uppercase / lowercase test          |
| `char_to_upper(s)`    | `s[0]` converted to uppercase (returns 1-char string) |
| `char_to_lower(s)`    | same, but lowercase                         |
| `char_code(s)`        | ASCII / byte value of `s[0]` (the `ord`)    |
| `char_from(n)`        | byte → 1-char string (the `chr`)            |

Each operates on a single-byte string (`str_at(s, i)` returns one of these). Non-character inputs raise at runtime.

**Examples:**

```calc
print is_digit("5");           // 1
print char_code("A");          // 65
print char_from(48);           // "0"

// Caesar cipher, no lookup table needed:
fn caesar(s, shift) {
    let out = "";
    for (let i = 0; i < len(s); i = i + 1) {
        let ch = str_at(s, i);
        if (is_lower(ch)) {
            let code = char_code(ch) - char_code("a");
            code = (code + shift + 26) % 26;
            out += char_from(code + char_code("a"));
        } else {
            out += ch;
        }
    }
    return out;
}
print caesar("hello", 3);    // khoor
print caesar("khoor", -3);   // hello
```

### `fmt` — printf-style formatting

`print "x = " + x` uses `%.10g` whether you want it or not. `fmt(format, args)` gives you the full C-style format-string toolbox:

```calc
extern fn fmt(format: str, args: arr): str;     // (always available; no import)

print fmt("hello %s, you are %d years old", ["Alice", 30]);
print fmt("pi = %.4f", [pi()]);                     // pi = 3.1416
print fmt("%5d|%-5d|%05d", [1, 2, 3]);              //     1|2    |00003
print fmt("hex: 0x%x  binary: 0b%b", [255, 10]);    // hex: 0xff  binary: 0b1010
print fmt("%.2f %% complete", [37.5]);              // 37.50 % complete
```

Supported specifiers:

| Spec | Meaning                                       |
|------|-----------------------------------------------|
| `%d` / `%i` | signed integer (operand cast to int64) |
| `%f`  | fixed-point float                            |
| `%e` / `%E` | scientific                             |
| `%g` / `%G` | shortest representation (like `print`) |
| `%x` / `%X` | hex (lowercase / uppercase)            |
| `%o`  | octal                                        |
| `%b`  | binary                                       |
| `%s`  | string — numbers auto-stringify via `%.10g` |
| `%c`  | character (num as ASCII code, or first char of str) |
| `%%`  | literal `%`                                  |

Flags: `-` (left-align), `0` (zero-pad), `+` (sign), space (leading space for positive), `#` (alternate form). Width is a decimal number; precision is `.N`. Examples:

```calc
fmt("%-10s | %8.2f", ["item",   3.14])     // "item       |     3.14"
fmt("%+d %+d", [42, -5])                   // "+42 -5"
fmt("%08.3f", [pi()])                      // "0003.142"
```

The arg array's length must be at least the number of specifiers in the format. Extra args are ignored. Type mismatches (`%d` with a string) raise at runtime.

#### Rust-style `{}` placeholders

`fmt` also recognizes Rust-style `{}` and `{:spec}` placeholders, mixed freely with `%`:

```calc
print fmt("hello {}, age {}", ["Alice", 30]);     // hello Alice, age 30
print fmt("pi = {:.4f}", [pi()]);                  // pi = 3.1416
print fmt("0x{:x}  0b{:b}", [255, 10]);            // 0xff  0b1010
print fmt("rust {}, c {:5d}", ["wins", 99]);       // rust wins, c    99
print fmt("literal {{ and }}", []);                // literal { and }
```

Default placeholder `{}` formats any value (numbers via `%.10g`, strings as-is). Width and precision use the `:N`/`:.N` syntax; an explicit type at the end (`{:5d}`, `{:.2f}`) overrides the default. `{{` and `}}` escape to literal braces.

Internally this is implemented by translating `{}` syntax to the equivalent `%` form before formatting — so anything you can do with `%` you can do with `{}`.

#### `printf` and `println` statement forms

The pattern `print fmt(format, [a, b, c])` is verbose for the common case. Two parser-level conveniences avoid the array wrap and the explicit `fmt` call:

```calc
printf  "x = %d, y = %.2f\n", x, y;       // C-style: no trailing newline added
println "x = {}, y = {:.2f}",  x, y;       // Rust-style: trailing newline added
```

Both desugar to a single statement. The args after the format string become an implicit array passed to `fmt`. `println` adds the trailing newline (using the normal `print` mechanism); `printf` does not (uses `write`, so the format controls its own newlines).

Use which feels natural for the context — they're shorthand, not separate machinery.

#### Common uses

- **Logging with timestamps and levels:**
  ```calc
  let log = fn(level, msg) {
      file_append("app.log", fmt("[%s] [%s] %s\n", [now, level, msg]));
  };
  ```
- **CSV / JSON writers** with controlled precision:
  ```calc
  file_append("data.csv", fmt("%d,%.6f,%.6f\n", [t, x, y]));
  ```
- **Aligned column output:**
  ```calc
  for (let i = 0; i < len(rows); i = i + 1) {
      print fmt("%-20s %8.2f %5d",
          [rows[i]["name"], rows[i]["price"], rows[i]["qty"]]);
  }
  ```

### Complex numbers *(native-only)*

| Function          | What it does                                            |
|-------------------|---------------------------------------------------------|
| `complex(re, im)` | construct a `cpx` value                                 |
| `real(v)`         | real part (accepts num too — returns it unchanged)      |
| `imag(v)`         | imaginary part (zero for num)                           |
| `conj(v)`         | complex conjugate                                       |
| `arg(v)`          | phase angle in radians                                  |
| `abs(v)`          | magnitude (overloaded — `|x|` for nums)                 |

**Examples:**

```calc
let z = complex(3, 4);
print real(z);                  // 3
print imag(z);                  // 4
print abs(z);                   // 5
print conj(z);                  // (3, -4)
print arg(z);                   // 0.927  (atan2(4, 3))

let one_i  = complex(0, 1);
print one_i * one_i;            // (-1, 0)     — i^2 = -1
print one_i + 1;                // (1, 1)      — auto-promotion
print 2 * z;                    // (6, 8)
print z / complex(1, 1);        // (3.5, 0.5)
```

### FFI *(native-only)*

| Function                            | What it does                                  |
|-------------------------------------|-----------------------------------------------|
| `ffi_load(path)`                    | load shared library; return handle            |
| `ffi_call(lib, name, sig, args)`    | invoke `name` from `lib` with arguments       |

Sig codes: `d` (double), `i` (int), `s` (C string), `v` (void). Format: `"<ret>:<args>"`, up to 4 args.

---

## Chapter 10.5 — Engineering library

Three CalcLang modules under `lib/` give you the basics for numerical and visualization work, written entirely on top of the language features we've built up so far.

### `lib/math.calc` — convenience math helpers

The calclib builtins cover the standard scalar math (`sqrt`, `pow`, trig, `log`, etc.). `lib/math.calc` adds the small helpers that come up over and over in numerical code:

```calc
extern fn sq(x: num): num;
extern fn cube(x: num): num;
extern fn power(b: num, e: num): num;        // alias for pow
extern fn power_int(b: num, n: num): num;    // O(log n) via repeated squaring
extern fn nth_root(x: num, n: num): num;
extern fn cbrt(x: num): num;
extern fn hypot(x: num, y: num): num;        // overflow-safe sqrt(x^2 + y^2)
extern fn sign(x: num): num;
extern fn sinh(x: num): num;  extern fn cosh(x: num): num;  extern fn tanh(x: num): num;
extern fn asinh(x: num): num; extern fn acosh(x: num): num; extern fn atanh(x: num): num;
extern fn deg_to_rad(d: num): num;
extern fn rad_to_deg(r: num): num;
extern fn log_base(x: num, b: num): num;
extern fn log2(x: num): num;
extern fn clamp(x: num, lo: num, hi: num): num;
extern fn lerp(a: num, b: num, t: num): num;
extern fn inv_lerp(a: num, b: num, x: num): num;
extern fn remap(x: num, a1: num, b1: num, a2: num, b2: num): num;
```

A few highlights:

- **`sq`, `cube`** — plain multiplies, faster and clearer than `pow(x, 2)` / `pow(x, 3)` for the common cases.
- **`power_int(b, n)`** — exponentiation by squaring: O(log |n|) multiplies, no libc `pow()` call. Faster and more exact than `pow(b, n)` when n is an integer. Handles negative n by inverting.
- **`nth_root`** — handles negative x correctly for odd integer n (e.g. `nth_root(-8, 3) == -2`).
- **`hypot`** — uses the scale-then-multiply trick so `hypot(1e150, 1e150)` returns `~1.414e150` instead of overflowing to infinity.
- **`tanh`** — uses the `e^(-2x) / (1 + e^(-2x))` form on the positive branch to stay numerically stable for large |x|.
- **`clamp`, `lerp`, `inv_lerp`, `remap`** — bread-and-butter for plotting, animation, normalising data.

```calc
print sq(7);                       // 49
print cube(4);                     // 64
print power_int(3, 5);             // 243   — no libc pow
print cbrt(-8);                    // -2
print hypot(1e150, 1e150);         // 1.414213562e+150
print clamp(15, 0, 10);            // 10
print remap(5, 0, 10, 100, 200);   // 150
```

Use it from any program:

```calc
import "math";
```

### `lib/linalg.calc` — matrices

Matrices are arrays-of-arrays: `m[r][c]` is the (r, c) element. Every operation allocates a fresh result; nothing mutates in place except the explicit `mat_set`.

```calc
extern fn mat_eye(n: num): arr;
extern fn mat_mul(a: arr, b: arr): arr;
extern fn mat_solve(a: arr, b: arr): arr;
extern fn mat_det(a: arr): num;

// Solve Ax = b for a 3×3 system.
let A = [[2, 1, -1], [-3, -1, 2], [-2, 1, 2]];
let b = [8, -11, -3];
print mat_solve(A, b);     // [2, 3, -1]
print mat_det(A);          // -1
```

Available: `mat_zeros`, `mat_ones`, `mat_eye`, `mat_from_rows`; `mat_shape`, `mat_rows`, `mat_cols`, `mat_get`, `mat_set`; `mat_add`, `mat_sub`, `mat_scale`, `mat_mul`, `mat_vec_mul`, `mat_transpose`; `mat_solve` (Gaussian elimination with partial pivoting), `mat_det` (via the same LU sweep, with sign tracking).

### `lib/stats.calc` — statistics

```calc
extern fn mean(xs: arr): num;
extern fn stddev(xs: arr): num;
extern fn linreg(xs: arr, ys: arr): map;
extern fn histogram(xs: arr, n_bins: num): map;

let xs = [2.1, 3.4, 5.0, 5.7, 7.8];
print mean(xs);            // 4.8
print stddev(xs);          // 2.156385878
```

Available: `sum_of`, `mean`, `variance` (sample, n-1 denominator), `stddev`, `median`, `min_of`, `max_of`, `range_of`, `correlate` (Pearson r), `linreg` (returns `{"slope", "intercept", "r2"}`), `histogram` (returns `{"edges", "counts"}`).

### `lib/plot.calc` — SVG plots

Generates standalone SVG files you can open in any browser. Streams via `file_append` so plots with thousands of points stay memory-bounded.

```calc
extern fn plot_line(filename: str, xs: arr, ys: arr, title: str);

let xs = [];  let ys = [];
for (let i = 0; i < 400; i = i + 1) {
    let x = (i / 399.0) * 4 * pi();
    push(xs, x);
    push(ys, sin(x));
}
plot_line("sine.svg", xs, ys, "sin(x) over [0, 4π]");
```

Available: `plot_line` (single series, polyline), `plot_scatter` (one circle per point), `plot_data_with_fit` (scatter + line overlay — useful for regression visualisations). All plots get axes with five tick labels per axis.

### `lib/numeric.calc` — root finding, integration, interpolation

```calc
extern fn bisect(f: fn, a: num, b: num, tol: num): num;
extern fn newton(f: fn, df: fn, x0: num, tol: num, max_iter: num): num;
extern fn adaptive_simpson(f: fn, a: num, b: num, tol: num): num;
extern fn poly_eval(coeffs: arr, x: num): num;
extern fn linterp(xs: arr, ys: arr, x: num): num;

let f  = fn(x) { return x * x * x - 2; };
let df = fn(x) { return 3 * x * x; };
print newton(f, df, 1.5, 0.0000001, 50);   // 1.25992105 (cube root of 2)

let sin_fn = fn(x) { return sin(x); };
print adaptive_simpson(sin_fn, 0, pi(), 0.000000001);   // 2.0
```

Available: `bisect`, `newton`, `secant` (root finding); `trapezoidal`, `simpson`, `adaptive_simpson` (numerical integration); `poly_eval`, `poly_derivative` (Horner-method poly ops, low-to-high coefficient order); `linterp` (linear interpolation, binary-search bracket, clamps outside data range).

The root-finders and integrators take **functions as arguments** — CalcLang's first-class fns let you pass `fn(x) { return cos(x); }` straight in. No callback boilerplate.

### `lib/random.calc` — reproducible PRNG

A class-based PRNG: the state lives in `this.state`, and methods generate distributions on top.

```calc
extern fn Rng(seed: num): map;     // constructor (a class IS just a fn)

let rng = Rng(2024);
print rng.uniform01();              // 0.236...
print rng.uniform(10, 20);          // 13.4...
print rng.normal(0, 1);             // standard-normal sample (Box-Muller)
print rng.poisson(3.5);             // small-lambda Poisson via Knuth
print rng.exponential(2);           // exponential with mean 2
print rng.choice(["a","b","c"]);    // random element
print rng.shuffle([1,2,3,4,5]);     // Fisher-Yates in place
print rng.normal_array(1000, 0, 1); // 1000 standard normals
```

The underlying generator is the linear-congruential Numerical-Recipes scheme. Reseed with `let rng = Rng(new_seed);` for reproducibility — same seed → same sequence.

Why a class? Top-level `fn`s in the native backend don't see top-level `let`s, so a module-level stateful PRNG would be awkward. Wrapping it in a class lets `this.state` ride along with each method call via the existing closure machinery — clean and natural.

### `lib/csv.calc` — CSV I/O

```calc
extern fn csv_write(path: str, rows: arr);
extern fn csv_write_with_headers(path: str, headers: arr, rows: arr);
extern fn csv_read(path: str): arr;
extern fn csv_read_with_headers(path: str): arr;   // returns array of maps

let data = [
    {"x": 0, "y": 1.5},
    {"x": 1, "y": 4.7},
    {"x": 2, "y": 7.9}
];
csv_write_with_headers("data.csv", ["x", "y"], data);

let loaded = csv_read_with_headers("data.csv");
print loaded[0]["x"];               // "0"   (cells come back as strings)
print to_num(loaded[0]["x"]);       // 0     (convert when you need numbers)
```

Deliberately simple: split on `\n` and `,`. Doesn't handle quoted fields with embedded commas. Cells come back as strings — convert with `to_num` where needed. Strips Windows-style `\r` on read so files round-trip cross-platform.

### Extended `lib/linalg.calc`: `mat_inv`, vector / matrix norms

```calc
extern fn mat_inv(a: arr): arr;
extern fn vec_norm1(v: arr): num;
extern fn vec_norm2(v: arr): num;
extern fn vec_norm_inf(v: arr): num;
extern fn mat_norm_frobenius(a: arr): num;

print vec_norm2([3, 4]);                            // 5
print mat_norm_frobenius([[1, 2], [3, 4]]);         // sqrt(30) ≈ 5.477
```

`mat_inv` runs Gauss-Jordan on the augmented `[A | I]` matrix with partial pivoting; throws on singular input.

### Plot library — extended views

Beyond the basic `plot_line` / `plot_scatter` / `plot_data_with_fit`, the plot library also has:

```calc
extern fn plot_lines(filename: str, xss: arr, yss: arr, labels: arr, title: str);
extern fn plot_bar(filename: str, labels: arr, values: arr, title: str);
extern fn plot_logy(filename: str, xs: arr, ys: arr, title: str);
extern fn plot_line_labeled(filename: str, xs: arr, ys: arr, title: str,
                            x_label: str, y_label: str);
```

- **`plot_lines`** draws multiple series on shared axes, with a small legend in the top-right and a six-color palette cycling through them.
- **`plot_bar`** is a vertical bar chart with categorical x-axis labels.
- **`plot_logy`** plots with a log-scale y-axis (all y values must be > 0). Useful for exponential decay or wide dynamic-range data.
- **`plot_line_labeled`** is `plot_line` with explicit x/y axis labels (the y-label is rotated 90° at the left edge).

### `lib/json.calc` — JSON parser + encoder

```calc
extern fn json_parse(src: str);
extern fn json_encode(v): str;

let person = {"name": "Alice", "age": 30, "tags": ["admin", "engineer"]};
let s = json_encode(person);
print s;     // {"name":"Alice","age":30,"tags":["admin","engineer"]}

let p = json_parse(s);
print p["name"];                          // Alice
print p["tags"][1];                       // engineer

// Parsing handles escapes.
let m = json_parse("{\"x\": 1, \"y\": [2, 3], \"flag\": true}");
print m["flag"];                          // 1   (true -> 1)
```

`true` and `false` become `1` and `0`; `null` becomes `0` (CalcLang has no distinct boolean or null Value). Strings are decoded with `\n` / `\t` / `\r` / `\"` / `\\` / `\/`. Round-trips `num`/`str`/`arr`/`map` cleanly.

### `lib/ode.calc` — ODE solvers

Fourth-order Runge-Kutta for both scalar and vector ODEs.

```calc
extern fn rk4(f: fn, t0: num, y0: num, t_end: num, n: num): map;
extern fn rk4_vec(f: fn, t0: num, y0: arr, t_end: num, n: num): map;
extern fn euler(f: fn, t0: num, y0: num, t_end: num, n: num): map;

// Scalar: y' = -y, y(0) = 1.   Exact answer at t=5 is e^(-5) ≈ 0.00674.
let decay = fn(t, y) { return -y; };
let sol = rk4(decay, 0, 1, 5, 100);
print sol["ys"][len(sol["ys"]) - 1];      // 0.006737948828  (RK4 nails it to 7 digits)

// Vector: harmonic oscillator   y1' = y2,   y2' = -y1.
let sho = fn(t, y) { return [y[1], -y[0]]; };
let path = rk4_vec(sho, 0, [1, 0], 10, 400);
let last = path["ys"][len(path["ys"]) - 1];
print last;            // [-0.83907..., 0.54402...]  -- equals (cos 10, -sin 10)
```

Each solver returns `{"ts": [...], "ys": [...]}`. For scalar ODEs `ys[i]` is a number; for vector ODEs it's an array of the same length as the initial state.

`euler` is provided for didactic comparison — same call shape, but linear convergence. At the same step count it lags RK4 substantially.

### `lib/fft.calc` — Cooley-Tukey FFT using complex numbers

```calc
extern fn fft(x: arr): arr;          // forward DFT
extern fn ifft(X: arr): arr;         // inverse DFT
extern fn fft_magnitude(X: arr): arr;
extern fn fft_freqs(n: num, fs: num): arr;

// Sample a sum of sinusoids at 100 Hz for 256 samples.
let N  = 256;  let fs = 100;
let x = [];
for (let i = 0; i < N; i = i + 1) {
    let t = i / fs;
    push(x, sin(2 * pi() * 7 * t) + 0.5 * sin(2 * pi() * 18 * t));
}
let X   = fft(x);
let mag = fft_magnitude(X);
// mag has peaks at bins corresponding to 7 Hz and 18 Hz.
```

Input length must be a power of 2. The forward `fft` accepts either real (`num`) or complex (`cpx`) values — real inputs are auto-promoted by the complex-number arithmetic. The output is always an array of `cpx`. `ifft(fft(x))` round-trips to within machine epsilon (~1e-16).

CalcLang's first-class complex type means there's no manual `{re, im}` packing — the inner-loop arithmetic in `lib/fft.calc` is the literal mathematical form: `let t = w * O[k]; result[k] = E[k] + t;`.

### `lib/nr/` — Numerical-Recipes-style algorithms

A separate directory of more substantial numerical algorithms, in the spirit of Press et al.'s *Numerical Recipes in C*. Each one is a well-known piece of canonical numerical computing, implemented from the NR explanations / pseudocode.

#### `lib/nr/brent.calc` — Brent's root finder

A bracketed root finder that combines bisection's reliability with secant / inverse-quadratic interpolation's speed. Always converges (the bracket only shrinks) and usually does so superlinearly. The standard "robust" choice for one-dimensional root finding.

```calc
extern fn brent_root(f: fn, a: num, b: num, tol: num, max_iter: num): num;

let f = fn(x) { return cos(x); };
print brent_root(f, 0, pi(), 0.0000000001, 100);   // 1.5707963268...
```

Caller must bracket the root (`f(a) * f(b) < 0`). Converges in ~10 iterations for typical engineering tolerances.

#### `lib/nr/spline.calc` — natural cubic spline

Smooth interpolation through a sequence of (xs, ys) knots. Two-step interface (the NR style): pre-compute second derivatives once with `spline_setup`, reuse for every interpolation query.

```calc
extern fn spline_setup(xs: arr, ys: arr): arr;
extern fn spline_eval(xs: arr, ys: arr, y2: arr, x: num): num;

// Tabulate sin at 7 points and interpolate.
let xs = [];  let ys = [];
for (let i = 0; i < 7; i = i + 1) {
    let x = i / 6.0 * 2 * pi();
    push(xs, x);  push(ys, sin(x));
}
let y2 = spline_setup(xs, ys);
print spline_eval(xs, ys, y2, 1.0);   // 0.84... (close to sin(1) = 0.8415)
```

The spline reproduces ys exactly at the knots. Off-knot error scales with the fourth power of the knot spacing — 7 evenly-spaced knots over [0, 2π] gives max error ~0.004 against the true sine.

#### `lib/nr/special.calc` — gamma, beta, erf

Special functions that come up everywhere in stats and physics.

```calc
extern fn gamma(x: num): num;
extern fn lgamma(x: num): num;
extern fn beta(a: num, b: num): num;
extern fn erf(x: num): num;
extern fn erfc(x: num): num;

print gamma(5);            // 24                (= 4!)
print gamma(0.5);          // 1.7724538509...   (= sqrt(π))
print lgamma(100);         // log(99!)
print beta(2, 3);          // 0.08333... (= 1/12)
print erf(1);              // 0.8427...
```

`lgamma` uses the Lanczos approximation (6-term, g = 5). `gamma` is `exp(lgamma)` plus reflection-formula handling for non-positive non-integer x. `erf` uses Abramowitz & Stegun's rational Chebyshev approximation (7.1.26), accurate to about 7 decimal places — plenty for engineering work. `erfc` is `1 - erf`.

#### `lib/nr/eigen.calc` — Jacobi eigenvalue decomposition

For real symmetric matrices: returns all eigenvalues and the full eigenvector matrix. O(n³) per sweep, O(log(1/eps)) sweeps — solid for small to medium problems (a few hundred rows). Not the right tool for huge dense matrices (use a real LAPACK binding for those), but plenty for engineering work on smallish matrices.

```calc
extern fn jacobi_eigen(a: arr): map;

let A = [[4, 1, 2],
         [1, 3, 0],
         [2, 0, 5]];
let eig = jacobi_eigen(A);
print eig["values"];     // [3.476..., 1.855..., 6.669...]   (sum = trace = 12)
let V = eig["vectors"];   // V[r][i] is the r-th component of the i-th eigenvector.
```

Each iteration finds the largest off-diagonal element and rotates it to zero with a Givens-like 2×2 plane rotation. The product of all rotations is the full eigenvector matrix; the diagonal of the rotated matrix converges to the eigenvalues. Residuals `|A v - λ v|` come out at machine epsilon (~1e-13) for well-conditioned problems.

#### `lib/nr/lu.calc` — LU decomposition

Doolittle LU with partial pivoting (NR §2.3). The big payoff over Gauss-Jordan is reuse: one factorization, then any number of right-hand sides solved cheaply, and the determinant comes out as a side effect.

```calc
extern fn lu_decompose(A: arr): map;
extern fn lu_solve(lu: map, b: arr): arr;
extern fn lu_det(lu: map): num;

let A = [[2, 1, -1], [-3, -1, 2], [-2, 1, 2]];
let lu = lu_decompose(A);
print lu_det(lu);              // -1
print lu_solve(lu, [8, -11, -3]);     // [2, 3, -1]
print lu_solve(lu, [0,  1,   4]);     // (reuses the factorization)
```

`lu_decompose` returns `{"L", "U", "P", "sign"}` — L is unit lower-triangular, U upper-triangular, P the row permutation as an index array, and sign is ±1 (parity of row swaps).

#### `lib/nr/romberg.calc` — Romberg integration

Recursively-refined trapezoidal estimates plus Richardson extrapolation along each row of the table — cancels successive orders of the Taylor remainder. For smooth integrands this typically reaches machine precision in 5-10 levels (NR §4.3).

```calc
extern fn romberg(f: fn, a: num, b: num, tol: num, max_levels: num): num;

let sin_fn = fn(x) { return sin(x); };
print romberg(sin_fn, 0, pi(), 0.0000000001, 12);    // 2 (to machine epsilon)
```

#### `lib/nr/rk45.calc` — adaptive Runge-Kutta-Cash-Karp

Six function evaluations per step give both a 5th-order and a 4th-order estimate; their difference is the local error estimate. We use it to grow the step when the integrand is tame and shrink it when it's stiff. NR §16.2.

```calc
extern fn rk45(f: fn, t0: num, y0: num, t_end: num, tol: num): map;

let decay = fn(t, y) { return -y; };
let sol = rk45(decay, 0, 1, 5, 0.0000000001);
let n = len(sol["ys"]);
print sol["ys"][n - 1];        // 0.006737946972  ~= e^-5
print sol["steps_accepted"];   // 65
print sol["steps_rejected"];   // 0
```

Returns `{"ts", "ys", "steps_accepted", "steps_rejected"}`. The timestamps are non-uniform — that's the whole point. For a stiff problem like `y' = -100(y - 1)`, RK45 hammers through the boundary layer with many tiny steps and then takes large strides afterward; constant-step RK4 would have to use the smallest step throughout.

#### `lib/nr/poly.calc` — orthogonal-polynomial families

Three-term recurrences for Chebyshev T_n, Legendre P_n, Hermite H_n (physicists'), Laguerre L_n; rational approximations for Bessel J_0 and J_1 (NR §5.5 / §6.5).

```calc
extern fn chebyshev_T(n: num, x: num): num;
extern fn legendre_P(n: num, x: num): num;
extern fn hermite_H(n: num, x: num): num;
extern fn laguerre_L(n: num, x: num): num;
extern fn bessel_J0(x: num): num;
extern fn bessel_J1(x: num): num;

print chebyshev_T(5, 0.5);     // 0.5  (T_n(cos θ) = cos nθ; cos(5π/3) = 0.5)
print legendre_P(5, 0.5);      // 0.0898...
print hermite_H(3, 0.5);       // -5   (8x³ - 12x at 0.5)
print bessel_J0(2.4048);       // 1.3e-05  (≈ 0; that's a Bessel zero)
```

All recurrences are evaluated bottom-up — numerically stable and O(n) per evaluation.

#### `lib/nr/minimize.calc` — 1-D and N-D minimization

Three routines, in order of increasing power:

- **`golden_section(f, a, b, c, tol)`** — bullet-proof bracketed 1-D minimizer. Needs `f(b) < f(a)` and `f(b) < f(c)`. Linear convergence, no derivatives.
- **`brent_min(f, a, b, c, tol)`** — Brent's parabolic-interpolation method. Falls back to golden-section when the parabolic step misbehaves. The standard workhorse (NR §10.3). Returns `{"x", "f"}`.
- **`simplex_min(f, x0, step, tol)`** — Nelder–Mead downhill simplex for multi-dim minimization (NR §10.5). `f` takes a length-n array. Returns `{"x", "f", "iters"}`. Doesn't need a derivative — useful when the gradient is hard or noisy.

```calc
extern fn brent_min(f: fn, a: num, b: num, c: num, tol: num): map;
extern fn simplex_min(f: fn, x0: arr, step: num, tol: num): map;

let f = fn(x) { return (x - 2) * (x - 2) + 1; };
let r = brent_min(f, 0, 1.5, 5, 1e-10);
print r["x"];                                  // 2.0
print r["f"];                                  // 1.0

// Rosenbrock — the classic NR test of a 2-D minimizer.
let rosen = fn(v) {
    let dx = 1 - v[0];
    let dy = v[1] - v[0] * v[0];
    return dx * dx + 100 * dy * dy;
};
print simplex_min(rosen, [-1.2, 1], 0.5, 1e-10)["x"];   // [~1, ~1]
```

#### `lib/nr/sort.calc` — heapsort and quickselect

```calc
extern fn heapsort(arr: arr): arr;             // in-place, O(n log n) worst-case
extern fn heapsort_idx(arr: arr): map;         // returns {"values", "index"}
extern fn quickselect(arr: arr, k: num): num;  // O(n) average, k-th smallest
extern fn median(arr: arr): num;
```

`heapsort_idx` returns the sort order's original-index permutation, useful when a parallel column needs to follow. `quickselect` mutates its input and runs in expected O(n) — pick `k = n/2` for a median, `k = n-1` for the maximum, etc. `median` does the right thing for both even and odd lengths.

NR §8.3 (heapsort) and §8.5 (selection).

#### `lib/nr/diff.calc` — Ridders' numerical differentiation

Centered differences have a competing pair of errors: truncation O(h²) and roundoff O(eps/h). Ridders' method starts with a generous h, repeatedly halves it, and Neville-extrapolates up the resulting table — driving the truncation error well below the roundoff floor. NR §5.7.

```calc
extern fn dfridr(f: fn, x: num, h: num): map;
extern fn deriv(f: fn, x: num, h: num): num;     // just the derivative
extern fn gradient(f: fn, x: arr, h: num): arr;  // scalar f, vector x
extern fn jacobian(f: fn, x: arr, h: num): arr;  // vector f, vector x

let r = dfridr(fn(x) { return sin(x); }, 1.0, 0.1);
print r["deriv"];                                 // 0.5403023059  = cos(1)
print r["err"];                                   // ~1e-15
```

`gradient` and `jacobian` build on `dfridr` to handle the multivariate cases — useful with `newton_num` below or any other gradient-driven solver.

#### `lib/nr/random_dist.calc` — extra distributions

Builds on `lib/random.calc`'s `Rng` (already provides uniform / Box-Muller normal / exponential / Poisson). Each function here takes an `Rng` instance and produces one sample from the named distribution. NR §7.3.

```calc
extern fn gamma_sample(rng: map, shape: num, scale: num): num;
extern fn chi2_sample(rng: map, k: num): num;
extern fn beta_sample(rng: map, a: num, b: num): num;
extern fn student_t_sample(rng: map, nu: num): num;
extern fn cauchy_sample(rng: map, x0: num, gamma: num): num;
extern fn binomial_sample(rng: map, n: num, p: num): num;
extern fn geometric_sample(rng: map, p: num): num;
extern fn triangular_sample(rng: map, a: num, b: num, c: num): num;
```

`gamma_sample` uses the Marsaglia–Tsang 2000 squeeze method (with the U^(1/shape) boost for shape < 1). chi² / beta / Student's t are derived from it.

#### `lib/nr/newton.calc` — Newton-Raphson for nonlinear systems

Solves F(x) = 0 for F: Rⁿ → Rⁿ. Each step solves J·dx = -F via the LU factorization from `lib/nr/lu.calc`. An Armijo back-tracking line search widens the basin of convergence (the full Newton step is halved until ||F|| actually decreases). NR §9.6–9.7.

```calc
extern fn newton_n(F: fn, J: fn, x0: arr, tol: num, max_iter: num): map;
extern fn newton_num(F: fn, x0: arr, tol: num, max_iter: num): map;

// Intersection of a circle and a line:
//   x^2 + y^2 = 25,   x - y = 1
let F = fn(v) {
    return [v[0]*v[0] + v[1]*v[1] - 25, v[0] - v[1] - 1];
};
let J = fn(v) { return [[2*v[0], 2*v[1]], [1, -1]]; };
let r = newton_n(F, J, [5, 0], 1e-12, 50);
print r["x"];     // [4, 3]
```

`newton_num` computes the Jacobian numerically (Ridders') — convenient when an analytic Jacobian is hard, at the cost of n + 1 extra function calls per step.

#### `lib/nr/fitnl.calc` — Levenberg-Marquardt nonlinear least squares

Fits y_i ≈ model(x_i; a) by minimizing Σ (y_i - model)². At each step builds the curvature matrix αⱼₖ = Σ (∂model/∂aⱼ)(∂model/∂aₖ) with a Marquardt diagonal boost (1+λ), solves α·δa = β = Σ rᵢ ∂model/∂aⱼ for a step, and adjusts λ depending on whether χ² decreased. Returns the fitted parameters, final χ², and the covariance matrix C = α⁻¹ (with λ=0) for parameter uncertainties. NR §15.5.

```calc
extern fn lm_fit(model: fn, xs: arr, ys: arr, a0: arr, tol: num, max_iter: num): map;

// model returns BOTH the prediction and the gradient wrt parameters.
let model = fn(x, a) {
    let e = exp(-a[1] * x);
    return {"y": a[0]*e + a[2], "dyda": [e, -a[0]*x*e, 1]};
};
let fit = lm_fit(model, xs, ys, [1, 1, 0], 1e-10, 200);
print fit["a"];          // recovered parameters
print fit["chisq"];      // residual sum of squares
print fit["covar"];      // parameter covariance matrix
```

The `dyda` array is the gradient with respect to a — supplying it directly (rather than finite-differencing) is faster and more accurate.

#### `lib/nr/qr.calc` — QR decomposition (Householder)

For A (m × n with m ≥ n), `qr_decompose` returns `{"Q", "R"}` where Q is m × m orthogonal and R is m × n upper triangular. `qr_solve` then handles both square systems and overdetermined least-squares problems via back-substitution on R. Householder reflections are the numerically-stable canonical choice (NR §2.10).

```calc
extern fn qr_decompose(A: arr): map;
extern fn qr_solve(qr: map, b: arr): arr;

let A = [[1, 1, 0], [1, 0, 1], [0, 1, 1], [1, 1, 1]];
let qr = qr_decompose(A);
let x = qr_solve(qr, [3, 2, 4, 5]);   // 4 x 3 LS, x in R^3
```

#### `lib/nr/svd.calc` — singular value decomposition

For A (m × n with m ≥ n), `svd(A)` returns `{"U", "S", "V"}` with U (m × n) having orthonormal columns, S the n singular values sorted descending, V (n × n) orthogonal. Implementation forms A^T A and Jacobi-eigendecomposes it — about half the precision of Golub-Reinsch but ~80 lines of CalcLang. `svd_solve` does rank-revealing LS / minimum-norm solve; `svd_pinv` returns the Moore-Penrose pseudo-inverse. NR §2.6.

```calc
extern fn svd(A: arr): map;
extern fn svd_solve(A: arr, b: arr): arr;
extern fn svd_pinv(A: arr): arr;

let r = svd(B);
print r["S"];     // singular values, biggest first
```

#### `lib/nr/polyroots.calc` — Laguerre polynomial roots

`poly_roots(coefs)` returns all `n = degree` complex roots of P(x) = Σ coefs[i] x^i. Laguerre's method has cubic convergence from almost any starting point and handles repeated / complex roots gracefully. After each root is found we deflate by synthetic division; a final polish pass re-runs Laguerre on the original polynomial to wipe out deflation roundoff. NR §9.5.

```calc
extern fn poly_roots(coefs: arr): arr;

// (x - 1)(x - 2)(x - 3) = x^3 - 6 x^2 + 11 x - 6.
// Coefficient convention: coefs[i] multiplies x^i.
let roots = poly_roots([-6, 11, -6, 1]);
for (let i = 0; i < len(roots); i = i + 1) { print roots[i]; }
// 1, 2, 3 — exact.
```

Each root is returned as a CalcLang complex value; real roots come back with imaginary part essentially zero.

#### `lib/nr/conv.calc` — convolution / correlation

```calc
extern fn conv_direct(a: arr, b: arr): arr;
extern fn conv_fft(a: arr, b: arr): arr;
extern fn corr_direct(a: arr, b: arr): arr;
extern fn correlate_fft(a: arr, b: arr): arr;
```

Direct is O(nm); use it for short signals or short kernels. FFT-backed variants run in O((n+m) log(n+m)) — zero-pad to the next power of two, transform both, multiply (or multiply by the conjugate, for correlation), and inverse-transform. Built on `lib/fft.calc`. NR §13.1–13.2.

#### `lib/nr/cheb.calc` — Chebyshev approximation

`cheb_fit(f, a, b, n)` returns a map with the `n` Chebyshev coefficients for f on [a, b]. `cheb_eval` does the Clenshaw recurrence in O(n) without ever building the individual T_k. `cheb_deriv` / `cheb_integral` produce the coefficient table of f' / ∫f exactly — useful for adaptive quadrature pipelines where you want both. NR §5.8–5.9.

```calc
extern fn cheb_fit(f: fn, a: num, b: num, n: num): map;
extern fn cheb_eval(cm: map, x: num): num;
extern fn cheb_deriv(cm: map): map;

let cm = cheb_fit(fn(x) { return exp(-x) * cos(x); }, 0, 5, 16);
print cheb_eval(cm, 1.0);                 // 0.198766...  matches exp(-1) cos(1)
print cheb_eval(cheb_deriv(cm), 1.0);     // -0.508326... = f'(1)
```

For smooth f, 8–16 coefficients usually reach machine precision.

#### `lib/nr/savgol.calc` — Savitzky-Golay smoothing / differentiation

```calc
extern fn savgol_coeffs(nl: num, nr: num, m: num, ld: num): arr;
extern fn savgol_apply(xs: arr, nl: num, nr: num, m: num, ld: num): arr;
```

A Savitzky-Golay filter fits a low-order polynomial to a sliding window and reads off its value (or derivative) at the center. It preserves spectral peaks far better than a moving average for the same noise reduction, and produces clean derivative estimates for free. NR §14.8.

`nl`/`nr` are points to the left / right of the window center. `m` is the polynomial order. `ld` is the derivative order (0 = smoothing). Window length is `nl + nr + 1`.

#### `lib/nr/kalman.calc` — discrete-time Kalman filter

```calc
extern fn kf_new(x0: arr, P0: arr, F: arr, B: arr, H: arr, Q: arr, R: arr): map;
extern fn kf_step(kf: map, u: arr, z: arr): map;
```

Generic linear-Gaussian state-space estimator. Each step does predict (`x' = F x + B u`, `P' = F P F^T + Q`) and update with measurement z (`K = P H^T (H P H^T + R)^{-1}`, etc.). For a stationary scalar value with sensor variance R and small process variance Q, the steady-state Kalman gain settles around √(Q/R) and the filter dramatically out-performs the raw measurement.

#### `lib/nr/pde.calc` — Crank-Nicolson 1-D diffusion

```calc
extern fn heat_1d(u0: arr, alpha: num, dx: num, dt: num,
                  n_steps: num, bcL: num, bcR: num): map;
```

Solves `du/dt = α d²u/dx²` on `[0, L]` with Dirichlet boundary conditions. Crank-Nicolson averages an explicit and an implicit step: unconditionally stable AND second-order in both space and time, much better than either Euler scheme alone. Each step is a tridiagonal Thomas solve in the interior. NR §19.2.

Returns `{"u_final", "history"}`, where `history` is the full per-step state — drop it in your caller if you only need the final field.

#### `lib/nr/cholesky.calc` — Cholesky factorization

For symmetric positive-definite A, `chol_decompose` returns L (lower-triangular) such that A = L L^T. Half the work and storage of LU and never needs pivoting (NR §2.9).

```calc
extern fn chol_decompose(A: arr): arr;
extern fn chol_solve(L: arr, b: arr): arr;
extern fn chol_logdet(L: arr): num;
extern fn chol_inverse(L: arr): arr;

let L = chol_decompose([[4, 12, -16], [12, 37, -43], [-16, -43, 98]]);
// L = [[2, 0, 0], [6, 1, 0], [-8, 5, 3]]
print chol_logdet(L);       // log(det(A)) — stable for huge determinants
```

`chol_logdet` avoids forming the determinant explicitly — useful when det(A) underflows or overflows.

#### `lib/nr/cg.calc` — conjugate gradient

```calc
extern fn cg_solve(A: arr, b: arr, x0: arr, tol: num, max_iter: num): map;
extern fn pcg_solve(A: arr, b: arr, x0: arr, apply_Minv: fn, tol: num, max_iter: num): map;
```

The standard Krylov-subspace method for SPD systems. Converges in O(√cond(A)) iterations to a given tolerance — much faster than direct factorization for large, sparse, or structured problems. `pcg_solve` accepts a preconditioner as a closure that maps a residual to M⁻¹r, supporting fully matrix-free workflows.

#### `lib/nr/anneal.calc` — simulated annealing

```calc
extern fn anneal_solve(f: fn, x0, propose: fn, t_start: num, t_end: num,
                       cooling: num, steps_per_T: num, rng: map): map;
extern fn anneal_continuous(f: fn, x0: arr, step: num, t_start: num, t_end: num,
                            cooling: num, steps_per_T: num, rng: map): map;
```

Generic minimizer that walks a Metropolis chain whose temperature decays on a geometric schedule. Uphill moves are accepted with probability exp(−ΔF/T) — escapes local minima while T is high, refines as T cools. `anneal_solve` takes any state and a problem-specific `propose(x, T, rng)`. `anneal_continuous` is the convenience wrapper for x ∈ Rⁿ with Gaussian random-walk proposals. NR §10.9.

#### `lib/nr/mcmc.calc` — Metropolis-Hastings sampler

```calc
extern fn metropolis(log_pi: fn, x0, propose: fn, log_q_ratio,
                     n_samples: num, burn_in: num, thin: num, rng: map): map;
extern fn metropolis_rw(log_pi: fn, x0: arr, step: num,
                        n_samples: num, burn_in: num, thin: num, rng: map): map;
```

Markov-chain Monte Carlo for sampling from an unnormalized target. Operates in log-space for numerical stability across many decades of probability. `metropolis_rw` is the symmetric random-walk variant for Rⁿ. Returns the post-burn-in, thinned samples plus the acceptance rate (target ~25–40% for Gaussian walks).

#### `lib/nr/welch.calc` — Welch's periodogram

```calc
extern fn welch(xs: arr, fs: num, n_per_seg: num, overlap_frac: num): map;
extern fn hann_window(N: num): arr;
```

Splits the signal into overlapping Hann-windowed segments, FFTs each, and averages |X(f)|² across segments. The averaging reduces estimator variance vs a single periodogram; the taper reduces leakage. Output is `{"freqs", "psd"}` of length `n_per_seg/2 + 1`. NR §13.4.

#### `lib/nr/wavelet.calc` — discrete wavelet transforms

```calc
extern fn haar_forward(xs: arr): arr;     extern fn haar_inverse(ys: arr): arr;
extern fn d4_forward(xs: arr): arr;       extern fn d4_inverse(ys: arr): arr;
```

Two transform families: the simplest possible (Haar) and Daubechies's compactly-supported orthogonal D4. Length must be a power of two; output layout is `[final-approx, coarse-detail, ..., finest-detail]`. Both transforms round-trip to machine epsilon. NR §13.10.

#### `lib/nr/toeplitz.calc` — Levinson-Durbin

```calc
extern fn levinson_solve(r: arr, y: arr): arr;
extern fn yule_walker(autocorr: arr, p: num): map;
```

Levinson exploits the constant-diagonal structure of a symmetric Toeplitz matrix `T[i][j] = r[|i-j|]` to solve `T x = y` in O(n²) instead of O(n³). `yule_walker` builds on it: given an autocorrelation sequence, returns the AR(p) coefficients and the residual variance σ². NR §2.8, §13.6.

#### `lib/nr/simplex_lp.calc` — linear programming

```calc
extern fn simplex_lp(c: arr, A: arr, b: arr): map;
// Solves:  max c^T x  subject to  A x <= b,  x >= 0.
```

Two-phase simplex method with big-M handling for negative RHS entries. Returns `{"status", "x", "value"}` where status ∈ {0=optimal, 1=unbounded, 2=infeasible}. NR §10.8. Cast minimization problems by negating `c`; cast `>=` constraints by negating the row.

#### `lib/nr/fft2d.calc` — two-dimensional FFT

```calc
extern fn fft2(X: arr): arr;
extern fn ifft2(Y: arr): arr;
```

Both dimensions must be powers of two. The implementation is straightforward separable: FFT each row, then FFT each column. NR §12.4.

#### `lib/nr/quad2d.calc` — two-dimensional quadrature

```calc
extern fn quad2d_gl(f: fn, ax, bx, ay, by, n: num): num;
extern fn quad2d_adaptive(f: fn, ax, bx, ay, by, tol: num): num;
```

`quad2d_gl` evaluates an n-point Gauss-Legendre tensor product (n ∈ {2, 3, 4, 5}). The fixed rule is exact for polynomials up to degree 2n-1 in each variable — beautiful for smooth integrands, poor for sharply peaked ones. `quad2d_adaptive` recursively bisects until each cell's estimate matches the sum of its four sub-cell estimates within tol — handles peaks gracefully. NR §4.5.

#### `lib/nr/power_eigen.calc` — power and inverse iteration

```calc
extern fn power_iterate(A: arr, x0: arr, tol: num, max_iter: num): map;
extern fn inverse_iterate(A: arr, sigma: num, x0: arr, tol: num, max_iter: num): map;
```

`power_iterate` finds the dominant eigenvalue (largest in absolute value) by repeatedly multiplying by A and renormalizing. The Rayleigh quotient at each step gives a quadratically-convergent estimate of the eigenvalue. `inverse_iterate` solves `(A - σI) y = x` each step (via LU once, reused thereafter) — converges to the eigenvalue nearest σ. Together they cover the "I want one specific eigenvalue" use case that the full Jacobi solver overshoots for. NR §11.7.

#### `lib/nr/bspline.calc` — B-spline evaluation and fitting

```calc
extern fn bspline_basis(knots: arr, k: num, x: num): arr;
extern fn bspline_eval(knots: arr, k: num, c: arr, x: num): num;
extern fn bspline_fit(xs: arr, ys: arr, knots: arr, k: num): arr;
extern fn bspline_clamped_knots(a, b: num, n_interior, k: num): arr;
```

`k` is the order (k = 4 gives cubics). The Cox-de Boor recurrence evaluates the B-spline basis in O(k) per query. `bspline_fit` builds the design matrix `B[i][j] = B_j(x_i)`, then QR-solves `B c = y` for the least-squares coefficients — handy when you have noisy data and want smooth interpolation. `bspline_clamped_knots` gives the standard clamped uniform knot vector (repeated endpoints) for convenience.

#### `lib/nr/neville.calc` — Neville polynomial interpolation

```calc
extern fn neville_interp(xs: arr, ys: arr, x: num): map;
// returns {"y": interpolated value, "err": rough error estimate}
```

Builds the degree-(n-1) polynomial through n samples and evaluates it at x. The Neville tableau gives both the value and an internal error indicator at no extra cost (NR §3.1). Use this when you have a few high-quality samples and want a built-in error bar; for dense data prefer a cubic spline to dodge the Runge phenomenon at high order.

#### `lib/nr/glnodes.calc` — arbitrary-order Gauss-Legendre nodes/weights

```calc
extern fn gauleg(n: num, a: num, b: num): map;            // -> {"x", "w"}
extern fn integrate_gauleg(f: fn, a: num, b: num, n: num): num;
```

Computes the n-point Gauss-Legendre nodes by Newton iteration on `P_n(x) = 0` starting from the standard Chebyshev-like initial guess; uses Bonnet's three-term recurrence to evaluate `P_n` and `P_n'` simultaneously. Affine-transforms to `[a, b]`. NR §4.5.

For smooth integrands this gives spectral accuracy: `int_0^pi sin(x) dx` reaches machine precision by n = 10.

#### `lib/nr/bfgs.calc` — BFGS quasi-Newton minimization

```calc
extern fn bfgs_min(f: fn, grad: fn, x0: arr, tol: num, max_iter: num): map;
```

BFGS maintains a rank-2 inverse-Hessian approximation built from successive (Δx, Δgradient) pairs. After a few steps the approximation gets good enough that convergence becomes superlinear — typically beats Nelder-Mead by an order of magnitude once `n` exceeds ~5. The implementation uses an Armijo back-tracking line search; pass an analytic gradient (or build one with `lib/nr/diff.calc`'s `gradient`). NR §10.7.

The demo solves the standard Rosenbrock function and an extended 5-D variant to f* ~ 1e-25 in fewer than 50 iterations.

#### `lib/nr/pca.calc` — Principal Component Analysis

```calc
extern fn pca_fit(X: arr): map;            // returns {"mean", "axes", "var", "scores"}
extern fn pca_transform(model: map, X_new: arr): arr;
```

Centers the data matrix and SVDs it. The columns of V are the principal directions (sorted by decreasing variance); U·diag(S) gives the projected coordinates. `pca_transform` projects new samples onto an existing model's axes.

### Building demos

Every demo is a single `import "..."; ...` source file. The Makefile targets are one-liners:

```bash
make math_demo            # examples/math_demo.calc — sq, cube, hypot, lerp, ...
make sine_plot            # examples/sine_plot.calc — writes build/sine.svg
make regression           # examples/regression.calc — linear regression + plot
make linsys               # examples/linsys.calc — Ax = b via LU
make numerical            # examples/numerical.calc — roots, integrals, interp
make monte_carlo          # examples/monte_carlo.calc — PRNG class + π estimate
make csv_demo             # examples/csv_demo.calc — CSV write/read/fit/plot
make multi_plot           # examples/multi_plot.calc — multi-series + bar + log-Y
make json_demo            # examples/json_demo.calc — JSON round-trip
make ode_demo             # examples/ode_demo.calc — exp-decay + SHO via RK4
make fft_demo             # examples/fft_demo.calc — spectrum of a synthetic signal
make nr_demo              # examples/nr_demo.calc — Brent + spline + special + Jacobi
make nr_demo2             # examples/nr_demo2.calc — LU + Romberg + RK45 + poly families
make nr_demo3             # examples/nr_demo3.calc — minimization + sort/select
make nr_demo4             # examples/nr_demo4.calc — Ridders' + dist + Newton + LM
make nr_demo5             # examples/nr_demo5.calc — QR + SVD + polyroots + conv
make nr_demo6             # examples/nr_demo6.calc — Chebyshev + Sav-Gol + Kalman + CN
make nr_demo7             # examples/nr_demo7.calc — Cholesky + CG + annealing + MCMC
make nr_demo8             # examples/nr_demo8.calc — Welch + wavelets + Toeplitz + simplex
make nr_demo9             # examples/nr_demo9.calc — 2-D FFT + 2-D quad + power eig + B-spline
make nr_demo10            # examples/nr_demo10.calc — Neville + GL + BFGS + PCA
make demos                # all of the above
```

The Makefile is one `define DEMO_template`:

```makefile
define DEMO_template
$(1): all
	$$(BUILD)/calcnat examples/$(1).calc -o $$(BUILD)/$(1)
	$$(BUILD)/$(1)
endef
$(eval $(call DEMO_template,math_demo))
$(eval $(call DEMO_template,sine_plot))
# ... one $(eval) per demo
```

By hand, every demo is the same shape:

```bash
build/calcnat examples/sine_plot.calc -o build/sine_plot.exe
build/sine_plot.exe                       # writes build/sine.svg
```

The example does `import "plot";` itself; the parser pulls in `lib/plot.calc` (and any libraries that `plot.calc` imports, transitively). No `--lib` step.

### Worked example: linear regression with plot

```calc
import "stats";
import "plot";

// Deterministic in-source PRNG (let-bound closure so it can capture seed).
let seed = 42;
let rand01 = fn() {
    seed = (seed * 1664525 + 1013904223) % 4294967296;
    return seed / 4294967296.0;
};

let xs = [];  let ys = [];
let n = 60;
for (let i = 0; i < n; i = i + 1) {
    let x = i;
    let noise = (rand01() - 0.5) * 12;
    push(xs, x);
    push(ys, 1.7 * x + 4.2 + noise);
}

let fit = linreg(xs, ys);
print "y = " + fit["slope"] + " x + " + fit["intercept"];
print "R^2 = " + fit["r2"];

let ys_fit = [];
for (let i = 0; i < n; i = i + 1) {
    push(ys_fit, fit["slope"] * xs[i] + fit["intercept"]);
}
plot_data_with_fit("regression.svg", xs, ys, ys_fit,
    "linear regression");
```

The fit recovers ~`y = 1.71 x + 4.10` with R² ≈ 0.985 — very close to the true `y = 1.7 x + 4.2` model.

### A note on scoping for engineering code

Two patterns to remember when factoring an engineering program:

1. A **top-level `fn`** does NOT see top-level `let` bindings — top-level fns are compiled as standalone units. Use a **let-bound closure** (`let foo = fn(...) { ... };`) when you need to capture top-level state like a library handle, a PRNG seed, or a configuration map.
2. Library files contribute only their function bodies once `import`ed — any top-level code in the library is dropped. The entry-point file owns `main`. Public surface goes in `pub fn`; private helpers stay `priv fn` (still inlined, but conventionally not called from outside).

These two rules together give you a clean module system: one unit per file, public surface via `pub fn`, private helpers as `priv fn`.

---

## Chapter 11 — Putting it together

Three worked programs that exercise most of the language. Each is short enough to read in one sitting but exercises types, control flow, classes, closures, and the standard library together.

### Program 1: descriptive statistics

```calc
// stats.calc — compute mean, median, and mode of a list of numbers.

class Stats {
    fn init(xs) {
        this.xs = xs;
    }

    fn mean() {
        let total = 0;
        for (let i = 0; i < len(this.xs); i = i + 1) {
            total += this.xs[i];
        }
        return total / len(this.xs);
    }

    fn median() {
        let sorted = array_sort(array_concat(this.xs, []));   // sort a fresh copy
        let n = len(sorted);
        if (n % 2 == 1) {
            return sorted[(n - 1) / 2];
        }
        return (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
    }

    fn mode() {
        let counts = {};
        for (let i = 0; i < len(this.xs); i = i + 1) {
            let k = to_str(this.xs[i]);
            if (has_key(counts, k)) { counts[k] = counts[k] + 1; }
            else                    { counts[k] = 1; }
        }
        let best_k = "";
        let best_n = 0;
        let ks = keys(counts);
        for (let i = 0; i < len(ks); i = i + 1) {
            if (counts[ks[i]] > best_n) {
                best_n = counts[ks[i]];
                best_k = ks[i];
            }
        }
        return to_num(best_k);
    }
}

let s = Stats([1, 2, 2, 3, 4, 4, 4, 5]);
print "mean   = " + s.mean();      // mean   = 3.125
print "median = " + s.median();    // median = 3.5
print "mode   = " + s.mode();      // mode   = 4
```

Compile and run:

```bash
build/calcnat stats.calc
./stats.exe
```

Walk-through of the techniques:

- **Class with three methods** that all read `this.xs`.
- **Copy via concat with []**: `array_concat(this.xs, [])` produces a fresh array; sorting it doesn't mutate the original.
- **Map for tallying**: keying by the string form (`to_str(v)`) lets us tally any numeric value, then convert back.
- **First-pass-then-find pattern**: build the count map, then walk its keys to pick the max.

### Program 2: word frequency from a file

```calc
// word_freq.calc — count word frequencies in a text file.

fn lower_alpha_only(s) {
    let out = "";
    for (let i = 0; i < len(s); i = i + 1) {
        let ch = str_at(s, i);
        if (ch >= "A" && ch <= "Z") {
            // Map A..Z to a..z by ASCII offset via a lookup string.
            let upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
            let lower = "abcdefghijklmnopqrstuvwxyz";
            out += str_at(lower, str_find(upper, ch));
        } else if (ch >= "a" && ch <= "z") {
            out += ch;
        } else {
            out += " ";       // turn punctuation/digits into separators
        }
    }
    return out;
}

fn count_words(path) {
    if (!file_exists(path)) {
        throw "file not found: " + path;
    }
    let text = lower_alpha_only(file_read(path));
    let tokens = str_split(text, " ");
    let counts = {};
    for (let i = 0; i < len(tokens); i = i + 1) {
        let w = tokens[i];
        if (w == "") { continue; }
        if (has_key(counts, w)) { counts[w] = counts[w] + 1; }
        else                    { counts[w] = 1; }
    }
    return counts;
}

fn top_n(counts, n) {
    let ks = keys(counts);
    // Selection-sort the first n positions of a key array by count.
    for (let i = 0; i < n && i < len(ks); i = i + 1) {
        let best = i;
        for (let j = i + 1; j < len(ks); j = j + 1) {
            if (counts[ks[j]] > counts[ks[best]]) { best = j; }
        }
        let tmp = ks[i];  ks[i] = ks[best];  ks[best] = tmp;
    }
    let out = [];
    let limit = min(n, len(ks));
    for (let i = 0; i < limit; i = i + 1) {
        push(out, {"word": ks[i], "count": counts[ks[i]]});
    }
    return out;
}

// Use it:
try {
    let counts = count_words("input.txt");
    let top = top_n(counts, 10);
    for (let i = 0; i < len(top); i = i + 1) {
        print top[i]["word"] + ": " + top[i]["count"];
    }
} catch (e) {
    print "ERROR: " + e;
}
```

This pulls together file I/O, exceptions, string manipulation, maps, and partial sort. The `lower_alpha_only` shows how to do per-character work without `ord`/`chr`. The `top_n` function does an in-place partial selection sort — useful when you want the top-k but don't need to sort the whole array.

### Program 3: complex-arithmetic ODE — the damped harmonic oscillator

```calc
// damped.calc — simulate y'' + 2 zeta omega y' + omega^2 y = 0
// using the standard exp(lambda t) ansatz with complex eigenvalues.

let omega = 1.0;
let zeta  = 0.2;   // < 1 → underdamped

// Characteristic equation: lambda^2 + 2 zeta omega lambda + omega^2 = 0.
// Roots: lambda = -zeta omega ± i omega sqrt(1 - zeta^2).
let real_part = -zeta * omega;
let imag_part = omega * sqrt(1 - zeta * zeta);
let lambda1 = complex(real_part,  imag_part);
let lambda2 = complex(real_part, -imag_part);
print "eigenvalues: " + lambda1 + ", " + lambda2;

// Initial conditions: y(0) = 1, y'(0) = 0.
// General solution: y(t) = c1 * exp(lambda1 t) + c2 * exp(lambda2 t).
// Apply ICs: c1 + c2 = 1 and lambda1*c1 + lambda2*c2 = 0.
// Solve by hand: c2 = lambda1 / (lambda1 - lambda2); c1 = 1 - c2.
let c2 = lambda1 / (lambda1 - lambda2);
let c1 = complex(1, 0) - c2;
print "c1 = " + c1 + ", c2 = " + c2;

// Complex exponential helper: exp(z) = exp(re(z)) * (cos(im(z)) + i sin(im(z))).
fn cexp(z) {
    let r = exp(real(z));
    return complex(r * cos(imag(z)), r * sin(imag(z)));
}

// Sample the response at 20 time points.
print "";
print "t      y(t) (real part)";
for (let i = 0; i <= 20; i = i + 1) {
    let t = i * 0.5;
    let y = c1 * cexp(lambda1 * complex(t, 0))
          + c2 * cexp(lambda2 * complex(t, 0));
    print "" + t + "   " + real(y);
}
```

Output (truncated):

```
eigenvalues: (-0.2, 0.9797958971), (-0.2, -0.9797958971)
c1 = (0.5, 0.1020620726), c2 = (0.5, -0.1020620726)

t      y(t) (real part)
0      1
0.5    0.7766...
1.0    0.4153...
1.5    -0.0008...
...
```

What this exercises:

- **Complex numbers** carrying the eigenvalues and amplitudes without a `{re, im}` map.
- A **closure-free function** (`cexp`) that's totally pure — easy to drop into a numeric library.
- The polymorphic `+ - * /` doing the right thing on `complex / complex`, `complex - complex`, `complex * complex` automatically.
- A loop that samples a continuous function and prints results — the bread-and-butter shape of any simulation program.

### Building up further

Each of these is around 50 lines. Combined with the engineering library (`lib/`) and the NR modules (`lib/nr/`), real applications — fitting a model to data, integrating an ODE, solving a least-squares system, computing an FFT — can all be expressed in another 50 lines. The book's Chapter 10.5 walks through the full library; Chapter 11 stops here because the rest is just composition.

---

## Chapter 12 — Compiler internals (for the curious)

CalcLang is small enough that the whole pipeline is worth understanding end-to-end. This chapter gives a tour.

### The source tree

```
src/lexer.c           tokenize: characters -> tokens
src/parser.c          tokens -> AST (abstract syntax tree)
src/ast.c             AST node constructors / helpers
src/type_infer.c      flow-sensitive type inference; annotate NODE_VAR

— bytecode pipeline —
src/codegen.c         AST -> bytecode assembly (.casm)
src/calcasm.c         .casm -> object file (.co)
src/calcld.c          object files -> executable bytecode (.cexe)
src/calcvm.c          run .cexe on the stack VM

— native pipeline —
src/codegen_x64.c     AST -> x86-64 Intel-syntax assembly (.s)
src/runtime_x64.c     C runtime (alloc + GC + calclib + op dispatch)
src/calcnat.c         drive the native pipeline; invoke gcc

src/calclib.c         shared builtin table (math, strings, etc.)
src/common.c          diagnostics, malloc-with-die, string helpers
```

The whole toolchain is around 6000 lines of C, designed to be read top to bottom.

### Lexer

`lexer.c` is a hand-written character-by-character scanner. The interesting bits:

- **Whitespace and comments** are skipped between tokens (`//` to end of line, `/* */` block).
- **Identifier vs. keyword** is decided by a small lookup table: `let`, `fn`, `if`, `while`, etc. all become distinct token kinds.
- **Number literals** support decimal points, leading `.5`-style, and scientific notation. The exponent marker `e` is only consumed if it's followed by a digit (or `+`/`-` and a digit) — that's the trick that keeps `e()` (Euler's number) tokenizing as an identifier when not in an exponent context.
- **String literals** support a handful of backslash escapes (`\n`, `\t`, `\\`, `\"`, `\r`, `\0`).
- **Source positions** (line, column) are carried with every token so error messages can point precisely.

### Parser

`parser.c` is a recursive-descent parser. Each grammar production is one function: `parse_expr`, `parse_stmt`, `parse_function`, etc. Operator precedence is encoded in the descent order — `parse_or` calls `parse_and`, which calls `parse_equality`, and so on down to `parse_unary` and `parse_primary`.

The AST is a tagged-union tree (`AST*` nodes with a `kind` enum and a union of per-kind data). Heavy use of `cl_die` and `cl_die_at` means errors bail out with a position-tagged message rather than propagating an error return.

### Type inference

`type_infer.c` is the optional layer between parser and codegen. It walks the AST once with a TypeEnv tracking, per scope, which types each variable could currently hold. At branch joins the per-branch envs are merged (union the bitmasks); at loops the body iterates to a fixpoint.

The output is a `n->inferred_type` annotation on every `NODE_VAR` node — `TYPE_NUM`, `TYPE_STR`, `TYPE_ANY`, etc. Codegen reads these to decide whether to emit a fast type-specialized path or a polymorphic call.

This is the layer that was buggy until recently (Chapter 9.5's complex-arithmetic story). The fix in `infer_binop` was a six-line change: widen the result to `TS_ANY_MASK` whenever either operand isn't statically `num`, instead of always returning `TS_NUM_BIT`.

### Optimizer

`optimizer.c` is an AST-rewriting pass that runs between parser and type inference. It applies three categories of transformation:

- **Constant folding** — evaluate compile-time-known expressions at compile time. `2 + 3 * 4` becomes `14` in the AST; `"hello" + " " + "world"` becomes `"hello world"`; bitwise, comparison, and logical ops all fold.
- **Algebraic simplification** — apply trivial identities: `x + 0` → `x`, `x * 1` → `x`, `x | 0` → `x`, etc. Plus short-circuit constants: `0 && expr` → `0`, `1 || expr` → `1` (the RHS is never evaluated, so it doesn't have to be valid).
- **Dead-branch elimination** — `if (0) { A } else { B }` becomes `B`; `while (0) { ... }` becomes a no-op; `do { body } while (0);` becomes just `body`; `cond ? a : b` with a constant condition picks one branch.

The pass walks the AST post-order so children fold first — `if (5 > 3)` becomes `if (1)` first, then the whole conditional collapses. It's safe to call multiple times; it idempotently won't keep simplifying after a fixpoint.

After the AST is lowered to x86-64 assembly, a **peephole pass** scans the emitted text for short patterns:

- `mov reg, 0` → `xor reg, reg` (shorter encoding, same effect).
- `add rsp, 16` immediately followed by `sub rsp, 16` (or vice versa) — both vanish. These appear when an intermediate stack-slot push/pop sandwich collapses.
- `movq rdx, xmm0` immediately followed by `movq xmm0, rdx` — both vanish. Generated when a value moves through a GPR but isn't used in that form.
- `jmp .Lx` immediately before `.Lx:` — the jmp is unnecessary, drop it.

The peephole is purely textual — it splits the emitted assembly into lines, pattern-matches, and stitches back. That keeps it about 200 lines of code. Production compilers do this on an IR with type and dataflow information; we get away with text because the codegen produces highly regular output.

#### Register stack for binop intermediates

When a binary operator's operands are statically proven `num` and the codegen takes the hardware-fast path, the natural pattern is:

```
evaluate left  -> xmm0
save left somewhere
evaluate right -> xmm0
combine left and right
```

CalcLang's first cut used a stack slot for the "save left" step:

```
sub  rsp, 16
movsd [rsp], xmm0
<evaluate right>
movapd xmm1, xmm0
movsd xmm0, [rsp]
add  rsp, 16
addsd xmm0, xmm1
```

That's two memory operations (store + load) per binop. The optimizer replaces them with a small **xmm register stack**: the codegen tracks the current depth of nested binop saves, and at depths 0–3 it saves into `xmm6`, `xmm7`, `xmm8`, `xmm9` instead of memory. Above depth 3 (very rare in practice) it falls back to the stack slot.

```
movapd xmm6, xmm0
<evaluate right>
movapd xmm1, xmm0
movapd xmm0, xmm6
addsd xmm0, xmm1
```

Two register-register moves instead of memory traffic. On numeric inner loops this saves roughly one instruction per binop and a load-store dependency chain.

`xmm6`–`xmm9` are nonvolatile in the Microsoft x64 ABI, so the runtime helpers (`cl_op_plus` etc.) preserve them across calls — they survive even when an inner expression dispatches through the polymorphic path. The function prologue saves the registers actually used (tracked via `cg->xmm_used_mask`) into the locals frame at known rbp-relative offsets; the epilogue restores them. **rbp-relative addressing** is important because `return` statements jump to the epilogue mid-expression when `rsp` is in an unknown state.

This is only the first piece of register allocation — a real graph-coloring or linear-scan allocator would do more (keeping loop induction variables in registers across iterations, eliminating redundant loads from local slots, etc.). The simple register stack is what we have today; the asymptotic complexity stays linear in code size.

#### What's NOT here yet

A real production optimizer also does **common-subexpression elimination**, **loop-invariant code motion**, **inlining**, and a proper register allocator (graph-coloring or linear-scan). Each of those is its own substantial project. The current passes (constant fold + algebraic + dead branch + peephole + simple register stack) are the high-value low-complexity starting points.

### Bytecode codegen and VM

`codegen.c` lowers the AST to a stack-based bytecode in a custom `.casm` text format. `calcasm.c` parses that text into binary `.co` object files. `calcld.c` resolves cross-file symbols and writes a single executable `.cexe`. `calcvm.c` is the interpreter loop:

```c
while (1) {
    Op op = read_op();
    switch (op) {
        case OP_PUSH_NUM:   ... break;
        case OP_ADD:        ... break;
        case OP_CALL:       ... break;
        ...
    }
}
```

Each opcode is a few lines of C. The VM is the simplest piece of the toolchain and a great place to start reading if you want to understand the language semantics without the noise of register allocation.

### Native codegen

`codegen_x64.c` is the more ambitious backend. It walks the same AST and emits x86-64 assembly in Intel syntax, then hands the `.s` file to `gcc` for assembly and linking with `runtime_x64.c`.

The key design choice is **NaN-boxing**: every CalcLang value is a single 64-bit word. IEEE-754 doubles encode normally — a number is just the bit pattern of the double. Any other type is encoded as a quiet-NaN bit pattern with a reserved 16-bit tag in the high bits:

| Tag (hex) | Type     |
|-----------|----------|
| `0xFFF9`  | string   |
| `0xFFFA`  | array    |
| `0xFFFB`  | map      |
| `0xFFFC`  | closure  |
| `0xFFFD`  | fn ref   |
| `0xFFFE`  | complex  |

For numeric programs this means **zero overhead**: an `addsd xmm0, xmm1` works directly on the bit pattern. For polymorphic operations (`+`, `==`, `print`), codegen emits a call to a runtime helper (`cl_op_plus`, `cl_op_eq`) that checks the tags and dispatches.

The flow-sensitive type inference is what makes this fast in practice: when both operands of `+` are statically `num`, codegen emits the hardware op directly, skipping the helper.

#### Function call convention

CalcLang on Windows x64 follows the Microsoft calling convention:

- First four args in `rcx`, `rdx`, `r8`, `r9` (or `xmm0..3` for floats).
- Stack args after that, with the standard 32-byte shadow space.
- Return value in `rax` (or `xmm0`).

CalcLang values are passed as `xmm` registers (treated as doubles, since that's how NaN-boxing works). Calls into the runtime use the same convention.

#### Closures

A closure is represented as a small heap object containing:
- A function-entry pointer.
- An array of "boxes" — each box is a heap-allocated 8-byte word holding one captured variable.

When the outer function creates the closure, it allocates one box per captured local and stores the local's current value in it. References to that local from both the outer and inner functions go through the box. That's how the "by reference" capture semantics are implemented.

When the closure is called, the prologue loads the box array from the closure object and accesses captured variables via box indirection.

#### Tail calls

A `return f(args)` where `f` is the current function compiles to:
1. Evaluate the arg expressions into temporary stack slots.
2. Copy each temp into the corresponding parameter slot.
3. Jump back to the function entry, skipping the prologue.

That's it — no new stack frame, no `call` / `ret`. The result is a flat-stack iterative loop. The codegen recognises only the self-recursive pattern; mutual and indirect tail calls fall back to the normal call path.

### WebAssembly backend *(Stage 1: numeric subset)*

`codegen_wasm.c` is the third backend, alongside the bytecode codegen and the x86-64 native codegen. It walks the same AST and emits **WebAssembly text format** (`.wat`) — the same language the browser, Node.js, wasmtime, wasmer, and every other Wasm runtime understands. One output file, every CPU and OS.

```
foo.calc  ──calcwasm──>  foo.wat  ──wasm runtime──>  output
                                       ↓
                            wasmtime / wasmer / node / browser
                            (Windows x86, macOS ARM, Linux ARM, Raspberry Pi, ...)
```

#### What Stage 1 covers

Numeric subset: number literals, arithmetic, comparison, logical, bitwise operators, variable declarations + assignment, `if`/`else`, `while`, `do-while`, `for` (classic + for-in over ranges), `break`/`continue`, function definitions with `f64` params + `f64` return, direct recursion, the standard math intrinsics (`sqrt`, `sin`, `cos`, `pow`, `sqrt`, etc.), and `print` (for numbers).

What's **not** in Stage 1 (reserved for later stages): strings, arrays, maps, classes, structs, closures, the `import` system, FFI, `read_key`/`sleep_ms`/`time_ms`. Wasm needs a linear-memory + allocator setup for the heap-backed types; that's Stage 2's job.

#### Wasm is a stack machine

Where the x86-64 backend pushes operands into `xmm` registers and emits register-to-register ops, the Wasm backend pushes operands onto an implicit operand stack:

```calc
let x = a + b * 2;
```

compiles to:

```wat
local.get $a
local.get $b
f64.const 2
f64.mul
f64.add
local.set $x
```

Each instruction consumes operands from the top of the stack and pushes its result back. That's much closer to how a JVM bytecode looks than to x86 — and easier to emit, because we never have to allocate registers.

#### Imports and the host

A `.wat` module declares the runtime functions it expects from the host, under named import slots. The host (browser JS, wasmtime, etc.) supplies the implementations. CalcLang's Stage 1 module starts with:

```wat
(module
  (import "env" "print_num" (func $print_num (param f64)))
  (import "env" "sin"  (func $sin  (param f64) (result f64)))
  (import "env" "cos"  (func $cos  (param f64) (result f64)))
  ;; ... pow, log, exp, atan2, pi, e, random, ...
```

The reference host is `tools/wasm_host.py` — a 100-line Python script using the `wasmtime` library:

```bash
pip install wasmtime
build/calcwasm tests/wasm_basic.calc -o build/wasm_basic.wat
python tools/wasm_host.py build/wasm_basic.wat
```

The host fulfills each `(import "env" …)` declaration with a Python function (`math.sin` for `sin`, `print` for `print_num`, etc.) and invokes the module's `_start` export.

#### Control flow translation

Wasm has **structured control flow** — no arbitrary jumps. The constructs are `block`, `loop`, and `if`, with `br` / `br_if` jumping to a labeled enclosing block. CalcLang's `while (cond) { body }` becomes:

```wat
(block $brk
  (loop $cont
    ;; eval cond, jump to $brk if false
    <cond>
    f64.const 0
    f64.ne
    i32.eqz
    br_if $brk
    ;; body
    <body>
    br $cont
  )
)
```

`break` is `br $brk`; `continue` is `br $cont`. Nested loops push/pop labels on a small codegen-side stack.

#### Why not native code on every CPU?

The alternative to one Wasm backend is multiple native backends: x86-64 (done), ARM64, RISC-V, ... — each ~700 lines of CPU-specific assembly. Wasm replaces the matrix with a single backend (Wasm) plus a single runtime (any Wasm engine). The runtime is what knows your CPU; we don't need to.

The price is a thin layer of interpretation/JIT between us and the metal. For numeric kernels that lands around 20–40% slower than hand-tuned native; for everything else it's a wash. That's a great trade for portability.

#### Roadmap

This is **Stage 1 of 4**:

- **Stage 1** (done) — numeric subset, runs in any Wasm engine.
- **Stage 2** — strings + arrays + maps via a Wasm-side linear-memory allocator (compile `runtime_x64.c`'s small-object allocator to Wasm).
- **Stage 3** — WASI bindings so `print`, `read_line`, `file_*`, `time_ms` work under `wasmtime --invoke _start`.
- **Stage 4** — browser harness: an HTML page + JS glue that loads the `.wasm` and wires keyboard + a canvas. Tetris in a browser tab.

### GC

The native runtime uses a **mark-and-sweep collector** with conservative stack scanning, defined in `runtime_x64.c`. The protocol:

1. Every heap allocation goes through `cl_alloc`, which prepends the new object to a singly-linked global allocation list.
2. `cl_alloc` keeps a byte counter. When it crosses 256 KB since the last collection, it triggers GC.
3. **Mark phase**: walk every 8-byte word on the native stack from the current `rsp` to the OS-provided stack base. Treat each word as a potential pointer. If it points into a known allocation, mark it (and recursively mark anything it references). Also follow some root globals (the print buffer, exception state, etc.).
4. **Sweep phase**: walk the allocation list and free anything unmarked.

This is **conservative**: a stack word that happens to look like a pointer keeps that allocation alive for one extra cycle. A false positive can't reclaim a live object; that's the safety property.

Why conservative? It works without compiler cooperation — no GC barriers, no maps of pointer locations, no precise stack roots. The price is occasionally retaining garbage for a cycle.

### Exceptions

`throw` and `try`/`catch` are implemented via direct stack unwinding. When a `try` block enters, it pushes a small record onto a per-thread handler stack: the current `rsp`, `rbp`, and the catch label. `throw` finds the most recent handler, restores those values (effectively rolling back the stack), and jumps to the catch label. Locals in unwound frames become unreachable and are reclaimed by the next GC cycle.

This is simpler than the table-driven exception unwinding C++ uses, at the cost of needing the per-thread handler stack. For a small language with a single thread of control, it's a clean fit.

### The big picture

The whole thing is supposed to be readable. Concretely:

- ~6500 lines of C across the front-end + three back-ends (VM, native, Wasm).
- ~700 lines for the native codegen, ~600 for the runtime, ~500 for the Wasm codegen.
- Building the entire toolchain takes about three seconds on a laptop.

If you want to add a feature — say, a new builtin, a new statement, or a new optimization — the path is:

1. **Lexer** if there's new syntax (a new keyword or operator).
2. **Parser** to construct the new AST node.
3. **AST** to add the node kind.
4. **Type infer** if the new feature interacts with types.
5. **Both codegens** (bytecode + native).
6. **Runtime** if there's new dynamic behaviour.
7. **Test** in `tests/`.

For a builtin (the simplest case), you only touch `calclib.c` and `runtime_x64.c` (for native dispatch). The whole loop, including a test, is usually under 100 lines of changes.

---

## Appendix — Operator precedence

From tightest binding to loosest:

| Level | Operators                       | Associativity |
|------:|---------------------------------|---------------|
|     1 | unary `-`, `!`                  | right         |
|     2 | `*`, `/`, `%`                   | left          |
|     3 | `+`, `-`                        | left          |
|     4 | `<`, `<=`, `>`, `>=`            | left          |
|     5 | `==`, `!=`                      | left          |
|     6 | `&&`                            | left          |
|     7 | `\|\|`                           | left          |

Use parentheses when in doubt.

---

## Appendix — Idioms and common patterns

A short collection of patterns that come up repeatedly in CalcLang code.

### Build, then return

Functions that compute an array or map usually allocate locally, fill it, and return it:

```calc
fn squares(n) {
    let out = [];
    for (let i = 0; i < n; i = i + 1) { push(out, i * i); }
    return out;
}
```

This is idiomatic. Avoid the temptation to mutate a passed-in array — it works (arrays are references), but it's harder to read.

### Map + filter + reduce, defined once

CalcLang doesn't include these as builtins. Define them once, at the top of your program, and use them everywhere:

```calc
fn map(xs, f) {
    let out = [];
    for (let i = 0; i < len(xs); i = i + 1) { push(out, f(xs[i])); }
    return out;
}
fn filter(xs, pred) {
    let out = [];
    for (let i = 0; i < len(xs); i = i + 1) {
        if (pred(xs[i])) { push(out, xs[i]); }
    }
    return out;
}
fn reduce(xs, init, f) {
    let acc = init;
    for (let i = 0; i < len(xs); i = i + 1) { acc = f(acc, xs[i]); }
    return acc;
}
```

### Default-then-override config

For a function with many tweakable parameters, take a config map and merge it with defaults:

```calc
fn defaults() {
    return {"tol": 1e-8, "max_iter": 100, "verbose": 0};
}

fn merge_config(d, override) {
    let out = {};
    let ks = keys(d);
    for (let i = 0; i < len(ks); i = i + 1) { out[ks[i]] = d[ks[i]]; }
    ks = keys(override);
    for (let i = 0; i < len(ks); i = i + 1) { out[ks[i]] = override[ks[i]]; }
    return out;
}

fn solve(f, x0, cfg) {
    cfg = merge_config(defaults(), cfg);
    // use cfg["tol"], cfg["max_iter"], etc.
}

solve(f, 0, {"tol": 1e-12});     // only override tol
```

This avoids 16-parameter function signatures.

### Result objects instead of multiple return values

CalcLang doesn't have tuple return. Use a map:

```calc
fn parse_int(s) {
    // ...
    return {"ok": 1, "value": 42, "pos": 5};
}

let r = parse_int("42 hello");
if (r["ok"]) { print r["value"]; }
```

`{"ok": 0, "error": "..."}` is the typical failure shape. This costs an allocation per call; if perf matters, use `throw` for the failure path and return a bare value on success.

### Class-as-namespace

CalcLang has no namespaces or modules. To group related functions under a single name, build a map at top level:

```calc
let math_ext = {
    "deg_to_rad": fn(d) { return d * pi() / 180; },
    "rad_to_deg": fn(r) { return r * 180 / pi(); },
    "clamp":       fn(x, lo, hi) {
        if (x < lo) { return lo; }
        if (x > hi) { return hi; }
        return x;
    }
};

print math_ext["deg_to_rad"](90);    // 1.570796327
print math_ext["clamp"](15, 0, 10);  // 10
```

Inside the map you get a tiny "namespace." Across files, prefer `pub fn` exports with clear prefixes (`mat_inverse`, `mat_solve`, …).

### Use classes for stateful libraries

A module-level mutable variable doesn't reach into top-level `fn`s in the native backend. The clean workaround is to wrap state in a class:

```calc
pub class Counter {
    fn init(start) { this.n = start; }
    fn bump() { this.n = this.n + 1; return this.n; }
    fn reset() { this.n = 0; }
}
```

Callers do `let c = Counter(0); c.bump();` — the state rides along.

### Closures for callback context

Higher-order calls (root finder, integrator, LM fit, …) take a function `f`. To pass context to `f`, capture it:

```calc
fn make_log_likelihood(data) {
    return fn(params) {
        let ll = 0;
        for (let i = 0; i < len(data); i = i + 1) {
            // ... use data[i] and params
        }
        return ll;
    };
}

let ll = make_log_likelihood(my_dataset);
let best = bfgs_min(fn(p) { return -ll(p); }, grad, [0, 0], 1e-8, 100);
```

Each closure remembers its `data`. You can keep many of them around simultaneously without conflict.

---

## Appendix — Debugging

CalcLang doesn't have a built-in debugger, but the language is small enough that you can usually narrow a bug down in minutes.

### Print everything

`print` accepts any value. When uncertain, print:

```calc
print x;                       // 5
print type_of(x);              // num
print {"x": x, "y": y};        // structured debug line
print "inside loop: i=" + i + " val=" + val;
```

For arrays and maps, `print` writes a JSON-like dump that's usually enough to spot the bug.

### Use `type_of` when types are surprising

A common bug: a value is a string when you thought it was a number (or vice versa). `type_of(x)` tells you what's actually there.

```calc
let val = some_function();
print "type = " + type_of(val) + " value = " + val;
```

### Trace down with intermediate prints

Recursive code is hardest to debug by inspection. Print at the entry and exit:

```calc
fn fact(n) {
    print "fact(" + n + ")";
    if (n <= 1) {
        print "  returning 1";
        return 1;
    }
    let r = n * fact(n - 1);
    print "  fact(" + n + ") = " + r;
    return r;
}
```

### Catch and re-throw

When you suspect a specific function is throwing, catch the exception and print it before re-throwing:

```calc
try {
    risky_operation();
} catch (e) {
    print "DEBUG: caught " + e;
    throw e;
}
```

### Run the smaller pipeline

If a native-only feature is misbehaving, run the same source through the bytecode pipeline (where it might not be supported, but at least the error message will be clearer about what's missing).

### View generated assembly

When a numeric inner loop is slow or behaves weirdly, dump the assembly:

```bash
build/calcnat -S myprog.calc myprog.s
```

Look for `call cl_op_plus` etc. — if you see those on a hot path, the type inferrer didn't prove both operands `num` and you're paying the polymorphic-dispatch tax. Add type annotations or simplify the expression.

### Diff against the VM

Both backends are designed to produce byte-identical output. When the native output looks wrong:

```bash
build/calcc   myprog.calc build/m.casm && \
build/calcasm build/m.casm build/m.co && \
build/calcld  build/m.co   build/m.cexe && \
build/calcvm  build/m.cexe > vm.out

build/calcnat myprog.calc -o m.exe && \
./m.exe > nat.out

diff vm.out nat.out
```

If the two disagree, one of the backends has a bug — and the test harness in `tests/run_tests.sh` is your friend for narrowing down which.

---

## Appendix — Performance tips

CalcLang is meant to be small and clear, not a barn-burner. But you can still get reasonable performance with a few habits.

### Annotate hot functions

The single biggest lever. A function declared `fn dist(x: num, y: num): num { ... }` compiles to hardware floating-point ops; the same function with no annotations goes through the polymorphic runtime helpers.

A 5×–10× speedup is typical for numeric inner loops.

### Build strings in a file, not in memory

`str + str + ...` in a loop is O(n²). For large generated output (SVG, CSV, JSON), use `file_append`:

```calc
// BAD: O(n^2)
let svg = "<svg ...>";
for (let i = 0; i < 100000; i = i + 1) {
    svg = svg + "<circle cx=\"" + i + "\"/>";
}
svg = svg + "</svg>";
file_write("out.svg", svg);

// GOOD: O(n)
file_write("out.svg", "<svg ...>");
for (let i = 0; i < 100000; i = i + 1) {
    file_append("out.svg", "<circle cx=\"" + i + "\"/>");
}
file_append("out.svg", "</svg>");
```

### Reuse allocations

Each `let v = [...]` in a hot loop allocates. If the inner loop body is large enough that this matters, hoist the allocation out and clear/refill it:

```calc
// Slower:
for (let i = 0; i < N; i = i + 1) {
    let buf = [];
    for (let j = 0; j < M; j = j + 1) { push(buf, j); }
    // ... use buf
}

// Faster:
let buf = [];
for (let j = 0; j < M; j = j + 1) { push(buf, 0); }  // initial size
for (let i = 0; i < N; i = i + 1) {
    for (let j = 0; j < M; j = j + 1) { buf[j] = j; }
    // ... use buf
}
```

### Cache `len`

`len(xs)` is O(1) but it does involve a tag check and a pointer dereference. In tight inner loops, hoist:

```calc
let n = len(xs);
for (let i = 0; i < n; i = i + 1) { ... }
```

The compiler doesn't (yet) automatically hoist this.

### Prefer `for` with a counter over closure-based iteration

`map(xs, fn(x) { ... })` is convenient but allocates a fresh array and pays one closure call per element. In a hot loop, the explicit `for` is faster:

```calc
// Faster:
for (let i = 0; i < len(xs); i = i + 1) {
    // do work
}

// Slower but cleaner:
map(xs, fn(x) { /* do work */ });
```

Use whichever fits the situation.

### Watch the GC threshold

The GC fires every 256 KB of allocations. A program that allocates tightly in a loop pays for many sweeps. If you can reuse buffers (previous tip), you skip both the allocations and the sweeps.

### Profile by timing chunks

CalcLang has no built-in profiler. Approximate one with `system("date +%s%N > timestamp")` before and after a chunk, then read the timestamps back as numbers and subtract. Or shell out to a `time` command on the whole program.

### Native vs. VM

The native backend is typically 3×–10× faster than the bytecode VM for numeric code, and roughly equivalent for IO-bound code. Use native for production runs; VM for cross-checking and quick scripts.

---

## Appendix — Limitations to keep in mind

- Types are dynamic; an annotation is enforced but the compiler can't always tell at compile time. Don't rely on it for performance unless you've checked the generated assembly.
- The bytecode VM doesn't support several native-only features: file I/O, shell commands, complex numbers, FFI, exceptions, tail-call optimization.
- The GC is conservative; in a long-running program with stack patterns that happen to look like pointers, dead allocations may linger one cycle longer than strictly necessary. Live objects are never reclaimed prematurely.
- No pattern matching, no generics, no traits, no async, no operator overloading, no modules-as-files (compilation is per-file but linking is one big symbol pool). The language stays small on purpose.
- Numbers are 64-bit doubles only; no 32-bit ints, no big integers, no rationals. The `2^53` threshold for exact integer representation is the practical ceiling.
- Strings hold raw bytes, not code points. UTF-8 sequences pass through unchanged but `len`, `str_at`, and `str_slice` are byte-oriented.
- `array_sort` doesn't accept a comparator.
- No multi-line raw strings; embed newlines as `\n`.

That's the language. Read the test files under `tests/` for many more worked examples, the engineering library under `lib/` for production-flavoured code, or just write a program and run it.
