#include "audio_packages.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static uint32_t ap_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static unsigned ap_u16(const unsigned char *p)
{ return (unsigned)p[0] | (unsigned)p[1]<<8; }
static int ap_error(char *error, size_t capacity, int result, const char *message)
{ if (error && capacity) snprintf(error, capacity, "%s", message); return result; }

void sh_audio_package_free(sh_audio_package *package)
{
    if (!package) return;
    for (size_t i = 0; i < package->language_count; i++) free(package->languages[i].name);
    free(package->languages);
    for (size_t i = 0; i < 3; i++) free(package->entries[i]);
    memset(package, 0, sizeof(*package));
}

static int ap_languages(sh_audio_bank_source source, uint32_t length,
    sh_audio_package *out)
{
    unsigned char *bytes;
    uint32_t count;
    int result = 0;
    if (length < 4) return 0;
    bytes = malloc(length);
    if (!bytes) return -1;
    if (source.read(source.context, 28, bytes, length) != 1) { result = -1; goto done; }
    count = ap_u32(bytes);
    if (!count || count > (length - 4) / 8 || count > SIZE_MAX / sizeof(*out->languages)) goto done;
    out->languages = calloc(count, sizeof(*out->languages));
    if (!out->languages) { result = -1; goto done; }
    out->language_count = count;
    for (size_t i = 0; i < count; i++) {
        uint32_t start = ap_u32(bytes + 4 + i * 8), id = ap_u32(bytes + 8 + i * 8);
        size_t end = start, units;
        wchar_t *name;
        if (start < 4 + (uint64_t)count * 8 || start > length - 2 || (start & 1) || id > UINT16_MAX) goto done;
        while (end < length - 1 && ap_u16(bytes + end)) end += 2;
        if (end >= length - 1) goto done;
        units = (end - start) / 2;
        if (units >= SIZE_MAX / sizeof(*name)) goto done;
        name = malloc((units + 1) * sizeof(*name));
        if (!name) { result = -1; goto done; }
        out->languages[i].name = name; out->languages[i].id = id;
        for (size_t j = 0; j < units; j++) name[j] = (wchar_t)ap_u16(bytes + start + j * 2);
        name[units] = 0;
        /* Native binary search requires names in strict UTF-16 order. */
        if (i && wcscmp(out->languages[i-1].name, name) >= 0) goto done;
    }
    result = 1;
done:
    free(bytes); return result;
}

static int ap_entries(sh_audio_bank_source source, uint64_t position, uint32_t length,
    uint64_t header_end, sh_audio_package *out, unsigned kind)
{
    unsigned char bytes[6144];
    uint32_t count;
    size_t width = kind == SH_AUDIO_PACKAGE_EXTERNAL ? 24 : 20, base = 0;
    sh_audio_package_entry *rows;
    if (length < 4) return 0;
    if (source.read(source.context, position, bytes, 4) != 1) return -1;
    count = ap_u32(bytes); position += 4;
    if (count > (length - 4) / width || count > SIZE_MAX / sizeof(*rows)) return 0;
    if (!count) return 1;
    rows = calloc(count, sizeof(*rows));
    if (!rows) return -1;
    out->entries[kind] = rows; out->counts[kind] = count;
    while (base < count) {
        size_t amount = count - base;
        if (amount > sizeof(bytes) / width) amount = sizeof(bytes) / width;
        if (source.read(source.context, position, bytes, amount * width) != 1) return -1;
        for (size_t j = 0; j < amount; j++) {
            const unsigned char *p = bytes + j * width;
            sh_audio_package_entry *row = &rows[base + j];
            row->id = ap_u32(p);
            if (width == 24) row->id |= (uint64_t)ap_u32(p + 4) << 32;
            p += width - 16;
            row->block = ap_u32(p); row->length = ap_u32(p + 4);
            row->offset = (uint64_t)row->block * ap_u32(p + 8); row->language = ap_u32(p + 12);
            if (!row->block || row->offset < header_end || row->offset > source.length ||
                row->length > source.length - row->offset || row->language > UINT16_MAX) return 0;
            if (base + j) {
                const sh_audio_package_entry *prior = row - 1;
                if (row->id < prior->id || (row->id == prior->id && row->language <= prior->language)) return 0;
            }
        }
        base += amount; position += amount * width;
    }
    return 1;
}

int sh_audio_package_read(sh_audio_bank_source source, sh_audio_package *out,
    char *error, size_t capacity)
{
    unsigned char header[28];
    uint64_t header_end, total = sizeof(header), position;
    uint32_t sizes[4];
    int result = 0;
    if (error && capacity) error[0] = 0;
    if (!out) return ap_error(error, capacity, -1, "missing audio package output");
    memset(out, 0, sizeof(*out));
    if (!source.read) return ap_error(error, capacity, -1, "audio package reader is unavailable");
    if (source.length < sizeof(header)) goto done;
    if (source.read(source.context, 0, header, sizeof(header)) != 1) { result = -1; goto done; }
    header_end = (uint64_t)ap_u32(header + 4) + 8;
    if (memcmp(header, "AKPK", 4) || !ap_u32(header + 8) ||
        header_end > UINT32_MAX || header_end > source.length) goto done;
    for (size_t i = 0; i < 4; i++) { sizes[i] = ap_u32(header + 12 + i * 4); total += sizes[i]; }
    if (total > header_end) goto done;
    result = ap_languages(source, sizes[0], out);
    if (result != 1) goto done;
    position = 28 + (uint64_t)sizes[0];
    for (unsigned i = 0; i < 3; i++) {
        result = ap_entries(source, position, sizes[i+1], header_end, out, i);
        if (result != 1) goto done;
        position += sizes[i+1];
    }
    return 1;
done:
    sh_audio_package_free(out);
    return ap_error(error, capacity, result < 0 ? -1 : 0, result < 0 ?
        "audio package metadata read or allocation failed" : "malformed or unsupported audio package metadata");
}

static int ap_language_compare(const wchar_t *stored, const wchar_t *query)
{
    for (;; stored++, query++) {
        unsigned a = (unsigned)*stored, b = (unsigned)*query;
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return a < b ? -1 : 1;
        if (!a) return 0;
    }
}
int sh_audio_package_language_id(const sh_audio_package *package,
    const wchar_t *name, uint32_t *id)
{
    size_t low = 0, high;
    if (!id) return 0;
    *id = 0;
    if (!package || !name) return 0;
    high = package->language_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = ap_language_compare(package->languages[middle].name, name);
        if (!order) { *id = package->languages[middle].id; return 1; }
        if (order < 0) low = middle + 1; else high = middle;
    }
    return 0;
}
const sh_audio_package_entry *sh_audio_package_find(const sh_audio_package *package,
    sh_audio_package_kind kind, uint64_t id, uint32_t language)
{
    size_t low = 0, high;
    if (!package || kind < SH_AUDIO_PACKAGE_BANK || kind > SH_AUDIO_PACKAGE_EXTERNAL ||
        (kind != SH_AUDIO_PACKAGE_EXTERNAL && id > UINT32_MAX)) return NULL;
    high = package->counts[kind];
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const sh_audio_package_entry *row = &package->entries[kind][middle];
        if (row->id == id && row->language == language) return row;
        if (row->id < id || (row->id == id && row->language < language)) low = middle + 1;
        else high = middle;
    }
    return NULL;
}
