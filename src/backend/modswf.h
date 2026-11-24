/* modswf.h -- serves installed mods' cooked Flash movies (.bswf) and a SnapMap module's per-map files to the
 * engine's file-resource loader. One vtable-slot swap of idFileResource +0x40 ("FileResourceLoad") makes the
 * override provider answer a served request at any load state (the engine's own path reaches it only below 2). */
/* BuildSoundWorld (VK 0x141151440 / GL 0x141150640), idUmbraContext::SetTome (0x14181a4b0 / 0x14181b280) and
 * idAmbientLighting::LoadAmbientLighting (0x1418a1f20 / 0x1418983e0) materialize an idFileResource at Play
 * (state 2 or above), so a mod-served .sbsp, .ambientsh, .tome or lightprobes/ .bimage takes the movie route. */
#ifndef SH_MODSWF_H
#define SH_MODSWF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Byte offsets inside the FileResourceLoad body (identical on both build
 * 13954591 executables). */
#define SH_MODSWF_OFF_STATE_CALL  0x24u   /* E8: lifecycle state getter */
#define SH_MODSWF_OFF_NEW_CALL    0xAEu   /* E8: operator new(0x180), below-2 branch */
#define SH_MODSWF_OFF_CTOR_CALL   0xC4u   /* E8: named idFile_Memory constructor */
#define SH_MODSWF_OFF_TAIL        0xECu   /* MOV [RSP+0x20],RBP ... MOV RCX,RBX */
#define SH_MODSWF_OFF_OPEN_CALL   0xFFu   /* E8: open-into helper */
#define SH_MODSWF_BODY_BYTES      0x104u  /* through the open-into call */
#define SH_MODSWF_LOAD_SLOT       0x40u   /* idFileResource vtable slot */

/* Engine functions decoded from one Load body. */
typedef struct sh_modswf_engine {
    uintptr_t state;       /* int(void) */
    uintptr_t alloc;       /* void *(size_t), the engine's operator new */
    uintptr_t named_ctor;  /* void *(void *mem, const char *name) */
    uintptr_t open_into;   /* uint8(resource, idFile **slot, name, char must_exist, arg) */
} sh_modswf_engine;

/* 1 when `name` is a cooked movie request: generated/swf/...<x>.bswf, any
 * case, either separator. Pure. */
int sh_modswf_is_movie(const char *name);

/* 1 for a module file the route serves: a .sbsp, .ambientsh or .tome, or a
 * .bimage under a lightprobes/ folder. Case and separators are ignored. */
int sh_modswf_is_module_file(const char *name);

/* Decode the four engine calls from a resolved Load body and verify the
 * fixed bytes between them (the state test, the +0x68 manager call, the
 * 0x180 allocations and the open-into argument setup). 1 on success. */
int sh_modswf_decode_load(const uint8_t *load, sh_modswf_engine *out);

/* 1 when the open-into helper opens through the file system's +0x148 slot
 * (CALL [RAX+0x148] within its first 0x40 bytes). */
int sh_modswf_open_into_uses_provider(const uint8_t *open_into);

/* The address of the only 8-aligned qword equal to `value` inside a
 * read-only, non-executable section of the PE at `module_base`, or NULL when
 * there is none or more than one. */
void **sh_modswf_find_slot(const uint8_t *module_base, uint64_t value);

#ifndef SH_MODSWF_PURE
#include "signatures.h"
/* Resolve FileResourceLoad (SIG_OK only), decode it, find its vtable slot and
 * swap it. Call after the override provider is installed. Returns 1 when the
 * route is live. */
int sh_modswf_install(const sig_result *results, size_t count, const uint8_t *module_base);
#endif

#ifdef SH_MODSWF_TESTING
/* Drive the replacement Load against synthetic engine functions. `lookup`
 * stands in for sh_modserve_lookup (returns 1 for a served request). */
typedef int64_t (*sh_modswf_test_load_fn)(void *resource, void *arg);
void sh_modswf_test_bind(const sh_modswf_engine *engine, sh_modswf_test_load_fn original,
                         int (*lookup)(const char *request, const char **bundle_id));
int64_t sh_modswf_test_load(void *resource, void *arg);
#endif

#ifdef __cplusplus
}
#endif
#endif
