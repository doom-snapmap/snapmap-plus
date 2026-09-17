/* Synthetic migration fixtures contain no game assets. */
#include "../src/backend/package_migration.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[2048];
static sh_package_migration *open_legacy(const char *json)
{
    sh_package_migration *m = sh_package_migration_open(json, strlen(json),
        "local.bosses", "Bosses", 1, error, sizeof(error));
    if (!m) fprintf(stderr, "%s\n", error);
    assert(m); return m;
}
static void add(sh_package_migration *m, const char *path, const char *text)
{
    int ok = sh_package_migration_add(m, path, 0, 0, text, text ? strlen(text) : 0);
    if (!ok) fprintf(stderr, "%s\n", error);
    assert(ok);
}
static char *finish(sh_package_migration *m)
{
    char *out = sh_package_migration_finish(m, NULL);
    if (!out) fprintf(stderr, "%s\n", error);
    assert(out); sh_package_migration_free(m); return out;
}
static void mapping_and_policy(void)
{
    sh_package_migration *m = open_legacy("{\"schema\":\"snapmap-plus.override-package.v1\",\"version\":\"1\",\"name\":\"Bosses\",\"description\":\"Retain me\"}");
    char *out;
    add(m, "decls/entitydef/ai/boss.decl", NULL);
    add(m, "images/boss.bimage", NULL);
    add(m, "shaders/generated/spirv/boss.spv", NULL);
    add(m, "shaders/generated/renderprogs/boss.bprog", NULL);
    add(m, "notes.txt", NULL);
    add(m, "resources/boss.manifest", "# retained identity\r\nentityDef\tai/boss\tgenerated/decls/entitydef/ai/boss.decl\r\r\n");
    add(m, "requirements/boss.requirements", "cvar\tg_useResourceBlackList\t0\r\n");
    add(m, "strings/en.json", "{\"#boss\":\"The boss\"}");
    add(m, "hud/weapons.json", "{\"schema\":\"snapmap-plus.weapon-hud.v1\",\"weapons\":{\"weapons/pistol\":{\"ammo_display\":\"weapon\"}}}");
    out = finish(m);
    assert(strstr(out, "assets/generated/decls/entitydef/ai/boss.decl"));
    assert(strstr(out, "assets/generated/image/boss.bimage"));
    assert(strstr(out, "assets/generated/spirv/boss.spv"));
    assert(strstr(out, "assets/generated/renderprogs/boss.bprog"));
    assert(strstr(out, "local.bosses") && strstr(out, "Retain me"));
    assert(strstr(out, "g_useResourceBlackList") && strstr(out, "#boss"));
    assert(strstr(out, "ammo_display") && !strstr(out, "override-package.v1"));
    assert(!strstr(out, "restart_required") && !strstr(out, "\"version\""));
    free(out);
}
static void conflicts(void)
{
    sh_package_migration *m = open_legacy("{}");
    add(m, "strings/a.json", "{\"#boss\":\"A\"}");
    assert(!sh_package_migration_add(m, "strings/b.json", 0, 0, "{\"#BOSS\":\"B\"}", 13));
    assert(!sh_package_migration_finish(m, NULL));
    sh_package_migration_free(m);
    m = open_legacy("{}");
    add(m, "resources/a.manifest", "model\ta\tmodels/boss.bmodel\n");
    assert(!sh_package_migration_add(m, "resources/b.manifest", 0, 0,
        "model\tb\tmodels/boss.bmodel\n", strlen("model\tb\tmodels/boss.bmodel\n")));
    assert(strstr(error, "different resource identities"));
    sh_package_migration_free(m);
    m = open_legacy("{}");
    assert(!sh_package_migration_add(m, "requirements/a.requirements", 0, 0,
        "cvar\tunsafe\t0", strlen("cvar\tunsafe\t0")));
    sh_package_migration_free(m);
}
static void wrappers_and_auxiliary(void)
{
    sh_package_migration *m = open_legacy("{}");
    char *out;
    add(m, "assets/decls/entitydef/ai/boss.decl", NULL);
    add(m, "assets/images/boss.bimage", NULL);
    add(m, "decls/readme.txt", NULL);
    assert(sh_package_migration_add(m, "assets/images/native", 0, 1, NULL, 0));
    assert(sh_package_migration_add(m, "shaders/generated/spirv", 1, 0, NULL, 0));
    out = finish(m);
    assert(strstr(out, "assets/generated/decls/entitydef/ai/boss.decl"));
    assert(strstr(out, "assets/generated/image/boss.bimage"));
    assert(strstr(out, "assets/images/native"));
    assert(!strstr(out, "assets/generated/decls/readme.txt"));
    free(out);
}
static void marker_and_existing_policy(void)
{
    sh_package_migration *m;
    char *out;
    m = open_legacy("\xef\xbb\xbf  "); out = finish(m); free(out);
    m = open_legacy("{\"strings\":{\"en\":{\"#boss\":\"Boss\"},\"es\":{\"#boss\":\"Jefe\"}}}");
    add(m, "strings/extra.json", "{\"#BOSS\":\"Boss\",\"#new\":\"New\"}");
    out = finish(m); assert(strstr(out, "Jefe") && strstr(out, "#new")); free(out);
    assert(!sh_package_migration_open("{\"id\":\"Bad ID\"}", 15, "valid", "Valid", 1, error, sizeof(error)));
    m = open_legacy("{\"schema\":\"author.extension\",\"custom\":{\"value\":12.00}}");
    out = finish(m); assert(strstr(out, "author.extension") && strstr(out, "12.00")); free(out);
}
int main(void)
{
    mapping_and_policy(); conflicts(); wrappers_and_auxiliary(); marker_and_existing_policy();
    puts("package migration planner tests passed"); return 0;
}
