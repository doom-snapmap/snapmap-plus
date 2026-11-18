/* modenv.h -- the Graphics SDK's environments (docs/graphics-sdk.md, "Environment").
 *
 * An env decl whose first key is `type environment` names, in plain words, what it changes in the level's look: fog,
 * bloom, exposure, saturation, tint, brightness, vignette, grain, colour fringes, and how long it blends. It compiles
 * to two things DOOM 2016 draws with its own code:
 *
 *   - the env decl itself (inherit default; a renderParms block of the keys it names), for an area of the map
 *     (an `envarea` entity, idEnvArea, carries one env decl for its volume);
 *   - an fx decl the product derives, one FX_ENV_CHANGE item per 16 values (the view's own parm override, set with
 *     the fade-in and released with the fade-out when the fx stops: VK FUN_1416f0240 / GL FUN_1416f0bc0, case 0xb),
 *     played on the players' views by set_environment and stopped by clear_environment. Its emitter is
 *     sh_modprog_effect_emitter of it; both are the file's needs.
 *
 * Values may change over time (sdkvalues.h): in the env decl they are the parm block's own register program. */
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
