#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "map_provenance.h"
#include "map_shards.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failed++; } } while (0)
#define MAP "{\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[" \
    "{\"info\":{\"name\":\"score\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"\",\"~type\":\"snapVarString_t\"}]}" \
    ",\"message\":\"old \x97 text; smpbind.0.1.0123456789abcdef; \xc3\xa9\"}"

static int variable_count(const char *map)
{
    const char *p = strstr(map, "\"allocCount\"");
    if (!p || !(p = strchr(p, '['))) return -1;
    for (int i = 0; i < 4; i++) { p = strchr(p, ','); if (!p) return -1; p++; }
    return atoi(p);
}
static size_t context(sh_package_provenance *value)
{
    unsigned char tree[1][32] = {{7}};
    return sh_package_provenance_context_add(value, SH_PACKAGE_CONTEXT_ISOLATED, tree, 1, "");
}
static void ordinary(void)
{
    sh_package_provenance value = {0}, read = {0}, empty = {0}; size_t index = context(&value);
    char error[256], *encoded, *stripped, *resaved, *carrier; size_t length, stripped_length, n, carrier_length;
    int present = 1;
    CHECK(sh_map_provenance_read(MAP, sizeof(MAP) - 1, &read, &present, error, sizeof(error)) && !present);
    encoded = sh_map_provenance_write(MAP, sizeof(MAP) - 1, &empty, &length, error, sizeof(error));
    CHECK(encoded && length == sizeof(MAP) - 1 && !memcmp(encoded, MAP, length)); HeapFree(GetProcessHeap(), 0, encoded);
    CHECK(sh_package_provenance_bind(&value, "entitydef", "actors/example", "smp/example", &index, 1));
    encoded = sh_map_provenance_write(MAP, sizeof(MAP) - 1, &value, &length, error, sizeof(error));
    CHECK(encoded && variable_count(encoded) == 2);
    CHECK(strstr(encoded, "old \x97 text; smpbind.0.1.0123456789abcdef; \xc3\xa9"));
    CHECK(sh_map_provenance_read(encoded, length, &read, &present, error, sizeof(error)) && present);
    CHECK(read.binding_count == 1 && !strcmp(read.bindings[0].name, "smp/example"));
    CHECK(read.semantics == SH_PACKAGE_PROVENANCE_HISTORICAL && !read.use_count);
    resaved = sh_map_provenance_write(encoded, length, &read, &n, error, sizeof(error));
    CHECK(resaved && n == length && !memcmp(resaved, encoded, length)); HeapFree(GetProcessHeap(), 0, resaved);
    stripped = sh_map_provenance_strip(encoded, length, &stripped_length, error, sizeof(error));
    CHECK(stripped && stripped_length == sizeof(MAP) - 1 && !memcmp(stripped, MAP, stripped_length));
    HeapFree(GetProcessHeap(), 0, stripped);
    stripped = sh_map_provenance_write(encoded, length, &empty, &stripped_length, error, sizeof(error));
    CHECK(stripped && stripped_length == sizeof(MAP) - 1 && !memcmp(stripped, MAP, stripped_length));
    HeapFree(GetProcessHeap(), 0, stripped);
    /* Another family's delivery bytes remain untouched while bindings change. */
    sh_shard_out package = {"smpkg.delivery.example.0.1.0123456789abcdef", "YWJj", 4};
    carrier = sh_shard_insert(encoded, length, &package, 1, &carrier_length, error, sizeof(error));
    CHECK(carrier && variable_count(carrier) == 3);
    resaved = sh_map_provenance_write(carrier, carrier_length, &read, &n, error, sizeof(error));
    CHECK(resaved && strstr(resaved, package.header) && strstr(resaved, "YWJj") && variable_count(resaved) == 3);
    stripped = sh_map_provenance_strip(resaved, n, &stripped_length, error, sizeof(error));
    CHECK(stripped && strstr(stripped, package.header) && variable_count(stripped) == 2);
    HeapFree(GetProcessHeap(), 0, stripped); HeapFree(GetProcessHeap(), 0, resaved); HeapFree(GetProcessHeap(), 0, carrier);
    /* Own corrupt headers are a metadata failure, not a historical map. */
    char *p = strstr(encoded, "smpbind.0.1."); CHECK(p != NULL); if (p) p[8] = 'x';
    CHECK(!sh_map_provenance_read(encoded, length, &read, &present, error, sizeof(error)) && !present && !read.bindings);
    HeapFree(GetProcessHeap(), 0, encoded);
    sh_package_provenance_free(&value); sh_package_provenance_free(&read);
}

static void chunks(void)
{
    sh_package_provenance value = {0}, read = {0}; size_t index = context(&value), at = 0, count = 0;
    size_t length, n, header_length, chunk_length; int present; const char *header, *chunk;
    sh_shard_out shards[64] = {0}; char *headers[64] = {0}, error[256], *encoded, *reordered, *missing, *duplicate;
    for (size_t i = 0; i < 300; i++) {
        char original[80], name[80]; snprintf(original, sizeof(original), "models/test/asset-%zu", i);
        snprintf(name, sizeof(name), "smp/bound/asset-%zu", i);
        CHECK(sh_package_provenance_bind(&value, "model", original, name, &index, 1));
    }
    encoded = sh_map_provenance_write(MAP, sizeof(MAP) - 1, &value, &length, error, sizeof(error)); CHECK(encoded);
    while (count < 64 && sh_shard_next(encoded, length, "smpbind.", 8, &at, &header, &header_length, &chunk, &chunk_length)) {
        headers[count] = malloc(header_length + 1); memcpy(headers[count], header, header_length); headers[count][header_length] = 0;
        shards[count] = (sh_shard_out){headers[count], chunk, chunk_length}; count++;
    }
    CHECK(count > 1 && count < 64 && variable_count(encoded) == (int)count + 1);
    {
        int refused = 0; unsigned removed = 9, runs = 9; size_t bytes = 9;
        char *partial = sh_shard_strip(encoded, length, "smpbind.", 8, NULL, NULL, 1,
            &bytes, &removed, &runs, &refused);
        CHECK(!partial && refused && !bytes && !removed && !runs);
        partial = sh_shard_strip(encoded, length, "smpbind.", 8, NULL, NULL, 0,
            &bytes, &removed, &runs, &refused);
        CHECK(!partial && refused && !bytes && !removed && !runs);
        partial = sh_shard_strip(encoded, length, "smpbind.", 8, NULL, NULL, SIZE_MAX,
            &bytes, &removed, &runs, &refused);
        CHECK(partial && !refused && removed == count && bytes == sizeof(MAP) - 1 && !memcmp(partial, MAP, bytes));
        HeapFree(GetProcessHeap(), 0, partial);
    }
    for (size_t i = 0; i < count / 2; i++) {
        sh_shard_out swap = shards[i]; shards[i] = shards[count - i - 1]; shards[count - i - 1] = swap;
    }
    reordered = sh_shard_insert(MAP, sizeof(MAP) - 1, shards, count, &n, error, sizeof(error)); CHECK(reordered);
    CHECK(sh_map_provenance_read(reordered, n, &read, &present, error, sizeof(error)) && present && read.binding_count == 300);
    HeapFree(GetProcessHeap(), 0, reordered);
    missing = sh_shard_insert(MAP, sizeof(MAP) - 1, shards, count - 1, &n, error, sizeof(error)); CHECK(missing);
    CHECK(!sh_map_provenance_read(missing, n, &read, &present, error, sizeof(error)) && !present && !read.bindings);
    HeapFree(GetProcessHeap(), 0, missing);
    duplicate = sh_shard_insert(encoded, length, shards, 1, &n, error, sizeof(error)); CHECK(duplicate);
    CHECK(!sh_map_provenance_read(duplicate, n, &read, &present, error, sizeof(error)) && !present);
    HeapFree(GetProcessHeap(), 0, duplicate);
    /* Valid base64 with altered content must still fail its checksum. */
    if (count && shards[0].chunk_len) ((char *)shards[0].chunk)[0] = shards[0].chunk[0] == 'A' ? 'B' : 'A';
    CHECK(!sh_map_provenance_read(encoded, length, &read, &present, error, sizeof(error)) && !present);
    CHECK(strstr(error, "checksum"));
    for (size_t i = 0; i < count; i++) free(headers[i]);
    HeapFree(GetProcessHeap(), 0, encoded); sh_package_provenance_free(&value); sh_package_provenance_free(&read);
}

static int copy_a(void *context, const sh_package_provenance_use *use, sh_package_provenance_site *site)
{
    (void)context;
    if (use->kind != SH_PACKAGE_USE_OBJECT || strcmp(use->owner, "source/a")) return 0;
    site->owner = "destination/renumbered-a"; return 1;
}
static int copy_none(void *context, const sh_package_provenance_use *use, sh_package_provenance_site *site)
{ (void)context; (void)use; (void)site; return 0; }
static void uses(void)
{
    sh_package_provenance catalog = {0}, selected = {0}, read = {0}, empty = {0};
    unsigned char trees[3][32] = {{1}, {2}, {3}}; size_t contexts[3];
    char error[256], *encoded, *resaved, *removed; size_t length, n; int present;
    for (size_t i = 0; i < 3; i++)
        contexts[i] = sh_package_provenance_context_add(&catalog, SH_PACKAGE_CONTEXT_ISOLATED, trees + i, 1, "nested/component");
    CHECK(sh_package_provenance_bind(&catalog, "entitydef", "actor", "smp/shared", contexts, 3));
    sh_package_provenance_site a = {SH_PACKAGE_USE_OBJECT, "source/a", "entityDef"};
    sh_package_provenance_site b = {SH_PACKAGE_USE_OBJECT, "source/b", "entityDef"};
    sh_package_provenance_site world = {SH_PACKAGE_USE_WORLD, "world/environment", "requiredActor"};
    CHECK(sh_package_provenance_use_set(&catalog, &a, 0, contexts, 1));
    CHECK(sh_package_provenance_use_set(&catalog, &b, 0, contexts + 1, 1));
    CHECK(sh_package_provenance_use_set(&catalog, &world, 0, contexts + 1, 1));
    encoded = sh_map_provenance_write(MAP, sizeof(MAP) - 1, &catalog, &length, error, sizeof(error));
    CHECK(encoded && strstr(encoded, "old \x97 text; smpbind.0.1.0123456789abcdef; \xc3\xa9"));
    CHECK(encoded && sh_map_provenance_read(encoded, length, &read, &present, error, sizeof(error)) && present);
    CHECK(read.semantics == SH_PACKAGE_PROVENANCE_USES && read.use_count == 3 && read.binding_count == 1 && read.context_count == 2);
    CHECK(read.uses[2].kind == SH_PACKAGE_USE_WORLD && !strcmp(read.uses[2].reference, "requiredActor"));
    HeapFree(GetProcessHeap(), 0, encoded);
    CHECK(sh_package_provenance_select_uses(&read, copy_a, NULL, &selected));
    CHECK(selected.use_count == 1 && selected.context_count == 1 && selected.contexts[0].trees[0][0] == 1);
    encoded = sh_map_provenance_write(MAP, sizeof(MAP) - 1, &selected, &length, error, sizeof(error));
    CHECK(encoded && sh_map_provenance_read(encoded, length, &read, &present, error, sizeof(error)) && present);
    CHECK(!strcmp(read.uses[0].owner, "destination/renumbered-a") && !strcmp(read.uses[0].reference, "entityDef"));
    CHECK(read.contexts[0].trees[0][0] == 1 && !strcmp(read.contexts[0].component, "nested/component"));
    resaved = sh_map_provenance_write(encoded, length, &read, &n, error, sizeof(error));
    CHECK(resaved && n == length && !memcmp(resaved, encoded, n)); HeapFree(GetProcessHeap(), 0, resaved);
    CHECK(sh_package_provenance_select_uses(&selected, copy_none, NULL, &empty));
    /* Keeping a provider catalog for future placements cannot tag an empty map. */
    size_t unused = context(&empty);
    CHECK(sh_package_provenance_bind(&empty, "entitydef", "unused", "smp/unused", &unused, 1));
    CHECK(empty.semantics == SH_PACKAGE_PROVENANCE_USES && empty.binding_count && !empty.use_count);
    removed = sh_map_provenance_write(encoded, length, &empty, &n, error, sizeof(error));
    CHECK(removed && n == sizeof(MAP) - 1 && !memcmp(removed, MAP, n)); HeapFree(GetProcessHeap(), 0, removed);
    selected.uses[0].binding = SIZE_MAX;
    resaved = sh_map_provenance_write(encoded, length, &selected, &n, error, sizeof(error));
    CHECK(!resaved && !n);
    CHECK(sh_map_provenance_read(encoded, length, &read, &present, error, sizeof(error)) && present && read.use_count == 1);
    HeapFree(GetProcessHeap(), 0, encoded);
    sh_package_provenance_free(&catalog); sh_package_provenance_free(&selected);
    sh_package_provenance_free(&read); sh_package_provenance_free(&empty);
}
static void rows(void)
{
    /* The exact editor row is optional per-use data inside the same carrier;
     * the old-map bytes stay those of a map without rows. */
    static const char row[] = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    sh_package_provenance value = {0}, read = {0};
    unsigned char trees[1][32] = {{1}}; size_t key; const char *found = NULL;
    char error[256], *plain, *encoded, *resaved; size_t plain_length, length, n; int present;
    key = sh_package_provenance_context_add(&value, SH_PACKAGE_CONTEXT_ISOLATED, trees, 1, "");
    CHECK(sh_package_provenance_bind(&value, "entitydef", "actor", "smp/actor", &key, 1));
    sh_package_provenance_site a = {SH_PACKAGE_USE_OBJECT, "entity/3", "entityDef"};
    CHECK(sh_package_provenance_use_set(&value, &a, 0, &key, 1));
    plain = sh_map_provenance_write(MAP, sizeof(MAP) - 1, &value, &plain_length, error, sizeof(error));
    CHECK(plain && sh_map_provenance_read(plain, plain_length, &read, &present, error, sizeof(error)) && present);
    CHECK(sh_package_provenance_row_get(&read, "entity/3", &found) == 0);
    CHECK(sh_package_provenance_row_set(&value, "entity/3", row) == 1);
    encoded = sh_map_provenance_write(MAP, sizeof(MAP) - 1, &value, &length, error, sizeof(error));
    CHECK(encoded && sh_map_provenance_read(encoded, length, &read, &present, error, sizeof(error)) && present);
    CHECK(sh_package_provenance_row_get(&read, "entity/3", &found) == 1 && !strcmp(found, row));
    resaved = sh_map_provenance_write(encoded, length, &read, &n, error, sizeof(error));
    CHECK(resaved && n == length && !memcmp(resaved, encoded, n)); HeapFree(GetProcessHeap(), 0, resaved);
    CHECK(sh_package_provenance_row_set(&read, "entity/3", NULL) == 1);
    resaved = sh_map_provenance_write(encoded, length, &read, &n, error, sizeof(error));
    CHECK(resaved && n == plain_length && !memcmp(resaved, plain, n)); HeapFree(GetProcessHeap(), 0, resaved);
    HeapFree(GetProcessHeap(), 0, plain); HeapFree(GetProcessHeap(), 0, encoded);
    sh_package_provenance_free(&value); sh_package_provenance_free(&read);
}

int main(void)
{
    ordinary(); chunks(); uses(); rows(); printf("map_provenance_test: %s\n", failed ? "FAILED" : "PASS"); return failed ? 1 : 0;
}
