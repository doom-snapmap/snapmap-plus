#include "virtual_material_native.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t u64(const unsigned char *p, size_t at)
{ uint64_t value; memcpy(&value, p + at, 8); return value; }
static int32_t i32(const unsigned char *p, size_t at)
{ int32_t value; memcpy(&value, p + at, 4); return value; }
static float f32(const unsigned char *p, size_t at)
{ float value; memcpy(&value, p + at, 4); return value; }

int main(void)
{
    sh_virtual_material_record source = {"surfaces/test", 96, 64, 32, 16, 2, 42, -123};
    sh_virtual_material_native *red, *green, *kept;
    unsigned char *a, *b, before[0x50]; char error[256]; uint64_t zero = 0;
    red = sh_virtual_material_native_create(&source, "Variants/Red", 0x1234, 256, 3, error, sizeof(error));
    green = sh_virtual_material_native_create(&source, "variants/green", 0x1234, 256, 3, error, sizeof(error));
    assert(red && green && red != green);
    a = sh_virtual_material_native_attach(red, 0x5678); b = sh_virtual_material_native_attach(green, 0x9876);
    assert(a && b && a != b && !strcmp((const char *)(uintptr_t)u64(a, 0), "variants/red"));
    assert(i32(a, 8) == -123 && i32(a, 12) == 42 && !u64(a, 16));
    assert(i32(a, 0x18) == 96 && i32(a, 0x1c) == 64 && i32(a, 0x20) == 32 && i32(a, 0x24) == 16);
    assert(i32(a, 0x28) == 2 && i32(a, 0x2c) == 3 && u64(a, 0x30) == 0x5678 && u64(a, 0x48) == 0x1234);
    assert(f32(a, 0x38) == .125f && f32(a, 0x3c) == .0625f && f32(a, 0x40) == .375f && f32(a, 0x44) == .25f);
    assert(!memcmp(a + 0x38, b + 0x38, 16)); /* shared rectangle, independent mutable associations */
    assert(!sh_virtual_material_native_attach(red, 0x9876) && u64(a, 0x30) == 0x5678);
    assert(sh_virtual_material_native_attach(red, 0x5678) == a);
    kept = red; assert(!sh_virtual_material_native_release(&red) && red == kept);
    memcpy(before, a, sizeof(before));
    assert(!sh_virtual_material_native_refresh(red, 0x5555, 100) && !memcmp(before, a, sizeof(before)));
    assert(!sh_virtual_material_native_refresh(red, 0, 512) && !memcmp(before, a, sizeof(before)));
    assert(sh_virtual_material_native_refresh(red, 0x4444, 512));
    assert(u64(a, 0x48) == 0x4444 && u64(a, 0x30) == 0x5678 && f32(a, 0x38) == .0625f);
    assert(u64(b, 0x48) == 0x1234 && f32(b, 0x38) == .125f);
    /* Model the native destructor's proven write, under a quiescent barrier. */
    memcpy(a + 0x30, &zero, 8);
    assert(sh_virtual_material_native_attach(red, 0x2222) == a && u64(a, 0x30) == 0x2222);
    memcpy(a + 0x30, &zero, 8); memcpy(b + 0x30, &zero, 8);
    assert(sh_virtual_material_native_release(&red) && !red);
    assert(sh_virtual_material_native_release(&green) && !green);
    assert(sh_virtual_material_native_release(&red));
    assert(!sh_virtual_material_native_create(&source, "bad\nname", 1, 256, 0, error, sizeof(error)));
    assert(!sh_virtual_material_native_create(&source, "valid", 0, 256, 0, error, sizeof(error)));
    assert(!sh_virtual_material_native_create(&source, "valid", 1, 100, 0, error, sizeof(error)));
    assert(!strcmp(source.name, "surfaces/test") && source.x == 96 && source.checksum == -123);
    puts("virtual material native row tests passed"); return 0;
}
