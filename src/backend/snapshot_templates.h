/* Multiplayer snapshot templates for package rows a map does not reach (R-398 perf17).
 *
 * idGameLocal::InitFromNewMap (Vulkan 0x140323680, OpenGL 0x140323060), in a networked game, copies the
 * entityDef resource list (idResourceList get-all, Vulkan 0x141801040 / OpenGL 0x1417f3990, called at
 * Vulkan 0x140324ca6 / OpenGL 0x140324686 and again at +0x11e when a template loads more definitions)
 * and spawns one temporary entity for every entry that is not an idStaticEntity and has no template yet,
 * to record its snapshot template. Shipped content reaches that list only when the map's load needs it:
 * an unused derivation is a level-1 resource the previous unload purged. Package rows registered for the
 * whole library stay in the list, so every multiplayer map load (Play, coop, published lobbies) spawned
 * a temporary entity for every private entityDef of every installed package.
 *
 * The filter presents the list the way shipped content would: it clears (sets to NULL, which both
 * native loops skip, so the native count check still sees the live count) every private package
 * entityDef ("smp/" + 64 hex digits) that the served view does not reach. Reached means named, through
 * declaration text, from a served row with an engine name (what any map can name) or from a row the
 * current map delivers; rows with engine names are never touched. A cleared row stays registered and
 * spawnable by name; like a definition loaded after template creation, it simply has no template.
 * SMP_SNAPSHOT_TEMPLATE_FILTER=0 restores the native list. */
#ifndef BACKEND_SNAPSHOT_TEMPLATES_H
#define BACKEND_SNAPSHOT_TEMPLATES_H

#include <stddef.h>
#include <stdint.h>

struct sh_package_compilation;

/* Resolve the "SnapshotTemplateList" signature in the host image, verify both call sites and the
 * get-all prologue, and detour get-all. 1 installed (or already), 0 not recognized. */
int sh_snapshot_templates_install(void);

/* 1 when name is a private package name: "smp/" followed by exactly 64 lowercase hex digits. */
int sh_snapshot_templates_private_name(const char *name);

/* The private names the served view reaches (see above), as sorted 32-byte digests. */
typedef struct sh_snapshot_reach {
    unsigned char (*digests)[32];
    size_t count, capacity;
    size_t roots, scanned;  /* rows scanned as roots, rows scanned in all */
} sh_snapshot_reach;
int sh_snapshot_reach_build(const struct sh_package_compilation *compiled, sh_snapshot_reach *out);
int sh_snapshot_reach_contains(const sh_snapshot_reach *reach, const char *name);
void sh_snapshot_reach_free(sh_snapshot_reach *reach);

/* Clear the unreached private entries of a native resource list (entries[i] points at an idResource
 * whose name pointer is at +8). Returns the number cleared. */
size_t sh_snapshot_templates_filter(void **entries, int count, const sh_snapshot_reach *reach);

#ifdef SH_SNAPSHOT_TEMPLATES_TESTING
typedef void (*sh_snapshot_list_all_fn)(void *list, void *out, char sort);
/* Bind a fake get-all, list and the two return addresses the detour filters at. */
void sh_snapshot_templates_test_bind(sh_snapshot_list_all_fn original, void *list,
    const void *first_return, const void *second_return);
/* The detour as the engine calls it, with the return address it would see. */
void sh_snapshot_templates_test_call(void *list, void *out, char sort, const void *return_address);
void sh_snapshot_templates_test_reset(void);
#endif

#endif /* BACKEND_SNAPSHOT_TEMPLATES_H */
