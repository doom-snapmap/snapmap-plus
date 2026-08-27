/* overrides.h -- the OVERRIDES FILE-SHADOW resource loader, native C
 * (port of OG's resource-open vtable swap FUN_18000b370 / FUN_18000b110 / FUN_18000ce50).
 *
 * "Overrides" are a transparent file-shadow soft-mod: any resource the engine opens by name
 * is FIRST looked up as a same-named file under %LOCALAPPDATA%\snapmap-plus\overrides\ (or
 * overrides\shader_includes\ for shader includes); if present, the engine is served that file's bytes
 * from disk instead of the packaged resource. This is how the original SnapHak shipped its 29
 * snapeditorentitydef / editor-settings / property-inspector overrides that expand the editor palette.
 *
 * MECHANISM (DIRECT, RE of OG XINPUT1_3.dll 2021-03-27 + live DOOM):
 *   The engine's resource-provider class has a C++ vtable at `engineBase + 0x27984a0` (DOOM
 *   PTR_FUN_1427984a0, set as member[0] by the ctor FUN_141a51070). Its open-by-name VIRTUAL METHOD is
 *   vtable slot +0xf8 (= engineBase + 0x2798598), originally engine fn 0x141a57a60. SnapHak overwrites
 *   THAT ONE 8-byte slot with its own open hook FUN_18000b370, saving the original (via a verbatim
 *   vtable copy whose +0xf8 it keeps as DAT_18003e708). On every engine open, the hook signature is
 *       int open(void* this, const char* name, uint8 b1, uint8 b2, uint mode)   // __fastcall, 5 args
 *   It calls FUN_18000b110 to test for overrides/<name> under the profile dir; if the file exists it
 *   returns a SnapHak idFile-subclass stream (vtable PTR_FUN_18003d050: Read=fread, Length, Name,
 *   Close=fclose+free, ...) that the engine reads through; else it CHAINS to the saved original
 *   (*DAT_18003e708)(this,name,b1,b2,mode). A mode>=2 recursion guard goes straight to the original.
 *   The path builder FUN_18000ce50 = SHGetFolderPathA(CSIDL_PROFILE) + "/snaphak/" + relative.
 *
 * NATIVE PORT (the difference from OG):
 *   - This is a VTABLE-SLOT swap, NOT an inline code detour: the target is an 8-byte .data function
 *     pointer (vtable+0xf8), so we do NOT use install_inline_hook (which writes a 14-byte code jmp). We
 *     read+save the original slot pointer, then write our open hook's address into the slot (VirtualProtect
 *     RW, store, restore, FlushInstructionCache) and record it so we can restore the slot on unload.
 *   - The vtable is .data (not masked-byte sig-scannable). We sig-resolve the ctor (DB name
 *     "ResProviderCtor") and decode its `LEA RAX,[rip+vtable]`, but publish only when the ctor, three
 *     native idFile helpers, and decoded vtable all occupy the audited Steam-build RVAs. The newer
 *     build has a different idFile layout; it is refused instead of receiving this 31-slot table.
 *   - Our returned stream is our OWN clean-room idFile subclass (its dtor frees with our allocator, so we
 *     need no engine allocator/free): the engine only ever touches it through the vtable methods (all
 *     ours) + the public Length/Name fields. Semantically equivalent to OG's stream.
 *   - FOUR-LAYER resolution (OG has two): user disk file -> a manifest-selected installed resource
 *     served from the user's read-only base-game archives -> our BUILT-IN default decls served FROM
 *     MEMORY (overrides_baked.h; the "*Custom" tab set) -> the engine's packaged resource. Built-ins
 *     are never written to the user's folder (they update with each release; deleting a user file =
 *     reset to default). Only the user layer is gated by an immutable, restart-only config snapshot;
 *     built-ins and packaged resources remain active. Install runs a reclaim (deletes OUR untouched
 *     previously-written default copies) + an audit log pass that reports whether user files are active.
 *
 * Clean-room: ported from our own RE (above). Zero OG SnapHak bytes.
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

/* Install the overrides file-shadow by swapping the engine resource-provider's open vtable slot.
 *   module_base    = live DOOM image base. The provider is deliberately enabled only when the
 *                    resolved functions and decoded provider vtable occupy the pinned Steam-build
 *                    RVAs for the audited 31-slot idFile ABI; incompatible builds refuse cleanly.
 *   ctor_fn        = resolved engine ResProviderCtor address (from the signature resolver, DB name
 *                    "ResProviderCtor"). 0 => not resolved; logs SKIPPED and returns 0.
 *   ctor_status_ok = 1 iff a CLEAN scan hit (SIG_OK), not the hook-tolerant known_rva fallback
 *                    (SIG_OK_HOOKED). The ctor is only used to DECODE the vtable LEA (we don't patch the
 *                    ctor's code), so a hooked prologue would corrupt the LEA decode -- refuse on a
 *                    hook-tolerant resolve, same conservative policy as the other installs.
 *   read_string_fn/compare_fn/write_string_fn = the pinned-build native idStr helper addresses for
 *   idFile slots +0xe0/+0xe8/+0xf0. Each corresponding *_status_ok must be exactly 1 (SIG_OK); a
 *   missing or hook-tolerant helper refuses the provider install. The three pointers are published
 *   atomically as one fully configured 31-slot table before the engine vtable slot is changed.
 * Returns 1 if the slot was swapped, 0 otherwise (logs the reason). Emits a "B1: overrides file-shadow
 * installed ..." marker on success. */
int sh_overrides_install(const uint8_t *module_base,
                         void *ctor_fn, int ctor_status_ok,
                         void *read_string_fn, int read_string_status_ok,
                         void *compare_fn, int compare_status_ok,
                         void *write_string_fn, int write_string_status_ok);

/* Set the overrides ROOT directory (the dir that holds overrides\ and overrides\shader_includes\). The
 * effective lookup is <root>\overrides\<name>. Default = %LOCALAPPDATA%\snapmap-plus (the OG used
 * %USERPROFILE%\snaphak). Pass NULL to reset to the default. Returns 1 if a path is set. */
int sh_overrides_set_root(const char *path);

/* Copy the effective data root into `out` (the configured test root, otherwise
 * %LOCALAPPDATA%\snapmap-plus). Shared with the dynamic decl server so both
 * override mechanisms always inspect the same tree. */
int sh_overrides_get_root(char *out, size_t cap);

/* One-shot publication of an immutable per-decl table for the dynamic decl
 * catalog. The function copies every canonical key and body before publishing;
 * callers may release their snapshot after success. Publication is accepted
 * only while the launch-captured user layer is enabled and the +0xf8 provider
 * hook is installed. Exact table entries cannot be shadowed by loose files,
 * linked resources, built-ins, or a second publication. */
/* Return 1 when this exact decltree/<type>/<name>.decl key names a published
 * new identity. Case-insensitive, because the engine spells a decl type with
 * its registered casing while the table is keyed from the override path.
 * Read-only: it never opens or copies a body. */
int sh_overrides_internal_decl_published(const char *name);

int sh_overrides_internal_decl_table_can_install(void);
int sh_overrides_internal_decl_table_install(
    const sh_overrides_internal_decl_entry *entries, size_t count);

/* How many times the shadow has FIRED (served an override file instead of the packaged resource).
 * Observability for the test harness. */
unsigned long sh_overrides_shadow_count(void);

/* Re-scan %LOCALAPPDATA%\\snapmap-plus\\overrides for packages and publish the new list to
 * the file-shadow open path, so a package installed mid-session becomes servable without a
 * restart. Lock-free for readers. Returns the package count now visible.
 * This makes the package's BYTES reachable; publishing new DECL IDENTITIES is decl_server's
 * job (sh_decl_server_rearm). */
unsigned long sh_overrides_rescan_packages(void);

/* Retire the published internal decl table so a runtime re-arm can publish a new one. The old
 * table is leaked on purpose: an engine thread may still be reading a body out of it. */
void sh_overrides_internal_decl_table_reopen(void);

/* Publish `entries` merged OVER the currently published table, so a runtime re-arm cannot drop
 * identities an earlier pass published -- including ones belonging to packages it never
 * touched. A new entry with the same key wins. */
int sh_overrides_internal_decl_table_merge(
    const sh_overrides_internal_decl_entry *entries, size_t count);

/* Restore the engine open vtable slot to the saved original (LIFO-safe; idempotent). Returns 1 if a
 * slot was restored, 0 if none was installed. Call on unload to leave the engine vtable clean. */
int sh_overrides_uninstall(void);

#ifdef SH_OVERRIDES_TESTING
void sh_overrides_test_internal_decl_table_reset(void);
int sh_overrides_test_internal_decl_table_install(
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
                                          const void *ctor,
                                          const void *read_string,
                                          const void *compare,
                                          const void *write_string);
void *sh_overrides_test_stream_open_file(const char *path);
void sh_overrides_test_stream_close(void *stream);
/* Resolve an engine resource name to the existing override file that serves it
 * -- the legacy shared tree first, then each installed package's decls. Returns
 * 0 (and empties `out`) when no layer provides the name. */
int sh_overrides_test_resolve_existing(const char *name, char *out, size_t cap);
#endif

#endif /* BACKEND_B1_OVERRIDES_H */
