# CalcLang

A small, dynamically-typed programming language with a complete educational compiler toolchain. The whole package — lexer, parser, type inference, two backends, garbage collector, runtime, and an engineering-flavored standard library — is plain C and CalcLang, designed to be readable end-to-end.

It produces real native `.exe` (Windows / PE) and ELF (Linux) executables via a tiny x86-64 backend, AND keeps a parallel bytecode pipeline that runs on the bundled `calcvm` interpreter. The bytecode path was the original; the native path is byte-identical and is what most users will use day-to-day.

**[Read the book →](docs/book.md)** A K&R-style tutorial from hello-world to closures, classes, multi-file programs, the engineering library, and the compiler internals.

## Quickstart

```bash
git clone <repo-url> calclang
cd calclang
make             # builds calcc, calcasm, calcld, calcvm, calcnat
make test        # runs the full test suite — VM + native, byte-identical
make demos       # builds + runs every engineering-library demo
```

You need `gcc` on PATH (MinGW on Windows, system gcc on Linux/macOS) and `sh`. No other dependencies.

After `make demos`, open `build/sine.svg`, `build/regression.svg`, `build/fft_mag.svg`, or any of the other generated SVGs in a browser to see the plotting library output.

## Table of contents

- [What's in the box](#whats-in-the-box)
- [Hello world](#hello-world)
- [The two pipelines](#the-two-pipelines)
- [Engineering standard library](#engineering-libraries)
- [Native x86-64 backend](#native-x86-64-backend)
- [Multi-file native builds](#multi-file-native-builds)
- [Tail-call optimization](#tail-call-optimization)
- [Exceptions](#exceptions)
- [FFI — dynamic linking + first-class C calls](#ffi--dynamic-linking--first-class-c-calls)
- [Phase-1 engineering additions (file I/O, system, complex)](#phase-1-engineering-additions-file-io-system-complex)
- [Project layout](#filemodule-map)
- [Limitations](#current-limitations)
- [License](#license)

## What's in the box

A real compiled educational toolchain:

```text
CalcLang source (.calc)
  ├─► .casm ─► .co ─► .cexe ─► calcvm        (bytecode VM)
  └─► .s ──────────── gcc ──► .exe / ELF      (native x86-64)
```

Language features:

- Dynamically-typed: `num`, `str`, `arr` (heterogeneous arrays), `map`, `fn`, `cpx` (first-class complex numbers).
- First-class functions, closures with mutable transitive capture, classes (sugar over maps + closures).
- Flow-sensitive type inference with union types at branch joins.
- Exceptions (`throw` / `try` / `catch`) and tail-call optimization in the native backend.
- 52 calclib builtins (math, trig, strings, arrays, maps, I/O), plus 10 native-only additions (file I/O, `system`, complex constructors, FFI).
- Multi-file builds with `pub fn` / `extern fn` linking across compilation units.
- Mark-and-sweep GC with conservative stack scanning (native).
- A standalone tutorial book under [docs/book.md](docs/book.md).
- An engineering standard library written entirely in CalcLang under [lib/](lib/).

**Engineering libraries** under [lib/](lib/) — all written in CalcLang on top of the language and the native runtime:

- **`linalg.calc`** — matrices, `mat_solve`/`mat_det`/`mat_inv`, vector and matrix norms.
- **`stats.calc`** — mean, stddev, median, Pearson correlation, linear regression, histogram.
- **`numeric.calc`** — root finding (bisect / Newton / secant), integration (trapezoidal / Simpson / adaptive), polynomial eval and derivative, linear interpolation.
- **`random.calc`** — class-based reproducible PRNG with uniform, normal (Box-Muller), Poisson, exponential, choice, shuffle.
- **`csv.calc`** — read/write tabular data with optional headers.
- **`plot.calc`** — SVG line / scatter / multi-series / bar / log-Y / labeled plots, streamed to disk.
- **`json.calc`** — JSON parser + encoder for round-tripping data.
- **`ode.calc`** — fourth-order Runge-Kutta for scalar and vector ODEs (plus Euler for comparison).
- **`fft.calc`** — Cooley-Tukey radix-2 FFT using CalcLang's first-class complex numbers.

Plus, under [`lib/nr/`](lib/nr/) (extended via the `numerical-library` branch), **Numerical-Recipes-style algorithms**:

- **`nr/brent.calc`** — Brent's root finder (robust bracketed method with inverse-quadratic interpolation).
- **`nr/spline.calc`** — natural cubic spline interpolation (two-step `setup`/`eval` interface, NR style).
- **`nr/special.calc`** — gamma / lgamma / beta / erf / erfc via Lanczos + Abramowitz rational Chebyshev.
- **`nr/eigen.calc`** — Jacobi eigenvalue decomposition for real symmetric matrices (eigenvalues + eigenvectors).
- **`nr/lu.calc`** — Doolittle LU decomposition with partial pivoting; solves, determinant, factorization reuse for multiple RHS.
- **`nr/romberg.calc`** — Romberg integration: trapezoidal table + Richardson extrapolation, machine-precision in 5-10 levels.
- **`nr/rk45.calc`** — adaptive Cash-Karp RK45 with step-size control; handles stiff problems.
- **`nr/poly.calc`** — orthogonal-polynomial families: Chebyshev T_n, Legendre P_n, Hermite H_n, Laguerre L_n, Bessel J_0/J_1.
- **`nr/minimize.calc`** — 1-D minimization (golden section, Brent's parabolic method) and Nelder–Mead downhill simplex for N-D.
- **`nr/sort.calc`** — heapsort (in-place, with optional index-companion permutation) and quickselect / median (O(n) average).
- **`nr/diff.calc`** — numerical differentiation by Ridders' polynomial extrapolation: scalar `deriv`, multivariate `gradient` / `jacobian`.
- **`nr/random_dist.calc`** — gamma (Marsaglia–Tsang), chi², beta, Student's t, Cauchy, binomial, geometric, and triangular sampling on top of `Rng`.
- **`nr/newton.calc`** — Newton-Raphson for nonlinear systems with Armijo line search; analytic or numerical Jacobian.
- **`nr/fitnl.calc`** — Levenberg-Marquardt nonlinear least squares with parameter covariance estimate.
- **`nr/qr.calc`** — QR decomposition via Householder reflections; solves square and over-determined least-squares systems.
- **`nr/svd.calc`** — singular value decomposition (via A^T A + Jacobi); pseudo-inverse and rank-revealing LS solve.
- **`nr/polyroots.calc`** — all roots of a real or complex polynomial via Laguerre's method with synthetic-division deflation and a polish pass.
- **`nr/conv.calc`** — direct and FFT-based linear convolution and cross-correlation.
- **`nr/cheb.calc`** — Chebyshev approximation of a function on [a, b] with Clenshaw evaluation and exact coefficient-level differentiation / integration.
- **`nr/savgol.calc`** — Savitzky-Golay filter coefficients and apply (smoothing or derivatives) — preserves peaks far better than a moving average.
- **`nr/kalman.calc`** — discrete-time Kalman filter (predict + update) for arbitrary linear Gaussian state-space models.
- **`nr/pde.calc`** — Crank-Nicolson scheme for the 1-D heat equation, unconditionally stable, second-order accurate.
- **`nr/cholesky.calc`** — Cholesky factorization for SPD matrices: decompose, solve, log-determinant, inverse.
- **`nr/cg.calc`** — conjugate-gradient solver for SPD linear systems (basic and preconditioned variants).
- **`nr/anneal.calc`** — simulated annealing for combinatorial and continuous global optimization with user-supplied proposal.
- **`nr/mcmc.calc`** — Metropolis-Hastings sampler in log-space; symmetric random-walk convenience wrapper for R^n targets.
- **`nr/welch.calc`** — Welch periodogram for power spectral density estimation with Hann tapering and overlapping segments.
- **`nr/wavelet.calc`** — discrete wavelet transforms: Haar and Daubechies-4 forward and inverse; periodic boundary handling.
- **`nr/toeplitz.calc`** — Levinson-Durbin recursion for symmetric Toeplitz systems in O(n²) and the Yule-Walker AR estimator built on top.
- **`nr/simplex_lp.calc`** — two-phase simplex method for `max c^T x s.t. A x <= b, x >= 0`; reports optimal / unbounded / infeasible.
- **`nr/fft2d.calc`** — two-dimensional FFT and inverse via row-then-column transforms (built on `lib/fft.calc`).
- **`nr/quad2d.calc`** — 2-D Gauss-Legendre quadrature (orders 2-5) and adaptive recursive 2-D integration.
- **`nr/power_eigen.calc`** — power iteration for the dominant eigenvalue and inverse iteration for the eigenvalue closest to a shift.
- **`nr/bspline.calc`** — B-spline basis evaluation (Cox-de Boor), spline evaluation, and least-squares curve fitting on arbitrary knot vectors.
- **`nr/neville.calc`** — Neville's algorithm: polynomial interpolation through few points with built-in error estimate.
- **`nr/glnodes.calc`** — generate Gauss-Legendre nodes / weights at arbitrary order via Newton iteration on Bonnet's recurrence; `integrate_gauleg` wraps it.
- **`nr/bfgs.calc`** — BFGS quasi-Newton minimization with Armijo back-tracking line search; uses analytic gradients.
- **`nr/pca.calc`** — Principal Component Analysis via SVD of the centered data matrix; returns variances, axes, and scores.

## Hello world

```calc
print "hello, world";
```

```bash
build/calcnat hello.calc
./hello.exe              # Windows; on Linux: ./hello
```

## The two pipelines

Both come from the same lexer / parser / AST. Past the AST, the codegen and runtime diverge:

- **Bytecode VM** (`calcc` → `calcasm` → `calcld` → `calcvm`) — the original pipeline. Compiles to a custom `.cexe` bytecode that runs on a stack-based VM in `calcvm.c`. Useful as a reference implementation and what the test suite diff-checks against.
- **Native** (`calcnat`) — emits Intel-syntax x86-64 assembly with NaN-boxed Values, pipes it through `gcc`, and links against [`src/runtime_x64.c`](src/runtime_x64.c) (the GC + runtime helpers). Produces a real OS executable.

## Language Syntax

```text
let x = 10;
let y = 5;
let z = x * y + 20;
print z;
print (z - 10) / 2;

// Block scope: inner `let` shadows; `=` reassigns existing.
let i   = 1;
let sum = 0;
while (i <= 10) {
    sum = sum + i;
    i   = i + 1;
}
print sum;            // 55

// for + break + continue.
for (let k = 0; k < 10; k = k + 1) {
    if (k > 6)         { break; }
    if (k % 2 == 0)    { continue; }
    print k;          // 1 3 5
}

// if / else if / else with logical && and ||.
if (sum % 2 == 0 && sum > 0) {
    print 1;
} else if (sum < 0 || !sum) {
    print 0;
} else {
    print -1;
}

// Functions.
fn square(n) {
    return n * n;
}
print square(9);      // 81

// Classes: `this` is the receiver; methods are closures that capture
// it. `Point(3, 4)` calls the constructor; `p.dist()` dispatches a
// method.
class Point {
    fn init(x, y) {
        this.x = x;
        this.y = y;
    }
    fn dist() {
        return sqrt(this.x * this.x + this.y * this.y);
    }
}
let p = Point(3, 4);
print p.dist();       // 5
```

Supported features:

- decimal numbers (IEEE 754 doubles)
- string literals: `"hello"` with escapes `\n \t \r \\ \"` (max 127 chars per literal)
- declarations: `let name = expr;` or with an optional type annotation `let name: T = expr;` (T is checked at compile time when the RHS is a literal, and at runtime via `TYPECHECK_TOP` for everything else). The compiler infers types from the RHS — `let x = 5;` records `x` as `num`, `let s = to_str(42);` records `s` as `str`. Type inference is **flow-sensitive**: each *use* of a variable is typed at the program point it occurs, so `let x = 5; x = "hi"; print x + "!";` correctly types `x` as `num` in the first use and `str` in the second. At branch joins types are unioned (so if one arm of an `if` assigns `num` and the other `str`, post-join `x` is the union and behaves like `any` for static checks); loops iterate to a fixpoint. This catches errors at compile time that the older per-symbol inference would have deferred to runtime — see [tests/flow_inference.calc](tests/flow_inference.calc)
- assignment: `name = expr;` (reassigns the nearest visible binding; the variable must already exist)
- variables can hold either a number or a string — the type can change across reassignments
- `print expr;` (numbers print with `%.10g`, strings print as-is)
- arithmetic: `+`, `-`, `*`, `/`, `%`, unary `-` (numbers only — except `+` also concatenates strings, and when one operand of `+` is a string the other is coerced via `%.10g` so `"x = " + 5` produces `"x = 5"`)
- compound assignment: `+=`, `-=`, `*=`, `/=`, `%=` and the postfix statements `x++;` / `x--;` (all desugar to plain `x = x ⊕ e;`; `s += "x"` therefore works on strings)
- comparison: `<`, `<=`, `>`, `>=` (numbers numerically; strings lexicographically; mixed types are a type error), `==`, `!=` (any types — different types are never equal)
- logical: `&&`, `||`, `!` (number `0` and the empty string `""` are falsy; everything else is truthy; results are 1.0 / 0.0)
- control flow: `if (cond) stmt`, `if (cond) stmt else stmt`, `while (cond) stmt`, `for (init; cond; step) stmt`
- loop control: `break;`, `continue;` (legal only inside a loop; `continue` targets the step of a `for`)
- functions: `fn name(p1, p2, ...) { body }`, called as expressions `name(arg1, arg2)`; `return expr;` or bare `return;` (returns 0); a body that falls off the end implicitly returns 0
- **first-class function values**: a bare function name (no parens) evaluates to a function reference that can be stored, passed, and called: `let f = add; print f(2, 3);`. **calclib builtins are also first-class** — the codegen emits a tiny `.bi_<name>` trampoline on demand, so `map(xs, sqrt)` Just Works. This is what `map` / `filter` / `reduce` are written on top of — they live in user code, not as VM builtins
- **anonymous function expressions and closures**: `fn(params) { body }` is an expression that produces a function value. The closure captures the enclosing function's locals; captures are **mutable** — assigning to a captured name inside the closure writes back to the original binding, so the classic counter pattern works: `fn make_counter() { let n = 0; fn inc() { n = n + 1; return n; } return inc; }`. Capture is also **transitive**: an inner fn can reference a local declared three or more levels out, threaded automatically through the intermediate scopes
- **nested function definitions**: `fn name(params) { body }` inside a block is sugar for `let name = fn(params) { body };` — same closure machinery, named binding
- type annotations on functions (optional): `fn add(a: num, b: num): num { ... }`. Allowed types are `num`, `str`, `arr`, `map`, `fn`, `bool` (alias for `num`), and `any`. Annotations are **enforced** in two complementary ways:
  - **Compile-time**: literal arguments whose type doesn't match the annotation are rejected with `semantic error: argument N to 'foo': expected X, got Y`. Arg-count mismatches are caught here too.
  - **Runtime**: a `TYPECHECK` opcode at function entry checks every typed parameter when the value's type couldn't be determined at compile time. Trap message: `runtime type error: parameter at offset N expected X, got Y`.
  - Anything annotated `any` skips both checks; the cost of fully-dynamic functions is zero.
- visibility: top-level functions are **private** by default. `pub fn name(...) { ... }` exports the function for multi-file linking; `priv fn name(...) { ... }` is the explicit form of the default. Inside one source file both kinds are called the same way; the difference shows up in the generated `.casm` (`fn_name:` vs `.fn_name:`) and the object file (`SYMS` vs `LOCALSYMS`)
- extern declarations: `extern fn name(params): T;` forward-declares a function defined in another compilation unit. Implicitly public; resolved by `calcld` against another object file's `SYMS`. See `examples/multi/` for a worked example
- call-as-statement: `f(args);` on its own line discards the return value
- arrays: `[a, b, c]` literal (heterogeneous, mutable), `arr[i]` read, `arr[i] = v;` write, **chained writes `m[i][j] = v;` and indexed compound assignment `m[i] += v;` / `arr[i]++;` work too**, reference semantics (`let b = a;` aliases); empty `[]` is falsy
- maps: `{ "k": v, ... }` literal, `m[k]` read, `m[k] = v;` write. Keys can be strings or numbers; values can be any type. Reference semantics like arrays; empty `{}` is falsy. Reading a missing key is a runtime error — guard with `has_key` first
- **struct-like field access**: `obj.field` is sugar for `obj["field"]` on both reads and writes, and composes freely with `[]` indexing: `team.members[0].name = "Bob";` works. There is no separate struct kind — maps with `.` access cover the ergonomic gap
- **classes**: `class Name { fn init(p1, p2) {...} fn method() {...} }` declares a constructor. `let obj = Name(a, b);` builds an instance; `obj.field` reads/writes data, `obj.method(args)` dispatches a method. `this` inside a method refers to the receiver. Classes are pure parser-level sugar — a class desugars to a constructor function that builds a map, attaches each method as a closure capturing `this`, then inlines the init body. Methods are first-class function values: `let f = obj.method; f();` works, and `pub class` exports the constructor for cross-file use. See [tests/classes.calc](tests/classes.calc) for the full surface.
- **indirect calls**: any expression that evaluates to a function value can be called with `(args)` — `obj.method(args)`, `arr[i](args)`, `make_fn()()` all parse and dispatch correctly
- **calclib**: 52 functions baked into the VM, called by bare name. See the calclib reference below
- `{ ... }` block introduces a new variable scope
- parentheses
- `//` line comments and `/* ... */` block comments (block comments do **not** nest — the first close marker terminates)

Operator precedence, highest to lowest: unary `-` / `!`, then `* / %`, then `+ -`, then `< <= > >=`, then `== !=`, then `&&`, then `||`.

Scoping rules at a glance:

- Each `{ ... }` block, each function body, and the implicit scope around a `for` (so the init binding stays local) are full scopes.
- `let x = ...` in an inner scope creates a fresh binding; the outer `x` is untouched and visible again once the inner scope ends.
- `let x = ...` repeated in the *same* scope is a reassignment, not an error.
- `x = expr;` reassigns the closest enclosing binding; if `x` is not in scope you get a semantic error.

Identifiers beginning with `.` are reserved by the code generator for file-local jump labels. Function entry labels look like `fn_<name>` and are exported globally.

## Multi-file builds

CalcLang can compile and link multiple source files together. Public (`pub`) functions in one file are visible to other files that forward-declare them with `extern`. See `examples/multi/` for the full demo (a `stats` library plus a `report` consumer).

```bash
# Compile each source independently:
build/calcc.exe   examples/multi/stats.calc  build/stats.casm
build/calcc.exe   examples/multi/report.calc build/report.casm

# Assemble each .casm to an object file:
build/calcasm.exe build/stats.casm  build/stats.co
build/calcasm.exe build/report.casm build/report.co

# Link: the FIRST .co is the entry point — its top-level code runs
# first. Library files (functions only, no top-level work) come after.
build/calcld.exe  build/report.co build/stats.co build/multi.cexe

build/calcvm.exe  build/multi.cexe
```

`extern fn name(params): T;` is implicitly public — only `pub` symbols cross the file boundary. The linker resolves the reference against another object file's `SYMS` section; private functions (bare `fn`) remain in `LOCALSYMS` and cannot be referenced from outside.

## VSCode editor support

A small extension under `editor/vscode/calclang/` provides syntax highlighting, bracket matching, and comment-toggle for `.calc` and `.casm` files. Copy that folder into your VSCode extensions directory and reload — full instructions in [editor/vscode/calclang/README.md](editor/vscode/calclang/README.md).

## Build

```bash
make
```

On Windows with MinGW/MSYS2:

```bash
gcc -std=c11 -Wall -Wextra -pedantic -Iinclude src/common.c src/calclib.c src/lexer.c src/ast.c src/parser.c src/symbol_table.c src/codegen.c src/type_infer.c src/calcc.c -o build/calcc.exe
gcc -std=c11 -Wall -Wextra -pedantic -Iinclude src/common.c src/calcasm.c -o build/calcasm.exe
gcc -std=c11 -Wall -Wextra -pedantic -Iinclude src/common.c src/calcld.c -o build/calcld.exe
gcc -std=c11 -Wall -Wextra -pedantic -Iinclude src/common.c src/calclib.c src/calcvm.c -o build/calcvm.exe
gcc -std=c11 -Wall -Wextra -pedantic -Iinclude src/common.c src/calclib.c src/lexer.c src/ast.c src/parser.c src/codegen_x64.c src/type_infer.c src/calcnat.c -o build/calcnat.exe
```

## Run Demo

```bash
make example
```

Manual pipeline:

```bash
build/calcc examples/demo.calc build/demo.casm
build/calcasm build/demo.casm build/demo.co
build/calcld build/demo.co build/demo.cexe
build/calcvm build/demo.cexe
```

Expected output:

```text
70
30
20
30
```

## Show AST

```bash
build/calcc --ast examples/demo.calc build/demo.casm
```

## Run Tests

```bash
make test
```

## File/Module Map

```text
include/lexer.h          token types and lexer API
src/lexer.c              source text -> tokens

include/ast.h            AST node definitions
src/ast.c                AST constructors/debug/free

include/parser.h         parser API
src/parser.c             tokens -> AST

include/symbol_table.h   variables -> memory slots
src/symbol_table.c       symbol table implementation

include/type_infer.h     flow-sensitive type inference API
src/type_infer.c         forward dataflow pass annotating NODE_VARs

include/codegen.h        code generator API
src/codegen.c            AST -> Calc assembly

src/calcc.c              compiler driver
src/calcasm.c            assembler: .casm -> .co
src/calcld.c             linker: .co -> .cexe
src/calcvm.c             VM runtime: executes .cexe

include/codegen_x64.h    native backend API
src/codegen_x64.c        AST -> x86-64 assembly (Intel syntax)
src/calcnat.c            native compiler driver: .calc -> .s

include/runtime.h        NaN-boxed Value + runtime entry points
src/runtime_x64.c        native runtime library (linked with output)
```

## Important Design Idea

For an interpreted language, the AST is evaluated directly.

For this compiled language, the AST is not directly evaluated. Instead:

```text
AST -> assembly -> object -> linked executable bytecode -> VM execution
```

That means the compiler produces a lower-level program that runs later.

## calclib reference

calclib functions are dispatched via a single `BUILTIN <id>` opcode in the VM. They are called by bare name from CalcLang source.

**Math** (17)

| Function       | Args                | Returns                                                |
|----------------|---------------------|--------------------------------------------------------|
| `sqrt(x)`      | num                 | √x (errors on negative)                                |
| `floor(x)`     | num                 | greatest integer ≤ x                                   |
| `ceil(x)`      | num                 | smallest integer ≥ x                                   |
| `abs(x)`       | num                 | absolute value                                         |
| `pow(b, e)`    | num, num            | `b ** e`                                               |
| `min(a, b)`    | num, num            | smaller of two                                         |
| `max(a, b)`    | num, num            | larger of two                                          |
| `int(x)`       | num                 | truncate toward zero                                   |
| `round(x)`     | num                 | round half away from zero                              |
| `sin(x)` / `cos(x)` / `tan(x)` | num   | trig (radians)                                         |
| `asin(x)` / `acos(x)` | num          | inverse trig (errors outside [-1, 1])                  |
| `atan(x)`      | num                 | inverse tangent                                        |
| `atan2(y, x)`  | num, num            | inverse tangent of y/x                                 |
| `exp(x)`       | num                 | e^x                                                    |
| `log(x)`       | num                 | natural log (errors on x ≤ 0)                          |
| `log10(x)`     | num                 | base-10 log                                            |
| `random()`     | —                   | uniform random in [0, 1)                               |
| `pi()`         | —                   | `3.141592653589793`                                    |
| `e()`          | —                   | `2.718281828459045`                                    |

**Strings** (11)

| Function                          | Args            | Returns                                              |
|-----------------------------------|-----------------|------------------------------------------------------|
| `to_str(x)`                       | num or str      | string form using `%.10g`; idempotent on strings     |
| `to_num(s)`                       | str or num      | parses with `strtod`; rejects empty / trailing garbage |
| `len(s)`                          | str             | byte length                                          |
| `str_at(s, i)`                    | str, num        | single-char string at index `i`                      |
| `str_slice(s, start, end)`        | str, num, num   | substring `[start, end)`                             |
| `str_find(s, sub)`                | str, str        | index of first match, `-1` if not found              |
| `str_upper(s)` / `str_lower(s)`   | str             | case-converted copy                                  |
| `str_trim(s)`                     | str             | strips leading/trailing whitespace                   |
| `str_repeat(s, n)`                | str, num        | `s` repeated `n` times                               |
| `str_starts_with(s, p)` / `str_ends_with(s, p)` | str, str | 1 or 0                                       |
| `str_split(s, sep)`               | str, str        | array of substrings                                  |
| `str_join(arr, sep)`              | arr (of str), str | concatenated string                                |

**Arrays** (9)

| Function                          | Args            | Returns                                              |
|-----------------------------------|-----------------|------------------------------------------------------|
| `len(arr)`                        | arr             | element count                                        |
| `push(arr, v)`                    | arr, value      | appends in place, returns new length                 |
| `pop(arr)`                        | arr             | removes and returns last element                     |
| `array_reverse(arr)`              | arr             | reverses in place, returns the array                 |
| `array_sort(arr)`                 | arr             | sorts in place (all-num or all-str), returns the array |
| `array_concat(a, b)`              | arr, arr        | new array with elements of both                      |
| `array_slice(arr, start, end)`    | arr, num, num   | new array `[start, end)`                             |
| `array_find(arr, v)`              | arr, value      | index of first match, `-1` if not found              |
| `array_contains(arr, v)`          | arr, value      | 1 or 0                                               |
| `array_range(start, end)`         | num, num        | new array of integers `[start, end)`                 |

**Maps** (5)

| Function          | Args            | Returns                                              |
|-------------------|-----------------|------------------------------------------------------|
| `len(map)`        | map             | entry count                                          |
| `keys(map)`       | map             | array of keys in insertion order                     |
| `values(map)`     | map             | array of values in insertion order                   |
| `has_key(map, k)` | map, key        | 1 or 0                                               |
| `del(map, k)`     | map, key        | 1 if removed, 0 if key wasn't present                |

**I/O and introspection** (3)

| Function       | Args                | Returns                                                |
|----------------|---------------------|--------------------------------------------------------|
| `read_line()`  | —                   | one line from stdin (`\n`/`\r` stripped); `""` at EOF  |
| `write(x)`     | num or str          | prints without newline; returns 0                      |
| `type_of(x)`   | any                 | `"num"`, `"str"`, `"arr"`, `"fn"`, or `"map"`          |

## Native x86-64 backend

In addition to the VM pipeline, CalcLang ships a **native** backend that emits x86-64 assembly (Intel syntax, GAS-compatible) and pipes it through `gcc` to produce a real OS executable — a `.exe` on Windows (PE) or an ELF binary on Linux. Same lexer/parser/AST as the VM; only the codegen and tooling diverge past the AST.

```bash
# One-step build: calcnat invokes gcc internally and links the runtime.
build/calcnat tests/native_basic.calc
./tests/native_basic.exe        # or pass `-o build/foo.exe` for a custom output

# Still works if you want the intermediate assembly:
build/calcnat -S tests/native_basic.calc build/native_basic.s
gcc build/native_basic.s src/runtime_x64.c -Iinclude -o build/native_basic.exe
```

### Value representation

Every CalcLang value at runtime is a 64-bit word using **NaN boxing**:

- A regular IEEE-754 double is stored as itself. Any non-NaN double — or a NaN whose top 16 bits don't match a reserved tag — IS the value. Numeric programs pay zero overhead; `addsd`/`subsd`/etc. operate directly.
- A heap-typed value is a quiet NaN with negative sign whose top 16 bits select the type and whose bottom 48 bits hold a pointer:
  - `0xFFF9` — `CalcStr *` (length-prefixed UTF-8 byte sequence)
  - `0xFFFA` — `CalcArr *` (dynamic array of `Value`s; reference semantics)
  - `0xFFFB` — `CalcMap *` (parallel-array hashmap, insertion-ordered, num-or-str keys; reference semantics)
  - `0xFFFC` — `CalcClosure *` (code pointer + upvalue table; covers anonymous fns, nested fns, captured locals, classes, first-class user-fn refs, AND first-class calclib-builtin refs via auto-generated trampolines)
  - `0xFFFD` — reserved (no users yet)
  - `0xFFFE` — `CalcCpx *` (first-class complex number: native-only; polymorphic `+ - * /` auto-promote `num` to `re + 0i`).

User-space pointers on x86-64 fit in 48 bits, so we lose nothing. The C side of the runtime is [include/runtime.h](include/runtime.h) + [src/runtime_x64.c](src/runtime_x64.c); the codegen emits MS x64 ABI calls into it for any operation that can't stay on the numeric fast path.

### Currently supported in native mode

- **All numeric features** from the previous cut: arithmetic (`+ - * / %`, unary `-`), comparisons returning 0/1, short-circuit `&& || !`, `let`/assign, nested scopes, `if`/`else`/`while`/`for` + `break`/`continue`, named functions with parameters/return/recursion/mutual recursion, math intrinsics (`sqrt abs floor ceil round pow min max pi e`).
- **Strings**: literals (any bytes, with the lexer's escape decoding); `print` of strings; `+` coercing num↔str like the VM (`"x = " + 5` -> `"x = 5"`); reassignment that changes a variable's type works (the flow-sensitive inference still steers the fast vs. slow path).
- **String calclib**: `len(str)`, `to_str(x)`, `to_num(s)`, `type_of(x)`, `str_at`, `str_slice`, `str_find`, `str_upper`, `str_lower`, `str_trim`, `str_repeat`, `str_starts_with`, `str_ends_with`. Each is implemented as a C function in the runtime and called via MS x64 ABI.
- **Arrays**: literals (`[1, "hi", 3.14]`), indexing reads (`a[i]`) and writes (`a[i] = v`), compound writes (`a[i] += v`), chained writes (`grid[r][c] = v`), `len(arr)`, reference semantics (aliasing shares storage), recursive `print` with nested-string quoting (`["a", 1]` prints `["a", 1]` — quoted-when-nested matches the VM).
- **Array calclib**: `push`, `pop`, `array_reverse`, `array_sort` (homogeneous all-num or all-str), `array_concat`, `array_slice`, `array_find`, `array_contains`, `array_range`.
- **Maps**: literals (`{"name": "Alice", "age": 30}`), indexing reads (`m["k"]`) and writes (`m["k"] = v`), numeric keys (`{1: "one", 2: "two"}`), nested chains (`users["alice"]["age"] = 31`), `len(map)`, reference semantics, insertion-ordered iteration, recursive `print` formatting (`{"name": "Alice", "age": 30}`).
- **Map calclib**: `keys`, `values`, `has_key`, `del`.
- **All 52 calclib builtins** plus **10 new native-only builtins**: math (`sqrt`/`floor`/`ceil`/`abs`/`int`/`round`/`pow`/`min`/`max`), trig (`sin`/`cos`/`tan`/`asin`/`acos`/`atan`/`atan2`), transcendental (`exp`/`log`/`log10`), constants (`pi`/`e`), `random`, full strings library (`str_*` including `str_split`/`str_join`), full arrays library (`push`/`pop`/`array_*`), full maps library (`keys`/`values`/`has_key`/`del`), I/O (`read_line`/`write`), `to_str`/`to_num`/`len`/`type_of`. **Native-only**: `file_read`/`file_write`/`file_append`/`file_exists`, `system(cmd)`, `complex`/`real`/`imag`/`conj`/`arg` (`abs` is overloaded for complex magnitude).
- **First-class references for any builtin** (`let f = sqrt; f(2)`, `map(xs, sqrt)`): each unique bare-name use emits a one-shot trampoline that adapts the cdecl calc convention to the runtime's MS x64 ABI, wraps the trampoline in a `CalcClosure` with 0 upvals, and lets the existing indirect-call path dispatch it.
- **Polymorphic comparisons + truthiness**: `<`/`<=`/`>`/`>=`/`==`/`!=` and `if`/`while`/`for`/`!`/`&&`/`||` all dispatch through runtime helpers when the inference can't prove both operands `num`. Lexicographic string ordering, "empty string is falsy", and "two arrays are equal iff same reference" all match the VM exactly.
- **Closures, classes, first-class user-fn refs, indirect calls**:
  - Anonymous fn expressions (`fn(y) { return x + y; }`) compile to a heap `CalcClosure` populated with captured upvalues at the construction site. Each captured outer local lives in a heap-allocated 1-Value box, so mutation through any closure (or through the original local) is visible everywhere — same semantics as the VM.
  - Nested named fns (`fn render(name) { ... }` inside another fn) desugar to inline closures and use the same machinery. Classes — themselves parser-level sugar over maps + closures — fall out for free: `p.dist()` is `cl_index_get(p, "dist")` followed by an indirect call.
  - Bare user-fn names referenced as values (`let f = add; f(2)`, `apply(my_fn)`) auto-wrap the code address in a Closure with 0 upvals.
  - Indirect calls (closure value as callee, `obj.method(args)`, `arr[i](args)`) extract the code pointer from the closure, load the closure pointer into `R10` for the callee's upval access, and `call` indirectly.
  - **Transitive capture** works: when a 3-deep `outer → middle → inner` closure references an outermost local, each intermediate closure auto-captures the box and forwards it.
- **Polymorphic `+`, `==`, `!=`, `print`**: when the flow-sensitive inference can't prove both operands are `num`, codegen emits a `cl_op_*` / `cl_print` runtime call that dispatches on tag. Pure-numeric code never touches the runtime. Equality on tagged values has to go through the runtime because hardware `ucomisd` is "unordered" on NaNs.

Calling convention details:
- Between CalcLang functions: stack-based, cdecl-style. Args are pushed right-to-left into 16-byte slots (only the low 8 bytes used) — the wider slot keeps `rsp` 16-byte aligned across nested calls inside expression operands. Inside the callee, arg `i` lives at `[rbp + 16 + i*16]`. Caller cleans up.
- Into the runtime / libc: Microsoft x64 ABI with 32-byte shadow space and 16-byte `rsp` alignment at the call site. The first Value arg goes in `RCX` (as an integer, since it's a 64-bit bit pattern that may or may not be a double), with additional args in `RDX`, `R8`. The runtime returns Values in `RAX`; codegen does `movq xmm0, rax` to drop the result back into the CalcLang register.

Memory: **mark-and-sweep GC** with conservative stack scanning. Every heap allocation (strings, arrays, maps, closures, upval boxes, and the internal items / keys / values / upvals buffers) goes through `cl_alloc`, which prepends a `GcHdr` and links into a global allocation list. When `g_bytes_since_gc` crosses a 256 KB threshold, `cl_alloc` runs a stop-the-world collection:

1. **Mark phase** — walk every 8-byte word on the native stack from the current `rsp` up to the OS-provided stack base (`TEB.StackBase` via `gs:[8]` on Windows; `pthread_attr_getstack` on Linux). Each word is probed as either a raw heap pointer (top 16 bits == 0) or a NaN-boxed tagged Value (top 16 bits in our tag range, low 48 bits = payload pointer). Matches against the allocation list mark the object and recursively descend its children — arrays mark their `items` buffer + each Value inside; maps mark `keys`/`values` buffers + entries; closures mark their `upvals` buffer + each box; boxes recurse into the Value they hold.
2. **Sweep phase** — walk the allocation list, `free` everything still unmarked, clear the marked bit on survivors.

The scan is **conservative**: any 8-byte stack word that looks like a payload pointer is treated as live, so we may retain a few false positives but never collect a live object. That trade is right for an educational runtime. To root values held in registers across a GC trigger, `cl_alloc` spills `r8`, `r9`, `xmm0`, `xmm1` to its own stack frame via inline asm at function entry, where the conservative scan picks them up.

**Build with -O0** (the default gcc setting used by the test runner and the `make native` target). At -O0 the compiler keeps every C local and parameter on the stack, so the conservative scan can see them. Higher optimization levels may keep Value arguments only in registers across calls, which could let live values get swept — annotate with `volatile` if you need an optimized build.

The test suite includes `tests/gc_stress.calc`, which runs 50000 iterations each allocating a string + array + map + closure box updates, forcing many collections, and verifies that retained state (an accumulating closure cell + a survivor map + a top-level string variable) survives every cycle byte-identical to the VM.

### FFI — dynamic linking + first-class C calls

`ffi_load(path)` loads a `.dll`/`.so`/`.dylib`; `ffi_call(lib, name, sig, args)` resolves a symbol and invokes it. The signature string is small: `v`/`i`/`d`/`s` for return + arg types (void, int, double, C string), up to 4 args. Wrap in a closure and you get a first-class function value passable anywhere.

```calc
let lib = ffi_load("msvcrt.dll");
print ffi_call(lib, "sqrt", "d:d", [16]);            // 4
let csqrt = fn(x) { return ffi_call(lib, "sqrt", "d:d", [x]); };
print map([1, 4, 9], csqrt);                          // [1, 2, 3]
```

Implementation: cross-platform — `LoadLibraryA` / `GetProcAddress` on Windows, `dlopen` / `dlsym` on Linux. The runtime has hand-rolled dispatch for the common signatures; libffi isn't required. Exotic shapes (pointer-to-array, struct-by-value, variadic) need libffi or a custom thunk and aren't in this round. Native-only.

See [docs/book.md](docs/book.md) for examples.

### Exceptions

`throw expr;` and `try { ... } catch (name) { ... }`. The thrown value can be any Value type — string, number, map, complex — and the catch binds it to a fresh local. Uncaught exceptions print a diagnostic and exit with code 1.

Implementation: a handler stack stores `(rsp, rbp, catch_pc)` at each `try`. `cl_throw` (the runtime helper) pops the topmost handler, restores `rsp`/`rbp` via inline assembly, then jumps to the catch label — non-local control transfer that unwinds whatever stack frames sit between the throw and the matching catch. The unwound frames' locals become unreachable; the GC reclaims them next cycle.

Native-only this round (the VM doesn't have exceptions yet). See [docs/book.md](docs/book.md) for examples.

### Tail-call optimization

`return foo(args);` inside `fn foo(...)` is compiled as a jump back to the function's body-start label after the new arg values are written into the parameter slots — turning self-tail-recursion into a loop. A million-deep `count_down` runs in flat stack space; without TCO the same code overflows the native stack.

Scope: direct self-recursion of named top-level functions. Mutual tail calls (`return bar(args)` from inside `foo`) and indirect tail calls (`return f(args)` where `f` is a value) still go through the normal call/return path. TCO is also suppressed when any parameter is captured by an inner closure, since the box would need to be reseated per "iteration" to preserve closure-per-call semantics.

### Phase-1 engineering additions (file I/O, system, complex)

For numerical/engineering work the native runtime ships:

- **File I/O**: `file_read(path)`, `file_write(path, content)`, `file_append(path, content)`, `file_exists(path)`.
- **`system(cmd)`**: run a shell command, return its exit code. Useful for invoking external plotters, image converters, or build helpers.
- **First-class complex numbers**: `complex(re, im)` produces a `cpx` value; `+ - * /` are polymorphic between `num` and `cpx`; `real`, `imag`, `conj`, `arg`, and an overloaded `abs` (magnitude). Equality compares by components; ordering (`<`, `>`, etc.) on `cpx` raises a runtime error.

See [docs/book.md §9.5](docs/book.md) for examples. These five additions land in the native pipeline only — the VM stays as-is for this round.

### Multi-file native builds

`extern fn` + `pub fn` work in native mode too. Compile each unit with `--lib` (suppresses `main` and top-level statements), then pass all the assembly files to the entry-point invocation:

```bash
build/calcnat --lib tests/multi/lib.calc -o build/multi_lib.s
build/calcnat tests/multi/main.calc build/multi_lib.s -o build/multi.exe
./build/multi.exe
```

The entry-point unit owns `main`; the library unit contributes only its function bodies. The system linker resolves cross-file references against the `pub fn` symbols.

The test suite compiles **every** single-file CalcLang test (including a GC stress test) through *both* the VM and native pipelines and asserts byte-identical output — numbers, strings, arrays, maps, closures + nested fns + higher-order, classes, first-class user-fn refs, first-class builtin refs, all 52 calclib builtins, transitive capture, control flow, multi-file linking, GC pressure, the lot. The native backend is meant to be a drop-in, not a "close enough" approximation.

## Current Limitations

The language stays small on purpose. It still does not include:

- **structs-by-value / variadic / pointer-to-array via FFI**: `ffi_load` + `ffi_call` work for up to-4-arg signatures with double/int/string types. Wider C-interop shapes (struct args, varargs, output pointer arrays) need libffi or per-shape thunks and aren't in this round.
- **first-class function values in the native backend**: `let f = sqrt; f(x)` works in the VM but not yet in `calcnat`.
- **garbage collection in the native runtime**: first cut is `malloc`-and-leak. Real GC (refcount or tracing) is queued after collections land.
- **path-sensitive narrowing** via `type_of(x) == "num"` guards: the flow-sensitive inference tracks types through assignments and branches, but it does not consume a type-of comparison inside an `if` as a refinement on the then-branch. Path-sensitivity would require special-casing `type_of` calls in the analyzer.
- **function specialization at call sites**: each function is analyzed once with its declared parameter types. Calls don't specialize per concrete arg type — a `fn id(x) { return x; }` doesn't infer a per-call return type from `id(5)` vs `id("hi")`.

Recursion *does* work in both backends.

Closures *do* support mutation and transitive capture in the VM backend — captures are heap-boxed and an inner function can read or write a local declared any number of scopes outward (see [tests/the_rest.calc](tests/the_rest.calc)).

No remaining major native chunks. Possible follow-ups:

- **Precise GC** — the current implementation is conservative (a stack word that happens to look like a heap pointer can keep a dead object alive). A precise GC would need the codegen to emit per-frame root descriptors and would be a substantial codegen rewrite. The conservative implementation is correct (never collects a live object) across the entire test suite, including the dedicated GC stress test, so the trade-off feels right for the project.
- **Path-sensitive narrowing** + **function specialization** — type inference improvements that would shave runtime checks in some patterns. See "Current Limitations" below.
- **Wider FFI** — `ffi_call` covers up to-4-arg signatures with num / int / string types. Struct-by-value, variadic functions, and pointer-to-array marshalling need libffi or per-shape thunks.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for a guided tour of the codebase and how to add features. Issues and pull requests welcome.

A condensed changelog of every shipped milestone lives in [CHANGELOG.md](CHANGELOG.md).

## License

MIT — see [LICENSE](LICENSE).
