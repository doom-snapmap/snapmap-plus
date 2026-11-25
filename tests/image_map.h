/* A data-only copy of the supported executable for offline fixture checks. */
#ifndef SH_TEST_IMAGE_MAP_H
#define SH_TEST_IMAGE_MAP_H
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *test_image_read(const char *path, size_t *image_size, uint64_t *image_base)
{
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    IMAGE_SECTION_HEADER section;
    uint8_t *image = NULL;
    FILE *file = fopen(path, "rb");
    long length, section_pos;
    uint32_t size, headers;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET)) goto bad;
    if (fread(&dos, sizeof dos, 1, file) != 1 || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < (long)sizeof dos || fseek(file, dos.e_lfanew, SEEK_SET) || fread(&nt, sizeof nt, 1, file) != 1 ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof nt.OptionalHeader || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        goto bad;
    size = nt.OptionalHeader.SizeOfImage;
    headers = nt.OptionalHeader.SizeOfHeaders;
    section_pos = ftell(file);
    if (section_pos < 0 || section_pos > (long)headers || headers > (size_t)length || headers > size ||
        (size_t)nt.FileHeader.NumberOfSections * sizeof section > headers - (size_t)section_pos) goto bad;
    /* Some instruction readers inspect the next cache line at the end of the image. */
    image = (uint8_t *)calloc(1, (size_t)size + 64);
    if (!image || fseek(file, 0, SEEK_SET) || fread(image, 1, headers, file) != headers) goto bad;
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; i++) {
        uint32_t rva, offset, count;
        memcpy(&section, image + section_pos + i * sizeof section, sizeof section);
        rva = section.VirtualAddress;
        offset = section.PointerToRawData;
        count = section.SizeOfRawData;
        if (!count) continue;
        if (rva < headers || rva > size || count > size - rva ||
            offset > (size_t)length || count > (size_t)length - offset ||
            fseek(file, (long)offset, SEEK_SET) || fread(image + rva, 1, count, file) != count) goto bad;
    }
    fclose(file);
    if (image_size) *image_size = size;
    if (image_base) *image_base = nt.OptionalHeader.ImageBase;
    return image;
bad:
    fclose(file);
    free(image);
    return NULL;
}

static __inline uint8_t *test_image_read32(const char *path, uint32_t *size)
{
    size_t n = 0;
    uint8_t *data = test_image_read(path, &n, NULL);
    if (data) *size = (uint32_t)n;
    return data;
}
#endif
