#ifndef CALCLANG_COMMON_H
#define CALCLANG_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>

#define CL_MAX_TEXT 128
#define CL_MAX_SYMBOLS 1024
#define CL_MAX_NODES 4096
#define CL_MAX_CODE 8192
#define CL_MAX_RELOCS 1024
#define CL_MAX_GLOBALS 1024
#define CL_MAX_STRINGS 512

_Noreturn void cl_die(const char *msg);
_Noreturn void cl_die_at(int line, int col, const char *fmt, ...);

void *cl_track_malloc(size_t n);
void *cl_track_calloc(size_t n);
void *cl_track_realloc(void *old, size_t n);
void  cl_track_free(void *p);

void cl_strncpy_z(char *dst, const char *src, size_t dst_size);
char *cl_track_strdup(const char *s);

char *cl_read_file(const char *path);
void  cl_write_text_file(const char *path, const char *text);

/* Derive the CalcLang install root from argv[0] and seed two env vars
   so the toolchain works from any CWD (system-installed or not):

     CALC_HOME     = <bin_dir>/..           (umbrella: stdlib, runtime srcs,
                                              include/, vendored SDL2 are all
                                              found relative to this)
     CALC_LIB_PATH = <bin_dir>/../lib       (stdlib search path the parser's
                                              `import` resolution falls back to)

   Each is set only if not already in the environment, so users can
   override individually. No-op if argv0 has no directory component
   (e.g. invoked via PATH search) — relies on existing CWD-relative
   defaults in that case. Safe to call multiple times; call once at
   main() entry. */
void cl_init_install_paths(const char *argv0);

/* Write `s` to `f` as a double-quoted, escape-encoded string. Used by
   every stage that produces a text-based file with embedded strings. */
void cl_fputs_quoted(FILE *f, const char *s);

/* Read a double-quoted, escape-encoded string from `f` into `out`.
   Skips leading whitespace, expects the opening quote, unescapes, and
   consumes the closing quote. Returns the string length. Dies on
   malformed input. */
int  cl_fread_quoted(FILE *f, char *out, size_t out_size);

/* Same but reads from an in-memory string, advancing *pp past the
   closing quote. */
int  cl_parse_quoted(const char **pp, char *out, size_t out_size);

#endif
