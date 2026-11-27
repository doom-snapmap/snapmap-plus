#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "modsound.h"
#include "modmanifest.h"
#include "modserve.h"
#include "modpin.h"

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
typedef struct bank_selection {
    sh_modsound_table *table;
    const char *build;
} bank_selection;

static void select_path(void *context, const char *path)
{
    bank_selection *s = (bank_selection *)context;
    for (int i = 0; i < s->table->count; ++i) {
        sh_modsound_bank *b = &s->table->banks[i];
        if (b->author_path && !strcmp(s->build, b->build) && !strcmp(path, b->author_path)) b->wanted = 1;
    }
}

static void select_row(sh_modsound_table *t, sh_modmanifest_row *r)
{
    sh_modserve_bundle_ref ref;
    bank_selection s;
    int i, whole;
    for (i = 0; i < t->count; i++) if (!strcmp(t->banks[i].bundle, r->salt)) break;
    if (i == t->count || !sh_modserve_row_build(r, &ref)) return;
    whole = r->pinned || r->attached || !r->name_count;
    s.table = t; s.build = ref.hash;
    if (!whole && !sh_modpin_object_banks(r, ref.hash, select_path, &s)) whole = 1;
    if (whole) for (i = 0; i < t->count; i++) if (!strcmp(t->banks[i].build, ref.hash)) t->banks[i].wanted = 1;
    sh_modserve_bundle_ref_free(&ref);
}

void sh_modsound_select_banks(sh_modsound_table *t)
{
    static const sh_modsound_table *last;
    static LONG last_revision, last_asked;
    static int last_open;
    sh_modmanifest_list copy = {0};
    int open = sh_modserve_scope_map_open(), ok;
    size_t i;
    LONG revision, asked = sh_modpin_asked_revision();
    AcquireSRWLockShared(&g_lock);
    revision = g_revision;
    if (t == last && revision == last_revision && asked == last_asked && open == last_open) {
        ReleaseSRWLockShared(&g_lock);
        return;
    }
    ok = !open || copy_rows(&copy, &g_rows);
    ReleaseSRWLockShared(&g_lock);
    if (ok) {
        for (i = 0; i < (size_t)t->count; i++) t->banks[i].wanted = 0;
        for (i = 0; i < copy.count; i++) select_row(t, &copy.rows[i]);
        last = t;
        last_revision = revision;
        last_asked = asked;
        last_open = open;
    }
    sh_modmanifest_list_free(&copy);
}
