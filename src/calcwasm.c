#include "parser.h"
#include "type_infer.h"
#include "optimizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
   calcwasm — WebAssembly text-format driver (Stage 1, numeric subset).

   Modes:
     calcwasm foo.clc                   -> foo.wat
     calcwasm foo.clc -o out.wat        -> out.wat

   The output is a syntactically valid `.wat` (WebAssembly text format)
   module. To execute it you'll need a Wasm runtime — wasmtime, wasmer,
   or the Python `wasmtime` package — plus a host that supplies the
   imports (print_num + math). See `examples/wasm/run.py` for a
   reference Python host.
*/

const char *codegen_wasm(const Program *prog);

static void usage(void) {
    fprintf(stderr,
        "usage: calcwasm <input.clc> [-o <out.wat>]\n"
        "\n"
        "Emits a .wat (WebAssembly text format) module covering the\n"
        "numeric subset of CalcLang. Run with wasmtime/wasmer/etc.\n");
    exit(1);
}

static char *replace_ext(const char *path, const char *new_ext) {
    size_t plen = strlen(path);
    size_t elen = strlen(new_ext);
    char *out = (char *)cl_track_malloc(plen + elen + 2);
    cl_strncpy_z(out, path, plen + 1);
    char *dot = strrchr(out, '.');
    char *slash1 = strrchr(out, '/');
    char *slash2 = strrchr(out, '\\');
    char *slash  = slash1 > slash2 ? slash1 : slash2;
    if (dot && (!slash || dot > slash)) *dot = '\0';
    strncat(out, new_ext, elen);
    return out;
}

int main(int argc, char **argv) {
    cl_init_install_paths(argv[0]);
    const char *input = NULL;
    const char *output = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "calcwasm: unknown option '%s'\n", argv[i]);
            usage();
        } else if (!input) {
            input = argv[i];
        } else {
            fprintf(stderr, "calcwasm: extra argument '%s'\n", argv[i]);
            usage();
        }
    }
    if (!input) usage();

    char *src = cl_read_file(input);
    Parser p;
    parser_init_with_path(&p, src, input);
    Program prog = parser_parse_program(&p);

    /* Type inference + optimizer are safe to run for the subset we
       support — they only annotate AST nodes and rewrite expressions. */
    infer_program_types(&prog);
    cl_optimize_program(&prog);

    const char *wat = codegen_wasm(&prog);

    char *out_path = output ? (char *)output : replace_ext(input, ".wat");
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "calcwasm: cannot open '%s' for writing\n", out_path);
        return 1;
    }
    fwrite(wat, 1, strlen(wat), f);
    fclose(f);
    fprintf(stderr, "calcwasm: wrote %s\n", out_path);
    return 0;
}
