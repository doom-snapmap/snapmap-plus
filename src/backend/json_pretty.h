/* Reformat whitespace between JSON tokens for sh_pretty_on. Preserve token
 * order, number spelling and all string bytes. Format a copy of the
 * serialized rawmap so the native save buffer stays unchanged.
 *
 * Output uses two-space indentation, a space after each colon, LF line
 * endings and one trailing newline. No filesystem or engine calls.
 */
#ifndef SNAPMAP_PLUS_JSON_PRETTY_H
#define SNAPMAP_PLUS_JSON_PRETTY_H

#include <stddef.h>

#define JSON_PRETTY_INDENT     2    /* spaces per nesting level (config_json.c's writer emits the same) */
#define JSON_PRETTY_MAX_DEPTH  64   /* deeper than any real map nests; refuse rather than emit a wall of indent */

/* Emit one character: counted always, stored only while it fits. `out == NULL` therefore turns the whole
 * pass into a pure size measurement, which is how the caller learns how much to allocate. */
static void jp_put(char *out, size_t outcap, size_t *pos, char ch)
{
    if (out != NULL && *pos < outcap) out[*pos] = ch;
    (*pos)++;
}

static void jp_indent(char *out, size_t outcap, size_t *pos, int depth)
{
    int n = depth * JSON_PRETTY_INDENT;
    for (int i = 0; i < n; i++) jp_put(out, outcap, pos, ' ');
}

static int jp_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Format len bytes and return the required output size, even when outcap is
 * too small. Measure with (NULL, 0), then allocate and write. Returns 0 for
 * an unterminated string, unbalanced/deep containers or whitespace-only
 * input; callers then retain the original bytes.
 */
static size_t json_pretty(const char *src, size_t len, char *out, size_t outcap)
{
    size_t pos = 0, i = 0;
    int    depth = 0;

    if (src == NULL || len == 0) return 0;

    while (i < len) {
        char c = src[i];

        /* Replace only inter-token whitespace. */
        if (jp_is_space(c)) { i++; continue; }

        if (c == '"') {
            /* Copy strings and escapes verbatim; delimiters inside them are data. */
            jp_put(out, outcap, &pos, c);
            i++;
            for (;;) {
                char s;
                if (i >= len) return 0;                 /* unterminated string -> refuse */
                s = src[i];
                jp_put(out, outcap, &pos, s);
                i++;
                if (s == '\\') {                        /* \" and \\ -- the escaped byte is never a delimiter */
                    if (i >= len) return 0;
                    jp_put(out, outcap, &pos, src[i]);
                    i++;
                    continue;
                }
                if (s == '"') break;
            }
            continue;
        }

        if (c == '{' || c == '[') {
            size_t j;
            if (depth >= JSON_PRETTY_MAX_DEPTH) return 0;
            jp_put(out, outcap, &pos, c);
            depth++;
            /* Keep empty containers together as {} or []. */
            j = i + 1;
            while (j < len && jp_is_space(src[j])) j++;
            if (j < len && ((c == '{' && src[j] == '}') || (c == '[' && src[j] == ']'))) {
                jp_put(out, outcap, &pos, src[j]);
                depth--;
                i = j + 1;
                continue;
            }
            jp_put(out, outcap, &pos, '\n');
            jp_indent(out, outcap, &pos, depth);
            i++;
            continue;
        }

        if (c == '}' || c == ']') {
            if (depth <= 0) return 0;                   /* a close with nothing open -> refuse */
            depth--;
            jp_put(out, outcap, &pos, '\n');
            jp_indent(out, outcap, &pos, depth);
            jp_put(out, outcap, &pos, c);
            i++;
            continue;
        }

        if (c == ',') {                                 /* separator hugs the value it follows */
            jp_put(out, outcap, &pos, c);
            jp_put(out, outcap, &pos, '\n');
            jp_indent(out, outcap, &pos, depth);
            i++;
            continue;
        }

        if (c == ':') {
            jp_put(out, outcap, &pos, c);
            jp_put(out, outcap, &pos, ' ');
            i++;
            continue;
        }

        jp_put(out, outcap, &pos, c);                   /* number / true / false / null, verbatim */
        i++;
    }

    if (depth != 0) return 0;                           /* truncated document -> refuse */
    if (pos == 0)   return 0;                           /* nothing but whitespace -> refuse */
    jp_put(out, outcap, &pos, '\n');                    /* one trailing newline, as config_json.c writes */
    return pos;
}

#endif /* SNAPMAP_PLUS_JSON_PRETTY_H */
