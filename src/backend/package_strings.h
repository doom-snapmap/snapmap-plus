/* Source-scoped localization for typed idStrId binding and publication. */
#ifndef SH_PACKAGE_STRINGS_H
#define SH_PACKAGE_STRINGS_H

#include "package_sources.h"

typedef struct sh_package_strings sh_package_strings;

typedef struct sh_package_string_request {
    size_t component;
    /* Complete native key, including #str_. Descriptor keys omit that prefix.
     * Empty, unknown and other-prefix keys pass through byte-for-byte. The
     * caller supplies only verified idStrId sites, never arbitrary prose. */
    const char *key;
} sh_package_string_request;

/* Reserve the COMPLETE native key against the real dictionary/ID function,
 * user/baked keys and all pending reservations. Return 1 when reserved, 0 for
 * a collision, -1 for a failed check. identity is a content SHA-256, NOT a
 * native ID or proof of native uniqueness. Different strings may have the
 * same actual native ID. mapping_json contains the complete canonical locale
 * mapping and original fallback key for exact comparison when reusing an
 * owned reservation; a digest match alone is insufficient. Both inputs are
 * borrowed only during the callback. The caller owns this transaction and
 * must discard its reservations if compilation or later publication fails. */
typedef int (*sh_package_string_reserve)(void *context, const char *key,
    const unsigned char identity[32], const char *mapping_json);
typedef struct sh_package_string_reservations {
    sh_package_string_reserve reserve;
    void *context;
    size_t max_attempts; /* Caller policy, including the initial candidate. */
    /* Optional admission of an existing/global native key into this pending
     * transaction. It excludes its actual ID from later private allocation;
     * existing global/native collisions retain their historical semantics.
     * Required when modern aliases compose with historical raw string policy. */
    int (*reserve_global)(void *context, const char *key);
} sh_package_string_reservations;

/* Read validated descriptors in one component and its same-owner ancestors.
 * Resolve each locale independently, nearest definition first. Siblings and
 * separate delivery owners never supply values. Whole locale maps and native
 * fallback keys establish equality; JSON escape spelling does not. Equal
 * mappings share private keys without depending on discovery/request order.
 * Native fallback identity prevents unsafe sharing between different original
 * keys that happen to have the same authored translations.
 *
 * Every mapped key is private, including an otherwise unique contribution;
 * local/global strings must not replace source-bound values. Unmapped keys
 * stay native. A reservation callback is required when any private key is
 * needed. All outputs are owned; sources/descriptors are never modified or
 * retained. Failure returns NULL, never a usable prefix. No engine calls,
 * current-language lookup, source-tree writes or arbitrary ID hashes occur. */
sh_package_strings *sh_package_strings_compile(const sh_package_sources *sources,
    const sh_package_string_request *requests, size_t count,
    const sh_package_string_reservations *reservations,
    char *error, size_t capacity);
void sh_package_strings_free(sh_package_strings *strings);

/* Request order is the caller's typed-site association. Returned data is
 * borrowed until free. Invalid indices return NULL. An unmapped request has
 * no content identity; its key is the exact original native byte string. */
const char *sh_package_strings_key(const sh_package_strings *strings, size_t request);
const unsigned char *sh_package_strings_identity(const sh_package_strings *strings,
    size_t request);
size_t sh_package_strings_mapping_count(const sh_package_strings *strings);

typedef enum sh_package_string_selection_kind {
    SH_PACKAGE_STRING_NATIVE,
    SH_PACKAGE_STRING_AUTHORED
} sh_package_string_selection_kind;
typedef struct sh_package_string_selection {
    sh_package_string_selection_kind kind;
    const char *key;        /* Complete compiled/native key used by the site. */
    const char *native_key; /* Original key if native fallback is required. */
    const char *locale;     /* Selected authored locale, or NULL. */
    const char *text;       /* Decoded UTF-8, including a valid empty string. */
    size_t length;
} sh_package_string_selection;

/* locale_order is the COMPLETE caller-verified language/fallback sequence.
 * There is no implicit English, regional fallback, sys_lang or native text
 * guess. ASCII locale case is ignored. First present value wins, even empty.
 * If none is present, NATIVE explicitly requires the original key's native
 * lookup. For a private compiled key the publisher must materialize that
 * original fallback at the alias or retain a verified native fallback route;
 * merely omitting the alias does not preserve fallback behavior.
 * Native fallback bytes belong to the caller/native engine and are not
 * decoded or transcoded here. Selection failure clears out. */
int sh_package_strings_select(const sh_package_strings *strings, size_t request,
    const char *const *locale_order, size_t locale_count,
    sh_package_string_selection *out, char *error, size_t capacity);

/* Assemble only resolved authored mappings as locale -> private ID -> text,
 * in the existing policy.strings shape (IDs omit #str_). It includes every
 * locale, independent of the current language, and no invented native text.
 * This is not a complete active-language publication plan: use select above
 * to account for native fallback before publishing aliases. out must be an
 * initialized JSON object; success replaces it, failure leaves it unchanged. */
int sh_package_strings_policy(const sh_package_strings *strings,
    sh_json_object *out, char *error, size_t capacity);

#endif
