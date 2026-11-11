/* store_text.h -- the store tool's report fields, filled from whole sentences.
 *
 * The store tool reports into fixed fields of its records and verdicts (the
 * installer prints them). A sentence the product composes is any length: it
 * goes into a field whole when it fits, else the field says that the
 * sentence is in the log and the log carries it whole. Nothing is cut. */
#ifndef SNAPMAP_PLUS_STORE_TEXT_H
#define SNAPMAP_PLUS_STORE_TEXT_H

#include <stdlib.h>
#include <string.h>
#include "../backend/backend_log.h"
#include "../common/text.h"
#include "../common/wide_path.h"

/* A path buffer of the store tool: any path Windows accepts, in UTF-8
 * (SH_U8PATH_MAX) or UTF-16 (SH_WPATH_MAX). The tool links a stack that
 * holds them (build.ps1); a folder walk keeps its paths on the heap. */
#define ST_PATH SH_U8PATH_MAX

#define ST_TEXT_OVER "the sentence is longer than this record holds; it is in the log whole"

/* `text` into out[cap] whole, else ST_TEXT_OVER there and `text` in the log. */
static __inline void st_put(char *out, size_t cap, const char *text)
{
    if (!out || !cap) return;
    if (!text) text = "";
    if (strlen(text) < cap) { memcpy(out, text, strlen(text) + 1); return; }
    {
        char *line = sh_textf("STORE: %s", text);
        backend_log(line ? line : "STORE: (a sentence longer than its record, and out of memory)");
        free(line);
    }
    if (strlen(ST_TEXT_OVER) < cap) memcpy(out, ST_TEXT_OVER, sizeof ST_TEXT_OVER);
    else out[0] = 0;
}

/* st_put of a formatted sentence. */
static __inline void st_putf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    char *text;
    va_start(ap, fmt);
    text = sh_vtextf(fmt, ap);
    va_end(ap);
    st_put(out, cap, text ? text : "out of memory");
    free(text);
}

/* A path or name `text` into out[cap] whole: 1; else 0 with out "" and the
 * whole text in the log, so a file operation on it fails instead of acting
 * on a cut path. */
static __inline int st_cpy(char *out, size_t cap, const char *text)
{
    if (!out || !cap) return 0;
    if (!text) text = "";
    if (strlen(text) < cap) { memcpy(out, text, strlen(text) + 1); return 1; }
    {
        char *line = sh_textf("STORE: a name longer than this tool's %u-byte field is not used: %s",
                              (unsigned)cap, text);
        backend_log(line ? line : "STORE: a path longer than its field is not used (out of memory)");
        free(line);
    }
    out[0] = 0;
    return 0;
}

/* st_cpy of a formatted path or name. */
static __inline int st_fmt(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    char *text;
    int ok;
    va_start(ap, fmt);
    text = sh_vtextf(fmt, ap);
    va_end(ap);
    ok = st_cpy(out, cap, text ? text : "");
    free(text);
    return ok;
}

/* A path `src` into out[cap] whole: 1, or 0 when it is longer than the
 * buffer holds (the caller refuses with its own sentence). */
static __inline int st_path(char *out, size_t cap, const char *src)
{
    return src && sh_text_fits(out, cap, src);
}

#endif /* SNAPMAP_PLUS_STORE_TEXT_H */
