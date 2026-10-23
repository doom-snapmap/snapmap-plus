/* resource_file_visibility.h -- serve the file shadow's claims to the resource
 * manager's file IO once a map is running.
 *
 * Below map-load state 2, file and JSON resources open their names through the
 * provider's open slot, which the file shadow replaces. From state 2 they ask
 * the resource manager's file open (vtable +0x68), which reads loose files only
 * under resource_loadLooseAssets and otherwise only the resource containers, so
 * a shadow-owned name loaded then would become an empty file. The swapped slot
 * serves every name the shadow claims as the engine's own named idFile_Memory,
 * filled through the provider's open; every other name keeps the native path.
 */
#ifndef BACKEND_RESOURCE_FILE_VISIBILITY_H
#define BACKEND_RESOURCE_FILE_VISIBILITY_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

typedef struct sh_resource_file_visibility_native {
    void **slot;            /* the resource manager vtable's +0x68 file-open entry */
    void *open;             /* the method in that slot */
    void *operator_new;     /* the operator new of idFileResource Load's new-expression */
    void *operator_delete;  /* its sized delete, which that expression's own unwind
                             * calls when the constructor raises */
    void *memory_file;      /* idFile_Memory(name) constructor */
    void *fill;             /* opens a name through the provider slot into a memory file */
} sh_resource_file_visibility_native;

/* Decode the slot and helpers from clean SIG_OK matches of
 * ResourceManagerConstruct (its vtable must be read-only image data),
 * ResourceManagerOpenFile (must be the slot's current method) and
 * FileResourceLoad, whose exception metadata names the sized delete. Reads
 * only. Returns 1 and fills out on success. */
int sh_resource_file_visibility_decode(const sig_result *results, size_t count,
                                       const uint8_t *module_base,
                                       sh_resource_file_visibility_native *out);

/* Swap the slot. Call after sh_overrides_install succeeded. Returns 1 when
 * installed or already installed, 0 when refused (logged); refusal leaves
 * file and JSON resources on the native route. The swap is a single pointer
 * store, so a loader thread calls either the native or the new method. */
int sh_resource_file_visibility_install(const sig_result *results, size_t count,
                                        const uint8_t *module_base);

/* Restore the native method. Idempotent; returns 1 when a swap was removed. */
int sh_resource_file_visibility_uninstall(void);

/* Files served through the provider since installation. */
unsigned long sh_resource_file_visibility_served(void);
/* Current exact owned slot and saved reached helpers, for a read-only caller
 * contract. A stale install flag or a different method never qualifies. */
int sh_resource_file_visibility_dispatch_ready(const sh_resource_file_visibility_native *expected);

#ifdef SH_RESOURCE_FILE_VISIBILITY_TESTING
/* Bind natives without a vtable swap and return the slot method. */
void *sh_resource_file_visibility_test_bind(void *open, void *operator_new, void *operator_delete,
                                            void *memory_file, void *fill);
#endif

#endif /* BACKEND_RESOURCE_FILE_VISIBILITY_H */
