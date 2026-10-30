/* modstock_migrate_test -- the overrides migration (modmigrate.c) turns the
 * unchanged stock files of a migrated pack into stock pointers.
 *
 * A synthetic install holds two entries; the retired overrides\ tree holds an
 * authored pack carrying both entries unchanged (one at its own path, one
 * renamed under the pack's id) beside one edited file, and a delivered package
 * carrying one of them. After the pass: the authored pack's bundle names two
 * pointers and its store folder holds only the edited file, the delivered
 * package's folder names one pointer, each folder's log line and the summary
 * say how many files became pointers and how many bytes were not stored, the
 * record carries the counts, and a rebuild of the migrated folder (an edit
 * under its assets) keeps the pointers its product block holds without files.
 *
 * Links the real builder, store, legacy converter and bridge; the serving
 * index is the seam stub (mod_seam_stub.c). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modmigrate.h"
#include "../src/backend/modlegacy.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modbundle.h"
#include "../src/backend/modstock.h"
#include "mod_test_util.h"
#include "mod_stock_fixture.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static char g_log[262144];
static size_t g_log_n;
void backend_log(const char *line)
{
    size_t n = line ? strlen(line) : 0;
    if (n && g_log_n + n + 2 < sizeof g_log) { memcpy(g_log + g_log_n, line, n); g_log_n += n; g_log[g_log_n++] = '\n'; g_log[g_log_n] = 0; }
}

#define P_IMAGE "generated/image/stock/b.bimage"
#define P_ANIM  "cooked/anim/stock/c.bmd6anim"

static unsigned char IMG[400], ANIM[200], EDITED[64];

static int count_files(const char *dir)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2], sub[MAX_PATH * 2];
    HANDLE h;
    int n = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
            n += count_files(sub);
        } else n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

int main(void)
{
    char tmp[MAX_PATH], doom[MAX_PATH * 2], root[MAX_PATH * 2], ov[MAX_PATH * 2], mods[MAX_PATH * 2], path[MAX_PATH * 2];
    sh_modmigrate_stats st;
    sh_ms_list l;
    size_t i, k;
    int authored = 0, delivered = 0;

    for (i = 0; i < sizeof IMG; i++) IMG[i] = (unsigned char)(i * 5 + 2);
    for (i = 0; i < sizeof ANIM; i++) ANIM[i] = (unsigned char)(i * 9 + 4);
    for (i = 0; i < sizeof EDITED; i++) EDITED[i] = (unsigned char)(i + 33);
    CHECK(mt_temp_dir("modstock_mg", tmp, sizeof tmp));
    _snprintf_s(doom, sizeof doom, _TRUNCATE, "%s\\doom", tmp);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\data", tmp);
    _snprintf_s(ov, sizeof ov, _TRUNCATE, "%s\\overrides", root);
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", root);
    {
        entry g[] = { { P_IMAGE, IMG, sizeof IMG, 0, 1 }, { P_ANIM, ANIM, sizeof ANIM, 1, 0 } };
        write_container(doom, "gameresources", g, 2);
    }
    mt_write(ov, "cyber\\package.json", "{\"id\": \"local.cyber\", \"name\": \"Cyber\"}");
    put_rel(ov, "cyber\\assets\\" P_IMAGE, IMG, sizeof IMG);
    put_rel(ov, "cyber\\assets\\cooked\\anim\\mods\\local.cyber\\c_copy.bmd6anim", ANIM, sizeof ANIM);
    put_rel(ov, "cyber\\assets\\generated\\image\\mods\\local.cyber\\edited.bimage", EDITED, sizeof EDITED);
    mt_write(ov, "map-0123456789abcdef\\doors\\package.json", "{\"id\": \"doors\", \"name\": \"SP doors\"}");
    put_rel(ov, "map-0123456789abcdef\\doors\\assets\\" P_ANIM, ANIM, sizeof ANIM);
    put_rel(ov, "map-0123456789abcdef\\doors\\assets\\generated\\image\\doors\\own.bimage", EDITED, sizeof EDITED);

    sh_modstock_set_install(doom);
    sh_modstock_register();
    sh_modmigrate_set_data_root(root);
    sh_modlegacy_set_data_root(root);
    sh_ms_set_root(mods);
    sh_modlegacy_reset();

    CHECK(sh_modmigrate_run(0, &st) == 1);
    CHECK(st.built == 1 && st.converted == 1 && st.refused == 0);
    CHECK(st.pointers == 3 && st.pointer_bytes == sizeof IMG + 2 * sizeof ANIM);
    if (st.refused || st.pointers != 3) fprintf(stderr, "%s", g_log);
    CHECK(strstr(g_log, "3 file(s), 2 of them stock pointer(s) to installed content, 0 KiB not stored") != NULL);
    CHECK(strstr(g_log, "2 file(s), 1 of them stock pointer(s) to installed content, 0 KiB not stored") != NULL);
    CHECK(strstr(g_log, "3 file(s) became stock pointers, 0 KiB not stored") != NULL);

    /* The authored pack's store folder holds only the edited file. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\cyber\\assets", mods);
    CHECK(count_files(path) == 1);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\blobs", mods);
    CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
    CHECK(sh_ms_enumerate(&l));
    for (i = 0; i < l.count; i++) {
        const sh_mb_bundle *b = &l.items[i].bundle;
        unsigned ptr = 0;
        if (!l.items[i].loaded) continue;
        for (k = 0; k < b->file_count; k++) ptr += b->files[k].stock != 0;
        if (!strcmp(b->id, "local.cyber")) { authored = 1; CHECK(ptr == 2 && b->file_count == 3 && sh_ms_entry_usable(&l.items[i])); }
        if (!strcmp(b->id, "doors")) { delivered = 1; CHECK(ptr == 1 && b->file_count == 2 && l.items[i].delivered); }
    }
    CHECK(authored && delivered);
    sh_ms_list_free(&l);
    {
        sh_ms_verify_stats vs;
        char cause[SH_MB_CAUSE_MAX];
        CHECK(sh_ms_verify("local.cyber@0.0.0", &vs, cause, sizeof cause) && vs.pointers == 2);
    }
    {
        char *record;
        size_t len = 0;
        (void)len;
        CHECK(sh_mb_record_get(mods, "migration", &record) == 1);
        CHECK(record && strstr(record, "\"stockBytes\":600,\"stockPointers\":2") && strstr(record, "\"stockBytes\":200,\"stockPointers\":1"));
        free(record);
    }
    {
        /* The rebuild rule over the migrated folder: the pointers live in its
         * product block only ("inAssets": false) and survive a rebuild. */
        sh_ms_sync_stats ss;
        sh_mb_bundle b;
        char *text;
        CHECK(sh_ms_sync(NULL, &ss) && ss.rebuilt == 0 && ss.failed == 0);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\cyber\\mod.json", mods);
        text = (char *)sh_mb_read_file(path, 1u << 20, NULL);
        CHECK(text && strstr(text, "\"inAssets\": false") && strstr(text, "\"origin\": \"migration\""));
        free(text);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\cyber", mods);
        put_rel(path, "assets\\generated\\image\\mods\\local.cyber\\added.bimage", EDITED, sizeof EDITED - 1);
        CHECK(sh_ms_sync(NULL, &ss) && ss.rebuilt == 1 && ss.failed == 0);
        if (ss.failed) fprintf(stderr, "%s", g_log);
        CHECK(sh_mb_load_folder(path, &b) && b.file_count == 4 && b.stock_count == 2 && b.build == 2 && b.legacy);
        sh_mb_free(&b);
    }

    sh_modmigrate_set_data_root(NULL);
    sh_modlegacy_set_data_root(NULL);
    sh_ms_set_root(NULL);
    sh_mb_set_stock_ops(NULL);
    sh_modstock_reset();
    mt_remove_tree(tmp);
    if (g_failed) { fprintf(stderr, "modstock_migrate_test: %d of %d checks failed\n", g_failed, g_checks); return 1; }
    printf("modstock_migrate_test: migrated packs name their unchanged stock files as pointers (%d checks)\n", g_checks);
    return 0;
}
