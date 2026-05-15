#ifndef CALCLANG_REGEX_H
#define CALCLANG_REGEX_H

/* Small regex engine used by the runtime regex_* builtins.
   See src/regex.c for the feature list. */

#define CL_RE_ICASE  0x01

typedef struct CalcRegex CalcRegex;

CalcRegex *cl_regex_compile(const char *pattern, int flags);
void       cl_regex_free(CalcRegex *r);
const char *cl_regex_error(void);
int        cl_regex_n_captures(CalcRegex *r);

/* Search for the first match in text[start_pos..text_len). Returns 1
   on success, 0 on no match. Fills *match_start / *match_end with the
   overall match bounds. If cap_starts / cap_ends / max_caps are
   non-NULL/positive, also fills capture-group bounds: index 0 is the
   whole match, indices 1..n_captures are the parenthesised groups
   (uncaptured groups get start/end == -1). */
int cl_regex_match(CalcRegex *r, const char *text, int text_len,
                   int start_pos,
                   int *match_start, int *match_end,
                   int *cap_starts, int *cap_ends, int max_caps, int *n_caps);

#endif
