/* Immutable native spawn registration semantics, before key relocation. */
#ifndef SH_PACKAGE_SPAWN_REGISTRY_H
#define SH_PACKAGE_SPAWN_REGISTRY_H
#include "package_keys.h"
#include "decl_tree.h"

typedef struct sh_package_spawn_row {
    uint32_t mask;
    char *entity; /* Bound declaration identity; empty for native NULL. */
    char *body;   /* Complete canonical authored row, including other fields. */
} sh_package_spawn_row;
typedef struct sh_package_spawn_modifier {
    uint32_t mask;
    char *body;
} sh_package_spawn_modifier;
typedef struct sh_package_spawn_registry {
    sh_package_spawn_row *rows;
    size_t count;
    sh_package_spawn_modifier *modifiers;
    size_t modifier_count;
} sh_package_spawn_registry;

/* Materialize native defaults of numbered spawn lists after inheritance has
 * been expanded. Sparse in-range rows retain their native positions. Returns
 * 1 with an owned envelope, -2 for invalid source, -1 for operational failure.
 * Input and delivery bytes remain unchanged; failure leaves output empty. */
int sh_package_spawn_registry_prepare(sh_decl_source source, char **output,
    size_t *length, char *error, size_t capacity);

/* Read effective rows after native inheritance preparation, not an
 * unexpanded authored patch. 1 captured, 0 no list, -1 malformed/unreadable. Out starts
 * zeroed and remains empty on failure. A present empty list is authoritative.
 * Duplicate rows and row order are retained: native selection and first-match
 * classification give them meaning before any composition decision. */
int sh_package_spawn_registry_read(sh_decl_source source, const sh_package_key_space *space,
    sh_package_spawn_registry *out, char *error, size_t capacity);
void sh_package_spawn_registry_free(sh_package_spawn_registry *registry);
/* Ignore presentation order only when the selection multiset and every AI's
 * first classification/modifier remain equal. Names must already be bound. */
int sh_package_spawn_registry_equivalent(const sh_package_spawn_registry *a, const sh_package_spawn_registry *b);
/* Project the first registration and its effective modifier for one AI. Other
 * registrations cannot affect that AI's classification identity. */
char *sh_package_spawn_registry_classification(sh_decl_source source,
    const sh_package_key_space *space, const char *entity, size_t *length, char *error, size_t capacity);

/* Native selection uses each row's mask. Classification and global modifiers
 * use the first registration of the selected AI, even when a later row caused
 * its spawn. SIZE_MAX denotes no native classification/modifier. */
size_t sh_package_spawn_registry_type(const sh_package_spawn_registry *registry, size_t row);
size_t sh_package_spawn_registry_modifier(const sh_package_spawn_registry *registry, size_t row);
enum { SH_PACKAGE_SPAWN_REQUEST, SH_PACKAGE_SPAWN_CLASSIFIER };

/* Lower a native mask to the exact private row keys it can select/classify.
 * The caller assigns one private key per row in native order. Keys remain
 * separate from classification bits and group-code encoding. A classifier
 * contributes only each AI's first row key, as the native type reader does.
 * Empty output means the native mask matches no registration, not a failure.
 * Failure leaves out empty; no arbitrary row wins or shares a request. */
int sh_package_spawn_registry_keys(const sh_package_spawn_registry *registry,
    const uint32_t *row_keys, uint32_t mask, int role, sh_package_key_set *out);

/* Encode a relocated request/classifier mask. Native named bits are not
 * retained in a request: all membership comes from its captured registry.
 * This preserves removed rows, custom bits and ALL without selecting another
 * source's registrations. 1 success (code may be zero), 0 exhausted, -1 error.
 * The encoder owns group-code reservations and must keep them with the result. */
int sh_package_spawn_registry_code(const sh_package_spawn_registry *registry,
    const uint32_t *row_keys, uint32_t mask, int role,
    const sh_package_key_encoder *encoder, uint32_t *code);

/* Publish retained private tables into the engine's shared table. Private
 * identities/keys are already resolved. Preserve each table's row order and
 * multiplicity, sharing exact repeated rows across tables. Other fields come
 * from base. Result is owned; inputs are unchanged. */
char *sh_package_spawn_registry_compose(sh_decl_source base, const sh_decl_source *tables,
    size_t count, const sh_package_key_groups *groups, size_t *length, char *error, size_t capacity);
#endif
