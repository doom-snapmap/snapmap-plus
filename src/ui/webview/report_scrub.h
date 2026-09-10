/* Pure helpers for report attachments: replace account/machine identifiers and
 * select bounded log tails. These helpers do not remove arbitrary personal data. */
#ifndef SNAPMAP_PLUS_REPORT_SCRUB_H
#define SNAPMAP_PLUS_REPORT_SCRUB_H

#include <stddef.h>
#include <string.h>

/* Copy with case-insensitive name replacement. Names shorter than two bytes are
 * skipped to avoid pervasive single-character replacements. Return bytes written;
 * a nonzero destination capacity is NUL-terminated, with truncation at cap. */
static int rs_scrub(char *dst, size_t cap, const char *src, const char *name, const char *repl)
{
    size_t o = 0, nlen, rlen, i;
    if (!dst || cap == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }
    nlen = name ? strlen(name) : 0;
    rlen = repl ? strlen(repl) : 0;
    while (*src && o + 1 < cap) {
        int hit = 0;
        if (nlen >= 2) {
            hit = 1;
            for (i = 0; i < nlen; i++) {
                char a = src[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { hit = 0; break; }
                if (src[i] == '\0') { hit = 0; break; }
            }
        }
        if (hit) {
            for (i = 0; i < rlen && o + 1 < cap; i++) dst[o++] = repl[i];
            src += nlen;
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = '\0';
    return (int)o;
}

/* Return a tail offset retaining at most keep bytes. Prefer a whole line; if
 * no newline remains, use a byte cut. Short buffers start at zero. */
static size_t rs_tail_offset(const char *buf, size_t len, size_t keep)
{
    size_t off;
    if (!buf || len <= keep) return 0;
    off = len - keep;
    while (off < len && buf[off - 1] != '\n') off++;   /* snap to a line boundary */
    if (off >= len) off = len - keep;                  /* one giant line -> plain byte cut */
    return off;
}

#endif /* SNAPMAP_PLUS_REPORT_SCRUB_H */
