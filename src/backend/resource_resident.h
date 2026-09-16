/* Native cached-resource refresh inside the existing provider transaction. */
#ifndef SH_RESOURCE_RESIDENT_H
#define SH_RESOURCE_RESIDENT_H
#include "package_compiler.h"
#include "signatures.h"

typedef struct sh_resource_resident sh_resource_resident;
int sh_resource_resident_bind(const sig_result *results, size_t count, const uint8_t *base);
/* Main thread, admitted native map/browser boundary. Captures identities before
 * parsing changes the graph; holds native renderer and process-heap scopes.
 * Recovery includes identities retained from the failed activation. */
sh_resource_resident *sh_resource_resident_begin(const sh_package_changes *changes,
    int restoring, char *error, size_t capacity);
/* A declaration pass owns these objects' reconstruction and pending marks.
 * Register exclusions before reconstruct. No object is rebuilt twice. */
void sh_resource_resident_external(sh_resource_resident *pass, void *resource);
int sh_resource_resident_reconstruct(sh_resource_resident *pass, char *error, size_t capacity);
/* Call after every declaration-owned pending mark exists, before its first
 * lookup. Drain also calls this for providers without declarations. */
int sh_resource_resident_defaults(sh_resource_resident *pass, char *error, size_t capacity);
/* Reloads pending resources, then rebinds material virtual-texture parms inside
 * the heap-0 scope before the consumer update. Idempotent once it succeeds. */
int sh_resource_resident_drain(sh_resource_resident *pass, char *error, size_t capacity);
/* How many identities recovery left at the engine default because the restored
 * provider supplies no source for them. Reported, never hidden. */
size_t sh_resource_resident_recovered_defaults(const sh_resource_resident *pass);
/* Native load/lookup observers preserve existing and newly loaded dependency
 * lifetimes only while the admitted main-thread refresh scope owns them. */
void sh_resource_resident_touch(void *resource);
/* Always call, including native exceptions. Disarms leftover owned marks,
 * restores scopes, retains failed identities for provider recovery. */
int sh_resource_resident_end(sh_resource_resident *pass, int succeeded,
    char *error, size_t capacity);
#endif
