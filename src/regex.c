/*
   src/regex.c — small but featureful regex engine for CalcLang.

   This is its own translation unit (no SDL2 / no asm) so it builds
   into every CalcLang binary. The public API is just three calls —
   `cl_regex_compile`, `cl_regex_match`, `cl_regex_free` — exposed
   through the runtime builtins regex_match / regex_find /
   regex_find_all / regex_replace / regex_split.

   What's supported (~PCRE subset):

     Literals:           ab, escape: \n \t \r \\ \. \/ \( etc.
     Any char:           .             (does NOT match newline)
     Anchors:            ^ $
     Character classes:  [abc], [a-z], [^a-z], [\d\w]
     Class shortcuts:    \d \D \w \W \s \S
     Word boundary:      \b \B
     Quantifiers:        *  +  ?  {n}  {n,}  {n,m}    (greedy)
     Lazy quantifiers:   *? +? ??  {n,m}?              (non-greedy)
     Groups:             (capturing)  (?:non-capturing)
     Alternation:        a|b|c
     Flags:              CL_RE_ICASE — case-insensitive matching

   Engine shape: regex source → parse tree (NodeRe) → recursive
   backtracking matcher. Backtracking is simple and good enough for
   the kind of patterns engineering scripts actually use; if you need
   pathological-input guarantees, you'd use an NFA. Up to 16 capture
   groups, up to ~1024 nodes per compiled pattern.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "regex.h"

/* ---- Limits ---- */
#define RE_MAX_NODES   2048
#define RE_MAX_CAPS    16

/* ---- AST node kinds ---- */
typedef enum {
    RE_LIT,         /* single literal char (with optional case-insensitive) */
    RE_ANY,         /* .  */
    RE_ANCHOR_BOS,  /* ^  */
    RE_ANCHOR_EOS,  /* $  */
    RE_ANCHOR_WB,   /* \b */
    RE_ANCHOR_NWB,  /* \B */
    RE_CLASS,       /* character class — payload is a 256-bit bitmask */
    RE_GROUP,       /* (...)   — capturing OR non-capturing */
    RE_ALT,         /* a|b     — children[0] | children[1] */
    RE_CONCAT,      /* sequence — children[0..n-1] */
    RE_REPEAT       /* X{n,m}? — child + min, max, greedy */
} ReKind;

typedef struct ReNode {
    ReKind kind;
    /* Per-kind payload. */
    int    ch;            /* LIT */
    unsigned char cls[32]; /* CLASS — 256-bit bitmap */
    int    cap_index;     /* GROUP — capture index (0 means non-capturing) */
    int    min, max;      /* REPEAT — max = -1 means unbounded */
    int    greedy;        /* REPEAT */
    struct ReNode *child; /* GROUP, REPEAT */
    /* For ALT + CONCAT we hold a small array of children inline.
       The 16 limit applies per CONCAT/ALT node — patterns with long
       chains of literals build chains of small CONCAT nodes. */
    struct ReNode *children[16];
    int    n_children;
} ReNode;

struct CalcRegex {
    ReNode nodes[RE_MAX_NODES];
    int    n_nodes;
    ReNode *root;
    int    flags;
    int    n_captures;       /* number of capture groups (1-based labels) */
};

/* ---- Error reporting ---- */
static char re_errbuf[256];
const char *cl_regex_error(void) { return re_errbuf; }
static void re_err(const char *msg) {
    snprintf(re_errbuf, sizeof re_errbuf, "%s", msg);
}

/* ---- AST allocation ---- */
static ReNode *re_alloc(CalcRegex *r, ReKind k) {
    if (r->n_nodes >= RE_MAX_NODES) { re_err("regex too complex"); return NULL; }
    ReNode *n = &r->nodes[r->n_nodes++];
    memset(n, 0, sizeof *n);
    n->kind = k;
    n->min = 1; n->max = 1; n->greedy = 1;
    return n;
}

/* ---- Char-class helpers ---- */
static void cls_set(unsigned char *cls, int c) {
    cls[(c >> 3) & 0x1F] |= (unsigned char)(1u << (c & 7));
}
static int  cls_has(const unsigned char *cls, int c) {
    return (cls[(c >> 3) & 0x1F] >> (c & 7)) & 1;
}
static void cls_invert(unsigned char *cls) {
    for (int i = 0; i < 32; i++) cls[i] = (unsigned char)~cls[i];
}
static void cls_set_range(unsigned char *cls, int lo, int hi) {
    for (int c = lo; c <= hi && c < 256; c++) cls_set(cls, c);
}
static void cls_add_shortcut(unsigned char *cls, int kind) {
    /* kind is the char after the backslash: d D w W s S */
    switch (kind) {
        case 'd':
            cls_set_range(cls, '0', '9');
            return;
        case 'D':
            for (int c = 0; c < 256; c++) {
                if (!(c >= '0' && c <= '9')) cls_set(cls, c);
            }
            return;
        case 'w':
            cls_set_range(cls, 'a', 'z');
            cls_set_range(cls, 'A', 'Z');
            cls_set_range(cls, '0', '9');
            cls_set(cls, '_');
            return;
        case 'W':
            for (int c = 0; c < 256; c++) {
                int is_w = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                        || (c >= '0' && c <= '9') || c == '_';
                if (!is_w) cls_set(cls, c);
            }
            return;
        case 's':
            cls_set(cls, ' '); cls_set(cls, '\t');
            cls_set(cls, '\n'); cls_set(cls, '\r');
            cls_set(cls, '\f'); cls_set(cls, '\v');
            return;
        case 'S':
            for (int c = 0; c < 256; c++) {
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r'
                    && c != '\f' && c != '\v') cls_set(cls, c);
            }
            return;
    }
}

/* ---- Parser ---- */
typedef struct {
    const char *src;
    int         pos;
    int         len;
    CalcRegex  *r;
    int         err;
    int         cap_counter;   /* next capture index to assign */
} ReParser;

static ReNode *parse_alt(ReParser *p);

/* Parse an escape sequence at p->pos (`\` already consumed). Returns
   the resulting node, which may be a LIT or a CLASS (for \d etc.) or
   an anchor (for \b \B). */
static ReNode *parse_escape(ReParser *p) {
    if (p->pos >= p->len) { re_err("dangling \\"); p->err = 1; return NULL; }
    char c = p->src[p->pos++];
    /* Class shortcuts. */
    if (c == 'd' || c == 'D' || c == 'w' || c == 'W' || c == 's' || c == 'S') {
        ReNode *n = re_alloc(p->r, RE_CLASS);
        if (!n) { p->err = 1; return NULL; }
        cls_add_shortcut(n->cls, c);
        return n;
    }
    if (c == 'b') { return re_alloc(p->r, RE_ANCHOR_WB); }
    if (c == 'B') { return re_alloc(p->r, RE_ANCHOR_NWB); }
    int lit_ch = c;
    switch (c) {
        case 'n': lit_ch = '\n'; break;
        case 't': lit_ch = '\t'; break;
        case 'r': lit_ch = '\r'; break;
        case 'f': lit_ch = '\f'; break;
        case 'v': lit_ch = '\v'; break;
        case '0': lit_ch = '\0'; break;
        /* Common metas and punct are passed through. */
    }
    ReNode *n = re_alloc(p->r, RE_LIT);
    if (!n) { p->err = 1; return NULL; }
    n->ch = (int)(unsigned char)lit_ch;
    return n;
}

/* Parse a [...] character class. The opening '[' is already consumed. */
static ReNode *parse_class(ReParser *p) {
    ReNode *n = re_alloc(p->r, RE_CLASS);
    if (!n) { p->err = 1; return NULL; }
    int negate = 0;
    if (p->pos < p->len && p->src[p->pos] == '^') {
        negate = 1;
        p->pos++;
    }
    while (p->pos < p->len && p->src[p->pos] != ']') {
        int c = (unsigned char)p->src[p->pos++];
        if (c == '\\' && p->pos < p->len) {
            char esc = p->src[p->pos];
            if (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W'
                || esc == 's' || esc == 'S') {
                p->pos++;
                cls_add_shortcut(n->cls, esc);
                continue;
            }
            p->pos++;
            switch (esc) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'f': c = '\f'; break;
                case 'v': c = '\v'; break;
                default:  c = (unsigned char)esc;
            }
        }
        if (p->pos + 1 < p->len && p->src[p->pos] == '-' && p->src[p->pos + 1] != ']') {
            p->pos++;  /* consume '-' */
            int hi = (unsigned char)p->src[p->pos++];
            if (hi == '\\' && p->pos < p->len) {
                char esc = p->src[p->pos++];
                switch (esc) {
                    case 'n': hi = '\n'; break;
                    case 't': hi = '\t'; break;
                    case 'r': hi = '\r'; break;
                    default:  hi = (unsigned char)esc;
                }
            }
            cls_set_range(n->cls, c, hi);
        } else {
            cls_set(n->cls, c);
        }
    }
    if (p->pos >= p->len || p->src[p->pos] != ']') {
        re_err("unterminated [class]"); p->err = 1; return NULL;
    }
    p->pos++; /* consume ']' */
    if (negate) cls_invert(n->cls);
    return n;
}

/* Parse a single atomic element: literal, ., class, group, anchor. */
static ReNode *parse_atom(ReParser *p) {
    if (p->pos >= p->len) return NULL;
    char c = p->src[p->pos];
    if (c == '(') {
        p->pos++;
        int cap_idx;
        if (p->pos + 1 < p->len && p->src[p->pos] == '?' && p->src[p->pos + 1] == ':') {
            cap_idx = 0;       /* non-capturing */
            p->pos += 2;
        } else {
            cap_idx = ++p->cap_counter;
        }
        ReNode *inner = parse_alt(p);
        if (p->err) return NULL;
        if (p->pos >= p->len || p->src[p->pos] != ')') {
            re_err("unmatched ("); p->err = 1; return NULL;
        }
        p->pos++;
        ReNode *g = re_alloc(p->r, RE_GROUP);
        if (!g) { p->err = 1; return NULL; }
        g->cap_index = cap_idx;
        g->child = inner;
        return g;
    }
    if (c == '.') {
        p->pos++;
        return re_alloc(p->r, RE_ANY);
    }
    if (c == '[') {
        p->pos++;
        return parse_class(p);
    }
    if (c == '^') {
        p->pos++;
        return re_alloc(p->r, RE_ANCHOR_BOS);
    }
    if (c == '$') {
        p->pos++;
        return re_alloc(p->r, RE_ANCHOR_EOS);
    }
    if (c == '\\') {
        p->pos++;
        return parse_escape(p);
    }
    /* Reserved metas that shouldn't appear here. */
    if (c == ')' || c == '|') return NULL;
    if (c == '*' || c == '+' || c == '?' || c == '{') {
        re_err("nothing to repeat"); p->err = 1; return NULL;
    }
    p->pos++;
    ReNode *n = re_alloc(p->r, RE_LIT);
    if (!n) { p->err = 1; return NULL; }
    n->ch = (int)(unsigned char)c;
    return n;
}

/* Parse {n}, {n,}, {n,m}, optionally with trailing ? for lazy.
   Caller has consumed the '{'. */
static int parse_curly(ReParser *p, int *min_out, int *max_out, int *greedy_out) {
    int min = 0, has_min = 0;
    while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) {
        min = min * 10 + (p->src[p->pos++] - '0');
        has_min = 1;
    }
    if (!has_min) { re_err("missing count in {}"); return -1; }
    int max = min;
    if (p->pos < p->len && p->src[p->pos] == ',') {
        p->pos++;
        max = -1;
        int has_max = 0;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) {
            if (!has_max) max = 0;
            max = max * 10 + (p->src[p->pos++] - '0');
            has_max = 1;
        }
        if (!has_max) max = -1;
    }
    if (p->pos >= p->len || p->src[p->pos] != '}') {
        re_err("unterminated {n,m}"); return -1;
    }
    p->pos++;
    int greedy = 1;
    if (p->pos < p->len && p->src[p->pos] == '?') {
        greedy = 0;
        p->pos++;
    }
    *min_out = min; *max_out = max; *greedy_out = greedy;
    return 0;
}

/* Parse an atom and any optional quantifier (`*`, `+`, `?`, `{n,m}`,
   plus lazy `?` suffix). */
static ReNode *parse_piece(ReParser *p) {
    ReNode *atom = parse_atom(p);
    if (!atom || p->err) return atom;
    /* Anchors aren't quantifiable. */
    if (atom->kind == RE_ANCHOR_BOS || atom->kind == RE_ANCHOR_EOS
        || atom->kind == RE_ANCHOR_WB || atom->kind == RE_ANCHOR_NWB) {
        return atom;
    }
    if (p->pos >= p->len) return atom;
    char c = p->src[p->pos];
    int min = -1, max = -1, greedy = 1;
    if (c == '*') {
        p->pos++; min = 0; max = -1;
        if (p->pos < p->len && p->src[p->pos] == '?') { greedy = 0; p->pos++; }
    } else if (c == '+') {
        p->pos++; min = 1; max = -1;
        if (p->pos < p->len && p->src[p->pos] == '?') { greedy = 0; p->pos++; }
    } else if (c == '?') {
        p->pos++; min = 0; max = 1;
        if (p->pos < p->len && p->src[p->pos] == '?') { greedy = 0; p->pos++; }
    } else if (c == '{') {
        p->pos++;
        if (parse_curly(p, &min, &max, &greedy) < 0) { p->err = 1; return NULL; }
    } else {
        return atom;
    }
    ReNode *rep = re_alloc(p->r, RE_REPEAT);
    if (!rep) { p->err = 1; return NULL; }
    rep->min = min; rep->max = max; rep->greedy = greedy;
    rep->child = atom;
    return rep;
}

/* Parse a sequence of pieces — concatenation. */
static ReNode *parse_concat(ReParser *p) {
    ReNode *seq[16];
    int     n = 0;
    while (p->pos < p->len && p->src[p->pos] != '|' && p->src[p->pos] != ')') {
        if (n >= 16) {
            /* Wrap the prefix in a CONCAT node and continue collecting. */
            ReNode *grp = re_alloc(p->r, RE_CONCAT);
            if (!grp) { p->err = 1; return NULL; }
            for (int i = 0; i < n; i++) grp->children[i] = seq[i];
            grp->n_children = n;
            seq[0] = grp;
            n = 1;
        }
        ReNode *piece = parse_piece(p);
        if (p->err) return NULL;
        if (!piece) break;
        seq[n++] = piece;
    }
    if (n == 0) {
        /* Empty alternative — use a CONCAT with zero children. */
        ReNode *node = re_alloc(p->r, RE_CONCAT);
        if (!node) { p->err = 1; return NULL; }
        return node;
    }
    if (n == 1) return seq[0];
    ReNode *node = re_alloc(p->r, RE_CONCAT);
    if (!node) { p->err = 1; return NULL; }
    for (int i = 0; i < n; i++) node->children[i] = seq[i];
    node->n_children = n;
    return node;
}

/* Parse a top-level alternation. */
static ReNode *parse_alt(ReParser *p) {
    ReNode *first = parse_concat(p);
    if (p->err) return NULL;
    if (p->pos >= p->len || p->src[p->pos] != '|') return first;
    ReNode *alts[16];
    int n = 0;
    alts[n++] = first;
    while (p->pos < p->len && p->src[p->pos] == '|') {
        p->pos++;
        if (n >= 16) { re_err("too many | alternatives"); p->err = 1; return NULL; }
        ReNode *next = parse_concat(p);
        if (p->err) return NULL;
        alts[n++] = next;
    }
    ReNode *node = re_alloc(p->r, RE_ALT);
    if (!node) { p->err = 1; return NULL; }
    for (int i = 0; i < n; i++) node->children[i] = alts[i];
    node->n_children = n;
    return node;
}

CalcRegex *cl_regex_compile(const char *pattern, int flags) {
    if (!pattern) { re_err("null pattern"); return NULL; }
    CalcRegex *r = (CalcRegex *)calloc(1, sizeof *r);
    if (!r) { re_err("oom"); return NULL; }
    r->flags = flags;
    ReParser p;
    p.src = pattern;
    p.len = (int)strlen(pattern);
    p.pos = 0;
    p.r   = r;
    p.err = 0;
    p.cap_counter = 0;
    r->root = parse_alt(&p);
    if (p.err || !r->root) {
        free(r);
        return NULL;
    }
    if (p.pos != p.len) {
        re_err("trailing junk in pattern");
        free(r);
        return NULL;
    }
    r->n_captures = p.cap_counter;
    return r;
}

void cl_regex_free(CalcRegex *r) {
    if (r) free(r);
}

int cl_regex_n_captures(CalcRegex *r) {
    return r ? r->n_captures : 0;
}

/* ---- Matcher ---- */

typedef struct {
    const char *text;
    int         len;
    int         flags;
    /* Capture positions. cap_starts[i] / cap_ends[i] for i in 0..max_caps. */
    int        *cap_starts;
    int        *cap_ends;
    int         n_caps;
} ReMatch;

static int re_match_node(ReMatch *m, ReNode *n, int pos, int *out_pos);
static int re_match_concat(ReMatch *m, ReNode *node, int pos, int *out_pos);
static int re_match_concat_from(ReMatch *m, ReNode *node, int pos, int i, int *out_pos);
static int re_repeat_with_tail(ReMatch *m, ReNode *rep, int pos,
                               ReNode **tail, int tail_n, int tail_i,
                               int *out_pos);

static int char_eq(int a, int b, int icase) {
    if (a == b) return 1;
    if (icase) {
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        return a == b;
    }
    return 0;
}

/* Try a sequence of children starting at index i, position pos. */
static int re_match_seq(ReMatch *m, ReNode **children, int n, int i, int pos, int *out_pos) {
    if (i >= n) { *out_pos = pos; return 1; }
    /* We need to backtrack between children — match child[i] in
       multiple positions if it's variable-length. The simple recursive
       form below works because re_match_node returns one match; for
       quantifiers we open up the backtracking explicitly there. */
    ReNode *child = children[i];
    /* Save captures so we can restore on backtrack-from-tail. */
    int saved_caps_starts[RE_MAX_CAPS];
    int saved_caps_ends[RE_MAX_CAPS];
    memcpy(saved_caps_starts, m->cap_starts, sizeof(int) * (size_t)m->n_caps);
    memcpy(saved_caps_ends,   m->cap_ends,   sizeof(int) * (size_t)m->n_caps);
    int next_pos;
    if (!re_match_node(m, child, pos, &next_pos)) return 0;
    if (re_match_seq(m, children, n, i + 1, next_pos, out_pos)) return 1;
    /* Restore on failure. */
    memcpy(m->cap_starts, saved_caps_starts, sizeof(int) * (size_t)m->n_caps);
    memcpy(m->cap_ends,   saved_caps_ends,   sizeof(int) * (size_t)m->n_caps);
    /* For variable-length children we'd need to re-enter re_match_node
       with backtracking state. The REPEAT handler already handles its
       own internal backtracking; for non-REPEAT children this single
       attempt is enough. */
    return 0;
}

/* Try to match a REPEAT node starting at pos. The repeat may produce
   0..min..max matches; we explore each possible count then ask the
   surrounding context (passed implicitly via out_pos chaining) to
   continue. Since this matcher operates on a node tree (not flat
   continuations), the easiest way to "ask the tail" is to require
   the caller to call re_match_seq for the rest of the surrounding
   concatenation. We do that by exposing a `tail` continuation: the
   REPEAT match function takes the children-tail to satisfy. */
static int re_repeat_with_tail(ReMatch *m, ReNode *rep, int pos,
                               ReNode **tail, int tail_n, int tail_i,
                               int *out_pos) {
    int min = rep->min, max = rep->max;
    int greedy = rep->greedy;

    /* Collect possible counts, then try them in greedy or lazy order. */
    int positions[1024];
    positions[0] = pos;
    int count = 0;
    int cur = pos;
    while (max < 0 || count < max) {
        int saved_cs[RE_MAX_CAPS], saved_ce[RE_MAX_CAPS];
        memcpy(saved_cs, m->cap_starts, sizeof(int) * (size_t)m->n_caps);
        memcpy(saved_ce, m->cap_ends,   sizeof(int) * (size_t)m->n_caps);
        int nxt;
        if (!re_match_node(m, rep->child, cur, &nxt)) break;
        if (nxt == cur) {
            /* Zero-width match — would loop forever. Stop. */
            memcpy(m->cap_starts, saved_cs, sizeof(int) * (size_t)m->n_caps);
            memcpy(m->cap_ends,   saved_ce, sizeof(int) * (size_t)m->n_caps);
            break;
        }
        count++;
        cur = nxt;
        if (count < (int)(sizeof positions / sizeof positions[0])) {
            positions[count] = cur;
        }
    }
    /* Now try counts in greedy (high->low) or lazy (low->high) order,
       returning the first that lets the tail succeed. */
    int from, to, step;
    if (greedy) { from = count; to = min - 1; step = -1; }
    else        { from = min;   to = count + 1; step = 1; }
    for (int k = from; greedy ? (k >= min) : (k <= count); k += step) {
        (void)to;
        if (k < 0 || k > count) continue;
        if (re_match_seq(m, tail, tail_n, tail_i, positions[k], out_pos)) return 1;
    }
    return 0;
}

/* Match a CONCAT, accounting for REPEAT children that need the tail
   continuation. */
static int re_match_concat(ReMatch *m, ReNode *node, int pos, int *out_pos) {
    return re_match_concat_from(m, node, pos, 0, out_pos);
}

/* Forward declaration above isn't C — provide the actual definition. */
static int re_match_concat_from(ReMatch *m, ReNode *node, int pos, int i, int *out_pos) {
    if (i >= node->n_children) { *out_pos = pos; return 1; }
    ReNode *child = node->children[i];
    if (child->kind == RE_REPEAT) {
        return re_repeat_with_tail(m, child, pos, node->children, node->n_children, i + 1, out_pos);
    }
    int next_pos;
    int saved_cs[RE_MAX_CAPS], saved_ce[RE_MAX_CAPS];
    memcpy(saved_cs, m->cap_starts, sizeof(int) * (size_t)m->n_caps);
    memcpy(saved_ce, m->cap_ends,   sizeof(int) * (size_t)m->n_caps);
    if (!re_match_node(m, child, pos, &next_pos)) return 0;
    if (re_match_concat_from(m, node, next_pos, i + 1, out_pos)) return 1;
    memcpy(m->cap_starts, saved_cs, sizeof(int) * (size_t)m->n_caps);
    memcpy(m->cap_ends,   saved_ce, sizeof(int) * (size_t)m->n_caps);
    return 0;
}

static int re_is_word(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

/* Match a single node. For REPEAT, we route through re_repeat_with_tail
   which needs the surrounding tail — so plain re_match_node won't
   handle REPEAT correctly. Callers should not pass REPEAT directly;
   re_match_concat_from special-cases it. We add a one-shot path here
   for REPEAT at the root (no tail) by treating tail as empty. */
static int re_match_node(ReMatch *m, ReNode *n, int pos, int *out_pos) {
    switch (n->kind) {
        case RE_LIT: {
            if (pos >= m->len) return 0;
            int t = (unsigned char)m->text[pos];
            if (!char_eq(t, n->ch, m->flags & CL_RE_ICASE)) return 0;
            *out_pos = pos + 1;
            return 1;
        }
        case RE_ANY: {
            if (pos >= m->len) return 0;
            if (m->text[pos] == '\n') return 0;
            *out_pos = pos + 1;
            return 1;
        }
        case RE_CLASS: {
            if (pos >= m->len) return 0;
            int t = (unsigned char)m->text[pos];
            int matched = cls_has(n->cls, t);
            if (!matched && (m->flags & CL_RE_ICASE)) {
                int alt = t;
                if (t >= 'a' && t <= 'z') alt = t - 32;
                else if (t >= 'A' && t <= 'Z') alt = t + 32;
                matched = cls_has(n->cls, alt);
            }
            if (!matched) return 0;
            *out_pos = pos + 1;
            return 1;
        }
        case RE_ANCHOR_BOS:
            if (pos != 0) return 0;
            *out_pos = pos;
            return 1;
        case RE_ANCHOR_EOS:
            if (pos != m->len) return 0;
            *out_pos = pos;
            return 1;
        case RE_ANCHOR_WB: {
            int before = pos > 0 ? re_is_word((unsigned char)m->text[pos - 1]) : 0;
            int after  = pos < m->len ? re_is_word((unsigned char)m->text[pos]) : 0;
            if (before == after) return 0;
            *out_pos = pos;
            return 1;
        }
        case RE_ANCHOR_NWB: {
            int before = pos > 0 ? re_is_word((unsigned char)m->text[pos - 1]) : 0;
            int after  = pos < m->len ? re_is_word((unsigned char)m->text[pos]) : 0;
            if (before != after) return 0;
            *out_pos = pos;
            return 1;
        }
        case RE_GROUP: {
            int start = pos;
            int next;
            if (!re_match_node(m, n->child, pos, &next)) return 0;
            if (n->cap_index > 0 && n->cap_index < m->n_caps) {
                m->cap_starts[n->cap_index] = start;
                m->cap_ends[n->cap_index]   = next;
            }
            *out_pos = next;
            return 1;
        }
        case RE_ALT: {
            int saved_cs[RE_MAX_CAPS], saved_ce[RE_MAX_CAPS];
            memcpy(saved_cs, m->cap_starts, sizeof(int) * (size_t)m->n_caps);
            memcpy(saved_ce, m->cap_ends,   sizeof(int) * (size_t)m->n_caps);
            for (int i = 0; i < n->n_children; i++) {
                if (re_match_node(m, n->children[i], pos, out_pos)) return 1;
                memcpy(m->cap_starts, saved_cs, sizeof(int) * (size_t)m->n_caps);
                memcpy(m->cap_ends,   saved_ce, sizeof(int) * (size_t)m->n_caps);
            }
            return 0;
        }
        case RE_CONCAT:
            return re_match_concat(m, n, pos, out_pos);
        case RE_REPEAT:
            /* Standalone REPEAT (no tail) — emulate with empty tail. */
            return re_repeat_with_tail(m, n, pos, NULL, 0, 0, out_pos);
    }
    return 0;
}

int cl_regex_match(CalcRegex *r, const char *text, int text_len,
                   int start_pos,
                   int *match_start, int *match_end,
                   int *cap_starts, int *cap_ends, int max_caps, int *n_caps) {
    if (!r || !text) return 0;
    ReMatch m;
    m.text = text;
    m.len  = text_len;
    m.flags = r->flags;
    int local_cs[RE_MAX_CAPS], local_ce[RE_MAX_CAPS];
    for (int i = 0; i < RE_MAX_CAPS; i++) { local_cs[i] = -1; local_ce[i] = -1; }
    m.cap_starts = local_cs;
    m.cap_ends   = local_ce;
    m.n_caps     = RE_MAX_CAPS;
    /* Try every starting position from start_pos forward — that's how
       find/match-anywhere works. Anchored patterns (^) will fail at
       any non-zero start, which the matcher handles naturally. */
    for (int s = start_pos; s <= text_len; s++) {
        for (int i = 0; i < RE_MAX_CAPS; i++) { local_cs[i] = -1; local_ce[i] = -1; }
        int end;
        if (re_match_node(&m, r->root, s, &end)) {
            if (match_start) *match_start = s;
            if (match_end)   *match_end   = end;
            if (cap_starts && cap_ends && max_caps > 0) {
                int n = r->n_captures + 1;
                if (n > max_caps) n = max_caps;
                cap_starts[0] = s; cap_ends[0] = end;
                for (int i = 1; i < n; i++) {
                    cap_starts[i] = local_cs[i];
                    cap_ends[i]   = local_ce[i];
                }
                if (n_caps) *n_caps = n;
            }
            return 1;
        }
    }
    return 0;
}
