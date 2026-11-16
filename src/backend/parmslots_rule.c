/* parmslots_rule.c -- where a render parm that asks for a parm state slot goes (parmslots.h). Pure. */
#include <string.h>

#include "parmslots.h"

int sh_parmslots_range(int kind, int *lo, int *hi)
{
    int a, b;
    if (kind == 0) { a = 0x000; b = 0x400; }                       /* vector */
    else if (kind >= 1 && kind <= 9) { a = 0x400; b = 0x600; }     /* texture */
    else if (kind == 10) { a = 0x600; b = 0x680; }                 /* sampler */
    else if (kind == 11) { a = 0x680; b = 0x800; }                 /* program */
    else if (kind == 12 || kind == 14) { a = 0x800; b = 0x880; }   /* string */
    else if (kind == 13) { a = 0x880; b = 0x8C0; }                 /* buffer */
    else if (kind == 15) { a = 0x8C0; b = 0x900; }                 /* uniform buffer */
    else if (kind >= 16 && kind <= 18) { a = 0x900; b = 0x93F; }   /* image buffer */
    else return 0;
    if (lo) *lo = a;
    if (hi) *hi = b;
    return 1;
}

static int is_reserved(const char *name, const char *const *reserved, unsigned n)
{
    unsigned i;
    if (!name) return 0;
    for (i = 0; i < n; i++)
        if (reserved[i] && !strcmp(reserved[i], name)) return 1;
    return 0;
}

int sh_parmslots_decide(int kind, const char *name, sh_parmslots_read read, void *context,
                        const char *const *reserved, unsigned n_reserved, int spill, int *becomes_spill)
{
    int lo, hi, s, free_slots = 0, same_kind = -1, held_reserved = 0, missing, mine;
    if (becomes_spill) *becomes_spill = 0;
    if (!read || !sh_parmslots_range(kind, &lo, &hi)) return SH_PARMSLOTS_ENGINE;
    for (s = lo; s < hi; s++) {
        const char *holder = NULL;
        int holder_kind = -1;
        if (!read(context, s, &holder, &holder_kind)) { free_slots++; continue; }
        if (name && holder == name) return SH_PARMSLOTS_ENGINE;     /* the engine gives the same slot again */
        if (holder_kind == kind) same_kind = s;
        if (kind == SH_PARMSLOTS_VECTOR && is_reserved(holder, reserved, n_reserved)) held_reserved++;
    }
    if (kind != SH_PARMSLOTS_VECTOR) {
        if (free_slots > 0) return SH_PARMSLOTS_ENGINE;
        return same_kind >= 0 ? same_kind : hi - 1;
    }
    mine = is_reserved(name, reserved, n_reserved);
    missing = (int)n_reserved - held_reserved - mine;               /* reserved parms still without a slot */
    if (missing < 0) missing = 0;
    if (mine) return free_slots > 0 ? SH_PARMSLOTS_ENGINE : (spill >= 0 ? spill : hi - 1);
    if (spill >= 0) return free_slots > missing ? SH_PARMSLOTS_ENGINE : spill;
    if (free_slots > missing + 1) return SH_PARMSLOTS_ENGINE;
    if (free_slots > 0) {                                           /* the last slot to give: it becomes shared */
        if (becomes_spill) *becomes_spill = 1;
        return SH_PARMSLOTS_ENGINE;
    }
    return hi - 1;
}
