/* Native entity-state evaluation of entity definitions (R-191).
 *
 * The engine compares entity states by writing temporary entities: entityDef
 * inheritance collapse and the game's derive-from-entity method create one
 * through game +0x448 (factory, initializer, SetEntityEditState of the
 * definition's prepared state; no registration or Spawn), write it with
 * +0x378 and delete it with +0x470. This module runs that route on a private
 * definition built the way the engine builds map entity definitions
 * (constructor, source text, Parse, which prepares +0x130 from the
 * inheritance chain), and compares the written states of a target and a
 * candidate definition. It runs only on the engine's main thread, inside the
 * process heap scope, and refuses after any engine fault. */
#ifndef BACKEND_NATIVE_DECL_EVAL_H
#define BACKEND_NATIVE_DECL_EVAL_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"
#include "package_native_inline.h"

/* Bind the private-definition and written-text routines and the main-thread
 * identity; the temporary-entity route itself comes from the verified type
 * cache. Returns 1 when evaluation is available. */
int sh_native_decl_eval_install(const sig_result *results, size_t count, const uint8_t *module_base);
int sh_native_decl_eval_available(void);

/* Written native state of one private definition text
 * ({ inherit = ...; class = ...; edit = { ... } }). 1 with malloc-owned text;
 * 0 when not evaluable here (unavailable, off-thread, the class is not
 * instantiable, the definition was not accepted); -1 on an engine fault,
 * after which evaluation stays disabled. */
int sh_native_decl_eval_state(const char *definition, char **state, char *detail, size_t capacity);

/* Compare the written states of two definitions. Their inherit names resolve
 * through the engine's global lookup at Parse (VK 0x17ae9d0 stores the parent
 * at +0x160), so callers pass names only where the engine serves the intended
 * source. layers (nearest first) are a target chain's definitions the engine
 * does not serve with that source; they would have to be linked privately.
 * The temporary read applies only a definition's own prepared text
 * (+0x140/+0x138, VK 0x17ad250/0x17ad260) after the instance initializer
 * (+0x18), whose parent contract is unrecovered, so a non-empty chain is
 * declined as UNAVAILABLE rather than guessed. */
sh_native_eval_status sh_native_decl_eval_compare(const sh_native_eval_layer *layers, size_t layer_count,
    const char *target, const char *candidate, char *detail, size_t capacity);

#ifdef SH_NATIVE_DECL_EVAL_TESTING
typedef struct sh_native_decl_eval_native {
    void **game_access;
    void (*context_ctor)(void *context, int zero);
    void (*context_args)(void *args, int a, int b);
    void (*context_reset)(void *context);
    void (*context_dtor)(void *context);
    const char *(*context_text)(void *context);
    void (*decl_ctor)(void *decl);
    void (*decl_set_text)(void *decl, const char *text, int length);
    unsigned int (*decl_parse)(void *decl, unsigned char read_source, void *error_text);
    void (*idstr_ctor)(void *text);
    void (*idstr_dtor)(void *text);
    const volatile uint32_t *main_thread;
} sh_native_decl_eval_native;
void sh_native_decl_eval_test_bind(const sh_native_decl_eval_native *native);
#endif

#endif
