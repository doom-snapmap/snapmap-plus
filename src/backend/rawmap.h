/* Rawmap load substitution and save mirroring. The load detour can replace
 * engine JSON with rawmap.json, then calls the native deserializer. Package
 * gating, marker migration and navigation preparation apply to both normal
 * and substituted loads.
 */
#ifndef BACKEND_RAWMAP_H
#define BACKEND_RAWMAP_H

#include <stdint.h>
#include <stddef.h>

/* Install the DeserializeFromJson detour only for a clean SIG_OK resolution.
 * An existing inline hook cannot supply the original stolen instructions.
 * Returns 1 when installed, otherwise logs the refusal and returns 0.
 */
int sh_rawmap_swap_install(void *deser_fn, int deser_status_ok);

/* Set the shared load/save arm state; default off. Returns the new state. A
 * sibling arm.flag also arms both paths for testing and follows the
 * configured source directory.
 */
int sh_rawmap_swap_arm(int on);

/* Read the explicit arm state only; the test flag is reported separately. */
int sh_rawmap_swap_is_armed(void);

/* Read the effective arm predicate: explicit state OR arm.flag. Check before
 * engine-direct deserialization; source availability is checked later.
 */
int sh_rawmap_swap_will_fire(void);

/* Set the file-backed load source. NULL restores the default rawmap.json
 * path. Returns 1 if accepted.
 */
int sh_rawmap_swap_set_source(const char *path);

/* How many times the swap has fired (substituted our bytes into a load). */
unsigned long sh_rawmap_swap_count(void);

/* How many substituted DeserializeFromJson calls have RETURNED. Exported by name and at ordinal 101
 * so the test harness can distinguish a completed in-place load even when the engine reuses the
 * idSnapMap pointer and emits no completion line. */
unsigned long sh_rawmap_swap_complete_count(void);

/* Save processing calls native SerializeToJson, preserves its bool return,
 * embeds package/navigation payloads into the output, then optionally mirrors
 * JSON to disk. Mirroring uses the same arm predicate as loading.
 * sh_pretty_on changes only the mirror's layout.
 */
/* Install the SerializeToJson detour only for a clean SIG_OK resolution.
 * Returns 1 when installed, otherwise logs the refusal and returns 0.
 */
int sh_rawmap_save_install(void *serialize_fn, int serialize_status_ok);

/* Resolve native idStr assignment for package/navigation embedding. Without
 * it, save-output replacement is unavailable.
 */
void sh_rawmap_embed_install(const void *module_base);

/* Set the mirror destination. NULL restores the default
 * %LOCALAPPDATA%/snapmap-plus/rawmap.json. The default matches the load
 * source; explicit overrides are independent. Returns 1 if accepted.
 */
int sh_rawmap_save_set_dest(const char *path);

/* Count successful save mirrors for diagnostics. */
unsigned long sh_rawmap_save_count(void);

/* Bytes in the most recent successful mirror, or 0 before any write. */
unsigned long long sh_rawmap_save_last_bytes(void);

/* Main-thread read-only serialization, bypassing save side effects. The caller
 * constructs and destroys the output idStr with the engine's own helpers. */
int sh_rawmap_snapshot(void *editor_serializer, void *map, void *out_idstr);

#endif /* BACKEND_RAWMAP_H */
