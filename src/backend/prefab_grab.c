/* Prefab Create grab-frame guard: engine-free math. See prefab_grab.h. */
#include "prefab_grab.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PG_DEG2RAD 0.017453292f

static int pg_finite3(const float *v)
{
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

void sh_prefab_grab_forward(const float angles[3], float forward[3])
{
    float p = angles[0] * PG_DEG2RAD, y = angles[1] * PG_DEG2RAD;
    float cp = cosf(p);
    forward[0] = cosf(y) * cp;
    forward[1] = sinf(y) * cp;
    forward[2] = -sinf(p);
}

void sh_prefab_grab_centred_box(const float size[3], float bmin[3], float bmax[3])
{
    int i;
    for (i = 0; i < 3; i++) { bmin[i] = size[i] * -0.5f; bmax[i] = size[i] * 0.5f; }
}

void sh_prefab_grab_box_centre(const sh_prefab_grab_box *box, float centre[3])
{
    float l[3];
    int i;
    for (i = 0; i < 3; i++) l[i] = (box->bmin[i] + box->bmax[i]) * 0.5f;
    for (i = 0; i < 3; i++)
        centre[i] = l[0] * box->axis[i] + l[1] * box->axis[3 + i] + l[2] * box->axis[6 + i] +
                    box->origin[i];
}

static int pg_box_valid(const sh_prefab_grab_box *box)
{
    int i;
    if (!box) return 0;
    if (!pg_finite3(box->origin) || !pg_finite3(box->bmin) || !pg_finite3(box->bmax)) return 0;
    for (i = 0; i < 9; i++) if (!isfinite(box->axis[i])) return 0;
    for (i = 0; i < 3; i++) if (box->bmin[i] > box->bmax[i]) return 0;
    return 1;
}

float sh_prefab_grab_outside(const sh_prefab_grab_box *box, const float p[3])
{
    float d[3], worst = 0.0f;
    int i;
    if (!pg_box_valid(box) || !pg_finite3(p)) return -1.0f;
    for (i = 0; i < 3; i++) d[i] = p[i] - box->origin[i];
    for (i = 0; i < 3; i++) {
        /* Row i is local axis i in world; the axes are orthonormal (mirrored
         * placements only flip a sign), so a dot product is the local coordinate. */
        const float *a = box->axis + 3 * i;
        float l = d[0] * a[0] + d[1] * a[1] + d[2] * a[2];
        float o = l < box->bmin[i] ? box->bmin[i] - l : l > box->bmax[i] ? l - box->bmax[i] : 0.0f;
        if (!isfinite(o)) return -1.0f;
        if (o > worst) worst = o;
    }
    return worst;
}

int sh_prefab_grab_guard(const float view[3], const float forward[3],
                         const sh_prefab_grab_box *box, float frame[12],
                         float *distance, sh_prefab_grab_result *result)
{
    sh_prefab_grab_result r;
    float d = *distance, tol, c[3], t;
    int i, bounded;

    memset(&r, 0, sizeof r);
    r.native_distance = d;
    r.distance = d;
    for (i = 0; i < 3; i++) r.native_point[i] = r.point[i] = frame[i];
    r.outside = -1.0f;

    bounded = isfinite(d) && fabsf(d) <= SH_PREFAB_GRAB_MAX_DISTANCE && pg_finite3(frame);
    for (i = 3; i < 12; i++) if (!isfinite(frame[i])) bounded = 0;

    if (!pg_box_valid(box) || !pg_finite3(view) || !pg_finite3(forward)) {
        /* Nothing to measure against: keep a usable native frame, refuse the rest. */
        r.status = bounded ? SH_PREFAB_GRAB_OK : SH_PREFAB_GRAB_REFUSED;
        if (result) *result = r;
        return r.status;
    }

    r.outside = sh_prefab_grab_outside(box, frame);
    /* A hit leaves the midpoint inside the box up to float rounding of the
     * clip, which grows with the distance along the line. */
    tol = 1.0f + (isfinite(d) ? fabsf(d) : 0.0f) * (1.0f / 65536.0f);
    if (bounded && r.outside >= 0.0f && r.outside <= tol) {
        r.status = SH_PREFAB_GRAB_OK;
        if (result) *result = r;
        return r.status;
    }

    /* Build the frame from the hovered box: its centre, its own axis (the
     * native helper's choice, which paste reapplies), and the centre's depth
     * along the view as the paste distance. */
    sh_prefab_grab_box_centre(box, c);
    t = (c[0] - view[0]) * forward[0] + (c[1] - view[1]) * forward[1] + (c[2] - view[2]) * forward[2];
    if (!pg_finite3(c) || !isfinite(t)) {
        r.status = SH_PREFAB_GRAB_REFUSED;
        if (result) *result = r;
        return r.status;
    }
    if (t < SH_PREFAB_GRAB_MIN_DISTANCE) t = SH_PREFAB_GRAB_MIN_DISTANCE;
    if (t > SH_PREFAB_GRAB_MAX_DISTANCE) t = SH_PREFAB_GRAB_MAX_DISTANCE;
    for (i = 0; i < 3; i++) frame[i] = r.point[i] = c[i];
    for (i = 0; i < 9; i++) frame[3 + i] = box->axis[i];
    *distance = r.distance = t;
    r.status = SH_PREFAB_GRAB_CORRECTED;
    if (result) *result = r;
    return r.status;
}

/* Find "key" followed by optional spaces and a colon; return the byte after
 * the colon, or NULL. */
static const char *pg_after_key(const char *p, const char *end, const char *key, const char **next)
{
    size_t kl = strlen(key);
    while (p && p + kl <= end) {
        const char *hit = NULL, *q;
        for (q = p; q + kl <= end; q++)
            if (*q == '"' && !memcmp(q, key, kl)) { hit = q; break; }
        if (!hit) return NULL;
        q = hit + kl;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        if (next) *next = hit + kl;
        if (q < end && *q == ':') return q + 1;
        p = hit + kl;
    }
    return NULL;
}

static int pg_number(const char *p, const char *end, double *out)
{
    char buf[64];
    size_t n = 0;
    char *stop = NULL;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    while (p < end && n + 1 < sizeof buf &&
           (strchr("+-.#0123456789eE", *p) || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
        buf[n++] = *p++;
    buf[n] = '\0';
    if (!n) return 0;
    *out = strtod(buf, &stop);
    if (stop == buf || *stop) {
        /* A non-finite float can be written as a word ("inf", "nan", "1.#INF").
         * Any other word (true, null) is not a number. */
        size_t i;
        for (i = 0; i < n; i++) if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] = (char)(buf[i] + 32);
        if (strstr(buf, "inf") || strstr(buf, "nan") || stop != buf) { *out = HUGE_VAL; return 1; }
        return 0;
    }
    return 1;
}

int sh_prefab_grab_json_check(const char *json, size_t len, double *distance)
{
    const char *end = json + len, *next = NULL, *v, *p;
    double d = 0.0;
    if (distance) *distance = 0.0;
    if (!json) return -1;
    v = pg_after_key(json, end, "\"grabDistance\"", &next);
    if (!v || !pg_number(v, end, &d)) return -1;
    if (distance) *distance = d;
    if (!isfinite(d) || fabs(d) > SH_PREFAB_GRAB_MAX_DISTANCE) return 0;

    p = json;
    while ((v = pg_after_key(p, end, "\"spawnPosition\"", &next)) != NULL) {
        const char *open = v, *close;
        while (open < end && *open != '{') open++;
        close = open;
        while (close < end && *close != '}') close++;
        if (open >= end || close >= end) break;
        for (p = open; p < close; p++) {
            double x;
            if (*p != ':') continue;
            if (pg_number(p + 1, close, &x) && (!isfinite(x) || fabs(x) > SH_PREFAB_GRAB_MAX_POSITION))
                return 0;
        }
        p = close;
    }
    return 1;
}
