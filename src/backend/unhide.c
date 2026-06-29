/* unhide.c -- see unhide.h. Native C port of OG FUN_180021EE0 (the `sh_target_any` unhide).
 *
 * MECHANISM (DIRECT, the live JS reimpl the reference implementation doUnhide):
 *   list  = GetDeclsOfType("idDeclSnapEditorEntity")   // engine fn; returns the typed decl-manager node
 *   array = *(list + 0x20)                              // decl-pointer array  (LIST_ARRAY_OFF)
 *   count = *(uint*)(list + 0x28)                       // decl count          (LIST_COUNT_OFF)
 *   for each non-null decl in array[0..count):
 *     UNHIDE : *(byte*)(decl + 0x3CD) |= 0xC0           // set bits 7-6  -> editor-visible/placeable
 *     RE-HIDE: if class(decl) != "idInfoPath": *(byte*)(decl + 0x3CD) &= 0x3F   // clear bits 7-6
 *
 * OFFSETS -- live-build provenance:
 *   +0x20 / +0x28 on the manager node  = decl-ptr array + count. LIVE-VERIFIED: the engine's own indexed
 *       getter idResourceList::Get (FUN_141801300) reads `*(mgr+0x20) + idx*8` after bounds-checking
 *       `mgr+0x28` ("Resource index exceeds current count"). So +0x20 is the LIVE-OWNED registry array.
 *   +0x3CD on the decl = the editor flags byte. LIVE-VERIFIED via the palette validator FUN_1404F8180:
 *       it reads `decl+0x3CD & 0x20` (isOutput) / `& 0x10` (isInput); the unhide pair is the high bits
 *       0x80/0x40 (= 0xC0), distinct from the output/input bits. (truth: snapmap-editor-palette-build.md)
 *   +0x1C8 on the decl = the entityDef pointer. LIVE-VERIFIED via FUN_1404F8180 (`if(*(decl+0x1C8)==0)
 *       reject ... null entityDef`).
 *   +0x60 on the entityDef = the class-name idStr ptr (used ONLY in the re-hide idInfoPath spare).
 *       This is the SnapHak-2021 layout; it is NOT independently re-confirmed on the live DOOM build
 *       (the build-mismatch lesson, memory reference-entity-layout-offsets-build-specific). It is read
 *       SEH-guarded + only after the entityDef-non-null gate, so a wrong offset degrades to "idInfoPath
 *       not spared" at worst, never a crash. The UNHIDE direction does NOT read it.
 *
 * NO FREE: OG's literal port frees `array` after the walk; but on THIS build +0x20 is the live-owned
 * registry array (proven by FUN_141801300 above), so freeing it would corrupt the decl registry. We
 * therefore intentionally do NOT free -- this is a deliberate, build-verified divergence from a literal
 * OG port, matching the JS reimpl (which also never frees). See the report's RISKS.
 *
 * THREAD/TIMING: GetDeclsOfType is an engine call -> must run on the DOOM main thread context in OG (it
 * is invoked from the `sh_target_any` console command = a main-thread Cbuf callback). Our bootstrap runs
 * on a worker thread; the flag writes + the decl-array read are benign there, but to mirror OG's "apply
 * when the entity list is ready" timing we POLL until the registry is populated before the single pass.
 * (A later pass will move this onto a frame/command hook for the on-demand toggle, per the
 * instrumentation-drive convention -- the reference implementation runOnMainThread.)
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "unhide.h"
#include "backend_log.h"

#define DECL_TYPE_NAME     "idDeclSnapEditorEntity"

#define LIST_ARRAY_OFF     0x20    /* manager node -> decl-pointer array */
#define LIST_COUNT_OFF     0x28    /* manager node -> decl count (uint) */
#define DECL_FLAGS_OFF     0x3CD   /* decl -> editor flags byte; bits 7-6 (0xC0) = visible/placeable */
#define DECL_ENTITYDEF_OFF 0x1C8   /* decl -> entityDef pointer */
#define ENTITYDEF_CLASS_OFF 0x60   /* entityDef -> class-name idStr ptr (2021 layout; re-hide only) */

#define VIS_BITS           0xC0    /* the editor-visibility pair the unhide toggles */

typedef void *(*get_decls_fn)(const char *type_name);

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

/* SEH-guarded: is the decl's entityDef class-name == "idInfoPath"? Mirrors the reference implementation's try/guarded read
 * `decl+0x1C8 -> +0x60 -> strcmp "idInfoPath"`. Any unreadable hop -> 0 (treat as not-idInfoPath). */
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

sh_unhide_result sh_unhide_apply(void *get_decls_of_type, int show)
{
    sh_unhide_result r;
    r.ok = 0; r.show = show ? 1 : 0; r.count = 0; r.touched = 0; r.spared = 0; r.error = NULL;

    if (get_decls_of_type == NULL) { r.error = "GetDeclsOfType not resolved"; return r; }

    get_decls_fn get_decls = (get_decls_fn)get_decls_of_type;
    void *list = get_decls(DECL_TYPE_NAME);
    if (list == NULL) { r.error = "GetDeclsOfType returned null"; return r; }

    void    *array = NULL;
    uint32_t count = 0;
    if (!safe_read_ptr((const uint8_t *)list + LIST_ARRAY_OFF, &array) ||
        !safe_read_u32((const uint8_t *)list + LIST_COUNT_OFF, &count)) {
        r.error = "decl list array/count unreadable";
        return r;
    }
    r.count = count;
    if (array == NULL || count == 0) { r.ok = 1; return r; }   /* empty registry -> clean no-op */

    /* Sanity cap: the snapEditorEntityDef registry is ~1361 decls (reference/.../snapeditorentitydef/);
     * a count wildly larger than that means we read a stale/garbage manager node -> bail rather than
     * stride a bogus array. 1<<20 is generous headroom and well under any address-space confusion. */
    if (count > (1u << 20)) { r.error = "decl count implausible (stale manager node?)"; return r; }

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
            __try { *fp = (uint8_t)(cur | VIS_BITS); r.touched++; }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
        } else {
            if (decl_class_is_infopath(decl)) { r.spared++; continue; }   /* keep idInfoPath visible */
            __try { *fp = (uint8_t)(cur & (uint8_t)~VIS_BITS); r.touched++; }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
    }

    r.ok = 1;
    return r;
}

/* Poll knobs: the snapEditorEntityDef registry fills as the SnapMap editor resources mount, which is
 * well after our DLL load. Poll a generous budget; a 0-count list before the editor is up is normal. */
#define B1_UNHIDE_POLL_INTERVAL_MS 250
#define B1_UNHIDE_POLL_TIMEOUT_MS  120000

int sh_unhide_apply_when_ready(void *get_decls_of_type)
{
    char line[160];

    if (get_decls_of_type == NULL) {
        backend_log("B1: unhide SKIPPED -- GetDeclsOfType not resolved");
        return 0;
    }

    get_decls_fn get_decls = (get_decls_fn)get_decls_of_type;
    DWORD t0 = GetTickCount();
    for (;;) {
        void *list = get_decls(DECL_TYPE_NAME);
        uint32_t count = 0;
        if (list != NULL && safe_read_u32((const uint8_t *)list + LIST_COUNT_OFF, &count) && count > 0)
            break;                                              /* registry populated -> apply */
        if (GetTickCount() - t0 >= B1_UNHIDE_POLL_TIMEOUT_MS) {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "B1: unhide SKIPPED -- decl registry empty after %lums (editor not up?)",
                GetTickCount() - t0);
            backend_log(line);
            return 0;
        }
        Sleep(B1_UNHIDE_POLL_INTERVAL_MS);
    }

    sh_unhide_result r = sh_unhide_apply(get_decls_of_type, /*show=*/1);
    if (!r.ok) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "B1: unhide FAIL -- %s",
                    r.error ? r.error : "unknown");
        backend_log(line);
        return 0;
    }
    /* "N/174" -- N = decls made visible this pass; 174 is the documented unhide-table class count
     * (xinput-unhide-table.md) used as the human-readable denominator. touched can exceed 174 because
     * the toggle flips EVERY snapEditorEntityDef decl (~1361), not just the 174 anchored ones -- so we
     * report both: touched out of the count walked, with the /174 marker the brief asked for. */
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B1: unhide applied %u/174 (touched %u of %u snapEditorEntityDef decls)",
        r.touched, r.touched, r.count);
    backend_log(line);
    return 1;
}
