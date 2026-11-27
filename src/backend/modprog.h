/* modprog.h -- render programs the product derives from the installed game's own: the layered, particle and hair
 * variants, each from one .rp source (programs/shaders/) through tools/gen_programs.js, with nothing of the game shipped.
 * Every file of a program's closure (decl, cooked program, SPIR-V) is derived on first use from the base program's
 * installed files and remembered for the process; modstock.c serves the paths in the container SH_MODPROG_CONTAINER.
 * A program's textures over all stages are bounded by the engine (ID_VULKAN_MAX_BOUND_TEXTURES, 32: VK 0x141a7bbb0),
 * and the derivation refuses a program past 32. */
#ifndef SNAPMAP_PLUS_MODPROG_H
#define SNAPMAP_PLUS_MODPROG_H

#include <stddef.h>
#include <stdint.h>

#define SH_MODPROG_CONTAINER "snapmapplus"
#define SH_MODPROG_PREFIX    "snapmapplus/layered/"
#define SH_MODPROG_EP_PREFIX "snapmapplus/particles/"
#define SH_MODPROG_EP_EXT_PREFIX "snapmapplus/particles-volume/"
#define SH_MODPROG_EP_EROSION_PREFIX "snapmapplus/particles-erosion/"
#define SH_MODPROG_HR_PREFIX "snapmapplus/hair/"

#define SH_MODPROG_OLD_PREFIX "snapmapplus/multilayer/"

/* Resolves a historical particle path only when checking a published unit's pinned bytes. Caller frees the result. */
char *sh_modprog_pinned_path(const char *path);

/* 1 when `path` (an archive path: '/' separated, any case) is a file the product derives, else 0. */
int sh_modprog_owns(const char *path);

/* The bytes of a product file, derived on first use: SH_MB_STOCK_OK with a process-heap copy (the caller frees it with
 * HeapFree), SH_MB_STOCK_ABSENT for a path the product does not derive, SH_MB_STOCK_UNAVAILABLE when the base program
 * cannot be read or is not the one the derivation knows (sh_modprog_cause says why). */
int sh_modprog_read(const char *path, unsigned char **bytes, size_t *len);

/* Every product file path starting with `prefix` (lower case), passed to `fn`. */
void sh_modprog_list(const char *prefix, void (*fn)(void *context, const char *path), void *context);

/* Why the last derivation failed (one sentence), or "" when none has; kept until the next failure or reset. */
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

/* A DOOM 2016 .bimage of format RGBA16F, `width` x `height`, one mip: texel i (row-major, 4 floats) as four halves,
 * round to nearest even. malloc'd; 1 on success. */
int sh_modprog_bimage_rgba16f(unsigned width, unsigned height, const float texel[][4], unsigned char **bytes,
                              size_t *len);

/* The layered program's parameter image: 8 x 4 RGBA16F, texel (k % 8, k / 8) = vec[k] for k < count (at most 32;
 * the rest 0). malloc'd; 1 on success. */
int sh_modprog_param_image(const float vec[][4], unsigned count, unsigned char **bytes, size_t *len);

/* Registers that image to be served and returns its path, generated/image/snapmapplus/look/<16 hex digits of the
 * SHA-256 of its bytes>.bimage (the same vectors, the same path), valid until reset; NULL on failure. */
const char *sh_modprog_register_param_image(const float vec[][4], unsigned count);

/* Registers a constant image (4 x 4 RGBA16F, every texel `rgba`) and returns its path,
 * generated/image/snapmapplus/look/c<16 hex digits of the SHA-256 of its bytes>.bimage; served as the parameter image is. */
const char *sh_modprog_register_const_image(const float rgba[4]);
const char *sh_modprog_register_ribbon_mask(float fade_in, float fade_out);
const char *sh_modprog_register_ribbon_age(const float length[2], const float life[2], const float colours[8]);

/* Registers `text` as an SDK decl of `type` (any case) to be served at generated/decls/<type lower case>/<name>.decl and
 * returns its name, snapmapplus/sdk/<16 hex digits of the SHA-256 of type and text> (snapmapplus_sdk_<hex> for a table,
 * whose name has no '/'). The same text gives the same name; valid until reset; NULL on failure. */
const char *sh_modprog_register_decl(const char *type, const char *text);

/* An SDK effect's emitter entityDef (inherit func/fx, fxEffect the fx), served at
 * generated/decls/entitydef/snapmapplus/effect/<fx name>.decl. malloc'd. */
char *sh_modprog_effect_emitter(const char *fx_name);

/* Rewrites old particle and layered vector keys into particleuberparms1..3, emissive, smokebrightness and one
 * `layerparms` line naming the registered image. Returns 1 with the text in *out (malloc'd), 0 when no old key is
 * written (*out NULL), -1 on failure. */
int sh_modprog_legacy_material(const char *text, size_t len, char **out, size_t *out_len);
int sh_modprog_pinned_material(const char *text, size_t len, char **out, size_t *out_len);

/* The layered variant of a base program's decl text (its line ends kept): the blend's functions in its
 * hlsl_fp_header section and the call as the first statements of its CUSTOM_TEXTURE_INPUTS template. NULL when the
 * text has no single CUSTOM_TEXTURE_INPUTS template or no hlsl_fp section. malloc'd. */
char *sh_modprog_patch_decl(const char *text, size_t len, size_t *out_len);

/* The base fragment stage text with the blend added: bindings at or after `tex_end` up by `shift`, the samplers before
 * the first struct or input, the blend's functions before main and its call after `inputs.normal = sampleNormal.wyz;`.
 * NULL when an anchor is missing. malloc'd. */
char *sh_modprog_patch_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len);

/* The base fragment module (SPIR-V words, no DOOM header) with the blend spliced in. NULL with `why` set (a
 * static sentence) when the module is not the shape the splice knows. malloc'd. */
uint32_t *sh_modprog_splice(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                            size_t *out_words, const char **why);

/* The particle program's building blocks, as the layered ones: ep_shade and its call added to the decl, the fragment
 * stage text (plus the three parameters and the emissive sampler) and the fragment module. NULL when the base differs. */
char *sh_modprog_ep_patch_decl(const char *text, size_t len, size_t *out_len);
char *sh_modprog_ep_patch_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len);
uint32_t *sh_modprog_ep_splice(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                               size_t *out_words, const char **why);

/* The hair programs' building blocks, as the particle ones, with hr_fade and its call and the deathfadeopacity and noise
 * sampler additions. NULL when an anchor is missing or the program already samples the noise. */
char *sh_modprog_hr_patch_decl(const char *text, size_t len, size_t *out_len);
char *sh_modprog_hr_patch_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len);
uint32_t *sh_modprog_hr_splice(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                               size_t *out_words, const char **why);

/* The generated module and texts (programs/programs_layered.c). */
extern const unsigned SH_MODPROG_ML_SPIRV_WORDS;
extern const unsigned SH_MODPROG_ML_SPIRV[];
extern const char SH_MODPROG_ML_GLSL[];
extern const char SH_MODPROG_ML_HEADER[];   /* OpenGL: the hlsl_fp_header section's text (no braces) */
extern const char SH_MODPROG_ML_CALL[];     /* OpenGL: the statements that call it */

/* The layered program's 28 vector keys (vector k of the parameter image; not render parms), then its textures in
 * the order the program reads them. */
extern const char *const SH_MODPROG_ML_PARMS[];
extern const unsigned SH_MODPROG_ML_PARM_COUNT;
/* Each vector's value when a material does not set it. */
extern const float SH_MODPROG_ML_DEFAULTS[28][4];
extern const char *const SH_MODPROG_ML_TEXTURES[];
extern const unsigned SH_MODPROG_ML_TEXTURE_COUNT;

/* The particle program's generated module and texts (programs/programs_particles.c), the keys of its constants
 * (not render parms) and its texture. */
extern const unsigned SH_MODPROG_EP_SPIRV_WORDS;
extern const unsigned SH_MODPROG_EP_SPIRV[];
extern const char SH_MODPROG_EP_GLSL[];
extern const char SH_MODPROG_EP_HEADER[];
extern const char SH_MODPROG_EP_CALL[];
extern const unsigned SH_MODPROG_EP_BASE_SPIRV_WORDS;
extern const unsigned SH_MODPROG_EP_BASE_SPIRV[];
extern const char SH_MODPROG_EP_BASE_GLSL[];
extern const char SH_MODPROG_EP_BASE_HEADER[];
extern const unsigned SH_MODPROG_EP_EROSION_SPIRV_WORDS;
extern const unsigned SH_MODPROG_EP_EROSION_SPIRV[];
extern const char SH_MODPROG_EP_EROSION_GLSL[];
extern const char SH_MODPROG_EP_EROSION_HEADER[];
extern const char *const SH_MODPROG_EP_TEXTURES[];
extern const unsigned SH_MODPROG_EP_TEXTURE_COUNT;

/* The hair programs' generated module and texts (programs/programs_hair.c) and the texture they gain. */
extern const unsigned SH_MODPROG_HR_SPIRV_WORDS;
extern const unsigned SH_MODPROG_HR_SPIRV[];
extern const char SH_MODPROG_HR_GLSL[];
extern const char SH_MODPROG_HR_HEADER[];
extern const char SH_MODPROG_HR_CALL[];
extern const char *const SH_MODPROG_HR_TEXTURES[];
extern const unsigned SH_MODPROG_HR_TEXTURE_COUNT;

#endif /* SNAPMAP_PLUS_MODPROG_H */
