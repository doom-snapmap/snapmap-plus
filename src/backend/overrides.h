/* Resource-provider file shadowing through the open-by-name vtable slot (+0x148 on build 13954591). Opens try the
 * placement grid, navigation, Grid Room variants, built-in defaults and installed mod bundles (modserve.h), then
 * the engine; mode >= 2 bypasses shadowing, and returned streams implement the 34-slot idFile ABI. */
#ifndef BACKEND_B1_OVERRIDES_H
#define BACKEND_B1_OVERRIDES_H

#include <stdint.h>
#include <stddef.h>

/* The idFile stream ABI this implementation provides (slot count) and the
 * provider open slot it replaces; startup compares them with engine_build.h. */
#define SH_OVERRIDES_STREAM_SLOTS 34u
#define SH_OVERRIDES_OPEN_SLOT    0x148u

/* Slot indices of the 34-slot idFile layout, shared by native streams and this module's stream; the engine's
 * ReadFile (idFileSystemLocal vtable +0xF8) calls Read (slot 6) and GetLength (slot 13) on build 13954591. */
#define SH_IDFILE_SLOT_CLOSE       0u    /* +0x00 destructor: close and free */
#define SH_IDFILE_SLOT_TRUE_FLAG   2u    /* +0x10 native constant true */
#define SH_IDFILE_SLOT_READ        6u    /* +0x30 Read(self, buffer, length) */
#define SH_IDFILE_SLOT_LENGTH      13u   /* +0x68 GetLength(self) */
#define SH_IDFILE_SLOT_SET_LENGTH  14u   /* +0x70 SetLength(self, length) */

/* Swap the resource-provider open slot. The constructor and three idStr helpers must be clean SIG_OK matches in
 * module_base; returns 1 on installation, otherwise 0 and logs the reason. */
int sh_overrides_install(const uint8_t *module_base,
                         void *ctor_fn, int ctor_status_ok,
                         void *read_string_fn, int read_string_status_ok,
                         void *compare_fn, int compare_status_ok,
                         void *write_string_fn, int write_string_status_ok);

/* Read the effective navigation base (Grid generation applied, without re-entering navigation) into a new
 * process-heap buffer the caller frees. Needs a prior hook call to have captured the provider object. */
unsigned char *sh_overrides_read_engine_resource(const char *name, size_t *out_len);

/* Restore the saved resource-open slot. Idempotent; returns 1 when restored,
 * 0 when no hook is installed.
 */
int sh_overrides_uninstall(void);

/* Runs the install's byte checks (the base idFile helper's prologue at its recorded RVA, the provider vtable
 * decoded from the constructor). Returns NULL when they hold, else the log line naming the failure. */
const char *sh_overrides_check_sites(const uint8_t *module_base, const void *ctor_fn,
                                     const uint8_t **helper_out, void **vtable_out);

#ifdef SH_OVERRIDES_TESTING
void *sh_overrides_test_open(const char *name, unsigned int mode);
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
/* Run read_installed_resource against a caller-supplied engine open and
 * provider, restoring the installed ones afterwards. The result is HeapAlloc'd. */
unsigned char *sh_overrides_test_read_installed(void *open_fn, void *provider,
                                                const char *name, size_t *out_len);
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
#endif

#endif /* BACKEND_B1_OVERRIDES_H */
