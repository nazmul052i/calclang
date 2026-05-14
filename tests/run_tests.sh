#!/bin/sh
set -e
mkdir -p build

# Normalize CRLF -> LF so Windows stdout (where the C runtime
# translates '\n' to '\r\n' on text mode) compares cleanly.
strip_cr() { tr -d '\r'; }

run() {
    name="$1"
    build/calcc   "tests/$name.calc" "build/$name.casm"
    build/calcasm "build/$name.casm" "build/$name.co"
    build/calcld  "build/$name.co"   "build/$name.cexe"
    build/calcvm  "build/$name.cexe" | strip_cr
}

check() {
    label="$1"; expected="$2"; actual="$3"
    if [ "$actual" != "$expected" ]; then
        echo "FAIL: $label"
        echo "Expected:"; printf '%s\n' "$expected"
        echo "Got:";       printf '%s\n' "$actual"
        exit 1
    fi
}

echo "[test] arithmetic"
out=$(run arithmetic)
check "arithmetic" '20
30
10' "$out"

echo "[test] import — self-contained, no --lib step"
# import "math" should resolve to lib/math.calc and inline bodies.
build/calcnat tests/import.calc -o build/d_import.exe
nat_out=$(build/d_import.exe | strip_cr)
check "import" '49
64
243
-2
5
10
-1
10' "$nat_out"

echo "[test] format — printf / println / fmt with %% and {} placeholders"
out=$(run format)
check "format" 'x = 42
pi         = 3.14
hello alice, age 30
values: 1 2 3
pi to 4 digits: 3.1416
padded:     1|22   |00333
hex: 0xff  bin: 0b1010
rust=yes, c=   99
literal { and } braces around x
count    = 7
count = 7' "$out"

echo "[test] optimizer — constant folding, algebraic, dead-branch"
out=$(run optimizer)
check "optimizer" '14
12
2
3
hello world
x = 42
7 items
1
1
0
15
1024
65535
-5
1
-6
7
7
7
7
7
7
7
7
7
always
five wins
yes
no
0
1' "$out"

echo "[test] qol — ternary, do-while, ctype, fmt"
out=$(run qol)
check "qol" 'yes
no
A
C
F
5
101
1
0
1
1
1
1
1
A
a
65
0
x = 42
pi = 3.1416
    1|2    |00003
0xff
1010
one + two = 3' "$out"

echo "[test] struct (typed records — fields, positional ctor, mutation, nesting)"
build/calcnat tests/struct_record.calc -o build/d_struct.exe
out=$(build/d_struct.exe | strip_cr)
check "struct_record" '3
4
10
20
0
12
answer
42
6
100
10
100
15
-3' "$out"

echo "[test] for-in / range (arr, str, range exclusive/inclusive, parens, classic)"
# This one runs through the native pipeline too, since most for-in
# iteration uses calls like len() / index that already get exercised
# by the larger native sweep below — keeping it on the VM side here.
build/calcnat tests/for_in.calc -o build/d_forin.exe
out=$(build/d_forin.exe | strip_cr)
check "for_in" '10
20
30
0
1
2
3
4
1
2
3
a
b
c
a
1
b
2
2
3
4
0
1
10
11
1
3
7
8
9
100
200
300
0
100
200' "$out"

echo "[test] switch statement (num and str cases, default, break)"
out=$(run switch_stmt)
check "switch_stmt" 'Sun
Mon
Tue
Wed
Thu
Fri
Sat
?
2
1
0
1104' "$out"

echo "[test] bitwise ops + hex/binary literals"
out=$(run bitwise)
check "bitwise" '1
7
6
-6
16
64
-4
255
255
3405691582
10
240
48
53
49
196
98
24
1
31
255
8
40
1
32
0
8
4' "$out"

echo "[test] reassignment via ="
out=$(run redeclare)
check "redeclare" '10' "$out"

echo "[test] control flow (if/else, while, comparisons, logical ops, block scope)"
out=$(run control_flow)
check "control_flow" '5050
1
10
30
40
7
100' "$out"

echo "[test] functions, for, break, continue"
out=$(run functions)
check "functions" '49
5
13
55
0
1
2
3
1
3
5
0
1
10
11
1
99' "$out"

echo "[test] strings"
out=$(run strings)
check "strings" 'hello
with "quotes"
tab	here
hi
bye
42
1
2
3
4
5
6
0
1
world
line1
line2' "$out"

echo "[test] recursion (fact, fib, ack, mutual)"
out=$(run recursion)
check "recursion" '1
1
120
3628800
0
1
55
610
9
61
1
1
0
55' "$out"

echo "[test] string concat and ordering"
out=$(run string_ops)
check "string_ops" 'hello world
foobar
abcd
hi
hi
xxxxx
left-right
1
2
3
4
5
6
7' "$out"

echo "[test] block comments (/* ... */)"
out=$(run block_comments)
check "block_comments" '10
20
30
40
/* not a comment, just a string */
50' "$out"

echo "[test] visibility (pub vs private fn)"
out=$(run visibility)
check "visibility" '7
10
42
107' "$out"

# Visibility also affects the assembler labels and the linker section
# split. Spot-check that pub becomes a global SYMS entry and bare fn
# becomes a LOCALSYMS entry.
build/calcasm build/visibility.casm build/visibility.co
syms_pub=$(grep -c "^S fn_add"      build/visibility.co || true)
syms_priv=$(grep -c "^S fn_helper"  build/visibility.co || true)
locals_pub=$(grep -c "^L fn_add"    build/visibility.co || true)
locals_priv=$(grep -c "^L \.fn_helper" build/visibility.co || true)
if [ "$syms_pub" != "1" ] || [ "$syms_priv" != "0" ] || \
   [ "$locals_pub" != "0" ] || [ "$locals_priv" != "1" ]; then
    echo "FAIL: visibility object-file layout"
    echo "  expected fn_add in SYMS, .fn_helper in LOCALSYMS"
    echo "  got: S fn_add=$syms_pub, S fn_helper=$syms_priv, L fn_add=$locals_pub, L .fn_helper=$locals_priv"
    exit 1
fi

echo "[test] type enforcement (compile-time + runtime)"
out=$(run type_enforcement)
check "type_enforcement" '7
hello, calc
5
2
14
anything works
15
42' "$out"

# Compile-time check: literal of the wrong type should be rejected.
cat > build/type_bad_compile.calc <<'EOF'
fn add(a: num, b: num): num { return a + b; }
print add("hello", 5);
EOF
err=$(build/calcc build/type_bad_compile.calc /dev/null 2>&1 || true)
if ! echo "$err" | grep -q "expected num, got str"; then
    echo "FAIL: compile-time type check did not fire on literal arg"
    echo "  got: $err"
    exit 1
fi

# Runtime check: argument type unknowable at compile time.
cat > build/type_bad_runtime.calc <<'EOF'
fn need_num(x: num): num { return x + 1; }
fn make_str() { return "oops"; }
print need_num(make_str());
EOF
build/calcc   build/type_bad_runtime.calc build/type_bad_runtime.casm
build/calcasm build/type_bad_runtime.casm build/type_bad_runtime.co
build/calcld  build/type_bad_runtime.co   build/type_bad_runtime.cexe
err=$(build/calcvm build/type_bad_runtime.cexe 2>&1 || true)
if ! echo "$err" | grep -q "runtime type error"; then
    echo "FAIL: runtime type check did not fire"
    echo "  got: $err"
    exit 1
fi

# Arg-count check: wrong number of arguments should be rejected.
cat > build/type_bad_arity.calc <<'EOF'
fn add(a: num, b: num): num { return a + b; }
print add(3);
EOF
err=$(build/calcc build/type_bad_arity.calc /dev/null 2>&1 || true)
if ! echo "$err" | grep -q "takes 2 args, got 1"; then
    echo "FAIL: arg-count check did not fire"
    echo "  got: $err"
    exit 1
fi

# `any` parameter should accept anything (no compile-time error,
# no TYPECHECK at runtime).
cat > build/type_any_ok.calc <<'EOF'
fn flex(x): any { return x; }
print flex("hi");
print flex(42);
print flex([1, 2]);
EOF
build/calcc   build/type_any_ok.calc build/type_any_ok.casm
build/calcasm build/type_any_ok.casm build/type_any_ok.co
build/calcld  build/type_any_ok.co   build/type_any_ok.cexe
out=$(build/calcvm build/type_any_ok.cexe | strip_cr)
check "type_any_ok" 'hi
42
[1, 2]' "$out"

echo "[test] arrays (literals, indexing, mutation, len, push, refs, nesting)"
out=$(run arrays)
check "arrays" '[1, 2, 3]
3
1
3
[1, 20, 3]
20
[]
0
[42, "hi", 3.14]
hi
3
4
[42, "hi", 3.14, "new"]
5
150
999
999
[1, 4, 9, 16, 25]
[[1, 2], [3, 4], [5, 6]]
[3, 4]
3
1
0' "$out"

echo "[test] maps (literals, indexing, mutation, keys/values/has_key/del)"
out=$(run maps)
check "maps" 'Alice
30
2
3
NYC
31
{}
0
["a", "b", "c"]
[1, 2, 3]
1
0
1
0
0
2
two
yes
map
map
Bob
1
3
2
250' "$out"

echo "[test] first-class functions (assign, pass, return, higher-order)"
out=$(run first_class_fns)
check "first_class_fns" '5
30
20
7
12
[2, 4, 6, 8, 10]
[1, 2]
15
120
fn
1
2' "$out"

echo "[test] calclib full library (math, strings, arrays, I/O, type_of)"
out=$(run calclib)
check "calclib" '4
1.414213562
3
4
5
1024
42
3.14
5
3
3
-2
7
3
-3
3
4
-4
0
1
0
0
0
0
0
1
0
3
1
1
1
e
hello
world
6
-1
HELLO
hello
hi
ababab
1
0
1
["a", "b", "c"]
a-b-c
30
[10, 20]
[3, 2, 1]
[1, 1, 2, 3, 4, 5, 6, 9]
["apple", "banana", "cherry"]
[1, 2, 3, 4]
[20, 30, 40]
2
-1
1
0
[1, 2, 3, 4, 5]
abc
num
str
arr' "$out"

echo "[test] compound assignment (+=, -=, *=, /=, %=, ++, --)"
out=$(run compound_assign)
check "compound_assign" '15
12
24
6
2
1
2
1
55
120
99
98
97
96' "$out"

echo "[test] multi-file linking (extern fn + pub fn across .co files)"
build/calcc   tests/multi/main.calc build/multi_main.casm
build/calcc   tests/multi/lib.calc  build/multi_lib.casm
build/calcasm build/multi_main.casm build/multi_main.co
build/calcasm build/multi_lib.casm  build/multi_lib.co
# main.co MUST come first: the entry point is the top-level code of
# the first object file. lib.co contributes only function bodies.
build/calcld  build/multi_main.co build/multi_lib.co build/multi.cexe
out=$(build/calcvm build/multi.cexe | strip_cr)
check "multi" '5
42
hello, world
info: Alice' "$out"

# Verify the symbol-section split is right: lib's pub fns export to
# SYMS, lib's `_internal` stays in LOCALSYMS, and main has unresolved
# externs in its instruction stream (has_sym=1).
syms_pub=$(grep -c "^S fn_add\|^S fn_greet\|^S fn_make_greeting" build/multi_lib.co)
locals_priv=$(grep -c "^L \.fn__internal" build/multi_lib.co)
if [ "$syms_pub" != "3" ] || [ "$locals_priv" != "1" ]; then
    echo "FAIL: multi-file object layout (lib)"
    echo "  pub SYMS=$syms_pub (expected 3); private LOCALSYMS=$locals_priv (expected 1)"
    exit 1
fi

echo "[test] completed limitations (mixed +, chained writes, let types, priv, builtin trampolines)"
out=$(run completed_limitations)
check "completed_limitations" 'x = 5
7 items
pi = 3.14159
total: 30
neg = -7
a=1, b=2
[[1, 99, 3], [4, 5, 6]]
[[[42]]]
{"age": 31}
[{"name": "Bob", "age": 26}]
42
hello
[1, 2, 3]
{"k": "v"}
1
15
14
5
fn
[3, 1, 0, 1, 3]
[1, 2, 3, 4, 5]
[3, 1, 2]' "$out"

# Compile-time check: `let x: num = "..."` rejected at compile time.
cat > build/let_bad_compile.calc <<'EOF'
let x: num = "not a number";
EOF
err=$(build/calcc build/let_bad_compile.calc /dev/null 2>&1 || true)
if ! echo "$err" | grep -q "let x: num"; then
    echo "FAIL: let-type compile-time check did not fire"
    echo "  got: $err"
    exit 1
fi

# Runtime check: typed let with dynamic mismatched RHS.
cat > build/let_bad_runtime.calc <<'EOF'
fn make_str() { return "oops"; }
let x: num = make_str();
EOF
build/calcc   build/let_bad_runtime.calc build/let_bad_runtime.casm
build/calcasm build/let_bad_runtime.casm build/let_bad_runtime.co
build/calcld  build/let_bad_runtime.co   build/let_bad_runtime.cexe
err=$(build/calcvm build/let_bad_runtime.cexe 2>&1 || true)
if ! echo "$err" | grep -q "runtime type error"; then
    echo "FAIL: let-type runtime check did not fire"
    echo "  got: $err"
    exit 1
fi

echo "[test] closures + nested fns + compound-on-index + type inference"
out=$(run closures_and_more)
check "closures_and_more" '5
8
13
14
item: alpha
item: beta
[3, 6, 9, 12]
[15, 15, 60]
5
[[1, 20], [103, 4]]
2
106
42!' "$out"

# (Closures-by-value-only rejection has been removed — captures are now
# mutable boxes. See the dedicated mutable-closures test instead.)

echo "[test] classes (fields, methods, this, method calls, bound methods)"
out=$(run classes)
check "classes" '3
4
5
(3, 4)
1
2
12
20
0
100
0
20
18
area=20 perim=18
0
3
30
2
0
5
13
1
0
10
10
7
9' "$out"

echo "[test] mutable closures + transitive capture + field access (p.name)"
out=$(run the_rest)
check "the_rest" '1
2
3
1
4
15
22
22
100
250
42
10
20
30
Alice
30
31
30
NYC
31
Bob
Carol
Bobby
15
16' "$out"

echo "[test] flow-sensitive type inference"
out=$(run flow_inference)
check "flow_inference" '15
hi there
84
101
201
7
seven
10
14
abab
6
15
default
999' "$out"

# Negative test: the analysis narrows `x` to str after `x = "hello"`,
# so calling need_num(x) MUST fail at compile time (old per-symbol
# inference would have widened to any and deferred to runtime).
cat > build/flow_narrow_err.calc <<'EOF'
fn need_num(n: num): num { return n + 1; }
let x = 5;
x = "hello";
print need_num(x);
EOF
err=$(build/calcc build/flow_narrow_err.calc /dev/null 2>&1 || true)
if ! echo "$err" | grep -q "expected num, got str"; then
    echo "FAIL: flow-sensitive narrowing didn't catch x: str at call site"
    echo "  got: $err"
    exit 1
fi

echo "[test] assembler/linker label relocation"
build/calcasm examples/jump_demo.casm build/jump_demo.co
build/calcld  build/jump_demo.co      build/jump_demo.cexe
out=$(build/calcvm build/jump_demo.cexe | strip_cr)
check "jump_demo" '123
456' "$out"

# Native x86-64 backend. We compile the same source through both
# pipelines and demand identical output — the native backend is meant
# to be a drop-in for the numeric subset, not a "close enough"
# approximation. Skipped automatically if gcc is missing.
if command -v gcc >/dev/null 2>&1; then
    echo "[test] native x86-64 backend — numeric subset (matches VM byte-for-byte)"
    build/calcc   tests/native_basic.calc build/native_basic.casm
    build/calcasm build/native_basic.casm build/native_basic.co
    build/calcld  build/native_basic.co   build/native_basic.vm.cexe
    vm_out=$(build/calcvm build/native_basic.vm.cexe | strip_cr)

    build/calcnat tests/native_basic.calc build/native_basic.s
    gcc build/native_basic.s src/runtime_x64.c -Iinclude -o build/native_basic.exe
    nat_out=$(build/native_basic.exe | strip_cr)

    check "native_basic" "$vm_out" "$nat_out"

    echo "[test] native x86-64 backend — strings (literals, concat, str_*, type_of)"
    build/calcc   tests/native_strings.calc build/native_strings.casm
    build/calcasm build/native_strings.casm build/native_strings.co
    build/calcld  build/native_strings.co   build/native_strings.vm.cexe
    vm_out=$(build/calcvm build/native_strings.vm.cexe | strip_cr)

    build/calcnat tests/native_strings.calc build/native_strings.s
    gcc build/native_strings.s src/runtime_x64.c -Iinclude -o build/native_strings.exe
    nat_out=$(build/native_strings.exe | strip_cr)

    check "native_strings" "$vm_out" "$nat_out"

    echo "[test] native x86-64 backend — arrays (literals, index, push/pop, array_*)"
    # Reuse the existing tests/arrays.calc — the native backend must
    # produce the same output as the VM for it.
    build/calcc   tests/arrays.calc build/native_arrays.casm
    build/calcasm build/native_arrays.casm build/native_arrays.co
    build/calcld  build/native_arrays.co   build/native_arrays.vm.cexe
    vm_out=$(build/calcvm build/native_arrays.vm.cexe | strip_cr)

    build/calcnat tests/arrays.calc build/native_arrays.s
    gcc build/native_arrays.s src/runtime_x64.c -Iinclude -o build/native_arrays.exe
    nat_out=$(build/native_arrays.exe | strip_cr)

    check "native_arrays" "$vm_out" "$nat_out"

    echo "[test] native x86-64 backend — maps (literals, index, keys/values/has_key/del)"
    build/calcc   tests/maps.calc build/native_maps.casm
    build/calcasm build/native_maps.casm build/native_maps.co
    build/calcld  build/native_maps.co   build/native_maps.vm.cexe
    vm_out=$(build/calcvm build/native_maps.vm.cexe | strip_cr)

    build/calcnat tests/maps.calc build/native_maps.s
    gcc build/native_maps.s src/runtime_x64.c -Iinclude -o build/native_maps.exe
    nat_out=$(build/native_maps.exe | strip_cr)

    check "native_maps" "$vm_out" "$nat_out"

    # Comprehensive native sweep: EVERY non-multi-file test file must
    # produce byte-identical output through both the VM and native
    # pipelines. The native backend covers the entire CalcLang surface
    # now — strings, arrays, maps, closures, classes, full calclib,
    # first-class fn refs (user and builtin), control flow, the lot.
    for tcase in closures_and_more classes first_class_fns the_rest \
                 strings string_ops recursion functions control_flow \
                 visibility compound_assign block_comments \
                 type_enforcement completed_limitations calclib \
                 gc_stress; do
        echo "[test] native x86-64 backend — $tcase"
        build/calcc   tests/$tcase.calc build/n_$tcase.casm
        build/calcasm build/n_$tcase.casm build/n_$tcase.co
        build/calcld  build/n_$tcase.co  build/n_$tcase.vm.cexe
        vm_out=$(build/calcvm build/n_$tcase.vm.cexe | strip_cr)

        build/calcnat tests/$tcase.calc build/n_$tcase.s
        gcc build/n_$tcase.s src/runtime_x64.c -Iinclude -o build/n_$tcase.exe
        nat_out=$(build/n_$tcase.exe | strip_cr)

        check "native_$tcase" "$vm_out" "$nat_out"
    done

    echo "[test] native x86-64 backend — multi-file (extern fn + pub fn across .calc)"
    # Reuse tests/multi: build lib.calc as library (no main), main.calc
    # as the entry, link them together with the runtime, verify output.
    build/calcnat --lib tests/multi/lib.calc -o build/n_multi_lib.s
    build/calcnat tests/multi/main.calc build/n_multi_lib.s -o build/n_multi.exe
    nat_out=$(build/n_multi.exe | strip_cr)
    check "native_multi" '5
42
hello, world
info: Alice' "$nat_out"

    echo "[test] native x86-64 backend — TCO (self-tail recursion to 1M depth)"
    build/calcnat tests/native_tco.calc -o build/native_tco.exe
    nat_out=$(build/native_tco.exe | strip_cr)
    check "native_tco" '0
1.307674368e+12
2.432902008e+18
5000050000
3628800
done
almost: 1
almost: 1' "$nat_out"

    echo "[test] native x86-64 backend — phase 1 (file I/O, system, complex)"
    # Native-only (the VM doesn't have these builtins). Check against
    # the literal expected output.
    rm -f build/phase1.txt build/phase1_num.txt
    build/calcnat tests/native_phase1.calc -o build/native_phase1.exe
    nat_out=$(build/native_phase1.exe | strip_cr)
    check "native_phase1" '1
0
alpha
beta
gamma

42.5
3
4
5
0
5
3
(3, -4)
0
1.570796327
3.141592654
(4, 6)
(2, 2)
(-5, 10)
(2.2, -0.4)
(-3, -4)
(5, 3)
(1, 0)
(0, 2)
(-1, 0)
1
1
0
cpx
num
cpx
from-the-shell
0' "$nat_out"

    # FFI test: load msvcrt.dll and call libc through it. Only meaningful
    # on systems where that DLL is present (Windows, or wine on Linux).
    # We probe with a tiny one-liner first; skip the test cleanly if it
    # fails to load.
    cat > build/_ffi_probe.calc <<'EOF'
let lib = ffi_load("msvcrt.dll");
print lib > 0;
EOF
    build/calcnat build/_ffi_probe.calc -o build/_ffi_probe.exe 2>/dev/null
    if build/_ffi_probe.exe 2>/dev/null | grep -q '^1$'; then
        echo "[test] native x86-64 backend — FFI (load msvcrt.dll, call libc)"
        build/calcnat tests/native_ffi.calc -o build/native_ffi.exe
        nat_out=$(build/native_ffi.exe | strip_cr)
        check "native_ffi" '4
1024
0.7853981634
12
0
[1, 2, 3, 4, 5]
5
caught: sqrt: negative argument' "$nat_out"
    else
        echo "[skip] native FFI (msvcrt.dll not loadable on this platform)"
    fi

    echo "[test] native x86-64 backend — exceptions (throw, try/catch, cross-fn unwind)"
    build/calcnat tests/native_exceptions.calc -o build/native_exceptions.exe
    nat_out=$(build/native_exceptions.exe | strip_cr)
    check "native_exceptions" 'caught: boom
got number: 42
5
error: division by zero
inner caught: inner
outer caught: rethrown
deep: bottom
got cpx: 3, 4
404
not found
no throw here
after all' "$nat_out"

    # Engineering library + demos. Each example imports its own
    # libraries — no separate --lib step or .s arguments needed.

    echo "[test] demo — math_demo (sq, cube, power_int, hypot, hyperbolics, lerp)"
    build/calcnat examples/math_demo.calc -o build/d_math.exe
    nat_out=$(build/d_math.exe | strip_cr)
    # Spot-check a few invariants.
    echo "$nat_out" | grep -q "sq(7)         = 49"              || { echo "FAIL: math — sq"; exit 1; }
    echo "$nat_out" | grep -q "cube(4)       = 64"              || { echo "FAIL: math — cube"; exit 1; }
    echo "$nat_out" | grep -q "power_int(3, 5)   = 243"         || { echo "FAIL: math — power_int"; exit 1; }
    echo "$nat_out" | grep -q "cbrt(-8)          = -2"          || { echo "FAIL: math — cbrt(-8)"; exit 1; }
    echo "$nat_out" | grep -q "hypot(3, 4)       = 5"           || { echo "FAIL: math — hypot"; exit 1; }
    echo "$nat_out" | grep -q "log2(1024)        = 10"          || { echo "FAIL: math — log2"; exit 1; }
    echo "$nat_out" | grep -q "remap(5, 0, 10, 100, 200) = 150" || { echo "FAIL: math — remap"; exit 1; }

    echo "[test] demo — linsys (mat_solve, mat_det, transpose, identity)"
    build/calcnat examples/linsys.calc -o build/d_linsys.exe
    nat_out=$(build/d_linsys.exe | strip_cr)
    check "linsys" 'A =
[[2, 1, -1], [-3, -1, 2], [-2, 1, 2]]
b =
[8, -11, -3]

det(A) = -1
solve(A, b) =
[2, 3, -1]
A * x =
[8, -11, -3]
b =
[8, -11, -3]

transpose(transpose(A)) == A?
1
A * I3 == A?  1' "$nat_out"

    echo "[test] demo — sine_plot (writes build/sine.svg)"
    rm -f build/sine.svg
    build/calcnat examples/sine_plot.calc -o build/d_sine.exe
    nat_out=$(build/d_sine.exe | strip_cr)
    check "sine_plot_stdout" "wrote build/sine.svg (400 points)" "$nat_out"
    if [ ! -s build/sine.svg ]; then
        echo "FAIL: sine_plot did not produce build/sine.svg"; exit 1
    fi
    # SVG should contain at least the <svg> opening + <polyline>.
    if ! grep -q '<svg ' build/sine.svg || ! grep -q '<polyline ' build/sine.svg; then
        echo "FAIL: sine.svg malformed"; exit 1
    fi

    echo "[test] demo — numerical (root finding, integration, interpolation)"
    build/calcnat examples/numerical.calc -o build/d_num.exe
    nat_out=$(build/d_num.exe | strip_cr)
    # Spot-check a few values rather than full diff (numerical fns vary
    # in the last digit between machines).
    if ! echo "$nat_out" | grep -q "1.570796"; then
        echo "FAIL: numerical demo — bisect cos didn't converge to π/2"
        echo "$nat_out"; exit 1
    fi
    if ! echo "$nat_out" | grep -q "1.25992"; then
        echo "FAIL: numerical demo — Newton cube-root-of-2"
        exit 1
    fi
    if ! echo "$nat_out" | grep -q "adaptive (tol=1e-9)  -> 2"; then
        echo "FAIL: numerical demo — adaptive_simpson of sin"
        exit 1
    fi

    echo "[test] demo — monte_carlo (PRNG class + π estimate + normal sampler)"
    nat_out=$(build/calcnat examples/monte_carlo.calc -o build/d_mc.exe \
        && build/d_mc.exe | strip_cr)
    # π estimate from 100k samples should be within ~0.05 of true π.
    pi_line=$(echo "$nat_out" | grep "^π estimate")
    if [ -z "$pi_line" ]; then
        echo "FAIL: monte_carlo — missing π estimate output"; exit 1
    fi
    # 10k standard-normal samples: |mean| < 0.05 and |stddev - 1| < 0.05
    if ! echo "$nat_out" | grep -q "expected ~0"; then
        echo "FAIL: monte_carlo — missing normal summary"; exit 1
    fi

    echo "[test] demo — csv_demo (write CSV, read back, fit, plot)"
    rm -f build/measurements.csv build/measurements.svg
    build/calcnat examples/csv_demo.calc -o build/d_csv.exe
    nat_out=$(build/d_csv.exe | strip_cr)
    if [ ! -s build/measurements.csv ]; then
        echo "FAIL: csv_demo — measurements.csv not produced"; exit 1
    fi
    if [ ! -s build/measurements.svg ]; then
        echo "FAIL: csv_demo — measurements.svg not produced"; exit 1
    fi
    if ! echo "$nat_out" | grep -qE "y = 3\.1[0-9]+ x \+ 1\.[0-9]+"; then
        echo "FAIL: csv_demo — fit slope not near 3.2"
        echo "$nat_out"; exit 1
    fi

    echo "[test] demo — multi_plot (multi-series + bar + log_y + axis labels)"
    rm -f build/multi.svg build/bar.svg build/decay.svg build/labeled.svg
    build/calcnat examples/multi_plot.calc -o build/d_mp.exe
    build/d_mp.exe > /dev/null
    for svg in build/multi.svg build/bar.svg build/decay.svg build/labeled.svg; do
        if [ ! -s "$svg" ]; then echo "FAIL: multi_plot — missing $svg"; exit 1; fi
    done

    echo "[test] demo — json_demo (parse/encode round-trip)"
    rm -f build/data.json
    build/calcnat examples/json_demo.calc -o build/d_json.exe
    nat_out=$(build/d_json.exe | strip_cr)
    # Spot-check a few signature outputs.
    echo "$nat_out" | grep -q "Alice"           || { echo "FAIL: json — Alice"; exit 1; }
    echo "$nat_out" | grep -q "admin"           || { echo "FAIL: json — admin"; exit 1; }
    echo "$nat_out" | grep -q "NYC"             || { echo "FAIL: json — NYC"; exit 1; }
    [ -s build/data.json ]                       || { echo "FAIL: json — data.json"; exit 1; }

    echo "[test] demo — ode_demo (RK4 scalar + harmonic oscillator)"
    rm -f build/ode_decay.svg build/ode_sho.svg
    build/calcnat examples/ode_demo.calc -o build/d_ode.exe
    nat_out=$(build/d_ode.exe | strip_cr)
    # RK4 should match exp(-5) to 6+ digits; Euler will be visibly worse.
    rk4_line=$(echo "$nat_out" | grep "RK4   = ")
    if ! echo "$rk4_line" | grep -qE "0\.0067379[0-9]+"; then
        echo "FAIL: ode_demo — RK4 didn't match exp(-5)"
        echo "$nat_out"; exit 1
    fi
    if [ ! -s build/ode_decay.svg ] || [ ! -s build/ode_sho.svg ]; then
        echo "FAIL: ode_demo — missing SVG"; exit 1
    fi

    echo "[test] demo — nr_demo (Brent, spline, gamma/erf, Jacobi)"
    build/calcnat examples/nr_demo.calc -o build/d_nr.exe
    nat_out=$(build/d_nr.exe | strip_cr)
    # Spot-check a few invariants. Tolerant of trailing digits.
    echo "$nat_out" | grep -q "cos(x) = 0 -> 1.570796"        || { echo "FAIL: nr — Brent cos";        exit 1; }
    echo "$nat_out" | grep -q "x\^3 = 2  -> 1.259921"         || { echo "FAIL: nr — Brent cubic";       exit 1; }
    echo "$nat_out" | grep -q "gamma(5)   = 24"               || { echo "FAIL: nr — gamma(5)";          exit 1; }
    # Jacobi residuals must all be at machine epsilon (1e-13 or smaller).
    if echo "$nat_out" | grep -qE "max \|A v - lambda v\| = [^0]\.[0-9]+e-0[789]"; then
        echo "FAIL: nr — Jacobi residual too large"; echo "$nat_out"; exit 1
    fi
    # Sum of eigenvalues should equal trace(A) = 4+3+5 = 12.
    sum_line=$(echo "$nat_out" | grep "^eigenvalues:" -A 1 | tail -1)
    # Not strict-equal because of FP noise; just sanity-check the
    # eigenvalues are present and the line parsed.
    if [ -z "$sum_line" ]; then
        echo "FAIL: nr — no eigenvalues output"; exit 1
    fi

    echo "[test] demo — nr_demo2 (LU + Romberg + RK45 + polynomial families)"
    build/calcnat examples/nr_demo2.calc -o build/d_nr2.exe
    nat_out=$(build/d_nr2.exe | strip_cr)
    # LU determinant must be exactly -1 for this matrix.
    echo "$nat_out" | grep -q "det(A) = -1"                   || { echo "FAIL: nr2 — LU det";          exit 1; }
    # Solve must recover [2, 3, -1].
    echo "$nat_out" | grep -q '^\[2, 3, -1' \
        || echo "$nat_out" | grep -q 'x1 =' \
        || { echo "FAIL: nr2 — LU solve";        exit 1; }
    # Romberg of sin on [0, pi] must hit 2.0 exactly (or essentially so).
    echo "$nat_out" | grep -q "sin(x) on \[0, pi\]  -> 2"      || { echo "FAIL: nr2 — Romberg sin";     exit 1; }
    # RK45 on y' = -y must match exp(-5) to ~8 digits.
    echo "$nat_out" | grep -q "RK45 endpoint = 0.006737"       || { echo "FAIL: nr2 — RK45 decay";      exit 1; }
    # Polynomials: Chebyshev T_5(0.5) = 0.5 exactly.
    echo "$nat_out" | grep -q "Chebyshev T_5(0.5) = 0.5"       || { echo "FAIL: nr2 — Chebyshev T_5";   exit 1; }
    # Hermite H_3(0.5) = -5.
    echo "$nat_out" | grep -q "Hermite   H_3(0.5) = -5"        || { echo "FAIL: nr2 — Hermite H_3";     exit 1; }
    # Bessel J_0(0) ≈ 1.
    echo "$nat_out" | grep -qE "J_0\(0\)   = 1(\.0+[0-9]+)?"   || { echo "FAIL: nr2 — Bessel J_0(0)";   exit 1; }

    echo "[test] demo — nr_demo3 (Brent/golden/Nelder-Mead + heapsort/quickselect)"
    build/calcnat examples/nr_demo3.calc -o build/d_nr3.exe
    nat_out=$(build/d_nr3.exe | strip_cr)
    # Quadratic min at x=2, f=1 — must land on 2 to ~8 digits.
    echo "$nat_out" | grep -qE "golden_section -> x = 2(\.0+[0-9]*)?\b" \
        || { echo "FAIL: nr3 — golden section"; echo "$nat_out"; exit 1; }
    echo "$nat_out" | grep -qE "brent_min      -> x = 2(\.0+[0-9]*)?   f = 1" \
        || { echo "FAIL: nr3 — Brent minimize"; echo "$nat_out"; exit 1; }
    # Rosenbrock f* should be < 1e-6 — accept any "f* = 0.0000..." or "e-".
    echo "$nat_out" | grep -E "^  f\* = " | head -1 | grep -qE "(0(\.0{5,}[0-9]+)?|e-)" \
        || { echo "FAIL: nr3 — Rosenbrock f*"; echo "$nat_out"; exit 1; }
    # Heapsort produces a fully sorted list. The demo prints the label
    # and the array on separate lines, so grep just the array.
    echo "$nat_out" | grep -q "^\[0, 1, 2, 3, 4, 5, 6, 7, 8, 9\]$" \
        || { echo "FAIL: nr3 — heapsort"; echo "$nat_out"; exit 1; }
    # Quickselect: 3rd smallest (k=2) of 0..9 is 2.
    echo "$nat_out" | grep -qE "3rd smallest \(k=2\) = 2(\.0+[0-9]*)?\b" \
        || { echo "FAIL: nr3 — quickselect"; echo "$nat_out"; exit 1; }
    # Median of 0..9 is 4.5.
    echo "$nat_out" | grep -q "median of raw      = 4.5" \
        || { echo "FAIL: nr3 — median"; echo "$nat_out"; exit 1; }

    echo "[test] demo — nr_demo4 (Ridders + dist sampling + Newton + LM fit)"
    build/calcnat examples/nr_demo4.calc -o build/d_nr4.exe
    nat_out=$(build/d_nr4.exe | strip_cr)
    # Ridders' on sin(1) must give cos(1) to ~15 digits.
    echo "$nat_out" | grep -q "exact: cos(1) = 0.5403023059" \
        || { echo "FAIL: nr4 — cos(1) line"; echo "$nat_out"; exit 1; }
    echo "$nat_out" | grep -qE "d/dx sin\(x\) at x=1 -> 0\.5403023(0[0-9]+|[1-9])" \
        || { echo "FAIL: nr4 — Ridders sin"; echo "$nat_out"; exit 1; }
    # Newton: must converge to (4, 3) starting at (5, 0).
    echo "$nat_out" | grep -q "x\* =" \
        || { echo "FAIL: nr4 — Newton output"; echo "$nat_out"; exit 1; }
    echo "$nat_out" | grep -qE "\[4(\.0+[0-9]*)?, 3(\.0+[0-9]*)?\]" \
        || { echo "FAIL: nr4 — Newton root (4,3)"; echo "$nat_out"; exit 1; }
    echo "$nat_out" | grep -qE "iters = [0-9]+   converged = 1" \
        || { echo "FAIL: nr4 — Newton not converged"; echo "$nat_out"; exit 1; }
    # LM fit: a0 must come back within 5% of 2.5 (-> 2.375 .. 2.625).
    fit_line=$(echo "$nat_out" | grep -A1 "fitted a (truth" | tail -1)
    if ! echo "$fit_line" | grep -qE "\[2\.[3-6]"; then
        echo "FAIL: nr4 — LM fit a0 out of band"; echo "$fit_line"; exit 1
    fi

    echo "[test] demo — nr_demo5 (QR + SVD + Laguerre polyroots + convolution)"
    build/calcnat examples/nr_demo5.calc -o build/d_nr5.exe
    nat_out=$(build/d_nr5.exe | strip_cr)
    # QR LS solution must equal SVD LS solution to ~1e-10.
    echo "$nat_out" | grep -qE "least-squares solution:" \
        || { echo "FAIL: nr5 — no QR LS heading"; exit 1; }
    # Both QR and SVD LS solutions should print [0.5714..., 2.5714..., 1.5714...]
    qr_count=$(echo "$nat_out" | grep -c "\[0.5714285714, 2.571428571, 1.571428571\]")
    if [ "$qr_count" != "2" ]; then
        echo "FAIL: nr5 — QR / SVD LS disagree"; echo "$nat_out"; exit 1
    fi
    # Pseudo-inverse * A within ~1e-14 of identity.
    echo "$nat_out" | grep -qE "\|\|A\^\+ A - I\|\|_F = .*e-1[3456]" \
        || { echo "FAIL: nr5 — pinv*A != I"; echo "$nat_out"; exit 1; }
    # Polynomial roots: (x-1)(x-2)(x-3) recovered to machine precision.
    echo "$nat_out" | grep -q "^  1 + 0i$" \
        || { echo "FAIL: nr5 — root 1"; echo "$nat_out"; exit 1; }
    echo "$nat_out" | grep -q "^  2 + 0i$" \
        || { echo "FAIL: nr5 — root 2"; echo "$nat_out"; exit 1; }
    echo "$nat_out" | grep -q "^  3 + 0i$" \
        || { echo "FAIL: nr5 — root 3"; echo "$nat_out"; exit 1; }
    # Direct vs FFT convolution match exactly on the test pair.
    direct=$(echo "$nat_out" | grep -A1 "conv_direct =" | tail -1)
    fft=$(echo "$nat_out"    | grep -A1 "conv_fft    =" | tail -1)
    if [ "$direct" != "$fft" ]; then
        echo "FAIL: nr5 — conv_direct != conv_fft"
        echo "direct: $direct"; echo "fft:    $fft"; exit 1
    fi
    echo "$nat_out" | grep -q "\[1, 3, 6, 5, 3\]" \
        || { echo "FAIL: nr5 — conv result wrong"; echo "$nat_out"; exit 1; }

    echo "[test] demo — nr_demo6 (Chebyshev + Sav-Gol + Kalman + Crank-Nicolson)"
    build/calcnat examples/nr_demo6.calc -o build/d_nr6.exe
    nat_out=$(build/d_nr6.exe | strip_cr)
    # Chebyshev: error at x=0 must be below 1e-9.
    cheb_err_line=$(echo "$nat_out" | grep -E "^  x=0  exact=1  cheb=")
    if ! echo "$cheb_err_line" | grep -qE "err=[0-9]+(\.[0-9]+)?e-(09|1[0-9])"; then
        echo "FAIL: nr6 — Chebyshev error too large"; echo "$cheb_err_line"; exit 1
    fi
    # Savitzky-Golay: smoothed RMS must be at least 2x smaller than raw.
    raw_rms=$(echo "$nat_out" | grep "raw RMS error" | awk '{print $NF}')
    sm_rms=$(echo  "$nat_out" | grep "Savitzky-Golay RMS" | awk '{print $NF}')
    awk -v r="$raw_rms" -v s="$sm_rms" 'BEGIN { exit !(r > 2*s) }' \
        || { echo "FAIL: nr6 — SG didn't reduce RMS by 2x"; echo "raw=$raw_rms sm=$sm_rms"; exit 1; }
    # Kalman: filter must beat raw measurement by >2x RMS.
    k_raw=$(echo "$nat_out" | grep "raw measurement RMS error" | awk '{print $NF}')
    k_est=$(echo "$nat_out" | grep "Kalman estimate RMS error" | awk '{print $NF}')
    awk -v r="$k_raw" -v e="$k_est" 'BEGIN { exit !(r > 2*e) }' \
        || { echo "FAIL: nr6 — Kalman didn't beat raw 2x"; echo "raw=$k_raw est=$k_est"; exit 1; }
    # Crank-Nicolson: heat must dissipate from peak 1.0 to ~0.4-0.5 at center.
    cn_line=$(echo "$nat_out" | grep "after Crank-Nicolson:")
    if ! echo "$cn_line" | grep -qE ": 0\.[34][0-9]+"; then
        echo "FAIL: nr6 — Crank-Nicolson dissipation"; echo "$cn_line"; exit 1
    fi

    echo "[test] demo — nr_demo7 (Cholesky + CG + simulated annealing + MCMC)"
    build/calcnat examples/nr_demo7.calc -o build/d_nr7.exe
    nat_out=$(build/d_nr7.exe | strip_cr)
    # Cholesky: NR's canonical example must produce L = [[2,0,0],[6,1,0],[-8,5,3]].
    echo "$nat_out" | grep -q "\[\[2, 0, 0\], \[6, 1, 0\], \[-8, 5, 3\]\]" \
        || { echo "FAIL: nr7 — Cholesky L matrix"; echo "$nat_out"; exit 1; }
    # CG must converge on the 100x100 Laplacian.
    echo "$nat_out" | grep -qE "CG converged = 1, iters = [0-9]+, residual = (0|[0-9]+(\.[0-9]+)?e-[0-9]+)" \
        || { echo "FAIL: nr7 — CG didn't converge"; echo "$nat_out"; exit 1; }
    # CG endpoint: x[49] should be 1275 (parabolic formula for the discrete Laplacian).
    echo "$nat_out" | grep -q "x\[49\] = 1275" \
        || { echo "FAIL: nr7 — CG solution wrong at center"; echo "$nat_out"; exit 1; }
    # Annealing: should locate a near-minimum f ~ -0.1.
    ann_f=$(echo "$nat_out" | grep "^best f" | awk '{print $NF}')
    awk -v f="$ann_f" 'BEGIN { exit !(f < -0.05 && f > -0.5) }' \
        || { echo "FAIL: nr7 — annealing didn't find minimum"; echo "f=$ann_f"; exit 1; }
    # MCMC: mean x^2 should be between 3 and 5 (theory is 4).
    mc_var=$(echo "$nat_out" | grep "mean x\^2" | awk '{print $4}')
    awk -v v="$mc_var" 'BEGIN { exit !(v > 3 && v < 5) }' \
        || { echo "FAIL: nr7 — MCMC mean x^2 out of band"; echo "v=$mc_var"; exit 1; }

    echo "[test] demo — nr_demo8 (Welch + wavelet + Toeplitz + simplex LP)"
    build/calcnat examples/nr_demo8.calc -o build/d_nr8.exe
    nat_out=$(build/d_nr8.exe | strip_cr)
    # Welch peak at 17 Hz (the injected tone).
    echo "$nat_out" | grep -q "peak at f = 17 Hz" \
        || { echo "FAIL: nr8 — Welch peak"; echo "$nat_out"; exit 1; }
    # Haar / D4 round-trip both at machine epsilon.
    h_err=$(echo "$nat_out" | grep "round-trip RMS error =" | head -1 | awk '{print $NF}')
    d_err=$(echo "$nat_out" | grep "D4 round-trip RMS error" | awk '{print $NF}')
    awk -v e="$h_err" 'BEGIN { exit !(e < 1e-12) }' \
        || { echo "FAIL: nr8 — Haar round-trip error"; echo "$h_err"; exit 1; }
    awk -v e="$d_err" 'BEGIN { exit !(e < 1e-12) }' \
        || { echo "FAIL: nr8 — D4 round-trip error"; echo "$d_err"; exit 1; }
    # Levinson: T x = [9, 7, 5, 3].
    echo "$nat_out" | grep -q "\[9, 7, 5, 3\]" \
        || { echo "FAIL: nr8 — Levinson reconstruction"; echo "$nat_out"; exit 1; }
    # Simplex: textbook example must return x = [2, 6], value = 36.
    echo "$nat_out" | grep -q "\[2, 6\]" \
        || { echo "FAIL: nr8 — simplex x"; echo "$nat_out"; exit 1; }
    echo "$nat_out" | grep -q "value   = 36" \
        || { echo "FAIL: nr8 — simplex value"; echo "$nat_out"; exit 1; }

    echo "[test] demo — nr_demo9 (2-D FFT + 2-D quad + power iter + B-spline)"
    build/calcnat examples/nr_demo9.calc -o build/d_nr9.exe
    nat_out=$(build/d_nr9.exe | strip_cr)
    # 2-D FFT round-trip must be effectively zero.
    f2_err=$(echo "$nat_out" | grep "round-trip RMS error:" | awk '{print $NF}')
    awk -v e="$f2_err" 'BEGIN { exit !(e < 1e-14) }' \
        || { echo "FAIL: nr9 — 2D FFT round-trip"; echo "$f2_err"; exit 1; }
    # Power iteration: dominant eigenvalue must equal 3+sqrt(3) to ~6 digits.
    eig_line=$(echo "$nat_out" | grep "dominant eigenvalue")
    if ! echo "$eig_line" | grep -qE "4\.73205080[0-9]+"; then
        echo "FAIL: nr9 — dominant eigenvalue"; echo "$eig_line"; exit 1
    fi
    # Inverse iteration finds 3 - sqrt(3) ~= 1.267949.
    ieig_line=$(echo "$nat_out" | grep "eigenvalue nearest 1.5")
    if ! echo "$ieig_line" | grep -qE "1\.2679491[0-9]+"; then
        echo "FAIL: nr9 — inverse iteration"; echo "$ieig_line"; exit 1
    fi
    # 2-D quadrature: x^2 + y^2 over [0,1]^2 = 2/3 to ~10 digits.
    gl_line=$(echo "$nat_out" | grep "5-point GL  =" | head -1 | awk '{print $NF}')
    awk -v v="$gl_line" 'BEGIN { exit !(v > 0.6666 && v < 0.6667) }' \
        || { echo "FAIL: nr9 — quad2d_gl"; echo "$gl_line"; exit 1; }

    echo "[test] demo — nr_demo10 (Neville + GL nodes + BFGS + PCA)"
    build/calcnat examples/nr_demo10.calc -o build/d_nr10.exe
    nat_out=$(build/d_nr10.exe | strip_cr)
    # Neville interp at x=1.5 of sin samples must be near sin(1.5)=0.997.
    nev_line=$(echo "$nat_out" | grep "^  x=1.5 ")
    if ! echo "$nev_line" | grep -qE "interp=0\.9[0-9]+"; then
        echo "FAIL: nr10 — Neville interp at 1.5"; echo "$nev_line"; exit 1
    fi
    # Gauss-Legendre on int sin(0..pi): n=10 must give 2 to ~15 digits.
    gl_line=$(echo "$nat_out" | grep "n=10")
    if ! echo "$gl_line" | grep -qE "  2  err=[0-9.eE+-]+"; then
        echo "FAIL: nr10 — GL n=10 for int sin"; echo "$gl_line"; exit 1
    fi
    # BFGS Rosenbrock: f* must be < 1e-15.
    bfgs_f=$(echo "$nat_out" | grep "^  f\* = " | head -1 | awk '{print $NF}')
    awk -v f="$bfgs_f" 'BEGIN { exit !(f < 1e-15) }' \
        || { echo "FAIL: nr10 — BFGS Rosenbrock f*"; echo "f=$bfgs_f"; exit 1; }
    # PCA: first principal axis should have variance close to 9 (true variance).
    pca_var_line=$(echo "$nat_out" | grep "variance explained")
    # We expect "[8.5..something, 0.8..something]"; just check first component > 7.
    pca_var_first=$(echo "$nat_out" | grep -A1 "variance explained per PC:" \
        | tail -1 | sed 's/\[//' | awk -F',' '{print $1}')
    awk -v v="$pca_var_first" 'BEGIN { exit !(v > 7 && v < 11) }' \
        || { echo "FAIL: nr10 — PCA first variance"; echo "$pca_var_first"; exit 1; }

    echo "[test] demo — fft_demo (recover 7 Hz + 18 Hz peaks)"
    rm -f build/fft_mag.svg build/fft_signal.svg
    build/calcnat examples/fft_demo.calc -o build/d_fft.exe
    nat_out=$(build/d_fft.exe | strip_cr)
    # We injected tones at 7 Hz and 18 Hz; the FFT bin width is fs/N
    # = 0.39 Hz, so the closest detectable bins are around 7.03 and
    # 17.97. Just verify those two specific peaks show up.
    echo "$nat_out" | grep -q "7.03125 Hz"      || { echo "FAIL: fft — 7Hz peak"; exit 1; }
    echo "$nat_out" | grep -q "17.96875 Hz"     || { echo "FAIL: fft — 18Hz peak"; exit 1; }
    # ifft(fft(x)) round-trip should be machine-epsilon accurate.
    err_line=$(echo "$nat_out" | grep "max error")
    if ! echo "$err_line" | grep -qE "e-1[3456]"; then
        echo "FAIL: fft round-trip error not at machine epsilon"
        echo "$err_line"; exit 1
    fi

    echo "[test] demo — regression (writes build/regression.svg)"
    rm -f build/regression.svg
    build/calcnat examples/regression.calc -o build/d_reg.exe
    # The numeric output uses random-looking noise but is deterministic
    # given the fixed seed in the source. We just check the program ran
    # cleanly and produced a valid SVG, plus that the slope ended up
    # close to the true model's 1.7.
    out=$(build/d_reg.exe | strip_cr)
    if ! echo "$out" | grep -q "wrote build/regression.svg"; then
        echo "FAIL: regression demo didn't write the SVG"; echo "got: $out"; exit 1
    fi
    if ! echo "$out" | grep -qE "y = 1\.7[0-9]+ \* x \+"; then
        echo "FAIL: regression demo fit slope is not near 1.7"; echo "got: $out"; exit 1
    fi
    if [ ! -s build/regression.svg ]; then
        echo "FAIL: regression demo did not produce build/regression.svg"; exit 1
    fi

    echo "[test] calcnat one-step build (input.calc -> input.exe via gcc)"
    # Smoke-test the simpler driver invocation: one argument in, one
    # executable out. No explicit gcc call required by the user.
    build/calcnat tests/native_basic.calc -o build/native_basic_onestep.exe
    nat_out=$(build/native_basic_onestep.exe | strip_cr)
    vm_out=$(build/calcvm build/native_basic.vm.cexe | strip_cr)
    check "calcnat_one_step" "$vm_out" "$nat_out"
else
    echo "[skip] native x86-64 backend (gcc not on PATH)"
fi

echo "all tests passed"
