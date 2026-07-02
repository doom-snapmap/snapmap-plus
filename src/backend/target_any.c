/* target_any.c -- see target_any.h. The console command sh_target_any: a clean-room port of OG SnapHak's
 * own sh_target_any (FUN_180021EE0), the editor-decl visibility bit-flip.
 *
 * OG MECHANISM (FUN_180021EE0, pair-for-pair, from our own reverse-engineering):
 *   a global toggle flag (OG DAT_18003e818) selects the direction; each call walks EVERY
 *   idDeclSnapEditorEntity decl and:
 *     REVEAL  (flag 0 -> 1): *(byte*)(decl + 0x3CD) |= 0xC0            // set the visibility pair (bits 7-6)
 *     RE-HIDE (flag 1 -> 0): if className(decl) != "idInfoPath":
 *                             *(byte*)(decl + 0x3CD) &= 0x3F           // clear bits 7-6 (idInfoPath stays visible)
 *   Revealing the editor-visibility pair is what surfaces the campaign-only / normally-hidden placeable
 *   entity decls in the SnapMap editor palette (and, since a wire is drawn green iff BOTH endpoints have
 *   decl+0x3CD >= 0x80, colours their logic wires green -- the visible "green wires" effect).
 *
 * OFFSETS -- live-build provenance:
 *   The decl registry: reached via the engine GetDeclsOfType("idDeclSnapEditorEntity") -> the typed
 *   decl-manager node. array = *(node + 0x20), count = *(uint*)(node + 0x28). LIVE-VERIFIED: the engine's
 *   own indexed getter idResourceList::Get reads *(node+0x20) + idx*8 after bounds-checking node+0x28.
 *   decl+0x3CD: the editor-flags byte; the palette validator (FUN_1404F8180) reads its 0x20 (isOutput) /
 *   0x10 (isInput) bits, and the reveal pair is the high bits 0x80/0x40 (= 0xC0), distinct from those.
 *   decl+0x1C8: the entityDef pointer (LIVE-VERIFIED via the palette validator's null-entityDef reject).
 *   entityDef+0x60: the className idStr ptr -- read ONLY for the re-hide idInfoPath spare. LIVE-CONFIRMED
 *   by our own reverse-engineering (an entityDef's className(Y) is at +0x60). Read SEH-guarded + only
 *   after the entityDef-non-null gate; the reveal direction never reads it.
 *
 * NO FREE (a deliberate, build-verified divergence from a literal OG port): OG's routine frees its decl
 * array after the walk, but on THIS DOOM build node+0x20 IS the live-owned registry array (proven by
 * idResourceList::Get above), so freeing it would corrupt the decl registry. We therefore never free.
 *
 * Every engine memory touch is SEH-guarded. Clean-room: zero OG SnapHak bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "target_any.h"
#include "commands.h"     /* idCmdArgs / sh_printf */
#include "backend_log.h"

#define DECL_TYPE_NAME      "idDeclSnapEditorEntity"
#define LIST_ARRAY_OFF      0x20     /* decl-manager node -> decl-pointer array */
#define LIST_COUNT_OFF      0x28     /* decl-manager node -> decl count (uint) */
#define DECL_FLAGS_OFF      0x3CD    /* decl -> editor-flags byte; bits 7-6 (0xC0) = the visibility pair */
#define DECL_ENTITYDEF_OFF  0x1C8    /* decl -> entityDef pointer */
#define ENTITYDEF_CLASS_OFF 0x60     /* entityDef -> className idStr ptr (re-hide idInfoPath spare only) */
#define VIS_BITS            0xC0     /* the editor-visibility pair the toggle flips */
#define COUNT_CAP           (1u << 20) /* stale-manager-node guard: the registry is ~1361 decls */

typedef void *(*get_decls_fn)(const char *type_name);

static void         *g_get_decls        = NULL;   /* GetDeclsOfType, cached at install */
static volatile LONG g_shown            = 0;      /* toggle state (0 = hidden, 1 = shown) -- mirrors OG DAT_18003e818 */

/* SEH-guarded pointer read: *out = *(void**)src. Returns 1 if read, 0 on access violation. */
static int safe_read_ptr(const void *src, void **out)
{
    __try { *out = *(void *const *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* SEH-guarded uint read. */
static int safe_read_u32(const void *src, uint32_t *out)
{
    __try { *out = *(const uint32_t *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* SEH-guarded: is the decl's entityDef className == "idInfoPath"? decl+0x1C8 -> entityDef+0x60 -> strcmp.
 * Any unreadable hop -> 0 (treat as not-idInfoPath). Read only on the re-hide direction. */
static int decl_class_is_infopath(const uint8_t *decl)
{
    __try {
        const uint8_t *entdef = *(const uint8_t *const *)(decl + DECL_ENTITYDEF_OFF);
        if (entdef == NULL) return 0;
        const char *name = *(const char *const *)(entdef + ENTITYDEF_CLASS_OFF);
        if (name == NULL) return 0;
        return strcmp(name, "idInfoPath") == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* Walk every idDeclSnapEditorEntity decl and SET (show!=0) / CLEAR (show==0) the +0x3CD visibility pair,
 * sparing idInfoPath on re-hide -- OG FUN_180021EE0's body. Returns the number of decls touched (out) and
 * the registry count (out); 0 on a resolve/read failure (error set). */
static int target_any_apply(int show, uint32_t *out_count, uint32_t *out_touched, const char **out_err)
{
    *out_count = 0; *out_touched = 0; *out_err = NULL;

    if (g_get_decls == NULL) { *out_err = "GetDeclsOfType not resolved"; return 0; }

    void *list = ((get_decls_fn)g_get_decls)(DECL_TYPE_NAME);
    if (list == NULL) { *out_err = "GetDeclsOfType returned null"; return 0; }

    void    *array = NULL;
    uint32_t count = 0;
    if (!safe_read_ptr((const uint8_t *)list + LIST_ARRAY_OFF, &array) ||
        !safe_read_u32((const uint8_t *)list + LIST_COUNT_OFF, &count)) {
        *out_err = "decl list array/count unreadable";
        return 0;
    }
    *out_count = count;
    if (array == NULL || count == 0) return 1;                 /* empty registry -> clean no-op */
    if (count > COUNT_CAP) { *out_err = "decl count implausible (stale manager node?)"; return 0; }

    uint32_t touched = 0;
    for (uint32_t i = 0; i < count; i++) {
        void *decl_v = NULL;
        if (!safe_read_ptr((const uint8_t *)array + (size_t)i * 8, &decl_v)) break;  /* array tail AV */
        uint8_t *decl = (uint8_t *)decl_v;
        if (decl == NULL) continue;

        uint8_t *fp = decl + DECL_FLAGS_OFF;
        uint8_t  cur;
        __try { cur = *fp; }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }     /* unreadable decl -> skip */

        if (show) {
            __try { *fp = (uint8_t)(cur | VIS_BITS); touched++; }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
        } else {
            if (decl_class_is_infopath(decl)) continue;        /* keep idInfoPath visible */
            __try { *fp = (uint8_t)(cur & (uint8_t)~VIS_BITS); touched++; }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
    }

    *out_touched = touched;
    return 1;
}

void sh_target_any_install(void *get_decls_of_type)
{
    g_get_decls = get_decls_of_type;
}

int sh_target_any_is_shown(void)
{
    return (int)g_shown;
}

void h_target_any(struct idCmdArgs *a)
{
    (void)a;
    if (g_get_decls == NULL) {
        sh_printf("sh_target_any: GetDeclsOfType unresolved -- cannot toggle.\n");
        return;
    }

    LONG shown = InterlockedExchange(&g_shown, !g_shown);
    shown = !shown;                          /* the NEW state after the toggle */

    uint32_t count = 0, touched = 0;
    const char *err = NULL;
    if (target_any_apply((int)shown, &count, &touched, &err)) {
        sh_printf("sh_target_any: %s %u/%u editor entity decls.\n",
                  shown ? "revealed" : "re-hid", touched, count);
        backend_log(shown ? "B2: sh_target_any REVEAL" : "B2: sh_target_any RE-HIDE");
    } else {
        sh_printf("sh_target_any: could not run (%s).\n", err ? err : "unknown");
    }
}
