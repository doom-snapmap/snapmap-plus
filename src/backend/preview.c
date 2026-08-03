/* preview.c -- see preview.h. The asset-preview transport and nothing else: request staging, RGBA ->
 * 24bpp BMP -> base64 data URI, and the cross-thread handoff to the UI. No engine calls, no detours, no
 * renderer state; this file is pure CPU and links against nothing but the CRT and Win32. */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "preview.h"
#include "backend_log.h"

/* ---- published image -------------------------------------------------------------------------------
 * BMP rather than PNG on purpose: no compression, no zlib dependency, and every browser renders
 * `data:image/bmp;base64,...` in an <img> directly. Written top-down via a NEGATIVE biHeight so the
 * source rows need no flipping.
 *
 * The producer thread swaps-and-FREES this buffer while the UI thread may be reading it through iface
 * ext 13, so the two are serialized. SRWLOCK because it needs no runtime init and the read side is
 * shared: concurrent UI fetches do not contend with each other, only with the (rare) producer swap. */
static char          *g_preview_b64   = NULL;
static volatile LONG  g_preview_ready = 0;
static SRWLOCK        g_preview_lock  = SRWLOCK_INIT;

/* ---- staged request --------------------------------------------------------------------------------
 * Written by the UI thread in sh_preview_request, read by the producer in sh_preview_take_request. Held
 * under the same lock as the image: contention is negligible (one write per user click) and one lock is
 * one fewer ordering rule to get wrong. */
static char           g_requested[512] = { 0 };
static volatile LONG  g_request_gen    = 0;   /* bumped per request; log correlation only */

int sh_preview_get(char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';

    AcquireSRWLockShared(&g_preview_lock);
    int rc = 0;
    if (g_preview_ready && g_preview_b64) {
        size_t len = strlen(g_preview_b64);
        if (len + 1 > cap) {
            rc = -(int)(len + 1);                /* negative = required size, so the UI can re-ask */
        } else {
            memcpy(out, g_preview_b64, len + 1);
            rc = (int)len;
        }
    }
    ReleaseSRWLockShared(&g_preview_lock);
    return rc;
}

void sh_preview_request(const char *name)
{
    if (!name || !*name) return;

    AcquireSRWLockExclusive(&g_preview_lock);
    strncpy_s(g_requested, sizeof g_requested, name, _TRUNCATE);
    /* Invalidate the current image BEFORE the generation bump, or a poll can see the new request with
     * the previous request's picture still marked ready and stop polling one image too early. */
    InterlockedExchange(&g_preview_ready, 0);
    LONG gen = InterlockedIncrement(&g_request_gen);
    ReleaseSRWLockExclusive(&g_preview_lock);

    char line[600];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: preview REQUEST #%ld -- '%s' staged (no producer installed yet; see preview.h)",
        gen, name);
    backend_log(line);
}

int sh_preview_take_request(char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';

    AcquireSRWLockShared(&g_preview_lock);
    int pending = (!g_preview_ready && g_requested[0] != '\0');
    if (pending) strncpy_s(out, cap, g_requested, _TRUNCATE);
    ReleaseSRWLockShared(&g_preview_lock);
    return pending;
}

void sh_preview_publish(const unsigned char *rgba, unsigned w, unsigned h)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *prefix = "data:image/bmp;base64,";

    if (!rgba || w == 0 || h == 0) return;

    __try {
        /* BMP rows are padded to a 4-byte boundary. 256*3 and 128*3 both happen to be aligned already,
         * but the producer picks the size, so do it properly rather than relying on that. */
        const unsigned rowBytes = ((w * 3u) + 3u) & ~3u;
        const unsigned pixBytes = rowBytes * h;
        const unsigned fileSize = 54u + pixBytes;

        unsigned char *bmp = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, fileSize);
        if (!bmp) { backend_log("B2: preview -- BMP alloc failed"); return; }

        bmp[0] = 'B'; bmp[1] = 'M';
        *(unsigned *)(bmp + 2)  = fileSize;
        *(unsigned *)(bmp + 10) = 54u;              /* pixel data offset */
        *(unsigned *)(bmp + 14) = 40u;              /* BITMAPINFOHEADER  */
        *(int *)     (bmp + 18) = (int)w;
        *(int *)     (bmp + 22) = -(int)h;          /* negative = top-down */
        *(unsigned short *)(bmp + 26) = 1;          /* planes   */
        *(unsigned short *)(bmp + 28) = 24;         /* bpp      */
        *(unsigned *)(bmp + 34) = pixBytes;

        /* Source is RGBA8 (that is what the engine's page decoder emits, and it was also the capture
         * format of the retired route); BMP wants BGR -- hence the swap. Alpha is dropped. */
        for (unsigned y = 0; y < h; ++y) {
            const unsigned char *src = rgba + (size_t)y * w * 4u;
            unsigned char       *dst = bmp + 54u + (size_t)y * rowBytes;
            for (unsigned x = 0; x < w; ++x) {
                dst[x * 3u + 0] = src[x * 4u + 2];   /* B */
                dst[x * 3u + 1] = src[x * 4u + 1];   /* G */
                dst[x * 3u + 2] = src[x * 4u + 0];   /* R */
            }
        }

        size_t prefixLen = strlen(prefix);
        size_t outCap    = prefixLen + ((size_t)fileSize + 2) / 3 * 4 + 1;
        char  *out       = (char *)HeapAlloc(GetProcessHeap(), 0, outCap);
        if (!out) {
            HeapFree(GetProcessHeap(), 0, bmp);
            backend_log("B2: preview -- base64 alloc failed");
            return;
        }
        memcpy(out, prefix, prefixLen);

        size_t o = prefixLen;
        unsigned i = 0;
        while (i + 2 < fileSize) {
            unsigned v = ((unsigned)bmp[i] << 16) | ((unsigned)bmp[i+1] << 8) | bmp[i+2];
            out[o++] = b64[(v >> 18) & 63]; out[o++] = b64[(v >> 12) & 63];
            out[o++] = b64[(v >>  6) & 63]; out[o++] = b64[v & 63];
            i += 3;
        }
        if (i < fileSize) {                          /* 1 or 2 trailing bytes */
            unsigned rem = fileSize - i;
            unsigned v = (unsigned)bmp[i] << 16;
            if (rem == 2) v |= (unsigned)bmp[i+1] << 8;
            out[o++] = b64[(v >> 18) & 63];
            out[o++] = b64[(v >> 12) & 63];
            out[o++] = (rem == 2) ? b64[(v >> 6) & 63] : '=';
            out[o++] = '=';
        }
        out[o] = '\0';

        HeapFree(GetProcessHeap(), 0, bmp);

        AcquireSRWLockExclusive(&g_preview_lock);
        char *old = g_preview_b64;
        g_preview_b64 = out;
        InterlockedExchange(&g_preview_ready, 1);
        ReleaseSRWLockExclusive(&g_preview_lock);
        if (old) HeapFree(GetProcessHeap(), 0, old);   /* safe: no reader can still hold it */

        char line[200];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: preview PUBLISHED -- %ux%u BMP, %zu base64 chars; fetch via iface ext 13", w, h, o);
        backend_log(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("B2: preview -- FAULTED encoding the preview");
    }
}
