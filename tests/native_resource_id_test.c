#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native_resource_id.h"

static void vector(const char *type, const char *name, uint32_t expected)
{
    uint32_t actual = 0;
    assert(sh_native_resource_id(type, name, &actual) && actual == expected);
}

int main(void)
{
    /* Fixed vectors evaluated from the public MurmurHash2A reference, covering
     * all word-tail lengths and the two-stage native type/name construction. */
    vector("entityDef", "a", UINT32_C(0x06beb2d3));
    vector("entityDef", "ab", UINT32_C(0x278247cb));
    vector("entityDef", "abc", UINT32_C(0xb59955bb));
    vector("entityDef", "abcd", UINT32_C(0x8c39921c));
    vector("entityDef", "abcde", UINT32_C(0x3118df15));
    vector("entitydef", "a", UINT32_C(0x227f9501));
    vector("model", "abc", UINT32_C(0x309b8c0b));
    vector("image", "textures/stone.tga$clamp$bc7", UINT32_C(0xe2926c6d));
    vector("image", "/TEXTURES\\STONE.TGA$CLAMP$BC7", UINT32_C(0xe2926c6d));
    vector("image", "//shared/path", UINT32_C(0xf5d6ddd8));
    vector("image", "\\\\SHARED\\PATH", UINT32_C(0xf5d6ddd8));
    /* A genuine native-space collision does not imply name equality. */
    vector("image", "smp/collision/75a3", UINT32_C(0x21287b03));
    vector("image", "smp/collision/153f0", UINT32_C(0x21287b03));
    /* The raw second-stage hash is zero for this printable ASCII name. */
    vector("image", "a2ix8iqm", 1);
    char *name = sh_native_resource_name("///ROOT\\File");
    assert(name && !strcmp(name, "///root/file")); free(name);
    name = sh_native_resource_name("../Mixed Path");
    assert(name && !strcmp(name, "../mixed path")); free(name);
    uint32_t id = 17;
    assert(!sh_native_resource_id("", "name", &id) && !id);
    assert(!sh_native_resource_id("image", "", &id) && !id);
    assert(!sh_native_resource_id("image", "/", &id) && !id);
    assert(!sh_native_resource_id("image", "bad\xC0", &id) && !id);
    assert(!sh_native_resource_id("bad\xC0", "name", &id) && !id);
    assert(!sh_native_resource_id(NULL, "name", &id) && !id);
    assert(!sh_native_resource_id("image", NULL, &id) && !id);
    assert(!sh_native_resource_id("image", "name", NULL));
    puts("native_resource_id_test: PASS"); return 0;
}
