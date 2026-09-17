#include "package_legacy.h"
#include "package_migration.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct pl_conversion {
    sh_package_archive_files files;
    char *error;
    size_t capacity;
} pl_conversion;

static int pl_error(pl_conversion *c, const char *format, ...)
{
    va_list ap;
    if (c->error && c->capacity) {
        va_start(ap, format); vsnprintf(c->error, c->capacity, format, ap); va_end(ap);
    }
    return 0;
}

static int pl_text(const char *raw, char *out, size_t capacity)
{
    size_t length;
    return raw && sh_json_decode_string(raw, strlen(raw), out, capacity, &length) &&
        length < capacity && strlen(out) == length;
}

static int pl_section(pl_conversion *c, const sh_json_object *parent,
    const char *key, sh_json_object *out)
{
    const char *raw = sh_json_object_get(parent, key);
    if (!raw) return 1;
    return sh_json_parse_object(raw, strlen(raw), 24, out) ||
        pl_error(c, "legacy package section '%s' must be an object", key);
}

static sh_package_archive_file *pl_find(sh_package_archive_files *files, const char *name)
{
    size_t i;
    for (i = 0; i < files->count; i++)
        if (!_stricmp(files->items[i].name, name)) return &files->items[i];
    return NULL;
}

static int pl_add(pl_conversion *c, const char *name, const unsigned char *body,
    size_t length, int directory)
{
    sh_package_archive_file *old = pl_find(&c->files, name), *grown, *file;
    char *key = sh_package_engine_path(name);
    if (!key) return pl_error(c, "legacy conversion produced an unsafe path: %s", name);
    free(key);
    if (old) {
        if (old->directory == directory && old->length == length &&
            (directory || !length || !memcmp(old->body, body, length))) return 1;
        return pl_error(c, "legacy files map to conflicting content at '%s'", name);
    }
    if (c->files.count == SIZE_MAX / sizeof(*grown) || length == SIZE_MAX)
        return pl_error(c, "legacy package inventory is too large");
    grown = realloc(c->files.items, (c->files.count + 1) * sizeof(*grown));
    if (!grown) return pl_error(c, "cannot allocate converted package inventory");
    c->files.items = grown; file = &grown[c->files.count++]; memset(file, 0, sizeof(*file));
    file->name = _strdup(name); file->directory = directory; file->length = length;
    if (!directory) {
        file->body = malloc(length + 1);
        if (file->body) { if (length) memcpy(file->body, body, length); file->body[length] = 0; }
    }
    return (file->name && (directory || file->body)) || pl_error(c, "cannot retain converted package file");
}

/* The archive adapter supplies file bytes; all format policy is in the same
 * planner that the installer calls. No descriptor/path conversion rules live
 * in this delivery adapter. */
static char *pl_decode(const char *raw)
{
    char *out;
    size_t n;
    if (!raw) return NULL;
    n = strlen(raw) + 1; out = malloc(n);
    if (out && !pl_text(raw, out, n)) { free(out); out = NULL; }
    return out;
}

static int pl_file_order(const void *left, const void *right)
{ return strcmp(((const sh_package_archive_file *)left)->name, ((const sh_package_archive_file *)right)->name); }

typedef struct pl_component {
    const sh_package_archive_file *marker;
    char *prefix;
    char fallback[SH_PACKAGE_ID_CAP];
    int legacy, convert;
} pl_component;

static char *pl_join(const char *prefix, const char *path)
{
    size_t a = strlen(prefix), b = strlen(path);
    char *out;
    if (a >= SH_PACKAGE_SOURCE_PATH_CAP || b >= SH_PACKAGE_SOURCE_PATH_CAP - a) return NULL;
    out = malloc(a + b + 1);
    if (out) { memcpy(out, prefix, a); memcpy(out + a, path, b + 1); }
    return out;
}

static int pl_under(const char *path, const char *prefix)
{ return !_strnicmp(path, prefix, strlen(prefix)); }

static size_t pl_owner(const pl_component *components, size_t count, const char *path)
{
    size_t owner = 0, length = 0, i;
    for (i = 0; i < count; i++) {
        size_t n = strlen(components[i].prefix);
        if (n >= length && pl_under(path, components[i].prefix)) { owner = i; length = n; }
    }
    return owner;
}

static int pl_namespace(const char *relative)
{
    static const char *names[] = {"decls/", "images/", "shaders/", "resources/", "requirements/", "strings/", "hud/"};
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (pl_under(relative, names[i])) return 1;
    return 0;
}

static int pl_plan_has_assets(const char *plan)
{
    sh_json_object object = {0}, files = {0};
    const char *raw;
    size_t i;
    int found = 0;
    if (!sh_json_parse_object(plan, strlen(plan), 32, &object)) goto done;
    raw = sh_json_object_get(&object, "files");
    if (!raw || !sh_json_parse_object(raw, strlen(raw), 8, &files)) goto done;
    for (i = 0; i < files.count; i++) {
        sh_json_object row = {0};
        char *target = NULL;
        raw = files.members[i].value_json;
        if (sh_json_parse_object(raw, strlen(raw), 4, &row)) target = pl_decode(sh_json_object_get(&row, "target"));
        if (target && pl_under(target, "assets/")) found = 1;
        free(target); sh_json_object_free(&row); if (found) break;
    }
done:
    sh_json_object_free(&object); sh_json_object_free(&files); return found;
}

/* Classification does not rewrite descriptors. A current descriptor can still
 * accompany an old active namespace after a partial manual migration. */
static int pl_classify(pl_conversion *c, const sh_package_archive_files *original, pl_component *component)
{
    const char *body = (const char *)component->marker->body;
    size_t length = component->marker->length, prefix = strlen(component->prefix), i;
    sh_json_object descriptor = {0};
    char *schema = NULL, *identity = NULL;
    char canonical[SH_PACKAGE_ID_CAP];
    int ok = 0;
    if (length >= 3 && !memcmp(body, "\xef\xbb\xbf", 3)) { body += 3; length -= 3; component->convert = 1; }
    while (length && strchr(" \t\r\n", *body)) { body++; length--; }
    if (length && !sh_json_parse_object(body, length, 24, &descriptor)) {
        pl_error(c, "%s must contain a valid JSON object", component->marker->name); goto done;
    }
    schema = pl_decode(sh_json_object_get(&descriptor, "schema"));
    identity = pl_decode(sh_json_object_get(&descriptor, "id"));
    if (identity && sh_package_id_normalize(identity, canonical) && strcmp(identity, canonical))
        component->convert = 1;
    component->legacy = !sh_json_object_get(&descriptor, "id") ||
        (schema && !strncmp(schema, "snapmap-plus.override-package.", 30));
    component->convert |= component->legacy;
    for (i = 0; !component->convert && i < original->count; i++) {
        const sh_package_archive_file *file = &original->items[i];
        const char *relative;
        char *target;
        if (file->directory || !pl_under(file->name, component->prefix)) continue;
        relative = file->name + prefix;
        target = sh_package_migration_namespace(relative);
        if (target) {
            /* Let the shared planner decide whether this is an active decl
             * rather than treating every file in decls/ as an override. */
            char probe_error[256] = "", *plan;
            sh_package_migration *probe = sh_package_migration_open(body, length, component->fallback,
                "Legacy component", 0, probe_error, sizeof(probe_error));
            if (probe && sh_package_migration_add(probe, relative, 0, 0, (const char *)file->body, file->length) &&
                (plan = sh_package_migration_finish(probe, NULL)) != NULL) {
                if (pl_plan_has_assets(plan)) component->convert = 1;
                free(plan);
            }
            sh_package_migration_free(probe); free(target);
        } else if (sh_package_migration_policy(relative)) {
            char probe_error[256] = "", *plan;
            sh_package_migration *probe = sh_package_migration_open(body, length, component->fallback,
                "Legacy component", 0, probe_error, sizeof(probe_error));
            if (probe && sh_package_migration_add(probe, relative, 0, 0, (const char *)file->body, file->length) &&
                (plan = sh_package_migration_finish(probe, NULL)) != NULL) {
                component->convert = 1; free(plan);
            }
            sh_package_migration_free(probe);
        }
    }
    ok = 1;
done:
    free(identity); free(schema); sh_json_object_free(&descriptor); return ok;
}

static int pl_convert_component(pl_conversion *c, const sh_package_archive_files *original,
    const pl_component *components, size_t count, size_t index,
    sh_package_legacy_reader reader, void *context)
{
    const pl_component *component = &components[index];
    sh_package_migration *migration = NULL;
    sh_json_object result = {0}, files = {0}, imports = {0};
    char *plan = NULL;
    size_t i, prefix = strlen(component->prefix);
    int ok = 0;
    if (!component->convert) {
        for (i = 0; i < original->count; i++) if (pl_owner(components, count, original->items[i].name) == index) {
            const sh_package_archive_file *f = &original->items[i];
            if (!pl_add(c, f->name, f->body, f->length, f->directory)) return 0;
        }
        return 1;
    }
    migration = sh_package_migration_open((const char *)component->marker->body, component->marker->length,
        component->fallback, index ? "Legacy component" : component->fallback,
        component->legacy, c->error, c->capacity);
    if (!migration) goto done;
    for (i = 0; i < original->count; i++) if (pl_owner(components, count, original->items[i].name) == index) {
        const sh_package_archive_file *f = &original->items[i];
        const char *relative = f->name + prefix;
        int native = 0;
        if (component->legacy && pl_under(relative, "assets/") && reader) {
            int found = reader(context, NULL, f->directory ? "directory" : NULL,
                relative + 7, NULL, NULL, c->error, c->capacity);
            if (found < 0) goto done;
            native = found != 0;
        }
        if (!sh_package_migration_add(migration, relative, f->directory, native,
            (const char *)f->body, f->length)) goto done;
    }
    plan = sh_package_migration_finish(migration, NULL);
    if (!plan || !sh_json_parse_object(plan, strlen(plan), 32, &result) ||
        !pl_section(c, &result, "files", &files) || !pl_section(c, &result, "imports", &imports)) goto done;
    for (i = 0; i < files.count; i++) {
        sh_json_object row = {0};
        char *source = pl_join(component->prefix, files.members[i].key), *relative = NULL, *target = NULL;
        const sh_package_archive_file *f = source ? pl_find((sh_package_archive_files *)original, source) : NULL;
        int copied;
        if (sh_json_parse_object(files.members[i].value_json, strlen(files.members[i].value_json), 4, &row))
            relative = pl_decode(sh_json_object_get(&row, "target"));
        if (relative) target = pl_join(component->prefix, relative);
        copied = f && target && pl_add(c, target, f->body, f->length, f->directory);
        free(source); free(relative); free(target); sh_json_object_free(&row); if (!copied) goto done;
    }
    for (i = 0; i < imports.count; i++) {
        sh_json_object row = {0};
        char *type = NULL, *name = NULL, *path = NULL, *target = NULL, *assets = pl_join(component->prefix, "assets/");
        unsigned char *data = NULL;
        size_t size = 0;
        int resolved = 0, copied = 0;
        if (sh_json_parse_object(imports.members[i].value_json, strlen(imports.members[i].value_json), 4, &row)) {
            type = pl_decode(sh_json_object_get(&row, "type")); name = pl_decode(sh_json_object_get(&row, "name"));
            path = pl_decode(sh_json_object_get(&row, "provider"));
        }
        if (!reader) pl_error(c, "legacy manifest requires the installed resource catalog");
        else if (type && name && path) resolved = reader(context, type, name, path, &data, &size, c->error, c->capacity);
        if (resolved > 0 && path && assets && (target = pl_join(assets, path)) != NULL)
            copied = resolved == 2 || pl_find(&c->files, target) || pl_add(c, target, data, size, 0);
        free(type); free(name); free(path); free(target); free(assets); free(data); sh_json_object_free(&row);
        if (!copied) goto done;
    }
    {
        const char *body = sh_json_object_get(&result, "descriptor");
        char *target = pl_join(component->prefix, "package.json");
        ok = body && target && pl_add(c, target, (const unsigned char *)body, strlen(body), 0);
        free(target);
    }
done:
    free(plan); sh_json_object_free(&result); sh_json_object_free(&files); sh_json_object_free(&imports);
    sh_package_migration_free(migration); return ok;
}

static int pl_marker_order(const void *a, const void *b)
{
    const sh_package_archive_file *left = *(const sh_package_archive_file *const *)a;
    const sh_package_archive_file *right = *(const sh_package_archive_file *const *)b;
    size_t l = strlen(left->name), r = strlen(right->name);
    return l == r ? strcmp(left->name, right->name) : l < r ? -1 : 1;
}

int sh_package_legacy_convert(const unsigned char *bytes, size_t length,
    const char *delivery_id, sh_package_legacy_reader reader, void *context,
    unsigned char **out, size_t *out_length, char *error, size_t capacity)
{
    sh_package_archive_files original = {0};
    pl_component *components = NULL;
    const sh_package_archive_file **markers = NULL;
    size_t i, count = 0, marker_count = 0;
    pl_conversion c = {0};
    int ok = 0, changed = 0;
    c.error = error; c.capacity = capacity;
    if (error && capacity) error[0] = 0;
    if (!out || !out_length) return pl_error(&c, "missing legacy conversion output");
    *out = NULL; *out_length = 0;
    if (!sh_package_archive_read(bytes, length, &original, error, capacity)) goto done;
    components = calloc(original.count, sizeof(*components)); markers = calloc(original.count, sizeof(*markers));
    if (!components || !markers) goto done;
    for (i = 0; i < original.count; i++) {
        const sh_package_archive_file *f = &original.items[i];
        const char *leaf = strrchr(f->name, '/'); leaf = leaf ? leaf + 1 : f->name;
        if (!f->directory && !_stricmp(leaf, "package.json")) markers[marker_count++] = f;
    }
    qsort(markers, marker_count, sizeof(*markers), pl_marker_order);
    for (i = 0; i < marker_count; i++) {
        const sh_package_archive_file *f = markers[i];
        size_t prefix = strlen(f->name) - strlen("package.json"), j;
        pl_component *component = &components[count];
        if (count) {
            size_t parent = pl_owner(components, count, f->name);
            const char *relative = f->name + strlen(components[parent].prefix);
            if (pl_under(relative, "assets/") || (components[parent].convert && pl_namespace(relative))) continue;
        } else if (prefix) { pl_error(&c, "missing root package.json"); goto done; }
        component->prefix = malloc(prefix + 1);
        if (!component->prefix) goto done;
        memcpy(component->prefix, f->name, prefix); component->prefix[prefix] = 0; component->marker = f;
        if (!count) snprintf(component->fallback, sizeof(component->fallback), "%s", delivery_id);
        else {
            /* Stable source identity for formerly id-less nested components.
             * This is an identifier, not an integrity or authentication hash. */
            uint64_t hash = UINT64_C(14695981039346656037);
            for (j = 0; j < prefix; j++) { hash ^= (unsigned char)f->name[j]; hash *= UINT64_C(1099511628211); }
            snprintf(component->fallback, sizeof(component->fallback), "legacy.component.%016llx", (unsigned long long)hash);
        }
        count++;
        if (!pl_classify(&c, &original, component)) goto done;
        changed |= component->convert;
    }
    if (!count) { pl_error(&c, "missing root package.json"); goto done; }
    if (!changed) { ok = 1; goto done; }
    for (i = 0; i < count; i++) if (!pl_convert_component(&c, &original, components, count, i, reader, context)) goto done;
    qsort(c.files.items, c.files.count, sizeof(*c.files.items), pl_file_order);
    *out = sh_package_archive_write(&c.files, out_length, error, capacity); ok = *out != NULL;
done:
    if (!ok && error && capacity && !error[0]) pl_error(&c, "legacy package conversion could not finish");
    for (i = 0; i < count; i++) free(components[i].prefix);
    free(components); free(markers); sh_package_archive_files_free(&original); sh_package_archive_files_free(&c.files); return ok;
}
