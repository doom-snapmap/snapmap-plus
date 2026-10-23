/* Reader-bound parent variants (R-255).
 *
 * Reading a placed entity merges its own state against the text of the
 * declaration its inherit names and drops every own member whose key and
 * value spell that text exactly (VK 1417ae380 / GL 1417a0e60). When a package
 * parent authors a member its own class does not reflect and a placed copy's
 * class reads it as a declaration, the copy reads the package's private
 * binding of that spelling, while a sibling that states the same spelling
 * means the global declaration. Serving the copy a variant of the parent with
 * the member already bound makes the engine's own comparison keep the
 * sibling's value, lets a copy without a value inherit the bound meaning, and
 * makes a reset inherit it as well.
 *
 * A variant is a child layer of the served parent that states only the bound
 * members, so ancestry, every other member and the editor variables stay the
 * parent's. Its name is derived from its exact text, so a served variant
 * identifies its parent and members without any table, and equal bindings
 * share one declaration. Each editor row naming the parent gets a child row
 * naming the variant, because rawmap validation looks the inherit name up in
 * the palette. Documents are saved, captured and converted in the parent form
 * (sh_package_variant_fold); only the document the engine reads names
 * variants. */
#ifndef SH_PACKAGE_VARIANTS_H
#define SH_PACKAGE_VARIANTS_H

#include <stddef.h>
#include "package_compiler.h"

typedef struct sh_package_variant_member { char *key, *value; } sh_package_variant_member;
typedef struct sh_package_variant {
    char *parent;
    sh_package_variant_member *members; /* sorted by key, unique */
    size_t count;
} sh_package_variant;
void sh_package_variant_free(sh_package_variant *variant);

/* Variant text of parent stating members (sorted by key here; keys must be
 * unique plain identifiers and values plain declaration names). malloc-owned,
 * NULL when a spelling cannot be stated or on allocation failure. */
unsigned char *sh_package_variant_text(const char *parent, const sh_package_variant_member *members,
    size_t count, size_t *length);
/* A child editor row of row naming entity_def. */
unsigned char *sh_package_variant_row_text(const char *row, const char *entity_def, size_t *length);
/* "smp/<sha256>" of the text in the entity (row 0) or editor-row (row 1)
 * domain. 1, or 0 on failure. */
int sh_package_variant_name(int row, const unsigned char *text, size_t length, char name[69]);

/* 1 when compiled serves name as a reader-bound entity variant (out owns its
 * parent and members), 0 when it does not, -1 on failure. */
int sh_package_variant_find(const sh_package_compilation *compiled, const char *name, sh_package_variant *out);
/* 1 when compiled serves the declaration type/name, 0 when not. */
int sh_package_variant_served(const sh_package_compilation *compiled, const char *type, const char *name);

/* Rewrite each placed entity carrier whose inherit names a served variant to
 * name its parent. materialize also states, first in the copy's own state,
 * each bound member that state does not set: the form the capture, save and
 * native conversion read (projection provenance tells these values from
 * authored ones). malloc-owned; *changed reports whether anything changed.
 * Other bytes stay exact. NULL on failure. */
char *sh_package_variant_fold(const sh_package_compilation *compiled, const char *json, size_t length,
    int materialize, size_t *out_length, int *changed);
/* The same, also listing each materialized member as "<carrier>\n<key>"
 * (carrier in sh_package_map_state_sites order; *sites malloc-owned, free
 * with sh_package_variant_sites_free). */
char *sh_package_variant_fold_sites(const sh_package_compilation *compiled, const char *json, size_t length,
    int materialize, size_t *out_length, int *changed, char ***sites, size_t *site_count);
void sh_package_variant_sites_free(char **sites, size_t count);
/* Retarget the inherit of state carriers, in sh_package_map_state_sites
 * order: targets[i] (NULL keeps it) replaces carrier i's inherit, which must
 * currently be expected[i]. malloc-owned; NULL on failure or mismatch. */
char *sh_package_variant_retarget(const char *json, size_t length, const char *const *expected,
    const char *const *targets, size_t count, size_t *out_length);

#endif
