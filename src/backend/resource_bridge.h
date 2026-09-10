/* resource_bridge.h -- sparse read-only access to installed DOOM resources. */
#ifndef BACKEND_RESOURCE_BRIDGE_H
#define BACKEND_RESOURCE_BRIDGE_H

#include <stddef.h>

enum {
    SH_RESOURCE_BRIDGE_ERROR = -1,
    SH_RESOURCE_BRIDGE_MISS = 0,
    SH_RESOURCE_BRIDGE_OPENED = 1
};

/* Capture and validate resources/*.manifest from installed packages. Each
 * tab-separated row names a decl type, logical name and installed virtual
 * path resolved through gameresources.pindex. Multiple resource paths may
 * belong to one identity; each provider path must be unambiguous. Duplicate
 * pindex rows require identical stored payloads. Archives stay read-only;
 * compressed slices with zero decoded bytes are refused. Capture publishes
 * READY only after validation; recapture explicitly reopens the state.
 */
int sh_resource_bridge_capture(const char *data_root);

/* Refresh manifests after package changes. Retain the previous entry
 * allocations for existing readers; each recapture adds retained memory. Call
 * at a serialized, quiescent boundary with no map loading. Returns the new
 * capture result.
 */
int sh_resource_bridge_recapture(const char *data_root);

/* Mark whether the provider hook that can serve captured entries is live. A
 * non-empty bridge snapshot gates dynamic decl registration until this is 1. */
void sh_resource_bridge_set_provider_ready(int ready);
int sh_resource_bridge_gate_ok(void);
int sh_resource_bridge_has_manifests(void);
size_t sh_resource_bridge_entry_count(void);

/* Resolve an engine resource name. OPENED transfers one HeapAlloc-owned buffer
 * to the caller; MISS means the name is not admitted; ERROR means it was
 * admitted but its installed source could not be read or decoded. */
int sh_resource_bridge_open(const char *name, unsigned char **out,
                            size_t *out_length, const char **out_source);

/* Linked .decl entries are exposed to the dynamic decl server without writing
 * their game-owned bytes into the user's override tree. */
size_t sh_resource_bridge_decl_count(void);
int sh_resource_bridge_decl_metadata(size_t index, const char **type,
                                     const char **name, const char **source);
int sh_resource_bridge_read_decl(size_t index, char **body, size_t *length,
                                 const char **reason);

#ifdef SH_RESOURCE_BRIDGE_TESTING
void sh_resource_bridge_test_set_doom_base(const char *path);
void sh_resource_bridge_test_reset(void);
int sh_resource_bridge_test_entry_metadata(size_t index, const char **alias,
                                           size_t *decoded_bytes,
                                           size_t *stored_bytes,
                                           const char **source);
#endif

#endif /* BACKEND_RESOURCE_BRIDGE_H */
