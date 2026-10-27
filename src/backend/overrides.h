/* Resource-provider file shadowing through the open-by-name vtable slot
 * (+0x148 on build 13954591). Private Grid Room resources are generated from
 * installed originals. Ordinary opens try map navigation, the generated Grid
 * Room variants, built-in defaults, then the engine. Mode >= 2 bypasses
 * shadowing. No user or package files are read.
 *
 * Returned streams implement the supported 34-slot idFile ABI and own their
 * heap buffers. Installation requires clean constructor/helper signatures and
 * a decoded read-only provider vtable.
 */
#ifndef BACKEND_B1_OVERRIDES_H
#define BACKEND_B1_OVERRIDES_H

#include <stdint.h>
#include <stddef.h>

/* The idFile stream ABI this implementation provides (slot count) and the
 * provider open slot it replaces; startup compares them with engine_build.h. */
#define SH_OVERRIDES_STREAM_SLOTS 34u
#define SH_OVERRIDES_OPEN_SLOT    0x148u

/* Slot indices of the 34-slot idFile layout that both the native streams the
 * provider returns and this module's own stream use. 2018 used Read 5 (+0x28)
 * and GetLength 11 (+0x58); 13954591 inserted slots at +0x18 and +0x50, moving
 * them to 6 (+0x30) and 13 (+0x68). The engine's own ReadFile (idFileSystemLocal
 * vtable +0xF8) calls exactly these. */
#define SH_IDFILE_SLOT_CLOSE       0u    /* +0x00 destructor: close and free */
#define SH_IDFILE_SLOT_TRUE_FLAG   2u    /* +0x10 native constant true */
#define SH_IDFILE_SLOT_READ        6u    /* +0x30 Read(self, buffer, length) */
#define SH_IDFILE_SLOT_LENGTH      13u   /* +0x68 GetLength(self) */
#define SH_IDFILE_SLOT_SET_LENGTH  14u   /* +0x70 SetLength(self, length) */

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

/* Number of opens served by the shadow; used by diagnostics. */
unsigned long sh_overrides_shadow_count(void);

/* Read the effective base for navigation into a new process-heap buffer.
 * This applies Grid generation to built-in or installed inputs, without
 * re-entering navigation. Caller frees the result. An owned source failure
 * does not fall back to stock. Reading originals requires a previous hook call
 * to capture the provider object.
 */
unsigned char *sh_overrides_read_engine_resource(const char *name, size_t *out_len);

/* Restore the saved resource-open slot. Idempotent; returns 1 when restored,
 * 0 when no hook is installed.
 */
int sh_overrides_uninstall(void);

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
