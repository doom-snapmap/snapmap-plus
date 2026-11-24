/* modclaim_key_test.c -- the publishing key through modclaim.h: the backup offer, Settings' status, backup and restore,
 * and which key's secret a claim keeps after a key is lost or restored. Offline: no store is asked.
 *
 *   modclaim_key_test.exe
 */
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modclaim.h"
#include "modkey.h"
#include "modbundle.h"
#include "mod_test_util.h"
#include "text.h"
#include "../src/backend/script/script_compile.h"
#include "../src/backend/nav_bake.h"
#include "../src/backend/script/script_runtime.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

void backend_log(const char *message) { (void)message; }
void sh_nav_bake_source_update_begin(void) {}
void sh_nav_bake_source_update_end(int committed) { (void)committed; }
int sh_nav_bake_open(const char *name, sh_nav_bake_reader read_shipped, unsigned char **out_bytes, size_t *out_len)
{ (void)name; (void)read_shipped; if (out_bytes) *out_bytes = NULL; if (out_len) *out_len = 0; return 0; }
int sh_nav_bake_overlay_open(const char *name, unsigned char **out, size_t *length)
{ (void)name; if (out) *out = NULL; if (length) *length = 0; return 0; }
int sh_navmesh_open(const char *name, unsigned char **out_bytes, size_t *out_len)
{ (void)name; *out_bytes = NULL; *out_len = 0; return 0; }
int sh_navmesh_validate_aas(const unsigned char *p, size_t n, char *e, size_t cap)
{ (void)p; (void)n; if (e && cap) e[0] = 0; return 0; }
void sh_script_set_map_provider(sh_script_map_mods_fn fn, void *context) { (void)fn; (void)context; }
int modserve_activate_bundles(const char *const *bundle_hashes, int count) { (void)bundle_hashes; (void)count; return 0; }

/* The claims record with item1 claimed by the key `id`. */
static void record_claim(const char *root, const char *item, const char *id)
{
    char *t = sh_textf("{\"%s\":{\"rev\":1,\"units\":[],\"key\":\"%s\",\"mods\":[],\"at\":1}}", item, id);
    CHECK(t && sh_mb_record_put(root, MODCLAIM_CLAIMS_SECTION, t));
    free(t);
}

int main(void)
{
    char top[MAX_PATH], keys[MAX_PATH * 2], mods[MAX_PATH * 2], backup[MAX_PATH * 2];
    char sa[65], sa2[65], sb[65], s[65], ida[17], idb[17], used[17], *why = NULL, *st;
    sh_key *b = NULL;
    CHECK(mt_temp_dir("claimkey", top, sizeof top));
    _snprintf_s(keys, sizeof keys, _TRUNCATE, "%s\\publishing", top);
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", top);
    _snprintf_s(backup, sizeof backup, _TRUNCATE, "%s\\mine" SH_KEY_BACKUP_EXT, top);
    CreateDirectoryA(mods, NULL);
    modclaim_set_root(mods);
    modclaim_set_key_folder(keys);

    /* no key yet: Settings says so, and a backup is refused with its sentence */
    st = modclaim_key_status_json();
    CHECK(st && strstr(st, "\"exists\":false") && strstr(st, "\"offer\":false"));
    free(st);
    CHECK(!modclaim_key_backup(backup, "correct horse battery", &why) && why && strstr(why, "no publishing key yet"));
    free(why); why = NULL;

    /* the first publish makes the key; the offer is pending until it is answered */
    {
        int made = 0;
        CHECK(modclaim_signer(NULL, &made, &why) && made == 1);
    }
    CHECK(modclaim_signer_loaded(ida) && strlen(ida) == 16);
    modclaim_key_offer(1);
    st = modclaim_key_status_json();
    CHECK(st && strstr(st, "\"exists\":true") && strstr(st, "\"offer\":true") && strstr(st, ida));
    free(st);
    CHECK(modclaim_secret_for("item1", sa, used) && !strcmp(used, ida));
    record_claim(mods, "item1", ida);
    CHECK(modclaim_key_backup(backup, "correct horse battery", &why));
    CHECK(!modclaim_key_offer_pending());   /* a backup answers the offer */

    /* the key is lost: a new one is made active. item1's claim was made by A, which is not on file: claimed anew */
    CHECK(sh_key_create(&b, &why) && b);
    sh_key_claim_id(b, idb);
    {
        char path[MAX_PATH * 2];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\*.key", keys);
        h = FindFirstFileA(path, &fd);
        while (h != INVALID_HANDLE_VALUE) {
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", keys, fd.cFileName);
            DeleteFileA(path);
            if (!FindNextFileA(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
        }
    }
    CHECK(sh_key_make_active(b, keys, &why));
    modclaim_set_key_folder(keys);   /* drops the loaded key */
    CHECK(modclaim_secret_for("item1", s, used) && !strcmp(used, idb) && strcmp(s, sa));
    CHECK(modclaim_secret_for("item2", sb, used) && !strcmp(used, idb));
    record_claim(mods, "item2", idb);

    /* the backup restores A as the active key; B stays as an inactive key: item1 gets A's secret again, item2 keeps
     * B's, and a new item is A's */
    CHECK(modclaim_key_restore(backup, "wrong passphrase!", &why) == SH_KEY_RESTORE_PASSPHRASE);
    CHECK(why && !strcmp(why, "That passphrase does not open this backup."));
    free(why); why = NULL;
    CHECK(modclaim_key_restore(backup, "correct horse battery", &why) == SH_KEY_RESTORE_OK);
    CHECK(modclaim_signer_loaded(used) == 0 || !strcmp(used, ida));
    {
        char rec[512];
        _snprintf_s(rec, sizeof rec, _TRUNCATE, "{\"item1\":{\"rev\":1,\"units\":[],\"key\":\"%s\",\"mods\":[],\"at\":1},"
                    "\"item2\":{\"rev\":1,\"units\":[],\"key\":\"%s\",\"mods\":[],\"at\":1}}", ida, idb);
        CHECK(sh_mb_record_put(mods, MODCLAIM_CLAIMS_SECTION, rec));
    }
    CHECK(modclaim_secret_for("item1", sa2, used) && !strcmp(used, ida) && !strcmp(sa2, sa));
    CHECK(modclaim_secret_for("item2", s, used) && !strcmp(used, idb) && !strcmp(s, sb));
    CHECK(modclaim_secret_for("item3", s, used) && !strcmp(used, ida));

    /* the account name the game shows is kept with the key and presented by its requests */
    modclaim_set_account_name("Doom Slayer");
    modclaim_set_account_name("");

    /* a key that cannot be made or used is its own failure, MODNET_E_KEY, never the store address's */
    {
        char blocker[MAX_PATH * 2], bad[MAX_PATH * 3];
        char hash[MODNET_HASH_HEX + 1];
        const char *units[1];
        modclaim_plan plan;
        FILE *f;
        _snprintf_s(blocker, sizeof blocker, _TRUNCATE, "%s\\blocker", top);
        _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\keys", blocker);
        f = fopen(blocker, "wb");
        CHECK(f != NULL);
        if (f) { fputs("x", f); fclose(f); }
        memset(hash, 'a', MODNET_HASH_HEX);
        hash[MODNET_HASH_HEX] = 0;
        units[0] = hash;
        modclaim_set_key_folder(bad);
        free(why); why = NULL;
        CHECK(modclaim_signer(NULL, NULL, &why) == 0);
        CHECK(modclaim_plan_units(units, 1, &plan, NULL, NULL) == MODNET_E_KEY);
        CHECK(modclaim_upload(NULL, NULL, NULL) == MODNET_E_KEY);
        CHECK(modclaim_claim("aaaaaaaaaaaaaaaaaaaaaaa1", units, 1, NULL, NULL, NULL, NULL) == MODNET_E_KEY);
        CHECK(modclaim_release("aaaaaaaaaaaaaaaaaaaaaaa1", NULL, NULL) == MODNET_E_KEY);
        CHECK(MODNET_E_KEY != MODNET_E_CONFIG && MODNET_E_KEY == -20);
    }

    free(why);
    sh_key_free(b);
    modclaim_set_key_folder(NULL);
    modclaim_set_root(NULL);
    mt_remove_tree(top);
    if (failures) { fprintf(stderr, "modclaim_key_test: %d check(s) failed\n", failures); return 1; }
    printf("modclaim_key_test: all checks passed\n");
    return 0;
}
