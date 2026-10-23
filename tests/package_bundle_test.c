/* Complete authored snapshots survive editable inventories and provider views. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_bundle.h"
#include "config_json.h"
#include "package_fixture.h"

typedef struct bundle_reader {
    sh_package_bundle *bundle;
    const unsigned char *expected;
    size_t length;
    int ok;
} bundle_reader;

static DWORD WINAPI read_many(void *context)
{
    bundle_reader *r = context;
    r->ok = 1;
    for (size_t i = 0; i < 50; i++) {
        size_t length; char error[256];
        unsigned char *bytes = sh_package_bundle_read(r->bundle, &length, error, sizeof(error));
        if (!bytes || length != r->length || memcmp(bytes, r->expected, length)) r->ok = 0;
        free(bytes);
    }
    return 0;
}

static void retained_tree(void)
{
    sh_package_sources *sources;
    sh_package_bundle *bundle, *retained;
    unsigned char *expected, *bytes, fingerprint[32];
    char path[4096], key[SH_PACKAGE_BUNDLE_KEY_CAP], seed[SH_PACKAGE_ID_CAP], error[512];
    size_t expected_length, length;
    create("overrides/complete/package.json", " { \"id\": \"same\", \"name\": \"Complete\", \"extra\": 7 } \r\n");
    create("overrides/complete/assets/generated/decls/entitydef/unused.decl", "{ edit = { health = 17; } }");
    create("overrides/complete/Notes/Preserve.txt", "author bytes\r\n");
    create("overrides/complete/empty", NULL);
    create("overrides/complete/editor/package.json", "{\"id\":\"nested\",\"name\":\"Editor\"}");
    create("overrides/complete/editor/assets/unused.bin", "not referenced by gameplay");
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    if (!sources) return;
    expected = sh_package_archive_pack(sources, 0, &expected_length, error, sizeof(error)); CHECK(expected);
    bundle = sh_package_bundle_capture(sources, 0, root, error, sizeof(error)); CHECK(bundle);
    CHECK(!sh_package_bundle_capture(sources, 1, root, error, sizeof(error)));
    if (!bundle || !expected) goto done;
    CHECK(!memcmp(sh_package_bundle_fingerprint(bundle), sources->fingerprints[0], 32));
    sh_package_bundle_delivery_key(bundle, key);
    CHECK(sh_package_bundle_parse_key(key, fingerprint, seed) && !seed[0]);
    CHECK(!memcmp(fingerprint, sources->fingerprints[0], 32));
    /* Editable source files are not sealed or consulted by retained reads. */
    create("overrides/complete/package.json", "{\"id\":\"same\",\"name\":\"Changed\"}");
    create("overrides/complete/Notes/Preserve.txt", "different local author bytes");
    CHECK(!sh_package_bundle_capture(sources, 0, root, error, sizeof(error)) && error[0]);
    snprintf(path, sizeof(path), "%s/overrides/complete/editor/assets/unused.bin", root);
    CHECK(DeleteFileA(path));
    sh_package_sources_free(sources); sources = NULL;
    retained = sh_package_bundle_retain(bundle); sh_package_bundle_release(bundle); bundle = retained;
    bytes = sh_package_bundle_read(bundle, &length, error, sizeof(error));
    CHECK(bytes && length == expected_length && !memcmp(bytes, expected, length)); free(bytes);
    {
        bundle_reader readers[2] = {{bundle, expected, expected_length, 0}, {bundle, expected, expected_length, 0}};
        HANDLE threads[2];
        for (size_t i = 0; i < 2; i++) { threads[i] = CreateThread(NULL, 0, read_many, readers + i, 0, NULL); CHECK(threads[i]); }
        for (size_t i = 0; i < 2; i++) if (threads[i]) {
            CHECK(WaitForSingleObject(threads[i], 10000) == WAIT_OBJECT_0); CloseHandle(threads[i]); CHECK(readers[i].ok);
        }
    }
    create("overrides/complete/editor/assets/unused.bin", "replacement for fixture cleanup");
done:
    sh_package_bundle_release(bundle); free(expected); sh_package_sources_free(sources);
}

static void raw_legacy(void)
{
    const char marker[] = "{\"schema\":\"snapmap-plus.override-package.v1\",\"name\":\"Old\"}";
    const unsigned char raw[] = {0, 0xff, 0x80, 'x'};
    sh_package_archive_file members[] = {
        {"package.json", (unsigned char *)marker, sizeof(marker) - 1, 0},
        {"author-extra.bin", (unsigned char *)raw, sizeof(raw), 0},
        {"empty", NULL, 0, 1}
    };
    sh_package_archive_files files = {members, 3};
    unsigned char *archive, *read, tree[32], parsed[32];
    sh_package_bundle *bundle;
    char error[512], key[SH_PACKAGE_BUNDLE_KEY_CAP], seed[SH_PACKAGE_ID_CAP], long_seed[SH_PACKAGE_ID_CAP];
    size_t length, read_length;
    archive = sh_package_archive_write(&files, &length, error, sizeof(error)); CHECK(archive);
    if (!archive) return;
    CHECK(!sh_package_archive_identity(archive, length, NULL, NULL, error, sizeof(error)));
    CHECK(sh_package_archive_tree_identity(archive, length, tree, error, sizeof(error)));
    memset(long_seed, 'a', sizeof(long_seed) - 1); long_seed[sizeof(long_seed) - 1] = 0;
    bundle = sh_package_bundle_from_legacy_archive(archive, length, long_seed, root, error, sizeof(error)); CHECK(bundle);
    sh_package_bundle_delivery_key(bundle, key);
    CHECK(strlen(key) + 1 == SH_PACKAGE_BUNDLE_KEY_CAP);
    CHECK(sh_package_bundle_parse_key(key, parsed, seed) && !memcmp(tree, parsed, 32) && !strcmp(seed, long_seed));
    read = sh_package_bundle_read(bundle, &read_length, error, sizeof(error));
    CHECK(read && read_length == length && !memcmp(read, archive, length)); free(read);
    key[16] = 'g'; CHECK(!sh_package_bundle_parse_key(key, parsed, seed));
    sh_package_bundle_delivery_key(bundle, key); key[strlen(key) - 1] = 0;
    CHECK(!sh_package_bundle_parse_key(key, parsed, seed));
    CHECK(!sh_package_bundle_from_legacy_archive(archive, length, "BAD", root, error, sizeof(error)));
    sh_package_bundle_release(bundle);
    archive[0] ^= 1;
    CHECK(!sh_package_bundle_from_archive(archive, length, root, error, sizeof(error)));
    free(archive);
}

/* D4: the boss-demons layout -- an outer descriptor with cyberdemon, hell_guard
 * and spider_mastermind components (descriptor text copied from the installed
 * package; payload bytes are small). A map using only the cyberdemon embeds that
 * component's complete tree plus the outer files its closure resolves to, with
 * the outer descriptor recorded as their author; the other components and the
 * outer-only files it does not use stay out. */
static const char boss_outer_descriptor[] =
    "{\n  \"id\": \"local.boss-demons\",\n  \"name\": \"Boss demons\",\n"
    "  \"description\": \"Campaign bosses: Cyberdemon, Hell Guards and Spider Mastermind.\"\n}\n";
static const char boss_cyberdemon_descriptor[] =
    "{\n  \"id\": \"local.cyberdemon\",\n  \"name\": \"Cyberdemon (Boss)\",\n"
    "  \"description\": \"Campaign Cyberdemon boss, including its SnapMap encounter, toybox entry and shared game resources.\",\n"
    "  \"requirements\": {\n    \"cvars\": {\n      \"g_useResourceBlackList\": 0,\n      \"g_useImageBlackList\": 0\n    }\n  },\n"
    "  \"strings\": {\n    \"en\": {\n      \"snapmapplus_cyberdemon_boss_name\": \"Cyberdemon (Boss)\",\n"
    "      \"snapmapplus_cyberdemon_boss_toybox_desc\": \"Campaign Cyberdemon boss. Uses the campaign boss attack and animation graphs.\"\n    }\n  }\n}\n";
static const char boss_hell_guard_descriptor[] =
    "{\n  \"id\": \"local.hell-guard\",\n  \"name\": \"Hell Guards\",\n"
    "  \"description\": \"Campaign staff, hammer and combined Hell Guard resources.\",\n"
    "  \"requirements\": {\n    \"cvars\": {\n      \"g_useResourceBlackList\": 0,\n      \"g_useImageBlackList\": 0\n    }\n  }\n}\n";
static const char boss_spider_descriptor[] =
    "{\n  \"id\": \"local.spider-mastermind\",\n  \"name\": \"Spider Mastermind\",\n"
    "  \"description\": \"Campaign Spider Mastermind with SnapMap spawn behavior.\",\n"
    "  \"requirements\": {\n    \"cvars\": {\n      \"g_useResourceBlackList\": 0,\n      \"g_useImageBlackList\": 0\n    }\n  }\n}\n";

static const sh_package_archive_file *member(const sh_package_archive_files *files, const char *name)
{
    for (size_t i = 0; i < files->count; i++) if (!strcmp(files->items[i].name, name)) return files->items + i;
    return NULL;
}

static void component_delivery(void)
{
    sh_package_sources *sources;
    sh_package_bundle *outer, *delivery;
    sh_package_archive_files files = {0};
    const sh_package_archive_file *file;
    unsigned char *bytes, *outer_bytes, fingerprint[32];
    char error[512], id[SH_PACKAGE_ID_CAP], key[SH_PACKAGE_BUNDLE_KEY_CAP];
    size_t length, outer_length;
    const char *enclosing[] = {"assets/generated/decls/entitydef/ai/boss_shared.decl",
                               "assets/generated/images/boss_shared.bimage",
                               "assets/generated/decls/entitydef/ai/boss_shared.decl"};
    const char *inside[] = {"cyberdemon/assets/generated/decls/entitydef/ai/demon/cyberdemon_hell.decl"};
    const char *absent[] = {"assets/generated/decls/entitydef/ai/nothing.decl"};
    const char *collide[] = {"assets/generated/images/collide.bimage"};
    create("overrides/boss-demons/package.json", boss_outer_descriptor);
    create("overrides/boss-demons/assets/generated/decls/entitydef/ai/boss_shared.decl", "{ class = \"Entity\"; edit = { shared = 1; } }");
    create("overrides/boss-demons/assets/generated/images/boss_shared.bimage", "shared image bytes");
    create("overrides/boss-demons/assets/generated/images/collide.bimage", "the outer copy");
    create("overrides/boss-demons/assets/generated/decls/entitydef/ai/outer_only.decl", "{ edit = { unused = 1; } }");
    create("overrides/boss-demons/cyberdemon/package.json", boss_cyberdemon_descriptor);
    create("overrides/boss-demons/cyberdemon/assets/generated/decls/entitydef/ai/demon/cyberdemon_hell.decl", "{ inherit = \"boss_shared\"; edit = { health = 7000; } }");
    create("overrides/boss-demons/cyberdemon/assets/generated/decls/snapeditorentitydef/cyberdemon_hell.decl", "{ edit = { toybox = 1; } }");
    create("overrides/boss-demons/cyberdemon/assets/generated/images/collide.bimage", "the component copy");
    create("overrides/boss-demons/cyberdemon/assets/generated/md6/cyberdemon.md6mesh", "mesh bytes");
    create("overrides/boss-demons/cyberdemon/notes/author.txt", "kept as authored");
    create("overrides/boss-demons/cyberdemon/empty", NULL);
    create("overrides/boss-demons/hell_guard/package.json", boss_hell_guard_descriptor);
    create("overrides/boss-demons/hell_guard/assets/generated/decls/entitydef/ai/demon/hell_guard.decl", "{ edit = { health = 2000; } }");
    create("overrides/boss-demons/spider_mastermind/package.json", boss_spider_descriptor);
    create("overrides/boss-demons/spider_mastermind/assets/generated/decls/entitydef/ai/demon/spider_mastermind.decl", "{ edit = { health = 9000; } }");
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    if (!sources) return;
    {
        /* The retained_tree fixture is still inventoried beside this one. */
        size_t owner, components = 0;
        for (owner = 0; owner < sources->package_count; owner++)
            if (!strcmp(sources->packages[owner].name, "boss-demons")) break;
        CHECK(owner < sources->package_count);
        if (owner == sources->package_count) { sh_package_sources_free(sources); return; }
        for (size_t c = 0; c < sources->component_count; c++) if (sources->components[c].owner == owner) components++;
        CHECK(components == 4);
        outer = sh_package_bundle_capture(sources, owner, root, error, sizeof(error)); CHECK(outer);
    }
    sh_package_sources_free(sources);
    if (!outer) return;
    outer_bytes = sh_package_bundle_read(outer, &outer_length, error, sizeof(error)); CHECK(outer_bytes);
    delivery = sh_package_bundle_component(outer, "cyberdemon", enclosing, 3, root, error, sizeof(error));
    CHECK(delivery);
    if (!delivery) fprintf(stderr, "component delivery: %s\n", error);
    if (delivery) {
        bytes = sh_package_bundle_read(delivery, &length, error, sizeof(error)); CHECK(bytes);
        CHECK(bytes && length < outer_length);
        /* Its own complete delivery: the component descriptor is the root. */
        CHECK(bytes && sh_package_archive_identity(bytes, length, id, fingerprint, error, sizeof(error)));
        CHECK(!strcmp(id, "local.cyberdemon") && !memcmp(fingerprint, sh_package_bundle_fingerprint(delivery), 32));
        CHECK(memcmp(fingerprint, sh_package_bundle_fingerprint(outer), 32));
        sh_package_bundle_delivery_key(delivery, key);
        CHECK(sh_package_bundle_parse_key(key, fingerprint, NULL) && !memcmp(fingerprint, sh_package_bundle_fingerprint(delivery), 32));
        CHECK(bytes && sh_package_archive_read(bytes, length, &files, error, sizeof(error)));
        /* Nothing inside the component is trimmed, re-rooted at its folder. */
        file = member(&files, "package.json");
        CHECK(file && file->length == strlen(boss_cyberdemon_descriptor) && !memcmp(file->body, boss_cyberdemon_descriptor, file->length));
        CHECK(member(&files, "assets/generated/decls/entitydef/ai/demon/cyberdemon_hell.decl"));
        CHECK(member(&files, "assets/generated/decls/snapeditorentitydef/cyberdemon_hell.decl"));
        CHECK(member(&files, "assets/generated/md6/cyberdemon.md6mesh"));
        CHECK(member(&files, "notes/author.txt"));
        file = member(&files, "empty"); CHECK(file && file->directory);
        file = member(&files, "assets/generated/images/collide.bimage");
        CHECK(file && file->length == 18 && !memcmp(file->body, "the component copy", 18));
        /* The enclosing files its closure named, at their own paths, once each. */
        file = member(&files, "assets/generated/decls/entitydef/ai/boss_shared.decl");
        CHECK(file && !file->directory && file->length == strlen("{ class = \"Entity\"; edit = { shared = 1; } }"));
        CHECK(member(&files, "assets/generated/images/boss_shared.bimage"));
        /* The other components and the outer-only unused file stay out. */
        CHECK(!member(&files, "hell_guard/package.json") && !member(&files, "spider_mastermind/package.json"));
        CHECK(!member(&files, "assets/generated/decls/entitydef/ai/demon/hell_guard.decl"));
        CHECK(!member(&files, "assets/generated/decls/entitydef/ai/outer_only.decl"));
        CHECK(!member(&files, "cyberdemon/package.json"));
        /* The record names the enclosing author and its files. */
        file = member(&files, SH_PACKAGE_DELIVERY_METADATA);
        CHECK(file && !file->directory);
        if (file) {
            char text[1024];
            size_t n = file->length < sizeof(text) - 1 ? file->length : sizeof(text) - 1;
            memcpy(text, file->body, n); text[n] = 0;
            CHECK(strstr(text, "\"enclosing\":{\"id\":\"local.boss-demons\",\"name\":\"Boss demons\"}"));
            CHECK(strstr(text, "\"component\":\"cyberdemon\""));
            CHECK(strstr(text, "\"files\":[\"assets/generated/decls/entitydef/ai/boss_shared.decl\",\"assets/generated/images/boss_shared.bimage\"]"));
            CHECK(sh_json_validate(text, n, 8, NULL));
        }
        {
            size_t regular = 0;
            for (size_t i = 0; i < files.count; i++) if (!files.items[i].directory) regular++;
            CHECK(regular == 9);   /* 6 component files, 2 enclosing, 1 record */
        }
        sh_package_archive_files_free(&files); free(bytes);
        /* The same inputs give the same delivery, so one map carries it once. */
        {
            sh_package_bundle *again = sh_package_bundle_component(outer, "cyberdemon", enclosing, 3, root, error, sizeof(error));
            CHECK(again && !memcmp(sh_package_bundle_fingerprint(again), sh_package_bundle_fingerprint(delivery), 32));
            sh_package_bundle_release(again);
        }
        sh_package_bundle_release(delivery);
    }
    /* No enclosing file: the component tree and its record alone. */
    delivery = sh_package_bundle_component(outer, "cyberdemon", NULL, 0, root, error, sizeof(error));
    CHECK(delivery);
    if (delivery) {
        bytes = sh_package_bundle_read(delivery, &length, error, sizeof(error));
        CHECK(bytes && sh_package_archive_read(bytes, length, &files, error, sizeof(error)));
        file = member(&files, SH_PACKAGE_DELIVERY_METADATA);
        CHECK(file && file->length && strstr((const char *)file->body, "\"files\":[]"));
        CHECK(!member(&files, "assets/generated/decls/entitydef/ai/boss_shared.decl"));
        sh_package_archive_files_free(&files); free(bytes);
        sh_package_bundle_release(delivery);
    }
    /* Refusals: a path inside the component, one the tree lacks, one colliding
     * with a component member, a folder that is not a component, no path. */
    CHECK(!sh_package_bundle_component(outer, "cyberdemon", inside, 1, root, error, sizeof(error)) && strstr(error, "inside the component"));
    CHECK(!sh_package_bundle_component(outer, "cyberdemon", absent, 1, root, error, sizeof(error)) && strstr(error, "not in the delivery"));
    CHECK(!sh_package_bundle_component(outer, "cyberdemon", collide, 1, root, error, sizeof(error)) && strstr(error, "collides"));
    CHECK(!sh_package_bundle_component(outer, "notes", NULL, 0, root, error, sizeof(error)) && strstr(error, "not a component"));
    CHECK(!sh_package_bundle_component(outer, "assets", NULL, 0, root, error, sizeof(error)));
    CHECK(!sh_package_bundle_component(outer, "", NULL, 0, root, error, sizeof(error)));
    CHECK(!sh_package_bundle_component(outer, "cyberdemon/", NULL, 0, root, error, sizeof(error)));
    CHECK(!sh_package_bundle_component(NULL, "cyberdemon", NULL, 0, root, error, sizeof(error)));
    /* The outer tree itself is untouched. */
    bytes = sh_package_bundle_read(outer, &length, error, sizeof(error));
    CHECK(bytes && length == outer_length && !memcmp(bytes, outer_bytes, length));
    free(bytes); free(outer_bytes);
    sh_package_bundle_release(outer);
    /* The retained cache root is process state set once at startup. */
    CHECK(!sh_package_bundle_cache_root());
    sh_package_bundle_set_cache_root(root);
    CHECK(sh_package_bundle_cache_root() && !strcmp(sh_package_bundle_cache_root(), root));
    sh_package_bundle_set_cache_root(NULL);
    CHECK(!sh_package_bundle_cache_root());
}

int main(void)
{
    char temp[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    CHECK(GetTempPathA(sizeof(temp), temp)); CHECK(GetTempFileNameA(temp, "pb", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    retained_tree(); raw_legacy(); component_delivery();
    snprintf(pattern, sizeof(pattern), "%s/package-cache/bundles/*.zip", root);
    search = FindFirstFileA(pattern, &found); CHECK(search == INVALID_HANDLE_VALUE);
    if (search != INVALID_HANDLE_VALUE) FindClose(search);
    cleanup();
    printf("package_bundle_test: %d failures\n", failures); return failures ? 1 : 0;
}
