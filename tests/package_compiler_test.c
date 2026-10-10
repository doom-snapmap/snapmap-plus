/* Compile complete packages against verified originals, retaining all owners. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_compiler.h"
#include "decl_tree.h"
#include "resource_catalog.h"
#include "package_fixture.h"

static const char *inspector = "generated/decls/snappropertyinspector_whitelistencounterdecl/demo.decl";
static const char *original = "{ edit = { validEncounters = { num = 1; item[0] = \"stock\"; } } }";
static const char *conductor = "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl";
static const char *conductor_base = "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = \"4\"; entityDef = \"ai/imp\"; } } } }";
static const char *editor = "generated/decls/snapeditorentitydef/volume/blocking.decl";
static const char *editor_base = "{ edit = { propertySheets = { num = 1; item[0] = { properties = { "
    "num = 1; item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } } } } } }";

static int baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *value = NULL;
    (void)context;
    *body = NULL; *length = 0;
    if (!strcmp(path, inspector)) value = original;
    if (!strcmp(path, conductor)) value = conductor_base;
    if (!strcmp(path, editor)) value = editor_base;
    if (!strcmp(path, "cooked/model/replaced.bmodel")) value = "original model";
    if (!value) return 0;
    *length = strlen(value); *body = (unsigned char *)_strdup(value); return *body ? 1 : -1;
}

static int probe_skipped(void *context, const sh_package *package, const char *reason)
{
    (void)context;
    printf("skipped: %s: %s\n", package->name, reason);
    return 1;
}

/* Offline library probe with the runtime's local isolation: invalid packages
 * are reported and removed; any other failure fails the probe. It has no
 * product built-ins, native reflection or live engine state. */
static void probe(const char *data_root, const char *base)
{
    char error[2048];
    sh_resource_catalog *catalog = sh_resource_catalog_open(base, error, sizeof(error));
    sh_package_sources *sources;
    sh_package_compilation *compiled = NULL;
    sh_package_compile_environment environment = {0};
    size_t i, bytes = 0, decls = 0, invalid = SIZE_MAX;
    if (!catalog) { fprintf(stderr, "%s\n", error); CHECK(0); return; }
    printf("catalog: %zu rows\n", sh_resource_catalog_count(catalog));
    sources = sh_package_sources_scan_local(data_root, probe_skipped, NULL, error, sizeof(error));
    if (!sources) { fprintf(stderr, "%s\n", error); CHECK(0); sh_resource_catalog_close(catalog); return; }
    environment.baseline = sh_resource_catalog_read_path; environment.baseline_context = catalog;
    environment.invalid_package = &invalid;
    for (;;) {
        compiled = sh_package_compile_with(sources, &environment, error, sizeof(error));
        if (compiled || invalid >= sources->package_count) break;
        printf("skipped: %s: %s\n", sources->packages[invalid].name, error);
        if (!sh_package_sources_remove(sources, invalid)) break;
    }
    if (!compiled) { fprintf(stderr, "%s\n", error); CHECK(0); goto done; }
    for (i = 0; i < compiled->resource_count; i++) {
        size_t length = 0;
        unsigned char *body = sh_package_compilation_read(compiled, &compiled->resources[i], 256u * 1024u * 1024u, &length, error, sizeof(error));
        if (!body) { fprintf(stderr, "%s: %s\n", compiled->resources[i].engine_path, error); CHECK(0); }
        bytes += length; decls += compiled->resources[i].type != NULL; free(body);
    }
    printf("compiled: %zu packages, %zu resources, %zu declarations, %zu bytes; %zu identical duplicates, %zu composed\n",
        sources->package_count, compiled->resource_count, decls, bytes, compiled->duplicate_count, compiled->composed_count);
done:
    sh_package_compilation_free(compiled); sh_package_sources_free(sources); sh_resource_catalog_close(catalog);
}

/* ---- Manual stock-corpus sweep (--editor-sweep <doom-base>) ----------------
 * Every stock snapEditorEntityDef child whose nearest parent has a first
 * property page: that parent gains a peer's control without a path at the end
 * of page 0 (a second statement of the list, as the live b3-1-vk parent did),
 * the child is rebased onto it and restated natively, and an independent
 * native reader (all pages, nested fields flattened) reads the stock child
 * over the stock chain (S) and the served child over the published chain (R).
 * R must hold exactly S's controls plus the addition on page 0 and S's
 * controls on every other page, as multisets of complete field sets: no
 * control lost or merged, no previous occupant's field left anywhere. */
#define SWEEP_PAGES 8
#define SWEEP_SLOTS 96
#define SWEEP_FIELDS 32
typedef struct sweep_control { char key[SWEEP_FIELDS][96], value[SWEEP_FIELDS][192]; int count; } sweep_control;
typedef struct sweep_page { sweep_control own, controls[SWEEP_SLOTS]; int count; } sweep_page;
typedef struct sweep_sheets { sweep_page pages[SWEEP_PAGES]; int count, overflow; } sweep_sheets;
static void sweep_write(sweep_sheets *sheets, sweep_control *control, const sh_decl_node *node, const char *prefix, int depth)
{
    for (const sh_decl_node *field = node->children; field; field = field->next) {
        char key[96]; const char *text; size_t length; int at = 0;
        snprintf(key, sizeof(key), "%s%s", prefix, field->key);
        if (field->compound) {
            char nested[96];
            if (depth > 6) { sheets->overflow = 1; continue; }
            snprintf(nested, sizeof(nested), "%s.", key);
            sweep_write(sheets, control, field, nested, depth + 1);
            continue;
        }
        while (at < control->count && strcmp(control->key[at], key)) at++;
        if (at == control->count) { if (control->count >= SWEEP_FIELDS) { sheets->overflow = 1; continue; } control->count++; }
        snprintf(control->key[at], sizeof(control->key[at]), "%s", key);
        if (sh_decl_tree_literal(field, &text, &length)) snprintf(control->value[at], sizeof(control->value[at]), "%.*s", (int)length, text);
        else snprintf(control->value[at], sizeof(control->value[at]), "%s", field->value ? field->value : "");
    }
}
static int sweep_count(const sh_decl_node *list, int *count)
{
    for (const sh_decl_node *child = list->children; child; child = child->next)
        if (!strcmp(child->key, "num")) {
            size_t n = 0;
            if (!sh_decl_tree_list_count(child, &n)) n = 0;
            *count = (int)n; return 1;
        }
    return 0;
}
static int sweep_read(sweep_sheets *sheets, const char *text, size_t length)
{
    sh_decl_node *tree = sh_decl_tree_parse_ordered((sh_decl_source){text, length}, NULL, 0);
    if (!tree) return 0;
    for (const sh_decl_node *edit = tree->children; edit; edit = edit->next) {
        if (strcmp(edit->key, "edit") || !edit->compound) continue;
        for (const sh_decl_node *sheet = edit->children; sheet; sheet = sheet->next) {
            int count, t;
            if (strcmp(sheet->key, "propertySheets") || !sheet->compound) continue;
            if (sweep_count(sheet, &count)) {
                if (count < 1) sheets->count = 0;
                else {
                    if (count > SWEEP_PAGES) { sheets->overflow = 1; count = SWEEP_PAGES; }
                    for (int fresh = sheets->count; fresh < count; fresh++) memset(sheets->pages + fresh, 0, sizeof(sheets->pages[fresh]));
                    sheets->count = count;
                }
            }
            for (const sh_decl_node *item = sheet->children; item; item = item->next) {
                sweep_page *page;
                if (sscanf_s(item->key, "item[%d]", &t) != 1 || t < 0 || t >= sheets->count || !item->compound) continue;
                page = sheets->pages + t;
                for (const sh_decl_node *field = item->children; field; field = field->next) {
                    int slots, j;
                    if (strcmp(field->key, "properties") || !field->compound) {
                        if (!field->compound) {
                            sh_decl_node one = *field; sh_decl_node holder = {0};
                            one.next = NULL; holder.compound = 1; holder.children = &one;
                            sweep_write(sheets, &page->own, &holder, "", 0);
                        } else {
                            char prefix[96]; snprintf(prefix, sizeof(prefix), "%s.", field->key);
                            sweep_write(sheets, &page->own, field, prefix, 1);
                        }
                        continue;
                    }
                    if (sweep_count(field, &slots)) {
                        if (slots < 1) page->count = 0;
                        else {
                            if (slots > SWEEP_SLOTS) { sheets->overflow = 1; slots = SWEEP_SLOTS; }
                            for (int fresh = page->count; fresh < slots; fresh++) memset(page->controls + fresh, 0, sizeof(page->controls[fresh]));
                            page->count = slots;
                        }
                    }
                    for (const sh_decl_node *control = field->children; control; control = control->next)
                        if (sscanf_s(control->key, "item[%d]", &j) == 1 && j >= 0 && j < page->count && control->compound)
                            sweep_write(sheets, page->controls + j, control, "", 0);
                }
            }
        }
    }
    sh_decl_tree_free(tree);
    return 1;
}
static void sweep_format(sweep_control *control, char *out, size_t capacity)
{
    out[0] = 0;
    for (int a = 0; a < control->count; a++) for (int b = a + 1; b < control->count; b++)
        if (strcmp(control->key[b], control->key[a]) < 0) {
            char key[96], value[192];
            memcpy(key, control->key[a], sizeof(key)); memcpy(control->key[a], control->key[b], sizeof(key)); memcpy(control->key[b], key, sizeof(key));
            memcpy(value, control->value[a], sizeof(value)); memcpy(control->value[a], control->value[b], sizeof(value));
            memcpy(control->value[b], value, sizeof(value));
        }
    int emitted = 0;
    for (int f = 0; f < control->count; f++) {
        if (!control->value[f][0] || (!strcmp(control->key[f], "inspector") && !strcmp(control->value[f], "NULL"))) continue;
        if (emitted++) strcat_s(out, capacity, ",");
        strcat_s(out, capacity, control->key[f]); strcat_s(out, capacity, "="); strcat_s(out, capacity, control->value[f]);
    }
}
static int sweep_string_order(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
/* Page t's controls as a sorted multiset ("|"-joined), page fields first. */
static char *sweep_page_set(sweep_sheets *sheets, int t, const char *extra)
{
    char *items[SWEEP_SLOTS + 1], line[8192], own[4096];
    size_t count = 0, total = 1;
    char *out;
    if (t >= sheets->count) return _strdup("-");
    for (int j = 0; j < sheets->pages[t].count; j++) {
        sweep_format(sheets->pages[t].controls + j, line, sizeof(line));
        items[count++] = _strdup(line);
    }
    if (extra) items[count++] = _strdup(extra);
    qsort(items, count, sizeof(items[0]), sweep_string_order);
    sweep_format(&sheets->pages[t].own, own, sizeof(own));
    total += strlen(own) + 2;
    for (size_t i = 0; i < count; i++) total += strlen(items[i]) + 3;
    out = malloc(total);
    snprintf(out, total, "%s", own);
    for (size_t i = 0; i < count; i++) { strcat_s(out, total, " | "); strcat_s(out, total, items[i]); free(items[i]); }
    return out;
}
static char *sweep_stock(sh_resource_catalog *catalog, const char *name, size_t *length)
{
    char path[512]; unsigned char *body = NULL;
    snprintf(path, sizeof(path), "generated/decls/snapeditorentitydef/%s.decl", name);
    if (sh_resource_catalog_read_path(catalog, path, &body, length) <= 0) return NULL;
    return (char *)body;
}
static char *sweep_inherit(const char *text, size_t length)
{
    sh_decl_node *tree = sh_decl_tree_parse_ordered((sh_decl_source){text, length}, NULL, 0);
    const char *value = NULL; size_t n = 0; char *out = NULL;
    for (const sh_decl_node *field = tree ? tree->children : NULL; field; field = field->next)
        if (!strcmp(field->key, "inherit") && sh_decl_tree_literal(field, &value, &n) && n) {
            out = malloc(n + 1); memcpy(out, value, n); out[n] = 0;
        }
    sh_decl_tree_free(tree);
    return out;
}
static void editor_sweep(const char *base)
{
    char error[2048];
    sh_resource_catalog *catalog = sh_resource_catalog_open(base, error, sizeof(error));
    size_t children = 0, rebased = 0, unchanged = 0, restated = 0, gaps = 0, failed = 0, mismatched = 0, skipped = 0, overflow = 0;
    const char *prefix = "generated/decls/snapeditorentitydef/";
    char **done = NULL; size_t done_count = 0, done_capacity = 0;
    if (!catalog) { fprintf(stderr, "%s\n", error); CHECK(0); return; }
    for (size_t e = 0; e < sh_resource_catalog_count(catalog); e++) {
        const sh_resource_catalog_entry *entry = sh_resource_catalog_at(catalog, e);
        char name[512], *child = NULL, *chain_text[32] = {0}, *published_first = NULL, *served = NULL, *out = NULL, *exact = NULL;
        size_t child_length = 0, chain_length[32] = {0}, depth = 0, path_length;
        sweep_sheets *stock = NULL, *read = NULL;
        sweep_control parent_own;
        sh_decl_source authored[32], published[32];
        int result = 0, kind = 0, encoded = 0, mismatch = 0;
        int seen = 0;
        if (!entry || !entry->path || _strnicmp(entry->path, prefix, strlen(prefix))) continue;
        path_length = strlen(entry->path);
        if (path_length < strlen(prefix) + 6 || _stricmp(entry->path + path_length - 5, ".decl")) continue;
        snprintf(name, sizeof(name), "%.*s", (int)(path_length - strlen(prefix) - 5), entry->path + strlen(prefix));
        /* One sweep per path: archives and patches list a path more than once
         * (the read takes the SnapMap provider). */
        for (size_t k = 0; k < done_count && !seen; k++) seen = !_stricmp(done[k], name);
        if (seen) continue;
        if (done_count == done_capacity) {
            size_t grown_capacity = done_capacity ? done_capacity * 2 : 256;
            char **grown = realloc(done, grown_capacity * sizeof(*grown));
            if (!grown) { CHECK(0); break; }
            done = grown; done_capacity = grown_capacity;
        }
        done[done_count++] = _strdup(name);
        if (!(child = sweep_stock(catalog, name, &child_length))) continue;
        {
            char *parent = sweep_inherit(child, child_length);
            while (parent && depth < 32) {
                char *next;
                if (!(chain_text[depth] = sweep_stock(catalog, parent, &chain_length[depth]))) break;
                next = sweep_inherit(chain_text[depth], chain_length[depth]);
                free(parent); parent = next; depth++;
            }
            free(parent);
        }
        if (!depth) { free(child); continue; }
        children++;
        stock = calloc(1, sizeof(*stock)); read = calloc(1, sizeof(*read));
        CHECK(stock && read);
        /* The parent chain alone gives page 0's control count. */
        for (size_t i = depth; i > 0; i--) sweep_read(stock, chain_text[i - 1], chain_length[i - 1]);
        if (!stock->count) { skipped++; goto next; }
        {
            int controls = stock->pages[0].count;
            char addition[256];
            size_t length = chain_length[0];
            const char *close = chain_text[0] + length;
            while (close > chain_text[0] && close[-1] != '}') close--;
            if (close == chain_text[0]) { skipped++; goto next; }
            snprintf(addition, sizeof(addition), " edit = { propertySheets = { item[0] = { properties = { num = %d; "
                "item[%d] = { displayNameTag = \"#sweep_added\"; } } } } } ", controls + 1, controls);
            published_first = malloc(length + strlen(addition) + 2);
            memcpy(published_first, chain_text[0], (size_t)(close - chain_text[0]) - 1);
            published_first[(close - chain_text[0]) - 1] = 0;
            strcat_s(published_first, length + strlen(addition) + 2, addition);
            strcat_s(published_first, length + strlen(addition) + 2, "}");
        }
        memset(stock, 0, sizeof(*stock));
        for (size_t i = depth; i > 0; i--) sweep_read(stock, chain_text[i - 1], chain_length[i - 1]);
        parent_own = stock->pages[0].own;
        sweep_read(stock, child, child_length);
        for (size_t i = 0; i < depth; i++) {
            authored[i] = (sh_decl_source){chain_text[i], chain_length[i]};
            published[i] = i ? authored[i] : (sh_decl_source){published_first, strlen(published_first)};
        }
        /* As the library does first: a child stating a member more than once
         * composes as its native form over its parents. */
        if (sh_package_editor_repeats((sh_decl_source){child, child_length}) == 1) {
            char *form = NULL; size_t form_length = 0;
            int formed = sh_package_editor_native_form((sh_decl_source){child, child_length}, authored, depth, &form, &form_length);
            if (formed < 0) { failed++; printf("FAIL %s: native form %d\n", name, formed); goto next; }
            if (formed) { free(child); child = form; child_length = form_length; }
        }
        {
            sh_package_editor_failure failure; char reason[512]; size_t length = 0;
            result = sh_package_editor_rebase("snapeditorentitydef", (sh_decl_source){child, child_length}, authored, depth,
                published, depth, 0, NULL, &out, &length, &failure, reason, sizeof(reason));
            kind = failure.kind;
            if (result < 0) { failed++; printf("FAIL %s: rebase kind %d: %s\n", name, kind, reason); goto next; }
        }
        if (!result) { unchanged++; served = child; }
        else {
            size_t length = 0;
            rebased++;
            encoded = sh_package_editor_native_restatement("snapeditorentitydef", (sh_decl_source){out, strlen(out)},
                published, depth, &exact, &length);
            if (encoded == -3) { gaps++; printf("GAP %s\n", name); goto next; }
            if (encoded < 0) { failed++; printf("FAIL %s: restatement %d\n", name, encoded); goto next; }
            if (encoded) restated++;
            served = exact ? exact : out;
        }
        memset(read, 0, sizeof(*read));
        for (size_t i = depth; i > 1; i--) sweep_read(read, chain_text[i - 1], chain_length[i - 1]);
        sweep_read(read, published_first, strlen(published_first));
        sweep_read(read, served, served == child ? child_length : strlen(served));
        if (stock->overflow || read->overflow) { overflow++; printf("OVERFLOW %s\n", name); goto next; }
        /* A child that states a zero page count removes every inherited page, but
         * a peer's addition to the parent survives it (a removal never erases a
         * peer's addition): page 0 then holds that control alone, under the
         * parent's own page fields, with no control of the stock reading. */
        {
            int emptied = !stock->count, want_pages = emptied ? 1 : stock->count;
            if (want_pages != read->count) {
                mismatch = 1; printf("MISMATCH %s: %d pages read, %d expected\n", name, read->count, want_pages);
            }
            for (int t = 0; !mismatch && t < want_pages; t++) {
                char *want, *got = sweep_page_set(read, t, NULL);
                if (emptied) {
                    sweep_sheets *only = calloc(1, sizeof(*only));
                    CHECK(only);
                    only->count = 1; only->pages[0].own = parent_own;
                    want = sweep_page_set(only, 0, "displayNameTag=#sweep_added");
                    free(only);
                } else want = sweep_page_set(stock, t, t ? NULL : "displayNameTag=#sweep_added");
                if (strcmp(want, got)) {
                    mismatch = 1;
                    printf("MISMATCH %s page %d\n  want %s\n  got  %s\n", name, t, want, got);
                }
                free(want); free(got);
            }
        }
        if (mismatch) mismatched++;
next:
        free(child); free(published_first); free(out); free(exact); free(stock); free(read);
        for (size_t i = 0; i < depth; i++) free(chain_text[i]);
    }
    printf("editor sweep: %zu children, %zu rebased (%zu restated with explicit defaults), %zu unchanged, %zu gaps, %zu failed, "
        "%zu mismatched, %zu skipped (no page under the parent), %zu over reader limits\n",
        children, rebased, restated, unchanged, gaps, failed, mismatched, skipped, overflow);
    CHECK(!failed && !mismatched && !gaps);
    for (size_t k = 0; k < done_count; k++) free(done[k]);
    free(done);
    sh_resource_catalog_close(catalog);
}

static void expect_change(const sh_package_changes *changes, size_t i,
    const char *path, sh_package_change_kind kind)
{
    CHECK(i < changes->count);
    if (i < changes->count) CHECK(!strcmp(changes->items[i].path, path) && changes->items[i].kind == kind);
}

static void test_native_identity_changes(void)
{
    sh_compiled_resource a[2] = {0}, b[2] = {0};
    sh_package_compilation before = {0}, after = {0};
    sh_package_changes changes = {0};
    char error[1024];
    for (size_t i = 0; i < 2; i++) {
        a[i].engine_path = i ? "compiled/vertex.bin" : "compiled/fragment.bin";
        a[i].body = (unsigned char *)"same"; a[i].body_length = 4;
        a[i].native_type = "renderprog"; a[i].native_name = "smp/shared";
        b[i] = a[i];
    }
    before.resources = a; before.resource_count = 2;
    after.resources = b; after.resource_count = 1;
    /* One physical permutation disappears, but its native object still has a
     * provider. Neither direction should retire/reintroduce that identity. */
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
    CHECK(changes.count == 1 && changes.identities_complete && !changes.removed_count && !changes.added_count);
    CHECK(changes.items[0].type && !strcmp(changes.items[0].type, "renderprog"));
    CHECK(changes.items[0].name && !strcmp(changes.items[0].name, "smp/shared"));
    sh_package_changes_reverse(&changes);
    CHECK(changes.items[0].kind == SH_PACKAGE_RESOURCE_ADDED && !changes.removed_count && !changes.added_count);
    sh_package_changes_reverse(&changes);
    CHECK(changes.items[0].kind == SH_PACKAGE_RESOURCE_REMOVED);
    /* Same path and bytes with a new native binding still needs activation.
     * Both old and new identity sets must survive the rollback reversal. */
    b[0].native_name = "smp/variant";
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
    CHECK(changes.count == 1 && changes.removed_count == 1 && changes.added_count == 1);
    CHECK(!strcmp(changes.items[0].path, "compiled/vertex.bin") &&
        changes.items[0].kind == SH_PACKAGE_RESOURCE_REMOVED);
    CHECK(!strcmp(changes.removed[0].name, "smp/shared") && !strcmp(changes.added[0].name, "smp/variant"));
    sh_package_changes_reverse(&changes);
    CHECK(changes.items[0].kind == SH_PACKAGE_RESOURCE_ADDED &&
        !strcmp(changes.removed[0].name, "smp/variant") && !strcmp(changes.added[0].name, "smp/shared"));
    sh_package_changes_reverse(&changes);
    /* Changing bytes as well produces a physical replacement with its own
     * previous identity, independently of the complete logical identity sets. */
    b[0].body = (unsigned char *)"edit";
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
    CHECK(changes.count == 2 && changes.removed_count == 1 && changes.added_count == 1);
    CHECK(!strcmp(changes.removed[0].name, "smp/shared") && !strcmp(changes.added[0].name, "smp/variant"));
    CHECK(!strcmp(changes.items[0].name, "smp/variant") && !strcmp(changes.items[0].previous_name, "smp/shared"));
    CHECK(changes.items[0].name != b[0].native_name && changes.items[0].previous_name != a[0].native_name);
    sh_package_changes_reverse(&changes);
    CHECK(!strcmp(changes.items[0].name, "smp/shared") && !strcmp(changes.items[0].previous_name, "smp/variant"));
    CHECK(!strcmp(changes.removed[0].name, "smp/variant") && !strcmp(changes.added[0].name, "smp/shared"));
    sh_package_changes_reverse(&changes);
    CHECK(!strcmp(changes.items[0].name, "smp/variant") && !strcmp(changes.removed[0].name, "smp/shared"));
    /* Complete removal deduplicates all physical members to one retirement. */
    CHECK(sh_package_compilation_changes(&before, NULL, &changes, error, sizeof(error)));
    CHECK(changes.count == 2 && changes.removed_count == 1 && !changes.added_count);
    before.resource_count = 1; a[0].native_type = a[0].native_name = NULL;
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
    CHECK(changes.count == 1 && !changes.items[0].previous_type && !changes.removed_count && changes.added_count == 1);
    sh_package_changes_reverse(&changes);
    CHECK(!changes.items[0].type && !strcmp(changes.items[0].previous_name, "smp/variant") && changes.removed_count == 1);
    /* Invalid half-bindings never publish partial file or native diffs. */
    b[0].native_type = NULL;
    CHECK(!sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
    CHECK(!changes.items && !changes.added && !changes.removed && !changes.identities_complete);
    b[0].native_type = "renderprog"; b[0].type = "material"; b[0].name = "smp/variant";
    CHECK(!sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
    sh_package_changes_free(&changes); sh_package_changes_reverse(NULL);
}

static void test_cached_changes(void)
{
    /* Generated native files have sealed payloads, not authored source rows.
     * Exercise all representation pairs and differences beyond one IO chunk. */
    const size_t length = 131073;
    unsigned char *body = malloc(length + 1);
    char path[4096], error[1024];
    sh_package_file *files[3] = {0};
    sh_package_changes changes = {0};
    sh_compiled_resource a = {0}, b = {0};
    sh_package_compilation before = {0}, after = {0};
    CHECK(body); if (!body) return;
    memset(body, 'a', length); body[length] = 0;
    for (size_t i = 0; i < 3; i++) {
        char relative[64]; snprintf(relative, sizeof(relative), "sealed-comparison-%zu", i);
        if (i == 2) body[length - 1] = 'b';
        create(relative, (char *)body);
        snprintf(path, sizeof(path), "%s/%s", root, relative);
        files[i] = sh_package_file_pin(path, error, sizeof(error)); CHECK(files[i]);
    }
    body[length - 1] = 'a';
    a.engine_path = b.engine_path = "sound/soundbanks/pc/100.bnk";
    a.source = b.source = SIZE_MAX;
    a.cache_file = files[0]; b.cache_file = files[1];
    before.resources = &a; after.resources = &b; before.resource_count = after.resource_count = 1;
    if (files[0] && files[1] && files[2]) {
        CHECK(sh_package_compilation_changes(NULL, &before, &changes, error, sizeof(error)) && changes.count == 1);
        CHECK(sh_package_compilation_changes(&before, NULL, &changes, error, sizeof(error)) && changes.count == 1);
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
        CHECK(sh_package_compilation_probe(&before, a.engine_path, error, sizeof(error)) == 1);
        a.body = body; a.body_length = length;
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
        CHECK(sh_package_compilation_changes(&after, &before, &changes, NULL, 0) && !changes.count);
        b.cache_file = files[2];
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
        a.body = NULL;
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
        /* Authored source metadata cannot override the generated payload. */
        sh_package_source_file source = {0}; sh_package_sources sources = {0};
        sh_package_file_identity identity;
        CHECK(sh_package_bytes_identity(body, length, &identity));
        sources.files = &source; sources.file_count = 1;
        source.length = identity.length; memcpy(source.digest, identity.digest, sizeof(source.digest));
        before.sources = after.sources = &sources; a.source = b.source = 0;
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
        CHECK(sh_package_compilation_probe(&before, a.engine_path, error, sizeof(error)) == 1);
        b.cache_file = files[1];
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
        a.cache_file = NULL; before.sources = NULL;
        CHECK(!sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
        CHECK(sh_package_compilation_probe(&before, a.engine_path, error, sizeof(error)) == -1);
    }
    sh_package_changes_free(&changes);
    for (size_t i = 0; i < 3; i++) sh_package_file_release(files[i]);
    free(body);
}

static void test_changes(const sh_package_compilation *compiled)
{
    test_cached_changes();
    test_native_identity_changes();
    sh_package_changes changes = {0};
    char error[1024];
    CHECK(sh_package_compilation_changes(NULL, NULL, &changes, error, sizeof(error)) && !changes.count);
    CHECK(sh_package_compilation_changes(NULL, compiled, &changes, error, sizeof(error)));
    CHECK(changes.count == compiled->resource_count);
    for (size_t i = 0; i < changes.count; i++) {
        expect_change(&changes, i, compiled->resources[i].engine_path, SH_PACKAGE_RESOURCE_ADDED);
        if (compiled->resources[i].type) {
            CHECK(changes.items[i].type && changes.items[i].name);
            CHECK(changes.items[i].type != compiled->resources[i].type);
            CHECK(changes.items[i].name != compiled->resources[i].name);
            CHECK(!strcmp(changes.items[i].type, compiled->resources[i].type));
            CHECK(!strcmp(changes.items[i].name, compiled->resources[i].name));
        } else CHECK(!changes.items[i].type && !changes.items[i].name);
    }
    CHECK(sh_package_compilation_changes(compiled, NULL, &changes, error, sizeof(error)));
    for (size_t i = 0; i < changes.count; i++) {
        expect_change(&changes, i, compiled->resources[i].engine_path, SH_PACKAGE_RESOURCE_REMOVED);
        if (compiled->resources[i].type) {
            CHECK(changes.items[i].type && changes.items[i].name);
            CHECK(!strcmp(changes.items[i].type, compiled->resources[i].type));
            CHECK(!strcmp(changes.items[i].name, compiled->resources[i].name));
        }
    }

    /* Different bundle ownership and duplicate counts do not change the game
     * payload. No copying/removal of those authored sources is required. */
    sh_compiled_resource resources[5];
    sh_package_compilation other = *compiled;
    CHECK(compiled->resource_count == 5);
    memcpy(resources, compiled->resources, sizeof(resources)); other.resources = resources;
    other.duplicate_count += 100;
    for (size_t i = 0; i < other.resource_count; i++) {
        resources[i].owners = (sh_package_owners){0}; resources[i].source_count = 0;
    }
    CHECK(sh_package_compilation_changes(compiled, &other, &changes, error, sizeof(error)) && !changes.count);

    /* Same provider path, different effective bytes. Embedded NULs count. */
    unsigned char bytes_a[] = {'x', 0, 'a'}, bytes_b[] = {'x', 0, 'b'};
    sh_compiled_resource a[3] = {0}, b[3] = {0};
    sh_package_compilation before = {0}, after = {0};
    before.resources = a; after.resources = b; before.resource_count = after.resource_count = 3;
    a[0].engine_path = "a"; a[1].engine_path = "c"; a[2].engine_path = "d";
    b[0].engine_path = "b"; b[1].engine_path = "c"; b[2].engine_path = "d";
    for (int i = 0; i < 3; i++) {
        a[i].body = bytes_a; b[i].body = i == 1 ? bytes_b : bytes_a;
        a[i].body_length = b[i].body_length = sizeof(bytes_a);
    }
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 3);
    expect_change(&changes, 0, "a", SH_PACKAGE_RESOURCE_REMOVED);
    expect_change(&changes, 1, "b", SH_PACKAGE_RESOURCE_ADDED);
    expect_change(&changes, 2, "c", SH_PACKAGE_RESOURCE_REPLACED);
    CHECK(sh_package_compilation_changes(&after, &before, &changes, error, sizeof(error)) && changes.count == 3);
    expect_change(&changes, 0, "a", SH_PACKAGE_RESOURCE_ADDED);
    expect_change(&changes, 1, "b", SH_PACKAGE_RESOURCE_REMOVED);
    expect_change(&changes, 2, "c", SH_PACKAGE_RESOURCE_REPLACED);
    before.resource_count = after.resource_count = 1; b[0] = a[0];
    b[0].type = "material"; b[0].name = "a";
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
    b[0] = a[0]; b[0].body_length--;
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);

    /* Compare a verified opaque source with the same bytes held in memory;
     * changing the representation must not cause a false reload. */
    const sh_compiled_resource *image = sh_package_compilation_find(compiled, "generated/image/shared.bimage");
    CHECK(image && !image->body);
    if (image) {
        size_t length = 0;
        unsigned char *body = sh_package_compilation_read(compiled, image, SIZE_MAX, &length, error, sizeof(error));
        CHECK(body && length);
        a[0] = b[0] = *image; before.sources = after.sources = compiled->sources;
        b[0].body = body; b[0].body_length = length;
        if (body && length) {
            CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
            CHECK(sh_package_compilation_changes(&after, &before, &changes, error, sizeof(error)) && !changes.count);
            body[length - 1] ^= 1;
            CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
            a[0].cache_path = "Z:/nonexistent-package-comparison-fixture";
            CHECK(!sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
            CHECK(error[0] && !changes.count && !changes.items);
        }
        free(body);
        /* Captured opaque identities survive source retirement and use full
         * 64-bit lengths without allocating or reading gigabytes. */
        sh_package_source_file af = compiled->sources->files[image->source], bf = af;
        sh_package_sources as = {0}, bs = {0};
        as.files = &af; bs.files = &bf; as.file_count = bs.file_count = 1;
        before.sources = &as; after.sources = &bs; a[0] = b[0] = *image;
        a[0].source = b[0].source = 0;
        af.absolute = bf.absolute = "Z:/nonexistent-package-comparison-fixture";
        af.length = bf.length = UINT64_C(1) << 33;
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
        bf.length++;
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
        bf.length = af.length; bf.digest[31] ^= 1;
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
        bs.files = NULL;
        CHECK(!sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
        bs.files = &bf; bf.directory = 1;
        CHECK(!sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
    }
    before.sources = after.sources = NULL;
    a[0] = a[1] = (sh_compiled_resource){0}; a[0].body = a[1].body = bytes_a;
    a[0].engine_path = "b"; a[1].engine_path = "a"; before.resource_count = 2;
    CHECK(!sh_package_compilation_changes(&before, NULL, &changes, error, sizeof(error)) && !changes.count);
    a[1].engine_path = "b";
    CHECK(!sh_package_compilation_changes(&before, NULL, &changes, NULL, 0) && !changes.items);
    CHECK(!sh_package_compilation_changes(NULL, NULL, NULL, NULL, 0));

    /* A map may add hundreds of resources. Result paths are owned, including
     * after the source snapshot's names and allocations have been retired. */
    sh_compiled_resource *many = calloc(257, sizeof(*many));
    CHECK(many);
    if (many) {
        before.resources = many; before.resource_count = 257;
        for (size_t i = 0; i < 257; i++) {
            char name[32]; snprintf(name, sizeof(name), "resource/%04zu", i);
            many[i].engine_path = _strdup(name); many[i].body = bytes_a; many[i].body_length = sizeof(bytes_a);
        }
        CHECK(sh_package_compilation_changes(NULL, &before, &changes, error, sizeof(error)) && changes.count == 257);
        for (size_t i = 0; i < 257; i++) free((void *)many[i].engine_path);
        free(many); expect_change(&changes, 256, "resource/0256", SH_PACKAGE_RESOURCE_ADDED);
    }
    sh_package_changes_free(&changes); sh_package_changes_free(&changes);
}

static int restoration_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    int mode = *(int *)context;
    int result = baseline(NULL, path, body, length);
    if (!strcmp(path, inspector)) {
        if (mode == -1) { free(*body); *body = NULL; *length = 0; return -1; }
        if (mode == 0 || mode == 2) return mode;
        if (mode == 3) { free(*body); *body = (unsigned char *)_strdup("{ broken"); *length = 8; }
    }
    return result;
}

/* Unchanged SnapMap originals stay native: the published provider drops them
 * and the links naming them. Campaign, restored, renamed, composed and rebound
 * resources remain. */
static void test_prune_native(void)
{
    static const char *const paths[] = {"generated/decls/entitydef/changed.decl",
        "generated/decls/material/composed.decl", "generated/decls/material/renamed.decl",
        "generated/decls/material/restored.decl", "generated/decls/material/stock.decl",
        "generated/decls/sound/bound.decl", "generated/image/campaign.bimage", "generated/image/stock.bimage"};
    sh_package_compilation *compiled = calloc(1, sizeof(*compiled));
    CHECK(compiled); if (!compiled) return;
    compiled->resources = calloc(8, sizeof(*compiled->resources)); compiled->resource_count = 8;
    CHECK(compiled->resources); if (!compiled->resources) { free(compiled); return; }
    for (size_t i = 0; i < 8; i++) {
        sh_compiled_resource *r = compiled->resources + i;
        r->engine_path = _strdup(paths[i]); r->owns_engine_path = 1; r->source = SIZE_MAX;
        r->native_original = 1; r->baseline_known = 1;
    }
    sh_compiled_resource *changed = compiled->resources;
    changed->native_original = 0;
    changed->bound_dependencies = calloc(3, sizeof(char *)); changed->bound_dependency_count = 3;
    changed->bound_dependencies[0] = _strdup("generated/decls/material/stock.decl");
    changed->bound_dependencies[1] = _strdup("generated/image/campaign.bimage");
    changed->bound_dependencies[2] = _strdup("generated/image/stock.bimage");
    compiled->resources[1].composed = 1;
    compiled->resources[2].original_path = _strdup("generated/decls/material/stock.decl");
    compiled->resources[3].restored_original = 1;
    compiled->resources[5].sound_authored_event = 7; compiled->resources[5].sound_event = 9;
    compiled->resources[6].native_original = 0; compiled->resources[6].baseline_known = 2;
    CHECK(sh_package_compilation_prune_native(compiled, NULL, NULL) == 2);
    CHECK(compiled->resource_count == 6);
    CHECK(!sh_package_compilation_find(compiled, "generated/decls/material/stock.decl"));
    CHECK(!sh_package_compilation_find(compiled, "generated/image/stock.bimage"));
    CHECK(sh_package_compilation_find(compiled, "generated/image/campaign.bimage"));
    CHECK(sh_package_compilation_find(compiled, "generated/decls/material/restored.decl"));
    CHECK(changed->bound_dependency_count == 1 && !strcmp(changed->bound_dependencies[0], "generated/image/campaign.bimage"));
    CHECK(sh_package_compilation_prune_native(compiled, NULL, NULL) == 0 && compiled->resource_count == 6);
    sh_package_compilation_free(compiled);
}

/* One shipped file can carry several native names. It stays native only when
 * SnapMap itself serves every name from that file: a kept stock image reloads
 * every consumer at activation, a pruned campaign-only name is unreachable. */
static int prune_served(void *context, const char *path, const char *type, const char *name)
{
    (void)context;
    if (strcmp(type, "image")) return 0;
    if (!strcmp(path, "generated/image/pair.bimage")) return !strcmp(name, "pair") || !strcmp(name, "pair.tga");
    return !strcmp(path, "generated/image/split.bimage") && !strcmp(name, "split");
}
static void test_prune_native_names(void)
{
    static const char *const paths[] = {"generated/image/pair.bimage", "generated/image/split.bimage"};
    static const char *const names[][2] = {{"pair", "pair.tga"}, {"split", "split.tga"}};
    for (int served = 0; served < 2; served++) {
        sh_package_compilation *compiled = calloc(1, sizeof(*compiled));
        CHECK(compiled); if (!compiled) return;
        compiled->resources = calloc(2, sizeof(*compiled->resources)); compiled->resource_count = 2;
        CHECK(compiled->resources); if (!compiled->resources) { free(compiled); return; }
        for (size_t i = 0; i < 2; i++) {
            sh_compiled_resource *r = compiled->resources + i;
            r->engine_path = _strdup(paths[i]); r->owns_engine_path = 1; r->source = SIZE_MAX;
            r->native_original = 1; r->baseline_known = 1;
            CHECK(sh_package_resource_native_add(r, "image", names[i][0]) &&
                  sh_package_resource_native_add(r, "image", names[i][1]));
        }
        CHECK(sh_package_compilation_prune_native(compiled, served ? prune_served : NULL, NULL) == (size_t)served);
        CHECK(!sh_package_compilation_find(compiled, paths[0]) == served);
        CHECK(sh_package_compilation_find(compiled, paths[1]) != NULL);
        sh_package_compilation_free(compiled);
    }
}

static void test_restoration(const sh_package_compilation *previous)
{
    sh_package_sources empty_sources = {0};
    sh_package_compilation *current = NULL, *again = NULL, *readded = NULL;
    const sh_compiled_resource *resource;
    char error[2048];
    int mode;
    current = sh_package_compile(&empty_sources, NULL, NULL, error, sizeof(error));
    CHECK(current);
    if (!current) return;
    /* A failure after an earlier original was staged publishes nothing. */
    for (mode = -1; mode <= 3; mode++) if (mode != 1) {
        CHECK(!sh_package_compilation_restore_missing(current, previous,
            restoration_baseline, &mode, error, sizeof(error)));
        CHECK(current->resource_count == 0);
        CHECK(strstr(error, "cannot restore verified SnapMap original"));
        CHECK(previous->resource_count == 5);
    }
    CHECK(!sh_package_compilation_restore_missing(current, previous, NULL, NULL, NULL, 0));
    CHECK(sh_package_compilation_restore_missing(current, previous, baseline, NULL, error, sizeof(error)));
    CHECK(current->resource_count == 3 && current->duplicate_count == 0 && current->composed_count == 0);
    resource = sh_package_compilation_find(current, editor);
    CHECK(resource && resource->restored_original && !sh_package_owners_count(&resource->owners) && !sh_package_owners_count(&resource->gameplay_owners));
    CHECK(resource && !strcmp((char *)resource->body, editor_base));
    resource = sh_package_compilation_find(current, inspector);
    CHECK(resource && resource->restored_original && resource->baseline_known == 1);
    CHECK(resource && !sh_package_owners_count(&resource->owners) && !sh_package_owners_count(&resource->gameplay_owners) && !resource->source_count);
    CHECK(resource && resource->source == SIZE_MAX && !strcmp((char *)resource->body, original));
    CHECK(!sh_package_compilation_find(current, "cooked/model/replaced.bmodel"));
    CHECK(!sh_package_compilation_find(current, "generated/image/shared.bimage"));
    CHECK(sh_package_compilation_restore_missing(current, previous, baseline, NULL, error, sizeof(error)));
    CHECK(current->resource_count == 3); /* Idempotent, no duplicate originals. */
    again = sh_package_compile(&empty_sources, NULL, NULL, error, sizeof(error));
    CHECK(again && sh_package_compilation_restore_missing(again, current, baseline, NULL, error, sizeof(error)));
    sh_package_compilation_free(current); current = NULL;
    resource = sh_package_compilation_find(again, inspector);
    if (resource) {
        size_t length;
        unsigned char *body = sh_package_compilation_read(again, resource, 1024, &length, error, sizeof(error));
        CHECK(body && length == strlen(original) && !strcmp((char *)body, original)); free(body);
    } else CHECK(0);
    readded = sh_package_compile(previous->sources, baseline, NULL, error, sizeof(error));
    CHECK(readded && sh_package_compilation_restore_missing(readded, again, baseline, NULL, error, sizeof(error)));
    sh_package_compilation_free(again);
    resource = sh_package_compilation_find(readded, inspector);
    CHECK(resource && !resource->restored_original && resource->owners.bits == 3 && resource->composed);
    sh_package_compilation_free(readded);
    /* Neither campaign imports nor new identities can be restored as stock. */
    {
        sh_compiled_resource imported[2] = {0};
        sh_package_compilation old = {0};
        imported[0].engine_path = inspector; imported[0].type = "material";
        imported[0].baseline_known = 2;
        imported[1].engine_path = "generated/decls/material/new.decl"; imported[1].type = "material";
        old.resources = imported; old.resource_count = 2;
        current = sh_package_compile(&empty_sources, NULL, NULL, error, sizeof(error));
        CHECK(current && sh_package_compilation_restore_missing(current, &old, NULL, NULL, error, sizeof(error)));
        CHECK(current && current->resource_count == 0);
        sh_package_compilation_free(current);
    }
}

static void test_builtin_composition(const sh_package_compilation *previous)
{
    const char *built_in = "{ edit = { propertySheets = { num = 1; item[0] = { properties = { "
        "num = 2; item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } "
        "item[1] = { path = \"builtinControl\"; inspector = \"boolinspector\"; } } } } } }";
    const char *conflict = "{ edit = { propertySheets = { num = 1; item[0] = { properties = { "
        "num = 2; item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } "
        "item[1] = { path = \"allowClimb\"; inspector = \"differentInspector\"; } } } } } }";
    const char *ordered = "{ edit = { validEncounters = { num = 3; item[0] = \"stock\"; "
        "item[1] = \"hell_guard\"; item[2] = \"cyber\"; } } }";
    sh_package_builtin builtins[] = {{inspector, (const unsigned char *)ordered, strlen(ordered)},
        {editor, (const unsigned char *)built_in, strlen(built_in)}};
    sh_package_compile_environment environment = {baseline, NULL, builtins, 2};
    sh_package_sources empty = {0};
    sh_package_compilation *compiled, *restored, *again, *refused;
    const sh_compiled_resource *resource;
    unsigned char *before;
    size_t length, count;
    char error[2048];
    int failure_mode = -1;
    compiled = sh_package_compile_with(previous->sources, &environment, error, sizeof(error));
    CHECK(compiled);
    if (!compiled) return;
    /* The original contributions and built-ins establish ordering together.
     * No intermediate output can invent author constraints. */
    resource = sh_package_compilation_find(compiled, inspector);
    CHECK(resource && strstr((char *)resource->body, "item[1] = \"hell_guard\";"));
    CHECK(resource && strstr((char *)resource->body, "item[2] = \"cyber\";") && resource->owners.bits == 3);
    resource = sh_package_compilation_find(compiled, editor);
    CHECK(resource && strstr((char *)resource->body, "num = 4;"));
    CHECK(resource && strstr((char *)resource->body, "allowClimb"));
    CHECK(resource && strstr((char *)resource->body, "receiveDecals"));
    CHECK(resource && strstr((char *)resource->body, "builtinControl"));
    CHECK(resource && resource->owners.bits == 3 && !sh_package_owners_count(&resource->gameplay_owners) && resource->source_count == 2);
    before = sh_package_compilation_read(compiled, resource, SIZE_MAX, &length, error, sizeof(error));
    CHECK(before); count = compiled->composed_count;
    again = sh_package_compile_with(previous->sources, &environment, error, sizeof(error));
    CHECK(again && again->composed_count == count);
    if (again) {
        const sh_compiled_resource *same = sh_package_compilation_find(again, editor);
        CHECK(same && same->body_length == length && !memcmp(before, same->body, length));
    }
    sh_package_compilation_free(again);
    builtins[1].body = (const unsigned char *)conflict; builtins[1].length = strlen(conflict);
    refused = sh_package_compile_with(previous->sources, &environment, error, sizeof(error));
    CHECK(!refused && strstr(error, "built-in default") && strstr(error, "properties"));
    CHECK(resource->body_length == length && !memcmp(before, resource->body, length));
    sh_package_compilation_free(refused);
    builtins[1].body = (const unsigned char *)built_in; builtins[1].length = strlen(built_in);
    environment.baseline = restoration_baseline; environment.baseline_context = &failure_mode;
    refused = sh_package_compile_with(previous->sources, &environment, error, sizeof(error));
    CHECK(!refused && strstr(error, "original is unreadable"));
    sh_package_compilation_free(refused);
    environment.baseline = baseline; environment.baseline_context = NULL;
    restored = sh_package_compile_with(&empty, &environment, error, sizeof(error));
    CHECK(restored && sh_package_compilation_restore_missing(restored, compiled, baseline, NULL, error, sizeof(error)));
    resource = sh_package_compilation_find(restored, editor);
    CHECK(resource && resource->restored_original && !sh_package_owners_count(&resource->owners) && !sh_package_owners_count(&resource->gameplay_owners) && !resource->source_count);
    CHECK(resource && strstr((char *)resource->body, "num = 2;") && strstr((char *)resource->body, "builtinControl"));
    CHECK(resource && !strstr((char *)resource->body, "allowClimb") && !strstr((char *)resource->body, "receiveDecals"));
    free(before); sh_package_compilation_free(restored); sh_package_compilation_free(compiled);
}
static void test_new_declarations(void)
{
    char directory[MAX_PATH], error[2048];
    const char *identity = "generated/decls/entitydef/new/shared.decl";
    sh_package_sources *sources;
    sh_package_compilation *compiled;
    const sh_compiled_resource *resource;
    snprintf(directory, sizeof(directory), "%s/new-definitions", root);
    create("new-definitions/overrides/a/package.json", "{\"id\":\"a\",\"name\":\"A\"}");
    create("new-definitions/overrides/b/package.json", "{\"id\":\"b\",\"name\":\"B\"}");
    create("new-definitions/overrides/a/assets/generated/decls/entitydef/new/shared.decl",
        "{ inherit = \"ai/base\"; class = \"idAI2\"; edit = { stats = { health = 100; } } }");
    create("new-definitions/overrides/b/assets/generated/decls/entitydef/new/shared.decl",
        "{ inherit = \"ai/base\"; class = \"idAI2\"; edit = { stats = { speed = 5; } } }");
    sources = sh_package_sources_scan(directory, error, sizeof(error)); CHECK(sources);
    if (!sources) return;
    compiled = sh_package_compile(sources, baseline, NULL, error, sizeof(error));
    CHECK(compiled);
    resource = sh_package_compilation_find(compiled, identity);
    CHECK(resource && resource->composed && !resource->baseline_known &&
        resource->owners.bits == 3 && resource->gameplay_owners.bits == 3 && resource->source_count == 2);
    CHECK(resource && strstr((char *)resource->body, "health = 100;") &&
        strstr((char *)resource->body, "speed = 5;"));
    if (resource) {
        const char *body = (const char *)resource->body;
        CHECK(strstr(body, "inherit =") && strstr(body, "class =") && strstr(body, "edit ="));
        CHECK(strstr(body, "inherit =") < strstr(body, "class ="));
        CHECK(strstr(body, "class =") < strstr(body, "edit ="));
    }
    sh_package_compilation_free(compiled);
    compiled = sh_package_compile(sources, NULL, NULL, error, sizeof(error));
    CHECK(!compiled && strstr(error, "verified original"));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    create("new-definitions/overrides/b/assets/generated/decls/entitydef/new/shared.decl",
        "{ class = \"idAI2\"; edit = { stats = { health = 200; } } }");
    sources = sh_package_sources_scan(directory, error, sizeof(error)); CHECK(sources);
    if (!sources) return;
    compiled = sh_package_compile(sources, baseline, NULL, error, sizeof(error));
    CHECK(!compiled && strstr(error, "edit.stats.health"));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
}

static int wiring_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    if (strcmp(path, "generated/decls/snapeditorentitydef/test/logic.decl")) return 0;
    *body = (unsigned char *)_strdup(context); *length = strlen(context);
    return *body ? 1 : -1;
}

static void test_editor_wiring_collections(void)
{
    const char *base = "{ edit = { inputs = { num = 2; item[0] = \"action/base\"; item[1] = \"action/remove\"; }"
        " outputs = { num = 1; item[0] = \"listener/base\"; } } }";
    const char *builtin = "{ edit = { inputs = { num = 4; item[0] = \"action/base\"; item[1] = \"action/remove\";"
        " item[2] = \"action/builtin\"; item[3] = \"action/shared\"; }"
        " outputs = { num = 2; item[0] = \"listener/base\"; item[1] = \"listener/builtin\"; } } }";
    const char *path = "generated/decls/snapeditorentitydef/test/logic.decl";
    sh_package_builtin contribution = {path, (const unsigned char *)builtin, strlen(builtin)};
    sh_package_compile_environment environment = {wiring_baseline, (void *)base, &contribution, 1};
    sh_package_sources *sources;
    sh_package_compilation *compiled;
    const sh_compiled_resource *resource;
    char directory[MAX_PATH], error[2048];
    snprintf(directory, sizeof(directory), "%s/wiring", root);
    create("wiring/overrides/a/package.json", "{\"id\":\"a\",\"name\":\"A\"}");
    create("wiring/overrides/b/package.json", "{\"id\":\"b\",\"name\":\"B\"}");
    create("wiring/overrides/a/assets/generated/decls/snapeditorentitydef/test/logic.decl",
        "{ edit = { inputs = { num = 3; item[0] = \"action/base\"; item[1] = \"action/a\"; item[2] = \"action/shared\"; }"
        " outputs = { num = 2; item[0] = \"listener/base\"; item[1] = \"listener/a\"; } } }");
    create("wiring/overrides/b/assets/generated/decls/snapeditorentitydef/test/logic.decl",
        "{ edit = { inputs = { num = 4; item[0] = \"action/base\"; item[1] = \"action/remove\";"
        " item[2] = \"action/b\"; item[3] = \"action/shared\"; }"
        " outputs = { num = 2; item[0] = \"listener/base\"; item[1] = \"listener/builtin\"; } } }");
    sources = sh_package_sources_scan(directory, error, sizeof(error)); CHECK(sources);
    if (!sources) return;
    compiled = sh_package_compile_with(sources, &environment, error, sizeof(error));
    CHECK(compiled); if (!compiled) fprintf(stderr, "%s\n", error);
    resource = sh_package_compilation_find(compiled, path);
    CHECK(resource && resource->composed && resource->owners.bits == 3 && !resource->gameplay_owners.bits);
    if (resource) {
        const char *body = (const char *)resource->body;
        CHECK(strstr(body, "num = 5;") && strstr(body, "num = 3;"));
        CHECK(strstr(body, "action/a") && strstr(body, "action/builtin") && strstr(body, "action/b\""));
        CHECK(strstr(body, "item[4] = \"action/shared\";") && !strstr(body, "action/remove"));
        CHECK(strstr(body, "listener/a") && strstr(body, "listener/builtin"));
    }
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
}

static int import_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)path;
    *body = (unsigned char *)_strdup("{ stageProgram uncooked }");
    *length = strlen((char *)*body);
    return *(int *)context;
}

static void test_native_original_provenance(void)
{
    char error[512];
    int scope;
    cleanup(); CHECK(CreateDirectoryA(root, NULL));
    create("overrides/native/package.json", "{\"id\":\"native\",\"name\":\"Native import\"}");
    create("overrides/native/assets/generated/decls/material/imported.decl", "{ stageProgram uncooked }");
    for (scope = 1; scope <= 3; scope++) {
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error));
        sh_package_compilation *compiled = sh_package_compile(sources, import_baseline, &scope, error, sizeof(error));
        CHECK(sources && compiled);
        const sh_compiled_resource *resource = sh_package_compilation_find(compiled, "generated/decls/material/imported.decl");
        CHECK(resource && resource->native_original == (scope != 3));
        sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    }
    create("overrides/native/assets/generated/decls/material/imported.decl", "{ stageProgram authored }");
    scope = 2;
    {
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error));
        sh_package_compilation *compiled = sh_package_compile(sources, import_baseline, &scope, error, sizeof(error));
        CHECK(sources && compiled);
        const sh_compiled_resource *resource = sh_package_compilation_find(compiled, "generated/decls/material/imported.decl");
        CHECK(resource && !resource->native_original);
        sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    }
}

static void test_duplicate_editor_controls(void)
{
    const char *path = "generated/decls/snapeditorentitydef/props/example.decl";
    const char *relative = "overrides/editor/assets/generated/decls/snapeditorentitydef/props/example.decl";
    const char *text = "{ edit = { propertySheets = { num = 2; "
        "item[0] = { properties = { num = 1; item[0] = { path = \"size\"; "
        "inspector = \"vec3\"; displayNameTag = \"custom\"; descriptionTag = \"custom help\"; } } } "
        "item[1] = { properties = { num = 2; item[0] = { path = \"size\"; "
        "inspector = \"vec3\"; displayNameTag = \"stock\"; descriptionTag = \"stock help\"; } "
        "item[1] = { path = \"color\"; inspector = \"vec3\"; } } } } } }";
    char error[1024];
    sh_package_sources *sources;
    sh_package_compilation *compiled;
    const sh_compiled_resource *resource;
    cleanup(); CHECK(CreateDirectoryA(root, NULL));
    create("overrides/editor/package.json", "{\"id\":\"editor\",\"name\":\"Editor\"}");
    create("overrides/healthy/package.json", "{\"id\":\"healthy\",\"name\":\"Healthy\"}");
    create("overrides/healthy/assets/generated/decls/entitydef/example.decl", "{ edit = { health = 10; } }");
    create(relative, text);
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    compiled = sh_package_compile(sources, NULL, NULL, error, sizeof(error)); CHECK(compiled);
    if (!compiled) fprintf(stderr, "editor control dedupe: %s\n", error);
    if (compiled) {
        CHECK(compiled->sources->package_count == 2 && compiled->resource_count == 2);
        resource = sh_package_compilation_find(compiled, path); CHECK(resource && resource->body);
        if (resource && resource->body) {
            sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)resource->body, resource->body_length}, error, sizeof(error));
            const sh_decl_node *sheets = sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "propertySheets");
            const sh_decl_node *first = sh_decl_tree_member(sh_decl_tree_member(sheets, "item[0]"), "properties");
            const sh_decl_node *second = sh_decl_tree_member(sh_decl_tree_member(sheets, "item[1]"), "properties");
            CHECK(!strcmp(sh_decl_tree_member(first, "num")->value, "1"));
            CHECK(!strcmp(sh_decl_tree_member(second, "num")->value, "1"));
            CHECK(strstr((char *)resource->body, "\"custom help\"") && !strstr((char *)resource->body, "\"stock help\""));
            CHECK(!strcmp(sh_decl_tree_member(sh_decl_tree_member(second, "item[0]"), "path")->value, "\"color\""));
            CHECK(!sh_package_owners_count(&resource->gameplay_owners));
            sh_decl_tree_free(tree);
        }
        /* Normalization is compiled output only. Intact package delivery must
         * retain the authored duplicate rows, not silently rewrite the file. */
        int found = 0;
        for (size_t i = 0; i < sources->file_count; i++) if (sources->files[i].engine_path &&
            !strcmp(sources->files[i].engine_path, path)) {
            size_t length = 0;
            unsigned char *body = sh_package_source_read(sources->files + i, SIZE_MAX, &length, error, sizeof(error));
            CHECK(body && length == strlen(text) && !memcmp(body, text, length)); free(body); found++;
        }
        CHECK(found == 1);
    }
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
}

/* Paths of one page (or scalar values of a flat list) joined by commas; the
 * page's first control label, when asked, follows a semicolon. */
static void rebased_layout(const char *text, const char *list, int page, char *out, size_t capacity)
{
    sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){text, strlen(text)}, NULL, 0);
    const sh_decl_node *items = tree ? sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), list) : NULL;
    char slot[32];
    *out = 0;
    if (items && page >= 0) {
        snprintf(slot, sizeof(slot), "item[%d]", page);
        items = sh_decl_tree_member(sh_decl_tree_member(items, slot), "properties");
    }
    for (int k = 0; items; k++) {
        const sh_decl_node *item, *value; const char *spelling; size_t length;
        snprintf(slot, sizeof(slot), "item[%d]", k);
        if (!(item = sh_decl_tree_member(items, slot))) break;
        value = page >= 0 ? sh_decl_tree_member(item, "path") : item;
        if (*out) strcat_s(out, capacity, ",");
        if (!sh_decl_tree_literal(value, &spelling, &length)) strcat_s(out, capacity, "?");
        else strncat_s(out, capacity, spelling, length);
        const sh_decl_node *label = page >= 0 ? sh_decl_tree_member(item, "descriptionTag") : NULL;
        if (label && sh_decl_tree_literal(label, &spelling, &length)) {
            strcat_s(out, capacity, "=");
            strncat_s(out, capacity, spelling, length);
        }
    }
    sh_decl_tree_free(tree);
}
#define SHEET(paths) "{ edit = { propertySheets = { num = 1; item[0] = { properties = { " paths " } } } } }"
static char *rebase_with(const char *child, const char *authored, const char *published, int force,
    sh_package_editor_slots *slots, int *result, int *kind)
{
    sh_decl_source a = {authored, strlen(authored)}, b = {published, strlen(published)};
    sh_package_editor_failure failure; char error[512], *out = NULL; size_t length = 0;
    *result = sh_package_editor_rebase("snapeditorentitydef", (sh_decl_source){child, strlen(child)},
        &a, 1, &b, 1, force, slots, &out, &length, &failure, error, sizeof(error));
    *kind = failure.kind;
    if (*result == -1) fprintf(stderr, "rebase: %s\n", error);
    return out;
}
static char *rebase_one(const char *child, const char *authored, const char *published, int force, int *result, int *kind)
{ return rebase_with(child, authored, published, force, NULL, result, kind); }
/* The identity a rebase gave the n-th control (from 0) of the first page, or
 * "" when it has none; stripped is the layout after dropping them under space. */
static void slot_identity(const char *text, int n, char *out, size_t capacity)
{
    char layout[1024], *at = layout, *end;
    *out = 0;
    rebased_layout(text, "propertySheets", 0, layout, sizeof(layout));
    for (int k = 0; k < n && at; k++) if ((at = strchr(at, ','))) at++;
    if (!at) return;
    if ((end = strchr(at, ','))) *end = 0;
    if ((end = strchr(at, '='))) *end = 0;
    snprintf(out, capacity, "%s", at);
}
static int stripped_layout(const char *text, const char *space, char *layout, size_t capacity)
{
    char *plain = NULL; size_t length = 0;
    int result = sh_package_editor_strip_slots("snapeditorentitydef", space, (sh_decl_source){text, strlen(text)},
        &plain, &length);
    if (result == 1) rebased_layout(plain, "propertySheets", 0, layout, capacity);
    free(plain); return result;
}
/* The property pages as the native reader leaves them, independent of the
 * compiler's own reader (B3 evidence: work/b3-classification checkpoint-1
 * F2-F6 and F10, and the list resize workers): texts are read in order (the
 * farthest parent first, the served child last), every statement of
 * propertySheets and of a page's properties in source order. A statement's
 * first count applies before its items: below 1 it only zeroes the count;
 * otherwise the list keeps its first min(old, new) elements and the rest start
 * empty. An item past the count is skipped; an item writes only the fields it
 * names, so every other field keeps the previous occupant's value. Scalar
 * fields only. */
#define NATIVE_SLOTS 16
#define NATIVE_FIELDS 12
#define NATIVE_PAGES 4
typedef struct native_field { char key[64], value[160]; } native_field;
typedef struct native_control { native_field fields[NATIVE_FIELDS]; int count; } native_control;
typedef struct native_page { native_control own, controls[NATIVE_SLOTS]; int count; } native_page;
typedef struct native_sheets { native_page pages[NATIVE_PAGES]; int count; } native_sheets;
static void native_write(native_control *control, const sh_decl_node *item, int page)
{
    for (const sh_decl_node *field = item->children; field; field = field->next) {
        const char *text; size_t length;
        int at = 0;
        if (field->compound || (page ? strcmp(field->key, "titleTag") && strcmp(field->key, "descriptionTag") :
            strcmp(field->key, "path") && strcmp(field->key, "displayNameTag") && strcmp(field->key, "descriptionTag") &&
            strcmp(field->key, "inspector"))) continue;
        while (at < control->count && strcmp(control->fields[at].key, field->key)) at++;
        if (at == control->count) {
            CHECK(control->count < NATIVE_FIELDS);
            if (control->count >= NATIVE_FIELDS) return;
            control->count++;
        }
        snprintf(control->fields[at].key, sizeof(control->fields[at].key), "%s", field->key);
        if (sh_decl_tree_literal(field, &text, &length))
            snprintf(control->fields[at].value, sizeof(control->fields[at].value), "%.*s", (int)length, text);
        else snprintf(control->fields[at].value, sizeof(control->fields[at].value), "%s", field->value ? field->value : "");
    }
}
/* A statement's first count; 0 when it gives none. */
static int native_count(const sh_decl_node *list, int *count)
{
    for (const sh_decl_node *child = list->children; child; child = child->next)
        if (!strcmp(child->key, "num")) { *count = atoi(child->value ? child->value : "0"); return 1; }
    return 0;
}
/* A list element's header is [type [ops]] name[n] =, with any name: the array
 * reader (Vulkan FUN_141a202d0 over FUN_141a24c70 / FUN_141a25b90, OpenGL
 * FUN_141a17030 / FUN_141a17f50) reads the numeric index and only reports a
 * differing name, type or ops as a warning, then reads the element at n. The
 * decl codec keeps the type and ops tokens apart as an annotation. */
static int native_index(const char *key, int *index)
{
    const char *open = strchr(key, '[');
    const char *number, *close = strrchr(key, ']');
    uint32_t bits = 0; unsigned base = 10;
    if (!open || open == key) return 0;
    for (const char *c = key; c < open; c++) if (!isalnum((unsigned char)*c) && *c != '_') return 0;
    if (!close || close[1]) return 0;
    number = open + 1;
    if (number[0] == '0' && (number[1] == 'x' || number[1] == 'X')) { base = 16; number += 2; }
    else if (number[0] == '0' && (number[1] == 'b' || number[1] == 'B')) { base = 2; number += 2; }
    else if (memchr(number,'.',close-number) || memchr(number,'e',close-number) || memchr(number,'E',close-number)) {
        char *end; float value = (float)strtod(number,&end);
        if (end != close || value < 0 || value >= 2147483648.0f) return 0;
        *index = (int)value; return 1;
    } else if (number[0] == '0') base = 8;
    if (number == close) return 0;
    for (; number < close; number++) {
        unsigned c = (unsigned char)*number;
        unsigned digit = c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : 16;
        if (digit >= base) return 0;
        bits = bits * base + digit;
    }
    *index = (int32_t)bits; return 1;
}
static void native_read(native_sheets *sheets, const char *text)
{
    sh_decl_node *tree = sh_decl_tree_parse_ordered((sh_decl_source){text, strlen(text)}, NULL, 0);
    CHECK(tree);
    for (const sh_decl_node *edit = tree ? tree->children : NULL; edit; edit = edit->next) {
        if (strcmp(edit->key, "edit") || !edit->compound) continue;
        for (const sh_decl_node *sheet = edit->children; sheet; sheet = sheet->next) {
            int count, t;
            if (strcmp(sheet->key, "propertySheets") || !sheet->compound) continue;
            if (native_count(sheet, &count)) {
                if (count < 1) sheets->count = 0;
                else {
                    CHECK(count <= NATIVE_PAGES);
                    for (int fresh = sheets->count; fresh < count && fresh < NATIVE_PAGES; fresh++)
                        memset(sheets->pages + fresh, 0, sizeof(sheets->pages[fresh]));
                    sheets->count = count < NATIVE_PAGES ? count : NATIVE_PAGES;
                }
            }
            for (const sh_decl_node *item = sheet->children; item; item = item->next) {
                native_page *page;
                if (!native_index(item->key, &t) || t < 0 || t >= sheets->count || !item->compound) continue;
                page = sheets->pages + t;
                native_write(&page->own, item, 1);
                for (const sh_decl_node *list = item->children; list; list = list->next) {
                    int slots, j;
                    if (strcmp(list->key, "properties") || !list->compound) continue;
                    if (native_count(list, &slots)) {
                        if (slots < 1) page->count = 0;
                        else {
                            CHECK(slots <= NATIVE_SLOTS);
                            for (int fresh = page->count; fresh < slots && fresh < NATIVE_SLOTS; fresh++)
                                memset(page->controls + fresh, 0, sizeof(page->controls[fresh]));
                            page->count = slots < NATIVE_SLOTS ? slots : NATIVE_SLOTS;
                        }
                    }
                    for (const sh_decl_node *control = list->children; control; control = control->next)
                        if (native_index(control->key, &j) && j >= 0 && j < page->count && control->compound)
                            native_write(page->controls + j, control, 0);
                }
            }
        }
    }
    sh_decl_tree_free(tree);
}
static int native_field_order(const void *a, const void *b)
{ return strcmp(((const native_field *)a)->key, ((const native_field *)b)->key); }
static void native_format(native_control *control, char *out, size_t capacity)
{
    qsort(control->fields, control->count, sizeof(control->fields[0]), native_field_order);
    int emitted = 0;
    for (int f = 0; f < control->count; f++) {
        /* Empty path/tag IDs and a NULL inspector are constructor state. */
        if (!control->fields[f].value[0] || (!strcmp(control->fields[f].key, "inspector") &&
            !strcmp(control->fields[f].value, "NULL"))) continue;
        strcat_s(out, capacity, emitted++ ? "," : "");
        strcat_s(out, capacity, control->fields[f].key); strcat_s(out, capacity, "=");
        strcat_s(out, capacity, control->fields[f].value);
    }
}
/* Page `page` after reading texts in order: its own fields, then '|', then
 * each control's fields sorted by key ("key=value" joined by ','), controls
 * joined by ';'. "-" when the page does not exist. */
static void native_page_text(const char *const *texts, size_t count, int page, char *out, size_t capacity)
{
    native_sheets *sheets = calloc(1, sizeof(*sheets));
    out[0] = 0;
    CHECK(sheets); if (!sheets) return;
    for (size_t i = 0; i < count; i++) native_read(sheets, texts[i]);
    if (page >= sheets->count) { strcat_s(out, capacity, "-"); free(sheets); return; }
    native_format(&sheets->pages[page].own, out, capacity);
    strcat_s(out, capacity, "|");
    for (int j = 0; j < sheets->pages[page].count; j++) {
        if (j) strcat_s(out, capacity, ";");
        native_format(sheets->pages[page].controls + j, out, capacity);
    }
    free(sheets);
}
/* State within an admitted owning-storage pass: parent copying precedes a
 * child that states inherit. Parent omission itself never proves native storage
 * fresh; the separate lifetime adapter supplies that guarantee. */
static void served_page_text(const char *parent, const char *served, int page, char *out, size_t capacity)
{
    const char *texts[2] = {parent, served};
    if (strstr(served, "inherit")) native_page_text(texts, 2, page, out, capacity);
    else native_page_text(texts + 1, 1, page, out, capacity);
}
/* A flat scalar list (inputs, outputs) after reading texts in order, values
 * joined by ','. A statement's first count applies before its items; below 1
 * it only zeroes the count. */
static void native_flat_text(const char *const *texts, size_t count, const char *list, char *out, size_t capacity)
{
    char values[32][128] = {{0}};
    int n = 0;
    out[0] = 0;
    for (size_t i = 0; i < count; i++) {
        sh_decl_node *tree = sh_decl_tree_parse_ordered((sh_decl_source){texts[i], strlen(texts[i])}, NULL, 0);
        CHECK(tree);
        for (const sh_decl_node *edit = tree ? tree->children : NULL; edit; edit = edit->next) {
            if (strcmp(edit->key, "edit") || !edit->compound) continue;
            for (const sh_decl_node *statement = edit->children; statement; statement = statement->next) {
                int total, k;
                if (strcmp(statement->key, list) || !statement->compound) continue;
                if (native_count(statement, &total)) {
                    if (total < 1) n = 0;
                    else {
                        for (int fresh = n; fresh < total && fresh < 32; fresh++) values[fresh][0] = 0;
                        n = total < 32 ? total : 32;
                    }
                }
                for (const sh_decl_node *item = statement->children; item; item = item->next) {
                    const char *text; size_t length;
                    if (native_index(item->key, &k) && k >= 0 && k < n && !item->compound &&
                        sh_decl_tree_literal(item, &text, &length))
                        snprintf(values[k], sizeof(values[k]), "%.*s", (int)length, text);
                }
            }
        }
        sh_decl_tree_free(tree);
    }
    for (int k = 0; k < n; k++) {
        if (k) strcat_s(out, capacity, ",");
        strcat_s(out, capacity, values[k]);
    }
}
static int restate(const char *text, const char *published, char **out)
{
    sh_decl_source parent = {published, strlen(published)};
    size_t length = 0;
    *out = NULL;
    return sh_package_editor_native_restatement("snapeditorentitydef", (sh_decl_source){text, strlen(text)}, &parent, 1,
        out, &length);
}
/* A row edit re-keying an inherited control (stock volume/spawn_inhibit_cylinder
 * binds its parent's size control to renderModelInfo.scale with a sparse patch
 * of slot 1) while unrelated packages append controls without a path to the
 * parent and edit that control's label (the live b3-1-vk Vulkan inventory):
 * the edited control keeps its provenance to the published slot, the
 * additions stay after it, and the served child, read natively over the
 * served parent, is exactly what the child's own sparse text reads there, in
 * either order of the additions, with no field of a previous occupant left
 * under any slot. A trim the child states stays, the parent's later addition
 * survives it, and where a restated control would keep the dropped control's
 * fields the complete row writes the native constructor defaults. */
static void test_editor_native_restatement(void)
{
    char want[1024], got[1024], plain[1024];
    int result, kind;
    char *out, *exact;
    const char *box = "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
        "item[0] = { path = \"displayName\"; displayNameTag = \"#name\"; descriptionTag = \"#name_desc\"; } "
        "item[1] = { path = \"clipModelInfo.size\"; displayNameTag = \"#size\"; descriptionTag = \"#size_desc\"; "
        "inspector = \"vec3\"; } } } } } }";
    const char *composed[2] = {
        "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 4; "
        "item[0] = { path = \"displayName\"; displayNameTag = \"#name\"; descriptionTag = \"#name_desc\"; } "
        "item[1] = { path = \"clipModelInfo.size\"; displayNameTag = \"#size\"; descriptionTag = \"#name_desc\"; "
        "inspector = \"vec3\"; } item[2] = { displayNameTag = \"#size\"; } item[3] = { descriptionTag = \"#size_desc\"; } "
        "} } } } }",
        "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 4; "
        "item[0] = { path = \"displayName\"; displayNameTag = \"#name\"; descriptionTag = \"#name_desc\"; } "
        "item[1] = { path = \"clipModelInfo.size\"; displayNameTag = \"#size\"; descriptionTag = \"#name_desc\"; "
        "inspector = \"vec3\"; } item[2] = { descriptionTag = \"#size_desc\"; } item[3] = { displayNameTag = \"#size\"; } "
        "} } } } }"};
    const char *expected[2] = {
        "|descriptionTag=#name_desc,displayNameTag=#name,path=displayName;"
        "descriptionTag=#name_desc,displayNameTag=#size,inspector=vec3,path=renderModelInfo.scale;"
        "displayNameTag=#size;descriptionTag=#size_desc",
        "|descriptionTag=#name_desc,displayNameTag=#name,path=displayName;"
        "descriptionTag=#name_desc,displayNameTag=#size,inspector=vec3,path=renderModelInfo.scale;"
        "descriptionTag=#size_desc;displayNameTag=#size"};
    const char *cylinder = "{ inherit = \"volume/spawn_inhibit_box\"; edit = { propertySheets = { item[0] = { "
        "properties = { item[1] = { path = \"renderModelInfo.scale\"; } } } } } }";
    for (int order = 0; order < 2; order++) {
        const char *stock_read[2] = {composed[order], cylinder};
        sh_package_editor_slots slots = {"#t-0:", {NULL, 0}};
        /* The child's own sparse text, read over the composed parent. */
        native_page_text(stock_read, 2, 0, want, sizeof(want));
        if (strcmp(want, expected[order])) fprintf(stderr, "native cylinder %d: %s\n", order, want);
        CHECK(!strcmp(want, expected[order]));
        /* The rebase (the parent's layout grew) reads exactly the same. */
        out = rebase_one(cylinder, box, composed[order], 0, &result, &kind);
        CHECK(result == 1 && out);
        if (out) {
            const char *served[2] = {composed[order], out};
            native_page_text(served, 2, 0, got, sizeof(got));
            if (strcmp(got, want)) fprintf(stderr, "rebased cylinder %d: %s\n%s\n", order, got, out);
            CHECK(!strcmp(got, want));
            CHECK(restate(out, composed[order], &exact) >= 0);
            free(exact);
        }
        free(out);
        /* With identities for composition, then dropped: the same. */
        out = rebase_with(cylinder, box, composed[order], 0, &slots, &result, &kind);
        CHECK(result == 1 && out);
        if (out) {
            char *stripped = NULL; size_t length = 0;
            CHECK(sh_package_editor_strip_slots("snapeditorentitydef", "#t-0:", (sh_decl_source){out, strlen(out)},
                &stripped, &length) >= 0);
            const char *served[2] = {composed[order], stripped ? stripped : out};
            native_page_text(served, 2, 0, got, sizeof(got));
            if (strcmp(got, want)) fprintf(stderr, "rebased cylinder slots %d: %s\n", order, got);
            CHECK(!strcmp(got, want));
            free(stripped);
        }
        free(out);
    }
    /* A trim over a parent with a later addition: the child keeps a and b, the
     * addition z survives, and z must not keep c's description. The rebased
     * list alone would leave it there; the restatement clears the missing
     * constructor fields explicitly while retaining the peer's row. */
    {
        const char *stock = SHEET("num = 3; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } "
            "item[2] = { path = \"c\"; descriptionTag = \"#c\"; }");
        const char *published = SHEET("num = 4; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } "
            "item[2] = { path = \"c\"; descriptionTag = \"#c\"; } item[3] = { path = \"z\"; }");
        const char *trim = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 2; } } } } }";
        out = rebase_one(trim, stock, published, 0, &result, &kind);
        CHECK(result == 1 && out);
        if (out) {
            const char *raw[2] = {published, out};
            native_page_text(raw, 2, 0, got, sizeof(got));
            CHECK(!strcmp(got, "|path=a;path=b;descriptionTag=#c,path=z"));
            CHECK(restate(out, published, &exact) == 1 && exact);
            if (exact) {
                const char *served[2] = {published, exact};
                native_page_text(served, 2, 0, got, sizeof(got));
                if (strcmp(got, "|path=a;path=b;path=z")) fprintf(stderr, "restated trim: %s\n%s\n", got, exact);
                CHECK(!strcmp(got, "|path=a;path=b;path=z"));
                CHECK(strstr(exact, "descriptionTag = \"\";") && !strstr(exact, "} properties = {"));
            }
            free(exact);
        }
        free(out);
        out = rebase_one(trim, SHEET("num = 3; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } item[2] = { path = \"c\"; }"),
            SHEET("num = 4; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } item[2] = { path = \"c\"; } item[3] = { path = \"z\"; }"),
            0, &result, &kind);
        CHECK(result == 1 && out);
        if (out) {
            const char *published_plain = SHEET("num = 4; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } "
                "item[2] = { path = \"c\"; } item[3] = { path = \"z\"; }");
            const char *served[2] = {published_plain, out};
            native_page_text(served, 2, 0, got, sizeof(got));
            CHECK(!strcmp(got, "|path=a;path=b;path=z"));
            CHECK(restate(out, published_plain, &exact) >= 0);
            free(exact);
        }
        free(out);
        /* A list the child empties (a count below 1, negative included) does
         * not erase a peer's addition: the composed child keeps z alone, read
         * with none of the dropped control's fields. That needs slot 0 read
         * fresh (B3 first-slot case): it is served exactly, never stale and
         * never refused, in whichever form the correction chooses. */
        for (int negative = 0; negative < 2; negative++) {
            const char *published_zero = SHEET("num = 2; item[0] = { path = \"c\"; descriptionTag = \"#c\"; } item[1] = { path = \"z\"; }");
            out = rebase_one(negative ?
                "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = -1; } } } } }" :
                "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 0; } } } } }",
                SHEET("num = 1; item[0] = { path = \"c\"; descriptionTag = \"#c\"; }"), published_zero, 0, &result, &kind);
            CHECK(result == 1 && out);
            if (out) {
                char stated[256];
                const char *alone[1] = {out};
                native_page_text(alone, 1, 0, stated, sizeof(stated));
                CHECK(!strcmp(stated, "|path=z"));
                CHECK(restate(out, published_zero, &exact) == 1 && exact);
                if (exact) {
                    served_page_text(published_zero, exact, 0, stated, sizeof(stated));
                    if (strcmp(stated, "|path=z")) fprintf(stderr, "first slot (negative %d): %s\n%s\n", negative, stated, exact);
                    CHECK(!strcmp(stated, "|path=z"));
                }
                free(exact);
            }
            free(out);
        }
        /* A parent relaid so that the control the child drops comes first: the
         * kept controls need slot 0 read fresh (B3 first-slot case), served
         * exactly rather than refused or respelled with stale fields. */
        out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 2; } } } } }",
            SHEET("num = 3; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } item[2] = { path = \"c\"; descriptionTag = \"#c\"; }"),
            SHEET("num = 3; item[0] = { path = \"c\"; descriptionTag = \"#c\"; } item[1] = { path = \"a\"; } item[2] = { path = \"b\"; }"),
            0, &result, &kind);
        CHECK(result == 1 && out);
        if (out) {
            const char *relaid = SHEET("num = 3; item[0] = { path = \"c\"; descriptionTag = \"#c\"; } item[1] = { path = \"a\"; } "
                "item[2] = { path = \"b\"; }");
            CHECK(restate(out, relaid, &exact) == 1 && exact);
            if (exact) {
                served_page_text(relaid, exact, 0, got, sizeof(got));
                if (strcmp(got, "|path=a;path=b")) fprintf(stderr, "relaid first slot: %s\n%s\n", got, exact);
                CHECK(!strcmp(got, "|path=a;path=b"));
            }
            free(exact);
        }
        free(out);
        /* Missing assignment is malformed; warning-only annotations and the
         * normal inherited reader's consumed reset marker do not alter state. */
        {
            const char *published_h = SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; }");
            const char *operation = "{ inherit = \"p\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
                "item[0] = { path = \"a\"; } item[1] { path = \"b\"; } } } } } }";
            const char *annotated = "{ inherit = \"p\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
                "item[0] = { path = \"a\"; } item[1] = { idStr path = \"b\"; } } } } } }";
            const char *marked = "{ inherit = \"p\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
                "item[0] = { path = \"a\"; } item[1] = ! { path = \"b\"; } } } } } }";
            CHECK(restate(operation, published_h, &exact) == -3 && !exact);
            free(exact);
            CHECK(restate(annotated, published_h, &exact) == 1 && exact && !strstr(exact, "idStr path"));
            free(exact);
            CHECK(restate(marked, published_h, &exact) >= 0);
            served_page_text(published_h, exact ? exact : marked, 0, got, sizeof(got));
            if (strcmp(got, "|path=a;path=b")) fprintf(stderr, "marked restatement: %s\n", got);
            CHECK(!strcmp(got, "|path=a;path=b"));
            free(exact);
            /* The same header as the occupant: nothing to cut. */
            CHECK(restate("{ inherit = \"p\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
                "item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } } } } } }", published_h, &exact) >= 0);
            free(exact);
            /* A restatement stating a member twice is not one this proof
             * covers: refused, never passed as already exact. */
            CHECK(restate("{ inherit = \"p\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
                "item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } } } } propertySheets = { num = 1; } } }", published_h,
                &exact) == 1 && exact);
            free(exact);
            /* A parent this model does not read is no proof either. */
            CHECK(restate("{ inherit = \"p\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 1; "
                "item[0] = { path = \"a\"; } } } } } }", "{ edit = { propertySheets = { num = broken; } } }", &exact) == -3 && !exact);
            free(exact);
        }
    }
    /* A page's omitted title must clear the previous occupant's native title. */
    {
        const char *published = "{ edit = { propertySheets = { num = 2; item[0] = { properties = { num = 1; "
            "item[0] = { path = \"a\"; } } } item[1] = { titleTag = \"#B\"; properties = { num = 1; item[0] = { path = \"b\"; } } } "
            "} } }";
        const char *text = "{ inherit = \"p\"; edit = { propertySheets = { num = 2; item[0] = { properties = { num = 1; "
            "item[0] = { path = \"a\"; } } } item[1] = { properties = { num = 1; item[0] = { path = \"z\"; } } } } } }";
        const char *raw[2] = {published, text};
        native_page_text(raw, 2, 1, plain, sizeof(plain));
        CHECK(!strcmp(plain, "titleTag=#B|path=z"));
        CHECK(restate(text, published, &exact) == 1 && exact);
        if (exact) {
            const char *served[2] = {published, exact};
            native_page_text(served, 2, 1, got, sizeof(got));
            CHECK(!strcmp(got, "|path=z"));
            native_page_text(served, 2, 0, got, sizeof(got));
            CHECK(!strcmp(got, "|path=a"));
            CHECK(strstr(exact, "titleTag = \"\";") && !strstr(exact, "} propertySheets = {"));
        }
        free(exact);
        /* Sparse statements remain author patches. Complete desired rows receive
         * defaults even without a parent: old native storage may still exist. */
        CHECK(restate("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { item[0] = { path = \"z\"; } } } } } }",
            published, &exact) == 0 && !exact);
        free(exact);
        {
            size_t length = 0;
            CHECK(sh_package_editor_native_restatement("snapeditorentitydef", (sh_decl_source){text, strlen(text)}, NULL, 0,
                &exact, &length) == 1 && exact); free(exact);
        }
    }
}

static int native_form(const char *text, const char *parent, char **out)
{
    sh_decl_source parents[1] = {{parent, parent ? strlen(parent) : 0}};
    size_t length = 0;
    return sh_package_editor_native_form((sh_decl_source){text, strlen(text)}, parent ? parents : NULL, parent ? 1 : 0, out,
        &length);
}
/* A declaration stating a member more than once (the native reader reads it
 * again, in order) has a native form: one statement per member writing what
 * its statements write, with the values the native reader leaves, which reads
 * the same over the parents. A slot dropped by one count and grown again by a
 * later one below the inherited count is explicitly cleared to constructor
 * state; a declaration without repeats or aliases is left as written (0). */
static void test_editor_native_form(void)
{
    char want[1024], got[1024];
    char *out = NULL;
    const char *parent = SHEET("num = 2; item[0] = { path = \"a\"; descriptionTag = \"#a\"; } item[1] = { path = \"b\"; }");
    /* Two edit blocks and two list statements (the second patching a slot
     * the first patched, the later label winning), plus a repeated scalar. */
    const char *twice = "{ inherit = \"p\"; edit = { displayNameTag = \"#first\"; propertySheets = { item[0] = { properties = { "
        "item[1] = { descriptionTag = \"#b1\"; } } } } } edit = { displayNameTag = \"#second\"; propertySheets = { item[0] = { "
        "properties = { num = 3; item[1] = { descriptionTag = \"#b2\"; } item[2] = { path = \"c\"; } } } } } }";
    const char *read_twice[2] = {parent, twice};
    native_page_text(read_twice, 2, 0, want, sizeof(want));
    CHECK(!strcmp(want, "|descriptionTag=#a,path=a;descriptionTag=#b2,path=b;path=c"));
    CHECK(native_form(twice, parent, &out) == 1 && out);
    if (out) {
        const char *read_form[2] = {parent, out};
        sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){out, strlen(out)}, NULL, 0);
        native_page_text(read_form, 2, 0, got, sizeof(got));
        if (strcmp(got, want)) fprintf(stderr, "native form: %s\n%s\n", got, out);
        CHECK(!strcmp(got, want));
        /* One statement per member: the strict codec reads it. */
        CHECK(tree && strstr(out, "#second") && !strstr(out, "#first"));
        sh_decl_tree_free(tree);
    }
    free(out); out = NULL;
    /* Dropped then regrown below the inherited count: slot 1's constructor
     * state is represented by explicit defaults over the previous occupant. */
    CHECK(native_form("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 1; } } } } "
        "edit = { propertySheets = { item[0] = { properties = { num = 2; } } } } }", parent, &out) == 1 && out);
    if (out) { const char *texts[2] = {parent,out}; native_page_text(texts,2,0,got,sizeof(got)); CHECK(!strcmp(got,"|descriptionTag=#a,path=a;")); }
    free(out); out = NULL;
    /* Regrown past the inherited count only: fresh slots are new natively too. */
    CHECK(native_form("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 3; } } } } "
        "edit = { propertySheets = { item[0] = { properties = { item[2] = { path = \"z\"; } } } } } }", parent, &out) == 1 && out);
    if (out) {
        const char *read_form[2] = {parent, out};
        native_page_text(read_form, 2, 0, got, sizeof(got));
        CHECK(!strcmp(got, "|descriptionTag=#a,path=a;path=b;path=z"));
    }
    free(out); out = NULL;
    /* Nothing repeated: left as written. */
    CHECK(native_form("{ inherit = \"p\"; edit = { displayNameTag = \"#x\"; } }", parent, &out) == 0 && !out);
    free(out); out = NULL;
    /* A field changing between a value and a block over its parent is the
     * text's own conflict. */
    CHECK(native_form("{ inherit = \"p\"; edit = { propertySheets = 1; } edit = { displayNameTag = \"#x\"; } }", parent, &out) == -2 &&
        !out);
    free(out); out = NULL;
    /* Repeated statements of one member with differing headers have no single
     * statement this model proves. */
    CHECK(native_form("{ inherit = \"p\"; edit = { displayNameTag = \"#x\"; } edit { displayNameTag = \"#y\"; } }", parent, &out) == -3 &&
        !out);
    free(out);
}

/* Growth is bounded by the declaration frame a statement addresses. With a
 * count, the native reading (B3 F3-F5): an item past it is skipped, so a
 * single count-0 statement naming item[0] stays empty. Without one, the slots
 * of the shipped declaration a package restates (its own controls past its
 * parents' included) or, with none, the parents' slots; an index past that
 * frame is skipped, never a control made up because it was named. */
static void test_editor_frame_bound(void)
{
    char got[1024];
    int result, kind;
    char *out;
    const char *parent = SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; }");
    /* A new child over a two-control parent naming item[9] without a count. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { item[9] = { path = \"x\"; } } } } } }",
        parent, parent, 1, &result, &kind);
    CHECK(result == 1 && out);
    if (out) {
        const char *served[2] = {parent, out};
        native_page_text(served, 2, 0, got, sizeof(got));
        CHECK(!strcmp(got, "|path=a;path=b") && !strstr(out, "\"x\""));
    }
    free(out);
    /* One statement with a count of 0 and item[0]: the item is past the count. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 0; "
        "item[0] = { path = \"x\"; } } } } } }", parent, parent, 1, &result, &kind);
    CHECK(result == 1 && out);
    if (out) {
        const char *served[2] = {parent, out};
        native_page_text(served, 2, 0, got, sizeof(got));
        CHECK(!strcmp(got, "|") && !strstr(out, "\"x\""));
    }
    free(out);
    /* A page index past the pages, without a count, makes no page either. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[3] = { properties = { num = 1; "
        "item[0] = { path = \"x\"; } } } } } }", parent, parent, 1, &result, &kind);
    CHECK(result == 1 && out && !strstr(out, "\"x\"") && strstr(out, "propertySheets = { num = 1;"));
    free(out);
    /* A package restating a shipped child that adds two controls without a
     * path: patching the shipped item[3] (no count) keeps both stock controls
     * and lands on the second; an index past the shipped frame is skipped. */
    {
        const char *shipped = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 4; "
            "item[2] = { inspector = \"float\"; } item[3] = { inspector = \"float\"; } } } } } }";
        sh_package_editor_slots slots = {"#t-0:", {shipped, strlen(shipped)}, 0};
        out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[3] = { descriptionTag = \"#str_second\"; } } } } } }", parent, parent, 1, &slots, &result, &kind);
        CHECK(result == 1 && out);
        if (out) {
            char *plain = NULL; size_t length = 0;
            CHECK(sh_package_editor_strip_slots("snapeditorentitydef", "#t-0:", (sh_decl_source){out, strlen(out)}, &plain,
                &length) == 1 && plain);
            if (plain) {
                const char *served[2] = {parent, plain};
                native_page_text(served, 2, 0, got, sizeof(got));
                if (strcmp(got, "|path=a;path=b;inspector=float;descriptionTag=#str_second,inspector=float"))
                    fprintf(stderr, "shipped frame: %s\n%s\n", got, plain);
                CHECK(!strcmp(got, "|path=a;path=b;inspector=float;descriptionTag=#str_second,inspector=float"));
            }
            free(plain);
        }
        free(out);
        slots.appends = 0;
        out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[9] = { descriptionTag = \"#str_far\"; } } } } } }", parent, parent, 1, &slots, &result, &kind);
        CHECK(result == 1 && out && !strstr(out, "#str_far"));
        free(out);
    }
}

/* ---- B3 semantics (package-architecture-v2, work/b3-semantics-20261001) ----
 * Gap 1: a first slot that must be read fresh. The stock child
 * weapons/no_weapon states a zero count for its inherited property sheets,
 * inputs and outputs; a peer's addition to the parent must survive it, alone,
 * with none of the previous occupant's fields. Natively only slot 0 itself is
 * out of reach of a positive count: the list hook keeps min(old, new) elements
 * of a list resized by a count of at least 1, so slot 0 is never regrown by
 * one (Vulkan FUN_1409cd610, FUN_141421750). The tests below read the served
 * child with the independent native model (no field of an old occupant, no
 * refusal) and do not depend on how a correction spells it; the encoding
 * test further down does. */
static void test_editor_first_slot(void)
{
    char got[1024], expected[1024];
    char *out = NULL, *exact = NULL;
    int result, kind;
    /* The stock shape: a weapon-like parent with one page of controls that
     * each carry a path, labels and an inspector, two inputs and an output;
     * its child states a zero count for sheets, outputs and inputs; a peer
     * package adds a label-only control and an input to the parent. */
    const char *weapon = "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 3; "
        "item[0] = { path = \"filter\"; displayNameTag = \"#f\"; descriptionTag = \"#f_desc\"; inspector = \"picker\"; } "
        "item[1] = { path = \"respawnDelay\"; displayNameTag = \"#r\"; descriptionTag = \"#r_desc\"; inspector = \"time\"; } "
        "item[2] = { path = \"enableThinkComponent\"; displayNameTag = \"#e\"; descriptionTag = \"#e_desc\"; inspector = \"bob\"; } "
        "} } } outputs = { num = 1; item[0] = \"listener/on_used\"; } "
        "inputs = { num = 2; item[0] = \"action/respawn\"; item[1] = \"action/remove\"; } } }";
    const char *published = "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 4; "
        "item[0] = { path = \"filter\"; displayNameTag = \"#f\"; descriptionTag = \"#f_desc\"; inspector = \"picker\"; } "
        "item[1] = { path = \"respawnDelay\"; displayNameTag = \"#r\"; descriptionTag = \"#r_desc\"; inspector = \"time\"; } "
        "item[2] = { path = \"enableThinkComponent\"; displayNameTag = \"#e\"; descriptionTag = \"#e_desc\"; inspector = \"bob\"; } "
        "item[3] = { displayNameTag = \"#peer\"; } } } } outputs = { num = 1; item[0] = \"listener/on_used\"; } "
        "inputs = { num = 3; item[0] = \"action/respawn\"; item[1] = \"action/remove\"; item[2] = \"action/peer\"; } } }";
    const char *no_weapon = "{ inherit = \"p\"; edit = { propertySheets = { num = 0; } outputs = { num = 0; } inputs = { num = 0; } } }";
    out = rebase_one(no_weapon, weapon, published, 0, &result, &kind);
    CHECK(result == 1 && out);
    if (out) {
        const char *text, *chain[2];
        size_t from;
        CHECK(restate(out, published, &exact) >= 0);
        text = exact ? exact : out;
        chain[0] = published; chain[1] = text;
        from = strstr(text, "inherit") ? 0 : 1;
        /* The peer's control alone, none of filter's path, labels or inspector. */
        served_page_text(published, text, 0, got, sizeof(got));
        if (strcmp(got, "|displayNameTag=#peer")) fprintf(stderr, "no_weapon sheets: %s\n%s\n", got, text);
        CHECK(!strcmp(got, "|displayNameTag=#peer"));
        native_flat_text(chain + from, 2 - from, "inputs", got, sizeof(got));
        CHECK(!strcmp(got, "action/peer"));
        native_flat_text(chain + from, 2 - from, "outputs", got, sizeof(got));
        CHECK(!strcmp(got, ""));
    }
    free(exact); exact = NULL; free(out); out = NULL;
    /* Without a peer the stock child is left exactly as written. */
    out = rebase_one(no_weapon, weapon, weapon, 0, &result, &kind);
    CHECK(result == 0 && !out);
    free(out); out = NULL;
    /* The first slot of a first page whose own fields would persist: the page
     * keeps its label natively unless it is read fresh. */
    {
        const char *labelled = "{ edit = { propertySheets = { num = 1; item[0] = { titleTag = \"#A\"; properties = { num = 1; "
            "item[0] = { path = \"a\"; } } } } } }";
        const char *text = "{ inherit = \"p\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 1; "
            "item[0] = { path = \"z\"; } } } } } }";
        const char *raw[2] = {labelled, text};
        native_page_text(raw, 2, 0, got, sizeof(got));
        CHECK(!strcmp(got, "titleTag=#A|path=z"));          /* what the text reads as written */
        CHECK(restate(text, labelled, &exact) == 1 && exact);
        if (exact) {
            served_page_text(labelled, exact, 0, got, sizeof(got));
            if (strcmp(got, "|path=z")) fprintf(stderr, "first page: %s\n%s\n", got, exact);
            CHECK(!strcmp(got, "|path=z"));
        }
        free(exact); exact = NULL;
    }
    /* The same first slot when a later page also needs a fresh read. */
    {
        const char *two = "{ edit = { propertySheets = { num = 2; item[0] = { properties = { num = 2; "
            "item[0] = { path = \"a\"; inspector = \"ia\"; } item[1] = { path = \"b\"; } } } "
            "item[1] = { titleTag = \"#B\"; properties = { num = 1; item[0] = { path = \"c\"; } } } } } }";
        const char *text = "{ inherit = \"p\"; edit = { propertySheets = { num = 2; item[0] = { properties = { num = 2; "
            "item[0] = { path = \"x\"; } item[1] = { path = \"b\"; } } } "
            "item[1] = { properties = { num = 1; item[0] = { path = \"y\"; } } } } } }";
        CHECK(restate(text, two, &exact) == 1 && exact);
        if (exact) {
            served_page_text(two, exact, 0, got, sizeof(got));
            snprintf(expected, sizeof(expected), "|path=x;path=b");
            if (strcmp(got, expected)) fprintf(stderr, "two pages, first: %s\n%s\n", got, exact);
            CHECK(!strcmp(got, expected));
            served_page_text(two, exact, 1, got, sizeof(got));
            if (strcmp(got, "|path=y")) fprintf(stderr, "two pages, second: %s\n%s\n", got, exact);
            CHECK(!strcmp(got, "|path=y"));
        }
        free(exact); exact = NULL;
    }
    /* Guard: a positive count still serves everything it can (the dropped
     * elements are destroyed, nothing strands), so a trim over a later
     * addition states no count below 1. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 2; } } } } }",
        SHEET("num = 3; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } item[2] = { path = \"c\"; descriptionTag = \"#c\"; }"),
        SHEET("num = 4; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } item[2] = { path = \"c\"; descriptionTag = \"#c\"; } "
            "item[3] = { path = \"z\"; }"), 0, &result, &kind);
    CHECK(result == 1 && out);
    if (out) {
        CHECK(restate(out, SHEET("num = 4; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } "
            "item[2] = { path = \"c\"; descriptionTag = \"#c\"; } item[3] = { path = \"z\"; }"), &exact) == 1 && exact);
        CHECK(exact && !strstr(exact, "num = 0;") && strstr(exact, "num = 3;"));
    }
    free(exact); exact = NULL; free(out);
}

/* First-slot restatement explicitly clears every previous occupant field.
 * Physical ownership is handled by the admitted native storage transaction;
 * the compiler never emits a temporary zero/regrowth statement. */
static void test_editor_first_slot_encoding(void)
{
    char *out = NULL, *exact = NULL;
    int result, kind;
    const char *published = SHEET("num = 3; item[0] = { path = \"c\"; descriptionTag = \"#c\"; } item[1] = { path = \"d\"; } "
        "item[2] = { path = \"z\"; }");
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 0; } } } } }",
        SHEET("num = 2; item[0] = { path = \"c\"; descriptionTag = \"#c\"; } item[1] = { path = \"d\"; }"), published, 0, &result, &kind);
    CHECK(result == 1 && out);
    if (out) {
        CHECK(restate(out, published, &exact) == 1 && exact);
        if (exact) {
            CHECK(strstr(exact, "path = \"z\";") != NULL);
            CHECK(strstr(exact, "descriptionTag = \"\";") != NULL);
            CHECK(strstr(exact, "inspector = NULL;") != NULL);
            CHECK(strstr(exact, "properties = { num = 0; } properties") == NULL);
            CHECK(!strstr(exact, "propertySheets = { num = 0; }"));
        }
    }
    free(exact); free(out);
}
static void test_editor_family_normalization(void)
{
    char *out = NULL; size_t length = 0;
    const char *type = "snappropertyinspector_whitelistencounterdecl";
    const char *source = "{ edit = { validEncounters = { num = 2; "
        "alias[ 0x0 ] = \"first\"; alias[01] = \"second\"; other[ 0b1 ] = \"last\"; } } }";
    CHECK(sh_package_editor_native_form_type(type, (sh_decl_source){source,strlen(source)},
        NULL,0,&out,&length) == 1 && out);
    CHECK(out && strstr(out,"item[0] = \"first\";") && strstr(out,"item[1] = \"last\";") &&
        !strstr(out,"second"));
    free(out); out = NULL;
    source = "{ edit = { validEncounters = { num = 2; alias[0x100000000] = \"zero\"; "
        "other[4294967297] = \"one\"; last[1.9e+0] = \"float-one\"; } } }";
    CHECK(sh_package_editor_native_form_type(type,(sh_decl_source){source,strlen(source)},NULL,0,&out,&length) == 1 && out);
    CHECK(out && strstr(out,"item[0] = \"zero\";") && strstr(out,"item[1] = \"float-one\";") && !strstr(out,"\"one\";"));
    free(out); out = NULL;
    /* Ordinary reflected objects have ordinary repeated-member semantics:
     * num does not truncate an indexed key outside an admitted collection. */
    source = "{ edit = { other = { num = 1; alias[9] = { value = 3; } num = 2; } } }";
    CHECK(sh_package_editor_native_form_type(type,(sh_decl_source){source,strlen(source)},
        NULL,0,&out,&length) == 1 && out);
    CHECK(out && strstr(out,"num = 2;") && strstr(out,"alias[9]"));
    free(out); out = NULL;
    {
        char error[512]; sh_decl_source input = {source,strlen(source)};
        out = sh_package_editor_contributions(type,input,&input,1,input,0,&length,error,sizeof(error));
        CHECK(out && strstr(out,"num = 2;") && strstr(out,"alias[9]")); free(out); out = NULL;
    }
    /* Native bare radix tokens retain integer0. Digitless lower-e exponents
     * retain the mantissa; consumed suffixes do not enter token conversion. */
    source = "{ edit = { validEncounters = { num = 2; alias[0b] = \"binary-zero\"; other[0X] = \"zero\"; "
        "a[1e+] = \"plus\"; b[1e-] = \"minus\"; c[1UL] = \"unsigned\"; d[1.9e+0F] = \"float-one\"; } } }";
    CHECK(sh_package_editor_native_form_type(type,(sh_decl_source){source,strlen(source)},
        NULL,0,&out,&length) == 1 && out);
    CHECK(out && strstr(out,"item[0] = \"zero\";") && strstr(out,"item[1] = \"float-one\";"));
    free(out); out=NULL;
    {
        const char *bad[] = {"1E1","01.5","0e5","1ULL","1.0FF","0b12"};
        for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {
            char text[256]; int result,kind;
            snprintf(text,sizeof(text),"{ inherit = \"p\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; alias[%s] = { path = \"x\"; } } } } } }",bad[i]);
            out=rebase_one(text,SHEET("num = 0;"),SHEET("num = 0;"),0,&result,&kind);
            CHECK(result<0 && kind==SH_PACKAGE_EDITOR_FAILURE_SOURCE && !out); free(out); out=NULL;
        }
    }
    /* These are the native model row's defaults, including its TRUE bit. */
    source = "{ edit = { renderModelInfoList = { num = 1; item[0] = { categoryId = \"#category\"; } } } }";
    CHECK(sh_package_editor_native_restatement("snappropertyinspector_blockingvolumerendermodel",
        (sh_decl_source){source,strlen(source)},NULL,0,&out,&length) == 1 && out);
    CHECK(out && strstr(out,"renderModelMaterial = NULL;") && strstr(out,"renderModelIcon = NULL;") &&
        strstr(out,"renderModelColorEnabled = true;") && strstr(out,"categoryId = \"#category\";"));
    free(out);
}

static void test_editor_semantic_control_key(void)
{
    const char *a = "{ inherit = \"p\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 1; "
        "item[0] = { displayNameTag = \"#own\"; descriptionTag = \"#help\"; inspector = \"action\"; } } } } } }";
    const char *b = "{ inherit = \"p\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 1; "
        "float ops alias[ 0b0 ] = ! { inspector = \"action\"; descriptionTag = \"#help\"; displayNameTag = \"#own\"; } } } } } }";
    const char *empty = SHEET("num = 0;");
    sh_package_editor_slots slots = {"#semantic:",{NULL,0}};
    char x[256], y[256], *one, *two; int result, kind;
    sh_package_file_identity first, second;
    CHECK(sh_package_bytes_identity(a,strlen(a),&first) && sh_package_bytes_identity(b,strlen(b),&second) &&
        memcmp(first.digest,second.digest,sizeof(first.digest)));
    one = rebase_with(a,empty,empty,1,&slots,&result,&kind); CHECK(result == 1 && one);
    two = rebase_with(b,empty,empty,1,&slots,&result,&kind); CHECK(result == 1 && two);
    if (one && two) {
        slot_identity(one,0,x,sizeof(x)); slot_identity(two,0,y,sizeof(y)); CHECK(x[0] && !strcmp(x,y));
        char error[512]; size_t length = 0;
        sh_decl_source inputs[2] = {{one,strlen(one)},{two,strlen(two)}}, base = {empty,strlen(empty)};
        char *merged = sh_package_editor_contributions("snapeditorentitydef",base,inputs,2,inputs[0],0,&length,error,sizeof(error));
        CHECK(merged);
        if (merged) {
            sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){merged,length},NULL,0);
            const sh_decl_node *list = sh_decl_tree_member(sh_decl_tree_member(sh_decl_tree_member(
                sh_decl_tree_member(tree,"edit"),"propertySheets"),"item[0]"),"properties");
            CHECK(list && !strcmp(sh_decl_tree_member(list,"num")->value,"1")); sh_decl_tree_free(tree);
        }
        free(merged);
    }
    free(one); free(two);
}
typedef struct native_model_row {
    char material[64], icon[64], display[64], description[64], category[64], parm1[64], parm2[64], bloom[64];
    int hotspot, color;
} native_model_row;
typedef struct native_models { native_model_row rows[4]; int count; } native_models;
static void native_model_string(char out[64], const sh_decl_node *field)
{
    const char *text; size_t length;
    CHECK(sh_decl_tree_literal(field,&text,&length));
    if (!sh_decl_tree_literal(field,&text,&length)) return;
    if (length == 4 && !memcmp(text,"NULL",4)) length = 0;
    memset(out,0,64); snprintf(out,64,"%.*s",(int)length,text);
}
/* Independent native value-state oracle for the paired seven-field model row
 * and its four-field propertyState. Fresh rows have color enabled. */
static void native_model_read(native_models *models, const char *text)
{
    sh_decl_node *tree = sh_decl_tree_parse_ordered((sh_decl_source){text,strlen(text)},NULL,0); CHECK(tree);
    for (const sh_decl_node *edit = tree ? tree->children : NULL; edit; edit = edit->next)
        for (const sh_decl_node *list = !strcmp(edit->key,"edit") ? edit->children : NULL; list; list = list->next) {
            int count, index;
            if (strcmp(list->key,"renderModelInfoList")) continue;
            if (native_count(list,&count)) {
                CHECK(count <= 4);
                for (int i = models->count; i < count && i < 4; i++) { memset(models->rows+i,0,sizeof(models->rows[i])); models->rows[i].color = 1; }
                models->count = count > 0 ? count : 0;
            }
            for (const sh_decl_node *item = list->children; item; item = item->next) {
                if (!native_index(item->key,&index) || index < 0 || index >= models->count || index >= 4) continue;
                native_model_row *row = models->rows + index;
                for (const sh_decl_node *field = item->children; field; field = field->next) {
                    if (!strcmp(field->key,"renderModelMaterial")) native_model_string(row->material,field);
                    else if (!strcmp(field->key,"renderModelIcon")) native_model_string(row->icon,field);
                    else if (!strcmp(field->key,"displayNameId")) native_model_string(row->display,field);
                    else if (!strcmp(field->key,"descriptionId")) native_model_string(row->description,field);
                    else if (!strcmp(field->key,"categoryId")) native_model_string(row->category,field);
                    else if (!strcmp(field->key,"usesHotspotModel")) row->hotspot = !strcmp(field->value,"true");
                    else if (!strcmp(field->key,"propertyState")) for (const sh_decl_node *state = field->children; state; state = state->next) {
                        if (!strcmp(state->key,"renderModelColorEnabled")) row->color = !strcmp(state->value,"true");
                        else if (!strcmp(state->key,"renderParm1")) native_model_string(row->parm1,state);
                        else if (!strcmp(state->key,"renderParm2")) native_model_string(row->parm2,state);
                        else if (!strcmp(state->key,"bloom")) native_model_string(row->bloom,state);
                    }
                }
            }
        }
    sh_decl_tree_free(tree);
}
static void test_editor_model_constructor_state(void)
{
    const char *parent = "{ edit = { renderModelInfoList = { num = 1; item[0] = { renderModelMaterial = \"material\"; "
        "renderModelIcon = \"icon\"; displayNameId = \"#name\"; descriptionId = \"#help\"; categoryId = \"#base\"; "
        "usesHotspotModel = true; propertyState = { renderModelColorEnabled = false; renderParm1 = \"parm1\"; "
        "renderParm2 = \"parm2\"; bloom = \"bloom\"; } } } } }";
    const char *child = "{ edit = { renderModelInfoList = { num = 0; } } edit = { renderModelInfoList = { num = 1; "
        "item[0] = { categoryId = \"#own\"; } } } }";
    const char *family = "snappropertyinspector_blockingvolumerendermodel";
    sh_decl_source base = {parent,strlen(parent)};
    native_models want = {0}, got = {0}; char *out = NULL; size_t length = 0;
    native_model_read(&want,parent); native_model_read(&want,child);
    CHECK(want.count == 1 && want.rows[0].color == 1 && !want.rows[0].material[0] && !want.rows[0].hotspot);
    CHECK(sh_package_editor_native_form_type(family,(sh_decl_source){child,strlen(child)},&base,1,&out,&length) == 1 && out);
    if (out) { native_model_read(&got,parent); native_model_read(&got,out); CHECK(!memcmp(&want,&got,sizeof(want))); }
    free(out);
}

/* Gap 2: a reset marker. The reflected reader consumes "!" after a member's
 * "=" through FUN_141a24850 (Vulkan; OpenGL FUN_141a16c10) in the dispatcher
 * FUN_141a209b0 (OpenGL FUN_141a12c00) and skips the block only when the
 * read context's byte at +0xe is set; the context constructor stores 0x101
 * at +0xc, so +0xe is 0, and the inheriting declaration's Parse reads its
 * text with such a context. In those contexts "key = ! { ... }" reads
 * exactly as "key = { ... }", and the marker is part of no state. The
 * independent model reads texts ignoring it, which is the contract below. */
static void test_editor_reset_markers(void)
{
    char got[1024], want[1024];
    char *out = NULL, *exact = NULL, *form = NULL;
    int result, kind;
    const char *published = SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; inspector = \"x\"; }");
    /* A marker on the list block of a complete restatement. */
    const char *marked_list = "{ inherit = \"p\"; edit = { propertySheets = { num = 1; item[0] = { properties = ! { num = 2; "
        "item[0] = { path = \"a\"; } item[1] = { path = \"b\"; descriptionTag = \"#b\"; inspector = \"x\"; } } } } } }";
    const char *raw[2] = {published, marked_list};
    native_page_text(raw, 2, 0, want, sizeof(want));
    CHECK(!strcmp(want, "|path=a;descriptionTag=#b,inspector=x,path=b"));
    CHECK(restate(marked_list, published, &exact) >= 0);
    served_page_text(published, exact ? exact : marked_list, 0, got, sizeof(got));
    if (strcmp(got, want)) fprintf(stderr, "marked list: %s\n", got);
    CHECK(!strcmp(got, want));
    free(exact); exact = NULL;
    /* A marker in the inherited (published) slot. */
    {
        const char *parent = SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = ! { path = \"b\"; }");
        const char *text = "{ inherit = \"p\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
            "item[0] = { path = \"a\"; } item[1] = { path = \"b2\"; } } } } } }";
        CHECK(restate(text, parent, &exact) >= 0);
        served_page_text(parent, exact ? exact : text, 0, got, sizeof(got));
        CHECK(!strcmp(got, "|path=a;path=b2"));
        free(exact); exact = NULL;
    }
    /* A marked sparse patch keeps the field it does not state, as the
     * unmarked one does, through a rebase onto a relaid parent. */
    {
        const char *relaid = SHEET("num = 3; item[0] = { path = \"z\"; } item[1] = { path = \"a\"; } "
            "item[2] = { path = \"b\"; inspector = \"x\"; }");
        const char *patch = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[1] = ! { descriptionTag = \"#x\"; } } } } } }";
        const char *author[2] = {published, patch};
        native_page_text(author, 2, 0, want, sizeof(want));
        CHECK(!strcmp(want, "|path=a;descriptionTag=#x,inspector=x,path=b"));
        out = rebase_one(patch, published, relaid, 0, &result, &kind);
        CHECK(result == 1 && out);
        if (out) {
            CHECK(restate(out, relaid, &exact) >= 0);
            served_page_text(relaid, exact ? exact : out, 0, got, sizeof(got));
            if (strcmp(got, "|path=z;path=a;descriptionTag=#x,inspector=x,path=b")) fprintf(stderr, "marked patch: %s\n", got);
            CHECK(!strcmp(got, "|path=z;path=a;descriptionTag=#x,inspector=x,path=b"));
        }
        free(exact); exact = NULL; free(out); out = NULL;
    }
    /* A member stated twice, the second statement marked. */
    {
        const char *parent = SHEET("num = 2; item[0] = { path = \"a\"; descriptionTag = \"#a\"; } item[1] = { path = \"b\"; }");
        const char *child = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[1] = { descriptionTag = \"#b1\"; } } } } } edit = ! { propertySheets = { item[0] = { properties = { "
            "item[1] = { descriptionTag = \"#b2\"; } } } } } }";
        const char *both[2] = {parent, child};
        native_page_text(both, 2, 0, want, sizeof(want));
        CHECK(!strcmp(want, "|descriptionTag=#a,path=a;descriptionTag=#b2,path=b"));
        CHECK(native_form(child, parent, &form) == 1 && form);
        if (form) {
            const char *read_form[2] = {parent, form};
            native_page_text(read_form, 2, 0, got, sizeof(got));
            if (strcmp(got, want)) fprintf(stderr, "marked repeat: %s\n%s\n", got, form);
            CHECK(!strcmp(got, want));
        }
        free(form);
    }
}

/* Gap 3: repeated statements of one member whose headers differ. A header is
 * [type [ops]] name =, and the member is found by the last token alone (the
 * dispatcher FUN_141a209b0 never reads the type or ops it parsed), so
 * statements that differ in annotation or marker read exactly in order: the
 * later one overlays the earlier. A statement without "=" is another matter:
 * the three-token header read of FUN_141a26810 takes the brace and the next
 * member's name as header tokens, so "edit { ... }" is not a block operation
 * but a mis-tokenized declaration, which no model of this reader can read. */
static void test_editor_header_repeats(void)
{
    char got[1024], want[1024];
    char *form = NULL;
    const char *parent = SHEET("num = 2; item[0] = { path = \"a\"; descriptionTag = \"#a\"; } item[1] = { path = \"b\"; }");
    /* The second edit block carries a type token the first does not. */
    {
        const char *child = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[1] = { descriptionTag = \"#b1\"; } } } } } float edit = { propertySheets = { item[0] = { properties = { "
            "item[1] = { descriptionTag = \"#b2\"; } } } } } }";
        const char *both[2] = {parent, child};
        native_page_text(both, 2, 0, want, sizeof(want));
        CHECK(!strcmp(want, "|descriptionTag=#a,path=a;descriptionTag=#b2,path=b"));
        CHECK(native_form(child, parent, &form) == 1 && form);
        if (form) {
            const char *read_form[2] = {parent, form};
            native_page_text(read_form, 2, 0, got, sizeof(got));
            if (strcmp(got, want)) fprintf(stderr, "annotated repeat: %s\n%s\n", got, form);
            CHECK(!strcmp(got, want));
        }
        free(form); form = NULL;
    }
    /* One slot stated twice in a list, the second under a type token. */
    {
        const char *child = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[1] = { descriptionTag = \"#b1\"; } idStr item[1] = { descriptionTag = \"#b2\"; } } } } } }";
        const char *both[2] = {parent, child};
        native_page_text(both, 2, 0, want, sizeof(want));
        CHECK(!strcmp(want, "|descriptionTag=#a,path=a;descriptionTag=#b2,path=b"));
        CHECK(native_form(child, parent, &form) == 1 && form);
        if (form) {
            const char *read_form[2] = {parent, form};
            native_page_text(read_form, 2, 0, got, sizeof(got));
            if (strcmp(got, want)) fprintf(stderr, "annotated slot repeat: %s\n%s\n", got, form);
            CHECK(!strcmp(got, want));
        }
        free(form); form = NULL;
    }
    /* Swapping the statements swaps the winner: order is state, headers are not. */
    {
        const char *child = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "idStr item[1] = { descriptionTag = \"#b2\"; } item[1] = { descriptionTag = \"#b1\"; } } } } } }";
        const char *both[2] = {parent, child};
        native_page_text(both, 2, 0, want, sizeof(want));
        CHECK(!strcmp(want, "|descriptionTag=#a,path=a;descriptionTag=#b1,path=b"));
        CHECK(native_form(child, parent, &form) == 1 && form);
        if (form) {
            const char *read_form[2] = {parent, form};
            native_page_text(read_form, 2, 0, got, sizeof(got));
            CHECK(!strcmp(got, want));
        }
        free(form); form = NULL;
    }
    /* Control: statements that differ in the block operation ("edit {" has no
     * "=") stay unmodelled; they are not read as a block operation. */
    CHECK(native_form("{ inherit = \"p\"; edit = { displayNameTag = \"#x\"; } edit { displayNameTag = \"#y\"; } }", parent, &form) == -3 &&
        !form);
    free(form);
}

/* Gap 4: list entries other than item[n]. The array reader's header is
 * [type [ops]] name[n] = (FUN_141a25b90 reads three tokens and takes the one
 * followed by "[", then the number, "]" and "="); FUN_141a24c70 compares
 * the type, ops and name with the expected ones and reports a difference as
 * a warning, and FUN_141a202d0 ignores that result and reads the element at
 * the index (OpenGL FUN_141a17f50 / FUN_141a17030 / the same caller). So
 * "control[1] = { ... }" is slot 1, exactly as "item[1] = { ... }". A member
 * with no "[n]" in its header is the lexer's error ("Expected 'type ops
 * name[n]'"), a malformed list that the child's author has to fix. */
static void test_editor_slot_names(void)
{
    char got[1024], want[1024];
    char *out = NULL, *item = NULL, *exact = NULL;
    int result, kind;
    const char *authored = SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; }");
    const char *relaid = SHEET("num = 3; item[0] = { path = \"z\"; } item[1] = { path = \"a\"; } item[2] = { path = \"b\"; }");
    /* A sparse patch naming its element differently lands on the same slot
     * and control as the item[n] spelling, through a rebase onto a relaid parent. */
    {
        const char *named = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "control[1] = { descriptionTag = \"#str_b\"; } } } } } }";
        const char *spelled = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[1] = { descriptionTag = \"#str_b\"; } } } } } }";
        const char *author[2] = {authored, named};
        native_page_text(author, 2, 0, want, sizeof(want));
        CHECK(!strcmp(want, "|path=a;descriptionTag=#str_b,path=b"));
        out = rebase_one(named, authored, relaid, 0, &result, &kind);
        item = rebase_one(spelled, authored, relaid, 0, &result, &kind);
        CHECK(result == 1 && item);
        CHECK(out && item && !strcmp(out, item));
        if (out) {
            const char *served[2] = {relaid, out};
            native_page_text(served, 2, 0, got, sizeof(got));
            if (strcmp(got, "|path=z;path=a;descriptionTag=#str_b,path=b")) fprintf(stderr, "named patch: %s\n%s\n", got, out);
            CHECK(!strcmp(got, "|path=z;path=a;descriptionTag=#str_b,path=b"));
        }
        free(out); free(item); out = item = NULL;
    }
    /* A parent stating a differently named slot, read as any other. */
    {
        const char *named_parent = "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
            "item[0] = { path = \"a\"; } entry[1] = { path = \"b\"; } } } } } }";
        const char *child = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[1] = { descriptionTag = \"#x\"; } } } } } }";
        const char *author[2] = {named_parent, child};
        native_page_text(author, 2, 0, want, sizeof(want));
        CHECK(!strcmp(want, "|path=a;descriptionTag=#x,path=b"));
        out = rebase_one(child, named_parent, named_parent, 1, &result, &kind);
        CHECK(result == 1 && out);
        if (out) {
            const char *served[2] = {named_parent, out};
            native_page_text(served, 2, 0, got, sizeof(got));
            if (strcmp(got, want)) fprintf(stderr, "named parent slot: %s\n%s\n", got, out);
            CHECK(!strcmp(got, want));
        }
        free(out); out = NULL;
    }
    /* A complete restatement naming a slot differently is still complete: its
     * second control would keep the occupant's label natively, so it is cut
     * and regrown exactly as the item[n] spelling is (read as a sparse patch,
     * it was passed on unchanged and read back stale). */
    {
        const char *published = SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; descriptionTag = \"#b\"; }");
        const char *text = "{ inherit = \"p\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
            "item[0] = { path = \"a\"; } part[1] = { path = \"z\"; } } } } } }";
        CHECK(restate(text, published, &exact) == 1 && exact);
        if (exact) {
            served_page_text(published, exact, 0, got, sizeof(got));
            if (strcmp(got, "|path=a;path=z")) fprintf(stderr, "named restatement: %s\n%s\n", got, exact);
            CHECK(!strcmp(got, "|path=a;path=z"));
        }
        free(exact); exact = NULL;
    }
    /* A member without an index is the child author's defect. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
        "label = \"x\"; } } } } }", authored, relaid, 0, &result, &kind);
    CHECK(result < 0 && kind == SH_PACKAGE_EDITOR_FAILURE_SOURCE && !out);
    free(out);
}

/* A child's sparse list addresses its parent's controls by position. When the
 * published parent's layout differs from its author's, each patch still lands
 * on its intended control, the parent's additions survive and the child's own
 * removals, restatements and pages are kept. */
static void test_editor_rebase(void)
{
    char layout[512]; int result, kind; char *out;
    const char *stock = SHEET("num = 2; item[0] = { path = \"displayName\"; } item[1] = { path = \"actionDelay\"; }");
    const char *added = SHEET("num = 3; item[0] = { path = \"displayName\"; } item[1] = { path = \"actionDelay\"; } "
        "item[2] = { path = \"comment\"; descriptionTag = \"#str_comment\"; }");
    /* Appending by index (stock apply_buff) would overwrite the added control. */
    out = rebase_one("{ inherit = \"input_base\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 4; "
        "item[2] = { path = \"modType\"; } item[3] = { path = \"modValue\"; } } } } } }", stock, added, 0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "displayName,actionDelay,comment=#str_comment,modType,modValue"));
    CHECK(out && strstr(out, "inherit = \"input_base\""));
    free(out);
    /* Parents are read in order, as the native reader applies them: a shipped
     * row repeating another field (fx/ambient_embers states fxInfo twice) still
     * rebases, and a child stating no collection never reads its parents. A
     * parent stating a collection twice is read twice, in order (pe_apply). */
    {
        const char *fx = "{ edit = { fxInfo = { rate = 1; } fxInfo = { rate = 2; } ";
        char repeated_stock[512], repeated_added[512];
        snprintf(repeated_stock, sizeof(repeated_stock), "%s%s", fx, stock + strlen("{ edit = { "));
        snprintf(repeated_added, sizeof(repeated_added), "%s%s", fx, added + strlen("{ edit = { "));
        out = rebase_one("{ inherit = \"embers\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 4; "
            "item[2] = { path = \"modType\"; } item[3] = { path = \"modValue\"; } } } } } }", repeated_stock, repeated_added,
            0, &result, &kind);
        CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
        CHECK(out && !strcmp(layout, "displayName,actionDelay,comment=#str_comment,modType,modValue"));
        free(out);
        const char *twice = "{ edit = { propertySheets = { num = 1; } propertySheets = { num = 2; } } }";
        out = rebase_one("{ inherit = \"embers\"; edit = { entityDef = \"embers_def\"; } }", twice, twice, 1, &result, &kind);
        CHECK(result == 0 && !out); free(out);
        /* The second statement extends the first: a, b, then c at slot 2. The
         * child's patch of b follows it past the published insertion of x. */
        out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[1] = { descriptionTag = \"#str_b\"; } } } } } }",
            "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; item[0] = { path = \"a\"; } "
            "item[1] = { path = \"b\"; } } } } propertySheets = { item[0] = { properties = { num = 3; "
            "item[2] = { path = \"c\"; } } } } } }",
            SHEET("num = 4; item[0] = { path = \"a\"; } item[1] = { path = \"x\"; } item[2] = { path = \"b\"; } "
                "item[3] = { path = \"c\"; }"), 0, &result, &kind);
        CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
        CHECK(out && !strcmp(layout, "a,x,b=#str_b,c"));
        free(out);
        /* A block met for the first time inside a parent's first statement is
         * read as the native reader reads it, not copied as written: its
         * repeated properties blocks both apply, the first num of each before
         * its items, an item past that count is skipped (a later resize leaves
         * its slot empty), and a repeated scalar keeps its last value. The
         * authored parent therefore lays out exactly as this published one, and
         * the child needs no rebase. */
        out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[1] = { descriptionTag = \"#str_b\"; } } } } } }",
            "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 1; "
            "item[0] = { path = \"a\"; type = \"float\"; type = \"int\"; } item[2] = { path = \"ghost\"; } } "
            "properties = { num = 3; item[1] = { path = \"b\"; } } } } } }",
            SHEET("num = 3; item[0] = { path = \"a\"; type = \"int\"; } item[1] = { path = \"b\"; } item[2] = { }"),
            0, &result, &kind);
        CHECK(result == 0 && !out); free(out);
        /* The repeated scalar's last value is what the child's author saw: a
         * child moving a with type "int" changes nothing of a, so a keeps the
         * published parent's own type (forced: the layouts agree). */
        out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 2; "
            "item[0] = { path = \"z\"; } item[1] = { path = \"a\"; type = \"int\"; } } } } } }",
            "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 1; "
            "item[0] = { path = \"a\"; type = \"float\"; type = \"int\"; } } } } } }",
            SHEET("num = 1; item[0] = { path = \"a\"; type = \"bool\"; }"), 1, &result, &kind);
        CHECK(result == 1 && out);
        {
            sh_decl_node *tree = out ? sh_decl_tree_parse((sh_decl_source){out, strlen(out)}, NULL, 0) : NULL;
            const sh_decl_node *properties = sh_decl_tree_member(sh_decl_tree_member(sh_decl_tree_member(
                sh_decl_tree_member(tree, "edit"), "propertySheets"), "item[0]"), "properties");
            const sh_decl_node *a = NULL;
            const char *type = NULL; size_t type_length = 0;
            for (const sh_decl_node *item = properties ? properties->children : NULL; item; item = item->next) {
                const char *path; size_t length;
                if (sh_decl_tree_literal(sh_decl_tree_member(item, "path"), &path, &length) && length == 1 && *path == 'a') a = item;
            }
            CHECK(a && sh_decl_tree_literal(sh_decl_tree_member(a, "type"), &type, &type_length) &&
                type_length == 4 && !memcmp(type, "bool", 4));
            sh_decl_tree_free(tree);
        }
        free(out);
        /* Only a block's first num counts: x and y stay, as without the second. */
        out = rebase_one("{ inherit = \"p\"; edit = { inputs = { num = 3; item[2] = \"z\"; } } }",
            "{ edit = { inputs = { num = 2; item[0] = \"x\"; item[1] = \"y\"; num = 1; } } }",
            "{ edit = { inputs = { num = 3; item[0] = \"x\"; item[1] = \"y\"; item[2] = \"w\"; } } }", 0, &result, &kind);
        CHECK(result == 1 && out); if (out) rebased_layout(out, "inputs", -1, layout, sizeof(layout));
        CHECK(out && !strcmp(layout, "x,y,w,z"));
        free(out);
        /* An item past the count is skipped when read, so growing the list later
         * gives an empty slot, not that item: the authored parent differs from
         * the published one, which states q there. */
        out = rebase_one("{ inherit = \"p\"; edit = { inputs = { item[1] = \"Y\"; } } }",
            "{ edit = { inputs = { num = 2; item[0] = \"x\"; item[1] = \"y\"; } inputs = { item[2] = \"q\"; } "
            "inputs = { num = 3; } } }",
            "{ edit = { inputs = { num = 3; item[0] = \"x\"; item[1] = \"y\"; item[2] = \"q\"; } } }", 0, &result, &kind);
        CHECK(result == 1 && out); free(out);
        out = rebase_one("{ inherit = \"p\"; edit = { inputs = { item[1] = \"Y\"; } } }",
            "{ edit = { inputs = { num = 2; item[0] = \"x\"; item[1] = \"y\"; } inputs = { num = 3; item[2] = \"q\"; } } }",
            "{ edit = { inputs = { num = 3; item[0] = \"x\"; item[1] = \"y\"; item[2] = \"q\"; } } }", 0, &result, &kind);
        CHECK(result == 0 && !out); free(out);
    }
    /* A control without a path, which the native reader places by slot alone,
     * has no identity a composition can union, so a rebase for composition
     * (slots) gives it one by where it came from, under the caller's
     * namespace: a published control its published slot (equal ones paired by
     * content, then order, so an edit lands on the slot its author edited and
     * their number is kept); the shipped declaration's own such control its
     * native slot; any other own control its content, equal ones paired by
     * occurrence. Dropping them afterwards touches nothing else. */
    {
        sh_package_editor_slots slots = {"#t-0:", {NULL, 0}};
        const char *stock_sheet = SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = { type = \"float\"; }");
        const char *relaid = SHEET("num = 3; item[0] = { path = \"x\"; } item[1] = { path = \"a\"; } "
            "item[2] = { type = \"float\"; }");
        const char *child = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
            "item[1] = { descriptionTag = \"#str_n\"; } item[2] = { type = \"int\"; } } } } } }";
        char first[160], second[160], third[160];
        out = rebase_with(child, stock_sheet, relaid, 0, &slots, &result, &kind);
        CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
        CHECK(out && !strncmp(layout, "x,a,#t-0:p2=#str_n,#t-0:c", 25) && strlen(layout) > 3 &&
            !strcmp(layout + strlen(layout) - 2, "#0"));
        CHECK(out && stripped_layout(out, "#t-0:", layout, sizeof(layout)) == 1 && !strcmp(layout, "x,a,?=#str_n,?"));
        CHECK(out && stripped_layout(out, "#t-1:", layout, sizeof(layout)) == 0);
        free(out);
        /* The unchanged layout stays sparse; without slots nothing is given. */
        out = rebase_with(child, stock_sheet, stock_sheet, 0, &slots, &result, &kind);
        CHECK(result == 0 && !out); free(out);
        out = rebase_one(child, stock_sheet, relaid, 0, &result, &kind);
        CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
        CHECK(out && !strcmp(layout, "x,a,?=#str_n,?")); free(out);
        /* Repeated equal published controls keep their number, and the edit of
         * the second stays on the second. */
        out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[1] = { descriptionTag = \"#str_second\"; } } } } } }",
            SHEET("num = 2; item[0] = { type = \"float\"; } item[1] = { type = \"float\"; }"),
            SHEET("num = 3; item[0] = { path = \"x\"; } item[1] = { type = \"float\"; } item[2] = { type = \"float\"; }"),
            0, &slots, &result, &kind);
        CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
        CHECK(out && !strcmp(layout, "x,#t-0:p1,#t-0:p2=#str_second")); free(out);
        /* Own controls: different ones differ, an exact duplicate in another
         * child is the same, a child's repeated equal ones are counted. */
        out = rebase_with(child, stock_sheet, relaid, 0, &slots, &result, &kind);
        if (out) slot_identity(out, 3, first, sizeof(first)); free(out);
        out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
            "item[2] = { type = \"int\"; } } } } } }", stock_sheet, relaid, 0, &slots, &result, &kind);
        if (out) slot_identity(out, 3, second, sizeof(second)); free(out);
        out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
            "item[2] = { type = \"bool\"; } } } } } }", stock_sheet, relaid, 0, &slots, &result, &kind);
        if (out) slot_identity(out, 3, third, sizeof(third)); free(out);
        CHECK(!strncmp(first, "#t-0:c", 6) && !strcmp(first, second) && strcmp(first, third) && !strncmp(third, "#t-0:c", 6));
        out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 4; "
            "item[2] = { type = \"int\"; } item[3] = { type = \"int\"; } } } } } }", stock_sheet, relaid, 0, &slots,
            &result, &kind);
        if (out) { slot_identity(out, 3, second, sizeof(second)); slot_identity(out, 4, third, sizeof(third)); }
        CHECK(out && !strcmp(first, second) && !strncmp(third, first, strlen(first) - 1) &&
            !strcmp(third + strlen(third) - 2, "#1"));
        free(out);
        /* The shipped declaration's own equal controls are its native slots in
         * every copy, edited or not; a further own one has its content. */
        {
            const char *shipped = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 4; "
                "item[2] = { type = \"float\"; } item[3] = { type = \"float\"; } } } } } }";
            sh_package_editor_slots shared = {"#t-0:", {shipped, strlen(shipped)}};
            const char *two = SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; }");
            const char *relaid_two = SHEET("num = 3; item[0] = { path = \"x\"; } item[1] = { path = \"a\"; } "
                "item[2] = { path = \"b\"; }");
            out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 5; "
                "item[2] = { type = \"float\"; } item[3] = { type = \"float\"; descriptionTag = \"#str_second\"; } "
                "item[4] = { type = \"float\"; } } } } } }", two, relaid_two, 0, &shared, &result, &kind);
            CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
            CHECK(out && !strncmp(layout, "x,a,b,#t-0:s0.2,#t-0:s0.3=#str_second,#t-0:c", 44) &&
                !strcmp(layout + strlen(layout) - 2, "#0"));
            free(out);
        }
        /* A rebase for composition reports when the child appends, in its
         * author's frame, an own control without a path; a sparse patch's item
         * addressing an inherited control is not one. */
        {
            sh_package_editor_slots probe = {"#t-0:", {NULL, 0}, 0};
            out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
                "item[1] = { descriptionTag = \"#str_n\"; } } } } } }", stock_sheet, stock_sheet, 0, &probe, &result, &kind);
            CHECK(result == 0 && !out && !probe.appends); free(out);
            out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
                "item[2] = { type = \"int\"; } } } } } }", stock_sheet, stock_sheet, 0, &probe, &result, &kind);
            CHECK(result == 0 && !out && probe.appends); free(out);
        }
        /* The namespace is the caller's: an input stating it is refused for that
         * namespace only (-2, choose another); an authored value that merely
         * looks like an identity is neither refused nor dropped. */
        out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
            "item[2] = { path = \"#t-0:p0\"; } } } } } }", stock_sheet, relaid, 0, &slots, &result, &kind);
        CHECK(result == -2 && !out); free(out);
        out = rebase_with(child, stock_sheet, SHEET("num = 3; item[0] = { path = \"#t-0:x\"; } item[1] = { path = \"a\"; } "
            "item[2] = { type = \"float\"; }"), 0, &slots, &result, &kind);
        CHECK(result == -2 && !out); free(out);
        {
            sh_package_editor_slots other = {"#t-1:", {NULL, 0}};
            out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 4; "
                "item[2] = { path = \"#t-0:p0\"; } item[3] = { type = \"int\"; } } } } } }", stock_sheet, relaid, 0,
                &other, &result, &kind);
            CHECK(result == 1 && out);
            CHECK(out && stripped_layout(out, "#t-1:", layout, sizeof(layout)) == 1 && !strcmp(layout, "x,a,?,#t-0:p0,?"));
            free(out);
        }
    }
    /* Equal layouts are left to native inheritance unless forced. */
    out = rebase_one("{ inherit = \"input_base\"; edit = { propertySheets = { item[0] = { properties = { "
        "item[1] = { descriptionTag = \"#str_delay\"; } } } } } }", stock, stock, 0, &result, &kind);
    CHECK(result == 0 && !out);
    out = rebase_one("{ inherit = \"input_base\"; edit = { propertySheets = { item[0] = { properties = { "
        "item[1] = { descriptionTag = \"#str_delay\"; } } } } } }", stock, stock, 1, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "displayName,actionDelay=#str_delay"));
    free(out);
    /* A control rebound to another inspector at its own slot still takes the
     * child's patch natively: nothing to restate. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
        "item[2] = { path = \"volume\"; } } } } } }",
        SHEET("num = 2; item[0] = { path = \"name\"; inspector = \"stock\"; } item[1] = { path = \"delay\"; }"),
        SHEET("num = 2; item[0] = { path = \"name\"; inspector = \"private\"; } item[1] = { path = \"delay\"; }"),
        0, &result, &kind);
    CHECK(result == 0 && !out); free(out);
    /* Controls sharing a path are told apart by their binding. Swapped, the
     * child's patch would land on the other one natively, so it is rebased to
     * follow its control; one of them rebound in place keeps both slots. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
        "item[0] = { descriptionTag = \"#str_x\"; } } } } } }",
        SHEET("num = 2; item[0] = { path = \"m\"; inspector = \"x\"; } item[1] = { path = \"m\"; inspector = \"y\"; }"),
        SHEET("num = 2; item[0] = { path = \"m\"; inspector = \"y\"; } item[1] = { path = \"m\"; inspector = \"x\"; }"),
        0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "m,m=#str_x"));
    free(out);
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
        "item[0] = { descriptionTag = \"#str_x\"; } } } } } }",
        SHEET("num = 2; item[0] = { path = \"m\"; inspector = \"x\"; } item[1] = { path = \"m\"; inspector = \"y\"; }"),
        SHEET("num = 2; item[0] = { path = \"m\"; inspector = \"z\"; } item[1] = { path = \"m\"; inspector = \"y\"; }"),
        0, &result, &kind);
    CHECK(result == 0 && !out); free(out);
    /* A control inserted first: the patch follows speed, not its old slot. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
        "item[1] = { descriptionTag = \"#str_child\"; } } } } } }",
        SHEET("num = 2; item[0] = { path = \"health\"; } item[1] = { path = \"speed\"; }"),
        SHEET("num = 3; item[0] = { path = \"armor\"; } item[1] = { path = \"health\"; } item[2] = { path = \"speed\"; }"),
        0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "armor,health,speed=#str_child"));
    free(out);
    /* Restating shifted slots (stock keycards): each restated control is the
     * published one at its published slot, the addition keeps its place and
     * the child's new control follows, so no slot inherits another's fields. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 4; "
        "item[1] = { path = \"m\"; } item[2] = { path = \"f\"; } item[3] = { path = \"c\"; } } } } } }",
        SHEET("num = 3; item[0] = { path = \"a\"; } item[1] = { path = \"f\"; } item[2] = { path = \"c\"; }"),
        SHEET("num = 4; item[0] = { path = \"a\"; } item[1] = { path = \"f\"; descriptionTag = \"#str_f\"; } "
            "item[2] = { path = \"c\"; } item[3] = { path = \"z\"; }"), 0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "a,f=#str_f,c,z,m"));
    free(out);
    /* The child's truncation stays; a control the parent's authors removed is
     * kept only where the child edited it. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 2; } } } } }",
        SHEET("num = 3; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } item[2] = { path = \"c\"; }"),
        SHEET("num = 4; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } item[2] = { path = \"c\"; } item[3] = { path = \"z\"; }"),
        0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "a,b,z"));
    free(out);
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
        "item[1] = { descriptionTag = \"#str_b\"; } } } } } }",
        SHEET("num = 3; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } item[2] = { path = \"c\"; }"),
        SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = { path = \"c\"; }"), 0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "a,c,b=#str_b"));
    free(out);
    /* Pages: the package moved b to its own page; the child's new page follows
     * the published pages. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { num = 2; item[0] = { properties = { "
        "item[1] = { descriptionTag = \"#str_b\"; } } } item[1] = { properties = { num = 1; item[0] = { path = \"q\"; } } } } } }",
        SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; }"),
        "{ edit = { propertySheets = { num = 2; item[0] = { properties = { num = 1; item[0] = { path = \"a\"; } } } "
        "item[1] = { properties = { num = 1; item[0] = { path = \"b\"; } } } } } }", 0, &result, &kind);
    CHECK(result == 1 && out);
    if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "a"));
    if (out) rebased_layout(out, "propertySheets", 1, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "b=#str_b"));
    if (out) rebased_layout(out, "propertySheets", 2, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "q"));
    free(out);
    /* Flat lists rebase by value. */
    out = rebase_one("{ inherit = \"p\"; edit = { inputs = { num = 3; item[2] = \"z\"; } } }",
        "{ edit = { inputs = { num = 2; item[0] = \"x\"; item[1] = \"y\"; } } }",
        "{ edit = { inputs = { num = 3; item[0] = \"x\"; item[1] = \"y\"; item[2] = \"w\"; } } }", 0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "inputs", -1, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "x,y,w,z"));
    free(out);
    /* A list member with no [n] index in its header (Vulkan FUN_141a25b90,
     * OpenGL FUN_141a17f50): the array reader finds no '[' among three header
     * tokens and reports "Expected 'type ops name[n]'" through the lexer's
     * error sink (not the warning sink), leaving the index unwritten and the
     * lexer three tokens on. That is a natively malformed list, the child
     * author's defect, not a valid composition the product lacks. */
    out = rebase_one("{ inherit = \"p\"; edit = { inputs = { bogus = 1; } } }",
        "{ edit = { inputs = { num = 1; item[0] = \"x\"; } } }",
        "{ edit = { inputs = { num = 2; item[0] = \"x\"; item[1] = \"w\"; } } }", 0, &result, &kind);
    CHECK(result < 0 && kind == SH_PACKAGE_EDITOR_FAILURE_SOURCE && !out);
    out = rebase_one("{ inherit = \"p\"; edit = { inputs = { num = 2; item[1] = \"z\"; } } }",
        "{ edit = { inputs = { num = broken; } } }",
        "{ edit = { inputs = { num = 1; item[0] = \"w\"; } } }", 0, &result, &kind);
    CHECK(result < 0 && kind != SH_PACKAGE_EDITOR_FAILURE_SOURCE && !out);
}

static void test_requirement_reduction(void)
{
    const char *descriptors[] = {
        "{\"id\":\"a\",\"name\":\"A\",\"requirements\":{\"cvars\":{"
        "\"g_useResourceBlackList\":1,\"g_useImageBlackList\":false}}}",
        "{\"id\":\"b\",\"name\":\"B\",\"requirements\":{\"cvars\":{"
        "\"g_useImageBlackList\":true,\"g_useResourceBlackList\":0}}}"
    };
    const char *invalid[] = {"{\"unknownGate\":0}", "{\"g_useImageBlackList\":2}",
        "{\"g_useImageBlackList\":\"0\"}", "{\"g_useImageBlackList\":null}",
        "{\"g_useImageBlackList\":{}}", "{\"g_useImageBlackList\\u0000suffix\":0}"};
    sh_package packages[2] = {0};
    sh_package_component components[2] = {0};
    sh_package_sources sources = {packages, 2, components, 2};
    sh_package_compile_environment environment = {0};
    sh_package_owners conflicted = {0}, selected = {0};
    size_t rejected = 0;
    char error[1024], descriptor[1024];
    char *expected = NULL;
    strcpy_s(packages[0].name, sizeof(packages[0].name), "a");
    strcpy_s(packages[1].name, sizeof(packages[1].name), "b");
    for (size_t i = 0; i < 2; i++) {
        components[i].owner = i; components[i].relative = "";
        CHECK(sh_package_descriptor_parse(descriptors[i], strlen(descriptors[i]),
            &components[i].descriptor, error, sizeof(error)));
    }
    environment.invalid_package = &rejected; environment.conflicted_packages = &conflicted;
    for (int independent = 0; independent < 2; independent++) {
        environment.independent_sources = independent;
        for (int reversed = 0; reversed < 2; reversed++) {
            sh_package_compilation *compiled = sh_package_compile_with(&sources, &environment, error, sizeof(error));
            CHECK(compiled && rejected == SIZE_MAX && !sh_package_owners_count(&conflicted));
            if (compiled) {
                sh_package_policy selected_policy = {0};
                const char *cvars = sh_json_object_get(&compiled->policy.requirements, "cvars");
                char *serialized = sh_json_serialize_object(&compiled->policy.requirements, 0, NULL);
                CHECK(compiled->sources->package_count == 2 && cvars && !strcmp(cvars,
                    "{\"g_useImageBlackList\":0,\"g_useResourceBlackList\":0}"));
                if (!expected) expected = serialized;
                else { CHECK(serialized && !strcmp(serialized, expected)); free(serialized); }
                CHECK(sh_package_owners_add(&selected, 0));
                CHECK(sh_package_compilation_policy(compiled, &selected, &selected_policy, error, sizeof(error)));
                cvars = sh_json_object_get(&selected_policy.requirements, "cvars");
                CHECK(cvars && !strcmp(cvars, "{\"g_useImageBlackList\":0}"));
                sh_package_policy_free(&selected_policy); sh_package_owners_free(&selected);
            }
            sh_package_compilation_free(compiled);
            sh_package_component temporary = components[0]; components[0] = components[1]; components[1] = temporary;
        }
    }
    /* The derived union must not rewrite the descriptor retained for delivery. */
    CHECK(!strcmp(sh_package_descriptor_section(&components[0].descriptor, "requirements"),
        "{\"cvars\":{\"g_useResourceBlackList\":1,\"g_useImageBlackList\":false}}"));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        sh_package_compilation *compiled;
        snprintf(descriptor, sizeof(descriptor), "{\"id\":\"b\",\"name\":\"B\","
            "\"requirements\":{\"cvars\":%s}}", invalid[i]);
        sh_package_descriptor_free(&components[1].descriptor);
        CHECK(sh_package_descriptor_parse(descriptor, strlen(descriptor), &components[1].descriptor, error, sizeof(error)));
        compiled = sh_package_compile_with(&sources, &environment, error, sizeof(error));
        CHECK(!compiled && rejected == 1 && strstr(error, "unsupported setting") &&
            !sh_package_owners_count(&conflicted));
        sh_package_compilation_free(compiled);
        sources.package_count = sources.component_count = 1;
        compiled = sh_package_compile_with(&sources, &environment, error, sizeof(error));
        CHECK(compiled && rejected == SIZE_MAX);
        sh_package_compilation_free(compiled);
        sources.package_count = sources.component_count = 2;
    }
    /* Map policy overlay still owns presentation and historical string values.
     * A map's restrictive permission request cannot close a local open gate. */
    sh_package_descriptor_free(&components[0].descriptor);
    sh_package_descriptor_free(&components[1].descriptor);
    descriptors[0] = "{\"id\":\"a\",\"name\":\"A\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":0}},"
        "\"strings\":{\"en\":{\"#str_demo\":\"Local\"}},\"hud\":{\"weapons\":{\"demo\":{\"ammo_display\":\"weapon\"}}}}";
    descriptors[1] = "{\"id\":\"b\",\"name\":\"B\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":true}},"
        "\"strings\":{\"en\":{\"#str_demo\":\"Map\"}},\"hud\":{\"weapons\":{\"demo\":{\"ammo_display\":\"engine\"}}}}";
    for (size_t i = 0; i < 2; i++) CHECK(sh_package_descriptor_parse(descriptors[i], strlen(descriptors[i]),
        &components[i].descriptor, error, sizeof(error)));
    {
        sh_package_compilation view = {0}; sh_package_policy policy = {0}; sh_package_owners selected = {0};
        view.sources = &sources; view.map_overlay = 1; view.map_owner_begin = 1;
        CHECK(sh_package_owners_add(&selected, 0) && sh_package_owners_add(&selected, 1));
        CHECK(sh_package_compilation_policy(&view, &selected, &policy, error, sizeof(error)));
        const char *cvars = sh_json_object_get(&policy.requirements, "cvars");
        const char *strings = sh_json_object_get(&policy.strings, "en");
        const char *weapons = sh_json_object_get(&policy.hud, "weapons");
        CHECK(cvars && strstr(cvars, "g_useImageBlackList") && strstr(cvars, "0"));
        CHECK(strings && strstr(strings, "Map") && !strstr(strings, "Local"));
        CHECK(weapons && strstr(weapons, "engine") && !strstr(weapons, "weapon\""));
        sh_package_policy_free(&policy); sh_package_owners_free(&selected);
    }
    for (size_t i = 0; i < 2; i++) sh_package_descriptor_free(&components[i].descriptor);
    sh_package_owners_free(&conflicted); free(expected);
}

static void test_encounter_membership_order(void)
{
    static const char *types[] = {"snappropertyinspector_whitelistencounterdecl",
        "snappropertyinspector_whitelistencounterdecllist"};
    const char *base_text = "{ edit = { validEncounters = { num = 0; } } }";
    const char *a = "{ edit = { validEncounters = { num = 2; item[0] = \"alpha\"; item[1] = \"beta\"; } } }";
    const char *b = "{ edit = { validEncounters = { num = 4; item[0] = \"beta\"; item[1] = \"alpha\"; "
        "item[2] = \"gamma\"; item[3] = \"beta\"; } } }";
    sh_decl_source base = {base_text, strlen(base_text)}, parts[] = {{a, strlen(a)}, {b, strlen(b)}};
    for (size_t type = 0; type < 2; type++) for (int reverse = 0; reverse < 2; reverse++) {
        sh_decl_source inputs[] = {parts[reverse], parts[!reverse]};
        char error[1024]; size_t length = 0;
        char *out = sh_package_editor_contributions(types[type], base, inputs, 2, inputs[0], 0,
            &length, error, sizeof(error));
        if (!out) fprintf(stderr, "encounter membership %s: %s\n", types[type], error);
        CHECK(out);
        if (out) {
            sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){out, length}, error, sizeof(error));
            const sh_decl_node *list = sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "validEncounters");
            const sh_decl_node *num = sh_decl_tree_member(list, "num");
            CHECK(num && !strcmp(num->value, "3"));
            CHECK(strstr(out, "\"alpha\"") && strstr(out, "\"beta\"") && strstr(out, "\"gamma\""));
            sh_decl_tree_free(tree); free(out);
        }
    }
}

int main(int argc, char **argv)
{
    char temp[MAX_PATH], error[2048];
    sh_package_sources *sources = NULL;
    sh_package_compilation *compiled = NULL;
    const sh_compiled_resource *resource;
    if (argc == 3 && !strcmp(argv[1], "--editor-sweep")) { editor_sweep(argv[2]); return failures ? 1 : 0; }
    if (argc == 3) { probe(argv[1], argv[2]); return failures ? 1 : 0; }
    CHECK(GetTempPathA(sizeof(temp), temp)); CHECK(GetTempFileNameA(temp, "pc2", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    create("overrides/a/package.json", "{\"id\":\"a\",\"name\":\"A\"}");
    create("overrides/b/package.json", "{\"id\":\"b\",\"name\":\"B\"}");
    create("overrides/a/assets/generated/decls/snappropertyinspector_whitelistencounterdecl/demo.decl",
        "{ edit = { validEncounters = { num = 2; item[0] = \"stock\"; item[1] = \"cyber\"; } } }");
    create("overrides/b/assets/generated/decls/snappropertyinspector_whitelistencounterdecl/demo.decl",
        "{ edit = { validEncounters = { num = 2; item[0] = \"stock\"; item[1] = \"hell_guard\"; } } }");
    create("overrides/a/assets/generated/image/shared.bimage", "identical image");
    create("overrides/b/assets/generated/image/shared.bimage", "identical image");
    create("overrides/a/assets/generated/decls/snapeditorentitydef/volume/blocking.decl",
        "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
        "item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } "
        "item[1] = { path = \"allowClimb\"; inspector = \"boolinspector\"; } } } } } }");
    create("overrides/b/assets/generated/decls/snapeditorentitydef/volume/blocking.decl",
        "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
        "item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } "
        "item[1] = { path = \"receiveDecals\"; inspector = \"boolinspector\"; } } } } } }");
    create("overrides/a/assets/cooked/model/replaced.bmodel", "original model");
    create("overrides/b/assets/cooked/model/replaced.bmodel", "modified model");
    create("overrides/a/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl",
        "{ edit = { aiTypeList = { num = 2; item[0] = { aiType = \"4\"; entityDef = \"ai/imp\"; } "
        "item[1] = { aiType = \"524288\"; entityDef = \"ai/cyber\"; } } } }");
    create("overrides/b/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl",
        "{ edit = { aiTypeList = { num = 2; item[0] = { aiType = \"4\"; entityDef = \"ai/imp\"; } "
        "item[1] = { aiType = \"1048576\"; entityDef = \"ai/spider\"; } } } }");
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    if (!sources) goto done;
    compiled = sh_package_compile(sources, baseline, NULL, error, sizeof(error));
    if (!compiled) fprintf(stderr, "%s\n", error);
    CHECK(compiled);
    if (!compiled) goto done;
    CHECK(compiled->resource_count == 5); CHECK(compiled->duplicate_count == 1); CHECK(compiled->composed_count == 3);
    resource = sh_package_compilation_find(compiled, editor);
    CHECK(resource && resource->composed && resource->owners.bits == 3 && resource->gameplay_owners.bits == 0);
    CHECK(resource && strstr((const char *)resource->body, "num = 3;"));
    CHECK(resource && strstr((const char *)resource->body, "allowClimb"));
    CHECK(resource && strstr((const char *)resource->body, "receiveDecals"));
    resource = sh_package_compilation_find(compiled, inspector);
    CHECK(resource && resource->composed && resource->owners.bits == 3 && resource->source_count == 2);
    CHECK(resource && strstr((const char *)resource->body, "num = 3;"));
    resource = sh_package_compilation_find(compiled, conductor);
    CHECK(resource && resource->composed && resource->gameplay_owners.bits == 3);
    CHECK(resource && strstr((const char *)resource->body, "num = 3;"));
    CHECK(resource && strstr((const char *)resource->body, "ai/imp"));
    CHECK(resource && strstr((const char *)resource->body, "ai/cyber"));
    CHECK(resource && strstr((const char *)resource->body, "ai/spider"));
    resource = sh_package_compilation_find(compiled, "generated/image/shared.bimage");
    CHECK(resource && resource->owners.bits == 3 && resource->source_count == 2 && !resource->composed);
    resource = sh_package_compilation_find(compiled, "cooked/model/replaced.bmodel");
    CHECK(resource && resource->owners.bits == 2 && !resource->composed);
    if (resource) {
        unsigned char *body; size_t length;
        body = sh_package_compilation_read(compiled, resource, 1024, &length, error, sizeof(error));
        CHECK(body && !strcmp((const char *)body, "modified model")); free(body);
    }
    test_changes(compiled);
    test_restoration(compiled);
    test_prune_native();
    test_prune_native_names();
    test_builtin_composition(compiled);
    test_editor_wiring_collections();
    sh_package_compilation_free(compiled); compiled = NULL;
    compiled = sh_package_compile(sources, NULL, NULL, error, sizeof(error)); CHECK(!compiled); CHECK(strstr(error, "verified original"));
    sh_package_sources_free(sources); sources = NULL;
    create("overrides/b/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl",
        "{ edit = { aiTypeList = { num = 2; item[0] = { aiType = \"4\"; entityDef = \"ai/imp\"; } "
        "item[1] = { aiType = \"524288\"; entityDef = \"ai/spider\"; } } } }");
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    compiled = sh_package_compile(sources, baseline, NULL, error, sizeof(error));
    CHECK(!compiled); CHECK(strstr(error, "aiTypeList")); CHECK(strstr(error, "[a; b]"));
    sh_package_compilation_free(compiled); compiled = NULL;
    sh_package_sources_free(sources); sources = NULL;
    create("overrides/b/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl", conductor_base);
    create("overrides/a/assets/cooked/model/replaced.bmodel", "different model");
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    compiled = sh_package_compile(sources, baseline, NULL, error, sizeof(error));
    CHECK(!compiled); CHECK(strstr(error, "opaque replacements")); CHECK(strstr(error, "[a; b]"));
done:
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    test_new_declarations(); test_native_original_provenance(); test_duplicate_editor_controls();
    test_requirement_reduction(); test_editor_rebase(); test_editor_native_restatement(); test_editor_native_form(); test_editor_frame_bound();
    test_editor_first_slot(); test_editor_first_slot_encoding(); test_editor_reset_markers(); test_editor_header_repeats();
    test_editor_family_normalization();
    test_editor_semantic_control_key(); test_editor_model_constructor_state();
    test_editor_slot_names(); test_encounter_membership_order(); cleanup();
    if (failures) return 1;
    puts("package compiler checks passed"); return 0;
}
