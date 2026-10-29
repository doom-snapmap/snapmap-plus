/* mp_json.h -- a small JSON tree for the module prefab cut and merge.
 *
 * Parses a complete document into nodes and writes it back compactly. Object
 * members keep their order, strings keep their exact escaped spelling and
 * numbers keep their source text, so an untouched value round-trips byte for
 * byte. Values created here are written in the engine's map spelling (integers
 * plain, floats with a fractional part). Pure C, no engine dependency.
 * Nesting is limited to MPJ_MAX_DEPTH; input past that is refused. */
#ifndef SH_MP_JSON_H
#define SH_MP_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MPJ_MAX_DEPTH 96

typedef enum mpj_type { MPJ_NULL, MPJ_BOOL, MPJ_NUM, MPJ_STR, MPJ_ARR, MPJ_OBJ } mpj_type;

typedef struct mpj {
    mpj_type     type;
    int          b;          /* MPJ_BOOL */
    char        *text;       /* MPJ_NUM: number text; MPJ_STR: escaped body without quotes */
    struct mpj **items;      /* MPJ_ARR / MPJ_OBJ values */
    char       **keys;       /* MPJ_OBJ: escaped key bodies, parallel to items */
    int          n, cap;
} mpj;

/* Parse len bytes. NULL on any syntax error, trailing data or allocation failure. */
mpj  *mpj_parse(const char *s, size_t len);
void  mpj_free(mpj *v);
mpj  *mpj_clone(const mpj *v);

/* Compact text, NUL-terminated, malloc-owned; *len excludes the NUL. NULL on failure. */
char *mpj_dump(const mpj *v, size_t *len);

mpj  *mpj_new(mpj_type t);
mpj  *mpj_new_int(long long v);
mpj  *mpj_new_float(double v);
/* raw is an already escaped string body (no quotes). */
mpj  *mpj_new_str(const char *raw);

/* Object member by key (exact match on the escaped spelling), or NULL. */
mpj  *mpj_get(const mpj *obj, const char *key);
/* Replace a member's value in place (freeing the old one) or append it. Takes
 * ownership of v. 0 on allocation failure (v is freed). */
int   mpj_set(mpj *obj, const char *key, mpj *v);
/* Append to an array; takes ownership. 0 on failure (v is freed). */
int   mpj_push(mpj *arr, mpj *v);

/* Numeric value; 0 for a non-number. */
double    mpj_num(const mpj *v);
long long mpj_int(const mpj *v);
int       mpj_is_num(const mpj *v);
/* 1 when v is a string whose escaped body equals s. */
int       mpj_str_is(const mpj *v, const char *s);

#ifdef __cplusplus
}
#endif

#endif
