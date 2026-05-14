#include "parser.h"
#include "codegen_x64.h"
#include "type_infer.h"
#include "optimizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
   calcnat — CalcLang native compiler driver.

   Modes:
     calcnat foo.calc                  -> foo.exe  (full build via gcc)
     calcnat foo.calc -o out.exe       -> out.exe  (explicit output)
     calcnat -S foo.calc out.s         -> assembly only
     calcnat --lib foo.calc out.s      -> assembly only, library mode
                                            (no `main`, no top-level)
     calcnat --lib foo.calc -o out.s   -> same; `-o` accepted everywhere

   In full-build mode the driver invokes the system `gcc` to assemble
   the generated `.s` and link it against `src/runtime_x64.c` from
   the project tree. Run calcnat from the repository root so the
   runtime is found. Override via the CALC_RUNTIME environment
   variable if you've installed CalcLang elsewhere.

   For multi-file builds, compile each unit with `--lib -o foo.s`,
   then pass all the `.s` files plus the runtime to gcc yourself, OR
   use `calcnat main.calc lib1.s lib2.s -o app.exe` — extra arguments
   ending in `.s`, `.o`, or `.c` are forwarded to gcc unchanged. */

static void usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  calcnat <input.calc> [-o <out>] [extra files...]\n"
        "  calcnat -S <input.calc> <out.s>\n"
        "  calcnat --lib <input.calc> [-o <out.s>]\n"
        "\n"
        "By default, produces an executable (input.calc -> input.exe).\n"
        "Use -S to emit assembly only, or --lib for library compilation\n"
        "(no `main` / no top-level code; output defaults to input.s).\n");
    exit(1);
}

/* Strip a trailing extension from `s` (in place). */
static void strip_ext(char *s) {
    char *dot = strrchr(s, '.');
    char *slash1 = strrchr(s, '/');
    char *slash2 = strrchr(s, '\\');
    char *slash = slash1 > slash2 ? slash1 : slash2;
    if (dot && (!slash || dot > slash)) *dot = '\0';
}

static int has_ext(const char *s, const char *ext) {
    size_t ls = strlen(s), le = strlen(ext);
    return ls >= le && strcmp(s + ls - le, ext) == 0;
}

int main(int argc, char **argv) {
    int  library_mode = 0;
    int  asm_only     = 0;
    const char *in    = NULL;
    const char *out   = NULL;
    /* Extra `.s`/`.o`/`.c` files forwarded verbatim to gcc when we
       link an executable. */
    const char *extras[32];
    int   extra_count = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--lib") == 0)        { library_mode = 1; asm_only = 1; }
        else if (strcmp(a, "-S") == 0)      { asm_only = 1; }
        else if (strcmp(a, "-o") == 0) {
            if (++i >= argc) usage();
            out = argv[i];
        } else if (a[0] == '-')             { usage(); }
        else if (has_ext(a, ".calc")) {
            if (in) usage();
            in = a;
        } else if (has_ext(a, ".s") || has_ext(a, ".o") || has_ext(a, ".c")) {
            /* Either an explicit asm output (when 2-arg legacy form)
               or an extra file to forward to gcc. We decide below. */
            if (asm_only && !out) {
                out = a;   /* legacy: `calcnat -S foo.calc out.s` */
            } else {
                if (extra_count >= 32) cl_die("too many extra files");
                extras[extra_count++] = a;
            }
        } else {
            /* Treat as the output if not yet set, else as extra. */
            if (!out) out = a; else extras[extra_count++] = a;
        }
    }
    if (!in) usage();

    /* Backward-compat: the original `calcnat in.calc out.s` form
       (a single positional that ends in .s with no flags) means
       "emit assembly to out.s". If we collected exactly one .s extra
       and nothing requested an exe, fold it into asm-only mode. */
    if (!asm_only && !out && extra_count == 1
        && has_ext(extras[0], ".s")) {
        out = extras[0];
        extra_count = 0;
        asm_only = 1;
    }

    /* Compute default output if not supplied. */
    char default_out[1024];
    if (!out) {
        cl_strncpy_z(default_out, in, sizeof(default_out));
        strip_ext(default_out);
        strcat(default_out, asm_only ? ".s" : ".exe");
        out = default_out;
    }

    /* Compile to assembly. */
    char *src = cl_read_file(in);
    Parser p;
    parser_init_with_path(&p, src, in);
    Program prog = parser_parse_program(&p);
    cl_optimize_program(&prog);
    infer_program_types(&prog);
    char *asm_text = codegen_x64_program_ex(&prog, library_mode);

    /* If the caller wants assembly directly, write it and stop. */
    if (asm_only) {
        cl_write_text_file(out, asm_text);
        return 0;
    }

    /* Full build: write asm to a temp file next to the output, then
       invoke gcc to assemble + link with the runtime. */
    char asm_path[1100];
    int wrote = snprintf(asm_path, sizeof(asm_path), "%s.s", out);
    if (wrote < 0 || (size_t)wrote >= sizeof(asm_path)) cl_die("output path too long");
    cl_write_text_file(asm_path, asm_text);

    const char *rt_dir = getenv("CALC_RUNTIME");
    if (!rt_dir || !*rt_dir) rt_dir = "src/runtime_x64.c";
    const char *include_dir = getenv("CALC_INCLUDE");
    if (!include_dir || !*include_dir) include_dir = "include";

    /* GUI support: if the vendored SDL2 tree exists, we always link
       SDL2 + the GUI runtime module. The dead-code stripper drops the
       GUI fns when the program doesn't use them, so non-GUI builds
       carry no runtime cost beyond a slightly slower link. Set
       CALC_NO_GUI=1 to opt out (useful if you've moved SDL2 or are
       building without it). */
    const char *no_gui = getenv("CALC_NO_GUI");
    int gui_enabled = (!no_gui || !*no_gui);
    const char *sdl_root = "third_party/SDL2-2.30.10/x86_64-w64-mingw32";
    /* Existence check on libSDL2.dll.a — present when setup is done. */
    char sdl_lib_probe[512];
    snprintf(sdl_lib_probe, sizeof sdl_lib_probe,
        "%s/lib/libSDL2.dll.a", sdl_root);
    FILE *probe = fopen(sdl_lib_probe, "rb");
    if (probe) fclose(probe);
    else gui_enabled = 0;

    char cmd[8192];
    int  off = 0;
    off += snprintf(cmd + off, sizeof(cmd) - off,
        "gcc -O0 -I%s \"%s\" \"%s\"", include_dir, asm_path, rt_dir);
    if (gui_enabled) {
        off += snprintf(cmd + off, sizeof(cmd) - off,
            " src/runtime_gui_sdl2.c -I%s/include -L%s/lib -lmingw32 -lSDL2main -lSDL2",
            sdl_root, sdl_root);
    }
    for (int i = 0; i < extra_count; i++) {
        off += snprintf(cmd + off, sizeof(cmd) - off, " \"%s\"", extras[i]);
    }
    off += snprintf(cmd + off, sizeof(cmd) - off, " -o \"%s\"", out);

    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "calcnat: gcc failed (%d): %s\n", rc, cmd);
        exit(1);
    }
    /* Copy SDL2.dll next to the output .exe so GUI programs Just Run.
       Done via plain C file I/O so we don't depend on `cp` (mingw) vs
       `copy` (cmd.exe) shell-builtin availability. */
    if (gui_enabled) {
        char src_dll[600], dst_dll[700];
        snprintf(src_dll, sizeof src_dll, "%s/bin/SDL2.dll", sdl_root);
        /* Strip filename component from `out` to get the dest dir. */
        char out_dir[600];
        cl_strncpy_z(out_dir, out, sizeof out_dir);
        char *s1 = strrchr(out_dir, '/');
        char *s2 = strrchr(out_dir, '\\');
        char *s  = s1 > s2 ? s1 : s2;
        if (s) *s = '\0'; else cl_strncpy_z(out_dir, ".", sizeof out_dir);
        snprintf(dst_dll, sizeof dst_dll, "%s/SDL2.dll", out_dir);
        FILE *fi = fopen(src_dll, "rb");
        if (fi) {
            FILE *fo = fopen(dst_dll, "wb");
            if (fo) {
                char buf[16 * 1024];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, fi)) > 0) {
                    fwrite(buf, 1, n, fo);
                }
                fclose(fo);
            }
            fclose(fi);
        }
    }
    return 0;
}
