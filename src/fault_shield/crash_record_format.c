/* Format complete JSON directly into caller-owned storage. */
#include "crash_record_format.h"
#include <stdio.h>
#include <string.h>

/* Return one complete UTF-8 sequence, or zero for an invalid byte. */
static size_t utf8_width(const unsigned char *p)
{
    size_t n, i;
    if (*p < 0x80) return 1;
    if (*p >= 0xc2 && *p <= 0xdf) n = 2;
    else if (*p >= 0xe0 && *p <= 0xef) n = 3;
    else if (*p >= 0xf0 && *p <= 0xf4) n = 4;
    else return 0;
    for (i = 1; i < n; i++) if ((p[i] & 0xc0) != 0x80) return 0;
    if ((*p == 0xe0 && p[1] < 0xa0) || (*p == 0xed && p[1] >= 0xa0) ||
        (*p == 0xf0 && p[1] < 0x90) || (*p == 0xf4 && p[1] >= 0x90)) return 0;
    return n;
}

static size_t escaped_unit(const unsigned char *p, char out[6], size_t *consumed)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char c = *p;
    size_t n;
    *consumed = 1;
    if (c == '"' || c == '\\') { out[0] = '\\'; out[1] = (char)c; return 2; }
    if (c == '\n' || c == '\r' || c == '\t') {
        out[0] = '\\'; out[1] = c == '\n' ? 'n' : c == '\r' ? 'r' : 't'; return 2;
    }
    if (c < 0x20) {
        memcpy(out, "\\u00", 4); out[4] = hex[c >> 4]; out[5] = hex[c & 15]; return 6;
    }
    n = utf8_width(p);
    if (!n) { memcpy(out, "\\ufffd", 6); return 6; }
    memcpy(out, p, n); *consumed = n; return n;
}

int crash_json_escape(char *dst, size_t cap, const char *src)
{
    size_t used = 0;
    if (!dst || !cap) return 0;
    if (!src) src = "";
    while (*src) {
        char unit[6]; size_t consumed, n = escaped_unit((const unsigned char *)src, unit, &consumed);
        if (n >= cap - used) break;
        memcpy(dst + used, unit, n); used += n; src += consumed;
    }
    dst[used] = '\0';
    return (int)used;
}

typedef struct json_writer { char *buf; size_t cap, used; int ok; } json_writer;

static void append(json_writer *w, const char *text, size_t n)
{
    if (!w->ok) return;
    if (n >= w->cap - w->used) { w->ok = 0; return; }
    memcpy(w->buf + w->used, text, n); w->used += n; w->buf[w->used] = '\0';
}

static void field(json_writer *w, const char *name, const char *value, int last)
{
    append(w, "\"", 1); append(w, name, strlen(name)); append(w, "\":\"", 3);
    if (!value) value = "";
    while (*value && w->ok) {
        char unit[6]; size_t consumed, n = escaped_unit((const unsigned char *)value, unit, &consumed);
        append(w, unit, n); value += consumed;
    }
    append(w, last ? "\"}" : "\",", 2);
}

int crash_record_json(char *buf, size_t cap, const crash_record *r)
{
    json_writer w = {buf, cap, 0, 1};
    char number[32];
    if (!buf || !cap) return 0;
    buf[0] = '\0';
    if (!r) return 0;
    append(&w, "{", 1);
    field(&w, "kind", r->kind ? r->kind : "unknown", 0);
    _snprintf_s(number, sizeof number, _TRUNCATE, "0x%08lx", r->code);
    field(&w, "code", number, 0);
    _snprintf_s(number, sizeof number, _TRUNCATE, "0x%llx", r->rip_rva);
    field(&w, "rip_rva", number, 0);
    _snprintf_s(number, sizeof number, _TRUNCATE, "0x%llx", r->fault_addr);
    field(&w, "fault_addr", number, 0);
    field(&w, "module", r->module, 0);
    field(&w, "stack", r->stack, 0);
    field(&w, "engineText", r->engine_text, 0);
    field(&w, "dump", r->dump, 0);
    field(&w, "version", r->version, 0);
    field(&w, "renderer", r->renderer, 0);
    field(&w, "time", r->time, 1);
    if (!w.ok) { buf[0] = '\0'; return 0; }
    return (int)w.used;
}
