/* modsalt.c -- see modsalt.h. */
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")   /* BCryptGenRandom */

#include "modsalt.h"

int sh_salt_valid(const char *s, size_t n)
{
    size_t i;
    if (!s || n != SH_SALT_LEN || !(s[0] >= 'a' && s[0] <= 'z')) return 0;
    for (i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9'))) return 0;
    return 1;
}

int sh_salt_generate(char out[SH_SALT_LEN + 1])
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
    static const char alnum[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned char r[SH_SALT_LEN * 2];
    size_t i, k = 0;
    if (!out) return 0;
    out[0] = 0;
    for (i = 0; i < SH_SALT_LEN; i++) {
        /* Rejection sampling keeps every character equally likely. */
        unsigned limit = i == 0 ? 26u * 9u : 36u * 7u;   /* 234 and 252: the largest multiples under 256 */
        for (;;) {
            if (k == sizeof r) k = 0;
            if (k == 0 && !BCRYPT_SUCCESS(BCryptGenRandom(NULL, r, (ULONG)sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
                return 0;
            if (r[k] < limit) break;
            k++;
        }
        out[i] = i == 0 ? letters[r[k] % 26u] : alnum[r[k] % 36u];
        k++;
    }
    out[SH_SALT_LEN] = 0;
    return 1;
}

int sh_salt_generate_except(char out[SH_SALT_LEN + 1], sh_salt_held_fn held, void *context)
{
    int round;
    for (round = 0; round < 64; round++) {
        if (!sh_salt_generate(out)) return 0;
        if (!held || !held(context, out)) return 1;
    }
    if (out) out[0] = 0;
    return 0;
}
