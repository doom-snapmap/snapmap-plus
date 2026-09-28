/* Native map representation of package entity definitions (R137).
 *
 * A map entity whose inline entityDef inherits a product-served definition P
 * can often be saved as native SnapMap data instead: inherit the first native,
 * unchanged ancestor S and carry P's own edits as inline state. Each verified
 * site is decided on its own, with its complete dependency graph; another use
 * of P may keep its package identity. Only exact equivalents convert:
 * the combined inline layer must expand to the same state as P's chain plus the
 * instance's own values, every value must survive SnapMap's carrier, and the
 * site must not depend on P's lookup identity. Everything else keeps its
 * package route, with the reason and whether it is a native limit. */
#ifndef SH_PACKAGE_NATIVE_INLINE_H
#define SH_PACKAGE_NATIVE_INLINE_H

#include "decl_dependencies.h"
#include "decl_native_carrier.h"

typedef struct sh_native_inline_source {
    void *context;
    /* Effective entityDef text in this map's authored context. Returns 1 with
     * malloc-owned text and served=1 when the product serves that identity
     * (package-private, or a stock name a package changes), served=0 for the
     * unchanged native original; 0 absent; -1 unavailable. A served definition
     * that replaces a stock identity under a private name also reports that
     * stock name through replaces (may be NULL), malloc-owned. */
    int (*read)(void *context, const char *name, sh_decl_source *text, int *served, char **replaces);
    /* Native class ancestry: 1 derives, 0 unrelated, -1 unavailable. */
    int (*derives)(void *context, const char *class_name, const char *base_name);
    /* Reflected field schema for typed checks of converted state. Required. */
    const sh_decl_dependency_schema *schema;
    /* Lookup identity: 1 when a map site other than this entity's own inherit
     * names the same bound definition (map logic may select entities by it);
     * 0 when none does; -1 unavailable. */
    int (*looked_up)(void *context, size_t entity, const char *name);
    /* Caller exclusion: return a static reason to keep this entity's package
     * route (and set *limit: ENGINE when native map data cannot carry what the
     * package still does, such as a global stock change), or NULL to allow
     * conversion. May be NULL. */
    const char *(*keep)(void *context, size_t entity, const char *name, sh_decl_carrier_limit *limit);
    /* The entity carrier's current `~version`; other versions are upgraded by
     * the native reader first, so their state is left alone. */
    int version;
    /* Native placement. SnapMap's map validator admits an entity only when its
     * inherit names the entityDef of a palette row (a snapEditorEntityDef).
     * Return NULL when base, the native ancestor the site would inherit, has a
     * shipped row and any editor row of the site's definition is a plain copy
     * of it; otherwise a reason (valid until the next call) with *limit. May
     * be NULL only where no map validation applies. */
    const char *(*placement)(void *context, const char *from, const char *base, sh_decl_carrier_limit *limit);
} sh_native_inline_source;

typedef struct sh_native_inline_site {
    size_t entity;              /* index in the document's entities array */
    char *from, *to;            /* P, and S when converted */
    int converted;
    sh_decl_carrier_limit limit; /* ENGINE for a native limit, GAP otherwise */
    char reason[320];
} sh_native_inline_site;

typedef struct sh_native_inline_report {
    sh_native_inline_site *sites;
    size_t count;
} sh_native_inline_report;
void sh_native_inline_report_free(sh_native_inline_report *report);

/* Rewrite eligible entityDef values of one native map document. Entities whose
 * inherit is not product-served are untouched and unreported. Bytes outside
 * rewritten entityDef values stay exact. Returns malloc-owned JSON (a copy when
 * nothing converts) and fills report; NULL with error on operational failure
 * (report then stays empty). This never edits sources or the live editor map. */
char *sh_native_inline_document(const char *json, size_t length,
    const sh_native_inline_source *source, size_t *output_length,
    sh_native_inline_report *report, char *error, size_t capacity);

#endif
