#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "package_virtual_page_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct vps_member {
    sh_compiled_resource input;
    sh_package_source_file expected;
    sh_package_file_identity identity;
    sh_package_file *sealed;
    unsigned char *sealed_body;
    int present, installed;
} vps_member;
struct sh_virtual_page_source {
    volatile LONG references;
    char *origin;
    vps_member members[21];
    unsigned char identity[32];
    int sealed;
};
typedef struct vps_read {
    const unsigned char *body;
    FILE *file;
    uint64_t length;
} vps_read;
static int vps_error(char *error, size_t capacity, const char *reason, const char *path)
{ if (error && capacity) snprintf(error, capacity, "%s%s%s", reason, path ? ": " : "", path ? path : ""); return 0; }
static int vps_read_at(void *context, uint64_t offset, void *bytes, size_t count)
{
    vps_read *read = context;
    if (offset > read->length || count > read->length - offset) return 0;
    if (read->body) { memcpy(bytes, read->body + (size_t)offset, count); return 1; }
    return read->file && offset <= INT64_MAX && !_fseeki64(read->file, (int64_t)offset, SEEK_SET) &&
        fread(bytes, 1, count, read->file) == count;
}
static int vps_reader(const vps_member *member, vps_read *read, char *error, size_t capacity)
{
    memset(read, 0, sizeof(*read));
    if (member->sealed_body) { read->body = member->sealed_body; read->length = member->identity.length; }
    else if (member->sealed) read->file = sh_package_file_open(member->sealed, &read->length, error, capacity);
    else if (member->input.body) { read->body = member->input.body; read->length = member->input.body_length; }
    else if (member->input.cache_file)
        read->file = sh_package_file_open(member->input.cache_file, &read->length, error, capacity);
    else if (member->expected.absolute) {
        read->length = member->expected.length;
        read->file = sh_package_source_open(&member->expected, error, capacity);
    }
    return read->body || read->file;
}
static void vps_reader_close(vps_read *read)
{ if (read->file) fclose(read->file); }
static void vps_free_member(vps_member *member)
{
    free((void *)member->input.engine_path); free(member->input.body); free(member->input.sources);
    free(member->input.cache_path);
    sh_package_owners_free(&member->input.owners); sh_package_owners_free(&member->input.gameplay_owners);
    sh_package_file_release(member->input.cache_file); sh_package_file_release(member->sealed);
    free(member->sealed_body);
    free(member->expected.absolute); memset(member, 0, sizeof(*member));
}
sh_virtual_page_source *sh_virtual_page_source_retain(sh_virtual_page_source *source)
{ if (source) InterlockedIncrement(&source->references); return source; }
void sh_virtual_page_source_release(sh_virtual_page_source *source)
{
    if (!source || InterlockedDecrement(&source->references)) return;
    for (size_t i = 0; i < 21; i++) vps_free_member(source->members + i);
    free(source->origin); free(source);
}
static int vps_capture(vps_member *member, const sh_package_compilation *scope, const char *path,
    sh_virtual_page_original original, void *context, char *error, size_t capacity)
{
    const sh_compiled_resource *input = sh_package_compilation_find(scope, path);
    member->input.source = SIZE_MAX; member->input.engine_path = _strdup(path);
    member->input.owns_engine_path = 1;
    if (!member->input.engine_path) return 0;
    if (input) {
        if (input->type || input->native_type || input->native_alias_count || input->generated_input_count ||
            input->bound_dependency_count || input->body_length == SIZE_MAX ||
            !sh_package_compilation_identity(scope, input, &member->identity, error, capacity)) return 0;
        member->input.source = input->source;
        if (input->body) {
            member->input.body = malloc(input->body_length + 1);
            if (!member->input.body) return 0;
            memcpy(member->input.body, input->body, input->body_length);
            member->input.body[input->body_length] = 0; member->input.body_length = input->body_length;
        } else if (input->cache_file) member->input.cache_file = sh_package_file_retain(input->cache_file);
        else {
            if (!scope->sources || input->source >= scope->sources->file_count) return 0;
            const sh_package_source_file *file = scope->sources->files + input->source;
            member->expected.absolute = _strdup(input->cache_path ? input->cache_path : file->absolute);
            if (!member->expected.absolute) return 0;
            if (input->cache_path && !(member->input.cache_path = _strdup(input->cache_path))) return 0;
            member->expected.engine_path = (char *)member->input.engine_path;
            member->expected.length = file->length; memcpy(member->expected.digest, file->digest, 32);
        }
        if (input->source_count) {
            if (!input->sources || input->source_count > SIZE_MAX / sizeof(*input->sources)) return 0;
            member->input.sources = malloc(input->source_count * sizeof(*input->sources));
            if (!member->input.sources) return 0;
            memcpy(member->input.sources, input->sources, input->source_count * sizeof(*input->sources));
            member->input.source_count = input->source_count;
        }
        if (!sh_package_owners_union(&member->input.owners, &input->owners) ||
            !sh_package_owners_union(&member->input.gameplay_owners, &input->gameplay_owners)) return 0;
        member->present = 1;
    } else {
        int status = original ? original(context, path, &member->input.cache_file, error, capacity) : 0;
        if (status < 0 || status > 1 || !!status != !!member->input.cache_file) return 0;
        if (!status) return 1;
        if (!sh_package_file_get_identity(member->input.cache_file, &member->identity)) return 0;
        member->present = member->installed = 1;
    }
    return 1;
}
sh_virtual_page_source *sh_virtual_page_source_open(const sh_package_compilation *scope,
    const char *source, sh_virtual_page_original original, void *context, char *error, size_t capacity)
{
    sh_virtual_page_source *out = calloc(1, sizeof(*out)); unsigned char fields[21 * 41] = {0};
    size_t present = 0; sh_package_file_identity identity;
    if (error && capacity) *error = 0;
    if (out) out->references = 1;
    if (!out || !scope || !(out->origin = sh_package_virtual_page_companion(source, 20))) goto failed;
    for (unsigned i = 0; i < 21; i++) {
        char *path = sh_package_virtual_page_companion(source, i);
        vps_member *member = out->members + i; sh_virtual_page_info info; vps_read read;
        int ok = path && vps_capture(member, scope, path, original, context, error, capacity);
        free(path); if (!ok) goto failed;
        if (!member->present) continue;
        if (!vps_reader(member, &read, error, capacity)) goto failed;
        ok = sh_package_virtual_page_inspect(i < 20 ? SH_VIRTUAL_PAGE_MEGA2 : SH_VIRTUAL_PAGE_PAGES,
            read.length, vps_read_at, &read, &info, error, capacity);
        vps_reader_close(&read); if (!ok) goto failed;
        if (info.empty) { vps_free_member(member); continue; }
        fields[i * 41] = 1;
        for (size_t j = 0; j < 8; j++) fields[i * 41 + 1 + j] = (unsigned char)(member->identity.length >> (j * 8));
        memcpy(fields + i * 41 + 9, member->identity.digest, 32); present++;
    }
    if (!present || !sh_package_bytes_identity(fields, sizeof(fields), &identity)) goto failed;
    memcpy(out->identity, identity.digest, 32); return out;
failed:
    if (!error || !capacity || !*error) vps_error(error, capacity, "cannot capture a complete virtual page source", source);
    sh_virtual_page_source_release(out); return NULL;
}
const unsigned char *sh_virtual_page_source_identity(const sh_virtual_page_source *source)
{ return source ? source->identity : NULL; }
const char *sh_virtual_page_source_origin(const sh_virtual_page_source *source)
{ return source ? source->origin : NULL; }
const sh_compiled_resource *sh_virtual_page_source_member(const sh_virtual_page_source *source, unsigned slot)
{ return source && slot < 21 && source->members[slot].present ? &source->members[slot].input : NULL; }
int sh_virtual_page_source_equal(const sh_virtual_page_source *a, const sh_virtual_page_source *b,
    char *error, size_t capacity)
{
    if (!a || !b) return -1;
    if (a == b) return 1;
    for (size_t i = 0; i < 21; i++) {
        const vps_member *x = a->members + i, *y = b->members + i;
        if (x->present != y->present) return 0;
        if (!x->present) continue;
        if (x->identity.length != y->identity.length || memcmp(x->identity.digest, y->identity.digest, 32)) return 0;
        vps_read left = {0}, right = {0}; int result = -1;
        if (vps_reader(x, &left, error, capacity) && vps_reader(y, &right, error, capacity))
            result = sh_package_virtual_page_equal(left.length, vps_read_at, &left, right.length, vps_read_at, &right);
        vps_reader_close(&left); vps_reader_close(&right);
        if (result != 1) return result;
    }
    return 1;
}
int sh_virtual_page_source_seal(sh_virtual_page_source *source, const sh_package_compilation *compiled,
    const char *const paths[21], char *error, size_t capacity)
{
    sh_package_file *files[21] = {0}; unsigned char *bodies[21] = {0}; int ok = 0;
    if (error && capacity) *error = 0;
    if (!source || !compiled || !paths) return 0;
    for (size_t i = 0; i < 21; i++) {
        const vps_member *member = source->members + i;
        if (!member->present) { if (paths[i]) goto done; continue; }
        const sh_compiled_resource *resource = paths[i] ? sh_package_compilation_find(compiled, paths[i]) : NULL;
        sh_package_file_identity identity;
        if (!resource || (!resource->body && !resource->cache_file) ||
            !sh_package_compilation_identity(compiled, resource, &identity, error, capacity) ||
            identity.length != member->identity.length || memcmp(identity.digest, member->identity.digest, 32)) goto done;
        vps_read before = {0}, after = {0}; int equal = -1;
        if (resource->body) { after.body = resource->body; after.length = resource->body_length; }
        else after.file = sh_package_file_open(resource->cache_file, &after.length, error, capacity);
        if ((after.body || after.file) && vps_reader(member, &before, error, capacity))
            equal = sh_package_virtual_page_equal(before.length, vps_read_at, &before, after.length, vps_read_at, &after);
        vps_reader_close(&before); vps_reader_close(&after); if (equal != 1) goto done;
        if (resource->body) {
            if (resource->body_length == SIZE_MAX || !(bodies[i] = malloc(resource->body_length + 1))) goto done;
            memcpy(bodies[i], resource->body, resource->body_length); bodies[i][resource->body_length] = 0;
        } else files[i] = sh_package_file_retain(resource->cache_file);
    }
    for (size_t i = 0; i < 21; i++) {
        sh_package_file_release(source->members[i].sealed); source->members[i].sealed = files[i]; files[i] = NULL;
        free(source->members[i].sealed_body); source->members[i].sealed_body = bodies[i]; bodies[i] = NULL;
    }
    source->sealed = 1; ok = 1;
done:
    for (size_t i = 0; i < 21; i++) { sh_package_file_release(files[i]); free(bodies[i]); }
    if (!ok && (!error || !capacity || !*error)) vps_error(error, capacity, "private virtual page cache differs from its captured source", source->origin);
    return ok;
}
int sh_virtual_page_source_open_member(const sh_virtual_page_source *source, unsigned slot,
    FILE **stream, uint64_t *length, char *error, size_t capacity)
{
    if (stream) *stream = NULL;
    if (length) *length = 0;
    if (!source || slot >= 21 || !stream || !length) return -1;
    if (!source->members[slot].present) return 0;
    if (!source->sealed) { vps_error(error, capacity, "virtual page source is not sealed", source->origin); return -1; }
    if (source->members[slot].sealed_body) {
        vps_error(error, capacity, "virtual page member requires its exact body stream", source->origin); return -1;
    }
    *stream = sh_package_file_open(source->members[slot].sealed, length, error, capacity);
    return *stream ? 1 : -1;
}
const unsigned char *sh_virtual_page_source_body_member(const sh_virtual_page_source *source,
    unsigned slot, size_t *length)
{
    if (length) *length = 0;
    if (!source || !source->sealed || slot >= 21 || !length || !source->members[slot].sealed_body) return NULL;
    *length = (size_t)source->members[slot].identity.length; return source->members[slot].sealed_body;
}
int sh_virtual_page_source_sealed(const sh_virtual_page_source *source)
{ return source && source->sealed; }
int sh_virtual_page_source_member_read(const sh_virtual_page_source *source, unsigned slot,
    uint64_t offset, void *bytes, size_t count, char *error, size_t capacity)
{
    vps_read read;
    if (!source || slot >= 21 || !bytes) return -1;
    if (!source->members[slot].present) return 0;
    if (!vps_reader(source->members + slot, &read, error, capacity)) return -1;
    int ok = vps_read_at(&read, offset, bytes, count);
    vps_reader_close(&read);
    if (!ok) vps_error(error, capacity, "cannot read a captured virtual page member", source->members[slot].input.engine_path);
    return ok ? 1 : -1;
}
int sh_virtual_page_source_member_installed(const sh_virtual_page_source *source, unsigned slot)
{
    return source && slot < 21 && source->members[slot].present && source->members[slot].installed;
}
