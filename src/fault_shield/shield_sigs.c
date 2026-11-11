/* The shield's engine functions, taken from the bootstrap's signature results
 * (resolved before any detour was committed). Data and call-site-derived
 * addresses use engine_globals; see engine_layout.h for layout constants. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "fault_record.h"
#include "shield_sigs.h"

shield_engine g_eng = { 0 };

static int take(const sig_result *results, size_t count, const char *name,
                uintptr_t *out_addr, uint32_t *out_rva)
{
    const sig_result *r = sig_lookup(results, count, name);
    if (r && r->status == SIG_OK) {
        *out_addr = r->addr;
        if (out_rva) *out_rva = r->rva;
        return 1;
    }
    {
        char msg[200];
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "%s: %s; dependent shield features are off", name,
                    sig_status_reason(r ? r->status : SIG_NOT_FOUND));
        shield_fault f = { "sig", r ? (int)r->status : -1, msg, 0, 0 };
        shield_emit(&f);
    }
    *out_addr = 0;
    if (out_rva) *out_rva = 0;
    return 0;
}

int shield_resolve_engine(const sig_result *results, size_t count)
{
    int resolved = 0;
    resolved += take(results, count, "Error6",             &g_eng.error6,      NULL);
    resolved += take(results, count, "FatalError7",        &g_eng.fatalerror7, NULL);
    resolved += take(results, count, "GridSetEditorState", &g_eng.setstate,    NULL);
    resolved += take(results, count, "Frame",              &g_eng.frame,       NULL);
    resolved += take(results, count, "EditorFrame",        &g_eng.editor_pump, &g_eng.editor_pump_rva);
    resolved += take(results, count, "Resolver",           &g_eng.resolver,    &g_eng.resolver_rva);
    resolved += take(results, count, "ClearDialogWrapper", &g_eng.clear_dialog, NULL);
    return resolved;
}
