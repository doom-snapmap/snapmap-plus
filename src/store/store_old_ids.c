/* store_old_ids.c -- the builder helpers only the store tool's conversion of
 * earlier shapes uses (declared in modbundle.h): an earlier release's mod id,
 * its install state file, and a bundle's content key. The game never links
 * this file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modbundle.h"
#include "mp_json.h"

int sh_mb_id_normalize(const char *in, size_t len, char out[SH_MB_ID_MAX + 1])
{
    size_t i, n;
    char buf[SH_MB_ID_MAX + 1];
    if (!in) return 0;
    while (len && strchr(" \t\r\n", *in)) { in++; len--; }
    while (len && strchr(" \t\r\n", in[len - 1])) len--;
    if (len == 0 || len > SH_MB_ID_MAX) return 0;
    for (i = 0; i < len; i++) {
        char c = in[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        buf[i] = c;
    }
    buf[len] = 0;
    n = len;
    for (i = 0; i < n; i++) {
        char c = buf[i];
        int alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) continue;
        if (c != '.' && c != '_' && c != '-') return 0;
        if (i == 0 || i + 1 == n) return 0;
        if (c == '.' && buf[i - 1] == '.') return 0;
    }
    memcpy(out, buf, n + 1);
    return 1;
}

int sh_mb_state_parse(const char *text, size_t len, int *validated, long long *installed_at, char *cause, size_t cap)
{
    mpj *root = text ? mpj_parse(text, len) : NULL;
    const mpj *v, *at, *fc;
    int ok = 0;
    if (validated) *validated = 0;
    if (installed_at) *installed_at = 0;
    if (cause && cap) cause[0] = 0;
    if (!root || root->type != MPJ_OBJ) goto done;
    v = mpj_get(root, "validated");
    at = mpj_get(root, "installedAt");
    fc = mpj_get(root, "firstCause");
    if (!v || v->type != MPJ_BOOL) goto done;
    if (validated) *validated = v->b;
    if (at && at->type == MPJ_NUM && at->text && installed_at) *installed_at = _strtoi64(at->text, NULL, 10);
    if (fc && fc->type == MPJ_STR && fc->text && cause && cap) strncpy_s(cause, cap, fc->text, _TRUNCATE);
    ok = 1;
done:
    mpj_free(root);
    return ok;
}

typedef struct sk_buf { char *p; size_t n, cap; int oom; } sk_buf;

static void sk_puts(sk_buf *b, const char *s)
{
    size_t n = strlen(s);
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        char *g;
        while (cap < b->n + n + 1) cap *= 2;
        if (!(g = (char *)realloc(b->p, cap))) { b->oom = 1; return; }
        b->p = g;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n + 1);
    b->n += n;
}

/* SHA-256 over the salt, requirements, shadows, the preserved author keys
 * (canonical text) and every file's request, kind, hash and size: two
 * bundles with one key serve the same bytes under the same names. */
void sh_mb_content_key(const sh_mb_bundle *b, char out[SH_MB_HASH_HEX + 1])
{
    sk_buf o = {0};
    size_t i;
    out[0] = 0;
    if (!b) return;
    sk_puts(&o, b->id);
    sk_puts(&o, "\n");
    if (b->requirements) sk_puts(&o, b->requirements);
    sk_puts(&o, "\n");
    for (i = 0; i < b->shadow_count; i++) { sk_puts(&o, b->shadows[i]); sk_puts(&o, "\n"); }
    sk_puts(&o, "--\n");
    if (b->extra) sk_puts(&o, b->extra);
    sk_puts(&o, "\n--\n");
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        char size[32];
        sk_puts(&o, f->request ? f->request : "");
        sk_puts(&o, f->stock ? "\tstock " : "\tcarried ");
        if (f->stock) sk_puts(&o, f->stock_source ? f->stock_source : "");
        sk_puts(&o, "\t");
        sk_puts(&o, f->blob);
        _snprintf_s(size, sizeof size, _TRUNCATE, "\t%llu\n", f->stock ? 0ull : f->size);
        sk_puts(&o, size);
    }
    if (!o.oom && o.p) sh_mb_sha256_hex(o.p, o.n, out);
    free(o.p);
}
