/* Keep published decls discoverable after the engine switches from source-
 * catalog lookup to resource-manager probes at map-load state 2. A negative
 * native probe for an exact published identity becomes true; the provider
 * then supplies its body.
 *
 * The live vtable slot must equal a clean signature match for the seven-
 * argument probe. Refuse an unknown method rather than forwarding an
 * unverified ABI.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "backend_log.h"
#include "decl_visibility.h"
#include "host_image.h"
#include "engine_globals.h"  /* the manager slot, resolved from the code site that computes it */
#include "overrides.h"
#include "signatures.h"

/* Resolve the manager global through engine_globals using its referencing
 * instruction. Pinned RVAs are audit references only: the constant below is
 * Vulkan; OpenGL uses 0x3E59350.
 */
#define DV_MANAGER_PTR_RVA 0x5557090u
#define DV_PROBE_SLOT      0x78u
/* Probe RVAs for signature repair: the constant below is Vulkan; OpenGL uses
 * 0x17F8A40. Installation uses the prologue match.
 */
#define DV_PINNED_PROBE_RVA 0x1806100u
/* Require the clean unique prologue match to equal the method in the live
 * vtable slot.
 */
#define DV_PROBE_SIGNATURE \
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 78 FF FF FF " \
    "48 81 EC 88 01 00 00 48 C7 44 24 30 FE FF FF FF"
#define DV_PATH_PREFIX     "generated/decls/"
#define DV_KEY_CAP         512

/* GetCacheFileInfo takes seven arguments: self, path, four output pointers
 * and quiet. It clears the outputs on entry. Nonzero quiet makes a miss
 * return false; zero raises an engine error. Preserve all seven arguments,
 * including quiet on the stack.
 */
typedef unsigned char (*dv_probe_fn)(void *self, const char *path, void *out1,
                                     void *out2, void *out3, void *out4,
                                     unsigned char quiet);

static dv_probe_fn g_orig_probe;
static void **g_slot;

static int dv_safe_read(const void *source, void *destination, size_t length)
{
    __try {
        memcpy(destination, source, length);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int dv_probe_key(const char *path, char *key, size_t key_size)
{
    size_t prefix = sizeof(DV_PATH_PREFIX) - 1;

    if (!path || !key || key_size == 0) return 0;
    if (_strnicmp(path, DV_PATH_PREFIX, prefix) != 0) return 0;
    if (path[prefix] == '\0') return 0;
    if (_snprintf_s(key, key_size, _TRUNCATE, "%s%s",
                    SH_OVERRIDES_INTERNAL_DECL_PREFIX, path + prefix) < 0)
        return 0;
    return 1;
}

/* Guard reads of the engine-owned path during map loading. */
static int dv_path_is_published(const char *path)
{
    char key[DV_KEY_CAP];
    int mapped = 0;

    __try {
        mapped = dv_probe_key(path, key, sizeof(key));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!mapped) return 0;
    return sh_overrides_internal_decl_published(key);
}

static unsigned char dv_probe_hook(void *self, const char *path, void *out1,
                                   void *out2, void *out3, void *out4,
                                   unsigned char quiet)
{
    unsigned char original = 0;
    dv_probe_fn orig = g_orig_probe;

    if (!orig) return 0;
    __try {
        original = orig(self, path, out1, out2, out3, out4, quiet);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    /* Keep native successes and all output values. Override only the boolean
     * miss for an exact published identity; the decl lookup then loads
     * through the provider.
     */
    if (original) return original;
    return dv_path_is_published(path) ? (unsigned char)1 : (unsigned char)0;
}

/* Require a clean SIG_OK match at the slot address. A hooked known-RVA
 * fallback does not establish the method ABI.
 */
static int dv_method_is_probe(const uint8_t *module_base, const void *probe)
{
    sig_entry entry;
    sig_result found;

    entry.name = "DeclResourceExistenceProbe";
    entry.pattern = DV_PROBE_SIGNATURE;
    entry.known_rva = DV_PINNED_PROBE_RVA;
    memset(&found, 0, sizeof(found));
    if (sig_resolve_one(module_base, &entry, &found) != SIG_OK) return 0;
    return found.addr != 0 && found.addr == (uintptr_t)probe;
}

static int dv_resolve(const uint8_t *module_base, void **out_manager,
                      void ***out_slot, dv_probe_fn *out_probe)
{
    void *manager = NULL;
    void *vtable = NULL;
    void *probe = NULL;
    void **slot;

    if (!module_base) return 0;
    /* Resolve the manager global from its code reference; the pinned RVA is
     * diagnostic.
     */
    {
        uintptr_t slot_addr = glb_resolve(module_base, "decl_visibility_manager", NULL);
        if (!slot_addr) return 0;
        if (!dv_safe_read((const uint8_t *)slot_addr, &manager, sizeof(manager)) || !manager)
            return 0;
    }
    if (!dv_safe_read(manager, &vtable, sizeof(vtable)) || !vtable) return 0;
    slot = (void **)((uint8_t *)vtable + DV_PROBE_SLOT);
    if (!dv_safe_read(slot, &probe, sizeof(probe)) || !probe) return 0;
    *out_manager = manager;
    *out_slot = slot;
    *out_probe = (dv_probe_fn)probe;
    return 1;
}

int sh_decl_visibility_install(const uint8_t *module_base,
                               const char *existing_probe_path,
                               const char *absent_probe_path)
{
    void *manager = NULL;
    void **slot = NULL;
    dv_probe_fn probe = NULL;
    unsigned long long method_rva;
    char line[512];
    DWORD old;

    (void)existing_probe_path;
    (void)absent_probe_path;

    if (g_orig_probe) {
        backend_log("decl-visibility already installed");
        return 1;
    }
    if (!dv_resolve(module_base, &manager, &slot, &probe)) {
        backend_log("decl-visibility REFUSED: decl-resource manager, its vtable, or its +0x78 method was unreadable");
        return 0;
    }
    method_rva = (unsigned long long)((const uint8_t *)probe - module_base);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "decl-visibility resolved: manager=%p vtable_rva=0x%llx slot+0x%x method_rva=0x%llx",
                manager,
                (unsigned long long)((const uint8_t *)*(void **)manager - module_base),
                (unsigned)DV_PROBE_SLOT, method_rva);
    backend_log(line);

    /* Refuse an unknown method because its argument shape may differ. */
    if (!dv_method_is_probe(module_base, probe)) {
        backend_log("decl-visibility REFUSED: the +0x78 method's prologue is not the decl-resource existence probe's");
        return 0;
    }

    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        backend_log("decl-visibility REFUSED: VirtualProtect on the method slot failed");
        return 0;
    }
    g_orig_probe = probe;
    *slot = (void *)dv_probe_hook;
    VirtualProtect(slot, sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    g_slot = slot;

    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "decl-visibility installed: published identities now answer the decl-resource existence probe (slot=%p, orig=%p)",
                (void *)slot, (void *)probe);
    backend_log(line);
    return 1;
}

int sh_decl_visibility_uninstall(void)
{
    DWORD old;
    if (!g_slot || !g_orig_probe) return 0;
    if (VirtualProtect(g_slot, sizeof(void *), PAGE_READWRITE, &old)) {
        *g_slot = (void *)g_orig_probe;
        VirtualProtect(g_slot, sizeof(void *), old, &old);
        FlushInstructionCache(GetCurrentProcess(), g_slot, sizeof(void *));
    }
    g_slot = NULL;
    g_orig_probe = NULL;
    backend_log("decl-visibility uninstalled");
    return 1;
}

#ifdef SH_DECL_VISIBILITY_TESTING
int sh_decl_visibility_test_probe_key(const char *path, char *key, size_t key_size)
{
    return dv_probe_key(path, key, key_size);
}
#endif
