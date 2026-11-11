/* store_saves.c -- see store_saves.h. */
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "store_saves.h"
#include "store_text.h"

wchar_t *sh_store_saves_root(void)
{
    static const GUID kSavedGames = { 0x4C5C32FFu, 0xBB9D, 0x43b0, { 0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4 } };
    static const wchar_t tail[] = L"\\id Software\\DOOM\\base\\savegame.user";
    PWSTR wide = NULL;
    wchar_t base[SH_WPATH_MAX], *out;
    size_t n;
    base[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(&kSavedGames, 0, NULL, &wide)) && wide) {
        wcsncpy_s(base, SH_WPATH_MAX, wide, _TRUNCATE);
        CoTaskMemFree(wide);
    } else if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, base))) {
        wcsncat_s(base, SH_WPATH_MAX, L"\\Saved Games", _TRUNCATE);
    }
    if (!base[0]) return NULL;
    n = wcslen(base) + wcslen(tail) + 1;
    out = (wchar_t *)malloc(sizeof(wchar_t) * n);
    if (!out) return NULL;
    _snwprintf_s(out, n, _TRUNCATE, L"%s%s", base, tail);
    return out;
}

/* SHA-256, or HMAC-SHA256 with a key, of the parts given. */
static int ss_hash(const unsigned char *key, size_t key_len, const unsigned char *const *parts, const size_t *lens, int count,
                   unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    int ok = 0, i;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) != 0) return 0;
    if (BCryptCreateHash(alg, &h, NULL, 0, (PUCHAR)key, (ULONG)key_len, 0) == 0) {
        ok = 1;
        for (i = 0; i < count && ok; i++)
            ok = BCryptHashData(h, (PUCHAR)parts[i], (ULONG)lens[i], 0) == 0;
        ok = ok && BCryptFinishHash(h, out, 32, 0) == 0;
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

int sh_store_sha256(const unsigned char *bytes, size_t n, unsigned char out[32])
{
    const unsigned char *parts[1];
    size_t lens[1];
    parts[0] = bytes;
    lens[0] = n;
    return ss_hash(NULL, 0, parts, lens, 1, out);
}

int sh_store_sidecar(const unsigned char *bytes, size_t n, const unsigned char salt[8], const char *user, const char *base,
                     unsigned char out[SH_STORE_SIDECAR_LEN])
{
    static const unsigned char cyber[] = "CYBERDEMON";   /* 11 bytes with its NUL */
    const unsigned char *parts[4];
    size_t lens[4];
    unsigned char key[32];
    parts[0] = salt; lens[0] = 8;
    parts[1] = (const unsigned char *)user; lens[1] = strlen(user);
    parts[2] = cyber; lens[2] = sizeof cyber;
    parts[3] = (const unsigned char *)base; lens[3] = strlen(base);
    if (!ss_hash(NULL, 0, parts, lens, 4, key)) return 0;
    parts[0] = bytes;
    lens[0] = n;
    memcpy(out, salt, 8);
    return ss_hash(key, sizeof key, parts, lens, 1, out + 8);
}

int sh_store_sidecar_user(const char *account, const unsigned char *bytes, size_t n,
                          const unsigned char sidecar[SH_STORE_SIDECAR_LEN], const char *base, char *user, size_t cap)
{
    unsigned long long id, cand[3];
    char text[32], *end = NULL;
    unsigned char got[SH_STORE_SIDECAR_LEN];
    int i;
    if (user && cap) user[0] = 0;
    if (!account || !account[0]) return 0;
    id = _strtoui64(account, &end, 10);
    cand[0] = id;
    cand[1] = id & ~(0xfffffull << 32);                 /* instance 0 */
    cand[2] = (id & ~(0xfffffull << 32)) | (1ull << 32);   /* instance 1 */
    for (i = 0; i < 3; i++) {
        if (i == 0 && end && *end) {
            /* Not a number: the name itself is the only candidate. */
            st_put(text, sizeof text, account);
        } else {
            if (end && *end) break;
            st_putf(text, sizeof text, "%llu", cand[i]);
        }
        if (sh_store_sidecar(bytes, n, sidecar, text, base, got) && !memcmp(got, sidecar, SH_STORE_SIDECAR_LEN)) {
            if (user && cap) st_cpy(user, cap, text);
            return 1;
        }
    }
    return 0;
}
