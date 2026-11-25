/* plain_reason_test -- the table of plain reasons (plain_reason.h): every Win32 code and store status it names
 * comes out as words with a next step and no bare number, and the store's codes agree with modnet.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/plain_reason.h"
#include "../src/backend/modnet.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static const char *R(int kind, long code, const char *subject, unsigned long long bytes)
{
    static char *out;
    free(out);
    out = sh_reason(kind, code, subject, bytes);
    return out ? out : "";
}

int main(void)
{
    size_t i;
    const char *t;

    /* The named cases, word for word. */
    CHECK(!strcmp(R(SH_REASON_WIN32, ERROR_ACCESS_DENIED, "C:\\Users\\p\\mods\\beta", 0),
                  "Windows refused to write C:\\Users\\p\\mods\\beta; close the program holding it or run the game "
                  "once as the same user"));
    CHECK(!strcmp(R(SH_REASON_WIN32, ERROR_DISK_FULL, "D:\\mods\\beta", 3u * 1024u * 1024u + 1),
                  "the drive D: is full (writing D:\\mods\\beta); free at least 4 MB on it and try again"));
    CHECK(!strcmp(R(SH_REASON_WIN32, ERROR_HANDLE_DISK_FULL, "E:\\x", 1500),
                  "the drive E: is full (writing E:\\x); free at least 2 KB on it and try again"));
    CHECK(strstr(R(SH_REASON_WIN32, ERROR_DISK_FULL, "beta", 0), "the drive holding it is full") &&
          strstr(R(SH_REASON_WIN32, ERROR_DISK_FULL, "beta", 0), "free some space"));
    CHECK(!strcmp(R(SH_REASON_STORE, MODNET_E_RATE, "it is retried at the next launch", 0),
                  "the mod store is busy; it is retried at the next launch"));
    CHECK(!strcmp(R(SH_REASON_STORE, MODNET_E_UNAVAILABLE, NULL, 0), "the mod store is busy; try again in a few minutes"));
    CHECK(!strcmp(R(SH_REASON_NAME, 0xe9, "assets/generated/image/mods/a/caf\xc3\xa9.bimage", 0),
                  "assets/generated/image/mods/a/caf\xc3\xa9.bimage has the character '\xc3\xa9' (U+00E9) in its name; the "
                  "game reads only ASCII file names (letters, digits, spaces and . _ - $ + @ ~ ( )), so rename it"));
    CHECK(strstr(R(SH_REASON_NAME, '*', "a*b.decl", 0), "has the character '*' (U+002A) in its name, which the game does not accept"));
    CHECK(strstr(R(SH_REASON_NAME, 0x20ac, "x", 0), "'\xe2\x82\xac' (U+20AC)"));
    CHECK(strstr(R(SH_REASON_NAME, 0x1f600, "x", 0), "'\xf0\x9f\x98\x80' (U+1F600)"));
    CHECK(strstr(R(SH_REASON_NAME, 9, "x", 0), "has a control character (U+0009)"));

    /* The store codes the table uses are modnet.h's. */
    CHECK(MODNET_E_NOMEM == -2 && MODNET_E_CONFIG == -3 && MODNET_E_NET == -4 && MODNET_E_TIMEOUT == -5 &&
          MODNET_E_CANCELLED == -6 && MODNET_E_HTTP == -7 && MODNET_E_PROTOCOL == -8 && MODNET_E_HASH == -9 &&
          MODNET_E_TOO_LARGE == -10 && MODNET_E_NOT_FOUND == -11 && MODNET_E_FORBIDDEN == -12 &&
          MODNET_E_CONFLICT == -13 && MODNET_E_REFUSED == -14 && MODNET_E_RATE == -15 && MODNET_E_BLOCKED == -16 &&
          MODNET_E_UNAVAILABLE == -17 && MODNET_E_ARG == -1 && MODNET_E_GAME == -18 && MODNET_E_RETIRED == -19 &&
          MODNET_E_KEY == -20);

    /* Every row: words, a next step, no bare "error <n>". */
    for (i = 0; i < sizeof SH_REASON_TABLE / sizeof SH_REASON_TABLE[0]; i++) {
        const sh_reason_row *r = &SH_REASON_TABLE[i];
        t = R(r->kind, r->code, r->kind == SH_REASON_WIN32 ? "C:\\m\\f" : NULL, 0);
        CHECK(t[0] && !strstr(t, "error ") && !strstr(t, "%"));
        /* no word a player has no use for, and the table's own text names none either */
        {
            static const char *const banned[] = { "mods.store", "config.json", "claim", "the log", "system error", "salt",
                                                  "hash", "manifest", "bundle", "composition", "snapmap-plus.json", "staging" };
            size_t b;
            for (b = 0; b < sizeof banned / sizeof banned[0]; b++) CHECK(!strstr(t, banned[b]));
        }
        CHECK(strchr(t, ';') || strchr(t, ':'));   /* the reason, then what to do */
        if (r->kind == SH_REASON_WIN32) CHECK(strstr(t, "C:\\m\\f") != NULL);
        else CHECK(r->next && strstr(t, r->next) != NULL);
    }

    /* Unknown codes still say what to do; the Win32 one names the number once, as a detail. */
    t = R(SH_REASON_WIN32, 1234567, "C:\\m\\f", 0);
    CHECK(!strcmp(t, "Windows could not use C:\\m\\f; close programs that may hold it and try again"));
    CHECK(!strstr(t, "1234567") && !strstr(t, "system error"));
    CHECK(!strcmp(R(SH_REASON_STORE, 12345, NULL, 0), "the mod store did not answer; try again in a few minutes"));
    CHECK(!strcmp(R(SH_REASON_WIN32, ERROR_ACCESS_DENIED, NULL, 0),
                  "Windows refused to write the file; close the program holding it or run the game once as the same user"));
    /* The rows the player reads at an upload or a download, word for word (W1-W6, P2-3, P2-4). */
    CHECK(!strcmp(R(SH_REASON_STORE, MODNET_E_CONFIG, NULL, 0),
                  "Snapmap+'s mod store address setting is not usable; reinstall Snapmap+ to restore it"));
    CHECK(!strcmp(R(SH_REASON_STORE, MODNET_E_HTTP, NULL, 0),
                  "the mod store's address answered, but not as the mod store; reinstall Snapmap+ to restore the address"));
    CHECK(!strcmp(R(SH_REASON_STORE, MODNET_E_PROTOCOL, NULL, 0),
                  "the mod store's address answered, but not as the mod store; reinstall Snapmap+ to restore the address"));
    CHECK(!strcmp(R(SH_REASON_STORE, MODNET_E_REFUSED, NULL, 0),
                  "the mod store refused it; use Send feedback in the Snapmap+ window"));
    CHECK(!strcmp(R(SH_REASON_STORE, MODNET_E_FORBIDDEN, NULL, 0),
                  "the mod store did not accept your publishing key for this map; restore the key that published it from its "
                  "backup (Settings menu)"));
    CHECK(!strcmp(R(SH_REASON_STORE, MODNET_E_ARG, NULL, 0),
                  "a file of the map's mod content is not on this computer; check the map's mods in the Mods tab, then try again"));
    CHECK(!strcmp(R(SH_REASON_STORE, MODNET_E_GAME, NULL, 0),
                  "the content needs a different version of DOOM's own files; use the version of DOOM it was made for"));
    CHECK(!strcmp(R(SH_REASON_STORE, MODNET_E_RETIRED, NULL, 0),
                  "this version of Snapmap+ is too old for the mod store; run the Snapmap+ installer to update"));
    CHECK(!strcmp(R(SH_REASON_STORE, MODNET_E_KEY, NULL, 0),
                  "Snapmap+ could not use your publishing key; restore it from your backup in the Settings menu"));
    /* the caller's own next step replaces the table's */
    CHECK(!strcmp(R(SH_REASON_STORE, MODNET_E_KEY, "press Save a Backup", 0),
                  "Snapmap+ could not use your publishing key; press Save a Backup"));
    /* The per-thread form, and a subject of any length named whole. */
    CHECK(strstr(sh_reason_win32(ERROR_SHARING_VIOLATION, "mod.json", 0), "mod.json is open in another program") != NULL);
    {
        size_t n = 40000;
        char *long_path = (char *)malloc(n + 1);
        CHECK(long_path != NULL);
        if (long_path) {
            memset(long_path, 'a', n);
            long_path[n] = 0;
            t = sh_reason_win32(ERROR_ACCESS_DENIED, long_path, 0);
            CHECK(strstr(t, long_path) != NULL && strstr(t, "; close the program holding it") != NULL);
            free(long_path);
        }
    }

    printf("plain_reason_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
