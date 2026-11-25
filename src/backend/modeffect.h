/* modeffect.h -- the Graphics SDK's effects (docs/graphics-sdk.md, "Effects" and "Particles").
 * An fx decl whose first key is `type effect` lists plain items (light, decal, particles, ...); sh_effect_compile turns
 * it into DOOM 2016's own fx decl (VK FUN_1416f0240 / GL FUN_1416f0bc0 switches on each item type) and names its
 * emitter entityDef in *needs (sh_modprog_effect_emitter). Pure apart from the decls it registers. */
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

#endif /* SNAPMAP_PLUS_MODEFFECT_H */
