/* modkey.c -- see modkey.h. */
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "modkey.h"
#include "modbundle.h"
#include "mp_json.h"
#include "text.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

struct sh_key {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_KEY_HANDLE key;
    unsigned char point[65];          /* 0x04 || X || Y */
    unsigned char scalar[32];
    unsigned char seed[32];
    char pub[SH_KEY_PUBLIC_B64 + 1];
    char *name;                       /* UTF-8, NULL: none */
    long long created;
};

static int mk_fail(char **why, const char *text)
{
    if (why) { free(*why); *why = sh_text(text); }
    return 0;
}

static void mk_b64url(const unsigned char *in, size_t n, char *out)
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 | in[i + 2];
        out[o++] = a[v >> 18]; out[o++] = a[(v >> 12) & 63]; out[o++] = a[(v >> 6) & 63]; out[o++] = a[v & 63];
    }
    if (n - i == 1) { unsigned v = (unsigned)in[i] << 16; out[o++] = a[v >> 18]; out[o++] = a[(v >> 12) & 63]; }
    else if (n - i == 2) {
        unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8;
        out[o++] = a[v >> 18]; out[o++] = a[(v >> 12) & 63]; out[o++] = a[(v >> 6) & 63];
    }
    out[o] = 0;
}

static int mk_b64url_decode(const char *s, unsigned char *out, size_t want)
{
    size_t n = s ? strlen(s) : 0, i, o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '-' ? 62 : c == '_' ? 63 : -1;
        if (v < 0) return 0;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; if (o >= want) return 0; out[o++] = (unsigned char)(acc >> bits); }
    }
    return o == want;
}

static int mk_sha256(const void *data, size_t n, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    int ok = NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) &&
             NT_SUCCESS(BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0)) &&
             NT_SUCCESS(BCryptHashData(h, (PUCHAR)data, (ULONG)n, 0)) && NT_SUCCESS(BCryptFinishHash(h, out, 32, 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* Builds the CNG key from the scalar and the point (an ECCPRIVATE blob), and the public text. */
static int mk_import(sh_key *k)
{
    unsigned char blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96];
    BCRYPT_ECCKEY_BLOB *h = (BCRYPT_ECCKEY_BLOB *)blob;
    h->dwMagic = BCRYPT_ECDSA_PRIVATE_P256_MAGIC;
    h->cbKey = 32;
    memcpy(blob + sizeof *h, k->point + 1, 64);
    memcpy(blob + sizeof *h + 64, k->scalar, 32);
    if (!k->alg && !NT_SUCCESS(BCryptOpenAlgorithmProvider(&k->alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0))) return 0;
    if (k->key) { BCryptDestroyKey(k->key); k->key = NULL; }
    if (!NT_SUCCESS(BCryptImportKeyPair(k->alg, NULL, BCRYPT_ECCPRIVATE_BLOB, &k->key, blob, (ULONG)sizeof blob, 0))) {
        SecureZeroMemory(blob, sizeof blob);
        return 0;
    }
    SecureZeroMemory(blob, sizeof blob);
    mk_b64url(k->point, 65, k->pub);
    return 1;
}

int sh_key_create(sh_key **out, char **why)
{
    sh_key *k;
    unsigned char blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96];
    ULONG got = 0;
    BCRYPT_KEY_HANDLE pair = NULL;
    if (why) *why = NULL;
    if (!out) return 0;
    *out = NULL;
    if (!(k = (sh_key *)calloc(1, sizeof *k))) return mk_fail(why, "out of memory");
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&k->alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0)) ||
        !NT_SUCCESS(BCryptGenerateKeyPair(k->alg, &pair, 256, 0)) || !NT_SUCCESS(BCryptFinalizeKeyPair(pair, 0)) ||
        !NT_SUCCESS(BCryptExportKey(pair, NULL, BCRYPT_ECCPRIVATE_BLOB, blob, (ULONG)sizeof blob, &got, 0)) ||
        got != sizeof blob ||
        !NT_SUCCESS(BCryptGenRandom(NULL, k->seed, (ULONG)sizeof k->seed, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        if (pair) BCryptDestroyKey(pair);
        SecureZeroMemory(blob, sizeof blob);
        sh_key_free(k);
        return mk_fail(why, "Windows could not make a publishing key");
    }
    BCryptDestroyKey(pair);
    k->point[0] = 4;
    memcpy(k->point + 1, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
    memcpy(k->scalar, blob + sizeof(BCRYPT_ECCKEY_BLOB) + 64, 32);
    SecureZeroMemory(blob, sizeof blob);
    k->created = (long long)time(NULL);
    if (!mk_import(k)) { sh_key_free(k); return mk_fail(why, "Windows could not use the publishing key"); }
    *out = k;
    return 1;
}

void sh_key_free(sh_key *k)
{
    if (!k) return;
    if (k->key) BCryptDestroyKey(k->key);
    if (k->alg) BCryptCloseAlgorithmProvider(k->alg, 0);
    free(k->name);
    SecureZeroMemory(k, sizeof *k);
    free(k);
}

const char *sh_key_public(const sh_key *k) { return k ? k->pub : ""; }

int sh_key_set_name(sh_key *k, const char *name)
{
    char *copy = NULL;
    if (!k) return 0;
    if (name && name[0] && !(copy = sh_text(name))) return 0;
    free(k->name);
    k->name = copy;
    return 1;
}

char *sh_key_name_header(const char *name)
{
    static const char hexd[] = "0123456789ABCDEF";
    size_t n = name ? strlen(name) : 0, i, o = 0;
    char *out = (char *)malloc(n * 3 + 1);
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.!~*'()", c) && c)
            out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hexd[c >> 4]; out[o++] = hexd[c & 15]; }
    }
    out[o] = 0;
    return out;
}

int sh_key_claim_secret(const sh_key *k, const char *ref, char out[MODNET_HASH_HEX + 1])
{
    static const char tag[] = "snapmap-plus/claim/1\n";
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char mac[32];
    int ok, i;
    if (!k || !ref || !out) return 0;
    ok = NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG)) &&
         NT_SUCCESS(BCryptCreateHash(alg, &h, NULL, 0, (PUCHAR)k->seed, (ULONG)sizeof k->seed, 0)) &&
         NT_SUCCESS(BCryptHashData(h, (PUCHAR)tag, (ULONG)(sizeof tag - 1), 0)) &&
         NT_SUCCESS(BCryptHashData(h, (PUCHAR)ref, (ULONG)strlen(ref), 0)) && NT_SUCCESS(BCryptFinishHash(h, mac, 32, 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return 0;
    for (i = 0; i < 32; i++) _snprintf_s(out + i * 2, 3, _TRUNCATE, "%02x", mac[i]);
    SecureZeroMemory(mac, sizeof mac);
    return 1;
}

static int mk_sign(void *context, const char *method, const char *route, const char *body_sha, wchar_t **headers)
{
    sh_key *k = (sh_key *)context;
    char *name = NULL, *message = NULL;
    unsigned char digest[32], sig[64];
    char sig64[90];
    ULONG got = 0;
    int ok;
    if (headers) *headers = NULL;
    if (!k || !k->key || !method || !route || !body_sha || !headers) return 0;
    name = sh_key_name_header(k->name ? k->name : "");
    message = name ? sh_textf("snapmap-plus/request/1\n%s\n%s\n%s\n%s", method, route, name, body_sha) : NULL;
    ok = message && mk_sha256(message, strlen(message), digest) &&
         NT_SUCCESS(BCryptSignHash(k->key, NULL, digest, 32, sig, 64, &got, 0)) && got == 64;
    if (ok) {
        mk_b64url(sig, 64, sig64);
        *headers = name[0] ? sh_wtextf(L"X-Publisher-Key: %hs\r\nX-Publisher-Name: %hs\r\nX-Publisher-Signature: %hs\r\n", k->pub, name, sig64)
                           : sh_wtextf(L"X-Publisher-Key: %hs\r\nX-Publisher-Signature: %hs\r\n", k->pub, sig64);
        ok = *headers != NULL;
    }
    free(name);
    free(message);
    return ok;
}

modnet_signer sh_key_signer(sh_key *k)
{
    modnet_signer s;
    s.sign = mk_sign;
    s.context = k;
    return s;
}

/* ------------------------------------------------------------ the file */

static char *mk_fingerprint_path(const sh_key *k, const char *folder)
{
    unsigned char d[32];
    char fp[17];
    int i;
    if (!mk_sha256(k->point, 65, d)) return NULL;
    for (i = 0; i < 8; i++) _snprintf_s(fp + i * 2, 3, _TRUNCATE, "%02x", d[i]);
    return sh_textf("%s\\%s.key", folder, fp);
}

int sh_key_save(const sh_key *k, const char *folder, char **why)
{
    unsigned char secret[64];
    DATA_BLOB in, out = { 0 };
    char *b64 = NULL, *path = NULL, *doc = NULL, *nm = NULL;
    DWORD n = 0;
    wchar_t *wf, *wp;
    HANDLE h;
    DWORD put = 0;
    int ok = 0;
    if (why) *why = NULL;
    if (!k || !folder) return mk_fail(why, "no key");
    memcpy(secret, k->scalar, 32);
    memcpy(secret + 32, k->seed, 32);
    in.pbData = secret;
    in.cbData = sizeof secret;
    if (!CryptProtectData(&in, L"snapmap-plus publishing key", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        SecureZeroMemory(secret, sizeof secret);
        return mk_fail(why, "Windows could not protect the publishing key");
    }
    SecureZeroMemory(secret, sizeof secret);
    if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &n) &&
        (b64 = (char *)malloc(n + 1)) != NULL)
        ok = CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &n);
    LocalFree(out.pbData);
    if (!ok) { free(b64); return mk_fail(why, "out of memory"); }
    ok = 0;
    nm = sh_key_name_header(k->name ? k->name : "");   /* stored percent-encoded: plain ASCII JSON */
    doc = nm ? sh_textf("{\"created\":%lld,\"format\":1,\"name\":\"%s\",\"protected\":\"%s\",\"public\":\"%s\"}", k->created, nm, b64, k->pub) : NULL;
    path = mk_fingerprint_path(k, folder);
    if ((wf = sh_mb_wpath(folder)) != NULL) { CreateDirectoryW(wf, NULL); free(wf); }
    wp = path ? sh_mb_wpath(path) : NULL;
    if (doc && wp) {
        h = CreateFileW(wp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            ok = WriteFile(h, doc, (DWORD)strlen(doc), &put, NULL) && put == strlen(doc) && FlushFileBuffers(h);
            CloseHandle(h);
        }
    }
    if (!ok) mk_fail(why, "the publishing key could not be written");
    free(wp);
    free(path);
    free(doc);
    free(nm);
    free(b64);
    return ok;
}

/* The text sh_key_name_header made, decoded (malloc'd), or NULL when it is not one. */
static char *mk_percent_decode(const char *in)
{
    size_t n = strlen(in), i, w = 0;
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        if (in[i] == '%' && i + 2 < n + 0 && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = { in[i + 1], in[i + 2], 0 };
            out[w++] = (char)strtoul(hex, NULL, 16);
            i += 2;
        } else if (in[i] == '%') { free(out); return NULL; }
        else out[w++] = in[i];
    }
    out[w] = 0;
    return out;
}

int sh_key_load(const char *path, sh_key **out, char **why)
{
    size_t len = 0;
    unsigned char *text, *raw = NULL;
    mpj *root = NULL;
    const mpj *prot, *pub, *created, *nmv;
    DWORD n = 0;
    DATA_BLOB in, plain = { 0 };
    sh_key *k = NULL;
    int ok = 0;
    if (why) *why = NULL;
    if (!out) return 0;
    *out = NULL;
    if (!(text = sh_mb_read_file(path, 1u << 20, &len))) return mk_fail(why, "the publishing key cannot be read");
    root = mpj_parse((const char *)text, len);
    free(text);
    prot = root ? mpj_get(root, "protected") : NULL;
    pub = root ? mpj_get(root, "public") : NULL;
    created = root ? mpj_get(root, "created") : NULL;
    if (!prot || prot->type != MPJ_STR || !pub || pub->type != MPJ_STR) { mpj_free(root); return mk_fail(why, "This file is not a Snapmap+ publishing key."); }
    if (CryptStringToBinaryA(prot->text, 0, CRYPT_STRING_BASE64, NULL, &n, NULL, NULL) && (raw = (unsigned char *)malloc(n)) &&
        CryptStringToBinaryA(prot->text, 0, CRYPT_STRING_BASE64, raw, &n, NULL, NULL)) {
        in.pbData = raw;
        in.cbData = n;
        if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &plain) && plain.cbData == 64 &&
            (k = (sh_key *)calloc(1, sizeof *k)) != NULL) {
            memcpy(k->scalar, plain.pbData, 32);
            memcpy(k->seed, plain.pbData + 32, 32);
            /* The point is the file's public half; CNG checks it belongs to the scalar on import. */
            ok = mk_b64url_decode(pub->text, k->point, 65) && k->point[0] == 4 && mk_import(k) && !strcmp(k->pub, pub->text);
            k->created = created && mpj_is_num(created) ? mpj_int(created) : 0;
            if (ok && (nmv = mpj_get(root, "name")) != NULL && nmv->type == MPJ_STR && nmv->text[0]) {
                /* the name as it was last presented, stored percent-encoded (sh_key_save) */
                char *plain_name = mk_percent_decode(nmv->text);
                if (plain_name) { k->name = plain_name; }
            }
        }
        if (plain.pbData) { SecureZeroMemory(plain.pbData, plain.cbData); LocalFree(plain.pbData); }
    }
    free(raw);
    mpj_free(root);
    if (!ok) { sh_key_free(k); return mk_fail(why, "This publishing key cannot be opened by this Windows user."); }
    *out = k;
    return 1;
}

int sh_key_load_or_create(const char *folder, sh_key **out, char **why)
{
    char *pattern = folder ? sh_textf("%s\\*.key", folder) : NULL, *best = NULL;
    wchar_t *wp = pattern ? sh_mb_wpath(pattern) : NULL;
    WIN32_FIND_DATAW fd;
    FILETIME newest = { 0 };
    HANDLE h = wp ? FindFirstFileW(wp, &fd) : INVALID_HANDLE_VALUE;
    int ok;
    if (why) *why = NULL;
    free(pattern);
    free(wp);
    while (h != INVALID_HANDLE_VALUE) {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && CompareFileTime(&fd.ftLastWriteTime, &newest) > 0) {
            char *leaf = sh_mb_utf8(fd.cFileName);
            if (leaf) { free(best); best = sh_textf("%s\\%s", folder, leaf); newest = fd.ftLastWriteTime; }
            free(leaf);
        }
        if (!FindNextFileW(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
    }
    if (best) {
        ok = sh_key_load(best, out, why);
        free(best);
        return ok;
    }
    if (!sh_key_create(out, why)) return 0;
    if (!sh_key_save(*out, folder, why)) { sh_key_free(*out); *out = NULL; return 0; }
    return 1;
}
