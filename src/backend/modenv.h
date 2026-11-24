/* modenv.h -- the Graphics SDK's environments (docs/graphics-sdk.md, "Environment").
 * An env decl (first key `type environment`) compiles to the env decl itself (a renderParms block, carried by an
 * `envarea` entity) and a derived fx decl of FX_ENV_CHANGE items, one per 16 values (VK FUN_1416f0240 / GL
 * FUN_1416f0bc0, case 0xb), which set_environment plays on the players' views and clear_environment stops. */
#ifndef SNAPMAP_PLUS_MODENV_H
#define SNAPMAP_PLUS_MODENV_H

#include <stddef.h>

#include "sdkparse.h"
#include "sdkvalues.h"

/* One value an environment sets: the render parm (the env decl's spelling) and its value. */
typedef struct sdk_env_parm {
    const char *parm;      /* e.g. "fogColor" */
    sdk_value v;
    size_t line;
} sdk_env_parm;

#define SDK_ENV_MAX 48

/* The first line of a compiled environment: a comment naming the fx set_environment plays (sdk_runtime.c). */
#define SH_ENV_MARK "// Snapmap+ environment fx"

/* The values a group of environment fields sets, in field order; `skip` names fields the caller reads itself (NULL
 * terminated, or NULL). Returns the count (0..SDK_ENV_MAX), or -1 with a sentence (an unknown field and its nearest, a
 * wrong shape). `what` names the group in sentences ("an environment"). */
int sdk_env_parms(const sdk_doc *d, const sdk_group *g, const char *what, const char *const *skip, sdk_env_parm *out,
                  char *why, size_t why_len);

/* The fx decl text (FX_ENV_CHANGE items, looping, `blend` seconds of fade) for those values. malloc'd, or NULL. */
char *sdk_env_fx_text(const sdk_env_parm *p, int n, double blend);

/* Compiles one env decl. SH_LOOK_SAME when its first key is not `type environment`. */
int sh_env_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                   size_t why_len, char **needs, char **notes);

/* The fx set_environment plays for this env decl text (its derived fx, registered), or NULL when the text is not an
 * environment or has a mistake. malloc'd. */
char *sh_env_moment_fx(const char *path, const char *text, size_t len);

#endif /* SNAPMAP_PLUS_MODENV_H */
