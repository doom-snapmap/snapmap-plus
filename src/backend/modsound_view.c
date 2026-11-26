#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include "modbundle.h"
#include "modsound_view.h"

static uint32_t word(const void *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static unsigned char *map_file(const char *path, uint64_t size)
{
    wchar_t *wide;
    HANDLE file, mapping;
    LARGE_INTEGER actual;
    unsigned char *raw;
    if (!path || size < 28 || size > UINT32_MAX - 15) return 0;
    wide = sh_mb_wpath(path);
    if (!wide) return NULL;
    file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (file == INVALID_HANDLE_VALUE) return 0;
    mapping = GetFileSizeEx(file, &actual) && (uint64_t)actual.QuadPart == size ?
              CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
    CloseHandle(file);
    if (!mapping) return 0;
    raw = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    return raw;
}

int sh_sound_bank_has_event(const char *path, uint64_t size, uint32_t id)
{
    unsigned char *raw = map_file(path, size);
    size_t at = 0, end, pos;
    int found = 0;
    if (!raw) return 0;
    while (size - at >= 8) {
        end = at + 8 + (size_t)word(raw + at + 4);
        if (end > size) break;
        if (!memcmp(raw + at, "HIRC", 4) && end - at >= 12) {
            uint32_t n = word(raw + at + 8);
            for (pos = at + 12; n && end - pos >= 5; n--) {
                uint32_t len = word(raw + pos + 1);
                if (len > end - pos - 5) break;
                if (raw[pos] == 4 && len >= 4 && word(raw + pos + 5) == id) { found = 1; break; }
                pos += 5 + (size_t)len;
            }
            break;
        }
        at = end;
    }
    UnmapViewOfFile(raw);
    return found;
}

int sh_sound_view_open(const char *path, uint64_t size, sh_sound_view *out)
{
    unsigned char *raw = map_file(path, size), *padded;
    size_t at, header_end, pad = 0, index = 0, index_len = 0, media_len = 0;
    memset(out, 0, sizeof *out);
    if (!raw) return 0;
    if (memcmp(raw, "BKHD", 4) || word(raw + 4) < 20 || word(raw + 8) != 113) goto bad;
    header_end = 8 + (size_t)word(raw + 4);
    for (at = 0; at < size; ) {
        uint32_t len;
        if (size - at < 8) goto bad;
        len = word(raw + at + 4);
        if (len > size - at - 8) goto bad;
        if (!memcmp(raw + at, "DIDX", 4)) {
            if (index || len % 12) goto bad;
            index = at + 8;
            index_len = len;
        }
        if (!memcmp(raw + at, "DATA", 4) && len) {
            if (media_len) goto bad;
            media_len = len;
            pad = (16 - ((at + 8) & 15)) & 15;
        }
        at += 8 + (size_t)len;
    }
    for (at = index; at < index + index_len; at += 12) {
        uint32_t offset = word(raw + at + 4);
        if (offset > media_len || word(raw + at + 8) > media_len - offset) goto bad;
    }
    out->id = word(raw + 12);
    out->size = (uint32_t)(size + pad);
    /* Keep media aligned and independent of the cache file across refreshes. */
    padded = VirtualAlloc(NULL, out->size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!padded) goto bad;
    memcpy(padded, raw, header_end);
    memcpy(padded + header_end + pad, raw + header_end, (size_t)size - header_end);
    {
        uint32_t len = (uint32_t)(header_end - 8 + pad);
        memcpy(padded + 4, &len, sizeof len);
    }
    UnmapViewOfFile(raw);
    out->padded = pad != 0;
    out->bytes = padded;
    return 1;
bad:
    UnmapViewOfFile(raw);
    memset(out, 0, sizeof *out);
    return 0;
}

void sh_sound_view_close(sh_sound_view *v)
{
    if (v && v->bytes) {
        VirtualFree(v->bytes, 0, MEM_RELEASE);
        memset(v, 0, sizeof *v);
    }
}

void **sh_sound_view_manager(const uint8_t *fn)
{
    static const uint8_t get_manager[] = { 0x48, 0x8b, 0x0d };
    static const uint8_t queue[] = { 0xff, 0x50, 0x18 };
    int32_t disp;
    if (!fn || memcmp(fn + 0x5a, get_manager, sizeof get_manager) || memcmp(fn + 0x8e, queue, sizeof queue)) return NULL;
    memcpy(&disp, fn + 0x5d, 4);
    return (void **)(fn + 0x61 + disp);
}

int sh_sound_view_queue(void *manager, const sh_sound_view *view, int unload, sh_sound_result_fn result, void *cookie)
{
    typedef int (*queue_fn)(void *, const sh_sound_request *);
    sh_sound_request q = {0};
    typedef char request_layout[(sizeof q == 56 && offsetof(sh_sound_request, bytes) == 40) ? 1 : -1];
    if (!manager || !view || !view->bytes || !result) return 0;
    q.op = unload ? 1 : 0;
    q.callback = result;
    q.cookie = cookie;
    q.source = 2;
    q.id = view->id;
    q.pool = -1;
    q.bytes = view->bytes;
    q.length = view->size;
    return ((queue_fn)(*(void ***)manager)[3])(manager, &q);
}
