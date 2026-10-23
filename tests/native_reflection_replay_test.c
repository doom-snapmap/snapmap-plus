/* Controls for native_reflection_replay.h: a replayed snapshot is read back by
 * the product's own registry readers (loaded entry, entityDef prepared state,
 * native resource identity with its verified ID and shader ABI fields), and
 * malformed or incomplete files are refused rather than defaulted. */
#include <windows.h>
#include "native_reflection_replay.h"
#include "decl_native_registry.h"
#include "native_resource_id.h"

static int g_failed;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); g_failed++; } } while (0)

static char g_path[MAX_PATH];
static int load_text(native_replay *r, const char *text, char *error, size_t capacity)
{
    FILE *file = NULL;
    if (fopen_s(&file, g_path, "wb") || !file) return 0;
    fputs(text, file); fclose(file);
    return native_replay_load(r, g_path, error, capacity);
}
static void hex(char *out, const char *text) { for (; *text; text++, out += 2) sprintf(out, "%02x", (unsigned char)*text); *out = 0; }
static void refused(const char *text, const char *reason)
{
    native_replay r; char error[256] = "";
    int loaded = load_text(&r, text, error, sizeof(error));
    CHECK(!loaded && strstr(error, reason));
    if (loaded || !strstr(error, reason)) fprintf(stderr, "  expected \"%s\", got \"%s\"\n", reason, error);
    if (loaded) native_replay_free(&r);
}

int main(void)
{
    static const char header[] = "H\tvk\t5ad2548ceab3dfa27f271e381222ca4a84dcac7cb83cce9446444c30c8309367\t2\t2\t2\n";
    static const char reflection[] = "C\tidThing\t\t16\t0\t\t\t1\t100\t200\nF\tint\t\tvalue\t8\t4\t\t\n"
        "E\tthingKind_t\t1\t300\t\nV\tTHING_A\t0\nM\tentityDef\tidDeclEntityDef\t\n";
    static const char entity[] = "inherit = \"base\";";
    static const char parm[] = "renderParm stagesort { }";
    char entity_hex[128], parm_hex[128], valid[2048], text[2048], error[256] = "";
    uint32_t id = 0;
    native_replay r;
    GetTempPathA(sizeof(g_path), g_path);
    strcat_s(g_path, sizeof(g_path), "native_reflection_replay_test.tsv");
    hex(entity_hex, entity); hex(parm_hex, parm);
    CHECK(sh_native_resource_id("renderParm", "stagesort", &id));
    snprintf(valid, sizeof(valid), "%s%sL\tthing/base\t7\t1\t1\t0\t0\t%zu\t1\t1\t%s\tidThing\t0\n"
        "R\trenderParm\nQ\tstagesort\t%u\t1\t1\t0\t0\t%zu\t1\t1\t%s\t1\t0\t655\t0\nQ\t\t\t\t0\n",
        header, reflection, strlen(entity), entity_hex, id, strlen(parm), parm_hex);

    /* Roundtrip through the product readers. */
    CHECK(load_text(&r, valid, error, sizeof(error)));
    if (r.arena) {
        sh_decl_registry_source source = {&r, native_replay_read, r.registry, NULL};
        sh_decl_native_registry *decls = sh_decl_native_registry_open(source, error, sizeof(error));
        sh_decl_reference reference = {0};
        sh_decl_prepared_state state = {0};
        CHECK(r.classes == 1 && r.fields == 1 && r.enums == 1 && r.managers == 1 && r.loaded == 1 &&
            r.resource_managers == 1 && r.resource_entries == 1);
        CHECK(decls && sh_decl_native_registry_resolve(decls, "idDeclEntityDef", "thing/base", &reference) == 1 && reference.resource);
        CHECK(decls && sh_decl_native_registry_entity_state(decls, &reference, &state) && state.length == strlen(entity) &&
            !memcmp(state.text, entity, state.length) && !strcmp(state.class_name, "idThing") && !state.expanded_inheritance);
        sh_decl_prepared_state_clear(&state); sh_decl_reference_clear(&reference); sh_decl_native_registry_close(decls);
        sh_native_resource_registry *resources = sh_native_resource_registry_open(
            (sh_native_resource_registry_source){&r, native_replay_read, r.resources}, error, sizeof(error));
        sh_native_resource_identity identity = {0};
        int32_t kind = -1; uint32_t index = 0, length = 0; uintptr_t at = 0; char body[64] = "";
        CHECK(resources && sh_native_resource_registry_identity(resources, "renderparm", "stagesort", &identity, error, sizeof(error)) ==
            SH_NATIVE_RESOURCE_ID_SAME_NAME && identity.resource);
        CHECK(native_replay_read(&r, identity.resource + 0x6c, &kind, 4) && kind == 0 &&
            native_replay_read(&r, identity.resource + 0x68, &index, 4) && index == 655);
        CHECK(native_replay_read(&r, identity.resource + 0x38, &at, 8) && native_replay_read(&r, identity.resource + 0x40, &length, 4) &&
            length == strlen(parm) && native_replay_read(&r, at, body, length + 1) && !memcmp(body, parm, length) && !body[length]);
        CHECK(resources && sh_native_resource_registry_validate(resources, error, sizeof(error)));
        sh_native_resource_identity_clear(&identity); sh_native_resource_registry_close(resources);
        native_replay_free(&r);
    }
    /* A replayed resource ID the name does not produce is refused by the product. */
    snprintf(text, sizeof(text), "%s%sR\trenderParm\nQ\tstagesort\t%u\t1\t1\n", header, reflection, id + 1);
    CHECK(load_text(&r, text, error, sizeof(error)));
    if (r.arena) {
        sh_native_resource_registry *resources = sh_native_resource_registry_open(
            (sh_native_resource_registry_source){&r, native_replay_read, r.resources}, error, sizeof(error));
        sh_native_resource_identity identity = {0};
        CHECK(resources && sh_native_resource_registry_identity(resources, "renderparm", "stagesort", &identity, error, sizeof(error)) ==
            SH_NATIVE_RESOURCE_ID_ERROR);
        sh_native_resource_identity_clear(&identity); sh_native_resource_registry_close(resources); native_replay_free(&r);
    }

    /* Refusals: nothing is defaulted. */
    refused("C\tidThing\t\t16\t0\t\t\t1\t\t\n", "missing header");
    snprintf(text, sizeof(text), "H\tvk\tx\t3\t2\t2\n%sR\trenderParm\n", reflection);
    refused(text, "snapshot tables are incomplete");
    snprintf(text, sizeof(text), "%sC\tidThing\t\t16\t0\t\t\t1\t\t\nF\tint\tvalue\t8\t4\n", header);
    refused(text, "malformed field");
    snprintf(text, sizeof(text), "%s%sL\t\t\t\t0\nR\trenderParm\n", header, reflection);
    refused(text, "malformed entry");
    snprintf(text, sizeof(text), "%s%sR\trenderParm\nQ\tstagesort\t%u\t1\t1\t0\t0\t4\t1\t1\tzz00zz00\t1\t0\t655\t0\n", header, reflection, id);
    refused(text, "malformed native text");
    snprintf(text, sizeof(text), "%s%s", header, reflection);
    refused(text, "snapshot tables are incomplete");
    snprintf(text, sizeof(text), "%s%sR\trenderParm\nX\tunknown\n", header, reflection);
    refused(text, "unknown or malformed line");
    snprintf(text, sizeof(text), "%s%sR\trenderParm", header, reflection);
    refused(text, "truncated");
    DeleteFileA(g_path);
    if (g_failed) fprintf(stderr, "native_reflection_replay_test: %d failure(s)\n", g_failed);
    else printf("native_reflection_replay_test: replayed snapshots read back through the product registries; malformed ones are refused\n");
    return g_failed != 0;
}
