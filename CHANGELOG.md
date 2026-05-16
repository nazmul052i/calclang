# Changelog

Reverse-chronological. Tracks language- and library-level changes. Bug-fix-only commits not listed.

## Unreleased

### Package manager v0.1 — `clc init`, `clc install`, `clc.toml`

Per-project manifest, lockfile, and `deps/` fallback in import resolution. Git URLs only — no registry, no version constraints beyond exact tags, no transitive resolution.

- **`clc.toml`** at project root — TOML-subset format. Top-level `name` / `version`, plus one `[dependencies.<name>]` section per dep with `git = "..."` and optional `tag = "..."`.
- **`clc init [<dir>]`** — scaffolds `clc.toml` + a `main.clc` stub with sensible defaults.
- **`clc install`** — reads `clc.toml`, shallow-clones each dep (via `git clone --depth 1 [--branch <tag>]`) into `deps/<name>/`, writes `clc.lock` pinning the resolved commit hash for each.
- **Parser import resolution** gains a `deps/<name>/<name>.clc` probe (after CWD and importer-relative, before the stdlib `CALC_LIB_PATH` fallback). Project deps shadow stdlib by precedence. Only fires for non-nested module specs — `import "nr.poly"` still targets stdlib.

### Toolchain: installable, gcc-style driver, `.clc` extension

CalcLang is now a self-contained installable toolchain rather than a repo-rooted one. `make install PREFIX=…` stages `bin/`, `lib/`, runtime sources, headers, and vendored SDL2 + stb into `<prefix>/calclang/`; `make uninstall` removes it.

- **`clc` driver** (`src/clc.c`) — unified gcc-flavored CLI. `clc foo.clc` builds native, `-o foo.wat` builds wasm, `-c` emits bytecode, `-S` emits assembly, `-r` runs on the VM. `-O0/-O1/-O2`, `-v`, `--help`, `--version` supported. Wraps the existing six tools (`calcc`, `calcasm`, `calcld`, `calcvm`, `calcnat`, `calcwasm`), which remain individually callable.
- **`bin/` split** — compiler binaries live in `bin/`; intermediates stay in `build/`. Mirrors the gcc/clang convention.
- **`.clc` extension** — source files use `.clc` (was `.calc`). Hard cut: 139 file renames, parser-side import resolution and `calcnat`'s arg parser both updated. VS Code extension migrated.
- **Install-aware path resolution** — front-end tools (`calcc`, `calcnat`, `calcwasm`) call `cl_init_install_paths(argv[0])` at main entry, which sets two env vars from the tool's own location: `CALC_HOME` (umbrella install root) and `CALC_LIB_PATH` (stdlib search). The parser falls back to `CALC_LIB_PATH/<rest>` when an `import "lib/..."` isn't found CWD- or importer-relative. `calcnat` honors `CALC_HOME` when constructing the gcc command-line for `src/runtime_x64.c`, `src/regex.c`, `src/runtime_gui_sdl2.c`, headers, and the vendored SDL2 root. Each env var respects user override and only auto-fills if unset.

### Numerical-Recipes library (branch `numerical-library`)

Ten tiers of NR-canon modules under `lib/nr/`. Demos `nr_demo` through `nr_demo10` exercise each.

- Tier 1: `brent` (root finder), `spline` (natural cubic), `special` (gamma/erf), `eigen` (Jacobi).
- Tier 2: `lu` (Doolittle + partial pivoting), `romberg` (Richardson extrapolation), `rk45` (Cash-Karp adaptive), `poly` (Chebyshev/Legendre/Hermite/Laguerre/Bessel).
- Tier 3: `minimize` (golden section, Brent 1-D, Nelder-Mead), `sort` (heapsort, quickselect, median).
- Tier 4: `diff` (Ridders'), `random_dist` (gamma/chi²/beta/t/Cauchy/binomial), `newton` (NR for nonlinear systems), `fitnl` (Levenberg-Marquardt).
- Tier 5: `qr` (Householder), `svd` (Jacobi-based + pseudo-inverse), `polyroots` (Laguerre + deflation), `conv` (direct & FFT convolution / correlation).
- Tier 6: `cheb` (Chebyshev approximation + exact coefficient calculus), `savgol` (Savitzky-Golay smoothing / derivative filters), `kalman` (discrete linear Kalman filter), `pde` (Crank-Nicolson 1-D diffusion via Thomas solver).
- Tier 7: `cholesky` (SPD factorization + solve + log-det + inverse), `cg` (conjugate gradient with optional preconditioner), `anneal` (simulated annealing), `mcmc` (Metropolis-Hastings sampler).
- Tier 8: `welch` (Welch periodogram PSD), `wavelet` (Haar + Daubechies-4 DWT), `toeplitz` (Levinson-Durbin + Yule-Walker AR estimation), `simplex_lp` (two-phase simplex linear programming).
- Tier 9: `fft2d` (two-dimensional FFT and inverse), `quad2d` (fixed-order and adaptive 2-D Gauss-Legendre), `power_eigen` (power / inverse iteration), `bspline` (Cox-de Boor basis + least-squares fitting).
- Tier 10: `neville` (Neville polynomial interpolation), `glnodes` (arbitrary-order Gauss-Legendre nodes/weights), `bfgs` (BFGS quasi-Newton minimizer), `pca` (PCA via SVD).

### Bug fix: complex arithmetic type inference

`infer_binop` previously returned `TS_NUM_BIT` for every non-`+` arithmetic op regardless of operand types. This wrongly tagged complex-valued expressions as numeric, and the codegen subsequently emitted hardware `divsd` / `mulsd` on NaN-tagged complex bit-patterns — producing silent NaN corruption. Now the inference widens to `TS_ANY_MASK` whenever either operand isn't statically `num`, forcing dispatch through the polymorphic runtime helpers.

### Lexer: scientific-notation literals

`1.5e-7`, `2.3e+4`, `.5e2` etc. parse as numeric literals. Bare `e` (the constant) still tokenizes as an identifier when not preceded by digits.

### Engineering stdlib expansion (tier 2)

- New `lib/json.clc` — JSON parser + encoder with escape decoding and the standard subset of types.
- New `lib/ode.clc` — fourth-order Runge-Kutta for scalar and vector ODEs, plus Euler for comparison.
- New `lib/fft.clc` — recursive Cooley-Tukey radix-2 FFT using CalcLang's first-class complex numbers; round-trips at machine epsilon.
- `lib/plot.clc` extended — `plot_lines` (multi-series with legend), `plot_bar`, `plot_logy`, `plot_line_labeled`.

### Engineering stdlib (tier 1)

- `lib/linalg.clc`, `lib/stats.clc`, `lib/numeric.clc`, `lib/random.clc`, `lib/csv.clc`, `lib/plot.clc` — first batch of CalcLang-implemented engineering libraries: matrices (with `solve`, `det`, `inv`, norms), descriptive statistics, root finding + numerical integration + interpolation, class-based reproducible PRNG, CSV I/O, SVG plotting.
- Demo programs under `examples/`: `linsys`, `sine_plot`, `regression`, `numerical`, `monte_carlo`, `csv_demo`.

### Phase 4 — dynamic linking + first-class FFI

- New native builtins `ffi_load(path)` and `ffi_call(lib, name, sig, args)`.
- Cross-platform: `LoadLibraryA` / `GetProcAddress` on Windows, `dlopen` / `dlsym` on Linux.
- Signature dispatcher supports up to four args with `v` / `i` / `d` / `s` codes — covers the bulk of libc/libm.
- Wrap an `ffi_call` in a closure and you get a first-class CalcLang function value.

### Phase 3 — exceptions

- `throw expr;` and `try { ... } catch (name) { ... }`.
- Manual stack unwinding via inline assembly — restores `rsp` / `rbp` and jumps to the catch label.
- Uncaught exceptions print a diagnostic and exit with code 1.
- Any Value type can be thrown (string / number / map / complex / etc.).

### Phase 2 — tail-call optimization

- `return foo(args);` inside `fn foo(...)` compiles to a jump back to the function's body-start label after rewriting parameter slots.
- A million-deep `count_down` runs in flat stack space; the same source on the VM (no TCO there) overflows.
- Scope: direct self-recursion to a named top-level function with no captured parameters.

### Phase 1 — engineering primitives

- New native builtins: `file_read`, `file_write`, `file_append`, `file_exists`, `system`.
- First-class complex numbers with a new NaN-boxed tag, polymorphic `+ - * /`, and a small calclib: `complex`, `real`, `imag`, `conj`, `arg`; `abs` overloaded for magnitude.

### Tutorial book

- New `docs/book.md` — a K&R-style tutorial in 12 chapters covering hello-world through closures, classes, multi-file programs, the engineering stdlib, and the compiler internals.

### Native backend completeness

- Multi-file native linking via `--lib` flag in `calcnat`.
- Simpler driver: `calcnat foo.clc` produces `foo.exe` directly (invokes gcc internally with the runtime).
- Garbage collection: mark-and-sweep with conservative stack scanning (Windows + Linux).
- Strings, arrays, maps, closures, classes, first-class fn refs (user *and* calclib builtin), all 52 calclib builtins, polymorphic comparisons + truthiness.

### Earlier milestones

- Flow-sensitive type inference (forward dataflow with union-type lattice).
- Native x86-64 backend foundations (NaN-boxed values, basic numeric subset).
- VM-side: classes with `this`, methods, field-access sugar; closures with mutable + transitive capture; first-class user-fn refs; first-class calclib builtins via auto-generated trampolines.

For per-commit history, see `git log`.
