/* Which strings keys are string edits (strkeys.h). */
#include <string.h>
#include "strkeys.h"
#include "strkeys_game.h"
#include "strids_baked.h"

static unsigned long long sk_hash(const char *key, size_t n)
{
    unsigned long long h = 0xcbf29ce484222325ull;
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)key[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

int sh_strkey_is_edit(const char *key, size_t n)
{
    unsigned long long h;
    size_t lo = 0, hi = SH_STRKEYS_GAME_COUNT, i;
    if (!key || !n) return 0;
    for (i = 0; i < B1_STRIDS_BAKED_COUNT; i++)
        if (strlen(g_strids_baked[i].id) == n && !_strnicmp(g_strids_baked[i].id, key, n)) return 1;
    h = sk_hash(key, n);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_strkeys_game[mid] == h) return 1;
        if (g_strkeys_game[mid] < h) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}
