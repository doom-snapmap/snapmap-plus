/* Engine-written map forms through the real typed map-binding walk (R-390).
 *
 * The product opens a local map through sh_package_map_uses_bind_local_projected
 * (pr_rebind_local); a gap there fails the open ("This map couldn't be opened").
 * This harness runs that entry, with the native schema of a captured reflection
 * replay, the installed script module's metadata and the installed catalog, on
 * engine-written maps, and lists every gap the walk counts and every typed
 * reference it observes.
 *
 *   map_form_sweep_test <replay.tsv> <doom-root> fixtures <fixtures.jsonl>
 *       The checked-in minimal entity states (map_form_sweep_fixtures.jsonl),
 *       one {"name","refuse"?,"expect","map"} object per line: each opens with
 *       no gap and observes exactly its "expect" references ("<family>|<name>"),
 *       or, as a control with "refuse", keeps a refusal naming that text.
 *   map_form_sweep_test <replay.tsv> <doom-root> corpus <list.txt> <out.tsv>
 *       Each listed rawmap JSON or saved slot map.decl (4-byte checksum, zlib):
 *       rows "result", "gap", "form" (the walk of each placed state alone,
 *       which a binding refusal cannot hide) and "ref"/"lookup" per map.
 *   map_form_sweep_test <replay.tsv> <doom-root> probe <path|~fragment|@text|=path>...
 *       Installed catalog lookups: a path's status, rows whose path contains
 *       a fragment, declaration bodies containing text, or one body.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_map_uses.h"
#include "package_runtime.h"
#include "package_native_inline.h"
#include "resource_catalog.h"
#include "resource_graph_prepared.h"
#include "virtual_material_runtime.h"
#include "decl_script_native.h"
#include "raw_deflate.h"
#include "hook.h"
#include "native_reflection_replay.h"

/* Linked through the same sources as package_map_uses_test; never reached. */
const sh_package_compilation *sh_package_runtime_acquire(void) { assert(0); return NULL; }
void sh_package_runtime_release(void) { assert(0); }
const sh_resource_catalog *sh_package_runtime_catalog(void) { assert(0); return NULL; }
const sh_package_provenance *sh_package_runtime_bindings(void) { assert(0); return NULL; }
sh_package_bundle *sh_package_runtime_source_bundle(size_t owner, const unsigned char tree[32])
{ (void)owner; (void)tree; assert(0); return NULL; }
int sh_decl_server_registry_source(sh_decl_registry_source *source) { (void)source; assert(0); return 0; }
void *sh_typeinfo_get_reflect(void) { assert(0); return NULL; }
sh_resource_graph_preparation *sh_resource_graph_prepare_open(sh_decl_registry_source source, uintptr_t reflect)
{ (void)source; (void)reflect; assert(0); return NULL; }
int sh_resource_graph_prepare_entity(sh_resource_graph_preparation *p, const char *name, sh_decl_dependency_result *result)
{ (void)p; (void)name; (void)result; assert(0); return 0; }
int sh_resource_graph_prepare_inline(sh_resource_graph_preparation *p, const char *class_name,
    const char *inherit, const char *json, size_t length,
    int (*visitor)(void *, const char *, const char *), void *context, sh_decl_dependency_result *result)
{ (void)p; (void)class_name; (void)inherit; (void)json; (void)length; (void)visitor; (void)context; (void)result; assert(0); return 0; }
void sh_resource_graph_prepare_close(sh_resource_graph_preparation *p) { (void)p; assert(0); }
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; assert(0); return NULL; }
sh_patch_status hook_commit(void *trampoline) { (void)trampoline; assert(0); return B2_PATCH_OK; }
int hook_unpatch(void *trampoline) { (void)trampoline; assert(0); return 0; }
void backend_log(const char *message) { (void)message; }
sh_virtual_material_view *sh_virtual_material_runtime_prepare(
    const sh_virtual_material_runtime_binding *bindings, size_t count, char *error, size_t capacity)
{ (void)bindings; (void)count; (void)error; (void)capacity; assert(0); return NULL; }
int sh_virtual_material_runtime_retire(sh_virtual_material_view **view) { assert(!view || !*view); return 1; }

/* The product's map-local play declaration reader (package_runtime.c pr_map_local). */
static const sh_package_map_local_names g_map_local = {sh_native_inline_generated_read,
    sh_native_inline_generated_name, sh_native_inline_generated_free};

typedef struct text { char *data; size_t length, capacity; int failed; } text;
static void text_add(text *t, const char *s, size_t n)
{
    if (t->failed) return;
    if (t->length + n + 1 > t->capacity) {
        size_t next = t->capacity ? t->capacity * 2 : 4096;
        while (next < t->length + n + 1) next *= 2;
        char *grown = realloc(t->data, next);
        if (!grown) { t->failed = 1; return; }
        t->data = grown; t->capacity = next;
    }
    memcpy(t->data + t->length, s, n); t->length += n; t->data[t->length] = 0;
}
static void text_put(text *t, const char *s) { text_add(t, s ? s : "", strlen(s ? s : "")); }
/* One TSV field: tabs, newlines and backslashes escaped, non-ASCII bytes as \xNN. */
static void text_field(text *t, const char *s)
{
    char escape[8];
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '\t') text_put(t, "\\t");
        else if (*p == '\n') text_put(t, "\\n");
        else if (*p == '\r') text_put(t, "\\r");
        else if (*p == '\\') text_put(t, "\\\\");
        else if (*p < 0x20 || *p >= 0x7f) { snprintf(escape, sizeof(escape), "\\x%02x", *p); text_put(t, escape); }
        else text_add(t, (const char *)p, 1);
    }
}

typedef struct sweep {
    text gaps, refs, forms;
    size_t gap_count, ref_count, form_count;
} sweep;
static void on_gap(void *context, const char *gap)
{
    sweep *s = context;
    s->gap_count++; text_field(&s->gaps, gap); text_put(&s->gaps, "\n");
}
/* Observes every typed name at its site and binds none, as an installed
 * library binding no custom name leaves a vanilla map. */
static int on_site(sweep *s, const char *kind, const sh_package_provenance_site *site,
    const char *family, const char *name, const char **replacement)
{
    *replacement = NULL;
    s->ref_count++;
    text_put(&s->refs, kind); text_put(&s->refs, "\t");
    text_field(&s->refs, site ? site->owner : ""); text_put(&s->refs, "\t");
    text_field(&s->refs, site ? site->reference : ""); text_put(&s->refs, "\t");
    text_field(&s->refs, family); text_put(&s->refs, "\t");
    text_field(&s->refs, name); text_put(&s->refs, "\n");
    return 0;
}
static int on_lookup(void *context, const sh_package_provenance_site *site, const char *family,
    const char *name, const char **replacement)
{ return on_site(context, "lookup", site, family, name, replacement); }
static int on_observe(void *context, const sh_package_provenance_site *site, const char *family,
    const char *name, const char **replacement)
{ return on_site(context, "ref", site, family, name, replacement); }
/* An empty installed library projects nothing (mu_local_project finds no supplier). */
static int no_projection(void *context, int op, const char *owner, const char *inherit, const char *layer,
    const char *member, const char *family, const char *name, const char **replacement)
{
    (void)context; (void)op; (void)owner; (void)inherit; (void)layer; (void)member; (void)family; (void)name;
    *replacement = NULL; return 0;
}
static int all_native(void *context, const char *family, const char *name)
{ (void)context; (void)family; (void)name; return 1; }

/* The engine's production source mode: a declaration exists when the installed
 * catalog holds its generated text (as occurrence_revival_test's retained run). */
static sh_resource_catalog *g_catalog;
static int source_exists(void *context, uintptr_t manager, const char *type, const char *name)
{
    char path[512];
    unsigned char *body = NULL;
    size_t length = 0;
    (void)context; (void)manager;
    if (!type || !name || snprintf(path, sizeof(path), "generated/decls/%s/%s.decl", type, name) >= (int)sizeof(path)) return -1;
    for (char *c = path; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
    int stock = sh_resource_catalog_read_path(g_catalog, path, &body, &length);
    free(body);
    return stock < 0 ? -1 : stock > 0;
}

/* Form pass: every placed state walked by the typed JSON reader alone. It sees
 * the forms a binding refusal earlier in the same map would hide. */
static sweep *g_form;
static int form_reference(void *context, const char *path, sh_decl_value_type type, const char *name, size_t length)
{ (void)context; (void)path; (void)type; (void)name; (void)length; return 1; }
static void form_gap(void *context, const char *path, sh_decl_value_type type, const char *reason)
{
    (void)context;
    if (!g_form) return;
    g_form->form_count++;
    text_put(&g_form->forms, "form "); text_put(&g_form->forms, path); text_put(&g_form->forms, " (");
    text_put(&g_form->forms, type.name); text_put(&g_form->forms, "): "); text_put(&g_form->forms, reason);
    text_put(&g_form->forms, "\n");
}
typedef struct form_walk { const sh_decl_dependency_schema *view; } form_walk;
static int form_state(void *context, const char *class_name, const char *inherit, const char *edit, size_t length)
{
    form_walk *walk = context;
    sh_decl_dependency_result result;
    (void)inherit;
    if (class_name && *class_name && edit && length)
        (void)sh_decl_json_state_dependencies(edit, length, (sh_decl_value_type){class_name, ""}, walk->view, &result);
    return 1;
}

typedef struct inputs {
    sh_decl_native_schema *forms;
    sh_decl_native_registry *registry;
    native_replay replay;
    sh_decl_script_schema *scripts;
    sh_resource_catalog *catalog;
    sh_package_sources sources;
    sh_package_compilation compiled;
    sh_package_source_reader *reader;
} inputs;

static int open_inputs(inputs *in, const char *replay, const char *doom)
{
    char error[1024] = "", path[MAX_PATH];
    memset(in, 0, sizeof(*in));
    if (!native_replay_load(&in->replay, replay, error, sizeof(error))) {
        fprintf(stderr, "map_form_sweep_test: reflection replay: %s\n", error); return 0;
    }
    snprintf(path, sizeof(path), "%s\\superscriptx64.dll", doom);
    HMODULE module = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module || !(in->scripts = sh_decl_script_native_capture(module, error, sizeof(error)))) {
        fprintf(stderr, "map_form_sweep_test: script metadata from %s: %s\n", path, error); return 0;
    }
    snprintf(path, sizeof(path), "%s\\base", doom);
    if (!(in->catalog = sh_resource_catalog_open(path, error, sizeof(error)))) {
        fprintf(stderr, "map_form_sweep_test: catalog %s: %s\n", path, error); return 0;
    }
    in->compiled.sources = &in->sources;
    g_catalog = in->catalog;
    sh_decl_registry_source registry = {&in->replay, native_replay_read, in->replay.registry, source_exists, in->scripts, NULL};
    in->reader = sh_package_source_reader_open(&in->compiled, in->catalog, sh_resource_catalog_read_path, in->catalog,
        registry, in->replay.reflection, error, sizeof(error));
    if (!in->reader) { fprintf(stderr, "map_form_sweep_test: source reader: %s\n", error); return 0; }
    sh_package_source_reader_map_local(in->reader, &g_map_local);
    /* The diagnostic form pass: the same native schema, with no binding. */
    in->registry = sh_decl_native_registry_open(registry, error, sizeof(error));
    in->forms = in->registry ? sh_decl_native_schema_open((sh_decl_native_source){&in->replay, native_replay_read,
        in->replay.reflection, in->scripts}, (sh_decl_dependency_observer){NULL, form_reference, form_gap}, error, sizeof(error)) : NULL;
    if (!in->forms) { fprintf(stderr, "map_form_sweep_test: form schema: %s\n", error); return 0; }
    sh_decl_native_schema_attach_registry(in->forms, in->registry);
    fprintf(stderr, "map_form_sweep_test: replay %s %.8s (%zu classes, %zu loaded declarations)\n",
        in->replay.renderer, in->replay.pin, in->replay.classes, in->replay.loaded);
    return 1;
}
static void close_inputs(inputs *in)
{
    sh_decl_native_schema_close(in->forms);
    sh_decl_native_registry_close(in->registry);
    sh_package_source_reader_close(in->reader);
    if (in->catalog) sh_resource_catalog_close(in->catalog);
    if (in->scripts) sh_decl_script_schema_free(in->scripts);
    native_replay_free(&in->replay);
}

/* The product's open (1 opened, 0 refused, with its error), then the same walk
 * observed for its gaps and typed references. */
static int sweep_map(inputs *in, const char *json, size_t length, sweep *s, char *error, size_t capacity)
{
    sh_package_provenance library = {0}, projected = {0};
    size_t out_length = 0;
    library.semantics = SH_PACKAGE_PROVENANCE_CATALOG;
    *error = 0;
    char *out = sh_package_map_uses_bind_local_projected(in->reader, json, length, &library, NULL, 0,
        all_native, NULL, &out_length, &projected, error, capacity);
    int opened = out != NULL;
    free(out); sh_package_provenance_free(&projected);
    char ignored[1024];
    sh_package_source_reader_gap_sink(in->reader, on_gap, s);
    sh_package_source_reader_project_mode(in->reader, 2);
    sh_package_source_reader_projection(in->reader, no_projection, NULL);
    out = sh_package_source_bind_map_sites_observed(in->reader, json, length, on_lookup, on_observe, s,
        &out_length, NULL, ignored, sizeof(ignored));
    sh_package_source_reader_projection(in->reader, NULL, NULL);
    sh_package_source_reader_project_mode(in->reader, 0);
    sh_package_source_reader_gap_sink(in->reader, NULL, NULL);
    if (out && out_length != length) { snprintf(error, capacity, "a vanilla walk changed the map"); opened = 0; }
    free(out);
    form_walk walk = {sh_decl_native_schema_view(in->forms)};
    g_form = s; sh_package_map_state_sites(json, length, form_state, &walk); g_form = NULL;
    return opened;
}

static char *read_file(const char *path, size_t *length)
{
    FILE *file = NULL;
    char *data = NULL;
    long size;
    *length = 0;
    if (fopen_s(&file, path, "rb") || !file) return NULL;
    if (!fseek(file, 0, SEEK_END) && (size = ftell(file)) >= 0 && !fseek(file, 0, SEEK_SET) &&
        (data = malloc((size_t)size + 1)) && fread(data, 1, (size_t)size, file) == (size_t)size) {
        data[size] = 0; *length = (size_t)size;
    } else { free(data); data = NULL; }
    fclose(file);
    return data;
}
/* A saved slot's map.decl: 4-byte checksum, a 2-byte zlib header, raw DEFLATE
 * and a 4-byte Adler-32 trailer. */
static char *read_map(const char *path, size_t *length)
{
    size_t raw_length = 0, n = strlen(path);
    char *raw = read_file(path, &raw_length);
    if (!raw || n < 8 || _stricmp(path + n - 8, "map.decl")) { *length = raw_length; return raw; }
    *length = 0;
    if (raw_length < 10 || (unsigned char)raw[4] != 0x78) { free(raw); return NULL; }
    for (size_t capacity = raw_length * 8 + 4096; capacity < ((size_t)1 << 32); capacity *= 2) {
        unsigned char *out = malloc(capacity + 1);
        if (!out) break;
        size_t got = sh_inflate_raw_upto((unsigned char *)raw + 6, raw_length - 10, out, capacity);
        if (got) { free(raw); out[got] = 0; *length = got; return (char *)out; }
        free(out);
    }
    free(raw); return NULL;
}

static int corpus(inputs *in, const char *list_path, const char *out_path)
{
    size_t list_length = 0, maps = 0, opened = 0;
    char *list = read_file(list_path, &list_length), *line, *next = NULL, error[1024];
    FILE *out = NULL;
    if (!list || fopen_s(&out, out_path, "wb") || !out) { fprintf(stderr, "map_form_sweep_test: list or output\n"); return 2; }
    for (line = strtok_s(list, "\r\n", &next); line; line = strtok_s(NULL, "\r\n", &next)) {
        size_t length = 0;
        char *json = read_map(line, &length);
        sweep s = {0};
        text name = {0};
        text_field(&name, line);
        if (!json) { fprintf(out, "%s\tresult\tunreadable\t\n", name.data); free(name.data); continue; }
        int ok = sweep_map(in, json, length, &s, error, sizeof(error));
        maps++; opened += ok;
        text e = {0}; text_field(&e, error);
        fprintf(out, "%s\tresult\t%s\t%s\t%zu\t%zu\n", name.data, ok ? "opened" : "refused", e.data ? e.data : "",
            s.gap_count, s.ref_count);
        for (char *row = s.gaps.data, *end; row && *row; row = end + 1) {
            end = strchr(row, '\n'); *end = 0; fprintf(out, "%s\tgap\t%s\n", name.data, row);
        }
        for (char *row = s.forms.data, *end; row && *row; row = end + 1) {
            end = strchr(row, '\n'); *end = 0; fprintf(out, "%s\tform\t%s\n", name.data, row);
        }
        for (char *row = s.refs.data, *end; row && *row; row = end + 1) {
            end = strchr(row, '\n'); *end = 0; fprintf(out, "%s\t%s\n", name.data, row);
        }
        fflush(out);
        fprintf(stderr, "%s %s gaps=%zu forms=%zu refs=%zu %s\n", ok ? "opened " : "REFUSED", line, s.gap_count, s.form_count, s.ref_count,
            ok ? "" : error);
        free(e.data); free(name.data); free(s.gaps.data); free(s.refs.data); free(s.forms.data); free(json);
    }
    fclose(out); free(list);
    fprintf(stderr, "map_form_sweep_test: %zu of %zu maps opened\n", opened, maps);
    return 0;
}

/* Each fixture is a whole minimal map: the engine-written entity states a
 * corpus map refused, extracted verbatim, with the typed references the walk
 * must observe ("expect", one "<family>|<name>" per entry). */
static int fixtures(inputs *in, const char *path)
{
    size_t length = 0, cases = 0, failed = 0;
    char *body = read_file(path, &length), error[1024];
    if (!body) { fprintf(stderr, "map_form_sweep_test: %s cannot be read\n", path); return 1; }
    /* The fixture file is one JSON object per line: {"name":...,"expect":[...],"map":{...}}. */
    for (char *line = body, *end; line && *line; line = end ? end + 1 : NULL) {
        end = strchr(line, '\n'); if (end) *end = 0;
        char *map = strstr(line, "\"map\":"), *name = strstr(line, "\"name\":\""), *expect = strstr(line, "\"expect\":[");
        if (!map || !name || !expect) continue;
        map += 6;
        size_t map_length = strlen(map);
        while (map_length && (map[map_length - 1] == '}' || map[map_length - 1] == '\r' || map[map_length - 1] == ' ')) {
            if (map[map_length - 1] == '}') { map_length--; break; }
            map_length--;
        }
        char label[128]; size_t n = strcspn(name + 8, "\"");
        snprintf(label, sizeof(label), "%.*s", (int)(n < 120 ? n : 120), name + 8);
        sweep s = {0};
        int ok = sweep_map(in, map, map_length, &s, error, sizeof(error));
        /* A control states the refusal it must keep ("refuse": a substring of
         * the open's error); every other fixture opens with no gap. */
        char *refuse = strstr(line, "\"refuse\":\""), reason[256] = "";
        if (refuse && refuse < map) {
            size_t r = strcspn(refuse + 10, "\"");
            snprintf(reason, sizeof(reason), "%.*s", (int)(r < 250 ? r : 250), refuse + 10);
        }
        int matched = *reason ? !ok && s.gap_count && strstr(error, reason) != NULL : ok && !s.gap_count;
        /* Every expected reference is observed at its typed site, and nothing else. */
        size_t expected = 0;
        for (char *e = expect + 10; matched && !*reason && *e == '"'; ) {
            char *close = strchr(e + 1, '"'), want[512];
            if (!close) { matched = 0; break; }
            snprintf(want, sizeof(want), "%.*s", (int)(close - e - 1), e + 1);
            char *bar = strchr(want, '|');
            if (!bar) { matched = 0; break; }
            *bar = 0; expected++;
            char needle[600];
            snprintf(needle, sizeof(needle), "\t%s\t%s\n", want, bar + 1);
            if (!s.refs.data || !strstr(s.refs.data, needle)) {
                fprintf(stderr, "  %s: expected reference %s|%s not observed\n", label, want, bar + 1); matched = 0;
            }
            e = close + 1; if (*e == ',') e++;
        }
        size_t observed = 0;
        for (char *r = s.refs.data; r && (r = strstr(r, "ref\t")) != NULL; r += 4)
            if (r == s.refs.data || r[-1] == '\n') observed++;
        if (matched && !*reason && observed != expected) {
            fprintf(stderr, "  %s: observed %zu references, expected %zu\n%s", label, observed, expected, s.refs.data);
            matched = 0;
        }
        if (!matched) {
            failed++;
            fprintf(stderr, "[FAIL] %s: %s gaps=%zu %s\n%s", label, ok ? "opened" : "refused", s.gap_count, error,
                s.gaps.data ? s.gaps.data : "");
        } else fprintf(stderr, "[ok]   %s (%zu reference(s))\n", label, observed);
        cases++;
        free(s.gaps.data); free(s.refs.data);
    }
    free(body);
    fprintf(stderr, "map_form_sweep_test: %zu fixture(s), %zu failed\n", cases, failed);
    return failed || !cases;
}

int main(int argc, char **argv)
{
    inputs in;
    int code;
    if (argc < 5) {
        fprintf(stderr, "usage: map_form_sweep_test <replay.tsv> <doom-root> fixtures <file> | corpus <list> <out.tsv>\n");
        return 2;
    }
    if (!open_inputs(&in, argv[1], argv[2])) { close_inputs(&in); return 2; }
    if (!strcmp(argv[3], "probelist")) {
        /* One catalog path per line of argv[4]: "<status>\t<path>". */
        size_t length = 0;
        char *list = read_file(argv[4], &length), *next = NULL;
        for (char *line = list ? strtok_s(list, "\r\n", &next) : NULL; line; line = strtok_s(NULL, "\r\n", &next)) {
            unsigned char *body = NULL; size_t n = 0;
            int status = sh_resource_catalog_read_path(in.catalog, line, &body, &n);
            printf("%d\t%s\n", status, line); free(body);
        }
        code = list ? 0 : 2; free(list);
    } else if (!strcmp(argv[3], "probe")) {
        for (int i = 4; i < argc; i++) {
            unsigned char *body = NULL; size_t length = 0;
            if (argv[i][0] == '@') {
                for (size_t k = 0; k < sh_resource_catalog_count(in.catalog); k++) {
                    const sh_resource_catalog_entry *e = sh_resource_catalog_at(in.catalog, k);
                    unsigned char *text = NULL; size_t n = 0;
                    if (!e || !e->path || strncmp(e->path, "generated/decls/", 16)) continue;
                    if (sh_resource_catalog_read_path(in.catalog, e->path, &text, &n) > 0 && text) {
                        char *z = realloc(text, n + 1);
                        if (z) { text = (unsigned char *)z; text[n] = 0; if (strstr((char *)text, argv[i] + 1)) printf("body\t%s\n", e->path); }
                    }
                    free(text);
                }
                continue;
            }
            if (argv[i][0] == '~') {
                for (size_t k = 0; k < sh_resource_catalog_count(in.catalog); k++) {
                    const sh_resource_catalog_entry *e = sh_resource_catalog_at(in.catalog, k);
                    if (e && e->path && strstr(e->path, argv[i] + 1)) printf("row\t%s\n", e->path);
                }
                continue;
            }
            int dump = argv[i][0] == '=';
            int status = sh_resource_catalog_read_path(in.catalog, argv[i] + dump, &body, &length);
            printf("%d\t%zu\t%s\n", status, length, argv[i] + dump);
            if (dump && body) fwrite(body, 1, length, stdout);
            free(body);
        }
        code = 0;
    } else if (!strcmp(argv[3], "fixtures")) code = fixtures(&in, argv[4]);
    else if (!strcmp(argv[3], "corpus") && argc > 5) code = corpus(&in, argv[4], argv[5]);
    else code = 2;
    close_inputs(&in);
    return code;
}
