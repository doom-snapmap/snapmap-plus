/* Cooked UI movie units: paired files and native constructor requests. */
#ifndef SH_PACKAGE_MOVIE_H
#define SH_PACKAGE_MOVIE_H
#include <stddef.h>
#include "swf_binary.h"
#include "package_consumers.h"

/* Native identities of one constructor pair. The movie is an idFileResource
 * named by its cooked path; the atlas is an image named by its explicit cooked
 * path. consumer is a field spelling that reproduces both through the native
 * constructor rule (swf/ prefix test, extension replacement, atlas filename
 * generation, first .bimage replacement). */
typedef struct sh_package_movie_names {
    char movie[256], atlas[256], consumer[256];
} sh_package_movie_names;

/* 1 for either member of a regular cooked pair, 0 for an unrelated path and -1
 * for movie data whose pair cannot be inverted (partitioned long names or a
 * .bimage directory). Such inputs need their typed consumer's field spelling. */
int sh_package_movie_names_for_path(const char *path, sh_package_movie_names *out);
/* Forward from an idSWF/idSWFFile field value. 1 movie, 0 empty (native null),
 * -1 invalid or an ambiguous pair that one file unit cannot describe. */
int sh_package_movie_names_for_consumer(const char *consumer, size_t length,
    sh_package_movie_names *out);

/* Constructor requests absent from cooked bytes, reported in native order:
 * each dictionary font once (normalized manager name), the fixed fallback
 * fonts the language resolver can request for a movie containing fonts, then
 * the fixed materials every constructor requests. All are native
 * create-or-default lookups, so absence is never a packaging error. visit
 * returns 1 captured, 0 positively absent, -1 failed. Dictionary material
 * records are byte sites and are not repeated here. Local text, labels and
 * ActionScript payloads are not resource names and remain opaque. */
typedef int (*sh_package_movie_request)(void *context, const char *slot,
    const char *family, const char *name);
int sh_package_movie_capture(const unsigned char *body, size_t length,
    sh_package_movie_request visit, void *context, char *error, size_t capacity);

/* Direct dictionary material sites for the shared idStr relocation
 * transaction. Fonts stay unchanged: the native resolver makes language
 * decisions from their original names, so a font binding is a constructor
 * slot routed at load time rather than a rewritten string. */
int sh_package_movie_material_sites(void *reader_context,
    const unsigned char *body, size_t length, sh_resource_binding_site visitor,
    void *context, char *error, size_t capacity);

/* Runtime routing for one captured movie plan: family is the native manager
 * label (font/material) and request the exact constructor lookup. Returns 0
 * when unrelated or when the plan binds the request to its own original name,
 * 1 with a borrowed replacement, -1 for a contradictory plan. A request that
 * the plan omits was positively absent everywhere in the movie's scope and
 * keeps its native default behavior. No allocation or native call occurs. */
int sh_package_movie_route(const sh_package_consumer *owner, const char *family,
    const char *request, const char **replacement);
/* Late calls use the immutable scope namespace, including computed names.
 * Resource slots are the existing request/sound and request/material slots.
 * Localized keys use movie/string/<native key>, with package_strings' ASCII
 * case folding only: slashes/backslashes and all other bytes stay distinct.
 * This owned slot is for compiler capture; NULL is an allocation failure. */
char *sh_package_movie_string_slot(const char *key);
/* No allocation/native calls. 0 keeps the exact native fallback, 1 borrows a
 * private dictionary key, -1 denotes a contradictory string binding. */
int sh_package_movie_string_route(const sh_package_consumer *owner,
    const char *key, const char **replacement);
/* Native manager spelling of a looked-up name: ASCII uppercase folds, back-
 * slashes become slashes and one leading slash is dropped. 0 when empty,
 * NUL-bearing or longer than capacity; other bytes are retained exactly. */
int sh_package_movie_native_name(const char *text, size_t length, char *out, size_t capacity);
/* Owners are movie file identities: family "file" and a generated/swf path. */
int sh_package_movie_owner(const char *family, const char *name);
/* Pure no-allocation prepublication validation. Accept every constructor and
 * late namespace row, canonical owner/request identities and equal repeats;
 * reject unsupported, empty, cross-family or contradictory rows. This checks
 * metadata only; provider preparation proves the complete physical closure. */
int sh_package_movie_plan_valid(const sh_package_consumer *owner);
/* Whether a valid plan is the stock movie's own lookup: every row requests
 * its own identity, a font, material or late sound/material under the exact
 * requested canonical name, so routing changes nothing. Localized rows and
 * redirects never are. Whether those targets changed is the caller's. */
int sh_package_movie_plan_native(const sh_package_consumer *owner);
#endif
