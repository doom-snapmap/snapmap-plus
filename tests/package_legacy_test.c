#include "../src/backend/package_legacy.h"
#include "../src/backend/resource_catalog.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int reads;
static int catalog(void *ctx, const char *type, const char *name, const char *path,
    unsigned char **bytes, size_t *length, char *error, size_t capacity)
{
    const char *text = "installed campaign resource";
    (void)ctx; (void)error; (void)capacity;
    if (!type) return !strcmp(path, "generated/model/already-native.bmodel");
    reads++;
    assert(!strcmp(type, "model")); assert(!strcmp(name, "boss") || !strcmp(name, "vanilla"));
    if (!strcmp(name, "vanilla")) return 2;
    *length = strlen(text); *bytes = malloc(*length); assert(*bytes);
    memcpy(*bytes, text, *length); return 1;
}

static void add(sh_package_archive_files *f, const char *name, const char *text)
{
    sh_package_archive_file *item;
    f->items = realloc(f->items, (f->count + 1) * sizeof(*f->items)); assert(f->items);
    item = &f->items[f->count++]; memset(item, 0, sizeof(*item));
    item->name = _strdup(name); item->body = (unsigned char *)_strdup(text); item->length = strlen(text);
}
static sh_package_archive_file *find(sh_package_archive_files *f, const char *path)
{
    size_t i;
    for (i = 0; i < f->count; i++) if (!strcmp(f->items[i].name, path)) return &f->items[i];
    return NULL;
}
static void migration(void)
{
    sh_package_archive_files f = {0}, converted = {0};
    unsigned char *zip, *out = NULL;
    size_t length, out_length;
    char error[2048], id[128];
    sh_package_archive_file *file;
    add(&f, "package.json", "{\"schema\":\"snapmap-plus.override-package.v1\",\"name\":\"Boss\"}");
    add(&f, "decls/entitydef/boss.decl", "authored health = 10;");
    add(&f, "images/boss.bimage", "authored image bytes");
    add(&f, "shaders/generated/spirv/boss.spv", "authored shader bytes");
    add(&f, "requirements/boss.requirements", "cvar\tg_useResourceBlackList\t0\r\n");
    add(&f, "resources/boss.manifest", "model\tboss\tgenerated/model/boss.bmodel\nmodel\tvanilla\tgenerated/model/vanilla.bmodel\n");
    zip = sh_package_archive_write(&f, &length, error, sizeof(error));
    if (!zip) fprintf(stderr, "%s\n", error);
    assert(zip);
    assert(!sh_package_archive_inspect(zip, length, id, NULL, error, sizeof(error)));
    assert(strstr(error, "missing its required id"));
    assert(sh_package_legacy_convert(zip, length, "boss", catalog, NULL, &out, &out_length, error, sizeof(error)));
    assert(out && reads == 2);
    assert(sh_package_archive_inspect(out, out_length, id, NULL, error, sizeof(error)) && !strcmp(id, "boss"));
    assert(sh_package_archive_read(out, out_length, &converted, error, sizeof(error)));
    file = find(&converted, "assets/generated/decls/entitydef/boss.decl");
    assert(file && file->length == strlen("authored health = 10;") && !memcmp(file->body, "authored health = 10;", file->length));
    assert(find(&converted, "assets/generated/spirv/boss.spv"));
    assert(find(&converted, "assets/generated/model/boss.bmodel"));
    assert(!find(&converted, "assets/generated/model/vanilla.bmodel"));
    {
        unsigned char *again = NULL; size_t again_length = 0;
        assert(sh_package_legacy_convert(out, out_length, "boss", catalog, NULL, &again, &again_length, error, sizeof(error)));
        assert(!again && !again_length);
        assert(!sh_package_legacy_convert(zip, length, "boss", NULL, NULL, &again, &again_length, error, sizeof(error)));
        assert(!again && !again_length);
    }
    free(zip); free(out); sh_package_archive_files_free(&f); sh_package_archive_files_free(&converted);
}
static void mixed_components(void)
{
    const char root[] = " { \"id\":\"boss\", \"name\":\"Authored bundle\" } ";
    const char current[] = "{\"id\":\"current\",\"name\":\"Current component\"}";
    sh_package_archive_files f = {0}, converted = {0};
    unsigned char *zip, *out = NULL, *again = NULL;
    size_t length, out_length, again_length;
    char error[2048];
    sh_package_archive_file *marker;
    add(&f, "package.json", root);
    add(&f, "old/package.json", "{}");
    add(&f, "old/decls/entitydef/boss.decl", "old authored values");
    add(&f, "old/strings/en.json", "{\"#old\":\"Old component\"}");
    add(&f, "new/package.json", current);
    add(&f, "new/assets/generated/model/current.bmodel", "current bytes");
    zip = sh_package_archive_write(&f, &length, error, sizeof(error)); assert(zip);
    assert(sh_package_legacy_convert(zip, length, "boss", NULL, NULL, &out, &out_length, error, sizeof(error)));
    assert(out && sh_package_archive_read(out, out_length, &converted, error, sizeof(error)));
    marker = find(&converted, "package.json"); assert(marker && marker->length == strlen(root) && !memcmp(marker->body, root, marker->length));
    marker = find(&converted, "new/package.json"); assert(marker && marker->length == strlen(current) && !memcmp(marker->body, current, marker->length));
    assert(find(&converted, "old/assets/generated/decls/entitydef/boss.decl"));
    assert(find(&converted, "new/assets/generated/model/current.bmodel"));
    assert(sh_package_legacy_convert(out, out_length, "boss", NULL, NULL, &again, &again_length, error, sizeof(error)) && !again);
    free(zip); free(out); sh_package_archive_files_free(&f); sh_package_archive_files_free(&converted);

    /* A manually added id does not make old active namespaces current. */
    add(&f, "package.json", root);
    add(&f, "images/old.bimage", "old image bytes");
    zip = sh_package_archive_write(&f, &length, error, sizeof(error)); assert(zip);
    assert(sh_package_legacy_convert(zip, length, "boss", NULL, NULL, &out, &out_length, error, sizeof(error)) && out);
    assert(sh_package_archive_read(out, out_length, &converted, error, sizeof(error)));
    assert(find(&converted, "assets/generated/image/old.bimage"));
    free(zip); free(out); sh_package_archive_files_free(&f); sh_package_archive_files_free(&converted);

    /* Prose mentioning assets/ is not structural legacy evidence. */
    add(&f, "package.json", "{\"id\":\"boss\",\"name\":\"Use assets/\"}");
    add(&f, "decls/readme.txt", "auxiliary file");
    zip = sh_package_archive_write(&f, &length, error, sizeof(error)); assert(zip);
    assert(sh_package_legacy_convert(zip, length, "boss", NULL, NULL, &out, &out_length, error, sizeof(error)) && !out);
    free(zip); sh_package_archive_files_free(&f);
}
/* Optional read-only installed-data probe; no game bytes enter test fixtures. */
static void normalized_identity(void)
{
    const char *descriptor = "{\"id\":\" Alex.BOSS-Demons \\t\",\"name\":\"Campaign Boss Demons\"}";
    sh_package_archive_files files = {0}, result = {0};
    unsigned char *zip, *converted = NULL, *again = NULL;
    size_t length, converted_length, again_length;
    char error[2048], id[SH_PACKAGE_ID_CAP];
    sh_package_archive_file *marker;
    add(&files, "package.json", descriptor);
    add(&files, "assets/generated/model/boss.bmodel", "authored bytes stay exact");
    zip = sh_package_archive_write(&files, &length, error, sizeof(error)); assert(zip);
    assert(sh_package_archive_inspect(zip, length, id, NULL, error, sizeof(error)));
    assert(!strcmp(id, "alex.boss-demons"));
    assert(sh_package_legacy_convert(zip, length, id, NULL, NULL, &converted, &converted_length, error, sizeof(error)));
    assert(converted && sh_package_archive_read(converted, converted_length, &result, error, sizeof(error)));
    marker = find(&result, "package.json");
    assert(marker && strstr((char *)marker->body, "alex.boss-demons") && strstr((char *)marker->body, "Campaign Boss Demons"));
    marker = find(&result, "assets/generated/model/boss.bmodel");
    assert(marker && !strcmp((char *)marker->body, "authored bytes stay exact"));
    assert(sh_package_legacy_convert(converted, converted_length, id, NULL, NULL, &again, &again_length, error, sizeof(error)));
    assert(!again && !again_length);
    assert(!strcmp((char *)files.items[0].body, descriptor));
    free(zip); free(converted); sh_package_archive_files_free(&files); sh_package_archive_files_free(&result);
}

static int probe(const char *source, const char *base, const char *id, const char *destination)
{
    FILE *file = NULL;
    __int64 size;
    unsigned char *bytes = NULL, *out = NULL;
    size_t out_length = 0;
    char error[2048] = "", actual[SH_PACKAGE_ID_CAP];
    sh_resource_catalog *resources = NULL;
    sh_package_archive_files files = {0};
    int ok = 0;
    if (fopen_s(&file, source, "rb") || !file || _fseeki64(file, 0, SEEK_END)) goto done;
    size = _ftelli64(file);
    if (size < 0 || (uint64_t)size > SIZE_MAX || _fseeki64(file, 0, SEEK_SET)) goto done;
    bytes = malloc((size_t)size); if (!bytes || fread(bytes, 1, (size_t)size, file) != (size_t)size) goto done;
    fclose(file); file = NULL;
    resources = sh_resource_catalog_open(base, error, sizeof(error));
    if (!resources || !sh_package_legacy_convert(bytes, (size_t)size, id,
        sh_resource_catalog_legacy_read, resources, &out, &out_length, error, sizeof(error))) goto done;
    if (!out) { out = bytes; bytes = NULL; out_length = (size_t)size; }
    if (!sh_package_archive_inspect(out, out_length, actual, NULL, error, sizeof(error)) ||
        !sh_package_archive_read(out, out_length, &files, error, sizeof(error))) goto done;
    if (fopen_s(&file, destination, "wb") || !file || fwrite(out, 1, out_length, file) != out_length) goto done;
    if (fclose(file)) { file = NULL; goto done; } file = NULL;
    printf("converted id=%s files=%zu zip_bytes=%zu\n", actual, files.count, out_length); ok = 1;
done:
    if (!ok) fprintf(stderr, "probe failed: %s\n", error);
    if (file) fclose(file);
    free(bytes); free(out); sh_resource_catalog_close(resources); sh_package_archive_files_free(&files);
    return ok ? 0 : 1;
}
int main(int argc, char **argv)
{
    if (argc == 5) return probe(argv[1], argv[2], argv[3], argv[4]);
    migration(); mixed_components(); normalized_identity(); puts("legacy archive migration tests passed"); return 0;
}
