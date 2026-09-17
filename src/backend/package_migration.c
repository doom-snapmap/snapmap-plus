#include "package_migration.h"
#include "package_descriptor.h"
#include "package_sources.h"
#include "decl_server_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

struct sh_package_migration {
    sh_json_object descriptor, requirements, cvars, strings, english, hud, weapons;
    sh_json_object files, imports;
    int legacy, failed;
    char *error;
    size_t capacity;
};

static int pm_error(sh_package_migration *m, const char *format, ...)
{
    va_list ap;
    m->failed = 1;
    if (m->error && m->capacity && !m->error[0]) {
        va_start(ap, format); vsnprintf(m->error, m->capacity, format, ap); va_end(ap);
    }
    return 0;
}

static char *pm_text(const char *raw)
{
    char *out;
    size_t n, decoded;
    if (!raw) return NULL;
    n = strlen(raw) + 1; out = malloc(n);
    if (out && (!sh_json_decode_string(raw, n - 1, out, n, &decoded) || strlen(out) != decoded)) {
        free(out); out = NULL;
    }
    return out;
}

/* Object serialization already supplies correct escaping for arbitrary UTF-8. */
static int pm_string(sh_json_object *object, const char *key, const char *value)
{
    sh_json_object wrapper = {0};
    char *encoded, *colon;
    int ok = 0;
    if (!sh_json_object_set(&wrapper, value, "0", 2)) return 0;
    encoded = sh_json_serialize_object(&wrapper, 0, NULL);
    if (encoded) {
        char *start = strchr(encoded, '"');
        /* The last colon is the separator after the escaped key. */
        colon = strrchr(encoded, ':');
        if (start && colon) {
            while (colon > start && colon[-1] != '"') colon--;
            *colon = 0; ok = sh_json_object_set(object, key, start, 24);
        }
    }
    free(encoded); sh_json_object_free(&wrapper); return ok;
}

static int pm_section(sh_package_migration *m, const sh_json_object *parent,
    const char *key, sh_json_object *out)
{
    const char *raw = sh_json_object_get(parent, key);
    return !raw || sh_json_parse_object(raw, strlen(raw), 24, out) ||
        pm_error(m, "package section '%s' must be an object", key);
}

static int pm_store(sh_package_migration *m, sh_json_object *parent,
    const char *key, const sh_json_object *object)
{
    char *raw = sh_json_serialize_object(object, 0, NULL);
    int ok = raw && sh_json_object_set(parent, key, raw, 24);
    free(raw); return ok || pm_error(m, "cannot retain migrated '%s'", key);
}

static void pm_remove(sh_json_object *object, const char *key)
{
    size_t i;
    for (i = 0; i < object->count; i++) if (!strcmp(object->members[i].key, key)) {
        free(object->members[i].key); free(object->members[i].value_json);
        memmove(object->members + i, object->members + i + 1,
            (--object->count - i) * sizeof(*object->members)); return;
    }
}

static int pm_decl(const char *path)
{
    char type[64], name[512], source[768];
    return sh_decl_server_identity_from_relative(path, type, sizeof(type), name,
        sizeof(name), source, sizeof(source), NULL);
}

static int pm_prefix(const char *text, const char *prefix)
{ return !_strnicmp(text, prefix, strlen(prefix)); }

char *sh_package_migration_namespace(const char *path)
{
    const char *rest = NULL, *prefix = NULL;
    char *out;
    if (!_stricmp(path, "decls") || pm_prefix(path, "decls/")) {
        prefix = "assets/generated/decls"; rest = path + 5;
    } else if (!_stricmp(path, "images") || pm_prefix(path, "images/")) {
        prefix = "assets/generated/image"; rest = path + 6;
    } else if (pm_prefix(path, "shaders/generated/spirv/") || !_stricmp(path, "shaders/generated/spirv") ||
        pm_prefix(path, "shaders/generated/renderprogs/") || !_stricmp(path, "shaders/generated/renderprogs")) {
        prefix = "assets/"; rest = path + 8;
    }
    if (!prefix) return NULL;
    out = malloc(strlen(prefix) + strlen(rest) + 1);
    if (out) { memcpy(out, prefix, strlen(prefix)); memcpy(out + strlen(prefix), rest, strlen(rest) + 1); }
    return out;
}

static int pm_suffix(const char *text, const char *suffix)
{
    size_t n = strlen(text), s = strlen(suffix);
    return n >= s && !_stricmp(text + n - s, suffix);
}

int sh_package_migration_policy(const char *path)
{
    const char *slash = strchr(path, '/');
    if (!slash || strchr(slash + 1, '/')) return 0;
    if (pm_prefix(path, "resources/") && pm_suffix(path, ".manifest")) return 1;
    if (pm_prefix(path, "requirements/") && pm_suffix(path, ".requirements")) return 2;
    if (pm_prefix(path, "strings/") && pm_suffix(path, ".json")) return 3;
    return !_stricmp(path, "hud/weapons.json") ? 4 : 0;
}

static int pm_field(const char *s, int type, size_t cap)
{
    const char *p, *part = s;
    if (!*s || strlen(s) >= cap) return 0;
    for (p = s; ; p++) {
        unsigned char c = (unsigned char)*p;
        if (!c || c == '/') {
            size_t n = (size_t)(p - part);
            if (!n || (n == 1 && *part == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return 0;
            if (!c) return 1;
            if (type) return 0;
            part = p + 1;
        } else if (c < 0x21 || c > 0x7e || c == '\\' || c == ':') return 0;
    }
}

static int pm_import(sh_package_migration *m, const char *type, const char *name,
    const char *provider, const char *origin, size_t line)
{
    sh_json_object row = {0}, previous = {0};
    char *key = sh_package_engine_path(provider), *old_type = NULL, *old_name = NULL;
    const char *old;
    char source[1024];
    int ok = 0;
    if (!key) return pm_error(m, "%s:%zu: unusable resource path '%s'", origin, line, provider);
    old = sh_json_object_get(&m->imports, key);
    if (old) {
        if (!sh_json_parse_object(old, strlen(old), 4, &previous)) goto done;
        old_type = pm_text(sh_json_object_get(&previous, "type"));
        old_name = pm_text(sh_json_object_get(&previous, "name"));
        if (old_type && old_name && !_stricmp(old_type, type) && !_stricmp(old_name, name)) ok = 1;
        else pm_error(m, "%s:%zu: different resource identities claim '%s'", origin, line, provider);
        goto done;
    }
    snprintf(source, sizeof(source), "%s:%zu", origin, line);
    ok = pm_string(&row, "type", type) && pm_string(&row, "name", name) &&
        pm_string(&row, "provider", provider) && pm_string(&row, "origin", source) &&
        pm_store(m, &m->imports, key, &row);
done:
    free(key); free(old_type); free(old_name);
    sh_json_object_free(&row); sh_json_object_free(&previous);
    return ok || pm_error(m, "cannot retain resource import from '%s'", origin);
}

static int pm_rows(sh_package_migration *m, const char *path, const char *body, size_t length, int manifest)
{
    char *copy, *line, *next;
    size_t number = 0;
    int ok = 0;
    if (!body || length == SIZE_MAX || memchr(body, 0, length)) return pm_error(m, "%s: invalid policy text", path);
    copy = malloc(length + 1); if (!copy) return pm_error(m, "cannot read '%s'", path);
    memcpy(copy, body, length); copy[length] = 0;
    for (line = copy; line; line = next) {
        char *a, *b, *end, *p;
        number++; next = strchr(line, '\n'); if (next) *next++ = 0;
        end = line + strlen(line); while (end > line && end[-1] == '\r') *--end = 0;
        if (!*line || *line == '#') continue;
        for (p = line; *p; p++) if (*p != '\t' && ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e)) goto invalid;
        a = strchr(line, '\t'); if (!a) goto invalid; *a++ = 0;
        b = strchr(a, '\t'); if (!b) goto invalid; *b++ = 0;
        if (strchr(b, '\t')) goto invalid;
        if (manifest) {
            if (!pm_field(line, 1, 64) || !pm_field(a, 0, 512) || (*b && !pm_field(b, 0, 768))) goto invalid;
            if (!pm_import(m, line, a, *b ? b : a, path, number)) goto done;
        } else {
            const char *old;
            if (strcmp(line, "cvar") || (strcmp(a, "g_useResourceBlackList") && strcmp(a, "g_useImageBlackList")) || strcmp(b, "0")) goto invalid;
            old = sh_json_object_get(&m->cvars, a);
            if (old && strcmp(old, "0")) { pm_error(m, "%s:%zu: conflicting requirement '%s'", path, number, a); goto done; }
            if (!sh_json_object_set(&m->cvars, a, "0", 24)) goto done;
        }
        continue;
invalid:
        pm_error(m, "%s:%zu: invalid legacy %s row", path, number, manifest ? "manifest" : "requirement"); goto done;
    }
    ok = 1;
done:
    free(copy); return ok || pm_error(m, "cannot retain policy '%s'", path);
}

static int pm_weapon_name(const char *s)
{
    const char *p;
    if (!pm_field(s, 0, 192)) return 0;
    for (p = s; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
        *p == '/' || *p == '-' || *p == '_' || *p == '.')) return 0;
    return 1;
}

static char *pm_mode(const char *raw)
{
    sh_json_object row = {0};
    char *mode = NULL;
    if (sh_json_parse_object(raw, strlen(raw), 3, &row) && row.count == 1)
        mode = pm_text(sh_json_object_get(&row, "ammo_display"));
    if (mode && strcmp(mode, "weapon") && strcmp(mode, "engine")) { free(mode); mode = NULL; }
    sh_json_object_free(&row); return mode;
}

static int pm_merge(sh_package_migration *m, sh_json_object *into, const sh_json_object *from,
    int hud, const char *origin)
{
    size_t i, j;
    for (i = 0; i < from->count; i++) {
        const sh_json_member *f = &from->members[i];
        char *value = hud ? pm_mode(f->value_json) : pm_text(f->value_json), *old_value = NULL;
        int ok = 1;
        if (!value || !f->key_length || strlen(f->key) != f->key_length || (hud && !pm_weapon_name(f->key))) ok = 0;
        for (j = 0; ok && j < into->count; j++) {
            const sh_json_member *old = &into->members[j];
            if (hud ? !strcmp(old->key, f->key) : !_stricmp(old->key, f->key)) {
                old_value = hud ? pm_mode(old->value_json) : pm_text(old->value_json);
                ok = old_value && !strcmp(value, old_value); break;
            }
        }
        if (ok && j == into->count) ok = sh_json_object_set(into, f->key, f->value_json, 24);
        free(value); free(old_value);
        if (!ok) return pm_error(m, "%s: invalid or conflicting %s '%s'", origin, hud ? "HUD rule" : "string", f->key);
    }
    return 1;
}

static int pm_json_policy(sh_package_migration *m, const char *path, const char *body, size_t length, int hud)
{
    sh_json_object object = {0}, weapons = {0};
    char *schema = NULL;
    int ok = 0;
    if (body && length >= 3 && !memcmp(body, "\xef\xbb\xbf", 3)) { body += 3; length -= 3; }
    if (!body || !sh_json_parse_object(body, length, hud ? 5 : 1, &object)) goto done;
    if (hud) {
        schema = pm_text(sh_json_object_get(&object, "schema"));
        if (!schema || strcmp(schema, "snapmap-plus.weapon-hud.v1") || object.count != 2 ||
            !sh_json_object_get(&object, "weapons") || !pm_section(m, &object, "weapons", &weapons)) goto done;
        ok = pm_merge(m, &m->weapons, &weapons, 1, path);
    } else ok = pm_merge(m, &m->english, &object, 0, path);
done:
    free(schema); sh_json_object_free(&object); sh_json_object_free(&weapons);
    return ok || pm_error(m, "%s: invalid legacy %s policy", path, hud ? "HUD" : "strings");
}

static int pm_file(sh_package_migration *m, const char *target, const char *source, int directory, int active)
{
    sh_json_object row = {0};
    char *key = sh_package_engine_path(target);
    const char *old;
    int ok = 0;
    if (!key) return pm_error(m, "'%s' is not a usable package path", target);
    if (!active && !directory) {
        const char *leaf = strrchr(target, '/'); leaf = leaf ? leaf + 1 : target;
        if (!_stricmp(leaf, "package.json")) {
            pm_error(m, "inactive package marker '%s' needs its own component", source); goto done;
        }
    }
    if (active && !directory && pm_prefix(target, "assets/generated/decls/") && !pm_decl(target + 23)) {
        pm_error(m, "'%s' is not a valid declaration path", target); goto done;
    }
    old = sh_json_object_get(&m->files, source);
    /* Key by source, not destination: adapters verify duplicate target bytes. */
    if (old) { pm_error(m, "source '%s' was submitted twice", source); goto done; }
    ok = pm_string(&row, "target", target) && sh_json_object_set(&row, "directory", directory ? "true" : "false", 2) &&
        pm_store(m, &m->files, source, &row);
done:
    free(key); sh_json_object_free(&row); return ok || pm_error(m, "cannot retain file '%s'", source);
}

int sh_package_migration_add(sh_package_migration *m, const char *path, int directory,
    int native, const char *body, size_t length)
{
    const char *inner = path;
    char *target = NULL;
    int kind, wrapped = 0, active = 0, ok;
    if (!m || m->failed) return 0;
    if (!path || !*path) return pm_error(m, "missing package file path");
    if (!_stricmp(path, "package.json")) return !directory || pm_error(m, "package.json is a directory");
    if (pm_prefix(path, "assets/")) {
        if (!m->legacy || native) return pm_file(m, path, path, directory, 1);
        inner = path + 7; wrapped = 1;
    }
    if (directory) {
        target = sh_package_migration_namespace(inner);
        ok = pm_file(m, target ? target : path, path, 1, target != NULL || wrapped);
        free(target); return ok;
    }
    if (pm_prefix(inner, "decls/")) {
        if (pm_decl(inner + 6)) target = sh_package_migration_namespace(inner);
        else return pm_file(m, inner, path, 0, 0);
    } else if (pm_prefix(inner, "images/") || pm_prefix(inner, "shaders/generated/spirv/") ||
        pm_prefix(inner, "shaders/generated/renderprogs/")) target = sh_package_migration_namespace(inner);
    else if ((kind = sh_package_migration_policy(inner)) != 0) {
        if (kind <= 2) return pm_rows(m, path, body, length, kind == 1);
        return pm_json_policy(m, path, body, length, kind == 4);
    }
    active = target != NULL || wrapped;
    if (!active) {
        const char *leaf = strrchr(path, '/'); leaf = leaf ? leaf + 1 : path;
        if (!_stricmp(leaf, "package.json")) return pm_error(m, "inactive package marker '%s' needs its own component", path);
    }
    ok = pm_file(m, target ? target : path, path, 0, active);
    free(target); return ok;
}

sh_package_migration *sh_package_migration_open(const char *descriptor, size_t length,
    const char *id, const char *name, int legacy, char *error, size_t capacity)
{
    sh_package_migration *m = calloc(1, sizeof(*m));
    char *text = NULL;
    const char *raw;
    size_t i;
    if (error && capacity) error[0] = 0;
    if (!m) { if (error && capacity) snprintf(error, capacity, "cannot allocate package migration"); return NULL; }
    m->error = error; m->capacity = capacity; m->legacy = legacy;
    if (!descriptor && length) { pm_error(m, "missing descriptor bytes"); goto bad; }
    if (descriptor && length >= 3 && !memcmp(descriptor, "\xef\xbb\xbf", 3)) { descriptor += 3; length -= 3; }
    while (length && strchr(" \t\r\n", *descriptor)) { descriptor++; length--; }
    if (length && !sh_json_parse_object(descriptor, length, 24, &m->descriptor)) {
        pm_error(m, "legacy package.json must contain one valid object"); goto bad;
    }
    raw = sh_json_object_get(&m->descriptor, "id");
    if (raw) {
        text = pm_text(raw);
        if (!text || !sh_package_id_valid(text)) { pm_error(m, "package.json has an invalid id"); goto bad; }
        free(text); text = NULL;
    } else if (!id || !sh_package_id_valid(id) || !pm_string(&m->descriptor, "id", id)) goto bad;
    if (legacy) {
        const char *retired[] = {"version", "priority", "contents", "restart_required"};
        for (i = 0; i < sizeof(retired) / sizeof(retired[0]); i++) pm_remove(&m->descriptor, retired[i]);
        text = pm_text(sh_json_object_get(&m->descriptor, "schema"));
        if (text && pm_prefix(text, "snapmap-plus.")) pm_remove(&m->descriptor, "schema");
        free(text); text = NULL;
    }
    raw = sh_json_object_get(&m->descriptor, "name");
    text = pm_text(raw);
    if (!raw || (legacy && text && !*text)) {
        if (!name || !*name || !pm_string(&m->descriptor, "name", name)) goto bad;
    }
    free(text); text = NULL;
    if (!pm_section(m, &m->descriptor, "requirements", &m->requirements) ||
        !pm_section(m, &m->requirements, "cvars", &m->cvars) ||
        !pm_section(m, &m->descriptor, "strings", &m->strings) ||
        !pm_section(m, &m->strings, "en", &m->english) ||
        !pm_section(m, &m->descriptor, "hud", &m->hud) ||
        !pm_section(m, &m->hud, "weapons", &m->weapons)) goto bad;
    return m;
bad:
    free(text); pm_error(m, "cannot migrate package descriptor"); sh_package_migration_free(m); return NULL;
}

char *sh_package_migration_finish(sh_package_migration *m, size_t *length)
{
    sh_json_object result = {0};
    sh_package_descriptor verified = {0};
    char *raw = NULL, *out = NULL;
    size_t i;
    if (length) *length = 0;
    if (!m || m->failed) return NULL;
    for (i = 0; i < m->cvars.count; i++) {
        const sh_json_member *v = &m->cvars.members[i];
        if ((strcmp(v->key, "g_useResourceBlackList") && strcmp(v->key, "g_useImageBlackList")) || strcmp(v->value_json, "0")) {
            pm_error(m, "unsupported cvar requirement '%s'", v->key); goto done;
        }
    }
    if ((m->requirements.count && (m->requirements.count != 1 || !sh_json_object_get(&m->requirements, "cvars"))) ||
        (m->hud.count && (m->hud.count != 1 || !sh_json_object_get(&m->hud, "weapons")))) {
        pm_error(m, "unsupported package policy section"); goto done;
    }
    for (i = 0; i < m->weapons.count; i++) {
        char *mode = pm_mode(m->weapons.members[i].value_json);
        int valid = mode && pm_weapon_name(m->weapons.members[i].key);
        free(mode); if (!valid) { pm_error(m, "invalid HUD rule"); goto done; }
    }
    if (m->weapons.count > 256) { pm_error(m, "too many HUD rules"); goto done; }
    if (m->cvars.count && (!pm_store(m, &m->requirements, "cvars", &m->cvars) ||
        !pm_store(m, &m->descriptor, "requirements", &m->requirements))) goto done;
    if (m->english.count && (!pm_store(m, &m->strings, "en", &m->english) ||
        !pm_store(m, &m->descriptor, "strings", &m->strings))) goto done;
    if (m->weapons.count && (!pm_store(m, &m->hud, "weapons", &m->weapons) ||
        !pm_store(m, &m->descriptor, "hud", &m->hud))) goto done;
    raw = sh_json_serialize_object(&m->descriptor, 0, NULL);
    if (!raw || !sh_package_descriptor_parse(raw, strlen(raw), &verified, m->error, m->capacity) ||
        !sh_json_object_set(&result, "descriptor", raw, 24) ||
        !pm_store(m, &result, "files", &m->files) || !pm_store(m, &result, "imports", &m->imports)) goto done;
    out = sh_json_serialize_object(&result, 0, length);
done:
    if (!out) pm_error(m, "package conversion could not finish");
    free(raw); sh_json_object_free(&result); sh_package_descriptor_free(&verified); return out;
}

void sh_package_migration_free(sh_package_migration *m)
{
    if (!m) return;
    sh_json_object_free(&m->descriptor); sh_json_object_free(&m->requirements); sh_json_object_free(&m->cvars);
    sh_json_object_free(&m->strings); sh_json_object_free(&m->english); sh_json_object_free(&m->hud);
    sh_json_object_free(&m->weapons); sh_json_object_free(&m->files); sh_json_object_free(&m->imports); free(m);
}

char *sh_package_migration_run(const char *request, size_t length,
    size_t *output_length, char *error, size_t capacity)
{
    sh_json_object input = {0}, files = {0}, policies = {0};
    sh_package_migration *m = NULL;
    char *descriptor = NULL, *id = NULL, *name = NULL, *out = NULL;
    const char *raw;
    size_t i;
    if (error && capacity) error[0] = 0;
    if (output_length) *output_length = 0;
    if (!sh_json_parse_object(request, length, 32, &input)) goto done;
    descriptor = pm_text(sh_json_object_get(&input, "descriptor"));
    id = pm_text(sh_json_object_get(&input, "id")); name = pm_text(sh_json_object_get(&input, "name"));
    raw = sh_json_object_get(&input, "legacy");
    if (!descriptor || !id || !name || !raw || (strcmp(raw, "true") && strcmp(raw, "false"))) goto done;
    m = sh_package_migration_open(descriptor, strlen(descriptor), id, name,
        !strcmp(raw, "true"), error, capacity);
    if (!m || !pm_section(m, &input, "files", &files) || !pm_section(m, &input, "policies", &policies)) goto done;
    for (i = 0; i < files.count; i++) {
        sh_json_object file = {0};
        char *body = NULL;
        const sh_json_member *f = &files.members[i];
        const char *directory, *native;
        int ok = 0;
        if (strlen(f->key) != f->key_length || !sh_json_parse_object(f->value_json, strlen(f->value_json), 4, &file)) goto done;
        directory = sh_json_object_get(&file, "directory"); native = sh_json_object_get(&file, "native");
        raw = sh_json_object_get(&file, "body");
        if (raw) body = pm_text(raw);
        if (directory && native && (!strcmp(directory, "true") || !strcmp(directory, "false")) &&
            (!strcmp(native, "true") || !strcmp(native, "false")) && (!raw || body))
            ok = sh_package_migration_add(m, f->key, !strcmp(directory, "true"), !strcmp(native, "true"), body, body ? strlen(body) : 0);
        free(body); sh_json_object_free(&file); if (!ok) goto done;
    }
    for (i = 0; i < policies.count; i++) {
        sh_json_object policy = {0};
        const sh_json_member *p = &policies.members[i];
        char *path = NULL, *body = NULL;
        int kind = 0, ok = 0;
        if (strlen(p->key) != p->key_length || !sh_json_parse_object(p->value_json, strlen(p->value_json), 4, &policy)) goto done;
        path = pm_text(sh_json_object_get(&policy, "path"));
        body = pm_text(sh_json_object_get(&policy, "body"));
        if (path) kind = sh_package_migration_policy(path);
        if (body && kind) ok = kind <= 2 ? pm_rows(m, p->key, body, strlen(body), kind == 1) :
            pm_json_policy(m, p->key, body, strlen(body), kind == 4);
        free(path); free(body); sh_json_object_free(&policy); if (!ok) goto done;
    }
    out = sh_package_migration_finish(m, output_length);
done:
    if (!out && error && capacity && !error[0]) snprintf(error, capacity, "invalid migration request");
    free(descriptor); free(id); free(name); sh_json_object_free(&input); sh_json_object_free(&files); sh_json_object_free(&policies);
    sh_package_migration_free(m); return out;
}

void sh_package_migration_release(void *memory) { free(memory); }
