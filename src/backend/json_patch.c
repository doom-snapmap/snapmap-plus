/* Locate JSON value spans and splice compact engine entity JSON in place.
 * Property-path descent is limited by JSON_MAX_SEGS; nested-value scanning has
 * no separate depth limit. This scanner is not a general JSON validator. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_patch.h"

#define JSON_MAX_SEGS   16
#define JSON_CHAIN_CAP  (16 * 1024)

/* Token scanners return the end of a span or NULL, without allocating a tree. */

static const char *json_skip_ws(const char *p)
{
    if (!p) return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Skip a quoted string, honoring escaped bytes; return NULL if unterminated. */
static const char *json_skip_string(const char *p)
{
    if (!p || *p != '"') return NULL;
    p++;
    while (*p != '\0') {
        if (*p == '\\') { p++; if (*p == '\0') return NULL; p++; continue; }
        if (*p == '"') return p + 1;
        p++;
    }
    return NULL;
}

/* Skip a value, recursively scanning objects and arrays. No depth cap is imposed. */
static const char *json_skip_value(const char *p)
{
    p = json_skip_ws(p);
    if (!p || *p == '\0') return NULL;

    if (*p == '"') return json_skip_string(p);

    if (*p == '{') {
        const char *q = json_skip_ws(p + 1);
        if (!q) return NULL;
        if (*q == '}') return q + 1;
        for (;;) {
            if (*q != '"') return NULL;
            q = json_skip_string(q);
            if (!q) return NULL;
            q = json_skip_ws(q);
            if (!q || *q != ':') return NULL;
            q = json_skip_ws(q + 1);
            q = json_skip_value(q);
            if (!q) return NULL;
            q = json_skip_ws(q);
            if (!q) return NULL;
            if (*q == ',') { q = json_skip_ws(q + 1); continue; }
            if (*q == '}') return q + 1;
            return NULL;
        }
    }

    if (*p == '[') {
        const char *q = json_skip_ws(p + 1);
        if (!q) return NULL;
        if (*q == ']') return q + 1;
        for (;;) {
            q = json_skip_value(q);
            if (!q) return NULL;
            q = json_skip_ws(q);
            if (!q) return NULL;
            if (*q == ',') { q = json_skip_ws(q + 1); continue; }
            if (*q == ']') return q + 1;
            return NULL;
        }
    }

    if (*p == 't') return (strncmp(p, "true", 4) == 0) ? p + 4 : NULL;
    if (*p == 'f') return (strncmp(p, "false", 5) == 0) ? p + 5 : NULL;
    if (*p == 'n') return (strncmp(p, "null", 4) == 0) ? p + 4 : NULL;

    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        const char *q = p;
        if (*q == '-') q++;
        while (*q >= '0' && *q <= '9') q++;
        if (*q == '.') { q++; while (*q >= '0' && *q <= '9') q++; }
        if (*q == 'e' || *q == 'E') {
            q++;
            if (*q == '+' || *q == '-') q++;
            while (*q >= '0' && *q <= '9') q++;
        }
        return (q > p) ? q : NULL;
    }
    return NULL;
}

/* Find a direct member of [obj_open,obj_close), where obj_close follows the }.
 * Return its value span and, optionally, the key opening quote. Outputs remain
 * untouched on a miss or malformed object. */
static int json_find_top_level_key(const char *obj_open, const char *obj_close, const char *key,
                                    const char **key_start, const char **val_start, const char **val_end)
{
    if (!obj_open || !obj_close || *obj_open != '{' || !key) return 0;
    size_t keylen = strlen(key);
    const char *p = json_skip_ws(obj_open + 1);
    if (!p) return 0;
    while (p < obj_close && *p != '}') {
        if (*p != '"') return 0;
        const char *memberStart = p;
        const char *kstart = p + 1;
        const char *kend_q = json_skip_string(p);          /* just past the closing quote */
        if (!kend_q) return 0;
        int match = ((size_t)(kend_q - 1 - kstart) == keylen) && (memcmp(kstart, key, keylen) == 0);
        p = json_skip_ws(kend_q);
        if (!p || p >= obj_close || *p != ':') return 0;
        p = json_skip_ws(p + 1);
        if (!p) return 0;
        const char *vstart = p;
        const char *vend = json_skip_value(p);
        if (!vend) return 0;
        if (match) {
            if (key_start) *key_start = memberStart;
            *val_start = vstart; *val_end = vend;
            return 1;
        }
        p = json_skip_ws(vend);
        if (!p) return 0;
        if (p < obj_close && *p == ',') { p = json_skip_ws(p + 1); continue; }
        break;
    }
    return 0;
}

/* Text construction. */

/* Replace [cut_start,cut_end) in doc. Return 0 on overflow; discard out on failure. */
static int json_splice(const char *doc, size_t doclen, const char *cut_start, const char *cut_end,
                        const char *repl, char *out, int outcap)
{
    if (!doc || !cut_start || !cut_end || !repl || !out || outcap <= 0) return 0;
    size_t pre = (size_t)(cut_start - doc);
    size_t post_off = (size_t)(cut_end - doc);
    if (pre > doclen || post_off > doclen || post_off < pre) return 0;
    size_t repl_len = strlen(repl);
    size_t post_len = doclen - post_off;
    size_t total = pre + repl_len + post_len;
    if (total + 1 > (size_t)outcap) return 0;
    memcpy(out, doc, pre);
    memcpy(out + pre, repl, repl_len);
    memcpy(out + pre + repl_len, doc + post_off, post_len);
    out[total] = '\0';
    return 1;
}

/* Insert a member after {, adding a comma only when the object is nonempty. */
static int json_insert_member(const char *doc, size_t doclen, const char *obj_open, const char *obj_close,
                               const char *key, const char *value_token, char *out, int outcap)
{
    const char *p = json_skip_ws(obj_open + 1);
    if (!p) return 0;
    /* obj_close follows }; test the first non-whitespace byte after { instead. */
    (void)obj_close;
    int empty = (*p == '}');
    char member[JSON_CHAIN_CAP + 128];
    int n = empty
        ? _snprintf_s(member, sizeof member, _TRUNCATE, "\"%s\":%s", key, value_token)
        : _snprintf_s(member, sizeof member, _TRUNCATE, "\"%s\":%s,", key, value_token);
    if (n <= 0) return 0;
    return json_splice(doc, doclen, obj_open + 1, obj_open + 1, member, out, outcap);
}

/* Build `"segs[from_idx]":{"segs[from_idx+1]":{...:leaf_token}}` (leaf_token used VERBATIM as the
 * innermost value -- a scalar token or an already-built object literal both work). */
static int json_build_scalar_chain(const char * const *segs, int nseg, int from_idx,
                                    const char *leaf_token, char *out, int outcap)
{
    if (from_idx < 0 || from_idx >= nseg) return 0;
    char buf[JSON_CHAIN_CAP];
    size_t len = 0;
    for (int i = from_idx; i < nseg; i++) {
        int n = _snprintf_s(buf + len, sizeof(buf) - len, _TRUNCATE, "\"%s\":%s",
                             segs[i], (i == nseg - 1) ? leaf_token : "{");
        if (n <= 0) return 0;
        len += (size_t)n;
    }
    for (int i = from_idx; i < nseg - 1; i++) {
        if (len + 1 >= sizeof buf) return 0;
        buf[len++] = '}';
    }
    if (len + 1 > (size_t)outcap) return 0;
    memcpy(out, buf, len);
    out[len] = '\0';
    return 1;
}

/* JSON-escape `raw` into a quoted string literal token (surrounding quotes included). */
int sh_json_quote_string(const char *raw, char *out, int cap)
{
    if (cap < 3 || !raw) return 0;
    int o = 0;
    out[o++] = '"';
    for (const unsigned char *p = (const unsigned char *)raw; *p; p++) {
        const char *esc = NULL;
        char u[8];
        switch (*p) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:
                if (*p < 0x20) { _snprintf_s(u, sizeof u, _TRUNCATE, "\\u%04x", *p); esc = u; }
                break;
        }
        if (esc) {
            int elen = (int)strlen(esc);
            if (o + elen >= cap - 1) return 0;
            memcpy(out + o, esc, (size_t)elen);
            o += elen;
        } else {
            if (o >= cap - 2) return 0;
            out[o++] = (char)*p;
        }
    }
    if (o >= cap - 1) return 0;
    out[o++] = '"';
    out[o] = '\0';
    return 1;
}

/* Compare an escaped JSON string with raw text. Unsupported escapes, including
 * Unicode escapes, compare unequal and may allow a duplicate append. */
static int json_string_span_equals(const char *span_start, const char *span_end, const char *target)
{
    if (!span_start || !span_end || span_end <= span_start || *span_start != '"') return 0;
    const char *p = span_start + 1;
    const char *end = span_end - 1;
    const char *t = target;
    while (p < end) {
        char c;
        if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case '"': c = '"';  break;
                case '\\': c = '\\'; break;
                case '/': c = '/';  break;
                default: return 0;   /* \uXXXX or unknown -- fail-closed to "not equal" */
            }
            p++;
        } else {
            c = *p;
            p++;
        }
        if (*t == '\0' || *t != c) return 0;
        t++;
    }
    return *t == '\0';
}

static int json_atoi_span(const char *start, const char *end)
{
    int v = 0, neg = 0;
    const char *p = start;
    if (p < end && *p == '-') { neg = 1; p++; }
    for (; p < end && *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    return neg ? -v : v;
}

/* Build `"item[N]":"<escaped ids[i]>",` for one entry. */
static int json_format_item_member(char *out, int cap, int index, const char *raw_idstr)
{
    char q[300];
    if (!sh_json_quote_string(raw_idstr, q, (int)sizeof q)) return 0;
    int n = _snprintf_s(out, (size_t)cap, _TRUNCATE, "\"item[%d]\":%s,", index, q);
    return n > 0;
}

/* Build a fresh `{"item[0]":"..",...,"item[n_ids-1]":"..","num":n_ids}` list object (no dedup needed --
 * there is no prior list to dedup against). */
static int json_build_fresh_list(const char * const *ids, int n_ids, char *out, int outcap)
{
    char buf[JSON_CHAIN_CAP];
    size_t len = 0;
    buf[len++] = '{';
    for (int i = 0; i < n_ids; i++) {
        char one[340];
        if (!json_format_item_member(one, sizeof one, i, ids[i])) return 0;
        size_t olen = strlen(one);
        if (len + olen >= sizeof buf) return 0;
        memcpy(buf + len, one, olen);
        len += olen;
    }
    int n = _snprintf_s(buf + len, sizeof(buf) - len, _TRUNCATE, "\"num\":%d}", n_ids);
    if (n <= 0) return 0;
    len += (size_t)n;
    if (len + 1 > (size_t)outcap) return 0;
    memcpy(out, buf, len);
    out[len] = '\0';
    return 1;
}

/* Same as json_build_scalar_chain, but the innermost leaf is a FRESH list object built from ids[]. */
static int json_build_list_chain(const char * const *segs, int nseg, int from_idx,
                                  const char * const *ids, int n_ids, char *out, int outcap)
{
    char freshlist[JSON_CHAIN_CAP];
    if (!json_build_fresh_list(ids, n_ids, freshlist, (int)sizeof freshlist)) return 0;
    return json_build_scalar_chain(segs, nseg, from_idx, freshlist, out, outcap);
}

/* Property-path traversal. */

/* Descend existing objects, then create any missing path in one splice. */
static int json_walk_set(const char *doc, size_t doclen, const char *obj_open,
                          const char * const *segs, int nseg, int idx,
                          const char *leaf_token, char *out, int outcap)
{
    const char *obj_close = json_skip_value(obj_open);
    if (!obj_close || *obj_open != '{') return 0;
    const char *vs = NULL, *ve = NULL;
    int found = json_find_top_level_key(obj_open, obj_close, segs[idx], NULL, &vs, &ve);

    if (idx == nseg - 1) {
        if (found) return json_splice(doc, doclen, vs, ve, leaf_token, out, outcap);
        return json_insert_member(doc, doclen, obj_open, obj_close, segs[idx], leaf_token, out, outcap);
    }
    if (found && ve > vs && *vs == '{')
        return json_walk_set(doc, doclen, vs, segs, nseg, idx + 1, leaf_token, out, outcap);

    {
        /* A missing or non-object intermediate value needs a braced object, not
         * a bare key:value member. */
        char inner[JSON_CHAIN_CAP];
        char chain[JSON_CHAIN_CAP];
        if (!json_build_scalar_chain(segs, nseg, idx + 1, leaf_token, inner, (int)sizeof inner)) return 0;
        if (_snprintf_s(chain, sizeof chain, _TRUNCATE, "{%s}", inner) <= 0) return 0;
        if (found) return json_splice(doc, doclen, vs, ve, chain, out, outcap);
        return json_insert_member(doc, doclen, obj_open, obj_close, segs[idx], chain, out, outcap);
    }
}

/* upsert_reflist's walker: identical descent; only the leaf action differs (list dedup-merge/create
 * instead of a scalar replace). */
static int json_walk_upsert_reflist(const char *doc, size_t doclen, const char *obj_open,
                                     const char * const *segs, int nseg, int idx,
                                     const char * const *ids, int n_ids,
                                     char *out, int outcap)
{
    const char *obj_close = json_skip_value(obj_open);
    if (!obj_close || *obj_open != '{') return 0;
    const char *vs = NULL, *ve = NULL;
    int found = json_find_top_level_key(obj_open, obj_close, segs[idx], NULL, &vs, &ve);

    if (idx < nseg - 1) {
        if (found && ve > vs && *vs == '{')
            return json_walk_upsert_reflist(doc, doclen, vs, segs, nseg, idx + 1, ids, n_ids, out, outcap);
        /* Wrap the missing path in an object, as in json_walk_set. */
        char inner[JSON_CHAIN_CAP];
        char chain[JSON_CHAIN_CAP];
        if (!json_build_list_chain(segs, nseg, idx + 1, ids, n_ids, inner, (int)sizeof inner)) return 0;
        if (_snprintf_s(chain, sizeof chain, _TRUNCATE, "{%s}", inner) <= 0) return 0;
        if (found) return json_splice(doc, doclen, vs, ve, chain, out, outcap);
        return json_insert_member(doc, doclen, obj_open, obj_close, segs[idx], chain, out, outcap);
    }

    /* Merge an existing num/item[] list by inserting new items before num. */
    if (found && ve > vs && *vs == '{') {
        const char *num_key = NULL, *num_vs = NULL, *num_ve = NULL;
        if (json_find_top_level_key(vs, ve, "num", &num_key, &num_vs, &num_ve)) {
            int base = json_atoi_span(num_vs, num_ve);
            if (base < 0) base = 0;
            char newitems[JSON_CHAIN_CAP];
            size_t nlen = 0;
            int next = base;
            for (int i = 0; i < n_ids; i++) {
                int dup = 0;
                for (int k = 0; k < base && !dup; k++) {
                    char keybuf[24];
                    _snprintf_s(keybuf, sizeof keybuf, _TRUNCATE, "item[%d]", k);
                    const char *ivs = NULL, *ive = NULL;
                    if (json_find_top_level_key(vs, ve, keybuf, NULL, &ivs, &ive) &&
                        json_string_span_equals(ivs, ive, ids[i]))
                        dup = 1;
                }
                if (dup) continue;
                char one[340];
                if (!json_format_item_member(one, sizeof one, next, ids[i])) return 0;
                size_t olen = strlen(one);
                if (nlen + olen >= sizeof newitems) return 0;
                memcpy(newitems + nlen, one, olen);
                nlen += olen;
                next++;
            }
            if (next == base) {
                /* All IDs are already present; preserve the original document. */
                if (doclen + 1 > (size_t)outcap) return 0;
                memcpy(out, doc, doclen);
                out[doclen] = '\0';
                return 1;
            }
            newitems[nlen] = '\0';
            char numrepl[32];
            int nn = _snprintf_s(numrepl, sizeof numrepl, _TRUNCATE, "%d", next);
            if (nn <= 0) return 0;

            char full_leaf[JSON_CHAIN_CAP * 2 + 128];
            size_t off = 0;
            /* Insert before the num key, not its value, to avoid duplicating the key
             * or placing new members after its colon. */
            size_t seg1 = (size_t)(num_key - vs);     /* [vs .. "num" key start) -- kept verbatim */
            size_t tail = (size_t)(ve - num_ve);      /* [num's value end .. ve) -- kept verbatim */
            if (seg1 + nlen + 6 + strlen(numrepl) + tail + 1 > sizeof full_leaf) return 0;
            memcpy(full_leaf + off, vs, seg1); off += seg1;
            memcpy(full_leaf + off, newitems, nlen); off += nlen;
            memcpy(full_leaf + off, "\"num\":", 6); off += 6;
            memcpy(full_leaf + off, numrepl, strlen(numrepl)); off += strlen(numrepl);
            memcpy(full_leaf + off, num_ve, tail); off += tail;
            full_leaf[off] = '\0';
            return json_splice(doc, doclen, vs, ve, full_leaf, out, outcap);
        }
        /* Replace objects that do not have a list count. */
    }
    {
        char freshlist[JSON_CHAIN_CAP];
        if (!json_build_fresh_list(ids, n_ids, freshlist, (int)sizeof freshlist)) return 0;
        if (found) return json_splice(doc, doclen, vs, ve, freshlist, out, outcap);
        return json_insert_member(doc, doclen, obj_open, obj_close, segs[idx], freshlist, out, outcap);
    }
}

/* Public operations. */

/* Prefix the dotted property path with entityDef.state.edit. Return 0 if invalid
 * or too long. segs points into pathbuf, which must outlive traversal. */
static int build_segs(char *pathbuf, size_t pathbuf_cap, const char *prop_path,
                       const char *segs[JSON_MAX_SEGS])
{
    int nseg = 0;
    segs[nseg++] = "entityDef";
    segs[nseg++] = "state";
    segs[nseg++] = "edit";
    if (_snprintf_s(pathbuf, pathbuf_cap, _TRUNCATE, "%s", prop_path) <= 0) return 0;
    char *save = NULL;
    for (char *tok = strtok_s(pathbuf, ".", &save); tok; tok = strtok_s(NULL, ".", &save)) {
        if (nseg >= JSON_MAX_SEGS) return 0;
        segs[nseg++] = tok;
    }
    return (nseg > 3) ? nseg : 0;
}

int sh_json_patch_set_leaf(const char *full_json, const char *prop_path, const char *raw_leaf_token,
                            char *out, int outcap)
{
    if (!full_json || !prop_path || !raw_leaf_token || !out || outcap <= 0) return 0;
    if (full_json[0] != '{') return 0;
    const char *segs[JSON_MAX_SEGS];
    char pathbuf[512];
    int nseg = build_segs(pathbuf, sizeof pathbuf, prop_path, segs);
    if (nseg == 0) return 0;
    return json_walk_set(full_json, strlen(full_json), full_json, segs, nseg, 0, raw_leaf_token, out, outcap);
}

int sh_json_patch_upsert_reflist(const char *full_json, const char *prop_path,
                                  const char * const *id_strings, int n_ids, char *out, int outcap)
{
    if (!full_json || !prop_path || !id_strings || n_ids <= 0 || !out || outcap <= 0) return 0;
    if (full_json[0] != '{') return 0;
    const char *segs[JSON_MAX_SEGS];
    char pathbuf[512];
    int nseg = build_segs(pathbuf, sizeof pathbuf, prop_path, segs);
    if (nseg == 0) return 0;
    return json_walk_upsert_reflist(full_json, strlen(full_json), full_json, segs, nseg, 0,
                                     id_strings, n_ids, out, outcap);
}
