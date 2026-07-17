/* vcm_fallback.c -- the static corpus-snapshot fallback behind the class/inherit dropdowns.
 *
 * valid_class_map.h is the inherit -> Y -> valid-classes snapshot generated from the game's own shipped
 * entityDef declarations. The dropdowns normally enumerate the engine's LIVE type registry / entityDef
 * manager (typeinfo.c), but that path depends on per-build data addresses; on a build where those are
 * stale the enumerators refuse (fail-soft) and the dropdowns must serve from THIS snapshot instead --
 * an empty dropdown is a regression, not a safe degradation. Pure table walks + string packing: no
 * engine calls, no globals, thread-safe anywhere, unit-testable off the game. */
#include <string.h>

#include "vcm_fallback.h"
#include "valid_class_map.h"

/* pack a NUL string into out_buf at *pw (double-NUL-able); bumps *pw + returns 1 if it fit, else 0. */
int sh_vcm_pack(char *out_buf, int cap, int *pw, const char *s)
{
    int nlen = (int)strlen(s);
    if (*pw + nlen + 1 > cap - 1) return 0;          /* leave room for the trailing arena NUL */
    memcpy(out_buf + *pw, s, (size_t)nlen);
    out_buf[*pw + nlen] = '\0';
    *pw += nlen + 1;
    return 1;
}

/* The valid classes for `inherit`, from the snapshot: inherit -> Y (its base class) -> Y's class list.
 * Returns 1 if the inherit is in the snapshot (entries packed, *names bumped), else 0. */
int sh_vcm_fallback_valid_classes(const char *inherit, char *out_buf, int cap, int *written, int *names)
{
    const char *ey = NULL;
    for (int i = 0; i < SH_VCM_INHERIT_Y_N; i++)
        if (strcmp(SH_VCM_INHERIT_Y[i].inherit, inherit) == 0) { ey = SH_VCM_INHERIT_Y[i].y; break; }
    if (!ey) return 0;
    for (int i = 0; i < SH_VCM_Y_CLASSES_N; i++)
        if (strcmp(SH_VCM_Y_CLASSES[i].y, ey) == 0) {
            const vcm_yc *e = &SH_VCM_Y_CLASSES[i];
            for (int j = 0; j < e->n; j++) { if (!sh_vcm_pack(out_buf, cap, written, e->classes[j])) break; (*names)++; }
            return 1;
        }
    return 0;
}

/* The full snapshot inherit set (the inherit dropdown's fallback list) -- every inherit the shipped
 * declarations attest, already alphabetical in the generated table. Packs into out_buf (double-NUL end),
 * *out_count = names packed. Returns 1 on >=1 name. */
int sh_vcm_fallback_inherits(char *out_buf, int cap, int *out_count)
{
    if (out_count) *out_count = 0;
    if (!out_buf || cap <= 1) return 0;
    out_buf[0] = '\0';
    int written = 0, cnt = 0;
    for (int i = 0; i < SH_VCM_INHERIT_Y_N; i++) {
        if (!sh_vcm_pack(out_buf, cap, &written, SH_VCM_INHERIT_Y[i].inherit)) break;
        cnt++;
    }
    out_buf[written] = '\0';                         /* double-NUL end marker */
    if (out_count) *out_count = cnt;
    return cnt > 0 ? 1 : 0;
}
