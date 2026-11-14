/* diag_status.c -- see diag_status.h. Diagnostic build only. */
#ifdef SH_DIAG
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diag_status.h"
#include "feature_health.h"
#include "rawmap.h"
#include "../common/snapmap_plus_iface.h"
#include "../common/text.h"
#include "../fault_shield/crash_report.h"   /* sh_product_version */

volatile long sh_diag_note_listening;
volatile long sh_diag_note_count;
void *volatile sh_diag_hold_entity;

/* Kept out of line and with a body long enough to be hooked at its entry. */
__declspec(noinline) void sh_diag_note(const char *mod, const char *text, size_t len)
{
    if (mod && text && len) InterlockedIncrement(&sh_diag_note_count);
}

/* The object being built: malloc'd, grown as text is appended. */
typedef struct diag_text { char *buf; size_t len, cap; } diag_text;

/* Append text; 0 when memory runs out. */
static int put(diag_text *t, const char *s)
{
    size_t n = strlen(s);
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap : 256;
        char *grown;
        while (t->len + n + 1 > cap) cap *= 2;
        if ((grown = (char *)realloc(t->buf, cap)) == NULL) return 0;
        t->buf = grown;
        t->cap = cap;
    }
    memcpy(t->buf + t->len, s, n + 1);
    t->len += n;
    return 1;
}

/* Append s as a JSON string body (quotes, backslashes and controls escaped). */
static int put_escaped(diag_text *t, const char *s)
{
    char one[8];
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { one[0] = '\\'; one[1] = (char)c; one[2] = '\0'; }
        else if (c < 0x20) _snprintf_s(one, sizeof one, _TRUNCATE, "\\u%04x", c);
        else { one[0] = (char)c; one[1] = '\0'; }
        if (!put(t, one)) return 0;
    }
    return 1;
}

char *sh_diag_status_build(void)
{
    diag_text t = { NULL, 0, 0 };
    char head[160], *name, *raw;
    int i, ok;
    _snprintf_s(head, sizeof head, _TRUNCATE, "{\"protocol\":%d,\"version\":\"", SH_DIAG_STATUS_PROTOCOL);
    ok = put(&t, head) && put_escaped(&t, sh_product_version());
    _snprintf_s(head, sizeof head, _TRUNCATE, "\",\"iface_vtbl\":%u,\"features_off\":[",
                (unsigned)sizeof(sh_iface_vtbl));
    ok = ok && put(&t, head);
    for (i = 0; ok && (name = sh_feature_off_name(i)) != NULL; i++) {
        ok = put(&t, i ? ",\"" : "\"") && put_escaped(&t, name) && put(&t, "\"");
        free(name);
    }
    raw = ok ? sh_rawmap_diag_status() : NULL;
    ok = ok && raw && put(&t, "],\"rawmap\":") && put(&t, raw) && put(&t, "}");
    free(raw);
    if (!ok) { free(t.buf); return NULL; }
    return t.buf;
}

int sh_diag_status(char *out, int cap)
{
    char *text = sh_diag_status_build();
    int n;
    if (!text) {
        if (out && cap > 0) out[0] = '\0';
        return -1;
    }
    n = (int)strlen(text);
    if (out && cap > n) memcpy(out, text, (size_t)n + 1);
    else if (out && cap > 0) out[0] = '\0';
    free(text);
    return n;   /* the length written, or needed when cap <= n; -1 only when memory ran out */
}
#endif
