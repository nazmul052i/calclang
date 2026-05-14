# The CalcLang Programming Language

*A tutorial introduction to CalcLang, in the style of K&R's "The C Programming Language."*

This book teaches CalcLang from scratch. CalcLang is a small dynamically-typed programming language that compiles to either a custom bytecode (runs on `calcvm`) or to a real native executable (`.exe` on Windows, ELF on Linux) via a tiny x86-64 backend. The book starts with hello-world and works up to closures, classes, and multi-file programs.

If you've written C, the syntax will feel familiar — semicolons, curly braces, `let`/`fn`/`if`/`while`/`for`. The semantics are closer to a scripting language: types are dynamic, strings and arrays are reference-counted-without-the-counts (just GC'd), and functions are first-class values.

The compiler is named `calcc` (bytecode) or `calcnat` (native). The bytecode VM is `calcvm`. Through this book you'll mostly use `calcnat` — it's the simpler workflow.

---

## Chapter 0 — Getting started

### Build the toolchain

You'll need `gcc` (MinGW on Windows or system gcc on Linux/macOS). From the repository root:

```bash
make
```

That produces `build/calcc`, `build/calcasm`, `build/calcld`, `build/calcvm`, and `build/calcnat`.

If you don't have `make`, the manual command is in the project README.

### Hello, world

Create a file `hello.calc`:

```calc
print "hello, world";
```

Compile and run:

```bash
build/calcnat hello.calc
./hello.exe        # Windows; on Linux, ./hello
```

That's it. `calcnat` takes one `.calc` file and produces an executable. By default the output filename is the input minus `.calc` plus `.exe`. To pick a different name, use `-o`:

```bash
build/calcnat hello.calc -o greeter.exe
```

### Two pipelines

CalcLang has two backends. Both come from the same parser/AST:

```
hello.calc ─┬─► .casm ─► .co ─► .cexe ─► calcvm        (bytecode VM)
            └─► .s ──────────── gcc ──► hello.exe       (native)
```

The native path runs faster (no interpreter) and produces real OS executables. The VM path is the original implementation; it's still maintained, byte-identical in output, and is what the test runner uses to verify the native backend.

This book uses the native path throughout. For the VM workflow, see the project README.

---

## Chapter 1 — Numbers and variables

### Arithmetic

CalcLang has the operators you expect:

```calc
print 2 + 3;       // 5
print 10 - 4;      // 6
print 6 * 7;       // 42
print 20 / 4;      // 5
print 17 % 5;      // 2  (modulo, like C's % but works on doubles)
print -7;          // -7
print (2 + 3) * 4; // 20
```

Numbers are 64-bit IEEE-754 doubles. There is no separate integer type — integers are just doubles that happen to have no fractional part. `print` uses `%.10g` formatting, so small integers print without a decimal.

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

Trying to assign to an undeclared name is a compile error:

```calc
y = 5;             // error: y is not in scope
```

### Compound assignment and increment

The compound forms work like in C:

```calc
let n = 10;
n += 5;            // n = 15
n -= 3;            // n = 12
n *= 2;            // n = 24
n /= 4;            // n = 6
n %= 4;            // n = 2
n++;               // n = 3
n--;               // n = 2
```

These desugar to plain assignment: `n += 5` is exactly `n = n + 5`.

### Comparison and logical operators

```calc
print 5 < 6;       // 1   (true is 1, false is 0)
print 5 == 5;      // 1
print 5 != 6;      // 1
print 5 >= 5;      // 1

print 1 && 1;      // 1   (short-circuit AND)
print 1 || 0;      // 1   (short-circuit OR)
print !0;          // 1   (logical NOT)
```

Boolean isn't a distinct type — true is the number 1.0 and false is 0.0. Comparison operators return numbers.

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

The condition can be any expression. Truthiness rules: `0` and the empty string `""` are falsy, everything else (any non-zero number, any non-empty string, any array, any map, any function) is truthy.

```calc
if ("yes") { print "truthy"; }   // truthy
if ("")    { print "never"; }    // never reached
```

### `while`

```calc
let i = 0;
let sum = 0;
while (i <= 10) {
    sum = sum + i;
    i = i + 1;
}
print sum;         // 55
```

### `for`

C-style `for` with init, condition, and step:

```calc
let total = 0;
for (let i = 1; i <= 10; i = i + 1) {
    total += i;
}
print total;       // 55
```

The init declaration is scoped to the loop. After the loop ends, `i` is gone.

### `break` and `continue`

```calc
// Print the first odd number greater than 100 in a sequence.
let n = 100;
while (1) {
    n = n + 1;
    if (n % 2 == 0) { continue; }
    print n;
    break;
}
// 101
```

`break` exits the nearest enclosing loop. `continue` jumps to the next iteration (the step part in a `for`).

### Block scope

Every `{ ... }` is a fresh scope:

```calc
let x = 1;
{
    let x = 999;
    print x;       // 999
}
print x;           // 1
```

The inner `let x = 999` is a fresh binding, not a reassignment.

---

## Chapter 3 — Functions

### Definition and call

```calc
fn square(n) {
    return n * n;
}
print square(7);   // 49
```

`fn name(p1, p2, ...) { body }` defines a function. Bare `return;` returns 0; falling off the end implicitly returns 0.

Recursion works as you'd expect:

```calc
fn fact(n) {
    if (n <= 1) { return 1; }
    return n * fact(n - 1);
}
print fact(10);    // 3628800
```

So does mutual recursion:

```calc
fn is_even(n) {
    if (n == 0) { return 1; }
    return is_odd(n - 1);
}
fn is_odd(n) {
    if (n == 0) { return 0; }
    return is_even(n - 1);
}
print is_even(13); // 0
```

### Optional type annotations

You can annotate parameters and return types:

```calc
fn add(a: num, b: num): num {
    return a + b;
}
```

Types are `num`, `str`, `arr`, `map`, `fn`, `bool` (alias for `num`), or `any`. Annotations are enforced — passing a string where `num` is expected is a compile-time error if the compiler can tell, otherwise a runtime error.

```calc
fn need_num(n: num): num { return n + 1; }
need_num("hello");   // compile error: argument 1: expected num, got str
```

You don't have to annotate. Bare `fn foo(x, y) { ... }` works fine and accepts anything.

### Visibility

Top-level functions are private (file-local) by default. Mark them `pub` to export across compilation units — see [Chapter 8 — Multi-file programs](#chapter-8--multi-file-programs).

```calc
pub fn area(w, h) { return w * h; }
priv fn helper(x) { return x * 2; }   // explicit private (same as bare fn)
```

---

## Chapter 4 — Strings

Strings are double-quoted, with the usual escapes:

```calc
print "hello, world";
print "with \"quotes\"";
print "tab\there";          // tab + a real tab character
print "two\nlines";          // two lines (\n is a newline)
```

### Concatenation

`+` concatenates when either operand is a string. Numbers are coerced via `%.10g`:

```calc
print "hello" + " " + "world";   // hello world
print "x = " + 5;                // x = 5
print 7 + " items";              // 7 items
print "pi = " + 3.14159;         // pi = 3.14159
```

### String library

The most useful string functions:

```calc
print len("calclang");           // 8
print str_upper("hello");        // HELLO
print str_lower("HELLO");        // hello
print str_at("abcde", 2);        // c
print str_slice("hello", 1, 4);  // ell
print str_find("foobar", "bar"); // 3
print str_starts_with("foobar", "foo"); // 1
print str_ends_with("foobar", "bar");   // 1
print str_repeat("ab", 3);       // ababab
print str_trim("   hi   ");      // hi
print str_split("a,b,c", ",");   // ["a", "b", "c"]
print str_join(["a", "b"], "-"); // a-b
print to_str(3.14);              // 3.14
print to_num("42") + 1;          // 43
```

Strings compare lexicographically:

```calc
if ("apple" < "banana") { print "yes"; }   // yes
```

Equality on strings is value-based (`"foo" == "foo"` is true even for two separately-constructed strings).

---

## Chapter 5 — Arrays and maps

### Arrays

```calc
let a = [1, 2, 3];
print a;              // [1, 2, 3]
print len(a);         // 3
print a[0];           // 1

a[1] = 20;
print a;              // [1, 20, 3]
```

Arrays are heterogeneous and have reference semantics — passing one to a function or assigning to another variable doesn't copy:

```calc
let a = [1, 2, 3];
let b = a;
b[0] = 99;
print a[0];           // 99 (same array)
```

### Array library

```calc
let xs = [];
push(xs, 10);
push(xs, 20);
push(xs, 30);
print xs;             // [10, 20, 30]
print pop(xs);        // 30
print xs;             // [10, 20]

print array_reverse([1, 2, 3]);            // [3, 2, 1]
print array_sort([3, 1, 2]);               // [1, 2, 3]
print array_concat([1, 2], [3, 4]);        // [1, 2, 3, 4]
print array_slice([10, 20, 30, 40], 1, 3); // [20, 30]
print array_find([10, 20, 30], 20);        // 1
print array_contains([10, 20, 30], 99);    // 0
print array_range(0, 5);                   // [0, 1, 2, 3, 4]
```

### Maps

Maps are key/value tables. Keys can be strings or numbers:

```calc
let person = {"name": "Alice", "age": 30};
print person["name"];         // Alice
person["age"] = 31;
print person["age"];          // 31

// Numeric keys work too.
let words = {1: "one", 2: "two", 3: "three"};
print words[2];               // two
```

Maps print as `{"key": value, ...}` with strings quoted when nested:

```calc
print {"name": "Bob", "scores": [90, 85]};
// {"name": "Bob", "scores": [90, 85]}
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

### Map library

```calc
let scores = {"alice": 90, "bob": 75};
print keys(scores);              // ["alice", "bob"]
print values(scores);            // [90, 75]
print has_key(scores, "alice");  // 1
print has_key(scores, "carol");  // 0
print del(scores, "alice");      // 1  (was present)
print scores;                    // {"bob": 75}
```

---

## Chapter 6 — Closures and first-class functions

Functions are values. You can store them in variables, pass them as arguments, and return them from other functions.

### Functions as values

```calc
fn add(a, b) { return a + b; }

let f = add;          // f now holds the function
print f(2, 3);        // 5
```

### Anonymous functions

```calc
let sq = fn(x) { return x * x; };
print sq(5);          // 25
```

### Higher-order functions

```calc
fn map(arr, f) {
    let out = [];
    for (let i = 0; i < len(arr); i = i + 1) {
        push(out, f(arr[i]));
    }
    return out;
}

print map([1, 2, 3, 4], fn(x) { return x * x; });  // [1, 4, 9, 16]
print map([-3, -1, 2], abs);                       // [3, 1, 2] (calclib fn as value)
```

`abs`, `sqrt`, and every other calclib builtin can be referenced by bare name and passed around like any other function.

### Closures

A function defined inside another function can read AND write its enclosing locals — those locals are captured "by reference":

```calc
fn make_counter() {
    let n = 0;
    fn inc() {
        n = n + 1;
        return n;
    }
    return inc;
}

let c = make_counter();
print c();            // 1
print c();            // 2
print c();            // 3
```

Each call to `make_counter` creates a fresh `n` and an inc-closure that captures it. The returned closure stays alive as long as something holds a reference to it; the GC handles cleanup.

Closures can capture multiple levels deep:

```calc
fn outer(x) {
    fn middle() {
        fn inner() {
            return x;     // captured from outer, two levels up
        }
        return inner;
    }
    return middle;
}
let m = outer(42);
let i = m();
print i();            // 42
```

### Nested named fns

Inside a function body, `fn name(params) { ... }` is shorthand for `let name = fn(params) { ... };` — same closure machinery, named binding:

```calc
fn make_pair_fn(prefix) {
    fn render(name) {
        return prefix + ": " + name;
    }
    return render;
}
let info = make_pair_fn("INFO");
print info("startup");   // INFO: startup
```

---

## Chapter 7 — Classes

Classes are syntax over maps + closures.

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

`class Name { ... }` declares a constructor. Inside, `fn init(...)` is the constructor; other `fn`s are methods. Inside any method, `this` refers to the receiver.

### Using a class

```calc
let p = Point(3, 4);
print p.x;            // 3
print p.dist();       // 5
p.move(7, 0);
print p.dist();       // 10.77032961
```

Calling `Point(...)` runs the constructor and returns the instance. Method calls (`p.dist()`) and field access (`p.x`) use the same dot syntax as maps — because instances ARE maps with closures attached.

### Methods are first-class

You can extract a bound method and call it later:

```calc
let p = Point(3, 4);
let bound = p.dist;
print bound();        // 5

fn apply(f) { return f(); }
print apply(p.dist);  // 5
```

### Public and private

`pub class Foo { ... }` exports the constructor for cross-file use. Default is private (file-local).

---

## Chapter 8 — Multi-file programs

For larger programs, split code across files. Use `pub fn` to export and `extern fn` to import:

**lib.calc:**

```calc
pub fn add(a, b) {
    return a + b;
}

pub fn greet(name) {
    return "hello, " + name;
}

// Bare `fn` is private — not visible to other files.
fn _internal_helper(x) {
    return x * 2;
}
```

**main.calc:**

```calc
extern fn add(a: num, b: num): num;
extern fn greet(name: str): str;

print add(2, 3);
print greet("world");
```

Build them together:

```bash
# Compile the library file as a "library" (no `main`, no top-level code):
build/calcnat --lib lib.calc -o lib.s

# Compile the entry-point file and link with the library object:
build/calcnat main.calc lib.s -o app.exe

./app.exe
# 5
# hello, world
```

`extern fn` declarations are implicitly public — they reach across the link boundary. `pub fn` symbols in the library are exported; bare `fn` definitions stay file-local even though they're physically in the same compilation unit.

The same source files work with the VM toolchain too — see the project README for the `calcc`/`calcasm`/`calcld` invocation.

---

## Chapter 9 — Flow-sensitive types

CalcLang infers types as the compiler walks the program. Each *use* of a variable is typed at the program point it occurs, not just at declaration. This catches errors that simpler "last write wins" inference would miss.

```calc
let x = 5;                    // x: num
print x + 10;                 // x is num here  — fast numeric path
x = "hello";                  // x: str now
print x + " world";           // x is str here  — concat path

x = 42;
print need_num(x);            // x narrowed back to num — call-site check passes
```

Each branch of an `if` is analyzed separately and the types are unioned at the join. Loops iterate to a fixpoint over the body.

The analysis is conservative when it can't be sure — values typed `any` skip the static check and rely on a runtime guard if the callee declared a specific type.

---

## Chapter 9.5 — File I/O, shell, and complex numbers

These three additions exist mainly to make CalcLang useful for engineering work — generating plots, talking to external tools, doing complex arithmetic. **Native-only**: the VM doesn't support these yet.

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
- TCO ([previous section](#tail-call-optimization)) and exceptions interact correctly: a tail-recursive function that throws is unwound the same way as any other.

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

### Why this matters for engineering work

With these three additions, you can:

- Write a `linalg.calc` library: matrices as arrays-of-arrays, operations defined on top.
- Write a `plot.calc` library: stream SVG to a file as you generate it, or write CSV and shell out to gnuplot.
- Do signal-processing math directly: complex arithmetic without manually packing `{re, im}` pairs.

The next phases of the language (tail-call optimization, exceptions, dynamic linking / FFI) extend what you can express; these three are about *interoperating* with the outside world.

---

## Chapter 10 — calclib reference

Every builtin works the same way: it has a bare name in CalcLang source, and it can be called directly (`sqrt(x)`) or stored as a value (`let f = sqrt; f(x)`).

### Math (17)

`sqrt(x)`, `floor(x)`, `ceil(x)`, `abs(x)`, `pow(b, e)`, `min(a, b)`, `max(a, b)`, `int(x)`, `round(x)`, `sin(x)`, `cos(x)`, `tan(x)`, `asin(x)`, `acos(x)`, `atan(x)`, `atan2(y, x)`, `exp(x)`, `log(x)`, `log10(x)`, `random()`, `pi()`, `e()`.

### Strings (13)

`len(s)`, `str_at(s, i)`, `str_slice(s, a, b)`, `str_find(s, sub)`, `str_upper(s)`, `str_lower(s)`, `str_trim(s)`, `str_repeat(s, n)`, `str_starts_with(s, p)`, `str_ends_with(s, p)`, `str_split(s, sep)`, `str_join(arr, sep)`, `to_str(x)`, `to_num(s)`.

### Arrays (9)

`len(arr)`, `push(arr, v)`, `pop(arr)`, `array_reverse(arr)`, `array_sort(arr)`, `array_concat(a, b)`, `array_slice(arr, a, b)`, `array_find(arr, v)`, `array_contains(arr, v)`, `array_range(lo, hi)`.

### Maps (5)

`len(map)`, `keys(m)`, `values(m)`, `has_key(m, k)`, `del(m, k)`.

### I/O and introspection (3)

`read_line()` — read a line from stdin (returns "" at EOF).
`write(x)` — print without newline.
`type_of(x)` — returns one of `"num"`, `"str"`, `"arr"`, `"map"`, `"fn"`, `"cpx"`.

### File I/O and shell (5, native-only)

`file_read(path)` — read whole file as a string.
`file_write(path, content)` / `file_append(path, content)` — write or append; return 0.
`file_exists(path)` — 1 if openable, 0 if not.
`system(cmd)` — run a shell command; return its exit code.

### Complex numbers (5, native-only)

`complex(re, im)` — construct a `cpx` value.
`real(v)` / `imag(v)` — components (accept num too).
`conj(v)` — complex conjugate.
`arg(v)` — phase angle in radians.
`abs(v)` — magnitude (overloaded; falls back to `|x|` for nums).

---

## Chapter 10.5 — Engineering library

Three CalcLang modules under `lib/` give you the basics for numerical and visualization work, written entirely on top of the language features we've built up so far.

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

#### Building the NR libraries

```bash
make nr_libs          # compile lib/nr/*.calc -> build/calclib/nr_*.s
make nr_demo          # tier 1: Brent + spline + special + Jacobi
make nr_demo2         # tier 2: LU + Romberg + RK45 + polynomial families
make nr_demo3         # tier 3: minimization + sort/select
make nr_demo4         # tier 4: Ridders' + distributions + Newton + LM fit
make nr_demo5         # tier 5: QR + SVD + Laguerre polyroots + convolution
```

### Building demos

The `lib/` modules compile once as libraries; demos link against them:

```bash
make libs                                # compiles all lib/*.calc -> build/calclib/*.s
make sine_plot                           # build + run examples/sine_plot.calc
make regression                          # build + run examples/regression.calc
make linsys                              # build + run examples/linsys.calc
make numerical                           # build + run examples/numerical.calc
make monte_carlo                         # build + run examples/monte_carlo.calc
make csv_demo                            # build + run examples/csv_demo.calc
make multi_plot                          # multi-series + bar + log-Y + axis labels
make json_demo                           # JSON parse/encode round-trip
make ode_demo                            # exp-decay + harmonic oscillator via RK4
make fft_demo                            # FFT spectrum of a synthetic signal
make nr_demo                             # Brent + spline + special fns + Jacobi
make nr_demo2                            # LU + Romberg + RK45 + polynomial families
make nr_demo3                            # 1-D / N-D minimization + sort/select
make nr_demo4                            # Ridders' + distributions + Newton + LM fit
make nr_demo5                            # QR + SVD + Laguerre polyroots + convolution
make demos                               # all of the above
```

Or by hand:

```bash
build/calcnat --lib lib/plot.calc  -o build/calclib/plot.s
build/calcnat examples/sine_plot.calc build/calclib/plot.s -o build/sine_plot.exe
build/sine_plot.exe                       # writes build/sine.svg
```

### Worked example: linear regression with plot

```calc
extern fn mean(xs: arr): num;
extern fn stddev(xs: arr): num;
extern fn linreg(xs: arr, ys: arr): map;
extern fn plot_data_with_fit(filename: str, xs: arr, ys_data: arr,
                             ys_fit: arr, title: str);

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
2. Library files compiled with `--lib` contribute only their function bodies. The entry-point unit (compiled without `--lib`) owns `main`. Public functions in libraries are `pub fn`; bare `fn` stays file-local.

These two rules together give you a clean module system: one unit per file, public surface via `pub fn`, private helpers as bare `fn`.

---

## Chapter 11 — Putting it together

A small program that uses most of the language:

```calc
// stats.calc — compute mean, median, mode of a list of numbers.

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
        let sorted = array_sort(array_concat(this.xs, []));   // sort a copy
        let n = len(sorted);
        if (n % 2 == 1) {
            return sorted[(n - 1) / 2];
        }
        return (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
    }

    fn mode() {
        let counts = {};
        for (let i = 0; i < len(this.xs); i = i + 1) {
            let v = this.xs[i];
            let k = to_str(v);
            if (has_key(counts, k)) {
                counts[k] = counts[k] + 1;
            } else {
                counts[k] = 1;
            }
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

---

## Chapter 12 — Compiler internals (for the curious)

CalcLang's whole pipeline is short and worth reading:

```
lexer.c           tokenize
parser.c          tokens -> AST
type_infer.c      annotate NODE_VAR with flow-sensitive types
codegen.c         AST -> Calc bytecode assembly (.casm)
calcasm.c         .casm -> object file (.co)
calcld.c          object files -> executable bytecode (.cexe)
calcvm.c          run .cexe

codegen_x64.c     AST -> x86-64 assembly (.s)
runtime_x64.c     C runtime (alloc + GC + calclib + dispatch helpers)
calcnat.c         drive the native pipeline; optionally invoke gcc
```

The native runtime uses **NaN-boxed Values**: every value is a 64-bit word; doubles use their natural encoding, and quiet-NaN bit patterns with reserved high-16-bit tags encode strings, arrays, maps, closures, and function references. Numeric programs pay zero overhead — `addsd`/`subsd`/etc. operate directly. Polymorphic operations (`+`, `==`, `<`, `print`, etc.) check the tag and dispatch to a runtime helper.

Memory is managed by a **mark-and-sweep GC** with conservative stack scanning. When `cl_alloc` notices that 256 KB has been allocated since the last collection, it walks every 8-byte word on the native stack (from the current `rsp` to the OS-provided stack base), treats each word as a potential pointer, and marks the alloc list. Then it sweeps unmarked allocations. This is *conservative* — a false positive keeps a dead object alive a little longer, but no live object is ever reclaimed.

For more on the bytecode VM, the bytecode format, or the linker, see the comments in the corresponding source files. The whole toolchain is ~6000 lines of C, designed to be readable.

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

## Appendix — Limitations to keep in mind

- Types are dynamic; an annotation is enforced but the compiler can't always tell at compile time. Don't rely on it for performance.
- The native backend doesn't support `extern` symbols pointing into shared libraries — only into other CalcLang `.s` files. No FFI to libc beyond what the runtime exposes.
- The GC is conservative; in a long-running program with stack patterns that happen to look like pointers, dead allocations may linger.
- Pattern matching, generics, namespaces, exceptions, async, and modules-as-files are NOT present. The language stays small on purpose.

That's the language. Read the test files under `tests/` for many more worked examples, or just write a program and run it.
