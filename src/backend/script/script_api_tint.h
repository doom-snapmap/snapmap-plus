/* script_api_tint.h -- the pure half of e:tint(r, g, b, [seconds]) and e:glow(r, g, b, [seconds]): what an
 * entity's look is as two colours, the render parms those colours drive, and the fade between two looks.
 * Header only, so the entity calls (script_api_entity.c) and the unit test (tests/script_api_tint_test.c) share
 * one implementation and nothing here touches the engine.
 *
 * An entity's look is a state: a tint (a colour multiplier, {1,1,1} = the entity's own look) and a glow (a
 * colour added, {0,0,0} = none). The state is written as three render parms, each at its engine default when
 * the state is neutral (build 13954591, the stock render programs' templates):
 *   color          {tint, 1}            default {1,1,1,1}: multiplies the emissive (bloom-mask) surfaces of every
 *                                       model program (lighting.inc: emissive = bloomMaskScale * mask^2 *
 *                                       colorScale * $color * $bloomColor)
 *   bloomcolor     {1 + glow, 1}        default {1,1,1,1}: the emissive's second factor, and the bloom colour the
 *                                       skinned demon programs add (skinvmtr, skinvmtrwounds: output_color +=
 *                                       bloomMaskScale * mask^2 * $bloomcolor)
 *   highlightcolor {tint - 1 + glow, 0} default {0,0,0,0}: added to the final colour of every model program
 *                                       (COMPUTE_SHADING: result.color0.xyz += $highlightColor.xyz), so it washes
 *                                       the whole lit surface; a tint below 1 subtracts, a glow adds
 * No model program multiplies a surface's albedo by a per-entity parm, so a tint reads as the product on the
 * emissive surfaces and as this wash elsewhere.
 *
 * A change is sent to the engine as one lerpRenderParmFrom event per parm (the engine's own fade, run in game
 * time by idGameLocal's render parm lerper): from the look the entity has now to the look it is given, over the
 * duration. An instant change is a fade of one millisecond, which also replaces a fade still running. The look
 * "now" is kept here, per entity handle, as the last request and the time it began, so a request made in the
 * middle of a fade starts from where the fade has got to. The table is fixed (SH_TINT_RECORDS); an entity that
 * has been displaced from it, or never asked, starts from the neutral look. */
#ifndef SH_SCRIPT_API_TINT_H
#define SH_SCRIPT_API_TINT_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_TINT_MAX_TINT     16.0     /* the largest tint channel */
#define SH_TINT_MAX_GLOW     64.0     /* the largest glow channel */
#define SH_TINT_MAX_SECONDS  3600.0   /* the longest fade */
/* e:glow(r, g, b, [seconds], [strength]): the glow added is the colour times the strength. A glow of full
 * strength (1, what e:glow gave before the strength) washes the whole lit surface with the colour, which hides
 * the surface under it at full blue. The default is a soft glow that leaves the surface readable, chosen from
 * frames of the Tyrant and a stock Baron in a lit arena (snaphak-re R-CR1, glow (0, 0.6, 1)): 0.3 and 0.15
 * still paint them nearly flat cyan, 0.08 glows clearly with the muscles, plates and horns readable, 0.04 and
 * below read as a dim tint. */
#define SH_TINT_GLOW_STRENGTH_DEFAULT 0.08
#define SH_TINT_MAX_STRENGTH 16.0
#define SH_TINT_PARMS        3
#define SH_TINT_RECORDS      256
#define SH_TINT_WASH_MIN     (-1.0f)  /* the wash never subtracts more than the colour it is added to */

#define SH_TINT_PARM_COLOR     "color"
#define SH_TINT_PARM_BLOOM     "bloomcolor"
#define SH_TINT_PARM_HIGHLIGHT "highlightcolor"

typedef struct sh_tint_state {
    float tint[3];
    float glow[3];
} sh_tint_state;

typedef struct sh_tint_parm {
    const char *name;    /* the renderParm decl */
    float       v[4];
} sh_tint_parm;

typedef struct sh_tint_rec {
    uint32_t      handle;      /* 0 = free */
    sh_tint_state from, to;
    int           start_ms, dur_ms;
} sh_tint_rec;

typedef struct sh_tint_table {
    sh_tint_rec rec[SH_TINT_RECORDS];
    int         next;          /* the record the next new entity takes (round robin) */
} sh_tint_table;

/* ------------------------------------------------------------------ the look */

static void sh_tint_neutral(sh_tint_state *s)
{
    int i;
    for (i = 0; i < 3; i++) { s->tint[i] = 1.0f; s->glow[i] = 0.0f; }
}

/* 1 when the look is the entity's own. */
static int sh_tint_is_neutral(const sh_tint_state *s)
{
    int i;
    for (i = 0; i < 3; i++) if (s->tint[i] != 1.0f || s->glow[i] != 0.0f) return 0;
    return 1;
}

/* The three render parms of a look, in a fixed order (color, bloomcolor, highlightcolor). */
static void sh_tint_parms(const sh_tint_state *s, sh_tint_parm out[SH_TINT_PARMS])
{
    int i;
    out[0].name = SH_TINT_PARM_COLOR;
    out[1].name = SH_TINT_PARM_BLOOM;
    out[2].name = SH_TINT_PARM_HIGHLIGHT;
    for (i = 0; i < 3; i++) {
        float wash = s->tint[i] - 1.0f + s->glow[i];
        out[0].v[i] = s->tint[i];
        out[1].v[i] = 1.0f + s->glow[i];
        out[2].v[i] = wash < SH_TINT_WASH_MIN ? SH_TINT_WASH_MIN : wash;
    }
    out[0].v[3] = 1.0f;
    out[1].v[3] = 1.0f;
    out[2].v[3] = 0.0f;
}

/* ----------------------------------------------------------------- arguments */

/* The words a refused channel is told in: "<call> needs <what> from 0 to <max>, got <value>". 1 when `value` is
 * a finite number in [0, max]; otherwise 0 and the sentence in `err`. */
static int sh_tint_check_channel(const char *call, const char *what, double value, double max, char *err, size_t cap)
{
    if (value == value && value >= 0.0 && value <= max) return 1;
    if (err && cap) {
        if (value == value) _snprintf_s(err, cap, _TRUNCATE, "\"%s\" needs %s from 0 to %g, got %g", call, what, max, value);
        else _snprintf_s(err, cap, _TRUNCATE, "\"%s\" needs %s from 0 to %g, got a value that is not a number", call, what, max);
    }
    return 0;
}

static int sh_tint_check_seconds(const char *call, double seconds, char *err, size_t cap)
{
    if (seconds == seconds && seconds >= 0.0 && seconds <= SH_TINT_MAX_SECONDS) return 1;
    if (err && cap)
        _snprintf_s(err, cap, _TRUNCATE, "\"%s\" needs the seconds to fade over from 0 to %g, got %g", call,
                    SH_TINT_MAX_SECONDS, seconds);
    return 0;
}

static int sh_tint_check_strength(const char *call, double strength, char *err, size_t cap)
{
    if (strength == strength && strength >= 0.0 && strength <= SH_TINT_MAX_STRENGTH) return 1;
    if (err && cap)
        _snprintf_s(err, cap, _TRUNCATE, "\"%s\" needs the strength from 0 to %g, got %g", call, SH_TINT_MAX_STRENGTH,
                    strength);
    return 0;
}

/* The glow a request adds: its colour times its strength. 1 when every channel stays within SH_TINT_MAX_GLOW;
 * otherwise 0 and the sentence in `err` (the colour and the strength were each in range, their product is not). */
static int sh_tint_glow_scaled(const char *call, float rgb[3], double strength, char *err, size_t cap)
{
    int i;
    for (i = 0; i < 3; i++) {
        double v = (double)rgb[i] * strength;
        if (v > SH_TINT_MAX_GLOW) {
            if (err && cap)
                _snprintf_s(err, cap, _TRUNCATE, "\"%s\" glows at most %g in a channel, got %g (colour %g at strength %g)",
                            call, SH_TINT_MAX_GLOW, v, (double)rgb[i], strength);
            return 0;
        }
    }
    for (i = 0; i < 3; i++) rgb[i] = (float)((double)rgb[i] * strength);
    return 1;
}

/* The engine's fade length for a request: whole milliseconds, at least 1 (a fade of 0 would never move). */
static int sh_tint_duration_ms(double seconds)
{
    double ms = seconds * 1000.0 + 0.5;
    if (!(ms >= 1.0)) return 1;
    if (ms > SH_TINT_MAX_SECONDS * 1000.0) ms = SH_TINT_MAX_SECONDS * 1000.0;
    return (int)ms;
}

/* ---------------------------------------------------------------------- fade */

/* The look a record shows at `now_ms`: its start look moved the fraction of the fade that has run. */
static void sh_tint_at(const sh_tint_rec *r, int now_ms, sh_tint_state *out)
{
    double f;
    int i;
    if (r->dur_ms <= 0) f = 1.0;
    else f = (double)((long long)now_ms - (long long)r->start_ms) / (double)r->dur_ms;
    if (!(f > 0.0)) f = 0.0;
    if (f > 1.0) f = 1.0;
    for (i = 0; i < 3; i++) {
        out->tint[i] = (float)(r->from.tint[i] + (r->to.tint[i] - r->from.tint[i]) * f);
        out->glow[i] = (float)(r->from.glow[i] + (r->to.glow[i] - r->from.glow[i]) * f);
    }
}

static sh_tint_rec *sh_tint_find(sh_tint_table *t, uint32_t handle)
{
    int i;
    if (!handle) return NULL;
    for (i = 0; i < SH_TINT_RECORDS; i++) if (t->rec[i].handle == handle) return &t->rec[i];
    return NULL;
}

/* Record a request: `kind` 0 sets the tint, 1 the glow. *from is the look the entity shows at `now_ms`; *to is
 * the look it is heading for: the half not asked for stays at the last request's target. Returns the record. */
static sh_tint_rec *sh_tint_begin(sh_tint_table *t, uint32_t handle, int kind, const float rgb[3], int now_ms,
                                  int dur_ms, sh_tint_state *from, sh_tint_state *to)
{
    sh_tint_rec *r = sh_tint_find(t, handle);
    if (r) {
        sh_tint_at(r, now_ms, from);
        *to = r->to;
    } else {
        sh_tint_neutral(from);
        *to = *from;
    }
    memcpy(kind ? to->glow : to->tint, rgb, 3 * sizeof(float));
    if (!r) {
        r = &t->rec[t->next];
        t->next = (t->next + 1) % SH_TINT_RECORDS;
    }
    r->handle = handle;
    r->from = *from;
    r->to = *to;
    r->start_ms = now_ms;
    r->dur_ms = dur_ms;
    return r;
}

/* Forget every record (a new map). */
static void sh_tint_reset(sh_tint_table *t)
{
    memset(t, 0, sizeof *t);
}

#ifdef __cplusplus
}
#endif
#endif
