/* sdkparse.h -- the Graphics SDK's author grammar, shared by its compilers (modeffect.c, modenv.c, sdkvalues.c).
 *
 * An SDK file is one brace group. A group holds items, one per line (or separated by commas); an item is a row of
 * atoms: words, numbers, quoted names, `game("...")` names and nested groups. So
 *
 *     {
 *         type        effect
 *         light {
 *             colour      { 1, 0.45, 0.1 }
 *             brightness  flicker { rate 8, low 0.6, high 1 }
 *         }
 *     }
 *
 * is a group of two items: [type effect] and [light {group}], and `colour { 1, 0.45, 0.1 }` an item of a word and a
 * group of three one-number items. Comments are // and / * * /. Pure; no allocation outlives sdk_doc_free. */
#ifndef SNAPMAP_PLUS_SDKPARSE_H
#define SNAPMAP_PLUS_SDKPARSE_H

#include <stddef.h>

#include "sdkwords.h"

enum { SDK_A_WORD, SDK_A_NUMBER, SDK_A_STRING, SDK_A_GROUP };

typedef struct sdk_group sdk_group;

typedef struct sdk_atom {
    int kind;
    const char *p;     /* the text (a word, a number as written, a string's contents) */
    size_t n;
    double num;        /* SDK_A_NUMBER */
    int game;          /* SDK_A_STRING written game("..."): one of the game's own names */
    sdk_group *group;  /* SDK_A_GROUP */
    size_t line;
} sdk_atom;

typedef struct sdk_item {
    sdk_atom *atoms;
    size_t count;
    size_t line;
} sdk_item;

struct sdk_group {
    sdk_item *items;
    size_t count, cap;
    size_t line;
};

typedef struct sdk_doc {
    sdk_group *root;
    void *arena;       /* every allocation, freed by sdk_doc_free */
    char file[128];    /* the file's base name, for sentences */
} sdk_doc;

/* Parses `text`. 1 with *doc filled; 0 with a sentence in `why` (unbalanced braces, an unclosed name, a stray
 * character); -1 out of memory. `path` names the file in sentences (its base name only). */
int sdk_parse(const char *path, const char *text, size_t len, sdk_doc *doc, char *why, size_t why_len);
void sdk_doc_free(sdk_doc *doc);

/* 1 when the atom is the word `w` (ASCII case-insensitive). */
int sdk_is(const sdk_atom *a, const char *w);
/* The atom's text as a NUL-terminated copy in `out` (truncated). */
void sdk_text(const sdk_atom *a, char *out, size_t cap);

/* The first item of `g` whose first atom is the word `key`, or NULL. */
const sdk_item *sdk_find(const sdk_group *g, const char *key);

/* The candidate closest to `word` (edit distance, at most a third of its length + 1), or NULL. */
const char *sdk_nearest(const char *word, size_t n, const char *const *cands, size_t count);

/* Reads numbers: a number atom or a group of one-number items; returns how many were read (at most max), 0 when the
 * atom is neither. */
int sdk_numbers(const sdk_atom *a, double *out, int max);

/* The kind an SDK file declares: 1 with the word after its first key `type` (in `kind`, lower case), 0 when its
 * first key is not `type`. A quick look before parsing: comments before the key are skipped. */
int sdk_first_type(const char *text, size_t len, char *kind, size_t cap);

/* A sentence buffer: "<file> line <n>: <text>". */
void sdk_say(char *why, size_t why_len, const sdk_doc *d, size_t line, const char *fmt, ...);

/* A growing text buffer. */
typedef struct sdk_buf { char *p; size_t n, cap; int oom; } sdk_buf;
void sdk_put(sdk_buf *b, const char *s, size_t n);
void sdk_printf(sdk_buf *b, const char *fmt, ...);
/* A float as the decl writers write it: up to 7 significant digits, no trailing zeros, "0" for zero. */
void sdk_fmt(char *out, size_t cap, double v);

#endif /* SNAPMAP_PLUS_SDKPARSE_H */
