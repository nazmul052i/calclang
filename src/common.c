#include "common.h"

/* Tracked heap allocations form a doubly-linked list. Every cl_track_*
   allocation is recorded; an atexit handler frees anything still live
   when the program exits. This means callers that bail out via cl_die
   (which calls exit) do not leak: the atexit handler still fires.

   Layout of each allocation:
     [ TAlloc header | user bytes ... ]
   We return the address right after the header. */

typedef struct TAlloc {
    struct TAlloc *prev;
    struct TAlloc *next;
} TAlloc;

static TAlloc *g_alloc_head = NULL;
static int     g_atexit_registered = 0;

static void cl_cleanup_all(void) {
    TAlloc *n = g_alloc_head;
    while (n) {
        TAlloc *next = n->next;
        free(n);
        n = next;
    }
    g_alloc_head = NULL;
}

static void ensure_atexit(void) {
    if (!g_atexit_registered) {
        if (atexit(cl_cleanup_all) == 0) {
            g_atexit_registered = 1;
        }
    }
}

_Noreturn void cl_die(const char *msg) {
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

_Noreturn void cl_die_at(int line, int col, const char *fmt, ...) {
    fprintf(stderr, "error at %d:%d: ", line, col);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void *cl_track_malloc(size_t n) {
    ensure_atexit();
    TAlloc *h = (TAlloc *)malloc(sizeof(TAlloc) + n);
    if (!h) cl_die("out of memory");
    h->prev = NULL;
    h->next = g_alloc_head;
    if (g_alloc_head) g_alloc_head->prev = h;
    g_alloc_head = h;
    return (void *)(h + 1);
}

void *cl_track_calloc(size_t n) {
    void *p = cl_track_malloc(n);
    memset(p, 0, n);
    return p;
}

void *cl_track_realloc(void *old, size_t n) {
    if (!old) return cl_track_malloc(n);
    ensure_atexit();
    TAlloc *oh = (TAlloc *)old - 1;
    TAlloc *old_prev = oh->prev;
    TAlloc *old_next = oh->next;
    TAlloc *nh = (TAlloc *)realloc(oh, sizeof(TAlloc) + n);
    if (!nh) cl_die("out of memory");
    /* realloc preserves the contents of the header, but neighbors still
       point at the old address. Patch them up if the block moved. */
    if (nh != oh) {
        if (old_prev) old_prev->next = nh; else g_alloc_head = nh;
        if (old_next) old_next->prev = nh;
    }
    return (void *)(nh + 1);
}

void cl_track_free(void *p) {
    if (!p) return;
    TAlloc *h = (TAlloc *)p - 1;
    if (h->prev) h->prev->next = h->next;
    else        g_alloc_head  = h->next;
    if (h->next) h->next->prev = h->prev;
    free(h);
}

void cl_strncpy_z(char *dst, const char *src, size_t dst_size) {
    if (dst_size == 0) return;
    size_t i = 0;
    while (i + 1 < dst_size && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

char *cl_track_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)cl_track_malloc(n);
    memcpy(p, s, n);
    return p;
}

void cl_fputs_quoted(FILE *f, const char *s) {
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '\n': fputs("\\n",  f); break;
            case '\t': fputs("\\t",  f); break;
            case '\r': fputs("\\r",  f); break;
            case '\\': fputs("\\\\", f); break;
            case '"':  fputs("\\\"", f); break;
            default:   fputc((int)c, f);
        }
    }
    fputc('"', f);
}

static int decode_escape(char esc, char *out) {
    switch (esc) {
        case 'n':  *out = '\n'; return 1;
        case 't':  *out = '\t'; return 1;
        case 'r':  *out = '\r'; return 1;
        case '\\': *out = '\\'; return 1;
        case '"':  *out = '"';  return 1;
        default:                return 0;
    }
}

int cl_fread_quoted(FILE *f, char *out, size_t out_size) {
    int c;
    do { c = fgetc(f); } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    if (c != '"') cl_die("expected opening '\"' for quoted string");
    size_t i = 0;
    while ((c = fgetc(f)) != EOF && c != '"') {
        char ch;
        if (c == '\\') {
            int esc = fgetc(f);
            if (esc == EOF) cl_die("unterminated escape in quoted string");
            if (!decode_escape((char)esc, &ch)) cl_die("bad escape in quoted string");
        } else {
            ch = (char)c;
        }
        if (out_size == 0 || i + 1 >= out_size) cl_die("quoted string too long");
        out[i++] = ch;
    }
    if (c != '"') cl_die("unterminated quoted string");
    out[i] = '\0';
    return (int)i;
}

int cl_parse_quoted(const char **pp, char *out, size_t out_size) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') cl_die("expected opening '\"' for quoted string");
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        char ch;
        if (*p == '\\') {
            p++;
            if (!*p || !decode_escape(*p, &ch)) cl_die("bad escape in quoted string");
            p++;
        } else {
            ch = *p++;
        }
        if (out_size == 0 || i + 1 >= out_size) cl_die("quoted string too long");
        out[i++] = ch;
    }
    if (*p != '"') cl_die("unterminated quoted string");
    p++;
    *pp = p;
    out[i] = '\0';
    return (int)i;
}

char *cl_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        cl_die("failed to seek file");
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        cl_die("failed to determine file size");
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        cl_die("failed to rewind file");
    }
    char *buf = (char *)cl_track_malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    if (got != (size_t)n) {
        fclose(f);
        cl_die("failed to read file");
    }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

void cl_init_install_paths(const char *argv0) {
    if (!argv0 || !*argv0) return;

    /* Strip the basename to get <bin_dir>. */
    char dir[768];
    cl_strncpy_z(dir, argv0, sizeof dir);
    char *s = strrchr(dir, '/');
    char *b = strrchr(dir, '\\');
    char *cut = (s && (!b || s > b)) ? s : b;
    if (!cut) return;                               /* bare name — no directory */
    *cut = '\0';

    /* Forward slashes work on both POSIX and Windows fopen / -I paths.
       Buffers are static because putenv on some libcs retains the pointer. */
    if (!getenv("CALC_HOME")) {
        static char home_env[1024];
        snprintf(home_env, sizeof home_env, "CALC_HOME=%s/..", dir);
        putenv(home_env);
    }
    if (!getenv("CALC_LIB_PATH")) {
        static char lib_env[1024];
        snprintf(lib_env, sizeof lib_env, "CALC_LIB_PATH=%s/../lib", dir);
        putenv(lib_env);
    }
}

void cl_write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    size_t n = strlen(text);
    if (fwrite(text, 1, n, f) != n) {
        fclose(f);
        cl_die("failed to write file");
    }
    if (fclose(f) != 0) cl_die("failed to close output file");
}
