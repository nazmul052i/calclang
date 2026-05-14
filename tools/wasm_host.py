#!/usr/bin/env python3
"""Reference host for CalcLang-generated .wat modules.

Provides the imports calcwasm expects under the "env" namespace:
  print_num — print a number, one per line
  sin / cos / tan / asin / acos / atan / atan2
  exp / log / log10 / pow / round / random / pi / e

Usage:
  python tools/wasm_host.py path/to/foo.wat
  python tools/wasm_host.py path/to/foo.wasm

Prints whatever the program prints to stdout. Returns the exit code
of the program (currently always 0; CalcLang Stage 1 doesn't expose
exit codes through the Wasm interface).
"""
import math
import random
import sys
from pathlib import Path

import wasmtime


def fmt(x: float) -> str:
    # Match CalcLang's print formatting roughly: integers show without
    # decimals, others use up to ~10 significant digits.
    if x == int(x) and abs(x) < 1e16:
        return str(int(x))
    return ("%.10g" % x)


def main():
    if len(sys.argv) != 2:
        print("usage: wasm_host.py <module.wat|module.wasm>", file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    if not path.exists():
        print(f"wasm_host: {path}: no such file", file=sys.stderr)
        return 2

    engine = wasmtime.Engine()
    store = wasmtime.Store(engine)

    src = path.read_bytes()
    module = wasmtime.Module(engine, src)

    def env(fn):
        return wasmtime.Func(store, fn.type, fn.fn)

    # Build the env imports the calcwasm modules expect.
    F64 = wasmtime.ValType.f64()

    def make_unary(callable_):
        ftype = wasmtime.FuncType([F64], [F64])
        return wasmtime.Func(store, ftype, lambda x: callable_(x))

    def make_binary(callable_):
        ftype = wasmtime.FuncType([F64, F64], [F64])
        return wasmtime.Func(store, ftype, lambda x, y: callable_(x, y))

    def make_nullary(callable_):
        ftype = wasmtime.FuncType([], [F64])
        return wasmtime.Func(store, ftype, lambda: callable_())

    def print_num_impl(x):
        print(fmt(x))

    print_ftype = wasmtime.FuncType([F64], [])
    print_func = wasmtime.Func(store, print_ftype, print_num_impl)

    imports_by_name = {
        "print_num": print_func,
        "sin":   make_unary(math.sin),
        "cos":   make_unary(math.cos),
        "tan":   make_unary(math.tan),
        "asin":  make_unary(math.asin),
        "acos":  make_unary(math.acos),
        "atan":  make_unary(math.atan),
        "atan2": make_binary(math.atan2),
        "exp":   make_unary(math.exp),
        "log":   make_unary(math.log),
        "log10": make_unary(math.log10),
        "pow":   make_binary(math.pow),
        "round": make_unary(lambda x: float(round(x))),
        "random": make_nullary(random.random),
        "pi":    make_nullary(lambda: math.pi),
        "e":     make_nullary(lambda: math.e),
    }

    imports = []
    for imp in module.imports:
        mod_name = imp.module
        item_name = imp.name
        if mod_name != "env":
            print(f"wasm_host: unexpected import module '{mod_name}'", file=sys.stderr)
            return 2
        if item_name not in imports_by_name:
            print(f"wasm_host: unknown import 'env.{item_name}'", file=sys.stderr)
            return 2
        imports.append(imports_by_name[item_name])

    inst = wasmtime.Instance(store, module, imports)
    start = inst.exports(store).get("_start")
    if start is None:
        print("wasm_host: module has no _start export", file=sys.stderr)
        return 2
    start(store)
    return 0


if __name__ == "__main__":
    sys.exit(main())
