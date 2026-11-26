#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "modsound.h"
#include "modmanifest.h"
#include "modserve.h"
#include "modcache.h"
#include "modpin.h"
#include "mp_json.h"

static SRWLOCK g_lock = SRWLOCK_INIT;
static sh_modmanifest_list g_rows;
static LONG g_revision;

static int same_rows(const sh_modmanifest_list *a, const sh_modmanifest_list *b)
{
    size_t i, j;
    if (a->count != b->count) return 0;
    for (i = 0; i < a->count; i++) {
        const sh_modmanifest_row *x = &a->rows[i], *y = &b->rows[i];
        if (strcmp(x->salt, y->salt) || strcmp(x->build, y->build) || x->attached != y->attached ||
            x->pinned != y->pinned || x->name_count != y->name_count) return 0;
        for (j = 0; j < x->name_count; j++) if (strcmp(x->names[j], y->names[j])) return 0;
    }
    return 1;
}

static int copy_rows(sh_modmanifest_list *dst, const sh_modmanifest_list *src)
{
    size_t i;
    for (i = 0; i < src->count; i++) if (!sh_modmanifest_list_add(dst, &src->rows[i])) return 0;
    return 1;
}

int sh_modsound_note_rows(const sh_modmanifest_list *rows)
{
    sh_modmanifest_list copy = {0}, empty = {0};
    int changed = 0;
    if (!rows) rows = &empty;
    AcquireSRWLockExclusive(&g_lock);
    if (!same_rows(&g_rows, rows) && copy_rows(&copy, rows)) {
        sh_modmanifest_list_free(&g_rows);
        g_rows = copy;
        memset(&copy, 0, sizeof copy);
        g_revision++;
        changed = 1;
    }
    ReleaseSRWLockExclusive(&g_lock);
    sh_modmanifest_list_free(&copy);
    return changed;
}

/* A published composition already contains exactly the row's files. For an installed author's placed
 * objects use the same object units as export, including the whole-mod fallback for dynamic dependencies. */
static int select_unit(sh_modsound_table *t, const char *build, const char *unit)
{
    size_t len = 0;
    char *text = sh_cache_manifest(unit, &len);
    mpj *doc = text ? mpj_parse(text, len) : NULL;
    mpj *files = doc ? mpj_get(doc, "files") : NULL;
    int i, j, ok = files && files->type == MPJ_ARR;
    for (i = 0; ok && i < files->n; i++) {
        const mpj *path = mpj_get(files->items[i], "path");
        if (!path || path->type != MPJ_STR) { ok = 0; break; }
        for (j = 0; j < t->count; j++) {
            sh_modsound_bank *b = &t->banks[j];
            if (b->author_path && !strcmp(b->build, build) && mpj_str_is(path, b->author_path)) b->wanted = 1;
        }
    }
    mpj_free(doc);
    free(text);
    return ok;
}

static void select_row(sh_modsound_table *t, sh_modmanifest_row *r)
{
    sh_modserve_bundle_ref ref;
    int i, whole;
    size_t k;
    for (i = 0; i < t->count; i++) if (!strcmp(t->banks[i].bundle, r->salt)) break;
    if (i == t->count || !sh_modserve_row_build(r, &ref)) return;
    whole = r->pinned || r->attached || !r->name_count;
    if (!whole && sh_modpin_row(r, NULL, 0, NULL, NULL) != SH_MODPIN_PINNED) whole = 1;
    for (k = 0; !whole && k < r->unit_count; k++) if (!select_unit(t, ref.hash, r->units[k])) whole = 1;
    if (whole) for (i = 0; i < t->count; i++) if (!strcmp(t->banks[i].build, ref.hash)) t->banks[i].wanted = 1;
    sh_modserve_bundle_ref_free(&ref);
}

void sh_modsound_select_banks(sh_modsound_table *t)
{
    static const sh_modsound_table *last;
    static LONG last_revision;
    static int last_open;
    sh_modmanifest_list copy = {0};
    int open = sh_modserve_scope_map_open(), ok;
    size_t i;
    LONG revision;
    AcquireSRWLockShared(&g_lock);
    revision = g_revision;
    if (t == last && revision == last_revision && open == last_open) { ReleaseSRWLockShared(&g_lock); return; }
    ok = !open || copy_rows(&copy, &g_rows);
    ReleaseSRWLockShared(&g_lock);
    if (ok) {
        for (i = 0; i < (size_t)t->count; i++) t->banks[i].wanted = 0;
        for (i = 0; i < copy.count; i++) select_row(t, &copy.rows[i]);
        last = t;
        last_revision = revision;
        last_open = open;
    }
    sh_modmanifest_list_free(&copy);
}
