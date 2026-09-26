/* Synthetic map source contexts and saved native bindings. */
#include "package_provenance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failed++; } } while (0)

static void fixture(sh_package_provenance *out, int reverse)
{
    unsigned char trees[3][32] = {{1}, {2}, {1}}, isolated[1][32] = {{3}};
    size_t old, fresh, peers[3];
    if (reverse) {
        fresh = sh_package_provenance_context_add(out, SH_PACKAGE_CONTEXT_ISOLATED, isolated, 1, "actors");
        old = sh_package_provenance_context_add(out, SH_PACKAGE_CONTEXT_COMPOSED, trees, 3, NULL);
    } else {
        old = sh_package_provenance_context_add(out, SH_PACKAGE_CONTEXT_COMPOSED, trees, 3, NULL);
        fresh = sh_package_provenance_context_add(out, SH_PACKAGE_CONTEXT_ISOLATED, isolated, 1, "actors");
    }
    CHECK(old != SIZE_MAX && fresh != SIZE_MAX && old != fresh);
    CHECK(sh_package_provenance_context_add(out, SH_PACKAGE_CONTEXT_COMPOSED, trees, 2, NULL) == old);
    peers[0] = reverse ? fresh : old; peers[1] = reverse ? old : fresh; peers[2] = peers[0];
    CHECK(sh_package_provenance_bind(out, "entitydef", "actors/test", "smp/green", &fresh, 1));
    CHECK(sh_package_provenance_bind(out, "entitydef", "actors/test", "actors/test", &old, 1));
    CHECK(sh_package_provenance_bind(out, "material", "surfaces/shared", "smp/shared", peers, 3));
    CHECK(sh_package_provenance_bind(out, "material", "surfaces/shared", "smp/shared", peers, 3));
    CHECK(out->context_count == 2 && out->binding_count == 3);
}

static void roundtrip(void)
{
    sh_package_provenance value = {0}, reversed = {0}, decoded = {0}, empty = {0};
    unsigned char *body, *other, *again; size_t length, other_length, n; char error[256];
    fixture(&value, 0); fixture(&reversed, 1);
    body = sh_package_provenance_encode(&value, &length, error, sizeof(error));
    other = sh_package_provenance_encode(&reversed, &other_length, error, sizeof(error));
    CHECK(body && other && length == other_length && !memcmp(body, other, length));
    CHECK(sh_package_provenance_decode(body, length, &decoded, error, sizeof(error)));
    again = sh_package_provenance_encode(&decoded, &n, error, sizeof(error));
    CHECK(again && n == length && !memcmp(again, body, n)); free(again);
    CHECK(decoded.context_count == 2 && decoded.binding_count == 3);
    CHECK(decoded.semantics == SH_PACKAGE_PROVENANCE_HISTORICAL && !decoded.use_count);
    CHECK(decoded.contexts[0].mode == SH_PACKAGE_CONTEXT_ISOLATED);
    CHECK(decoded.contexts[1].mode == SH_PACKAGE_CONTEXT_COMPOSED && decoded.contexts[1].tree_count == 2);
    CHECK(decoded.bindings[2].context_count == 2); /* one shared object, two supplying contexts */
    /* Every truncation rejects atomically, including a context-complete prefix. */
    for (size_t i = 0; i < length; i++) {
        CHECK(!sh_package_provenance_decode(body, i, &decoded, error, sizeof(error)));
        CHECK(!decoded.context_count && !decoded.binding_count && !decoded.contexts && !decoded.bindings);
    }
    CHECK(sh_package_provenance_decode(body, length, &decoded, error, sizeof(error)));
    again = malloc(length + 1); memcpy(again, body, length); again[length] = 0;
    CHECK(!sh_package_provenance_decode(again, length + 1, &decoded, error, sizeof(error))); free(again);
    /* Counts cannot amplify a tiny hostile payload into huge allocations. */
    memset(body + 8, 255, 4);
    CHECK(!sh_package_provenance_decode(body, length, &decoded, error, sizeof(error)));
    free(body); free(other);
    body = sh_package_provenance_encode(&empty, &length, error, sizeof(error));
    CHECK(body && sh_package_provenance_decode(body, length, &decoded, error, sizeof(error)));
    CHECK(!decoded.context_count && !decoded.binding_count); free(body);
    sh_package_provenance_free(&value); sh_package_provenance_free(&reversed); sh_package_provenance_free(&decoded);
}

static void invalid(void)
{
    sh_package_provenance value = {0}; unsigned char tree[1][32] = {{1}};
    size_t key, bad = 4; char error[256]; size_t length = 44;
    CHECK(sh_package_provenance_context_add(&value, 99, tree, 1, "") == SIZE_MAX);
    CHECK(sh_package_provenance_context_add(&value, SH_PACKAGE_CONTEXT_ISOLATED, tree, 1, "../other") == SIZE_MAX);
    CHECK(sh_package_provenance_context_add(&value, SH_PACKAGE_CONTEXT_ISOLATED, tree, 1, "a//b") == SIZE_MAX);
    CHECK(sh_package_provenance_context_add(&value, SH_PACKAGE_CONTEXT_ISOLATED, tree, 1, "a/") == SIZE_MAX);
    CHECK(sh_package_provenance_context_add(&value, SH_PACKAGE_CONTEXT_COMPOSED, tree, 1, "") == SIZE_MAX);
    CHECK(!value.context_count);
    key = sh_package_provenance_context_add(&value, SH_PACKAGE_CONTEXT_ISOLATED, tree, 1, ""); CHECK(key == 0);
    CHECK(sh_package_provenance_bind(&value, "entitydef", "actor", "alias", &key, 1));
    CHECK(!sh_package_provenance_bind(&value, "entitydef", "different", "alias", &key, 1));
    CHECK(!sh_package_provenance_bind(&value, "entitydef", "actor", "alias", &bad, 1));
    CHECK(!sh_package_provenance_bind(&value, "entitydef", "actor", "bad\nname", &key, 1));
    CHECK(value.binding_count == 1 && value.bindings[0].context_count == 1);
    value.bindings[0].contexts[0] = 99;
    CHECK(!sh_package_provenance_encode(&value, &length, error, sizeof(error)) && !length);
    sh_package_provenance_free(&value);
}

static void scopes(void)
{
    sh_package_provenance value = {0}, decoded = {0}; sh_package_scope view = {0};
    sh_package packages[4] = {0}; unsigned char trees[4][32] = {{1}, {2}, {3}, {3}};
    sh_package_component components[4] = {0}; sh_package_source_file files[4] = {0};
    sh_package_sources sources = {packages, 4, components, 4, files, 4, trees};
    size_t length, old = SIZE_MAX, fresh = SIZE_MAX; char error[256]; unsigned char *body;
    for (size_t i = 0; i < 4; i++) {
        components[i].owner = i; components[i].relative = i >= 2 ? "actors" : "";
        files[i].owner = i; files[i].component = i; files[i].engine_path = "decls/entitydef/actor.decl";
        strcpy(packages[i].name, "same-descriptor-id");
    }
    fixture(&value, 0); body = sh_package_provenance_encode(&value, &length, error, sizeof(error));
    CHECK(body && sh_package_provenance_decode(body, length, &decoded, error, sizeof(error))); free(body);
    for (size_t i = 0; i < decoded.context_count; i++)
        if (decoded.contexts[i].mode == SH_PACKAGE_CONTEXT_COMPOSED) old = i; else fresh = i;
    CHECK(sh_package_provenance_scope(&decoded, old, &sources, &view, error, sizeof(error)));
    CHECK(view.sources.files[0].engine_path && view.sources.files[1].engine_path);
    CHECK(!view.sources.files[2].engine_path && !view.sources.files[3].engine_path);
    CHECK(sh_package_provenance_scope(&decoded, fresh, &sources, &view, error, sizeof(error)));
    CHECK(!view.sources.files[0].engine_path && !view.sources.files[1].engine_path);
    CHECK(view.sources.files[2].engine_path && !view.sources.files[3].engine_path);
    /* A changed same-ID package cannot replace the absent map source tree. */
    trees[0][0] = 9;
    CHECK(!sh_package_provenance_scope(&decoded, old, &sources, &view, error, sizeof(error)));
    CHECK(!view.sources.files && !view.sources.components);
    trees[2][0] = 8;
    CHECK(sh_package_provenance_scope(&decoded, fresh, &sources, &view, error, sizeof(error)));
    CHECK(!view.sources.files[2].engine_path && view.sources.files[3].engine_path);
    trees[3][0] = 8;
    CHECK(!sh_package_provenance_scope(&decoded, fresh, &sources, &view, error, sizeof(error)));
    CHECK(!view.sources.files);
    sh_package_scope_close(&view); sh_package_provenance_free(&value); sh_package_provenance_free(&decoded);
}

static int choose_name(void *context, const char *family, const char *name)
{ (void)family; return !strcmp(context, name); }
static void providers(void)
{
    sh_package_provenance local = {0}, map = {0}, view = {0}, selected = {0}, rebuilt = {0};
    unsigned char trees[2][32] = {{1}, {2}}; const char *replacement = NULL;
    size_t l = sh_package_provenance_context_add(&local, SH_PACKAGE_CONTEXT_ISOLATED, trees, 1, "");
    size_t m = sh_package_provenance_context_add(&map, SH_PACKAGE_CONTEXT_ISOLATED, trees + 1, 1, "");
    CHECK(sh_package_provenance_bind(&local, "entitydef", "actor", "smp/equal", &l, 1));
    CHECK(sh_package_provenance_bind(&local, "entitydef", "other", "smp/unused", &l, 1));
    CHECK(sh_package_provenance_bind(&map, "entitydef", "actor", "smp/equal", &m, 1));
    CHECK(sh_package_provenance_overlay(&local, &map, &view));
    CHECK(sh_package_provenance_select(&view, choose_name, "smp/equal", &selected));
    CHECK(selected.binding_count == 1 && selected.context_count == 1 && selected.contexts[0].trees[0][0] == 2);
    size_t r = sh_package_provenance_context_add(&rebuilt, SH_PACKAGE_CONTEXT_ISOLATED, trees + 1, 1, "");
    CHECK(sh_package_provenance_bind(&rebuilt, "entitydef", "actor", "smp/new-name", &r, 1));
    CHECK(sh_package_provenance_resolve(&selected, &rebuilt, "entitydef", "smp/equal", &replacement) == 1);
    CHECK(!strcmp(replacement, "smp/new-name"));
    CHECK(sh_package_provenance_resolve(&selected, &local, "entitydef", "smp/equal", &replacement) == -1 && !replacement);
    CHECK(sh_package_provenance_resolve(&selected, &rebuilt, "entitydef", "stock/actor", &replacement) == 0 && !replacement);
    /* Equal bytes from a different local grouping cannot satisfy map provenance. */
    CHECK(sh_package_provenance_bind(&rebuilt, "entitydef", "actor", "smp/inconsistent", &r, 1));
    CHECK(sh_package_provenance_resolve(&selected, &rebuilt, "entitydef", "smp/equal", &replacement) == -1 && !replacement);
    CHECK(sh_package_provenance_select(&view, choose_name, "unused", &selected));
    CHECK(!selected.binding_count && !selected.context_count);
    sh_package_provenance_free(&local); sh_package_provenance_free(&map); sh_package_provenance_free(&view);
    sh_package_provenance_free(&selected); sh_package_provenance_free(&rebuilt);
    /* A map provider serving a source the local library also serves, under
     * another name: a saved use of that source means the map's binding, and
     * the local binding keeps its other suppliers. */
    {
        sh_package_provenance saved = {0}, twin = {0};
        unsigned char both[2][32] = {{5}, {6}};
        size_t a = sh_package_provenance_context_add(&local, SH_PACKAGE_CONTEXT_ISOLATED, both, 1, "");
        size_t b = sh_package_provenance_context_add(&local, SH_PACKAGE_CONTEXT_ISOLATED, both + 1, 1, "");
        size_t pair[2] = {a, b};
        CHECK(sh_package_provenance_bind(&local, "entitydef", "placed", "smp/local", pair, 2));
        size_t served = sh_package_provenance_context_add(&map, SH_PACKAGE_CONTEXT_ISOLATED, both + 1, 1, "");
        CHECK(sh_package_provenance_bind(&map, "entitydef", "placed", "smp/map", &served, 1));
        CHECK(sh_package_provenance_overlay(&local, &map, &view));
        size_t used = sh_package_provenance_context_add(&saved, SH_PACKAGE_CONTEXT_ISOLATED, both + 1, 1, "");
        CHECK(sh_package_provenance_bind(&saved, "entitydef", "placed", "smp/local", &used, 1));
        CHECK(sh_package_provenance_resolve(&saved, &view, "entitydef", "smp/local", &replacement) == 1 &&
            !strcmp(replacement, "smp/map"));
        used = sh_package_provenance_context_add(&twin, SH_PACKAGE_CONTEXT_ISOLATED, both, 1, "");
        CHECK(sh_package_provenance_bind(&twin, "entitydef", "placed", "smp/local", &used, 1));
        CHECK(sh_package_provenance_resolve(&twin, &view, "entitydef", "smp/local", &replacement) == 1 &&
            !strcmp(replacement, "smp/local"));
        sh_package_provenance_free(&local); sh_package_provenance_free(&map); sh_package_provenance_free(&view);
        sh_package_provenance_free(&saved); sh_package_provenance_free(&twin);
    }
    /* Nested components of one tree serving one native name: the map serves
     * only the north component, so a saved use of north means the map's
     * binding, south keeps its own local binding, and neither can select the
     * other component's. */
    {
        sh_package_provenance saved = {0}, other = {0};
        unsigned char tree[1][32] = {{7}};
        size_t north = sh_package_provenance_context_add(&local, SH_PACKAGE_CONTEXT_ISOLATED, tree, 1, "north");
        size_t south = sh_package_provenance_context_add(&local, SH_PACKAGE_CONTEXT_ISOLATED, tree, 1, "south");
        CHECK(sh_package_provenance_bind(&local, "entitydef", "nested", "smp/north-local", &north, 1));
        CHECK(sh_package_provenance_bind(&local, "entitydef", "nested", "smp/south-local", &south, 1));
        CHECK(sh_package_provenance_bind(&local, "entitydef", "unused", "smp/unused-local", &north, 1));
        size_t served = sh_package_provenance_context_add(&map, SH_PACKAGE_CONTEXT_ISOLATED, tree, 1, "north");
        CHECK(sh_package_provenance_bind(&map, "entitydef", "nested", "smp/north-map", &served, 1));
        CHECK(sh_package_provenance_overlay(&local, &map, &view));
        {
            /* The map serves north's "nested" only: north's other resources
             * stay with their local bindings. */
            sh_package_provenance kept = {0};
            size_t context = sh_package_provenance_context_add(&kept, SH_PACKAGE_CONTEXT_ISOLATED, tree, 1, "north");
            CHECK(sh_package_provenance_bind(&kept, "entitydef", "unused", "smp/unused-local", &context, 1));
            CHECK(sh_package_provenance_resolve(&kept, &view, "entitydef", "smp/unused-local", &replacement) == 1 &&
                !strcmp(replacement, "smp/unused-local"));
            sh_package_provenance_free(&kept);
        }
        size_t used = sh_package_provenance_context_add(&saved, SH_PACKAGE_CONTEXT_ISOLATED, tree, 1, "north");
        CHECK(sh_package_provenance_bind(&saved, "entitydef", "nested", "smp/north-local", &used, 1));
        CHECK(sh_package_provenance_resolve(&saved, &view, "entitydef", "smp/north-local", &replacement) == 1 &&
            !strcmp(replacement, "smp/north-map"));
        used = sh_package_provenance_context_add(&other, SH_PACKAGE_CONTEXT_ISOLATED, tree, 1, "south");
        CHECK(sh_package_provenance_bind(&other, "entitydef", "nested", "smp/south-local", &used, 1));
        CHECK(sh_package_provenance_resolve(&other, &view, "entitydef", "smp/south-local", &replacement) == 1 &&
            !strcmp(replacement, "smp/south-local"));
        /* The map binding never claims the other component's context. */
        size_t found = SIZE_MAX;
        for (size_t i = 0; i < view.binding_count; i++) if (!strcmp(view.bindings[i].name, "smp/north-map")) found = i;
        CHECK(found != SIZE_MAX && view.bindings[found].context_count == 1 &&
            !strcmp(view.contexts[view.bindings[found].contexts[0]].component, "north"));
        sh_package_provenance_free(&local); sh_package_provenance_free(&map); sh_package_provenance_free(&view);
        sh_package_provenance_free(&saved); sh_package_provenance_free(&other);
    }
}

static size_t binding(const sh_package_provenance *value, const char *family, const char *name)
{
    for (size_t i = 0; i < value->binding_count; i++)
        if (!strcmp(value->bindings[i].family, family) && !strcmp(value->bindings[i].name, name)) return i;
    return SIZE_MAX;
}
static void occurrences(sh_package_provenance *out, int reverse)
{
    unsigned char trees[3][32] = {{11}, {22}, {33}};
    size_t contexts[3], shared[3];
    for (size_t i = 0; i < 3; i++) {
        size_t j = reverse ? 2 - i : i;
        contexts[j] = sh_package_provenance_context_add(out, SH_PACKAGE_CONTEXT_ISOLATED, trees + j, 1, "actors/nested");
    }
    memcpy(shared, contexts, sizeof(shared));
    CHECK(sh_package_provenance_bind(out, "entitydef", "actor", "smp/equal", shared, 3));
    CHECK(sh_package_provenance_bind(out, "material", "sky", "smp/sky", shared, 3));
    for (size_t i = 0; i < 2; i++) {
        size_t j = reverse ? 1 - i : i;
        sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, j ? "owner/b" : "owner/a", "entityDef"};
        CHECK(sh_package_provenance_use_set(out, &site, 0, contexts + j, 1));
    }
    sh_package_provenance_site world = {SH_PACKAGE_USE_WORLD, "world/environment", "globalLighting.material"};
    CHECK(sh_package_provenance_use_set(out, &world, 1, contexts + 1, 1));
    sh_package_provenance_site registry = {SH_PACKAGE_USE_WORLD, "world/registration", "requiredActors[0]"};
    CHECK(sh_package_provenance_use_set(out, &registry, 0, contexts, 2));
}
typedef struct pick { const char *owner, *replacement; int objects, fail; } pick;
static int select_occurrence(void *opaque, const sh_package_provenance_use *use, sh_package_provenance_site *site)
{
    pick *selection = opaque;
    if (selection->fail) return -1;
    if (selection->objects && use->kind != SH_PACKAGE_USE_OBJECT) return 0;
    if (selection->owner && strcmp(selection->owner, use->owner)) return 0;
    if (selection->replacement) site->owner = selection->replacement;
    return 1;
}
static int omit_uses(void *opaque, const sh_package_provenance_use *use, sh_package_provenance_site *site)
{ (void)opaque; (void)use; (void)site; return 0; }
static void per_use(void)
{
    sh_package_provenance source = {0}, reverse = {0}, copy = {0}, read = {0}, current = {0};
    size_t n, m; char error[256]; unsigned char *body, *other;
    occurrences(&source, 0); occurrences(&reverse, 1);
    CHECK(source.use_count == 4 && source.binding_count == 2 && source.context_count == 3);
    body = sh_package_provenance_encode(&source, &n, error, sizeof(error));
    other = sh_package_provenance_encode(&reverse, &m, error, sizeof(error));
    CHECK(body && other && n == m && !memcmp(body, other, n)); free(other);
    CHECK(body && body[7] == 1 && sh_package_provenance_decode(body, n, &read, error, sizeof(error)));
    CHECK(read.semantics == SH_PACKAGE_PROVENANCE_USES && read.context_count == 2 && read.use_count == 4);
    CHECK(read.uses[0].contexts[0] != read.uses[1].contexts[0]);
    CHECK(read.uses[3].context_count == 2); /* actual shared registration contributors */
    other = sh_package_provenance_encode(&read, &m, error, sizeof(error));
    CHECK(other && n == m && !memcmp(body, other, n)); free(other);
    pick a = {"owner/a", "copied/a", 1, 0};
    CHECK(sh_package_provenance_select_uses(&source, select_occurrence, &a, &copy));
    CHECK(copy.use_count == 1 && copy.binding_count == 1 && copy.context_count == 1);
    CHECK(copy.contexts[0].trees[0][0] == 11 && !strcmp(copy.contexts[0].component, "actors/nested"));
    CHECK(!strcmp(copy.uses[0].owner, "copied/a") && !strcmp(copy.uses[0].reference, "entityDef"));
    CHECK(copy.bindings[0].context_count == 1);
    /* Resolution uses A alone even when the shared provider advertised B and C. */
    unsigned char tree[1][32] = {{11}};
    size_t ctx = sh_package_provenance_context_add(&current, SH_PACKAGE_CONTEXT_ISOLATED, tree, 1, "actors/nested");
    CHECK(sh_package_provenance_bind(&current, "entitydef", "actor", "smp/recompiled", &ctx, 1));
    const char *replacement = NULL;
    CHECK(sh_package_provenance_resolve_use(&source, 0, &current, &replacement) == 1 && !strcmp(replacement, "smp/recompiled"));
    CHECK(sh_package_provenance_resolve_use(&source, 1, &current, &replacement) == -1 && !replacement);
    CHECK(sh_package_provenance_resolve(&copy, &current, "entitydef", "smp/equal", &replacement) == 1);
    CHECK(sh_package_provenance_resolve(&source, &current, "entitydef", "smp/equal", &replacement) == -1);
    sh_package_provenance_site selected_a = {SH_PACKAGE_USE_OBJECT, "owner/a", "entityDef"};
    sh_package_provenance_site selected_b = {SH_PACKAGE_USE_OBJECT, "owner/b", "entityDef"};
    CHECK(sh_package_provenance_resolve_site(&source, &current, &selected_a,
        "entitydef", "smp/equal", &replacement) == 1 && !strcmp(replacement, "smp/recompiled"));
    CHECK(sh_package_provenance_resolve_site(&source, &current, &selected_b,
        "entitydef", "smp/equal", &replacement) == -1 && !replacement);
    CHECK(sh_package_provenance_resolve_site(&source, &current, &selected_a,
        "entitydef", "edited/selection", &replacement) == 0 && !replacement);
    selected_a.reference = "different/field";
    CHECK(sh_package_provenance_resolve_site(&source, &current, &selected_a,
        "entitydef", "smp/equal", &replacement) == 0 && !replacement);
    CHECK(sh_package_provenance_resolve_site(&source, &current, NULL,
        "entitydef", "smp/equal", &replacement) == 0 && !replacement);
    /* Remapping two actual owners onto one typed site cannot silently pick one. */
    pick collision = {NULL, "copied/shared", 1, 0};
    CHECK(!sh_package_provenance_select_uses(&source, select_occurrence, &collision, &copy));
    CHECK(!copy.uses && !copy.bindings && !copy.contexts);
    pick both = {NULL, NULL, 1, 0};
    CHECK(sh_package_provenance_select_uses(&source, select_occurrence, &both, &copy));
    CHECK(copy.use_count == 2 && copy.binding_count == 1 && copy.context_count == 2);
    /* A typed-reference edit replaces one owner choice; it never changes B. */
    sh_package_provenance_site edit = {SH_PACKAGE_USE_OBJECT, "owner/a", "entityDef"};
    size_t b = copy.uses[1].contexts[0];
    CHECK(sh_package_provenance_use_set(&copy, &edit, 0, &b, 1));
    other = sh_package_provenance_encode(&copy, &m, error, sizeof(error));
    CHECK(other && sh_package_provenance_decode(other, m, &read, error, sizeof(error))); free(other);
    CHECK(read.use_count == 2 && read.context_count == 1 && read.contexts[0].trees[0][0] == 22);
    CHECK(sh_package_provenance_select_uses(&copy, omit_uses, NULL, &read));
    CHECK(read.semantics == SH_PACKAGE_PROVENANCE_USES && !read.use_count && !read.context_count && !read.binding_count);
    other = sh_package_provenance_encode(&read, &m, error, sizeof(error));
    CHECK(other && other[7] == 1 && sh_package_provenance_decode(other, m, &copy, error, sizeof(error))); free(other);
    CHECK(copy.semantics == SH_PACKAGE_PROVENANCE_USES && !copy.binding_count);
    /* All prefixes and an out-of-range use context reject atomically. */
    for (size_t i = 0; i < n; i++) {
        CHECK(!sh_package_provenance_decode(body, i, &read, error, sizeof(error)));
        CHECK(!read.uses && !read.bindings && !read.contexts);
    }
    memset(body + n - 4, 255, 4);
    CHECK(!sh_package_provenance_decode(body, n, &read, error, sizeof(error))); free(body);
    body = sh_package_provenance_encode(&source, &n, error, sizeof(error));
    /* Two encoded owners targeting one site with different suppliers refuse. */
    if (body) for (size_t i = 0; i + 7 <= n; i++) if (!memcmp(body + i, "owner/b", 7)) { body[i + 6] = 'a'; break; }
    CHECK(!sh_package_provenance_decode(body, n, &read, error, sizeof(error))); free(body);
    CHECK(sh_package_provenance_select_uses(&source, select_occurrence, &a, &copy));
    sh_package_provenance_free(&source); /* metadata copies survive; source files require external retention */
    CHECK(copy.contexts[0].trees[0][0] == 11 && !strcmp(copy.bindings[0].name, "smp/equal"));
    sh_package_provenance_free(&reverse); sh_package_provenance_free(&copy);
    sh_package_provenance_free(&read); sh_package_provenance_free(&current);
}
static void historical_uses(void)
{
    sh_package_provenance old = {0}, decoded = {0}, mixed = {0}, selected = {0};
    unsigned char trees[2][32] = {{71}, {72}}, fresh[1][32] = {{73}};
    size_t legacy = sh_package_provenance_context_add(&old, SH_PACKAGE_CONTEXT_COMPOSED, trees, 2, NULL);
    size_t length; char error[256]; unsigned char *body;
    CHECK(sh_package_provenance_bind(&old, "entitydef", "actor", "actor", &legacy, 1));
    /* A provider union has no proof of any occurrence. */
    CHECK(!sh_package_provenance_select_uses(&old, NULL, NULL, &mixed));
    body = sh_package_provenance_encode(&old, &length, error, sizeof(error));
    CHECK(body && body[7] == 0 && sh_package_provenance_decode(body, length, &decoded, error, sizeof(error))); free(body);
    CHECK(decoded.semantics == SH_PACKAGE_PROVENANCE_HISTORICAL && !decoded.use_count);
    sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, "new/entity", "entityDef"};
    CHECK(!sh_package_provenance_use_set(&decoded, &site, 0, &legacy, 1));
    CHECK(sh_package_provenance_select_uses(&decoded, NULL, NULL, &mixed));
    CHECK(mixed.use_count == 1 && mixed.uses[0].kind == SH_PACKAGE_USE_HISTORICAL && !*mixed.uses[0].owner);
    size_t modern = sh_package_provenance_context_add(&mixed, SH_PACKAGE_CONTEXT_ISOLATED, fresh, 1, "");
    CHECK(sh_package_provenance_bind(&mixed, "entitydef", "new", "smp/new", &modern, 1));
    CHECK(sh_package_provenance_use_set(&mixed, &site, binding(&mixed, "entitydef", "smp/new"), &modern, 1));
    body = sh_package_provenance_encode(&mixed, &length, error, sizeof(error));
    CHECK(body && sh_package_provenance_decode(body, length, &decoded, error, sizeof(error))); free(body);
    CHECK(decoded.use_count == 2 && decoded.uses[1].kind == SH_PACKAGE_USE_HISTORICAL);
    CHECK(decoded.contexts[decoded.uses[1].contexts[0]].mode == SH_PACKAGE_CONTEXT_COMPOSED);
    CHECK(decoded.contexts[decoded.uses[1].contexts[0]].tree_count == 2);
    pick only_new = {"new/entity", NULL, 1, 0};
    CHECK(sh_package_provenance_select_uses(&decoded, select_occurrence, &only_new, &selected));
    CHECK(selected.context_count == 1 && selected.contexts[0].trees[0][0] == 73);
    sh_package_provenance_free(&old); sh_package_provenance_free(&decoded);
    sh_package_provenance_free(&mixed); sh_package_provenance_free(&selected);
}
static void invalid_uses(void)
{
    sh_package_provenance value = {0}, output = {0}; unsigned char trees[2][32] = {{1}, {2}};
    size_t a = sh_package_provenance_context_add(&value, SH_PACKAGE_CONTEXT_ISOLATED, trees, 1, "");
    size_t b = sh_package_provenance_context_add(&value, SH_PACKAGE_CONTEXT_ISOLATED, trees + 1, 1, "");
    sh_package_provenance_site good = {SH_PACKAGE_USE_OBJECT, "owner", "edit.model"};
    CHECK(sh_package_provenance_bind(&value, "model", "mesh", "smp/mesh", &a, 1));
    CHECK(sh_package_provenance_use_set(&value, &good, 0, &a, 1));
    CHECK(!sh_package_provenance_use_set(&value, &good, 0, &b, 1));
    CHECK(!sh_package_provenance_use_set(&value, &good, 1, &a, 1));
    sh_package_provenance_site bad = {SH_PACKAGE_USE_WORLD, "", "lighting"};
    CHECK(!sh_package_provenance_use_set(&value, &bad, 0, &a, 1));
    bad = (sh_package_provenance_site){SH_PACKAGE_USE_OBJECT, "owner", "edit.\nmodel"};
    CHECK(!sh_package_provenance_use_set(&value, &bad, 0, &a, 1));
    CHECK(value.use_count == 1 && value.uses[0].contexts[0] == a);
    pick failed_selection = {NULL, NULL, 0, 1};
    CHECK(!sh_package_provenance_select_uses(&value, select_occurrence, &failed_selection, &output));
    CHECK(!output.contexts && !output.bindings && !output.uses);
    sh_package_provenance_free(&value); sh_package_provenance_free(&output);
}
static void typed_sites(void)
{
    sh_package_provenance value = {0}, read = {0}, copy = {0};
    unsigned char trees[2][32] = {{1}, {2}}; size_t contexts[2], length; char error[256];
    for (size_t i = 0; i < 2; i++) contexts[i] = sh_package_provenance_context_add(&value,
        SH_PACKAGE_CONTEXT_ISOLATED, trees + i, 1, "");
    CHECK(sh_package_provenance_bind(&value, "entitydef", "actor", "smp/equal", contexts, 2));
    sh_package_provenance_site root = {SH_PACKAGE_USE_OBJECT, "owner", "entityDef"};
    sh_package_provenance_site child = {SH_PACKAGE_USE_OBJECT, "owner", "edit.spawnEntity"};
    sh_package_provenance_site world = {SH_PACKAGE_USE_WORLD, "owner", "entityDef"};
    CHECK(sh_package_provenance_use_set(&value, &root, 0, contexts, 1));
    CHECK(sh_package_provenance_use_set(&value, &child, 0, contexts + 1, 1));
    CHECK(sh_package_provenance_use_set(&value, &world, 0, contexts + 1, 1));
    CHECK(value.use_count == 3); /* same spelling is distinct by owner kind and typed site */
    unsigned char *body = sh_package_provenance_encode(&value, &length, error, sizeof(error));
    CHECK(body && sh_package_provenance_decode(body, length, &read, error, sizeof(error))); free(body);
    CHECK(read.use_count == 3 && read.context_count == 2 && read.binding_count == 1);
    pick objects = {"owner", "new/owner", 1, 0};
    CHECK(sh_package_provenance_select_uses(&read, select_occurrence, &objects, &copy));
    CHECK(copy.use_count == 2 && copy.context_count == 2 && !strcmp(copy.uses[0].owner, "new/owner"));
    CHECK(strcmp(copy.uses[0].reference, copy.uses[1].reference));
    sh_package_provenance_free(&value); sh_package_provenance_free(&read); sh_package_provenance_free(&copy);
}

int main(void)
{
    roundtrip(); invalid(); scopes(); providers(); per_use(); historical_uses(); invalid_uses(); typed_sites();
    printf("package_provenance_test: %s\n", failed ? "FAILED" : "PASS"); return failed ? 1 : 0;
}
