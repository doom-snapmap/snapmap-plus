/* Read-only view of the engine's idClass construction registry.
 *
 * Construction resolves an entityDef class through this registry, not through
 * the reflection database Parse uses (R-221): CreateManagedClassFromDecl
 * (VK 0x14030ce40 / GL 0x14030c650) looks the name up with GetClass
 * (VK 0x1403dd9b0 / GL 0x1403dd5e0), requires the record's typeNum (+0x68) to
 * lie within idManagedClass::Type's [typeNum, lastChild], then calls the
 * record's CreateInstance (+0x28); an abstract record (+0x60) registers a
 * factory that warns and returns NULL. Nothing here calls a factory or any
 * other native code. */
#ifndef SH_NATIVE_CLASS_REGISTRY_H
#define SH_NATIVE_CLASS_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

/* Addresses of the bound globals (engine_globals_table.gen.h:
 * class_registry_ready, class_sorted_types, class_sorted_count,
 * managed_class_first, managed_class_last). read is a guarded copy that
 * returns 1 when every byte was read. It need not know object bounds (the
 * process source copies any mapped memory), so a record name is read only up
 * to its first difference from the key or its terminator. A zero address
 * leaves the registry unavailable. */
/* Effective entity-pool settings, the integer values of the session cvars
 * the pooled construction routes read when a pool is created:
 * g_entityPoolTypeInfoClone selects pool creation's template branch
 * (VK 0x1408293d0 / GL 0x140828220; 0 spawns each entity individually and
 * skips a NULL result), g_entityPoolMode the generic spawn's pool for a
 * definition's own poolCount (0 none, 1 per definition, 2 per type, whose
 * template list builds its first instance unconditionally), and
 * ai_pool_enable world AI pool setup (VK 0x14069d930). */
typedef struct sh_native_pool_settings {
    int type_info_clone, pool_mode, ai_pool_enable;
} sh_native_pool_settings;

typedef struct sh_native_class_source {
    void *context;
    int (*read)(void *context, uintptr_t address, void *destination, size_t length);
    uintptr_t ready, types, count, managed_first, managed_last;
    /* Optional: reads the current pool settings; 1 when all three were read.
     * NULL or a failed read leaves them unavailable. Never writes a cvar. */
    int (*pool_settings)(void *context, sh_native_pool_settings *out);
} sh_native_class_source;

typedef struct sh_native_class {
    int type_num, last_child;
    int abstract_class; /* +0x60: the registered factory returns NULL */
    int managed;        /* typeNum within idManagedClass::Type's range */
    uintptr_t factory;  /* +0x28 CreateInstance, never called */
} sh_native_class;

/* GetClass after idClass::Init: binary search of the sorted type array with
 * idStr::Cmp (signed bytes). Before Init (static construction still running)
 * and after Shutdown (arrays freed, readiness cleared) the registry is
 * unready. 1 present, 0 conclusively absent from the initialized registry,
 * -1 unavailable (unbound, unready, unreadable, or changed during the read). */
int sh_native_class_lookup(const sh_native_class_source *source, const char *name, sh_native_class *out);

/* What CreateManagedClassFromDecl does with this class, in its own order.
 * ELIGIBLE means it calls a registered concrete factory; that is metadata
 * eligibility, not proof that allocation or the factory cannot fail. */
typedef enum sh_native_construction {
    SH_NATIVE_CONSTRUCTION_UNAVAILABLE = -1,
    SH_NATIVE_CONSTRUCTION_ELIGIBLE = 0,
    SH_NATIVE_CONSTRUCTION_NO_CLASS,   /* "No class name." */
    SH_NATIVE_CONSTRUCTION_ABSENT,     /* "Class '%s' not found." */
    SH_NATIVE_CONSTRUCTION_UNMANAGED,  /* "Class is not derived from idManagedClass." */
    SH_NATIVE_CONSTRUCTION_ABSTRACT    /* "Cannot instantiate abstract class %s." (factory returns NULL) */
} sh_native_construction;

sh_native_construction sh_native_class_construction(const sh_native_class_source *source,
    const char *name, sh_native_class *out);
/* Static text: why the native constructor returns NULL, or the state. */
const char *sh_native_construction_text(sh_native_construction construction);

/* The process registry, bound once at installation from resolved globals.
 * Reads are SEH-guarded. Never NULL: unbound or unresolved globals make every
 * lookup unavailable. */
void sh_native_class_bind_process(uintptr_t ready, uintptr_t types, uintptr_t count,
    uintptr_t managed_first, uintptr_t managed_last);
const sh_native_class_source *sh_native_class_process(void);

/* The current pool settings of a source: 1 read, 0 unavailable (no reader,
 * unreadable or unregistered cvar). The process source finds the three cvars
 * by name in the live cvar registry (cvar_system_slot, bound once at
 * installation; zero leaves them unavailable) and reads value +0x30. */
int sh_native_pool_settings_read(const sh_native_class_source *source, sh_native_pool_settings *out);
void sh_native_pool_bind_process(uintptr_t cvar_system_slot);

/* Read-only native name-event metadata. dispatch_verified means the caller
 * established the supported native idEventReceiver dispatch contract (R354,
 * paired executable identities); registry resemblance alone does not prove
 * what an event does. The decl-manager accessor and event-index/count methods
 * are decoded and checked, never called. No default or native object is made. */
typedef struct sh_native_event_source {
    void *context;
    int (*read)(void *context, uintptr_t address, void *destination, size_t length);
    uintptr_t decl_manager;
    int dispatch_verified;
} sh_native_event_source;
/* 1 only for native findEntity/getName with expected name, number, result
 * type, type-0 dispatch and no notification alias, stable across the read.
 * The proved reflected timeline-component execution discards its local
 * result. The caller must restrict the proof to that execution context; it
 * does not cover direct Execute results or turn arbitrary strings and
 * event-shaped objects into verified event calls.
 * Every unknown, overridden or unreadable record returns 0. */
int sh_native_event_discarded_name(const sh_native_event_source *source, const char *name);
/* The native dropEntity declaration argument (index zero, idDeclEntityDef)
 * constructs through the ordinary spawn service. Its full live argument
 * format, arity, type signature, kind, number and alias must still match.
 * argument_count is the authored args.num, not the native arity. The borrowed
 * argument_signature ends after the highest supplied index: native reader
 * letters, '0' for an explicit NULL, '?' for an omitted slot. Omitted slots
 * retain the native existing/default value; partial argument blocks are valid.
 * NULL/unknown metadata, a mismatched supplied reader or an out-of-range slot
 * proves nothing. No event or argument is removed or otherwise changed. */
int sh_native_event_decl_constructs(const sh_native_event_source *source, const char *event_name,
    size_t argument_index, size_t argument_count, const char *argument_signature, const char *decl_class);

#endif
