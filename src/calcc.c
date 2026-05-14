#include "codegen.h"
#include "type_infer.h"
#include "optimizer.h"

static void usage(void) {
    fprintf(stderr, "usage: calcc [--ast] input.calc output.casm\n");
    exit(1);
}

int main(int argc, char **argv) {
    int show_ast = 0;
    const char *in = NULL, *out = NULL;
    if (argc == 4 && strcmp(argv[1], "--ast") == 0) {
        show_ast = 1; in = argv[2]; out = argv[3];
    } else if (argc == 3) {
        in = argv[1]; out = argv[2];
    } else {
        usage();
    }

    char *src = cl_read_file(in);
    Parser p;
    parser_init_with_path(&p, src, in);
    Program prog = parser_parse_program(&p);
    cl_optimize_program(&prog);
    infer_program_types(&prog);
    if (show_ast) {
        for (int i = 0; i < prog.count; i++) ast_print_debug(prog.items[i], 0);
    }
    char *asm_text = codegen_program(&prog);
    cl_write_text_file(out, asm_text);

    cl_track_free(asm_text);
    program_free(&prog);
    cl_track_free(src);
    /* atexit cleanup would catch any stragglers; we free explicitly so
       leak detectors stay happy and the work is auditable in source. */
    return 0;
}
