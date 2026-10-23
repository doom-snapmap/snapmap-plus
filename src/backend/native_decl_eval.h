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

/* Compare two definitions by the state the engine prepares for them. Parse
 * ends by storing entityStateWithInheritanceText at +0x130 (VK 0x17ae9d0 /
 * GL 0x17a14b0). expandInheritance defaults to 1 in the constructor (both
 * images); then the chain's own state texts merge root-first through the
 * global name lookup and the engine writer prints the merged tree. With
 * expandInheritance = false the text is the definition's own state only.
 * Spawn and the temporary read apply that one text to a freshly constructed
 * instance; the initializer applies no parent state. Parse otherwise inherits
 * only the class name and editor variables. Identical class, instantiable
 * flag and prepared text therefore give the engine identical reader input:
 * EQUAL. Differing classes or instantiable flags are UNEQUAL. The other
 * editor variables are editor presentation that no gameplay route reads
 * (R-201): a difference there alone stays EQUAL, named in the detail. A
 * differing pool or inheritance setting is UNSUPPORTED (named in the
 * detail). Differing prepared text is UNSUPPORTED,
 * because distinct text can still read to the same state. No temporary entity is created, so no registration, interned
 * reference or SetEntityEditState error path is reached. Names resolve
 * through the engine's lookup, so callers pass names only where the engine
 * serves the intended source. layers (nearest first) are the target chain's
 * definitions from its parent up to the topmost one the engine does not serve
 * with the document's source; the topmost layer's parent must be served as
 * intended. Each is parsed privately, root-first, by the engine's own Parse;
 * its name then answers Parse's and the inheritance merge's parent lookups
 * (redirected call sites) on this thread only, so class, editor variables and
 * the root-first per-layer merge with its post-pass are the engine's. The
 * candidate is parsed with no private names visible. A layer the engine
 * refuses makes the chain UNSUPPORTED with its name; layers are UNAVAILABLE
 * where the lookups are not redirected. Parsing runs only while the
 * parser-state owner (pointer decoded from DeclParseSource +0x19a; slot +0xb0
 * sets +0x2c34, which the parser brackets with 0 and 15) is idle at 15, so
 * the bracket restores the prior state. */
sh_native_eval_status sh_native_decl_eval_compare(const sh_native_eval_layer *layers, size_t layer_count,
    const char *target, const char *candidate, char *detail, size_t capacity);
/* The same comparison; when the prepared texts differ and reduction is given,
 * its proved complete-assignment reduction (R-212) is applied to both texts and
 * their canonical results must be identical. Class, instantiable and member
 * checks are unchanged. */
sh_native_eval_status sh_native_decl_eval_compare_reduced(const sh_native_eval_layer *layers, size_t layer_count,
    const char *target, const char *candidate, const sh_native_eval_reduction *reduction, char *detail, size_t capacity);

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
    void **parse_state_owner;
    void *(*lookup)(void *list, const char *name, char make_default);
    void *entitydef_list;
    int private_chains;
} sh_native_decl_eval_native;
void sh_native_decl_eval_test_bind(const sh_native_decl_eval_native *native);
/* What the redirected parent-lookup calls reach. */
void *sh_native_decl_eval_test_lookup(void *list, const char *name, char make_default);
/* Redirect the two parent-lookup calls named by these results, as the first
 * install of a process when first is set, else as a later one; 1 when private
 * chains are enabled. */
int sh_native_decl_eval_test_install_chains(const sig_result *r, size_t n, const uint8_t *base, int first);
#endif

#endif
