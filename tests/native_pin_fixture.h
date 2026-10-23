#ifndef SH_NATIVE_PIN_FIXTURE_H
#define SH_NATIVE_PIN_FIXTURE_H

/* Whole-body pin fixtures come from explicitly supplied local game images.
 * No executable bodies are distributed with the test suite. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/native_pin_hash.h"

static int sh_native_pin_fixture_digest(const unsigned char *bytes, size_t length,
                                       const unsigned char expected[32])
{
    sh_native_pin_hash hash;
    unsigned char actual[32];
    sh_native_pin_hash_init(&hash);
    sh_native_pin_hash_update(&hash, bytes, length);
    sh_native_pin_hash_finish(&hash, actual);
    return !memcmp(actual, expected, sizeof actual);
}

typedef struct sh_native_pin_fixture_image {
    unsigned char *bytes;
    size_t length;
    int attempted;
} sh_native_pin_fixture_image;
static sh_native_pin_fixture_image sh_native_pin_fixture_images[2];
static uint16_t sh_native_pin_fixture_u16(const unsigned char *p)
{ return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t sh_native_pin_fixture_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void sh_native_pin_fixture_free(void)
{
    for (unsigned i = 0; i < 2; i++) {
        free(sh_native_pin_fixture_images[i].bytes);
        sh_native_pin_fixture_images[i].bytes = NULL;
    }
}
static int sh_native_pin_fixture_copy(int renderer, uint32_t rva, void *out, size_t length)
{
    static int cleanup_registered;
    if (renderer < 0 || renderer > 1 || (!out && length)) return 0;
    sh_native_pin_fixture_image *image = sh_native_pin_fixture_images + renderer;
    if (!image->attempted) {
        const char *name = renderer ? "SNAPMAPPLUS_NATIVE_TEST_VK" : "SNAPMAPPLUS_NATIVE_TEST_GL";
        const char *path = getenv(name);
        image->attempted = 1;
        if (!path || !*path) {
            fprintf(stderr, "Native pin fixtures require %s; pass both -Doom and -DoomAlt to the test runner.\n", name);
            return 0;
        }
        FILE *file = fopen(path, "rb");
        if (!file) return 0;
        if (fseek(file, 0, SEEK_END)) { fclose(file); return 0; }
        long size = ftell(file);
        if (size < 64 || fseek(file, 0, SEEK_SET)) { fclose(file); return 0; }
        image->bytes = (unsigned char *)malloc((size_t)size);
        if (!image->bytes) { fclose(file); return 0; }
        image->length = (size_t)size;
        size_t got = fread(image->bytes, 1, image->length, file);
        fclose(file);
        if (got != image->length) {
            free(image->bytes); image->bytes = NULL; image->length = 0; return 0;
        }
        if (!cleanup_registered) { atexit(sh_native_pin_fixture_free); cleanup_registered = 1; }
    }
    const unsigned char *raw = image->bytes;
    size_t size = image->length;
    if (!raw || size < 64 || raw[0] != 'M' || raw[1] != 'Z') return 0;
    size_t pe = sh_native_pin_fixture_u32(raw + 0x3c);
    if (pe > size || size - pe < 24 || memcmp(raw + pe, "PE\0\0", 4) ||
        sh_native_pin_fixture_u16(raw + pe + 4) != 0x8664) return 0;
    size_t section = pe + 24;
    size_t optional = sh_native_pin_fixture_u16(raw + pe + 20);
    unsigned count = sh_native_pin_fixture_u16(raw + pe + 6);
    if (optional > size - section || optional < 2 ||
        sh_native_pin_fixture_u16(raw + section) != 0x20b) return 0;
    section += optional;
    if (count > (size - section) / 40) return 0;
    for (unsigned i = 0; i < count; i++, section += 40) {
        uint32_t start = sh_native_pin_fixture_u32(raw + section + 12);
        uint32_t span = sh_native_pin_fixture_u32(raw + section + 16);
        size_t offset = sh_native_pin_fixture_u32(raw + section + 20);
        if (rva < start || rva - start > span || length > span - (rva - start)) continue;
        if (offset > size || rva - start > size - offset) return 0;
        offset += rva - start;
        if (length > size - offset) return 0;
        memcpy(out, raw + offset, length);
        return 1;
    }
    return 0;
}
#endif
