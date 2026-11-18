/* sdkvalues.h -- values that change over time (docs/graphics-sdk.md, "Values that change").
 *
 * Wherever a number or a colour goes, an author may write instead
 *
 *     pulse   { rate r, low a, high b }      a smooth rise and fall, r times a second
 *     flicker { rate r, low a, high b }      a random smooth wobble
 *     fade    { from a, to b, over s }       once, from a to b (on an effect item or a particle: over its life)
 *     curve   { t v, t v, ... [, loop yes] } points; seconds on a material or an effect item, 0..1 of the life on
 *                                            a particle
 *
 * (a and b numbers or colours). On a material the value becomes DOOM 2016's own register program over `time` and a
 * table (every vector parm of a parm block is an expression: VK 0x141a035e0 / GL 0x141a1f680), so it costs no script
 * time: a pulse samples the game's `sintable`, a flicker its `random_smooth03`, a curve a table the product derives
 * (sdk_register). Pure apart from the registration callback. */
#ifndef SNAPMAP_PLUS_SDKVALUES_H
#define SNAPMAP_PLUS_SDKVALUES_H

#include <stddef.h>

#include "sdkparse.h"

enum { SDK_VAL_CONST, SDK_VAL_PULSE, SDK_VAL_FLICKER, SDK_VAL_FADE, SDK_VAL_CURVE };

#define SDK_CURVE_MAX 64

typedef struct sdk_value {
    int form;
    int dims;                          /* components: 1 (a number) to 4 */
    double c[4];                       /* SDK_VAL_CONST */
    double low[4], high[4], rate;      /* pulse, flicker */
    double from[4], to[4], over;       /* fade (over 0: over the life) */
    int points, loop;                  /* curve */
    double t[SDK_CURVE_MAX], v[SDK_CURVE_MAX][4];
} sdk_value;

/* Registers a derived decl (its type as the engine spells it, its text) and returns its name, or NULL. The default
 * is modprog's (sh_modprog_register_decl); tests install their own. */
typedef const char *(*sdk_register_fn)(const char *type, const char *text);
void sdk_set_register(sdk_register_fn fn);
const char *sdk_register(const char *type, const char *text);

/* Reads a field's value from atoms[first..count) of `it`: a number or a group of numbers, or one of the four forms.
 * `dims_min`..`dims_max` bound the components; `field` names it in sentences. 1 with *out; 0 with `why`. */
int sdk_value_read(const sdk_doc *d, const sdk_item *it, size_t first, int dims_min, int dims_max, const char *field,
                   sdk_value *out, char *why, size_t why_len);

/* 1 when `text` (a material line's value, after its key) is one of the four forms (it starts with its word and '{'). */
int sdk_value_is_form(const char *text, size_t n);

/* A material's key `key` taking `v`: its register lines, each ended by `eol`, appended to `out`; temporaries from
 * *temp up (advanced). 1, or 0 with a sentence (a fade, or a curve that does not loop: a material has no start). */
int sdk_value_material(const sdk_doc *d, size_t line, const char *key, const sdk_value *v, int *temp, sdk_buf *out,
                       const char *eol, char *why, size_t why_len);

/* Every value-form line of a material text rewritten to its register lines: 0 no form (unchanged), 1 *out holds the
 * new text (malloc'd), 2 a mistake in `why`, -1 out of memory. */
int sdk_values_material_text(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                             size_t why_len);

/* A table decl text for a curve's component `c` over `span` (the last time, or 1 for a life): times / span, values
 * normalised to 0..1 (the lookup's min 0 max 1); *lo and *hi the value range. `clamp` when it does not repeat. */
char *sdk_value_table_text(const sdk_value *v, int c, int clamp, double *lo, double *hi);

/* 1 when the render parm is one DOOM 2016 decodes from sRGB at parse (its parm decl is flagged srgba: 43 parms,
 * measured from the installed renderparm decls); a constant the compiler moves into registers is decoded the same
 * way, clamped to 0..1 as the parser does. */
int sdk_parm_is_srgb(const char *name, size_t n);
double sdk_srgb_to_linear(double x);

#endif /* SNAPMAP_PLUS_SDKVALUES_H */
