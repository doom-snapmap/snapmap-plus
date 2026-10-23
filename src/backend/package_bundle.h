/* Retained complete authored delivery, independent of editable source paths. */
#ifndef SH_PACKAGE_BUNDLE_H
#define SH_PACKAGE_BUNDLE_H

#include "package_archive.h"

typedef struct sh_package_bundle sh_package_bundle;
#define SH_PACKAGE_BUNDLE_KEY_CAP (16u + 64u + 1u + (SH_PACKAGE_ID_CAP - 1u) * 2u + 1u)

/* A private importer records both identities. source_name identifies its
 * exact extracted owner in the compiler inventory, never an author-ID match. */
typedef struct sh_package_bundle_binding {
    char source_name[SH_PACKAGE_NAME_CAP];
    unsigned char compiler_tree[32];
    sh_package_bundle *original;
} sh_package_bundle_binding;

/* Capture every inventoried member of one complete delivery owner, including
 * raw descriptors, unused assets, extras and empty directories. Verify its
 * identity against that inventory before returning. No source is rewritten or
 * kept locked. cache_root is the existing product data directory; private
 * temporary archives live beneath package-cache/bundles and disappear when
 * their last handle is released. Failure publishes no partial snapshot. */
sh_package_bundle *sh_package_bundle_capture(const sh_package_sources *sources,
    size_t owner, const char *cache_root, char *error, size_t capacity);

/* Retain exact original ZIP bytes BEFORE any migration. Validates paths, CRCs,
 * complete tree identity and presence of package.json, without requiring its
 * descriptor to be modern. This does not admit legacy authoring or convert it;
 * the caller separately validates/interprets it and records the relationship
 * between this original tree and any derived compiler inventory. */
sh_package_bundle *sh_package_bundle_from_archive(const unsigned char *bytes,
    size_t length, const char *cache_root, char *error, size_t capacity);
/* Same original bytes, with the canonical incoming carrier that legacy
 * conversion used as its fallback descriptor ID. It is transport metadata,
 * never an authored descriptor change. A re-save encodes this seed once. */
sh_package_bundle *sh_package_bundle_from_legacy_archive(const unsigned char *bytes,
    size_t length, const char *seed, const char *cache_root, char *error, size_t capacity);
void sh_package_bundle_delivery_key(const sh_package_bundle *bundle,
    char out[SH_PACKAGE_BUNDLE_KEY_CAP]);
/* Recognize an exact complete-tree key. A legacy key also returns its decoded
 * canonical seed. Returns zero for other strings; no archive is trusted until
 * the caller verifies the returned fingerprint against its original bytes. */
int sh_package_bundle_parse_key(const char *key, unsigned char fingerprint[32],
    char seed[SH_PACKAGE_ID_CAP]);

sh_package_bundle *sh_package_bundle_retain(sh_package_bundle *bundle);
void sh_package_bundle_release(sh_package_bundle *bundle);
/* Original complete tree identity, never descriptor ID or a converted tree. */
const unsigned char *sh_package_bundle_fingerprint(const sh_package_bundle *bundle);
/* malloc-owned exact retained archive bytes. The caller owns a reference for
 * the duration of this read. No editable path or current provider is consulted. */
unsigned char *sh_package_bundle_read(sh_package_bundle *bundle, size_t *length,
    char *error, size_t capacity);

/* The product data directory under which derived deliveries are retained
 * (package-cache/bundles). Set at startup; NULL until then. */
void sh_package_bundle_set_cache_root(const char *root);
const char *sh_package_bundle_cache_root(void);

/* A component delivery (D4): the complete tree of one nested component of
 * `outer`, re-rooted so its own package.json is the root descriptor, plus the
 * enclosing tree's files its dependency closure resolves to, at their own
 * paths, and a root "package-delivery.json" recording the enclosing
 * descriptor's id and name, the component path and those files. Nothing inside
 * the component is left out. An enclosing path inside the component, missing
 * from the outer tree or colliding with a component member refuses the
 * delivery, as does a component without a package.json. The result is an
 * ordinary complete delivery with its own tree identity. */
#define SH_PACKAGE_DELIVERY_METADATA "package-delivery.json"
sh_package_bundle *sh_package_bundle_component(sh_package_bundle *outer, const char *component,
    const char *const *enclosing, size_t enclosing_count, const char *root,
    char *error, size_t capacity);

#endif
