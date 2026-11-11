/* Asset-preview request staging, PNG/base64 encoding and cross-thread
 * delivery. This module uses only CPU work, the CRT and Win32.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "preview.h"
#include "backend_log.h"
#include "snapmap_plus_iface.h"
#include "../common/text.h"

/* One published buffer, consumed through iface ext 13. A shared lock
 * serializes publication, probing, consumption and invalidation; release the
 * buffer after its successful copy.
 */
static char          *g_preview_b64   = NULL;
static volatile LONG  g_preview_ready = 0;
static SRWLOCK        g_preview_lock  = SRWLOCK_INIT;

/* Stage requests under the same lock as published images. */
static char          *g_requested      = NULL;   /* malloc'd, or NULL for none */
static int            g_requested_kind = SH_PREVIEW_KIND_AUTO;
static volatile LONG  g_request_gen    = 0;
/* Why the latest request produced no picture (malloc'd); NULL while it is pending or served. */
static char          *g_failed         = NULL;

/* Drop the request and its reason (lock held); the caller frees what it returns. */
static void preview_forget_locked(char **request, char **failed)
{
    *request = g_requested;
    *failed = g_failed;
    g_requested = NULL;
    g_failed = NULL;
    g_requested_kind = SH_PREVIEW_KIND_AUTO;
}

char *sh_preview_take(void)
{
    char *taken = NULL, *request = NULL, *failed = NULL;
    AcquireSRWLockExclusive(&g_preview_lock);
    if (g_preview_ready && g_preview_b64) {
        taken = g_preview_b64;   /* process heap: the caller releases it as owned text */
        g_preview_b64 = NULL;
        preview_forget_locked(&request, &failed);
        InterlockedExchange(&g_preview_ready, 0);
    }
    ReleaseSRWLockExclusive(&g_preview_lock);
    free(request);
    free(failed);
    return taken;
}

void sh_preview_request(const char *name)
{
    sh_preview_request_kind(name, SH_PREVIEW_KIND_AUTO);
}

void sh_preview_request_kind(const char *name, int kind)
{
    char *copy, *old_request, *old_failed;
    if (!name || !*name || (copy = sh_text(name)) == NULL) return;

    AcquireSRWLockExclusive(&g_preview_lock);
    preview_forget_locked(&old_request, &old_failed);
    g_requested = copy;
    g_requested_kind = kind;
    /* Invalidate the current image BEFORE the generation bump, or a poll can see the new request with
     * the previous request's picture still marked ready and stop polling one image too early. */
    char *superseded = g_preview_b64;
    g_preview_b64 = NULL;
    InterlockedExchange(&g_preview_ready, 0);
    LONG gen = InterlockedIncrement(&g_request_gen);
    ReleaseSRWLockExclusive(&g_preview_lock);
    if (superseded) HeapFree(GetProcessHeap(), 0, superseded);
    free(old_request);
    free(old_failed);

    char *line = sh_textf("preview: request #%ld '%s' staged (%s)", gen, name,
                          kind == SH_PREVIEW_KIND_AUTO ? "automatic route" : "typed route");
    backend_log(line ? line : "preview: request staged");
    free(line);
}

void sh_preview_cancel(void)
{
    char *discarded = NULL, *request, *failed;
    AcquireSRWLockExclusive(&g_preview_lock);
    preview_forget_locked(&request, &failed);
    discarded = g_preview_b64;
    g_preview_b64 = NULL;
    InterlockedExchange(&g_preview_ready, 0);
    InterlockedIncrement(&g_request_gen);   /* any producer already decoding now publishes stale */
    ReleaseSRWLockExclusive(&g_preview_lock);
    if (discarded) HeapFree(GetProcessHeap(), 0, discarded);
    free(request);
    free(failed);
}

char *sh_preview_take_request(unsigned long *generation, int *kind)
{
    char *name = NULL;
    if (!generation || !kind) return NULL;
    *generation = 0;
    *kind = SH_PREVIEW_KIND_AUTO;

    AcquireSRWLockShared(&g_preview_lock);
    if (!g_preview_ready && !g_failed && g_requested && (name = sh_text(g_requested)) != NULL) {
        *generation = (unsigned long)g_request_gen;
        *kind = g_requested_kind;
    }
    ReleaseSRWLockShared(&g_preview_lock);
    return name;
}

int sh_preview_fail(unsigned long generation, const char *reason)
{
    int rc = SH_PREVIEW_STALE;
    char *kept = sh_text(reason && *reason ? reason : "This asset has no preview."), *old = NULL;
    AcquireSRWLockExclusive(&g_preview_lock);
    if (kept && generation && (unsigned long)g_request_gen == generation && g_requested && !g_preview_ready) {
        old = g_failed;
        g_failed = kept;
        kept = NULL;
        rc = SH_PREVIEW_FAILED;
    }
    ReleaseSRWLockExclusive(&g_preview_lock);
    free(kept);
    free(old);
    if (rc == SH_PREVIEW_FAILED) {
        char *line = sh_textf("preview: request #%lu failed: %s", generation,
                              reason && *reason ? reason : "no reason given");
        backend_log(line ? line : "preview: a request failed");
        free(line);
    }
    return rc;
}

char *sh_preview_failure(void)
{
    char *out = NULL;
    AcquireSRWLockShared(&g_preview_lock);
    if (g_failed) out = sh_iface_text(g_failed);
    ReleaseSRWLockShared(&g_preview_lock);
    return out;
}

/* Copy `h` rows of `w` RGBA pixels behind a zero filter byte each. Returns 0 on a fault reading
 * the source. */
static int preview_copy_rows(unsigned char *raw, size_t raw_stride, const unsigned char *rgba,
                             unsigned w, unsigned h)
{
    __try {
        for (unsigned y = 0; y < h; ++y) {
            unsigned char *dst = raw + (size_t)y * raw_stride;
            dst[0] = 0;                                        /* filter type 0 (None) */
            memcpy(dst + 1, rgba + (size_t)y * w * 4u, (size_t)w * 4u);
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sh_preview_publish(unsigned long generation, const unsigned char *rgba, unsigned w, unsigned h)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *prefix = "data:image/png;base64,";

    if (!generation || !rgba || w == 0 || h == 0) return SH_PREVIEW_FAILED;

    {
        /* Use RGBA PNG so icons and decals retain their alpha channel. Encode
         * zlib with stored DEFLATE blocks of at most 65535 bytes and an
         * Adler-32; each PNG chunk also has a CRC-32.
         */
        if ((size_t)w > ((size_t)-1 - 1u) / 4u) return SH_PREVIEW_FAILED;
        const size_t raw_stride = (size_t)w * 4u + 1u;        /* +1 = per-scanline filter byte */
        if ((size_t)h > (size_t)-1 / raw_stride) return SH_PREVIEW_FAILED;
        const size_t raw_len = raw_stride * h;

        unsigned char *raw = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, raw_len);
        if (!raw) { backend_log("preview: out of memory for the PNG rows"); return SH_PREVIEW_FAILED; }
        /* The caller's pixels are the only memory here this module does not own, so the copy is
         * the only guarded step and nothing allocated below can leak on a fault. */
        if (!preview_copy_rows(raw, raw_stride, rgba, w, h)) {
            HeapFree(GetProcessHeap(), 0, raw);
            backend_log("preview: the decoded pixels could not be read");
            return SH_PREVIEW_FAILED;
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

        if (raw_len > (size_t)-1 - 65534u) {
            HeapFree(GetProcessHeap(), 0, raw);
            return SH_PREVIEW_FAILED;
        }
        const size_t nblocks  = (raw_len + 65534u) / 65535u;
        if (nblocks > ((size_t)-1 - raw_len - 6u) / 5u) {
            HeapFree(GetProcessHeap(), 0, raw);
            return SH_PREVIEW_FAILED;
        }
        const size_t idat_len = 2u + nblocks * 5u + raw_len + 4u;   /* hdr + blocks + adler */
        if (idat_len > 0xFFFFFFFFu || idat_len > (size_t)-1 - 57u) {
            HeapFree(GetProcessHeap(), 0, raw);
            return SH_PREVIEW_FAILED;
        }
        const size_t pngCap = 8u + (12u + 13u) + (12u + idat_len) + 12u;

        unsigned char *png = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, pngCap);
        if (!png) { HeapFree(GetProcessHeap(), 0, raw); backend_log("preview: out of memory for the PNG"); return SH_PREVIEW_FAILED; }

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

        unsigned char *png_bytes = png;
        const size_t fileSize = o;
        size_t prefixLen = strlen(prefix);
        if (fileSize > ((size_t)-1 - prefixLen - 1u) / 4u * 3u) {
            HeapFree(GetProcessHeap(), 0, png_bytes);
            return SH_PREVIEW_FAILED;
        }
        size_t outCap    = prefixLen + ((size_t)fileSize + 2) / 3 * 4 + 1;
        char  *out       = (char *)HeapAlloc(GetProcessHeap(), 0, outCap);
        if (!out) {
            HeapFree(GetProcessHeap(), 0, png_bytes);
            backend_log("preview: out of memory for the data URI");
            return SH_PREVIEW_FAILED;
        }
        memcpy(out, prefix, prefixLen);

        o = prefixLen;
        size_t i = 0;
        while (i + 2 < fileSize) {
            unsigned v = ((unsigned)png_bytes[i] << 16) | ((unsigned)png_bytes[i+1] << 8) | png_bytes[i+2];
            out[o++] = b64[(v >> 18) & 63]; out[o++] = b64[(v >> 12) & 63];
            out[o++] = b64[(v >>  6) & 63]; out[o++] = b64[v & 63];
            i += 3;
        }
        if (i < fileSize) {                          /* 1 or 2 trailing bytes */
            size_t rem = fileSize - i;
            unsigned v = (unsigned)png_bytes[i] << 16;
            if (rem == 2) v |= (unsigned)png_bytes[i+1] << 8;
            out[o++] = b64[(v >> 18) & 63];
            out[o++] = b64[(v >> 12) & 63];
            out[o++] = (rem == 2) ? b64[(v >> 6) & 63] : '=';
            out[o++] = '=';
        }
        out[o] = '\0';

        HeapFree(GetProcessHeap(), 0, png_bytes);

        AcquireSRWLockExclusive(&g_preview_lock);
        if ((unsigned long)g_request_gen != generation) {
            ReleaseSRWLockExclusive(&g_preview_lock);
            HeapFree(GetProcessHeap(), 0, out);
            return SH_PREVIEW_STALE;
        }
        char *old = g_preview_b64, *old_failed = g_failed;
        g_preview_b64 = out;
        InterlockedExchange(&g_preview_ready, 1);
        g_failed = NULL;
        ReleaseSRWLockExclusive(&g_preview_lock);
        if (old) HeapFree(GetProcessHeap(), 0, old);   /* safe: no reader can still hold it */
        free(old_failed);

        char line[200];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "preview: request #%lu published, %ux%u RGBA PNG, %zu base64 chars",
            generation, w, h, o);
        backend_log(line);
        return SH_PREVIEW_PUBLISHED;
    }
}
