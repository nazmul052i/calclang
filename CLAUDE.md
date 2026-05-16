# CalcLang — status

> What's in, what's deferred, where the seams are. Update this file
> whenever a significant feature lands or a planned one moves.

Last updated: 2026-05-15 (post-`clc` driver / `bin/` split / `.clc` extension)

---

## 1. What CalcLang is today

A statically-compiled, dynamically-typed scripting language with three
back-ends and a real engineering / scientific standard library.
~15,500 lines of C in the compiler + runtime, ~11,000 lines of
CalcLang in `lib/` and `examples/`, ~5,800-line book in
`docs/book.md`. Builds in ~3 s on a laptop. Source files use the
`.clc` extension; compiler tools live in `bin/`.

### Pipelines

The unified `clc` driver (gcc-flavored CLI) is the front door:

```
clc foo.clc                   # native build → foo.exe
clc -o foo.wat foo.clc        # wasm
clc -o foo.casm foo.clc       # bytecode only
clc -r foo.clc                # compile + run on VM
clc -S foo.clc                # emit native assembly
```

Under the hood `clc` dispatches to the same six tools:

| Tool         | Front-end              | What it emits                | Run with               |
|--------------|------------------------|------------------------------|------------------------|
| `calcc`      | full parser            | `.casm` text bytecode        | `calcasm` → `calcld` → `calcvm` |
| `calcnat`    | same parser            | x86-64 assembly via gcc      | direct `.exe`          |
| `calcwasm`   | same parser            | WebAssembly text (`.wat`)    | `wasmtime`, browser, Node — runs on *any* Wasm host (numeric subset today) |

The VM is portable C and runs anywhere a C compiler targets — already
the answer for "does this work on ARM" until the Wasm backend covers
strings/arrays.

### Language surface

- Dynamic types: `num` (f64), `str`, `arr`, `map`, `fn`, `cpx`,
  `struct` (records), `class` (with methods + `this`)
- Flow-sensitive type inference annotates each AST use with a precise
  type — drives the native backend's fast-path codegen
- Control flow: `if`/`else`, `while`, `do`/`while`, `for` (C-style),
  `for x in xs`, `for i in 0..n` / `0..=n`, `break`, `continue`,
  ternary `? :`, `switch`/`case`/`default` (no fall-through),
  `try`/`catch`/`throw`
- Functions: top-level, anonymous closures, first-class fns,
  classes-as-callable, optional type annotations + per-arg compile-time
  checks
- Modules: `import "name"` (bare → `lib/name.clc`), `import "nr.brent"`
  (dot syntax), `import "./helper.clc"` (importer-relative), cycle
  detection, single-command builds
- Operators: full set — arithmetic, comparison, logical, bitwise
  (`& | ^ ~ << >>`), compound assignment, hex / binary literals
- Format string with C-style + Rust `{}` placeholders; `printf` /
  `println` statements; `\e` and `\0` escapes

### Standard library

- **`lib/math`** — sq, cube, hypot, hyperbolics, lerp, remap …
- **`lib/linalg`** — matrices, mat_mul, mat_solve, mat_det,
  mat_inv, vec / matrix norms
- **`lib/stats`** — mean, variance, stddev, median, min/max,
  linreg, histogram, plus extended (argmin/argmax, cumsum,
  cumprod, diff, percentile, quartiles, covariance, sem,
  zscore, normalize, coeff_variation)
- **`lib/vec`** — NumPy-style vec_add / sub / mul / div with
  scalar broadcasting, vec_sin / cos / exp / log / sqrt / abs,
  vec_dot, vec_norm / norm1 / norm_inf, vec_axpy, linspace,
  arange, vec_zeros / ones / full
- **`lib/constants`** — math + 2019-SI physics constants +
  unit conversions (eV/J, atm/Pa, °C/°F/K, rad/deg, etc.)
- **`lib/plot`** — SVG output (line, multi-series, bar, log_y,
  labeled)
- **`lib/numeric`** — root finding, integration, interpolation
- **`lib/random`** — reproducible PRNG class, normal samples
- **`lib/csv`** — CSV read / write with optional headers
- **`lib/json`** — parser + encoder
- **`lib/ode`** — RK4 scalar + vector, Euler
- **`lib/fft`** — Cooley-Tukey FFT (complex)
- **`lib/regex`** — friendly wrappers + validators
  (`is_email`, `is_ipv4`, `is_iso_date`, …)
- **`lib/datetime`** — DateTime record + arithmetic, ISO parse
- **`lib/http`** — typed HTTP responses, JSON helpers
- **`lib/gui`** — Color, button, checkbox, slider, text_input,
  dropdown, table, plot_xy, menubar, scrollbar, modal helpers
- **`lib/nr/`** — 30 Numerical-Recipes modules: Brent, spline,
  special fns, eigen, LU, Romberg, RK45, polynomial families,
  minimization, sort, Ridders, Newton, LM fit, QR, SVD,
  polyroots, conv, Chebyshev, Savitzky-Golay, Kalman, PDE,
  Cholesky, CG, anneal, MCMC, Welch, wavelet, Toeplitz, simplex,
  fft2d, quad2d, power_eigen, B-spline, Neville, GL nodes,
  BFGS, PCA

### Native runtime builtins (selected)

- **Math + complex**: sin/cos/log/exp/pow/sqrt/abs/…, complex(re, im),
  real/imag/conj/arg
- **Strings**: str_slice, str_split, str_join, str_upper, str_lower,
  str_starts_with, str_ends_with, str_find, str_repeat, str_trim, fmt
- **Arrays / maps**: push, pop, array_*, keys, values, has_key, del
- **I/O**: print, write, read_line, file_read / write / append /
  exists, system
- **Time**: time_ms (monotonic), epoch_ms (wall clock),
  time_components, time_make, time_format, sleep_ms, time_format
  (strftime)
- **Terminal interactive**: read_key (non-blocking),
  `\e` ANSI escape literal
- **Regex**: regex_match, regex_find, regex_find_all,
  regex_replace, regex_split (full PCRE subset)
- **HTTP**: http_get, http_post, http_post_json, http_status
  (WinHTTP)
- **Integer helpers**: parse_hex, to_hex, to_bin, bit_count,
  wrap_i32 / u32 / u64, hash_u32 / u64 (FNV-1a)
- **Debug**: assert, trace
- **IEEE-754**: is_nan, is_inf, is_finite, is_normal
- **GUI (SDL2 + stb_truetype)**: 24 `gui_*` functions —
  window, draw, text, mouse, keyboard, clip, modal dialogs,
  native file open/save
- **FFI**: ffi_load, ffi_call (up to 4 args)

### Examples shipped

- `tetris.clc` — terminal Tetris (ANSI escapes)
- `tetris_gui.clc` — SDL2 windowed Tetris with score panel
- `gui_form_demo.clc` — settings form (button / checkbox /
  slider / dropdown / text_input)
- `engineering_dashboard.clc` — menubar + scrollable table +
  plot + modal alerts + native file dialogs
- `nr_demo` through `nr_demo10` — Numerical-Recipes examples
- `math_demo`, `sine_plot`, `regression`, `linsys`, `numerical`,
  `monte_carlo`, `csv_demo`, `multi_plot`, `json_demo`, `ode_demo`,
  `fft_demo`

### Test suite

~70 entries in `tests/run_tests.sh`. Runs every backend (VM and
native), all 21 demos with output spot-checks, the Wasm backend if
the `wasmtime` Python package is installed.

---

## 2. Recent additions (chronological)

| Commit       | Lands                                                          |
|--------------|----------------------------------------------------------------|
| `be5e161`    | `import` — Python-style dot syntax + nested module specs       |
| `ad09628`    | All examples + libs migrated to single-command `import` form   |
| `dd910f7`    | Book Chapter 8 rewritten around `import`                       |
| `0157c87`    | `for x in xs`, `for i in 0..n` / `0..=n`, ranges               |
| `334197d`    | Migrated 566 C-style for-loops in example/lib to for-in        |
| `7053adc`    | `struct` typed records                                         |
| `b287f8b`    | `LinReg` / `LU` / `QR` libs use struct                         |
| `c64c229`    | calcnat enforces per-arg types at user-fn call sites           |
| `b354730`    | Terminal I/O — `sleep_ms`, `read_key`, `time_ms`, `\e` escape  |
| `993a2da`    | `calcwasm` Stage 1 — Wasm backend for numeric subset           |
| `f1470f5`    | SDL2 GUI — window, draw, input, widget kit, GUI Tetris         |
| `efb1f69`    | GUI bitmap text + circles                                      |
| `c2e2294`    | text_input + dropdown widgets, focus tracking                  |
| `539fd2f`    | Clipping, scrollable table, plot, menubar, modal, file dialogs |
| `0afb9f4`    | GUI text — anti-aliased TTF via stb_truetype                   |
| `9319cb0`    | `datetime` — wall-clock date/time + lib/datetime.clc          |
| `e5d72dc`    | Regex engine — full PCRE subset, 5 builtins, lib/regex         |
| `14340a7`    | HTTP client + typed-integer helpers + debugger-lite            |
| `f5ff1ef`    | Vec ops + extended stats + constants + NaN/Inf predicates      |
| `d5e1fae`    | `m[i, j]` chained indexing + 1-D slicing `m[lo:hi]`            |
| (this work)  | `bin/` split + `clc` gcc-style driver + `.clc` extension hard cut |
| (this work)  | install-aware paths (`CALC_HOME`/`CALC_LIB_PATH` from `argv[0]`) + `make install` |
| (this work)  | package manager v0.1 — `clc init`, `clc install`, `clc.toml` + `clc.lock`, `deps/` probe |

---

## 3. What's pending

### Big multi-session projects (each its own focused work)

| Item                              | Why it's a multi-session effort                                            |
|-----------------------------------|-----------------------------------------------------------------------------|
| **Real concurrency / asyncio**    | Needs C-to-CalcLang call bridge (so C code can resume a Task), then fibers or state-machine async transform, then async I/O. Estimated 4 sessions: bridge → callback event loop → Promise → `async`/`await` syntax. |
| **Real typed-integer types**      | Adds new NaN-box tags (one per int variant) + per-op dispatch + parser type-annotation syntax. Currently shipped as a "lite" set of `wrap_i32 / u32 / u64` + `hash_*` + `parse_hex` builtins. |
| **Source-level debugger**         | Needs DWARF-style debug info baked into the emitted `.s`, a `calcdbg` driver, breakpoint protocol. Currently shipped as `assert(cond, msg)` + `trace(label)`. |
| **ARM64 native backend**          | Parallel to `codegen_x64.c`. Different register set, calling convention, instruction syntax. The VM already runs on ARM today, so this is purely a speed play. |
| **Wasm Stages 2-4**               | Stage 2 — strings/arrays/maps via Wasm linear memory + allocator. Stage 3 — WASI bindings for print, file, time. Stage 4 — browser harness (HTML + JS glue, canvas, keyboard). Each is roughly one session. |
| **BLAS/LAPACK bindings (FFI)**    | Bind `dgemm`, `dgemv`, `dgetrf`, `dsyev`, `dgeev` to system OpenBLAS / MKL. Real performance unlock for large matrices. |
| **SIMD-vectorized unboxed arrays**| Stage 3 + 4 of the typed-records project. `[f64; N]` arrays stored flat, AVX intrinsics in the native backend. The Fortran-class performance push. |
| **OPC-UA package**                | Wrap `open62541` via new builtins. ~3 sessions: connect/read/write → browse/subscribe → security. Deferred per user. |
| **GUI text editor primitives**    | Selection, multi-line wrap, syntax highlighting hooks. Stage 2 of the GUI text-input work. |
| **Native file dialogs on POSIX**  | Currently Win32-only via comdlg32. Linux needs zenity / GTK FileChooser; macOS NSOpenPanel via Cocoa. |
| **Interactive plot widget**       | Extend `lib/gui.clc::plot_xy` with mouse zoom/pan, multi-series legend, log-scale toggle. ~1 session. |
| **NumPy `.npy` / HDF5 I/O**       | Exchange data with Python/SciPy. NPY is ~1 session; HDF5 needs a binding. |
| **Date/time — timezones**         | Today's UTC-only datetime needs a timezone lib; bind to ICU or zoneinfo. |
| **Real package manager**          | Versioned dependencies, registry, lockfiles. Multi-session. v0.1 (`clc init` / `install` / `clc.toml` / `deps/` probe with git URLs only) has landed; what's missing is version constraints (`^1.2`, `~1.2.3`), transitive resolution, conflict resolution, a registry, and `clc add` / `clc publish`. |
| **LSP server**                    | Editor integration with completion, go-to-definition, hover. Multi-session. |

### Smaller / fixable in a single session

- **Matrix-slice primitive** so `m[1:5, :]` and `m[:, j]` work (currently rejected at the parser-comma case)
- **POSIX HTTP** — bind libcurl via FFI; current http_* returns "" on non-Windows
- **`re.IGNORECASE` flag plumbing** — engine supports CL_RE_ICASE but the CalcLang side doesn't expose it
- **Selective imports** — `from "stats" import mean, stddev`
- **`const` qualifier** — compile-time immutable bindings
- **`enum` type** — named integer constants
- **Numeric spinner widget** in `lib/gui.clc`
- **POSIX raw-mode terminal** — `read_key` currently has the termios path; needs testing on real Linux/macOS
- **Tree widget** in `lib/gui.clc`
- **Status-bar widget** in `lib/gui.clc`
- **Drag and drop between widgets**

### Known wart fixes

- Trailing-space loss in `Edit` tool stripping test fixtures — workaround: bracket whitespace-edge output with `[...]`
- `calcnat` driver evaluates a slice's target twice when upper bound is omitted; document or fix with a let-temp
- `format-truncation` warning in `parse_import` (cosmetic, doesn't affect output)
- Coroutine / fiber notion is conspicuously missing; nothing in the codebase calls `cl_throw` / `cl_op_*` from a context other than the main CalcLang stack

---

## 4. Suggested next session

Pick one from the **smaller/fixable** list (quick win, 1 session) or
commit to a stage of one of the **multi-session projects**. The user
has been picking items from `book.md`'s "what could you build on top"
prompts.

When starting a new session: read this file's section 2 to see what
just happened, then update section 3 if you add/move items.
