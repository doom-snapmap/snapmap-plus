/* The saved-map guard against a faked engine: DeleteBadSaveSlots is handed an
 * idList<idStr> laid out as the engine's, the slot folders are real files in a
 * temporary tree, and the guard must delete nothing, keep a byte-exact copy,
 * and claim only what it verified. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/fault_shield/saved_map_guard.c"

static int failed, emitted, tells, prepared, stolen_seen;
static char told[4096];
#define CHECK(x) do { if (!(x)) { ++failed; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

void shield_emit(const shield_fault *f) { (void)f; emitted++; }
void shield_tell(const char *text) { tells++; strcpy_s(told, sizeof told, text); }
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)detour; prepared++; stolen_seen = (int)stolen; return target; }
sh_patch_status hook_commit(void *t) { (void)t; return B2_PATCH_OK; }
int hook_unpatch(void *t) { (void)t; return 1; }
static sig_result g_sig;
const sig_result *sig_lookup(const sig_result *r, size_t n, const char *name)
{ (void)n; return r && name && !strcmp(name, "DeleteBadSaveSlots") ? r : NULL; }

/* The engine's idStr (0x30 bytes: length at +8, text at +0x10) and idList. */
typedef struct fake_idstr { void *vt; int len; int pad; const char *data; unsigned char rest[0x18]; } fake_idstr;
typedef struct fake_list { fake_idstr *items; int count; int cap; } fake_list;

static wchar_t root[MAX_PATH], saves[MAX_PATH], kept[MAX_PATH];
static const char ID1[] = "AAAA6E84712D438B91B0", ID2[] = "0123456789ABCDEF0123";

static void wfile(const wchar_t *dir, const wchar_t *name, const char *bytes)
{
    wchar_t p[MAX_PATH];
    HANDLE h;
    DWORD w;
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, name);
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    WriteFile(h, bytes, (DWORD)strlen(bytes), &w, NULL);
    CloseHandle(h);
}

/* Remove this test's own temporary tree. */
static void cleanup(void)
{
    wchar_t from[MAX_PATH + 2] = {0};
    SHFILEOPSTRUCTW op = {0};
    wcscpy_s(from, MAX_PATH, root);
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

static void run(const char *const *names, int n)
{
    fake_idstr items[4];
    fake_list list = { items, n, 4 };
    int i;
    memset(items, 0, sizeof items);
    for (i = 0; i < n; i++) { items[i].data = names[i]; items[i].len = (int)strlen(names[i]); }
    smg_delete_bad_slots(NULL, &list);
}

int main(void)
{
    wchar_t tmp[MAX_PATH], slot[MAX_PATH], acct[MAX_PATH], p1[MAX_PATH], p2[MAX_PATH];
    const char *names[3];
    char want[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    _snwprintf_s(root, MAX_PATH, _TRUNCATE, L"%ssmg_test_%lu", tmp, GetCurrentProcessId());
    CreateDirectoryW(root, NULL);
    _snwprintf_s(saves, MAX_PATH, _TRUNCATE, L"%s\\savegame.user", root);
    _snwprintf_s(acct, MAX_PATH, _TRUNCATE, L"%s\\7656119", saves);
    _snwprintf_s(slot, MAX_PATH, _TRUNCATE, L"%s\\SNAPMAPS%S", acct, ID1);
    _snwprintf_s(kept, MAX_PATH, _TRUNCATE, L"%s\\snapmap-plus\\kept-maps", root);
    CreateDirectoryW(saves, NULL);
    CreateDirectoryW(acct, NULL);
    CreateDirectoryW(slot, NULL);
    wfile(slot, L"map.decl", "zlib bytes the build cannot read");
    wfile(slot, L"map.decl.verify", "0123456789012345678901234567890123456789");
    wfile(slot, L"game.details", "checksum=1\nmetadata={\"name\":\"test5\",\"localId\":\"x\"}\n");
    wcscpy_s(g_saves_root, SH_WPATH_MAX, saves);
    wcscpy_s(g_kept_root, SH_WPATH_MAX, kept);

    /* A rejected slot is never deleted; a byte-exact copy is kept and named. */
    names[0] = "SNAPMAPSAAAA6E84712D438B91B0";
    names[1] = ID2;                        /* no folder: nothing claimed */
    names[2] = "not a slot";
    run(names, 3);
    _snwprintf_s(p1, MAX_PATH, _TRUNCATE, L"%s\\map.decl", slot);
    _snwprintf_s(p2, MAX_PATH, _TRUNCATE, L"%s\\SNAPMAPS%S\\map.decl", kept, ID1);
    CHECK(GetFileAttributesW(p1) != INVALID_FILE_ATTRIBUTES);
    CHECK(smg_same_file(p1, p2));
    _snwprintf_s(p1, MAX_PATH, _TRUNCATE, L"%s\\game.details", slot);
    _snwprintf_s(p2, MAX_PATH, _TRUNCATE, L"%s\\SNAPMAPS%S\\game.details", kept, ID1);
    CHECK(smg_same_file(p1, p2));
    CHECK(tells == 1 && strstr(told, "\"test5\"") && strstr(told, "kept a copy in"));
    sh_wpath_utf8(kept, want, sizeof want);
    CHECK(strstr(told, want) != NULL);
    CHECK(emitted >= 2);

    /* The scan runs at every My Maps visit: one sentence per slot per session,
     * and an identical copy is not made twice. */
    run(names, 1);
    CHECK(tells == 1);
    _snwprintf_s(p2, MAX_PATH, _TRUNCATE, L"%s\\SNAPMAPS%S-2", kept, ID1);
    CHECK(GetFileAttributesW(p2) == INVALID_FILE_ATTRIBUTES);

    /* A copy holding other bytes is never overwritten. */
    wfile(slot, L"map.decl", "a later save");
    g_told_count = 0;
    run(names, 1);
    CHECK(tells == 2 && GetFileAttributesW(p2) != INVALID_FILE_ATTRIBUTES);
    _snwprintf_s(p1, MAX_PATH, _TRUNCATE, L"%s\\map.decl", slot);
    wcscat_s(p2, MAX_PATH, L"\\map.decl");
    CHECK(smg_same_file(p1, p2));

    /* When no copy can be made the sentence says only where the files still are. */
    _snwprintf_s(g_kept_root, SH_WPATH_MAX, _TRUNCATE, L"%s\\map.decl\\kept", slot);
    g_told_count = 0;
    run(names, 1);
    CHECK(tells == 3 && strstr(told, "its files are still in") && !strstr(told, "kept a copy"));
    _snwprintf_s(p1, MAX_PATH, _TRUNCATE, L"%s\\map.decl", slot);
    CHECK(GetFileAttributesW(p1) != INVALID_FILE_ATTRIBUTES);

    /* An unreadable list deletes nothing and does not fault. */
    smg_delete_bad_slots(NULL, (void *)(uintptr_t)0x10);

    /* Installed only on a resolved signature, with the 19-byte window. */
    CHECK(!saved_map_guard_install(NULL, 0));
    g_sig.name = "DeleteBadSaveSlots"; g_sig.status = SIG_OK; g_sig.addr = (uintptr_t)&g_sig;
    CHECK(saved_map_guard_install(&g_sig, 1) && prepared == 1 && stolen_seen == 19);

    printf("saved_map_guard_test: %d failures\n", failed);
    return failed ? 1 : 0;
}
