/* Helpers for report attachments: replace the account, profile-folder and
 * machine names in log text, select bounded log tails, and fit the logs into
 * the relay's payload cap. These helpers do not remove arbitrary personal data.
 *
 * A name can reach a log in several encodings: the backend and window logs are
 * UTF-8, an engine or Windows message may carry the ANSI code page, a wide
 * string may be dumped as UTF-16LE, and paths carry the profile folder's name
 * (which can differ from the account name, and has an 8.3 short form). Each
 * form is kept as its own byte needle, so the scrub works on raw bytes. */
#ifndef SNAPMAP_PLUS_REPORT_SCRUB_H
#define SNAPMAP_PLUS_REPORT_SCRUB_H

#include <stddef.h>
#include <string.h>
#include <windows.h>

#define RS_FORMS_MAX 32
#define RS_FORM_BYTES 256

typedef struct rs_form {
    char   bytes[RS_FORM_BYTES];
    size_t len;
    int    utf16;               /* 1: UTF-16LE bytes; the replacement is written as UTF-16LE too */
    const char *repl;           /* ASCII replacement, e.g. "<user>" */
} rs_form;

typedef struct rs_forms {
    rs_form form[RS_FORMS_MAX];
    int     n;
} rs_forms;

static char rs_fold(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Add one byte needle unless it is shorter than two characters or already present. */
static void rs_add_bytes(rs_forms *f, const char *bytes, size_t len, int utf16, const char *repl)
{
    int i;
    size_t k;
    if (!f || !bytes || f->n >= RS_FORMS_MAX || len > RS_FORM_BYTES) return;
    if (len < (utf16 ? 4u : 2u)) return;   /* a one-character name would mangle the whole text */
    for (i = 0; i < f->n; i++) {
        if (f->form[i].len != len || f->form[i].utf16 != utf16) continue;
        for (k = 0; k < len && rs_fold(f->form[i].bytes[k]) == rs_fold(bytes[k]); k++) {}
        if (k == len) return;
    }
    memcpy(f->form[f->n].bytes, bytes, len);
    f->form[f->n].len = len;
    f->form[f->n].utf16 = utf16;
    f->form[f->n].repl = repl;
    f->n++;
}

/* Add a name in UTF-8, the ANSI code page (when it is exactly representable) and
 * UTF-16LE, each as written, lower-cased and upper-cased (ASCII letters also
 * match in any case). */
static void rs_add_name(rs_forms *f, const wchar_t *name, const char *repl)
{
    wchar_t cased[3][RS_FORM_BYTES / 2];
    int c;
    size_t wl;
    if (!f || !name || !name[0]) return;
    wl = wcslen(name);
    if (wl >= RS_FORM_BYTES / 2) return;
    for (c = 0; c < 3; c++) {
        memcpy(cased[c], name, (wl + 1) * sizeof(wchar_t));
        if (c > 0)
            LCMapStringEx(LOCALE_NAME_INVARIANT, c == 1 ? LCMAP_LOWERCASE : LCMAP_UPPERCASE,
                          name, (int)wl, cased[c], (int)wl, NULL, NULL, 0);
    }
    for (c = 0; c < 3; c++) {
        char buf[RS_FORM_BYTES];
        BOOL lossy = FALSE;
        int n = WideCharToMultiByte(CP_UTF8, 0, cased[c], (int)wl, buf, (int)sizeof buf, NULL, NULL);
        if (n > 0) rs_add_bytes(f, buf, (size_t)n, 0, repl);
        n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, cased[c], (int)wl, buf, (int)sizeof buf, NULL, &lossy);
        if (n > 0 && !lossy) rs_add_bytes(f, buf, (size_t)n, 0, repl);
        rs_add_bytes(f, (const char *)cased[c], wl * sizeof(wchar_t), 1, repl);
    }
}

/* Add the last component of a path (the profile folder's name). */
static void rs_add_path_leaf(rs_forms *f, const wchar_t *path, const char *repl)
{
    const wchar_t *leaf;
    if (!path || !path[0]) return;
    leaf = wcsrchr(path, L'\\');
    rs_add_name(f, leaf ? leaf + 1 : path, repl);
}

/* Scrub src[0..len) into dst (capacity cap). Returns the bytes written; output
 * past cap is cut. Longer needles are tried first so a short form cannot split
 * a longer one. */
static size_t rs_scrub_forms(char *dst, size_t cap, const char *src, size_t len, const rs_forms *f)
{
    size_t o = 0, i = 0;
    int k;
    if (!dst || !cap) return 0;
    while (i < len && o < cap) {
        const rs_form *hit = NULL;
        for (k = 0; f && k < f->n; k++) {
            const rs_form *x = &f->form[k];
            size_t j;
            if (x->len > len - i || (hit && x->len <= hit->len)) continue;
            for (j = 0; j < x->len && rs_fold(src[i + j]) == rs_fold(x->bytes[j]); j++) {}
            if (j == x->len) hit = x;
        }
        if (hit) {
            const char *r = hit->repl ? hit->repl : "";
            size_t rl = strlen(r), j;
            for (j = 0; j < rl && o < cap; j++) {
                dst[o++] = r[j];
                if (hit->utf16 && o < cap) dst[o++] = '\0';
            }
            i += hit->len;
        } else {
            dst[o++] = src[i++];
        }
    }
    return o;
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

/* The length one byte takes once JSON-escaped (sh_webview_json::escape_utf8). */
static size_t rs_json_escaped_len(unsigned char c)
{
    if (c == '\\' || c == '"' || c == '\n' || c == '\r' || c == '\t') return 2;
    return c < 0x20 ? 6 : 1;
}

/* The offset from which buf[off..len), once JSON-escaped, fits in budget bytes:
 * the newest text is kept, the cut moves forward to the next line start when
 * there is one, and never splits a UTF-8 sequence. len when nothing fits. */
static size_t rs_fit_escaped_tail(const char *buf, size_t len, size_t budget)
{
    size_t used = 0, off = len, line;
    if (!buf) return 0;
    while (off > 0) {
        size_t c = rs_json_escaped_len((unsigned char)buf[off - 1]);
        if (used + c > budget) break;
        used += c;
        off--;
    }
    if (off == 0) return 0;
    for (line = off; line < len && buf[line - 1] != '\n'; line++) {}
    if (line < len) return line;
    while (off < len && ((unsigned char)buf[off] & 0xC0) == 0x80) off++;
    return off;
}

#endif /* SNAPMAP_PLUS_REPORT_SCRUB_H */
