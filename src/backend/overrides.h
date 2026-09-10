/* Resource-provider file shadowing through the open-by-name vtable slot
 * (+0xf8). Ordinary opens try map navigation, exact published decltree
 * entries, user files, linked installed resources, built-in defaults, then
 * the engine. Mode >= 2 bypasses shadowing.
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
 * new identity. Case-insensitive, because the engine spells a decl type with
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

/* Read the original engine resource into a new process-heap buffer; caller
 * frees it. Returns NULL on a miss, unavailable provider, or fault. Mode 2
 * bypasses shadowing so navigation baking can extend the shipped payload
 * without recursion. Requires a previous hook call to capture the provider
 * object.
 */
unsigned char *sh_overrides_read_engine_resource(const char *name, size_t *out_len);

/* Refresh the package list used by resource opens and return its count.
 * Readers hold a shared lock through path selection. Incomplete enumeration
 * publishes an empty inventory and returns SH_OVERRIDES_RESCAN_FAILED.
 */
unsigned long sh_overrides_rescan_packages(void);
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

#ifdef SH_OVERRIDES_TESTING
int sh_overrides_test_resolve_cached(const char *name, char *out, size_t cap);
void sh_overrides_test_internal_decl_table_reset(void);
int sh_overrides_test_internal_decl_table_install(
    const sh_overrides_internal_decl_entry *entries, size_t count);
int sh_overrides_test_internal_decl_table_merge(
    const sh_overrides_internal_decl_entry *entries, size_t count);
void *sh_overrides_test_internal_decl_open(const char *name);
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
/* Resolve a resource to an existing file: the shared override tree first,
 * then the package namespaces supported by the resolver. Returns 0 and
 * empties out on a miss.
 */
int sh_overrides_test_resolve_existing(const char *name, char *out, size_t cap);
#endif

#endif /* BACKEND_B1_OVERRIDES_H */
