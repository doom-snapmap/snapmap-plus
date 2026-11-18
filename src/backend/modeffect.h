/* modeffect.h -- the Graphics SDK's effects (docs/graphics-sdk.md, "Effects" and "Particles").
 *
 * An fx decl whose first key is `type effect` describes what plays, in plain items (light, decal, shake, flash,
 * screen, blur, focus, flare, trail, model, sound, glow, environment, wind, particles), each with when it starts, how
 * long it lasts and how it fades. sh_effect_compile turns it into DOOM 2016's own fx decl, which the game's FX
 * manager plays item by item with its own code on both renderers (the action update VK FUN_1416f0240 / GL
 * FUN_1416f0bc0 switches on every item type). What an item needs and no author writes, the compiler derives
 * (sdk_register): a particles item's particle decl and its material (a `type particle` material compiled by
 * modlook.c), a changing value's table. The file's needs name its emitter (sh_modprog_effect_emitter), the entityDef
 * the script calls spawn to play it at a point or on an entity.
 *
 * Mistakes are one sentence (an unknown item or field and its nearest, a wrong shape, a missing value); limits are
 * never mistakes. Pure apart from the decls it registers. */
#ifndef SNAPMAP_PLUS_MODEFFECT_H
#define SNAPMAP_PLUS_MODEFFECT_H

#include <stddef.h>

/* Compiles one fx decl (results as modlook.h). SH_LOOK_SAME when its first key is not `type effect`. `path` names the
 * file (its fx decl name, after generated/decls/fx/, names the emitter in *needs). */
int sh_effect_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                      size_t why_len, char **notes, char **needs);

/* 1 when `text` is an SDK effect (its first key is `type effect`). */
int sh_effect_is(const char *text, size_t len);

/* The first line of a compiled effect (a comment the engine's lexer skips): how a served fx decl says it is an SDK
 * effect. */
#define SH_EFFECT_MARK "// Snapmap+ effect"

/* Particles the GPU draws when a stage has this many or more and the GPU can draw it as written (CPU otherwise). */
#define SH_EFFECT_GPU_COUNT 256

#endif /* SNAPMAP_PLUS_MODEFFECT_H */
