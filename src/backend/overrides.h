/* Resource-provider file shadowing through the open-by-name vtable slot
 * (+0xf8). Private Grid Room resources are generated from installed originals.
 * Ordinary opens try map navigation, exact published decltree entries,
 * compiled package resources (including matching built-in contributions),
 * unclaimed built-in defaults, then the engine. Mode >= 2 bypasses shadowing.
 *
 * Returned streams implement the supported 31-slot idFile ABI and own their
 * file handles or heap buffers. Installation requires clean
 * constructor/helper signatures and a decoded read-only provider vtable. User
 * content is gated by the launch configuration; package discovery can be
 * refreshed during runtime rearm.
 */
#ifndef BACKEND_B1_OVERRIDES_H
#define BACKEND_B1_OVERRIDES_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* Construct a native idFile stream for an exact compiled resource already
 * opened by its owning subsystem. Takes ownership of file on every outcome;
 * successful streams close it through their native destructor. Rewinds before
 * publication and requires the verified idStr helpers to be installed. This
 * does not resolve a path, consult local packages, or fall back to the game. */
void *sh_overrides_stream_take_file(FILE *file, uint64_t length, const char *name);
/* Copy a compiled in-memory resource into a stream with the same native ABI.
 * Source bytes remain caller-owned. No partial stream is returned on failure. */
void *sh_overrides_stream_copy_bytes(const void *bytes, size_t length, const char *name);

/* Failed SOURCE observations retain the actual opening until every affected
 * consumer lifetime has positively recovered. Collect only completed streams;
 * admission also checks still-open streams before changing an actual input. */
void sh_overrides_collect_source_receipts(void);
int sh_overrides_source_receipts_admit(void *const *resources, size_t count, char *error, size_t capacity);

/* Native DeclRegisterFile canonicalizes a source argument to decltree/<source>.
 * The dynamic decl server therefore publishes one exact provider entry per
 * genuinely new identity. There is intentionally no aggregate source alias. */
#define SH_OVERRIDES_INTERNAL_DECL_PREFIX "decltree/"

typedef struct sh_overrides_internal_decl_entry {
    const char *type;
    const char *name;
    const unsigned char *body;
    size_t body_length;
} sh_overrides_internal_decl_entry;

/* Swap the resource-provider open slot. module_base identifies the host
 * image. The constructor and three idStr helpers must be clean SIG_OK matches
 * within it; hooked fallbacks are refused. Decode the constructor to locate a
 * read-only provider vtable, then configure all three native helper slots
 * before publishing the hook. Returns 1 on installation, otherwise 0 and logs
 * the reason.
 */
int sh_overrides_install(const uint8_t *module_base,
                         void *ctor_fn, int ctor_status_ok,
                         void *read_string_fn, int read_string_status_ok,
                         void *compare_fn, int compare_status_ok,
                         void *write_string_fn, int write_string_status_ok);

/* Set the data root used by <root>\overrides. NULL restores
 * %LOCALAPPDATA%\snapmap-plus. Returns 1 when a path is set.
 */
int sh_overrides_set_root(const char *path);

/* Copy the effective data root into `out` (the configured test root, otherwise
 * %LOCALAPPDATA%\snapmap-plus). Shared with the dynamic decl server so both
 * override mechanisms always inspect the same tree. */
int sh_overrides_get_root(char *out, size_t cap);

/* Initial table installation copies all canonical keys and bodies before
 * publication, so callers may release their snapshot. It requires the launch
 * user-layer gate and provider hook. Exact entries take precedence over
 * physical files and linked resources; runtime rearm uses reopen and merge
 * below.
 */
/* Return 1 when this exact decltree/<type>/<name>.decl key names a published
 * new identity present in the current compilation. Retired aliases and a
 * missing provider return 0. Case-insensitive, because the engine spells a decl type with
 * its registered casing while the table is keyed from the override path.
 * Read-only: it never opens or copies a body. */
int sh_overrides_internal_decl_published(const char *name);

/* Return the number of published new decl identities. Packages that only
 * shadow existing identities do not contribute to this count.
 */
size_t sh_overrides_internal_decl_published_count(void);

int sh_overrides_internal_decl_table_can_install(void);
int sh_overrides_internal_decl_table_install(
    const sh_overrides_internal_decl_entry *entries, size_t count);

/* Number of opens served by the shadow; used by diagnostics. */
unsigned long sh_overrides_shadow_count(void);

/* Read the effective base for navigation into a new process-heap buffer.
 * This applies Grid generation to compiled package/product/original inputs,
 * without re-entering navigation. Caller frees the result. An owned source
 * failure does not fall back to stock. Reading originals requires a previous
 * hook call to capture the provider object.
 */
unsigned char *sh_overrides_read_engine_resource(const char *name, size_t *out_len);

/* Refresh the package list used by resource opens and return its count.
 * Readers hold a shared lock through path selection. Incomplete enumeration
 * retains the previous inventory and returns SH_OVERRIDES_RESCAN_FAILED.
 */
unsigned long sh_overrides_rescan_packages(void);
/* Run consumer activation before committing the provider. The callback has the
 * sh_package_runtime_refresh_activated contract, including restoring=1 after
 * a failure. Both publication and restoration retire derived source caches. */
struct sh_package_changes;
unsigned long sh_overrides_rescan_packages_activated(
    int (*activate)(void *context, int restoring, const struct sh_package_changes *changes,
        char *error, size_t capacity), void *context);
struct sh_package_map_plan;
int sh_overrides_activate_map(struct sh_package_map_plan *plan,
    int (*activate)(void *context, int restoring, const struct sh_package_changes *changes,
        char *error, size_t capacity), void *context);
struct sh_package_occurrence_snapshot;
int sh_overrides_activate_occurrences(const struct sh_package_occurrence_snapshot *map,
    int (*activate)(void *context, int restoring, const struct sh_package_changes *changes,
        char *error, size_t capacity), void *context);
#define SH_OVERRIDES_RESCAN_FAILED ((unsigned long)-1)

/* Prepare runtime rearm without hiding existing entries. This retains READY
 * until the replacement snapshot is complete.
 */
void sh_overrides_internal_decl_table_reopen(void);

/* Merge entries over the published table without dropping earlier identities.
 * New entries win on key collisions. Open streams own their bytes, so the
 * previous snapshot can be released after publication.
 */
int sh_overrides_internal_decl_table_merge(
    const sh_overrides_internal_decl_entry *entries, size_t count);

/* Restore the saved resource-open slot. Idempotent; returns 1 when restored,
 * 0 when no hook is installed.
 */
int sh_overrides_uninstall(void);

/* The resource manager's file route (resource_file_visibility.h) serves the
 * names this shadow claims. Claim name as an ordinary open would, without
 * chaining to the engine: 1 with *stream set when the shadow owns the name
 * (NULL when that owned name is unavailable), 0 when the name belongs to the
 * engine or the shadow is not installed. */
int sh_overrides_claim_open(const char *name, void **stream);
/* Hand a claimed stream to the next open of exactly this name pointer on the
 * calling thread, so a native helper that opens by name receives it. */
void sh_overrides_pending_open(const char *name, void *stream);
/* Withdraw the calling thread's pending stream; NULL once it was consumed. */
void *sh_overrides_pending_take(void);
/* Close a stream that was not given to the engine. */
void sh_overrides_stream_close(void *stream);
/* A synchronous observer runs after receipt attachment and before the native
 * opening caller resumes. It borrows stream/provider, never replaces the
 * result, and is not called on exceptional native exit. */
typedef uint64_t (*sh_overrides_file_open_guard)(uintptr_t caller,void *provider);
typedef void (*sh_overrides_file_return_observer)(void *stream,uintptr_t caller,void *provider,uint64_t stamp);
/* The guard's opaque stamp belongs only to this stack opening. No resource
 * origin/capture is transferred or duplicated. A failed guard never skips the
 * native open; the return observer receives zero and records incompleteness. */
int sh_overrides_set_file_return_observer(sh_overrides_file_open_guard guard,sh_overrides_file_return_observer observer);
int sh_overrides_file_return_observer_ready(sh_overrides_file_open_guard guard,sh_overrides_file_return_observer observer,
    const void *provider_table,const void *native_open);
/* Reached ScriptLoadFile dispatch only: deleting, relative name, Read, Length,
 * Timestamp. A live owned receipt is authenticated by allocation/stream/table
 * identity before copying its original callbacks. No lease is acquired.
 * product=1 identifies our current exact native-compatible stream methods;
 * otherwise the caller must qualify every returned native callback. */
typedef struct sh_overrides_file_dispatch {
    uintptr_t table,slots[5],read_string,tell;
    int product,receipt,shader;
} sh_overrides_file_dispatch;
int sh_overrides_file_dispatch_snapshot(void *stream,sh_overrides_file_dispatch *out);

#ifdef SH_OVERRIDES_TESTING
void *sh_overrides_test_open(const char *name, unsigned int mode);
void sh_overrides_test_internal_decl_table_reset(void);
int sh_overrides_test_internal_decl_table_install(
    const sh_overrides_internal_decl_entry *entries, size_t count);
int sh_overrides_test_internal_decl_table_merge(
    const sh_overrides_internal_decl_entry *entries, size_t count);
void *sh_overrides_test_internal_decl_open(const char *name);
/* Whether the published table holds exactly this lower-case decltree key. */
int sh_overrides_test_internal_decl_registered(const char *key);
void *sh_overrides_test_memory_stream(const unsigned char *body, size_t length);
long long sh_overrides_test_stream_read(void *stream, void *buffer, uint64_t length);
long long sh_overrides_test_stream_read_at(void *stream, long long offset,
                                           void *buffer, uint64_t length);
long long sh_overrides_test_stream_write(void *stream, const void *buffer, uint64_t length);
long long sh_overrides_test_stream_write_at(void *stream, long long offset,
                                            const void *buffer, uint64_t length);
int sh_overrides_test_stream_seek(void *stream, long long offset, int origin);
long long sh_overrides_test_stream_length(void *stream);
int sh_overrides_test_stream_true_flag(void *stream);
int sh_overrides_test_stream_set_length(void *stream, long long length);
size_t sh_overrides_test_stream_vtable_slots(void);
void *sh_overrides_test_stream_vtable_slot(size_t index);
int sh_overrides_test_stream_helpers_configure(void *read_string, int read_clean,
                                                void *compare, int compare_clean,
                                                void *write_string, int write_clean);
int sh_overrides_test_stream_helpers_ready(void);
void sh_overrides_test_stream_helpers_reset(void);
int sh_overrides_test_supported_build_abi(const uint8_t *module_base,
                                          const void *ctor, int ctor_status_ok,
                                          const void *read_string, int read_string_status_ok,
                                          const void *compare, int compare_status_ok,
                                          const void *write_string, int write_string_status_ok);
int sh_overrides_test_address_in_readonly_section(const uint8_t *module_base, const void *address);
void *sh_overrides_test_stream_open_file(const char *path);
void sh_overrides_test_stream_close(void *stream);
void sh_overrides_test_receipt_attach(void *stream, const char *path);
size_t sh_overrides_test_failed_source_count(void);
size_t sh_overrides_test_pending_source_count(void);
struct sh_package_resource_capture;
void sh_overrides_test_source_attach(void *stream, const char *path,
    struct sh_package_resource_capture *capture, int baseline);
struct sh_resource_instance_origin;
void sh_overrides_test_vt_observer(int (*recording)(void), int (*observed)(struct sh_resource_instance_origin *));
#endif

#endif /* BACKEND_B1_OVERRIDES_H */
