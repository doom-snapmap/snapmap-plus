/* R-398 perf17: the multiplayer snapshot-template list keeps only the package rows the served view reaches.
 * Reach follows "smp/<64 hex>" names through served declaration text (every family a name carries) from
 * the rows with engine names and the rows the current map delivers; the filter clears the unreached private
 * entries of the native list (both native loops skip NULL, and the count stays the live count), only for the
 * two template call sites and the entityDef list, and leaves the list native when the view is busy or the
 * switch is off. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snapshot_templates.h"
#include "package_compiler.h"

static int g_failures, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char g_last_log[512];
static int g_logs;
void backend_log(const char *message) { snprintf(g_last_log, sizeof(g_last_log), "%s", message); g_logs++; }

static sh_package_compilation g_compiled;
static int g_busy, g_acquires, g_releases;
const sh_package_compilation *sh_package_runtime_try_acquire(int *held)
{
    g_acquires++;
    *held = !g_busy;
    return *held ? &g_compiled : NULL;
}
void sh_package_runtime_release(void) { g_releases++; }
unsigned char *sh_package_compilation_read_served(const sh_package_compilation *compilation,
    const sh_compiled_resource *resource, size_t limit, size_t *length, char *error, size_t error_capacity)
{ (void)compilation; (void)resource; (void)limit; (void)length; (void)error; (void)error_capacity; return NULL; }

static char g_names[16][80];
static const char *name_of(char letter)
{
    char *out = g_names[letter - 'A'];
    strcpy(out, "smp/");
    for (int i = 0; i < 64; i++) out[4 + i] = "0123456789abcdef"[(letter * 7 + i) % 16];
    out[68] = 0;
    return out;
}
static char g_bodies[16][400];
static void resource(sh_compiled_resource *r, const char *type, const char *name, const char *body, int body_slot)
{
    memset(r, 0, sizeof(*r));
    r->type = (char *)type; r->name = (char *)name;
    snprintf(g_bodies[body_slot], sizeof(g_bodies[body_slot]), "%s", body);
    r->body = (unsigned char *)g_bodies[body_slot]; r->body_length = strlen(g_bodies[body_slot]);
}

static sh_compiled_resource g_rows[16];
static char g_presented[200];
static void build_view(void)
{
    char text[400];
    size_t n = 0;
    snprintf(text, sizeof(text), "{ edit = { entityDef = \"%s\"; } }", name_of('A'));
    resource(&g_rows[n], "entitydef", "ai/demon/stock", text, (int)n); n++;
    snprintf(text, sizeof(text), "{ a = \"%s\"; b = \"generated/decls/entitydef/%s.decl\"; }", name_of('B'), name_of('F'));
    resource(&g_rows[n], "entitydef", name_of('A'), text, (int)n); n++;
    resource(&g_rows[n], "snapeditorentitydef", name_of('B'), "{ }", (int)n); n++;
    snprintf(text, sizeof(text), "{ x = \"%s\"; }", name_of('D'));
    resource(&g_rows[n], "entitydef", name_of('C'), text, (int)n); n++;              /* unreached: C, D */
    resource(&g_rows[n], "entitydef", name_of('D'), "{ }", (int)n); n++;
    snprintf(text, sizeof(text), "{ x = \"%s\"; }", name_of('G'));
    resource(&g_rows[n], "entitydef", name_of('E'), text, (int)n);                   /* map-delivered */
    sh_package_owners_add(&g_rows[n].owners, 3); n++;
    resource(&g_rows[n], "entitydef", name_of('G'), "{ }", (int)n); n++;
    resource(&g_rows[n], "entitydef", name_of('F'), "{ }", (int)n); n++;
    snprintf(text, sizeof(text), "%s", name_of('H'));
    resource(&g_rows[n], NULL, NULL, text, (int)n); n++;                             /* not a declaration */
    resource(&g_rows[n], "entitydef", name_of('H'), "{ }", (int)n); n++;
    snprintf(text, sizeof(text), "{ y = \"%s\"; }", name_of('I'));
    resource(&g_rows[n], "snapeditorentitydef", name_of('A'), text, (int)n); n++;    /* A's other family */
    resource(&g_rows[n], "entitydef", name_of('I'), "{ }", (int)n); n++;
    snprintf(text, sizeof(text), "{ z = \"%s\"; }", name_of('K'));
    resource(&g_rows[n], "entitydef", "x/presented", text, (int)n);
    snprintf(g_presented, sizeof(g_presented), "{ z = \"%s\"; }", name_of('J'));
    g_rows[n].presented = (unsigned char *)g_presented; g_rows[n].presented_length = strlen(g_presented); n++;
    resource(&g_rows[n], "entitydef", name_of('J'), "{ }", (int)n); n++;
    resource(&g_rows[n], "entitydef", name_of('K'), "{ }", (int)n); n++;
    memset(&g_compiled, 0, sizeof(g_compiled));
    g_compiled.resources = g_rows; g_compiled.resource_count = n;
    g_compiled.map_overlay = 1; g_compiled.map_owner_begin = 2;
}

static void names_are_private_only_when_exact(void)
{
    char upper[80];
    CHECK(sh_snapshot_templates_private_name(name_of('A')));
    CHECK(!sh_snapshot_templates_private_name("ai/demon/baron"));
    CHECK(!sh_snapshot_templates_private_name("smp/0123"));
    snprintf(upper, sizeof(upper), "%s", name_of('A')); upper[10] = 'A';
    CHECK(!sh_snapshot_templates_private_name(upper));
    snprintf(upper, sizeof(upper), "%sa", name_of('A'));
    CHECK(!sh_snapshot_templates_private_name(upper));
    CHECK(!sh_snapshot_templates_private_name(NULL));
}

static void reach_follows_served_text_from_engine_names_and_map_rows(void)
{
    sh_snapshot_reach reach;
    build_view();
    CHECK(sh_snapshot_reach_build(&g_compiled, &reach));
    CHECK(sh_snapshot_reach_contains(&reach, name_of('A')) && sh_snapshot_reach_contains(&reach, name_of('B')));
    CHECK(sh_snapshot_reach_contains(&reach, name_of('F')));                   /* a path spelling names it too */
    CHECK(sh_snapshot_reach_contains(&reach, name_of('E')) && sh_snapshot_reach_contains(&reach, name_of('G')));
    CHECK(sh_snapshot_reach_contains(&reach, name_of('I')));                   /* through A's other family */
    CHECK(sh_snapshot_reach_contains(&reach, name_of('J')) && !sh_snapshot_reach_contains(&reach, name_of('K')));
    CHECK(!sh_snapshot_reach_contains(&reach, name_of('C')) && !sh_snapshot_reach_contains(&reach, name_of('D')));
    CHECK(!sh_snapshot_reach_contains(&reach, name_of('H')));                  /* only a non-declaration names it */
    CHECK(reach.count == 7);
    sh_snapshot_reach_free(&reach);
    /* Without a map overlay, the map's own rows are not roots. */
    g_compiled.map_overlay = 0;
    CHECK(sh_snapshot_reach_build(&g_compiled, &reach));
    CHECK(!sh_snapshot_reach_contains(&reach, name_of('E')) && !sh_snapshot_reach_contains(&reach, name_of('G')));
    CHECK(sh_snapshot_reach_contains(&reach, name_of('A')));
    sh_snapshot_reach_free(&reach);
    CHECK(sh_snapshot_reach_build(NULL, &reach) && reach.count == 0);
}

/* A native idResource: vtable, then the name pointer at +8. */
typedef struct fake_resource { void *vtable; const char *name; } fake_resource;
static fake_resource g_res[8];
static void *g_list_entries[8];
static int g_list_count;
static int g_native_calls;
typedef struct fake_list { void **entries; int count, capacity; } fake_list;
static char g_entitydefs, g_other_list;
static void fake_list_all(void *list, void *out, char sort)
{
    fake_list *l = out; (void)list; (void)sort;
    g_native_calls++;
    for (int i = 0; i < g_list_count; i++) l->entries[i] = g_list_entries[i];
    l->count = g_list_count;
}
static void fill_list(void)
{
    static char unknown[80];
    g_res[0].name = "ai/demon/stock"; g_res[1].name = name_of('A'); g_res[2].name = name_of('C');
    snprintf(unknown, sizeof(unknown), "%s", name_of('L')); g_res[3].name = unknown;
    g_res[4].name = name_of('D'); g_res[5].name = name_of('E');
    g_list_entries[0] = &g_res[0]; g_list_entries[1] = &g_res[1]; g_list_entries[2] = &g_res[2];
    g_list_entries[3] = &g_res[3]; g_list_entries[4] = NULL; g_list_entries[5] = &g_res[4]; g_list_entries[6] = &g_res[5];
    g_list_count = 7;
}

static void the_filter_clears_unreached_private_entries_in_place(void)
{
    sh_snapshot_reach reach;
    void *entries[8];
    build_view(); fill_list();
    CHECK(sh_snapshot_reach_build(&g_compiled, &reach));
    memcpy(entries, g_list_entries, sizeof(entries));
    CHECK(sh_snapshot_templates_filter(entries, g_list_count, &reach) == 3);
    CHECK(entries[0] == &g_res[0] && entries[1] == &g_res[1] && entries[6] == &g_res[5]);
    CHECK(!entries[2] && !entries[3] && !entries[4] && !entries[5]);
    sh_snapshot_reach_free(&reach);
}

static void only_the_template_sites_on_the_entitydef_list_are_filtered(void)
{
    static const char first[1], second[1], elsewhere[1];
    void *entries[8];
    fake_list out = {entries, 0, 8};
    build_view(); fill_list();
    sh_snapshot_templates_test_reset();
    sh_snapshot_templates_test_bind(fake_list_all, &g_entitydefs, first, second);
    /* Another caller of get-all: native. */
    sh_snapshot_templates_test_call(&g_entitydefs, &out, 0, elsewhere);
    CHECK(out.count == 7 && entries[2] == &g_res[2]);
    /* Another list at the template site: native. */
    sh_snapshot_templates_test_call(&g_other_list, &out, 0, first);
    CHECK(entries[2] == &g_res[2]);
    /* The first site: filtered, the count stays the live count, the view released. */
    g_acquires = g_releases = 0;
    sh_snapshot_templates_test_call(&g_entitydefs, &out, 0, first);
    CHECK(out.count == 7 && !entries[2] && !entries[3] && !entries[5] && entries[1] == &g_res[1] && entries[6] == &g_res[5]);
    CHECK(g_acquires == 1 && g_releases == 1);
    CHECK(strstr(g_last_log, "3 of 7 entityDef(s) left out") && !strstr(g_last_log, "kept from"));
    /* The refetch (and the client's pass over the same view) reuses the reach. */
    sh_snapshot_templates_test_call(&g_entitydefs, &out, 0, second);
    CHECK(!entries[2] && strstr(g_last_log, "kept from the previous pass") && strstr(g_last_log, "(list refetched)"));
    /* A changed view rebuilds. */
    g_compiled.resource_count--;
    sh_snapshot_templates_test_call(&g_entitydefs, &out, 0, first);
    CHECK(!strstr(g_last_log, "kept from"));
    g_compiled.resource_count++;
}

static void a_busy_view_or_the_switch_keeps_the_native_list(void)
{
    static const char first[1], second[1];
    void *entries[8];
    fake_list out = {entries, 0, 8};
    build_view(); fill_list();
    sh_snapshot_templates_test_reset();
    sh_snapshot_templates_test_bind(fake_list_all, &g_entitydefs, first, second);
    g_busy = 1; g_releases = 0;
    sh_snapshot_templates_test_call(&g_entitydefs, &out, 0, first);
    CHECK(entries[2] == &g_res[2] && g_releases == 0 && strstr(g_last_log, "busy"));
    g_busy = 0;
    _putenv("SMP_SNAPSHOT_TEMPLATE_FILTER=0");
    g_logs = 0;
    sh_snapshot_templates_test_call(&g_entitydefs, &out, 0, first);
    CHECK(entries[2] == &g_res[2] && g_logs == 0);
    _putenv("SMP_SNAPSHOT_TEMPLATE_FILTER=");
    sh_snapshot_templates_test_call(&g_entitydefs, &out, 0, first);
    CHECK(!entries[2]);
}

int main(void)
{
    names_are_private_only_when_exact();
    reach_follows_served_text_from_engine_names_and_map_rows();
    the_filter_clears_unreached_private_entries_in_place();
    only_the_template_sites_on_the_entitydef_list_are_filtered();
    a_busy_view_or_the_switch_keeps_the_native_list();
    if (g_failures) { printf("snapshot_templates_test: %d of %d checks failed\n", g_failures, g_checks); return 1; }
    printf("snapshot_templates_test: reach, filter, call-site gating and fallbacks passed (%d checks)\n", g_checks);
    return 0;
}
