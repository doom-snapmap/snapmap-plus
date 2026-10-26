/* Resolve the shield's engine functions with the shared backend signature
 * scanner. Cache them before recovery handlers run. Data and call-site-derived
 * addresses use engine_globals; see engine_layout.h for layout constants. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "engine_layout.h"
#include "fault_record.h"
#include "shield_sigs.h"
#include "../backend/signatures.h"/* shared signature resolver */
#include "../backend/host_image.h"/* exact-executable gate for literal RVA fallback */
#include "../backend/editor_frame.h"/* the backend's pre-hook EditorFrame entry */

shield_engine g_eng = { 0 };

/* NULL-terminated function signatures with their build 13954591 Vulkan/OpenGL RVAs.
 * The RVA fallback applies only on that exact executable (host_image.h). */
static const sig_entry SHIELD_ENGINE_SIGNATURES[] = {
    /* Recoverable Error(6) wrapper, not the shared dispatcher. The level immediate
     * distinguishes it from FatalError7. Class-B recovery supplies fmt in RCX. */
    { "Error6",
      "48 89 4C 24 08 48 89 54 24 10 4C 89 44 24 18 4C 89 4C 24 20 48 83 EC 28 "
      "E8 ?? ?? ?? ?? 48 8B 54 24 30 4C 8D 44 24 38 B9 06 00 00 00",
      0x282B50u, 0x282D40u },
    /* FatalError7 differs from Error6 at MOV ECX,7. recovery.c patches that
     * immediate to request the recoverable exception class. */
    { "FatalError7",
      "48 89 4C 24 08 48 89 54 24 10 4C 89 44 24 18 4C 89 4C 24 20 48 83 EC 28 "
      "E8 ?? ?? ?? ?? 48 8B 54 24 30 4C 8D 44 24 38 B9 07 00 00 00",
      0x282B90u, 0x282D80u },
    /* Synchronous editor state transition; state 0xB opens StartMenu. */
    { "SetState",
      "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 8B 91 18 36 02 00",
      0x1189580u, 0x11886E0u },
    /* Frame hook target. These 15 position-independent prologue bytes are exactly
     * the window stolen by recovery.c. */
    { "Frame",
      "40 57 41 54 41 55 41 56 41 57 B8 C0 ?? 01 00 E8 ?? ?? ?? ?? 48 2B E0",
      0x159D930u, 0x159E9A0u },
    /* Editor Think entry; start of the Class-A unwind target range. */
    { "EditorPump",
      "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC 90 00 00 00 48 C7 45 C7 FE FF FF FF 48 89 58 18",
      0x1182E80u, 0x1181FE0u },
    /* Connection resolver entry; start of the CSR repair range. */
    { "Resolver",
      "44 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 "
      "41 54 41 55 41 56 41 57 48 8D 6C 24 E9 48 81 EC 88 00 00 00",
      0x123C660u, 0x123BB00u },
    /* Shared patterns also used by backend/signatures.c. */
    { "Toast",      /* editor toast-show FUN_140cfa0b0 */
      "40 57 48 83 EC 20 48 8B F9 48 8B 89 F0 08 00 00",
      0x1272760u, 0x1271D10u },
    { "IdStrCtor",  /* idStr from a C-string */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B DA 48 89 01 48 8B F9",
      0x33A0F0u, 0x339D40u },
    { "IdStrDtor",  /* idStr dtor */
      "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B 51 10",
      0x33A320u, 0x339F70u },
    { NULL, NULL, 0 }
};

/* Publish a unique scan result, or a logged fallback on the exact pinned build.
 * Other builds stay unresolved on a miss. Return 1 only for SIG_OK; fallback,
 * absence, and guarded faults return 0. */
static int resolve_fn(const uint8_t *module_base, const char *name,
                      uintptr_t *out_addr, uint32_t *out_rva)
{
    __try {
        const sig_entry *e = SHIELD_ENGINE_SIGNATURES;
        for (; e->name; e++)
            if (strcmp(e->name, name) == 0) break;
        if (!e->name) { if (out_addr) *out_addr = 0; return 0; }

        sig_result r;
        sig_status st = sig_resolve_one(module_base, e, &r);
        if (st == SIG_OK) {
            if (out_addr) *out_addr = r.addr;
            if (out_rva)  *out_rva  = r.rva;
            return 1;   /* portable scan hit */
        }
        /* Literal RVAs identify only one link output. Other builds must leave the
         * address zero so dependent recovery paths can decline safely. */
        {
            uint32_t known_rva = sh_host_image_rva(module_base, e->rva_vk, e->rva_gl);
            int pinned = known_rva != 0;
            char msg[200];
            if (pinned) {
                if (out_addr) *out_addr = (uintptr_t)(module_base + known_rva);
                if (out_rva)  *out_rva  = known_rva;
                _snprintf_s(msg, sizeof msg, _TRUNCATE,
                    "sig miss for %s (status=%d) -> fell back to recorded RVA 0x%x (re-derive on patch)",
                    name, (int)st, (unsigned)known_rva);
            } else {
                if (out_addr) *out_addr = 0;
                if (out_rva)  *out_rva  = 0;
                _snprintf_s(msg, sizeof msg, _TRUNCATE,
                    "sig miss for %s (status=%d) on an executable the recorded RVA does not describe -> "
                    "unresolved; dependent shield features will decline",
                    name, (int)st);
            }
            {
                shield_fault f = { "sig", (int)st, msg, known_rva, 0 };
                shield_emit(&f);
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (out_addr) *out_addr = 0;
        return 0;
    }
}

/* EditorPump and the backend's EditorFrame are the same function, and the backend hooks it,
 * which overwrites the prologue this scan matches on. It resolved the entry before patching,
 * so take that address when the scan comes back empty. Bounded to the host image: a value
 * outside it would put the Class-A unwind range somewhere that is not engine code. */
static int adopt_backend_editor_frame(const uint8_t *module_base)
{
    const uint8_t *entry = (const uint8_t *)sh_editor_frame_target();
    char msg[160];

    if (entry == NULL || module_base == NULL) return 0;
    if (entry < module_base || (size_t)(entry - module_base) > 0xFFFFFFFFu) return 0;

    g_eng.editor_pump     = (uintptr_t)entry;
    g_eng.editor_pump_rva = (uint32_t)(entry - module_base);
    _snprintf_s(msg, sizeof msg, _TRUNCATE,
                "EditorPump adopted from the installed editor-frame hook at 0x%x "
                "(its prologue is a detour, so the scan cannot see it)",
                (unsigned)g_eng.editor_pump_rva);
    {
        shield_fault f = { "sig", 0, msg, g_eng.editor_pump_rva, 0 };
        shield_emit(&f);
    }
    return 1;
}

int shield_resolve_engine(const uint8_t *module_base)
{
    int scanned = 0;
    scanned += resolve_fn(module_base, "Error6",      &g_eng.error6,       NULL);
    scanned += resolve_fn(module_base, "FatalError7", &g_eng.fatalerror7,  NULL);
    scanned += resolve_fn(module_base, "SetState",    &g_eng.setstate,     NULL);
    scanned += resolve_fn(module_base, "Frame",      &g_eng.frame,       NULL);
    if(adopt_backend_editor_frame(module_base))++scanned;
    else scanned += resolve_fn(module_base, "EditorPump", &g_eng.editor_pump, &g_eng.editor_pump_rva);
    scanned += resolve_fn(module_base, "Resolver",   &g_eng.resolver,    &g_eng.resolver_rva);
    scanned += resolve_fn(module_base, "Toast",      &g_eng.toast_show,  NULL);
    scanned += resolve_fn(module_base, "IdStrCtor",  &g_eng.idstr_ctor,  NULL);
    scanned += resolve_fn(module_base, "IdStrDtor",  &g_eng.idstr_dtor,  NULL);
    return scanned;
}
