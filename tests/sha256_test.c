/* sha256_test.c -- the product's one SHA-256 (sha256.h) against the FIPS
 * 180-4 examples, fed whole and a byte at a time, and the 16-character shard
 * digest as its prefix. */
#include <stdio.h>
#include <string.h>
#include "sha256.h"
#include "map_shards.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_failed++; } } while (0)

static void hex_of(const char *msg, size_t n, int bytewise, char out[65])
{
    sh_sha256 s;
    size_t i;
    sh_sha256_init(&s);
    if (bytewise) for (i = 0; i < n; i++) sh_sha256_update(&s, msg + i, 1);
    else sh_sha256_update(&s, msg, n);
    sh_sha256_hex(&s, out, 64);
}

int main(void)
{
    static const struct { const char *msg, *digest; } v[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        { "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1" },
    };
    char out[65], shard[SH_SHARD_DIGEST_CHARS + 1];
    size_t i;
    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        hex_of(v[i].msg, strlen(v[i].msg), 0, out);
        CHECK(!strcmp(out, v[i].digest));
        hex_of(v[i].msg, strlen(v[i].msg), 1, out);
        CHECK(!strcmp(out, v[i].digest));
        sh_shard_digest16((const unsigned char *)v[i].msg, strlen(v[i].msg), shard);
        CHECK(strlen(shard) == SH_SHARD_DIGEST_CHARS && !strncmp(shard, v[i].digest, SH_SHARD_DIGEST_CHARS));
    }
    {
        /* A million 'a': many blocks through the whole-block path. */
        static char a[1000000];
        memset(a, 'a', sizeof a);
        hex_of(a, sizeof a, 0, out);
        CHECK(!strcmp(out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    }
    if (g_failed) { printf("sha256_test: %d failure(s)\n", g_failed); return 1; }
    printf("sha256_test: FIPS 180-4 examples pass\n");
    return 0;
}
