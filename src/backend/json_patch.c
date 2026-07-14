/* json_patch.c -- see json_patch.h. A hand-written recursive-descent SKIP scanner (never a full
 * parse-to-tree) over the engine's compact JSON, used only to locate key/value SPANS so we can splice
 * raw text in place. Every recursive walker strictly increases its segment index each call (bounded by
 * JSON_MAX_SEGS), so there is no unbounded recursion on attacker/malformed input -- worst case is a
 * clean 0 return.
 *
 * Clean-room: our own design; generalizes apply_engine.c's proven ae_splice_targets technique (insert
 * item[N] before "num", bump "num") to an arbitrary dotted path + a real dedup-append. Zero OG bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_patch.h"

#define JSON_MAX_SEGS   16
#define JSON_CHAIN_CAP  (16 * 1024)

/* ============================================================ the SKIP scanner ===================
 * Every json_skip_* returns a pointer just PAST the scanned token, or NULL on malformed/truncated
 * input. No allocation, no tree -- callers keep only the spans they need. */

static const char *json_skip_ws(const char *p)
{
    if (!p) return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* p must point AT the opening '"'. Returns a pointer just past the CLOSING '"' (handles \" and \\
 * escapes so an escaped quote never ends the string early). NULL if unterminated. */
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

/* p must point at the FIRST character of a value (object/array/string/number/true/false/null). Returns
 * a pointer just past the value's last character, or NULL on malformed input. Recurses into nested
 * objects/arrays (depth bounded by the real JSON nesting depth -- entity JSON is not pathological). */
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

/* Find `key` as a DIRECT (top-level) member of the object spanning [obj_open, obj_close) (obj_open AT
 * '{', obj_close just past the matching '}'). Only scans this object's own members -- never matches a
 * same-named key nested inside a child object/array, unlike a naive strstr. On a hit, *val_start points
 * at the value's first character and *val_end just past its last; *key_start (if non-NULL) points at the
 * key's OPENING quote, i.e. the true start of the "key":value member -- needed by any caller that must
 * splice/insert relative to the member as a whole, not just its value (get this wrong and a splice meant
 * to land BEFORE the key lands after the colon instead, corrupting the JSON -- see json_patch.c's own
 * postmortem comment on the accl/acctargets merge bug this guarded against). Returns 1 on a hit, 0 on a
 * miss or malformed object (never partial: outputs are untouched on a 0 return). */
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

/* ============================================================ splice / insert / build primitives == */

/* out := doc[0..cut_start) + repl + doc[cut_end..doclen). Bounds-checked; 0 on overflow (out untouched
 * in any meaningful way on failure -- caller must not use it). */
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

/* Insert `"key":value_token` as a new member of the object [obj_open, obj_close), right after the
 * opening '{' (before any existing members, with a trailing ',' if the object wasn't empty). */
static int json_insert_member(const char *doc, size_t doclen, const char *obj_open, const char *obj_close,
                               const char *key, const char *value_token, char *out, int outcap)
{
    const char *p = json_skip_ws(obj_open + 1);
    if (!p) return 0;
    /* empty-object test: the first non-ws char after '{' is the closing '}'. (NOT `p == obj_close` --
     * obj_close points PAST the '}', so an empty object {} has p AT the '}' but p != obj_close, which
     * mis-classified {} as non-empty and appended a trailing comma -> `{"k":v,}` -> the engine lexer
     * rejects it. Live-found on an edit-less entity whose `edit` serialized as `{}`.) */
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

/* Decode-compare a JSON string SPAN (span_start AT the opening '"', span_end just past the closing '"')
 * against a raw (unescaped) C string, without materializing a decoded copy. A handful of rare escapes
 * (\uXXXX) are not decoded -- on those bytes this degrades to "not equal" (never a false positive
 * match), so the worst case is a harmless duplicate append, never a wrong dedup-skip. */
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

/* ============================================================ the two recursive walkers =========== */

/* set_leaf's walker: descend through EXISTING objects along segs[idx..]; once the chain breaks (a
 * segment is missing, or exists but isn't an object short of the leaf), build the remainder fresh in
 * ONE splice/insert against the ORIGINAL doc -- there is never more than one text mutation per call,
 * because pure descent never touches doc at all. */
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
        /* segs[idx] is missing, OR it exists but its value is NOT an object (e.g. `"edit":null` /
         * `"edit":""` on an edit-less entity -- the engine serializes an empty edit that way, and QJson
         * laundered it into `{}` on the Qt path). Build the remaining path segs[idx+1..] as a BRACED
         * OBJECT VALUE `{"segs[idx+1]":{...:leaf}}` -- NOT the bare `"key":value` member json_build_
         * scalar_chain returns, which when spliced as segs[idx]'s value produced the invalid
         * `"edit":"renderModelInfo":{...}` the engine lexer rejected (live-found: bss/acctargets
         * "applied 0/1"). Both paths below use segs[idx]'s VALUE = this braced object:
         *   found  -> splice [vs,ve) (replace the non-object value with the object),
         *   !found -> insert `"segs[idx]":<object>`. */
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
        /* segs[idx] missing OR a non-object value -- build segs[idx+1..] as a BRACED OBJECT VALUE, same
         * fix + rationale as json_walk_set above (a bare member spliced as edit's value gave the invalid
         * `"edit":"targets":{...}` the lexer rejected on an edit-less entity). */
        char inner[JSON_CHAIN_CAP];
        char chain[JSON_CHAIN_CAP];
        if (!json_build_list_chain(segs, nseg, idx + 1, ids, n_ids, inner, (int)sizeof inner)) return 0;
        if (_snprintf_s(chain, sizeof chain, _TRUNCATE, "{%s}", inner) <= 0) return 0;
        if (found) return json_splice(doc, doclen, vs, ve, chain, out, outcap);
        return json_insert_member(doc, doclen, obj_open, obj_close, segs[idx], chain, out, outcap);
    }

    /* idx == nseg-1: the leaf. If it's already a proper num/item[] list, dedup-append (the
     * ae_splice_targets technique: insert new item[N] members right before "num", bump "num"). */
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
                /* nothing new (every id already targeted) -- output doc unchanged, verbatim. */
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
            /* BUGFIX (found via a live acctargets 0/1-apply repro): this MUST be num_key (the "num" key's
             * own opening quote), not num_vs (its VALUE's start, i.e. the digit). num_vs..ve is only the
             * digit + trailing '}' -- using it here silently swallowed the literal "num": text into the
             * "kept verbatim" prefix, so the re-emitted "\"num\":" below duplicated it and the new items
             * landed AFTER "num" instead of before -- e.g. {"item[0]":"a","num":"item[1]":"b","num":2} --
             * a colon directly after a colon, which the engine's deserialize correctly (silently) rejects
             * (apply_engine.c's "applied 0/1"). Only exercised when a PRE-EXISTING targets list is being
             * merged into; a fresh/empty list (json_build_fresh_list) never took this path, which is why
             * it looked flaky rather than consistently broken. */
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
        /* an object, but not list-shaped (no "num") -- fall through to wholesale replace. */
    }
    {
        char freshlist[JSON_CHAIN_CAP];
        if (!json_build_fresh_list(ids, n_ids, freshlist, (int)sizeof freshlist)) return 0;
        if (found) return json_splice(doc, doclen, vs, ve, freshlist, out, outcap);
        return json_insert_member(doc, doclen, obj_open, obj_close, segs[idx], freshlist, out, outcap);
    }
}

/* ============================================================ public entry points ================= */

/* Splits prop_path on '.' into segs[3..], having already seeded segs[0..2] = entityDef/state/edit.
 * Returns the segment count, or 0 on overflow/empty. pathbuf is the caller's scratch (must outlive the
 * walk -- strtok_s writes NULs into it and segs[] points into it). */
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
