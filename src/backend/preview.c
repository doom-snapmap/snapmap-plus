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
        "B2: preview REQUEST #%ld -- '%s' staged", gen, name);
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
    const char *prefix = "data:image/png;base64,";

    if (!rgba || w == 0 || h == 0) return;

    __try {
        /* PNG, not BMP, and the reason is load-bearing: a 24bpp BMP has nowhere to put ALPHA.
         * Many DOOM assets are a flat white RGB plane with the entire image carried in the alpha
         * channel -- GUI icons, decals, POI markers. Encoded as BMP those arrive as a solid white
         * square, which reads as "the decoder failed" when in fact the decode was perfect and the
         * ENCODER threw the answer away. (Observed live 2026-08-03: snap_talk_poi blank while
         * snap_poi_dope_fish, which has real colour, was fine.)
         *
         * No compressor is needed. PNG's IDAT is a zlib stream, and DEFLATE permits STORED
         * (uncompressed) blocks, so this emits a valid zlib stream with zero compression: a 2-byte
         * header, stored blocks of at most 65535 bytes, and an Adler-32. Every chunk gets a CRC-32.
         * Slightly larger than a BMP; correct, which the BMP was not. */
        const unsigned raw_stride = w * 4u + 1u;              /* +1 = per-scanline filter byte */
        const size_t   raw_len    = (size_t)raw_stride * h;

        unsigned char *raw = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, raw_len);
        if (!raw) { backend_log("B2: preview -- PNG raw alloc failed"); return; }
        for (unsigned y = 0; y < h; ++y) {
            unsigned char *dst = raw + (size_t)y * raw_stride;
            dst[0] = 0;                                        /* filter type 0 (None) */
            memcpy(dst + 1, rgba + (size_t)y * w * 4u, w * 4u);
        }

        /* CRC-32 table, built once. */
        static unsigned crcTab[256];
        static LONG crcReady = 0;
        if (InterlockedCompareExchange(&crcReady, 1, 0) == 0) {
            for (unsigned i = 0; i < 256; ++i) {
                unsigned c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                crcTab[i] = c;
            }
            InterlockedExchange(&crcReady, 2);
        }
        while (InterlockedCompareExchange(&crcReady, 2, 2) != 2) Sleep(0);

        const size_t nblocks  = (raw_len + 65534u) / 65535u;
        const size_t idat_len = 2u + nblocks * 5u + raw_len + 4u;   /* hdr + blocks + adler */
        const size_t pngCap = 8u + (12u + 13u) + (12u + idat_len) + 12u;

        unsigned char *png = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, pngCap);
        if (!png) { HeapFree(GetProcessHeap(), 0, raw); backend_log("B2: preview -- PNG alloc failed"); return; }

        size_t o = 0;
        static const unsigned char SIG[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
        memcpy(png + o, SIG, 8); o += 8;

        #define PUT32(p, v) do { unsigned v_ = (unsigned)(v); \
            (p)[0]=(unsigned char)(v_>>24); (p)[1]=(unsigned char)(v_>>16); \
            (p)[2]=(unsigned char)(v_>>8);  (p)[3]=(unsigned char)(v_); } while (0)

        size_t ihdr = o;
        PUT32(png + o, 13); o += 4;
        memcpy(png + o, "IHDR", 4); o += 4;
        PUT32(png + o, w); o += 4;
        PUT32(png + o, h); o += 4;
        png[o++] = 8;    /* bit depth   */
        png[o++] = 6;    /* colour type 6 = RGBA */
        png[o++] = 0; png[o++] = 0; png[o++] = 0;   /* deflate / filter 0 / no interlace */
        {   unsigned c = 0xFFFFFFFFu;
            for (size_t i = ihdr + 4; i < o; ++i) c = crcTab[(c ^ png[i]) & 0xFFu] ^ (c >> 8);
            PUT32(png + o, c ^ 0xFFFFFFFFu); o += 4; }

        size_t idat = o;
        PUT32(png + o, (unsigned)idat_len); o += 4;
        memcpy(png + o, "IDAT", 4); o += 4;
        png[o++] = 0x78; png[o++] = 0x01;                     /* zlib: deflate, 32K, no dict */
        for (size_t b = 0, done = 0; b < nblocks; ++b) {
            size_t n = raw_len - done; if (n > 65535u) n = 65535u;
            png[o++] = (b + 1u == nblocks) ? 1u : 0u;         /* BFINAL on the last, BTYPE = stored */
            png[o++] = (unsigned char)(n & 0xFFu);
            png[o++] = (unsigned char)(n >> 8);
            png[o++] = (unsigned char)(~n & 0xFFu);
            png[o++] = (unsigned char)((~n >> 8) & 0xFFu);
            memcpy(png + o, raw + done, n); o += n; done += n;
        }
        {   unsigned a = 1, b2 = 0;                            /* Adler-32 over the RAW bytes */
            for (size_t i = 0; i < raw_len; ++i) { a = (a + raw[i]) % 65521u; b2 = (b2 + a) % 65521u; }
            PUT32(png + o, (b2 << 16) | a); o += 4; }
        {   unsigned c = 0xFFFFFFFFu;
            for (size_t i = idat + 4; i < o; ++i) c = crcTab[(c ^ png[i]) & 0xFFu] ^ (c >> 8);
            PUT32(png + o, c ^ 0xFFFFFFFFu); o += 4; }

        PUT32(png + o, 0); o += 4;
        memcpy(png + o, "IEND", 4); o += 4;
        {   unsigned c = 0xFFFFFFFFu;
            for (size_t i = o - 4; i < o; ++i) c = crcTab[(c ^ png[i]) & 0xFFu] ^ (c >> 8);
            PUT32(png + o, c ^ 0xFFFFFFFFu); o += 4; }
        #undef PUT32

        HeapFree(GetProcessHeap(), 0, raw);

        const unsigned char *bmp = png;      /* the base64 stage below is format-agnostic */
        const unsigned fileSize = (unsigned)o;
        size_t prefixLen = strlen(prefix);
        size_t outCap    = prefixLen + ((size_t)fileSize + 2) / 3 * 4 + 1;
        char  *out       = (char *)HeapAlloc(GetProcessHeap(), 0, outCap);
        if (!out) {
            HeapFree(GetProcessHeap(), 0, bmp);
            backend_log("B2: preview -- base64 alloc failed");
            return;
        }
        memcpy(out, prefix, prefixLen);

        o = prefixLen;
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
            "B2: preview PUBLISHED -- %ux%u RGBA PNG, %zu base64 chars; fetch via iface ext 13", w, h, o);
        backend_log(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("B2: preview -- FAULTED encoding the preview");
    }
}
