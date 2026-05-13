# Contributing to CalcLang

Thanks for your interest. CalcLang is a small educational compiler and a CalcLang-implemented standard library. Contributions of any size are welcome — typo fixes in the book, new test cases, new stdlib functions, codegen improvements.

## Quickstart

```bash
git clone <repo-url> calclang
cd calclang
make            # builds calcc, calcasm, calcld, calcvm, calcnat
make test       # runs the full test suite (VM + native pipelines)
make demos      # builds + runs all engineering-library demos
```

You need `gcc` on PATH (MinGW on Windows, system gcc on Linux/macOS) and `sh`. No other dependencies.

## Repository layout

| Path | What's in it |
|---|---|
| `src/` | The C compiler + runtime: lexer, parser, AST, type inference, two codegens (bytecode for `calcvm`, native x86-64), the GC'd runtime |
| `include/` | Public headers |
| `lib/` | Engineering standard library written *in CalcLang* — linalg, stats, numeric methods, ODE, FFT, JSON, CSV, plotting, random |
| `examples/` | Worked-out CalcLang programs that exercise the language and the stdlib |
| `tests/` | Test sources (one `.calc` per feature/scenario) + `run_tests.sh` |
| `docs/` | [`book.md`](docs/book.md) — a K&R-style tutorial, the primary reference |
| `editor/` | VS Code syntax-highlighting extension |
| `Makefile` | Top-level build + test + demo targets |

## Running the test suite

```bash
make test                                # 58 tests as of last release
```

Every test compiles a `.calc` source through *both* the bytecode VM (`calcc` → `calcasm` → `calcld` → `calcvm`) and the native x86-64 backend (`calcnat` → `gcc`) and asserts byte-identical output. New features should land with a test that exercises both pipelines if applicable; native-only features (file I/O, complex numbers, FFI, exceptions, etc.) land with native-only tests.

## Adding a new language feature

The compilation pipeline is small and worth reading end-to-end:

```
src/lexer.c            tokenize
src/parser.c           tokens → AST
src/type_infer.c       flow-sensitive type annotations
src/codegen.c          AST → Calc bytecode .casm  (for the VM)
src/codegen_x64.c      AST → x86-64 .s            (for native)
src/runtime_x64.c      NaN-boxed Value runtime + GC + calclib (native)
src/calcvm.c           VM interpreter
```

When you add a new construct:

1. **Lexer**: add the keyword/token to `include/lexer.h` and `src/lexer.c`.
2. **AST**: add the node kind to `include/ast.h`, a constructor in `src/ast.c`, plus debug-print and free.
3. **Parser**: extend `src/parser.c`.
4. **Type inference (optional)**: extend `src/type_infer.c` if the new node affects flow-sensitive types.
5. **Bytecode codegen**: extend `src/codegen.c` and possibly `include/insn.h` + `src/calcvm.c` for a new opcode.
6. **Native codegen**: extend `src/codegen_x64.c`, and `src/runtime_x64.c` if a new runtime helper is needed.
7. **Test**: add `tests/<feature>.calc` and wire it into `tests/run_tests.sh`.
8. **Document**: update `docs/book.md`.

Most existing features are bounded to a few hundred lines across the whole pipeline — the design intentionally keeps each pass small.

## Adding a stdlib module (CalcLang code)

1. Drop `lib/<module>.calc` in. Mark exports with `pub fn`; private helpers as bare `fn` or `priv fn`.
2. Add a one-shot build to `Makefile`'s `libs:` target.
3. Add a demo under `examples/` and a `make <demo>` target.
4. Wire test-runner verification: at minimum, build the demo, run it, check the exit code; ideally also `diff` its output against an expected string.
5. Document in `docs/book.md` (Chapter 10.5 — Engineering library).

## Style

- C code is `-std=c11 -Wall -Wextra -pedantic` clean. The Makefile builds with these flags; CI fails on warnings.
- Comments explain *why*, not *what*. Most non-obvious decisions in the codebase have a paragraph nearby explaining the trade-off.
- CalcLang code in `lib/` uses 4-space indent, single-quote-free strings (CalcLang only has `"..."`), and explicit type annotations on `pub fn` parameters where it improves clarity.
- No dependencies. The whole compiler + runtime is plain C; the stdlib is plain CalcLang.

## Filing issues

A good issue includes:

- The CalcLang source that reproduces the problem (or, for compiler bugs, the smallest piece that triggers it).
- What you expected and what you got.
- Which pipeline (VM via `calcc`/`calcvm`, or native via `calcnat`).
- Platform (OS, compiler version).

## License

By contributing, you agree your contributions are licensed under the [MIT License](LICENSE).
