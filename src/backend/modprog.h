/* modprog.h -- render programs the product derives from the installed game's own.
 *
 * The layered programs, snapmapplus/layered/<base> for the bases listed in modprog.c (skinvmtr and
 * skinvmtrwounds, the lit skinned virtual-texture programs a ported demon's body, head and wounds draw with), draw
 * a layered surface at draw time: the material's own layer from its virtual-texture pages, blended with up to four
 * tiled layers chosen by a mask.
 * Everything else is the base program unchanged: skinning, page sampling and feedback, lighting, shadows,
 * the wound block, the effects. The blend's rule and what it writes are in programs/shaders/layered.rp.
 *
 * Each program has ONE source, an .rp file in the render program language (programs/shaders/): a function section
 * (hlsl_fp_header) and a call section. tools/gen_programs.js derives from it the OpenGL texts (the two sections as
 * they are), the Vulkan stage text (the functions translated to GLSL, samplers named as the engine names a
 * material's textures) and the SPIR-V (the same GLSL, compiled), so the two renderers run one rule.
 *
 * Every file of such a program's closure is derived, when first asked for and then remembered for the process,
 * from the base program's installed files and the product's own sources (programs/programs_layered.c, generated from
 * programs/shaders/ by tools/gen_programs.js); nothing of the game is shipped:
 *   generated/decls/renderprog/<P>.decl and each permutation's decl: the base's text with the blend's functions in
 *     an hlsl_fp_header section (before its hlsl_fp section; at the end of its own hlsl_fp_header when it has one)
 *     and the blend's call statements as its CUSTOM_TEXTURE_INPUTS template's first statements (the OpenGL renderer
 *     compiles this text);
 *   generated/renderprogs/<P>_pc_vulkan.bin and each permutation's: the base's cooked program with that decl's
 *     checksum, its textures (each with defaultsampler) after the stage's own, the binding tables moved past them,
 *     and the fragment stage text with the blend added (what the module was compiled from, as the engine's
 *     freshness check hashes it); the fragment stage's material uniforms do not grow;
 *   the fragment SPIR-V: the base's module with the blend's functions spliced in (types and constants shared),
 *     its samplers bound after the program's own textures (the program's later bindings move up), and a call where
 *     CUSTOM_TEXTURE_INPUTS begins (right after the store of inputs.normal that ends the texture inputs); its
 *     header hashA is the MD5fold of that stage text;
 *   the vertex SPIR-V: the base's, unchanged.
 * The render parms the programs read that are textures (generated/decls/renderparm/<name>.decl) are product decls
 * too. No vector render parm is: the layered program reads its 28 vectors from a texture, the `layerparms` image
 * (an 8 x 4 RGBA16F image, vector k the texel ( k % 8, k / 8 ); sh_modprog_param_image writes it, and
 * sh_modprog_register_param_image serves it at generated/image/snapmapplus/look/<hash>.bimage), and the particle
 * program reads its constants from the keys the game's particle program already holds (particleuberparms1..3,
 * emissive) and smokebrightness, a vector no program, fx or engine code names. The product's programs take none of
 * the engine's 1,024 vector render parm slots.
 *
 * A program's textures over all its stages are bounded by the engine (ID_VULKAN_MAX_BOUND_TEXTURES, 32: VK
 * 0x141a7bbb0); the layered programs' 20 (19 images and the parameter image) bring skinvmtrwounds to exactly 32
 * (2 vertex, 10 + 20 fragment), and the derivation refuses a program past 32. OpenGL's 32 texture units per stage
 * hold the fragment stage's 30.
 *
 * The particle program, snapmapplus/particles/uberparticles (the base uberparticles, every particle material's program
 * once the engine remaps transsortblend, particleshighq and explosionhighq to it), draws a CPU particle stage with
 * unlit, lit and explosion shading, the near fade and the soft-particle distance, from the same particle vertices and
 * the stage's constants in the material (programs/shaders/particles.rp). Its closure is derived the same way: the decl
 * with the shading's functions in an hlsl_fp_header section and the call statements after its
 * PREMULTIPLY_ALPHA_BLENDED( result ); the cooked program with smokebrightness, projectionmatrixz and globalvieworigin
 * after the fragment stage's material uniforms and the emissive sheet after its textures; the fragment SPIR-V with
 * ep_shade spliced in and called before main returns, replacing the colour the stock shading wrote (it reads the
 * program's own transmap, viewdepthmap and light atlas samplers and its own particleuberparms1..3 and emissive);
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
#define SH_MODPROG_PREFIX    "snapmapplus/layered/"
#define SH_MODPROG_EP_PREFIX "snapmapplus/particles/"

/* The names these two programs had before they stopped naming a source game. Served until 1.0 for mods emitted
 * before the rename: sh_modprog_owns and sh_modprog_read answer a path under either with the bytes of the
 * new-name path; sh_modprog_list lists only the new names. */
#define SH_MODPROG_OLD_PREFIX    "snapmapplus/multilayer/"
#define SH_MODPROG_EP_OLD_PREFIX "snapmapplus/eternal/"

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

/* The i-th vector render parm the product's programs declare, or NULL past the last: the parms that keep their parm
 * state slots (parmslots.h). The programs declare none, so this is NULL for every index. */
const char *sh_modprog_vector_parm(unsigned i);

/* ------------------------------------------------------------------ the parameter image and old keys ---- */

/* A DOOM 2016 .bimage of format RGBA16F, `width` x `height`, one mip: texel i (row-major, 4 floats) as four halves,
 * round to nearest even. malloc'd; 1 on success. */
int sh_modprog_bimage_rgba16f(unsigned width, unsigned height, const float texel[][4], unsigned char **bytes,
                              size_t *len);

/* The layered program's parameter image: 8 x 4 RGBA16F, texel (k % 8, k / 8) = vec[k] for k < count (at most 32;
 * the rest 0). malloc'd; 1 on success. */
int sh_modprog_param_image(const float vec[][4], unsigned count, unsigned char **bytes, size_t *len);

/* Registers that image to be served and returns its path, generated/image/snapmapplus/look/<16 lowercase hex digits of
 * the SHA-256 of its bytes>.bimage (the same vectors give the same path), valid until sh_modprog_reset; NULL on failure.
 * sh_modprog_owns and sh_modprog_read answer the path (SH_MODPROG_CONTAINER) once it is registered, and
 * sh_modprog_list names it. */
const char *sh_modprog_register_param_image(const float vec[][4], unsigned count);

/* A material that writes the OLD keys, rewritten so that it draws the same: for a particle material the four
 * eternalparticle* vectors and eternalparticleemissivemap into particleuberparms1..3, emissive, smokebrightness and
 * particleemissivemap (merged field by field with any of those lines the material has; see particles.rp for where each
 * went), for a layered material the 28 vector keys and blendlayer0heightmap into one `layerparms` line naming the
 * registered image (snapmapplus/look/<hash>.tga) and, with vector 9 saying where layer 0's height comes from, a
 * constant (a constant image's value, else a neutral height when no extra map is free) or a free extra map
 * (multilayerextramap0 or 1) the height image takes. Returns 1 with the new text in *out (malloc'd, *out_len bytes),
 * 0 when the material writes no old key (*out NULL), -1 on failure. Registers the image it names. */
int sh_modprog_legacy_material(const char *text, size_t len, char **out, size_t *out_len);

/* ------------------------------------------------------------------ building blocks (tests) ---- */

/* The layered variant of a base program's decl text (its line ends kept): the blend's functions in its
 * hlsl_fp_header section and the call as the first statements of its CUSTOM_TEXTURE_INPUTS template. NULL when the
 * text has no single CUSTOM_TEXTURE_INPUTS template or no hlsl_fp section. malloc'd. */
char *sh_modprog_patch_decl(const char *text, size_t len, size_t *out_len);

/* The base fragment stage text with the blend added: bindings at or after `tex_end` up by `shift`, the
 * samplers before the program's first struct or input (the material uniform block does not grow), the blend's functions before main and its call after `inputs.normal = sampleNormal.wyz;`. NULL when an
 * anchor is missing. malloc'd. */
char *sh_modprog_patch_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len);

/* The base fragment module (SPIR-V words, no DOOM header) with the blend spliced in. NULL with `why` set (a
 * static sentence) when the module is not the shape the splice knows. malloc'd. */
uint32_t *sh_modprog_splice(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                            size_t *out_words, const char **why);

/* The particle program's building blocks, as the layered ones above: the decl with the shading's functions and
 * its call; the fragment stage text with bindings at or after `tex_end` up by `shift`, the three parameters after the
 * material uniforms, the emissive sheet's sampler before the first input, ep_shade before main and its call at
 * main's end; the fragment module with ep_shade spliced in. NULL (with `why`) when the base is not the shape known. */
char *sh_modprog_ep_patch_decl(const char *text, size_t len, size_t *out_len);
char *sh_modprog_ep_patch_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len);
uint32_t *sh_modprog_ep_splice(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                               size_t *out_words, const char **why);

/* The generated module and texts (programs/programs_layered.c). */
extern const unsigned SH_MODPROG_ML_SPIRV_WORDS;
extern const unsigned SH_MODPROG_ML_SPIRV[];
extern const char SH_MODPROG_ML_GLSL[];
extern const char SH_MODPROG_ML_HEADER[];   /* OpenGL: the hlsl_fp_header section's text (no braces) */
extern const char SH_MODPROG_ML_CALL[];     /* OpenGL: the statements that call it */

/* The layered program's 28 vectors, by the material keys they had as render parms: vector k of the parameter image.
 * NOT render parms (the product declares none of them). Then its textures, in the order the program reads them. */
extern const char *const SH_MODPROG_ML_PARMS[];
extern const unsigned SH_MODPROG_ML_PARM_COUNT;
extern const char *const SH_MODPROG_ML_TEXTURES[];
extern const unsigned SH_MODPROG_ML_TEXTURE_COUNT;

/* The particle program's generated module and texts (programs/programs_particles.c), the old keys of its constants
 * (SH_MODPROG_EP_PARMS: eternalparticle, eternalparticlefade, eternalparticlecolor, eternalparticlecolor2; NOT render
 * parms) and its texture. */
extern const unsigned SH_MODPROG_EP_SPIRV_WORDS;
extern const unsigned SH_MODPROG_EP_SPIRV[];
extern const char SH_MODPROG_EP_GLSL[];
extern const char SH_MODPROG_EP_HEADER[];
extern const char SH_MODPROG_EP_CALL[];
extern const char *const SH_MODPROG_EP_PARMS[];
extern const unsigned SH_MODPROG_EP_PARM_COUNT;
extern const char *const SH_MODPROG_EP_TEXTURES[];
extern const unsigned SH_MODPROG_EP_TEXTURE_COUNT;

#endif /* SNAPMAP_PLUS_MODPROG_H */
