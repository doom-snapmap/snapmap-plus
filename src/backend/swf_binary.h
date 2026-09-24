/* Checked traversal of the cooked UI movie envelope. */
#ifndef SH_SWF_BINARY_H
#define SH_SWF_BINARY_H
#include <stddef.h>
#include <stdint.h>
#include "resource_rebind.h"

/* Sprite commands are borrowed opaque payloads. Tag values and ActionScript
 * semantics belong to their native consumers. SIZE_MAX identifies the main
 * sprite; other indices identify dictionary entries. Initialization blocks have
 * initialization=1 and tag=0. Return zero to stop the walk. */
typedef int (*sh_swf_command_visitor)(void *context, size_t dictionary_index,
    int initialization, uint32_t tag, const unsigned char *bytes,
    size_t offset, size_t length);
typedef struct sh_swf_binary_observer {
    void *context;
    sh_resource_binding_site reference;
    sh_swf_command_visitor command;
} sh_swf_binary_observer;

typedef struct sh_swf_binary_paths {
    char movie[256];
    char atlas[256];
} sh_swf_binary_paths;

/* Exact UI constructor naming: test the authored swf/ prefix before native
 * normalization, replace the source extension, generate the atlas filename,
 * then derive the cooked movie from that atlas filename. Outputs are native
 * lookup spellings; package path validation remains a separate boundary.
 * Empty names mean no movie and return zero. Failure clears both outputs. */
int sh_swf_binary_paths_for(const char *name, sh_swf_binary_paths *out);

/* Walk all counted records and direct material/font idStr sites, preserving
 * local labels, display strings, geometry and command bytes. A material name
 * starting with '.' denotes the movie's own atlas, not an external resource.
 * The caller owns atlas pairing, font fallback and command/runtime dependencies;
 * success here certifies the binary envelope, not complete movie linking.
 * Visitors can run before a later malformed record is found. Publish queued
 * changes only after a successful complete walk. */
int sh_swf_binary_read(const unsigned char *body, size_t length,
    const sh_swf_binary_observer *observer, char *error, size_t capacity);

/* Adapter for the shared idStr relocation transaction. reader_context is unused.
 * This reports direct dictionary references only, with original record spans. */
int sh_swf_binary_reference_sites(void *reader_context,
    const unsigned char *body, size_t length, sh_resource_binding_site visitor,
    void *context, char *error, size_t capacity);
#endif
