/* modkey_test.c -- the publishing key (modkey.h): made, saved and read back under DPAPI, the active and inactive
 * keys, and the passphrase backup and restore with its three refusals. */
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modkey.h"
#include "modbundle.h"
#include "mod_test_util.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

void backend_log(const char *message) { (void)message; }

static char *read_all(const char *path)
{
    size_t len = 0;
    unsigned char *t = sh_mb_read_file(path, 1u << 20, &len);
    return (char *)t;
}

static void write_all(const char *path, const char *text)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "wb") || !f) return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

int main(void)
{
    char top[MAX_PATH], folder[MAX_PATH * 2], backup[MAX_PATH * 2], bad[MAX_PATH * 2], s1[65], s2[65], id[17], id2[17];
    char *why = NULL, *text;
    sh_key *a = NULL, *b = NULL, *c = NULL, *r = NULL, *f = NULL;
    CHECK(mt_temp_dir("modkey", top, sizeof top));
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\publishing", top);
    _snprintf_s(backup, sizeof backup, _TRUNCATE, "%s\\mine" SH_KEY_BACKUP_EXT, top);
    _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\bad" SH_KEY_BACKUP_EXT, top);

    /* the first use makes the key, writes it and makes it active; the next use reads the same key */
    CHECK(sh_key_load_or_create(folder, &a, &why) && a);
    CHECK(strlen(sh_key_public(a)) == SH_KEY_PUBLIC_B64);
    CHECK(mt_has(folder, SH_KEY_ACTIVE));
    CHECK(sh_key_load_or_create(folder, &b, &why) && b && !strcmp(sh_key_public(a), sh_key_public(b)));
    CHECK(sh_key_claim_secret(a, "item1", s1) && sh_key_claim_secret(b, "item1", s2) && !strcmp(s1, s2));
    sh_key_free(b);
    b = NULL;

    /* the backup: too short a passphrase is refused; then written, and opened only with its passphrase */
    CHECK(!sh_key_backup(a, "short", backup, &why) && why && strstr(why, "at least 8 characters"));
    free(why); why = NULL;
    CHECK(sh_key_backup(a, "correct horse battery", backup, &why));
    text = read_all(backup);
    CHECK(text && strstr(text, "\"pbkdf2-sha256\":600000") && strstr(text, sh_key_public(a)) && strstr(text, "\"sealed\":\""));
    free(text);
    CHECK(sh_key_restore(backup, "wrong horse battery", &r, &why) == SH_KEY_RESTORE_PASSPHRASE && !r);
    CHECK(why && !strcmp(why, "That passphrase does not open this backup."));
    free(why); why = NULL;
    write_all(bad, "{\"format\":1,\"public\":\"nope\"}");
    CHECK(sh_key_restore(bad, "correct horse battery", &r, &why) == SH_KEY_RESTORE_DAMAGED && !r);
    CHECK(why && !strcmp(why, "This file is not a Snapmap+ publishing key backup."));
    free(why); why = NULL;

    /* a file whose public half is another key's than the one it seals: refused as damaged */
    CHECK(sh_key_create(&c, &why) && c);
    text = read_all(backup);
    if (text) {
        char *p = strstr(text, sh_key_public(a));
        if (p) memcpy(p, sh_key_public(c), SH_KEY_PUBLIC_B64);
        write_all(bad, text);
        free(text);
    }
    CHECK(sh_key_restore(bad, "correct horse battery", &r, &why) == SH_KEY_RESTORE_DAMAGED && !r);
    free(why); why = NULL;

    /* a new key replaces the active one (a lost key); restoring the backup brings the first back as active, the
     * second stays inactive, and both still derive their claims' secrets */
    CHECK(sh_key_make_active(c, folder, &why));
    CHECK(sh_key_load_or_create(folder, &b, &why) && b && !strcmp(sh_key_public(b), sh_key_public(c)));
    sh_key_free(b);
    b = NULL;
    CHECK(sh_key_restore(backup, "correct horse battery", &r, &why) == SH_KEY_RESTORE_OK && r);
    CHECK(r && !strcmp(sh_key_public(r), sh_key_public(a)));
    CHECK(r && sh_key_claim_secret(r, "item1", s2) && !strcmp(s1, s2));   /* the same claim secrets again */
    CHECK(r && sh_key_make_active(r, folder, &why));
    CHECK(sh_key_load_or_create(folder, &b, &why) && b && !strcmp(sh_key_public(b), sh_key_public(a)));
    sh_key_claim_id(c, id);
    CHECK(strlen(id) == 16);
    CHECK(sh_key_find(folder, id, &f, &why) && f && !strcmp(sh_key_public(f), sh_key_public(c)));
    sh_key_claim_id(a, id2);
    CHECK(strcmp(id, id2) != 0);
    sh_key_free(f);
    f = NULL;
    CHECK(!sh_key_find(folder, "0000000000000000", &f, &why) && !f);
    free(why);

    sh_key_free(a);
    sh_key_free(b);
    sh_key_free(c);
    sh_key_free(r);
    sh_key_free(f);
    mt_remove_tree(top);
    if (failures) { fprintf(stderr, "modkey_test: %d check(s) failed\n", failures); return 1; }
    printf("modkey_test: all checks passed\n");
    return 0;
}
