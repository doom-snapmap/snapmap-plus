/* Maps saved or published with the retired first-generation package system
 * carry their packages as "smpkg." string variables; unreleased v2 builds
 * also wrote "smpbind." and "smpref." rows. This module owns their header
 * grammar and their removal. Conversion of complete packages into bundles is
 * modlegacy.c's (DESIGN.md section 11). */
#ifndef SNAPMAP_PLUS_MAP_LEGACY_PAYLOAD_H
#define SNAPMAP_PLUS_MAP_LEGACY_PAYLOAD_H

#include <stddef.h>

#define SH_LEGACY_MAGIC      "smpkg."
#define SH_LEGACY_MAGIC_LEN  6
/* Released writers capped ids at 128 characters; a delivery key is 73. */
#define SH_LEGACY_KEY_CAP    160

/* How a released writer spelled the key, which dates it. */
enum {
    SH_LEGACY_KEY_FOLDER = 1,   /* beta.6-13: the package folder name, no dots */
    SH_LEGACY_KEY_DESCRIPTOR,   /* beta.14-15: the descriptor id, dotted */
    SH_LEGACY_KEY_DELIVERY,     /* beta.16-19: "delivery.<64 hex tree fingerprint>" */
    SH_LEGACY_KEY_OVERRIDES     /* not a carrier: a retired overrides folder migrated by modmigrate.c */
};

typedef struct sh_legacy_header {
    char     key[SH_LEGACY_KEY_CAP];
    unsigned index;      /* 0-based, as every released writer emitted it */
    unsigned total;      /* at least 1 */
    char     digest[17]; /* first 16 lowercase hex of SHA-256 of the packed ZIP */
    int      key_form;
} sh_legacy_header;

/* 1 when `header` (quotes excluded) is a retired package shard header:
 * "smpkg.<key>.<index>.<total>.<16 lowercase hex>". The last three dots
 * delimit the transport fields, so the key may contain dots. */
int sh_map_legacy_payload_header(const char *header, size_t length);

/* The same grammar, filling `out`. Index and total are 1-7 digits and total
 * is at least 1; whether index is in range is the caller's completeness check. */
int sh_map_legacy_payload_parse(const char *header, size_t length, sh_legacy_header *out);

/* A short name for a key form, for log lines. */
const char *sh_map_legacy_key_form_name(int key_form);

/* 1 when `header` is an unreleased v2 row: "smpbind.<...>.<16 hex>" or
 * "smpref.<...>.<16 hex>". */
int sh_map_legacy_unreleased_header(const char *header, size_t length);

/* Remove every smpkg., smpbind. and smpref. row and rewrite allocCount[4],
 * without logging. Returns an owned NUL-terminated process-heap buffer
 * (HeapFree), or NULL when the map carries none or its structure cannot be
 * read (*doc_failed set; the caller keeps the original and the engine treats
 * the rows as inert strings). */
char *sh_map_legacy_payload_strip_all(const char *json, size_t length, size_t *out_length,
                                      unsigned *package_rows, unsigned *unreleased_rows,
                                      int *doc_failed);

#endif
