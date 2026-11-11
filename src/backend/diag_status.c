/* diag_status.c -- see diag_status.h. Diagnostic build only. */
#ifdef SH_DIAG
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "diag_status.h"
#include "feature_health.h"
#include "rawmap.h"
#include "../common/snapmap_plus_iface.h"
#include "../fault_shield/crash_report.h"   /* sh_product_version */

/* Append text; 0 once the buffer is full. */
static int put(char *buf, size_t cap, size_t *len, const char *s)
{
    size_t n = strlen(s);
    if (*len + n + 1 > cap) return 0;
    memcpy(buf + *len, s, n + 1);
    *len += n;
    return 1;
}

/* Append s as a JSON string body (quotes, backslashes and controls escaped). */
static int put_escaped(char *buf, size_t cap, size_t *len, const char *s)
{
    char one[8];
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { one[0] = '\\'; one[1] = (char)c; one[2] = '\0'; }
        else if (c < 0x20) _snprintf_s(one, sizeof one, _TRUNCATE, "\\u%04x", c);
        else { one[0] = (char)c; one[1] = '\0'; }
        if (!put(buf, cap, len, one)) return 0;
    }
    return 1;
}

int sh_diag_status_build(char *buf, size_t cap)
{
    char head[160], name[96], raw[2048];
    size_t len = 0;
    int i, ok;
    if (!buf || !cap) return -1;
    buf[0] = '\0';
    _snprintf_s(head, sizeof head, _TRUNCATE, "{\"protocol\":%d,\"version\":\"", SH_DIAG_STATUS_PROTOCOL);
    ok = put(buf, cap, &len, head) && put_escaped(buf, cap, &len, sh_product_version());
    _snprintf_s(head, sizeof head, _TRUNCATE, "\",\"iface_vtbl\":%u,\"features_off\":[",
                (unsigned)sizeof(sh_iface_vtbl));
    ok = ok && put(buf, cap, &len, head);
    for (i = 0; ok && sh_feature_off_name(i, name, sizeof name); i++)
        ok = put(buf, cap, &len, i ? ",\"" : "\"") && put_escaped(buf, cap, &len, name) &&
             put(buf, cap, &len, "\"");
    ok = ok && sh_rawmap_diag_status(raw, sizeof raw) > 0 &&
         put(buf, cap, &len, "],\"rawmap\":") && put(buf, cap, &len, raw) && put(buf, cap, &len, "}");
    if (!ok) { buf[0] = '\0'; return -1; }
    return (int)len;
}

int sh_diag_status(char *out, int cap)
{
    static char big[16384];
    static SRWLOCK lock = SRWLOCK_INIT;
    int n;
    AcquireSRWLockExclusive(&lock);
    n = sh_diag_status_build(big, sizeof big);
    if (n >= 0 && out && cap > n) memcpy(out, big, (size_t)n + 1);
    else if (out && cap > 0) out[0] = '\0';
    ReleaseSRWLockExclusive(&lock);
    return n;   /* the length written, or needed when cap <= n; -1 only past 16 KiB */
}
#endif
