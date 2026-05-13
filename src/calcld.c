#include "common.h"
#include "insn.h"

typedef struct {
    char name[CL_MAX_TEXT];
    int  addr;
} Sym;

typedef struct {
    Insn code[CL_MAX_CODE];
    int  code_count;
    int  var_count;
    Sym  syms[CL_MAX_SYMBOLS];        /* exported (global) labels */
    int  sym_count;
    Sym  local_syms[CL_MAX_SYMBOLS];  /* file-local labels (".L*") */
    int  local_sym_count;
    char *strings[CL_MAX_STRINGS];
    int  string_count;
} Obj;

static Obj *read_obj(const char *path) {
    Obj *o = (Obj *)cl_track_calloc(sizeof(Obj));
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }

    char tag[64];
    if (fscanf(f, "%63s", tag) != 1 || strcmp(tag, "COBJ4") != 0) {
        fclose(f);
        cl_die("not a COBJ4 object (regenerate with the current assembler)");
    }
    if (fscanf(f, "%63s %d", tag, &o->var_count) != 2 || strcmp(tag, "VARCOUNT") != 0) {
        fclose(f);
        cl_die("bad object VARCOUNT");
    }
    if (fscanf(f, "%63s %d", tag, &o->string_count) != 2 || strcmp(tag, "STRINGS") != 0) {
        fclose(f);
        cl_die("bad object STRINGS");
    }
    if (o->string_count < 0 || o->string_count > CL_MAX_STRINGS) {
        fclose(f);
        cl_die("object string_count out of range");
    }
    for (int i = 0; i < o->string_count; i++) {
        if (fscanf(f, "%63s", tag) != 1 || strcmp(tag, "S") != 0) {
            fclose(f);
            cl_die("bad string record");
        }
        char buf[CL_MAX_TEXT];
        cl_fread_quoted(f, buf, sizeof(buf));
        o->strings[i] = cl_track_strdup(buf);
    }
    if (fscanf(f, "%63s %d", tag, &o->code_count) != 2 || strcmp(tag, "CODE") != 0) {
        fclose(f);
        cl_die("bad object CODE");
    }
    if (o->var_count < 0 || o->var_count > CL_MAX_SYMBOLS) {
        fclose(f);
        cl_die("object var_count out of range");
    }
    if (o->code_count < 0 || o->code_count > CL_MAX_CODE) {
        fclose(f);
        cl_die("object code_count out of range");
    }

    for (int i = 0; i < o->code_count; i++) {
        char sym[CL_MAX_TEXT];
        int got = fscanf(f, "%63s %d %lf %d %d %127s",
            tag, &o->code[i].op, &o->code[i].farg,
            &o->code[i].iarg, &o->code[i].has_sym, sym);
        if (got != 6 || strcmp(tag, "I") != 0) {
            fclose(f);
            cl_die("bad instruction record");
        }
        if (strcmp(sym, "_") != 0) {
            cl_strncpy_z(o->code[i].sym, sym, CL_MAX_TEXT);
        }
    }

    if (fscanf(f, "%63s %d", tag, &o->sym_count) != 2 || strcmp(tag, "SYMS") != 0) {
        fclose(f);
        cl_die("bad object SYMS");
    }
    if (o->sym_count < 0 || o->sym_count > CL_MAX_SYMBOLS) {
        fclose(f);
        cl_die("object sym_count out of range");
    }
    for (int i = 0; i < o->sym_count; i++) {
        char name[CL_MAX_TEXT];
        int got = fscanf(f, "%63s %127s %d", tag, name, &o->syms[i].addr);
        if (got != 3 || strcmp(tag, "S") != 0) {
            fclose(f);
            cl_die("bad symbol record");
        }
        cl_strncpy_z(o->syms[i].name, name, CL_MAX_TEXT);
    }

    if (fscanf(f, "%63s %d", tag, &o->local_sym_count) != 2 || strcmp(tag, "LOCALSYMS") != 0) {
        fclose(f);
        cl_die("bad object LOCALSYMS");
    }
    if (o->local_sym_count < 0 || o->local_sym_count > CL_MAX_SYMBOLS) {
        fclose(f);
        cl_die("object local_sym_count out of range");
    }
    for (int i = 0; i < o->local_sym_count; i++) {
        char name[CL_MAX_TEXT];
        int got = fscanf(f, "%63s %127s %d", tag, name, &o->local_syms[i].addr);
        if (got != 3 || strcmp(tag, "L") != 0) {
            fclose(f);
            cl_die("bad local symbol record");
        }
        cl_strncpy_z(o->local_syms[i].name, name, CL_MAX_TEXT);
    }

    fclose(f);
    return o;
}

static int find_in(Sym *arr, int n, const char *name) {
    for (int i = 0; i < n; i++) {
        if (!strcmp(arr[i].name, name)) return arr[i].addr;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: calcld input1.co [input2.co ...] output.cexe\n");
        return 1;
    }
    const char *outpath = argv[argc - 1];

    Insn  *linked       = (Insn  *)cl_track_calloc(sizeof(Insn) * CL_MAX_CODE);
    Sym   *globals      = (Sym   *)cl_track_calloc(sizeof(Sym)  * CL_MAX_GLOBALS);
    char **strings      = (char **)cl_track_calloc(sizeof(char*) * CL_MAX_STRINGS);
    int    linked_count = 0;
    int    global_count = 0;
    int    total_vars   = 0;
    int    total_strs   = 0;

    for (int a = 1; a < argc - 1; a++) {
        Obj *o = read_obj(argv[a]);
        int base        = linked_count;
        int slot_offset = total_vars;
        int str_offset  = total_strs;

        /* Concatenate this object's string pool onto the merged pool.
           PUSH_STR indices in this object's code shift by str_offset. */
        if (total_strs + o->string_count > CL_MAX_STRINGS) cl_die("too many linked strings");
        for (int i = 0; i < o->string_count; i++) {
            strings[total_strs++] = o->strings[i];
        }

        for (int i = 0; i < o->sym_count; i++) {
            if (global_count >= CL_MAX_GLOBALS) cl_die("too many global symbols");
            if (find_in(globals, global_count, o->syms[i].name) >= 0) {
                fprintf(stderr, "linker error: duplicate symbol %s\n", o->syms[i].name);
                return 1;
            }
            cl_strncpy_z(globals[global_count].name, o->syms[i].name, CL_MAX_TEXT);
            globals[global_count].addr = base + o->syms[i].addr;
            global_count++;
        }

        for (int i = 0; i < o->code_count; i++) {
            if (linked_count >= CL_MAX_CODE) cl_die("linked code too large");
            Insn ins = o->code[i];

            /* LOAD/STORE slot operands are relative to this object's
               variable space; shift them into the merged space. */
            if (ins.op == OP_LOAD || ins.op == OP_STORE) {
                ins.iarg += slot_offset;
            }
            /* PUSH_STR indices are relative to this object's string
               pool; shift into the merged pool. */
            if (ins.op == OP_PUSH_STR) {
                ins.iarg += str_offset;
            }

            /* Resolve local labels now, while we know this object's
               base. Local labels do not enter the global registry and
               cannot satisfy references from a different object. */
            if (ins.has_sym) {
                int local_addr = find_in(o->local_syms, o->local_sym_count, ins.sym);
                if (local_addr >= 0) {
                    ins.iarg = base + local_addr;
                    ins.has_sym = 0;
                }
            }

            linked[linked_count++] = ins;
        }

        total_vars += o->var_count;
        cl_track_free(o);
    }

    /* Second pass: anything still has_sym=1 must resolve through the
       merged global symbol table. */
    for (int i = 0; i < linked_count; i++) {
        if (linked[i].has_sym) {
            int addr = find_in(globals, global_count, linked[i].sym);
            if (addr < 0) {
                fprintf(stderr, "linker error: unresolved symbol %s\n", linked[i].sym);
                return 1;
            }
            linked[i].iarg = addr;
            linked[i].has_sym = 0;
        }
    }

    FILE *f = fopen(outpath, "wb");
    if (!f) { perror(outpath); return 1; }
    fprintf(f, "CEXE3\nVARCOUNT %d\nSTRINGS %d\n", total_vars, total_strs);
    for (int i = 0; i < total_strs; i++) {
        fputs("S ", f);
        cl_fputs_quoted(f, strings[i]);
        fputc('\n', f);
    }
    fprintf(f, "CODE %d\n", linked_count);
    for (int i = 0; i < linked_count; i++) {
        fprintf(f, "I %d %.17g %d\n", linked[i].op, linked[i].farg, linked[i].iarg);
    }
    if (fclose(f) != 0) cl_die("failed to close output file");

    cl_track_free(linked);
    cl_track_free(globals);
    cl_track_free(strings);
    return 0;
}
