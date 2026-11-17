/* modprog.h -- render programs the product derives from the installed game's own.
 *
 * The multilayer programs, snapmapplus/multilayer/<base> for the bases listed in modprog.c (skinvmtr and
 * skinvmtrwounds, the lit skinned virtual-texture programs a ported demon's body, head and wounds draw with), draw
 * DOOM Eternal's MATERIAL_IS_MULTILAYER blend at draw time: the material's own layer from its virtual-texture
 * pages, blended with up to four tiled layers chosen by a mask, each layer image sampled as Eternal samples it.
 * Everything else is the base program unchanged: skinning, page sampling and feedback, lighting, shadows,
 * the wound block, the effects. The blend's rule and what it writes are in eternal/shaders/multilayer.frag.
 *
 * Every file of such a program's closure is derived, when first asked for and then remembered for the process,
 * from the base program's installed files and the product's own sources (eternal/modprog_multilayer.c, generated from
 * eternal/shaders/ by tools/gen_multilayer.js); nothing of the game is shipped:
 *   generated/decls/renderprog/<P>.decl and each permutation's decl: the base's text with the blend's templates
 *     before its CUSTOM_TEXTURE_INPUTS template and MULTILAYER_BLEND( inputs ) as that template's first statement
 *     (the OpenGL renderer compiles this text);
 *   generated/renderprogs/<P>_pc_vulkan.bin and each permutation's: the base's cooked program with that decl's
 *     checksum, the blend's parameters after the fragment stage's material uniforms, its textures (each with
 *     defaultsampler) after the stage's own, the binding tables moved past them, and the fragment stage text with
 *     the blend added (what the module was compiled from, as the engine's freshness check hashes it);
 *   the fragment SPIR-V: the base's module with the blend's functions spliced in (types and constants shared),
 *     its samplers bound after the program's own textures (the program's later bindings move up), the parameters
 *     appended to the material uniform block, and a call where CUSTOM_TEXTURE_INPUTS begins (right after the
 *     store of inputs.normal that ends the texture inputs); its header hashA is the MD5fold of that stage text;
 *   the vertex SPIR-V: the base's, unchanged.
 * The render parms the blend reads (generated/decls/renderparm/<name>.decl) are product decls too.
 *
 * A program's textures over all its stages are bounded by the engine (ID_VULKAN_MAX_BOUND_TEXTURES, 32: VK
 * 0x141a7bbb0); the blend's 20 bring skinvmtrwounds to exactly 32 (2 vertex, 10 + 20 fragment). OpenGL's 32
 * texture units per stage hold the fragment stage's 30.
 *
 * The particle program, snapmapplus/eternal/uberparticles (the base uberparticles, every particle material's program
 * once the engine remaps transsortblend, particleshighq and explosionhighq to it), draws a CPU particle stage as DOOM
 * Eternal's uberparticles does: its unlit, lit and explosion shading, the near fade and the soft-particle distance,
 * from the same particle vertices and the stage's constants in the material (eternal/shaders/eternalparticles.frag). Its
 * closure is derived the same way: the decl with the shading's templates after the fragment stage's includes and
 * ETERNAL_PARTICLE( result ) after its PREMULTIPLY_ALPHA_BLENDED( result ); the cooked program with the four
 * parameters, projectionmatrixz and globalvieworigin after the fragment stage's material uniforms and the emissive
 * sheet after its textures; the fragment SPIR-V with ep_shade spliced in and called before main returns, replacing
 * the colour the stock shading wrote (it reads the program's own transmap, viewdepthmap and light atlas samplers);
 * the vertex SPIR-V unchanged. A liquid stage keeps the game's own blood shading (its material names liquidhighq).
 *
 * modstock.c answers for these paths as for an archive entry, in the container SH_MODPROG_CONTAINER: a mod's
 * material that names a multilayer program brings its closure along as pointers, which the builder checks and
 * the server serves like any other installed program, and which an upload never sends.
 */
#ifndef SNAPMAP_PLUS_MODPROG_H
#define SNAPMAP_PLUS_MODPROG_H

#include <stddef.h>
#include <stdint.h>

#define SH_MODPROG_CONTAINER "snapmapplus"
#define SH_MODPROG_PREFIX    "snapmapplus/multilayer/"
#define SH_MODPROG_EP_PREFIX "snapmapplus/eternal/"

/* 1 when `path` (an archive path: '/' separated, any case) is a file the product derives, else 0. */
int sh_modprog_owns(const char *path);

/* The bytes of a product file, derived on first use: SH_MB_STOCK_OK with a process-heap copy (HeapAlloc on
 * GetProcessHeap; the caller frees it with HeapFree), SH_MB_STOCK_ABSENT for a path the product does not derive,
 * SH_MB_STOCK_UNAVAILABLE when the installed base program cannot be read or is not the one the derivation knows
 * (sh_modprog_cause says why in one sentence). */
int sh_modprog_read(const char *path, unsigned char **bytes, size_t *len);

/* Every product file path starting with `prefix` (lower case), passed to `fn`. */
void sh_modprog_list(const char *prefix, void (*fn)(void *context, const char *path), void *context);

/* Why the last derivation failed (one sentence), or "" when none has. Thread-local copy not needed: the text is
 * kept until the next failure or sh_modprog_reset. */
const char *sh_modprog_cause(void);

/* How a derivation reads the installed game: OK with a process-heap buffer, or a SH_MB_STOCK_* error. The
 * default reads through modstock (identify, then read); tests install their own. NULL restores the default. */
typedef int (*sh_modprog_reader)(const char *path, unsigned char **bytes, size_t *len);
void sh_modprog_set_reader(sh_modprog_reader reader);

/* Forget every derived file (tests). */
void sh_modprog_reset(void);

/* The i-th vector render parm the product's programs declare (the multilayer blend's, the particle shading's, the
 * radial blur's), or NULL past the last: the parms that keep their parm state slots (parmslots.h). */
const char *sh_modprog_vector_parm(unsigned i);

/* ------------------------------------------------------------------ building blocks (tests) ---- */

/* The multilayer variant of a base program's decl text (its line ends kept), or NULL when the text has no single
 * CUSTOM_TEXTURE_INPUTS template. malloc'd. */
char *sh_modprog_patch_decl(const char *text, size_t len, size_t *out_len);

/* The base fragment stage text with the blend added: bindings at or after `tex_end` up by `shift`, the
 * parameters after the material uniform block's members, the samplers before the program's first struct or
 * input, the blend's functions before main and its call after `inputs.normal = sampleNormal.wyz;`. NULL when an
 * anchor is missing. malloc'd. */
char *sh_modprog_patch_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len);

/* The base fragment module (SPIR-V words, no DOOM header) with the blend spliced in. NULL with `why` set (a
 * static sentence) when the module is not the shape the splice knows. malloc'd. */
uint32_t *sh_modprog_splice(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                            size_t *out_words, const char **why);

/* The particle program's building blocks, as the multilayer ones above: the decl with the shading's templates and
 * its call; the fragment stage text with bindings at or after `tex_end` up by `shift`, the parameters after the
 * material uniforms, the emissive sheet's sampler before the first input, ep_shade before main and its call at
 * main's end; the fragment module with ep_shade spliced in. NULL (with `why`) when the base is not the shape known. */
char *sh_modprog_ep_patch_decl(const char *text, size_t len, size_t *out_len);
char *sh_modprog_ep_patch_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len);
uint32_t *sh_modprog_ep_splice(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                               size_t *out_words, const char **why);

/* The generated module and texts (eternal/modprog_multilayer.c). */
extern const unsigned SH_MODPROG_ML_SPIRV_WORDS;
extern const unsigned SH_MODPROG_ML_SPIRV[];
extern const char SH_MODPROG_ML_GLSL[];
extern const char SH_MODPROG_ML_DECL[];

/* The blend's material keys, in the order the program reads them (uniforms, then textures). */
extern const char *const SH_MODPROG_ML_PARMS[];
extern const unsigned SH_MODPROG_ML_PARM_COUNT;
extern const char *const SH_MODPROG_ML_TEXTURES[];
extern const unsigned SH_MODPROG_ML_TEXTURE_COUNT;

/* The particle program's generated module and texts (eternal/modprog_particles.c) and its material keys. */
extern const unsigned SH_MODPROG_EP_SPIRV_WORDS;
extern const unsigned SH_MODPROG_EP_SPIRV[];
extern const char SH_MODPROG_EP_GLSL[];
extern const char SH_MODPROG_EP_DECL[];
extern const char *const SH_MODPROG_EP_PARMS[];
extern const unsigned SH_MODPROG_EP_PARM_COUNT;
extern const char *const SH_MODPROG_EP_TEXTURES[];
extern const unsigned SH_MODPROG_EP_TEXTURE_COUNT;

/* The post process's radial blur (radialblur.frag, postfx.c). The game's own `postprocess` (fragment) and
 * `computepostprocess` (compute) programs, each permutation too, are served under their own names with their five
 * clamped scene reads drawn through rb_clamped, which blurs the scene toward the radial blurs that are on (the
 * render parms radialblur0..15, which postfx.c writes each frame; with none on it is the plain read). The
 * derivation reads the installed files; a program that is not the shape known is left to the game (the cause is
 * sh_modprog_cause()). sh_modprog_post_owns names every file served this way and the radialblur parm decls;
 * sh_modprog_post_read derives on first use and returns a heap copy (SH_MB_STOCK_*). */
int sh_modprog_post_owns(const char *path);
/* 1 for the radialblur parm decls: names the game's resource index does not hold, which its existence probe must
 * find (modserve_engine.c claims them whether or not a mod is installed). */
int sh_modprog_post_claims(const char *path);
int sh_modprog_post_read(const char *path, unsigned char **bytes, size_t *len);
/* The pieces: the post decl with the blur templates and its include's scene reads taken from them (`inc` the
 * installed postprocess.inc); the stage module with its clamped scene reads turned into rb_clamped. */
char *sh_modprog_rb_patch_decl(const char *text, size_t len, const char *inc, size_t inc_len, size_t *out_len);
uint32_t *sh_modprog_rb_splice(const uint32_t *stock, size_t stock_words, int compute, size_t *out_words,
                               const char **why);
extern const unsigned SH_MODPROG_RB_SPIRV_WORDS;
extern const unsigned SH_MODPROG_RB_SPIRV[];
extern const char SH_MODPROG_RB_GLSL[];
extern const char SH_MODPROG_RB_DECL[];
extern const char *const SH_MODPROG_RB_PARMS[];      /* the material uniforms the post stage gains, in P order */
extern const unsigned SH_MODPROG_RB_PARM_COUNT;

#endif /* SNAPMAP_PLUS_MODPROG_H */
