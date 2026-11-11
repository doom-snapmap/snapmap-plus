/* The open map's Snapmap+ settings: read from the map's member at load,
 * written back at save, held in between (map_settings.h). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "map_settings.h"
#include "map_carrier.h"

void backend_log(const char *message);

#define MS_RENDER 0
#define MS_BOUNDS 1
#define MS_VALUE_MAX 192

void sh_map_settings_default(sh_map_settings *s)
{
    if (!s) return;
    sh_map_render_default(&s->render);
    sh_map_bounds_default(&s->bounds);
}

/* The member's "settings" part (map_carrier.h): 1 with `s` holding it (an
 * absent key is that setting off), 0 when the map has no such part. A value
 * that does not read is off and counted in *refused. */
static int ms_member(const char *json, size_t len, sh_map_settings *s, unsigned *refused)
{
    static const char *const keys[2] = {"render", "bounds"};
    size_t from = 0, to = 0;
    int i;
    sh_map_settings_default(s);
    if (sh_carrier_part(json, len, "settings", &from, &to) != 1) return 0;
    for (i = 0; i < 2; i++) {
        const char *body = NULL;
        size_t n = 0;
        char text[MS_VALUE_MAX + 1];
        int got = sh_carrier_object_string(json + from, json + to, keys[i], &body, &n);
        if (got == 0) continue;
        if (got < 0 || n == 0 || n > MS_VALUE_MAX || memchr(body, '\\', n)) { (*refused)++; continue; }
        memcpy(text, body, n);
        text[n] = 0;
        if (i == MS_RENDER) {
            sh_map_render r;
            if (sh_map_render_decode(text, &r) && sh_map_render_valid(&r)) s->render = r;
            else (*refused)++;
        } else {
            sh_map_bounds b;
            if (sh_map_bounds_decode(text, &b) && sh_map_bounds_valid(&b)) s->bounds = b;
            else (*refused)++;
        }
    }
    return 1;
}

char *sh_map_settings_take(const char *json, size_t len, sh_map_settings *out, size_t *out_len,
                           sh_map_settings_report *report)
{
    sh_map_settings_report r;
    sh_map_settings s;
    memset(&r, 0, sizeof r);
    sh_map_settings_default(&s);
    if (out_len) *out_len = 0;
    if (json && len) r.member = ms_member(json, len, &s, &r.refused);
    if (out) *out = s;
    if (report) *report = r;
    /* Nothing to remove: the member goes with the parse detour's last step. */
    return NULL;
}

/* The row text for a setting that is on; NULL when it is off. */
static const char *ms_text(int which, const sh_map_settings *s, char *buf, size_t cap)
{
    if (which == MS_RENDER) return sh_map_render_encode(&s->render, buf, cap) ? buf : NULL;
    if (!sh_map_bounds_overrides(&s->bounds)) return NULL;
    return sh_map_bounds_encode(&s->bounds, buf, cap) ? buf : NULL;
}

char *sh_map_settings_put(const char *json, size_t len, const sh_map_settings *settings,
                          size_t *out_len, sh_map_settings_report *report)
{
    sh_map_settings_report r;
    char text[2][MS_VALUE_MAX + 1], value[2 * MS_VALUE_MAX + 48], err[160] = "";
    const char *want[2];
    int i;
    char *result = NULL;
    size_t vn = 0, put_len = 0;
    memset(&r, 0, sizeof r);
    if (out_len) *out_len = 0;
    if (!json || !len || !settings) { r.failed = 1; goto done; }
    for (i = 0; i < 2; i++) {
        want[i] = ms_text(i, settings, text[i], sizeof text[i]);
        /* The encodings are digits and separators; anything else is not written. */
        if (want[i] && (strchr(want[i], '"') || strchr(want[i], '\\'))) want[i] = NULL;
    }
    if (want[0] || want[1]) {
        vn = (size_t)_snprintf_s(value, sizeof value, _TRUNCATE, "{%s%s%s%s%s%s%s}",
                                 want[0] ? "\"render\":\"" : "", want[0] ? want[0] : "", want[0] ? "\"" : "",
                                 want[0] && want[1] ? "," : "",
                                 want[1] ? "\"bounds\":\"" : "", want[1] ? want[1] : "", want[1] ? "\"" : "");
        r.written = (want[0] ? 1u : 0u) + (want[1] ? 1u : 0u);
    }
    result = sh_carrier_put(json, len, "settings", vn ? value : NULL, vn, &put_len, err, sizeof err);
    if (result) {
        if (out_len) *out_len = put_len;
    } else if (err[0]) {
        r.failed = 1;
    }
done:
    if (report) *report = r;
    return result;
}

/* ------------------------------------------------------------ the open map */

static SRWLOCK g_lock = SRWLOCK_INIT;
static sh_map_settings g_current = {{0}, {0}};
static sh_map_settings g_pending = {{0}, {0}};
static sh_map_settings g_previous = {{0}, {0}};
static int g_previous_valid;

static void ms_describe(const sh_map_settings *s, char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "Unlock View Distance %s, Unlock Map Bounds %s",
                sh_map_render_unlocked(&s->render) ? "on" : "off",
                sh_map_bounds_enabled(&s->bounds) ? "on" : "off");
}

char *sh_map_settings_prepare_load(const char *json, size_t len, size_t *out_len)
{
    sh_map_settings s;
    sh_map_settings_report r;
    char *out, line[320], what[96];
    out = sh_map_settings_take(json, len, &s, out_len, &r);
    AcquireSRWLockExclusive(&g_lock);
    g_pending = s;
    ReleaseSRWLockExclusive(&g_lock);
    if (r.member) {
        ms_describe(&s, what, sizeof what);
        _snprintf_s(line, sizeof line, _TRUNCATE, "SETTINGS: map read: %s (from the snapmap-plus member); %u unreadable",
                    what, r.refused);
        backend_log(line);
    }
    return out;
}

void sh_map_settings_loaded(int ok)
{
    if (!ok) return;
    AcquireSRWLockExclusive(&g_lock);
    g_previous = g_current;
    g_previous_valid = 1;
    g_current = g_pending;
    ReleaseSRWLockExclusive(&g_lock);
}

void sh_map_settings_current(sh_map_settings *out)
{
    if (!out) return;
    AcquireSRWLockShared(&g_lock);
    *out = g_current;
    ReleaseSRWLockShared(&g_lock);
}

void sh_map_settings_set_render(const sh_map_render *render)
{
    if (!render || !sh_map_render_valid(render)) return;
    AcquireSRWLockExclusive(&g_lock);
    g_current.render = *render;
    ReleaseSRWLockExclusive(&g_lock);
}

void sh_map_settings_set_bounds(const sh_map_bounds *bounds)
{
    if (!bounds || !sh_map_bounds_valid(bounds)) return;
    AcquireSRWLockExclusive(&g_lock);
    g_current.bounds = *bounds;
    ReleaseSRWLockExclusive(&g_lock);
}

void sh_map_settings_reset(void)
{
    AcquireSRWLockExclusive(&g_lock);
    sh_map_settings_default(&g_current);
    g_previous_valid = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

int sh_map_settings_restore_previous(void)
{
    int ok;
    AcquireSRWLockExclusive(&g_lock);
    ok = g_previous_valid;
    if (ok) g_current = g_previous;
    g_previous_valid = 0;
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

char *sh_map_settings_on_save(const char *json, size_t len, size_t *out_len)
{
    sh_map_settings s;
    sh_map_settings_report r;
    char *out, line[320], what[96];
    sh_map_settings_current(&s);
    out = sh_map_settings_put(json, len, &s, out_len, &r);
    if (r.written || r.failed) {
        ms_describe(&s, what, sizeof what);
        _snprintf_s(line, sizeof line, _TRUNCATE, "SETTINGS: map saved: %s; %u setting(s) in the snapmap-plus member%s",
                    what, r.written, r.failed ? "; FAILED: the saved text did not read, it carries what it had" : "");
        backend_log(line);
    }
    return out;
}
