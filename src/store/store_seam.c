/* store_seam.c -- what the store tool links in place of the game's serving
 * layer (modserve.c, modserve_engine.c) and the builder entry with the game's
 * resource reader (modcmd.c). The tool serves nothing: no bundle is active,
 * nothing is indexed, and a rescan has nothing to rebuild. The builder entry
 * builds a package a map carried (modlegacy.c, while the tool converts that
 * map) with the real builder over the store's installed bundles and the
 * game's archives read from disk (modstock.h), its replacements kept. */
#include <stddef.h>
#include <string.h>

#include "modserve.h"
#include "modmanifest.h"
#include "modbundle.h"
#include "modstore.h"
#include "modstock.h"

/* The registry choices' input (modmanifest.c gives them at every load). */
int sh_modserve_scope_set_choices(const char *const *slots, const char *const *keys, int n, int recompute)
{ (void)slots; (void)keys; (void)n; (void)recompute; return 0; }

int modserve_activate_bundles(const char *const *bundle_hashes, int n)
{
    (void)bundle_hashes;
    (void)n;
    return 0;
}

size_t modserve_bundles_for_names_visit(const char *const *names, size_t count, sh_modserve_closure_fn visit, void *context)
{
    (void)names; (void)count; (void)visit; (void)context;
    return 0;
}

int sh_modserve_bundle_state(const char *bundle_hash, char *cause, size_t cause_cap)
{
    (void)bundle_hash;
    if (cause && cause_cap) cause[0] = 0;
    return 0;
}

int sh_modserve_active(void) { return 0; }

/* The manifest's activation asks which build serves a row: none here. */
int sh_modserve_row_build(const sh_modmanifest_row *row, sh_modserve_bundle_ref *out)
{
    (void)row;
    if (out) memset(out, 0, sizeof *out);
    return 0;
}

int sh_modserve_index_rescan(void) { return 0; }

static int ss_stock(void *context, const char *request)
{
    (void)context;
    return sh_modstock_exists(request);
}

int modbundle_build_folder(const char *folder, char *first_cause, size_t n)
{
    sh_ms_list list;
    sh_mb_resolver r;
    sh_mb_bundle b;
    char cause[SH_MB_CAUSE_MAX];
    int ok = 0;
    if (first_cause && n) first_cause[0] = 0;
    memset(&list, 0, sizeof list);
    if (!sh_ms_enumerate(&list)) memset(&list, 0, sizeof list);
    r.installed = sh_ms_request_exists;
    r.installed_context = &list;
    r.stock = ss_stock;
    r.stock_context = NULL;
    sh_mb_keep_replacements_here(1);
    if (!sh_mb_build(folder, NULL, &r, &b)) {
        if (first_cause && n) strncpy_s(first_cause, n, b.first_cause, _TRUNCATE);
    } else if (!sh_mb_write(&b, folder, cause, sizeof cause)) {
        if (first_cause && n) strncpy_s(first_cause, n, cause, _TRUNCATE);
    } else ok = 1;
    sh_mb_keep_replacements_here(0);
    sh_mb_free(&b);
    sh_ms_list_free(&list);
    return ok;
}
