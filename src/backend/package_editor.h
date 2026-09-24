/* Package provenance at verified native editor object lifetime boundaries. */
#ifndef SH_PACKAGE_EDITOR_H
#define SH_PACKAGE_EDITOR_H
#include "package_occurrence.h"
#include "signatures.h"

enum { SH_PACKAGE_EDITOR_MAP = 1, SH_PACKAGE_EDITOR_PREFAB = 2, SH_PACKAGE_EDITOR_ENTITY = 3 };
/* Called only from native editor work on its own thread. Inspect exact native
 * serialization and return a complete document snapshot. No partial ownership
 * is attached on failure. previous is borrowed for this call. */
typedef int (*sh_package_editor_inspect_fn)(void *context, unsigned kind, void *flat,
    const sh_package_occurrence_snapshot *previous, sh_package_occurrence_snapshot **next,
    char *error, size_t capacity);
void sh_package_editor_bind(sh_package_editor_inspect_fn inspect, void *context);
/* Install the production serializer/typed-reader adapter. */
void sh_package_editor_bind_native(void);
/* Native targets must be quiescent during install/removal. Every target and
 * complete stolen prologue is verified before any detour is committed. */
int sh_package_editor_install(const sig_result *results, size_t count, const uint8_t *base);
int sh_package_editor_uninstall(void);
int sh_package_editor_ready(void);
/* A failed ownership transfer is never silently treated as vanilla on save. */
int sh_package_editor_healthy(char *error, size_t capacity);
/* MAP: flat idSnapMap; PREFAB: idSnapEntityPrefab; ENTITY: idSnapEntity.
 * Snapshot order comes from the actual contiguous native array, never uniqueId.
 * Returned document is owned; attach borrows it. Caller serializes with native
 * mutation. Map world ownership is included only for MAP operations. */
sh_package_occurrence_snapshot *sh_package_editor_snapshot(unsigned kind, const void *flat,
    char *error, size_t capacity);
int sh_package_editor_attach(unsigned kind, const void *flat,
    const sh_package_occurrence_snapshot *document, char *error, size_t capacity);
int sh_package_editor_inspect(unsigned kind, void *flat, char *error, size_t capacity);
/* Retire current world policy after confirmed map departure. Clipboard/undo
 * bodies retain their own snapshots until their native destructors execute. */
void sh_package_editor_clear_world(void);
/* Caller retains this across a native load transaction and restores it only
 * when that load rolls back. Returned handle is owned; restore borrows. */
sh_package_occurrence_snapshot *sh_package_editor_world(void);
void sh_package_editor_restore_world(sh_package_occurrence_snapshot *world);
#endif
