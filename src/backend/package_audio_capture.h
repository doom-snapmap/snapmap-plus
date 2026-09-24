/* Retained typed audio inputs from an authored compiler scope. */
#ifndef SH_PACKAGE_AUDIO_CAPTURE_H
#define SH_PACKAGE_AUDIO_CAPTURE_H
#include "package_audio.h"
#include "audio_link_graph.h"

typedef struct sh_package_audio_capture sh_package_audio_capture;
/* Uses the existing locale-aware original/override selection and bank closure.
 * Only selected banks are decoded; media stays in retained random-access
 * sources. Memory sources are copied once and file sources retain their open
 * handle. compiled is borrowed until seal; originals outlives this capture.
 * No native calls, provider writes or author-file changes occur. */
sh_package_audio_capture *sh_package_audio_capture_open(const sh_package_compilation *compiled,
    sh_audio_originals *originals, const uint32_t *events, size_t count,
    char *error, size_t capacity);
/* Also account for effective authored bank/media inventory, including banks
 * with no directly named sound declaration. Reads typed metadata before source
 * nomination, so plug-in-only references cannot hide a media replacement. */
sh_package_audio_capture *sh_package_audio_capture_open_inventory(const sh_package_compilation *compiled,
    sh_audio_originals *originals, char *error, size_t capacity);
/* Capture named bank roots and their complete typed closure, independently of
 * sound events. Used with an empty compilation for native baseline witnesses.
 * Root IDs are copied and retained until seal; all source rules stay shared. */
sh_package_audio_capture *sh_package_audio_capture_open_roots(const sh_package_compilation *compiled,
    sh_audio_originals *originals, const uint32_t *banks, size_t count, char *error, size_t capacity);
/* Before graph construction only: add the complete discovered event set to an
 * inventory capture without rereading already captured bank/codec sources.
 * A failed extension invalidates the working capture; close it. */
int sh_package_audio_capture_extend(sh_package_audio_capture *capture,
    const uint32_t *events, size_t count, char *error, size_t capacity);
void sh_package_audio_capture_close(sh_package_audio_capture *capture);
/* Borrowed graph input. The capture remains alive through graph close and
 * prepared bank streaming. Use one capture for the complete event set of each
 * authored scope, not one independently activated capture per event. */
const sh_audio_graph_scope *sh_package_audio_capture_input(sh_package_audio_capture *capture);
/* After the complete graph has captured its media, discard discovery state.
 * The compiler scopes may then be freed. Bank/media source handles and decoded
 * metadata remain valid until close; no more media discovery is permitted. */
void sh_package_audio_capture_seal(sh_package_audio_capture *capture);
/* Move retained authored file handles to exact immutable cache snapshots.
 * Keeps graph contexts/bytes stable while releasing editor-owned source files.
 * All replacement streams are prepared before swapping any in this capture.
 * Originals and memory bodies already have independent lifetimes. Complete
 * cache objects can survive failure; capture content never changes. */
int sh_package_audio_capture_cache(sh_package_audio_capture *capture,
    const char *directory, char *error, size_t capacity);
/* Visit only streamed media actually captured by graph construction. Works
 * after seal; returned source handles stay valid through capture close. */
int sh_package_audio_capture_media(const sh_package_audio_capture *capture,
    int (*visit)(void *, uint32_t, const sh_audio_graph_media *), void *context);
#endif
