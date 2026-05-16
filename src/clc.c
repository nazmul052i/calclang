/* clc — unified gcc-style driver for CalcLang.
 *
 * Thin wrapper that dispatches to the existing toolchain
 * (calcc/calcasm/calcld/calcvm/calcnat/calcwasm), which it locates
 * as siblings next to its own executable.
 *
 * Targets (inferred from -o extension unless -t is given):
 *   clc foo.clc                  -> foo.exe        (native, default)
 *   clc -o app.exe foo.clc       -> app.exe        (native)
 *   clc -o app.wat foo.clc       -> app.wat        (wasm)
 *   clc -o app.casm foo.clc      -> app.casm       (bytecode only)
 *   clc -t vm|native|wasm foo.clc
 *
 * Modes:
 *   -c           compile to .casm, no link/run
 *   -S           emit native assembly (.s) via calcnat -S
 *   -r           compile and run on the VM (script mode)
 *   --lib        native library compile (no `main`, no top-level)
 *   --ast        dump AST instead of compiling
 *
 * Other:
 *   -O0|-O1|-O2   optimization level (default -O1; passed via CLC_OPT env)
 *   -o <path>     explicit output path
 *   -v            verbose — print every subprocess invocation
 *   -h|--help
 *   -V|--version
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <process.h>
#include <io.h>
#include <direct.h>
#define PATH_SEP  '\\'
#define EXE_EXT   ".exe"
#define MKDIR(p)  _mkdir(p)
#define STAT_T    struct _stat
#define STAT_FN   _stat
#else
#include <unistd.h>
#include <sys/wait.h>
#define PATH_SEP  '/'
#define EXE_EXT   ""
#define MKDIR(p)  mkdir(p, 0755)
#define STAT_T    struct stat
#define STAT_FN   stat
#endif

#include "common.h"
#include "version.h"

typedef enum { TGT_NONE, TGT_NATIVE, TGT_VM, TGT_WASM, TGT_BYTECODE } Target;

static int g_verbose = 0;

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "clc: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static int file_exists(const char *p) {
    STAT_T st;
    return STAT_FN(p, &st) == 0;
}

static void self_dir(const char *argv0, char *out, size_t cap) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", argv0);
    char *s = strrchr(buf, '/');
    char *b = strrchr(buf, '\\');
    char *cut = (s && (!b || s > b)) ? s : b;
    if (cut) { *cut = '\0'; snprintf(out, cap, "%s", buf); }
    else     { snprintf(out, cap, "."); }
}

/* Sibling-first tool resolution: if <bin_dir>/<name>[.exe] exists, use it;
   otherwise just the name (let the OS search PATH). */
static void tool_path(const char *bin_dir, const char *name, char *out, size_t cap) {
    char cand[1024];
    snprintf(cand, sizeof cand, "%s%c%s%s", bin_dir, PATH_SEP, name, EXE_EXT);
    if (file_exists(cand)) { snprintf(out, cap, "%s", cand); return; }
    snprintf(out, cap, "%s%s", name, EXE_EXT);
}

static int spawn_(char *const argv[]) {
    if (g_verbose) {
        fprintf(stderr, "+");
        for (int i = 0; argv[i]; i++) fprintf(stderr, " %s", argv[i]);
        fputc('\n', stderr);
    }
#ifdef _WIN32
    intptr_t rc = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
    if (rc < 0) { perror(argv[0]); return 1; }
    return (int)rc;
#else
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) { execvp(argv[0], argv); perror(argv[0]); _exit(127); }
    int st; if (waitpid(pid, &st, 0) < 0) { perror("waitpid"); return 1; }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
#endif
}

static const char *ext_of(const char *p) {
    const char *d = strrchr(p, '.');
    if (!d) return "";
    const char *s = strrchr(p, '/');
    const char *b = strrchr(p, '\\');
    const char *base = (s && (!b || s > b)) ? s : b;
    if (base && d < base) return "";
    return d;
}

static void basename_no_ext(const char *in, char *out, size_t cap) {
    const char *s = strrchr(in, '/');
    const char *b = strrchr(in, '\\');
    const char *start = (s && (!b || s > b)) ? s + 1 : (b ? b + 1 : in);
    const char *d = ext_of(start);
    size_t n = d && *d ? (size_t)(d - start) : strlen(start);
    if (n >= cap) n = cap - 1;
    memcpy(out, start, n);
    out[n] = '\0';
}

static void usage(int rc) {
    FILE *f = rc ? stderr : stdout;
    fprintf(f,
"clc — CalcLang compiler driver (v%s)\n"
"\n"
"Usage:\n"
"  clc [options] input.clc\n"
"\n"
"Targets (inferred from -o extension; override with -t):\n"
"  -o foo.exe        native executable (default if no -o)\n"
"  -o foo.wat        WebAssembly text\n"
"  -o foo.casm       bytecode only\n"
"  -t vm|native|wasm explicit target\n"
"\n"
"Modes:\n"
"  -c                compile to .casm, do not link or run\n"
"  -S                emit native assembly (.s)\n"
"  -r                run input.clc on the VM\n"
"  --lib             native library compile (no `main`, no top-level)\n"
"  --ast             dump AST and exit\n"
"\n"
"Other:\n"
"  -O0|-O1|-O2       optimization level (default -O1)\n"
"  -o <path>         output path\n"
"  -v                verbose: print every subprocess invocation\n"
"  -h, --help        this message\n"
"  -V, --version     version info\n"
"\n"
"Project subcommands:\n"
"  clc init [<dir>]    scaffold a new project (clc.toml + main.clc)\n"
"  clc install         fetch dependencies declared in clc.toml -> deps/\n"
"                      (writes clc.lock with pinned commit hashes)\n"
"\n"
"Examples:\n"
"  clc hello.clc                # native build -> hello.exe\n"
"  clc -o app src/main.clc      # native build -> app.exe\n"
"  clc -o app.wat src/main.clc  # wasm build\n"
"  clc -r script.clc            # compile + run on VM\n"
"  clc -S hot.clc               # emit hot.s assembly\n"
"  clc init my-app && cd my-app # start a new project\n"
"  clc install                  # fetch deps after editing clc.toml\n",
        CLC_VERSION);
    exit(rc);
}

static void version(void) {
    printf("clc %s — CalcLang compiler driver\n", CLC_VERSION);
    printf("targets: vm, native (x86-64), wasm\n");
    exit(0);
}

/* ===================================================================
 * Package manager v0.1.
 *
 * Manifest: clc.toml at project root. Dependencies are git URLs cloned
 * shallowly into deps/<name>/. After `clc install`, the parser probes
 * deps/<name>/<name>.clc when resolving `import "<name>"`.
 *
 * v0.1 deliberately omits: registry lookup, version constraints,
 * transitive dependency resolution. Git URL + optional tag/branch only.
 * ===================================================================*/

#define PKG_MAX_DEPS 64

typedef struct {
    char name[128];
    char git[512];
    char tag[128];           /* optional — empty means "default branch HEAD" */
} PkgDep;

typedef struct {
    char    name[128];
    char    version[64];
    PkgDep  deps[PKG_MAX_DEPS];
    int     n_deps;
} PkgManifest;

/* Strip leading/trailing whitespace from `s` in place. Returns the
   (possibly advanced) start pointer. */
static char *strip_ws(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
                     s[n-1] == '\r' || s[n-1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

/* Parse a TOML-subset key/value line of the form `key = "value"` into
   out_key and out_val (unquoted). Returns 1 on success, 0 if the line
   doesn't match (caller can skip). */
static int parse_kv(const char *line, char *out_key, size_t kcap,
                    char *out_val, size_t vcap) {
    const char *eq = strchr(line, '=');
    if (!eq) return 0;

    /* key: trim trailing whitespace before = */
    size_t klen = (size_t)(eq - line);
    while (klen > 0 && (line[klen-1] == ' ' || line[klen-1] == '\t')) klen--;
    /* skip leading whitespace */
    const char *kstart = line;
    while (*kstart == ' ' || *kstart == '\t') { kstart++; klen--; }
    if (klen == 0 || klen >= kcap) return 0;
    memcpy(out_key, kstart, klen);
    out_key[klen] = '\0';

    /* value: must be a double-quoted string for v0.1 */
    const char *p = eq + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    size_t vi = 0;
    while (*p && *p != '"' && vi + 1 < vcap) out_val[vi++] = *p++;
    if (*p != '"') return 0;
    out_val[vi] = '\0';
    return 1;
}

/* Parse [section.name] header. Returns 1 on success, 0 otherwise. */
static int parse_section(const char *line, char *out, size_t cap) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '[') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != ']' && i + 1 < cap) out[i++] = *p++;
    if (*p != ']') return 0;
    out[i] = '\0';
    return 1;
}

static int pkg_read_manifest(const char *path, PkgManifest *m) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(m, 0, sizeof *m);

    char line[2048];
    char section[256] = "";
    int  dep_idx = -1;

    while (fgets(line, sizeof line, f)) {
        char *s = strip_ws(line);
        if (*s == '\0' || *s == '#') continue;

        char sec[256];
        if (parse_section(s, sec, sizeof sec)) {
            cl_strncpy_z(section, sec, sizeof section);
            if (strncmp(section, "dependencies.", 13) == 0) {
                if (m->n_deps >= PKG_MAX_DEPS) {
                    fclose(f);
                    cl_die("too many dependencies (max " "64" ")");
                }
                dep_idx = m->n_deps++;
                cl_strncpy_z(m->deps[dep_idx].name, section + 13,
                             sizeof m->deps[dep_idx].name);
            } else {
                dep_idx = -1;
            }
            continue;
        }

        char k[128], v[1024];
        if (!parse_kv(s, k, sizeof k, v, sizeof v)) continue;

        if (*section == '\0') {
            if      (!strcmp(k, "name"))    cl_strncpy_z(m->name, v, sizeof m->name);
            else if (!strcmp(k, "version")) cl_strncpy_z(m->version, v, sizeof m->version);
        } else if (dep_idx >= 0) {
            if      (!strcmp(k, "git")) cl_strncpy_z(m->deps[dep_idx].git, v, sizeof m->deps[dep_idx].git);
            else if (!strcmp(k, "tag")) cl_strncpy_z(m->deps[dep_idx].tag, v, sizeof m->deps[dep_idx].tag);
        }
    }
    fclose(f);
    return 0;
}

/* Derive a default project name from a directory path (basename). */
static void default_proj_name(const char *dir, char *out, size_t cap) {
    char buf[1024]; cl_strncpy_z(buf, dir, sizeof buf);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '/' || buf[n-1] == '\\')) buf[--n] = '\0';
    const char *s = strrchr(buf, '/');
    const char *b = strrchr(buf, '\\');
    const char *base = (s && (!b || s > b)) ? s + 1 : (b ? b + 1 : buf);
    if (!*base || !strcmp(base, ".")) base = "my-project";
    cl_strncpy_z(out, base, cap);
}

static int pkg_init(int argc, char **argv) {
    const char *dir = (argc >= 2) ? argv[1] : ".";
    MKDIR(dir);

    char manifest_path[1024], main_path[1024];
    snprintf(manifest_path, sizeof manifest_path, "%s/clc.toml", dir);
    snprintf(main_path,     sizeof main_path,     "%s/main.clc",  dir);

    if (file_exists(manifest_path)) {
        fprintf(stderr, "clc init: %s already exists\n", manifest_path);
        return 1;
    }

    char proj[128];
    default_proj_name(dir, proj, sizeof proj);

    FILE *f = fopen(manifest_path, "w");
    if (!f) { perror(manifest_path); return 1; }
    fprintf(f,
"# clc.toml — project manifest. See `clc --help` and the CalcLang book\n"
"# for the package format.\n"
"\n"
"name = \"%s\"\n"
"version = \"0.1.0\"\n"
"\n"
"# Declare dependencies as [dependencies.<name>] sections:\n"
"#\n"
"#   [dependencies.vec-extras]\n"
"#   git = \"https://github.com/user/calclang-vec-extras.git\"\n"
"#   tag = \"v1.0.0\"          # optional; default is the default branch\n"
"#\n"
"# Then run `clc install` to fetch into deps/, and `import \"vec-extras\"`\n"
"# from your CalcLang source will resolve against deps/vec-extras/vec-extras.clc.\n",
        proj);
    fclose(f);

    if (!file_exists(main_path)) {
        f = fopen(main_path, "w");
        if (!f) { perror(main_path); return 1; }
        fprintf(f,
"// main.clc — entry point for project `%s`.\n"
"//\n"
"// Build native:  clc main.clc -o main.exe\n"
"// Run on VM:     clc -r main.clc\n"
"\n"
"print \"hello from %s\";\n",
            proj, proj);
        fclose(f);
    }

    printf("Created %s\n", manifest_path);
    printf("Created %s\n", main_path);
    printf("\nNext steps:\n");
    printf("  edit %s to declare dependencies\n", manifest_path);
    printf("  clc install                    # fetch deps into deps/\n");
    printf("  clc -r %s              # run\n", main_path);
    return 0;
}

static int pkg_install(int argc, char **argv) {
    (void)argc; (void)argv;
    PkgManifest m;
    if (pkg_read_manifest("clc.toml", &m) < 0) {
        fprintf(stderr, "clc install: no clc.toml in current directory\n");
        fprintf(stderr, "             (run `clc init` first)\n");
        return 1;
    }
    if (m.n_deps == 0) {
        printf("clc install: no dependencies declared in clc.toml.\n");
        return 0;
    }
    MKDIR("deps");

    FILE *lf = fopen("clc.lock", "w");
    if (!lf) { perror("clc.lock"); return 1; }
    fprintf(lf,
        "# Auto-generated by `clc install`. Do not edit by hand; rerun\n"
        "# `clc install` to refresh.\n\n");

    int ok = 0;
    for (int i = 0; i < m.n_deps; i++) {
        PkgDep *d = &m.deps[i];
        if (!*d->git) {
            fprintf(stderr, "clc install: %s missing 'git' URL\n", d->name);
            fclose(lf);
            return 1;
        }
        char dep_dir[768];
        snprintf(dep_dir, sizeof dep_dir, "deps/%s", d->name);

        if (file_exists(dep_dir)) {
            printf("  %-24s already present (skipping clone)\n", d->name);
        } else {
            if (*d->tag) {
                printf("  %-24s clone %s @ %s\n", d->name, d->git, d->tag);
            } else {
                printf("  %-24s clone %s\n", d->name, d->git);
            }
            char cmd[2560];
            if (*d->tag) {
                snprintf(cmd, sizeof cmd,
                    "git clone --depth 1 --branch \"%s\" \"%s\" \"%s\"",
                    d->tag, d->git, dep_dir);
            } else {
                snprintf(cmd, sizeof cmd,
                    "git clone --depth 1 \"%s\" \"%s\"",
                    d->git, dep_dir);
            }
            int rc = system(cmd);
            if (rc != 0) {
                fprintf(stderr, "clc install: git clone failed for %s (exit %d)\n", d->name, rc);
                fclose(lf);
                return 1;
            }
        }

        /* Capture HEAD commit hash for the lockfile. Best-effort — if
           popen / git fail we omit the commit field. */
        char hash[80] = "";
        char rev_cmd[1024];
        snprintf(rev_cmd, sizeof rev_cmd,
            "git -C \"%s\" rev-parse HEAD", dep_dir);
#ifdef _WIN32
        FILE *p = _popen(rev_cmd, "r");
#else
        FILE *p = popen(rev_cmd, "r");
#endif
        if (p) {
            if (fgets(hash, sizeof hash, p)) {
                size_t hl = strlen(hash);
                while (hl > 0 && (hash[hl-1] == '\n' || hash[hl-1] == '\r'))
                    hash[--hl] = '\0';
            }
#ifdef _WIN32
            _pclose(p);
#else
            pclose(p);
#endif
        }

        fprintf(lf, "[dependencies.%s]\n", d->name);
        fprintf(lf, "git = \"%s\"\n", d->git);
        if (*d->tag) fprintf(lf, "tag = \"%s\"\n", d->tag);
        if (*hash)   fprintf(lf, "commit = \"%s\"\n", hash);
        fprintf(lf, "\n");
        ok++;
    }
    fclose(lf);

    printf("\nInstalled %d dependency(ies). Lockfile: clc.lock\n", ok);
    return 0;
}

int main(int argc, char **argv) {
    /* Subcommand dispatch — runs BEFORE the gcc-style flag parsing so
       `clc init`, `clc install`, etc. don't collide with the compile
       path. Anything else falls through to the compile flow below. */
    if (argc >= 2) {
        if (!strcmp(argv[1], "init"))    return pkg_init(argc - 1, argv + 1);
        if (!strcmp(argv[1], "install")) return pkg_install(argc - 1, argv + 1);
    }

    const char *input  = NULL;
    const char *output = NULL;
    Target target = TGT_NONE;
    int opt_level = 1;
    int emit_S = 0, emit_c = 0, run_vm = 0, ast = 0, lib_mode = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "-h") || !strcmp(a, "--help"))    usage(0);
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) version();
        else if (!strcmp(a, "-v"))     g_verbose = 1;
        else if (!strcmp(a, "-S"))     emit_S = 1;
        else if (!strcmp(a, "-c"))     emit_c = 1;
        else if (!strcmp(a, "-r"))     run_vm = 1;
        else if (!strcmp(a, "--ast"))  ast = 1;
        else if (!strcmp(a, "--lib"))  lib_mode = 1;
        else if (!strcmp(a, "-O0"))    opt_level = 0;
        else if (!strcmp(a, "-O1"))    opt_level = 1;
        else if (!strcmp(a, "-O2"))    opt_level = 2;
        else if (!strcmp(a, "-o")) {
            if (++i >= argc) die("-o needs an argument");
            output = argv[i];
        }
        else if (!strcmp(a, "-t") || !strcmp(a, "--target")) {
            if (++i >= argc) die("%s needs an argument", a);
            const char *t = argv[i];
            if      (!strcmp(t, "vm"))     target = TGT_VM;
            else if (!strcmp(t, "native")) target = TGT_NATIVE;
            else if (!strcmp(t, "wasm"))   target = TGT_WASM;
            else die("unknown target: %s (expected vm|native|wasm)", t);
        }
        else if (a[0] == '-') die("unknown option: %s", a);
        else {
            if (input) die("multiple inputs given (%s and %s)", input, a);
            input = a;
        }
    }
    if (!input) usage(1);

    /* Infer target: -t > -r > -S/-c > -o ext > native. */
    if (target == TGT_NONE) {
        if      (run_vm)  target = TGT_VM;
        else if (emit_S)  target = TGT_NATIVE;
        else if (emit_c)  target = TGT_BYTECODE;
        else if (output) {
            const char *e = ext_of(output);
            if      (!strcmp(e, ".wat"))  target = TGT_WASM;
            else if (!strcmp(e, ".casm")) target = TGT_BYTECODE;
            else                          target = TGT_NATIVE;
        }
        else target = TGT_NATIVE;
    }

    char bin_dir[1024];
    self_dir(argv[0], bin_dir, sizeof bin_dir);

    char calcc_p[1024], calcasm_p[1024], calcld_p[1024], calcvm_p[1024];
    char calcnat_p[1024], calcwasm_p[1024];
    tool_path(bin_dir, "calcc",    calcc_p,    sizeof calcc_p);
    tool_path(bin_dir, "calcasm",  calcasm_p,  sizeof calcasm_p);
    tool_path(bin_dir, "calcld",   calcld_p,   sizeof calcld_p);
    tool_path(bin_dir, "calcvm",   calcvm_p,   sizeof calcvm_p);
    tool_path(bin_dir, "calcnat",  calcnat_p,  sizeof calcnat_p);
    tool_path(bin_dir, "calcwasm", calcwasm_p, sizeof calcwasm_p);

    char stem[1024];
    basename_no_ext(input, stem, sizeof stem);

    /* Pass -O level to child tools via env (they don't all consume it
       yet — wiring is a future task; the env var is the seam). */
    static char opt_env[32];
    snprintf(opt_env, sizeof opt_env, "CLC_OPT=%d", opt_level);
    putenv(opt_env);

    if (target == TGT_BYTECODE) {
        char out_path[1024];
        if (output) snprintf(out_path, sizeof out_path, "%s", output);
        else        snprintf(out_path, sizeof out_path, "%s.casm", stem);
        if (ast) {
            char *args[] = { calcc_p, "--ast", (char*)input, out_path, NULL };
            return spawn_(args);
        }
        char *args[] = { calcc_p, (char*)input, out_path, NULL };
        return spawn_(args);
    }

    if (target == TGT_WASM) {
        char out_path[1024];
        if (output) snprintf(out_path, sizeof out_path, "%s", output);
        else        snprintf(out_path, sizeof out_path, "%s.wat", stem);
        char *args[] = { calcwasm_p, (char*)input, "-o", out_path, NULL };
        return spawn_(args);
    }

    if (target == TGT_NATIVE) {
        char out_path[1024];
        if (output) {
            /* If user gave -o without an extension, append .exe on Windows. */
            const char *e = ext_of(output);
            if (!*e && *EXE_EXT) snprintf(out_path, sizeof out_path, "%s%s", output, EXE_EXT);
            else                 snprintf(out_path, sizeof out_path, "%s", output);
        }
        else if (emit_S) snprintf(out_path, sizeof out_path, "%s.s", stem);
        else             snprintf(out_path, sizeof out_path, "%s%s", stem, EXE_EXT);

        char *args[16]; int n = 0;
        args[n++] = calcnat_p;
        if (lib_mode) args[n++] = "--lib";
        if (emit_S)   args[n++] = "-S";
        if (ast)      args[n++] = "--ast";
        args[n++] = (char*)input;
        args[n++] = "-o";
        args[n++] = out_path;
        args[n]   = NULL;
        return spawn_(args);
    }

    /* TGT_VM: calcc -> calcasm -> calcld, then run if -r. */
    MKDIR("build");
    char casm[1024], co[1024], cexe[1024];
    snprintf(casm, sizeof casm, "build%c%s.casm", PATH_SEP, stem);
    snprintf(co,   sizeof co,   "build%c%s.co",   PATH_SEP, stem);
    snprintf(cexe, sizeof cexe, "build%c%s.cexe", PATH_SEP, stem);
    if (output) snprintf(cexe, sizeof cexe, "%s", output);

    if (ast) {
        char *args[] = { calcc_p, "--ast", (char*)input, casm, NULL };
        return spawn_(args);
    }
    { char *args[] = { calcc_p,   (char*)input, casm, NULL }; int r = spawn_(args); if (r) return r; }
    { char *args[] = { calcasm_p, casm, co,           NULL }; int r = spawn_(args); if (r) return r; }
    { char *args[] = { calcld_p,  co,   cexe,         NULL }; int r = spawn_(args); if (r) return r; }
    if (run_vm) {
        char *args[] = { calcvm_p, cexe, NULL };
        return spawn_(args);
    }
    return 0;
}
