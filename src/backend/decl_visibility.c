/* decl_visibility.c -- see decl_visibility.h.
 *
 * WHY THIS EXISTS
 *
 * `DeclFind` (RVA 0x17B36F0) resolves an identity that has no live object by
 * branching on the map-load lifecycle state at RVA 0x6DDE198. Below state 2 it
 * consults the source catalog that `DeclRegisterFile` populates. At state 2 and
 * above -- which covers every gameplay map load -- it instead builds the path
 * "generated/decls/<type>/<name>.decl" (RVA 0x17AB4E0) and asks the
 * decl-resource manager behind the global at RVA 0x5557090 through vtable slot
 * +0x78 whether that resource exists. If the answer is no and the caller passed
 * makeDefault=0, the lookup returns null.
 *
 * Nothing this product writes reaches that manager. The engine's own entityDef
 * inheritance helper (RVA 0x17AEC10) looks its parent up with makeDefault=0 and
 * logs "Unknown entityDef '%s' inherited by '%s'" on null, which is exactly what
 * a gameplay map load reported for identities that had registered and
 * materialized cleanly in the editor.
 *
 * So this service answers that one probe, for exactly the identities the
 * dynamic decl server published, and only when the engine's own answer was no.
 * Everything after the probe is already provided: the engine creates the decl
 * and loads it through the file-system open-by-name slot the overrides layer
 * hooks, which serves the published bytes.
 *
 * The method is reached through a runtime-resolved vtable, so its address comes
 * from the live object rather than from a scan. The service still refuses to
 * touch a slot it cannot identify -- a different method would have a different
 * argument shape, and forwarding the wrong one would corrupt the engine's stack
 * -- but it identifies the method by scanning for ITS OWN PROLOGUE and
 * requiring that unique match to be the address the slot holds. That is a
 * stronger proof than the RVA equality this used to demand, and unlike an RVA
 * it survives the OpenGL executable, which is the same source tree re-linked
 * with every function moved. An unrecognised slot is left alone and only logged.
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
#include "overrides.h"
#include "signatures.h"

/* Pointer-to-manager global; the manager's own vtable is read from it. This is a raw DATA rva, not
 * a function, so no signature covers it and it is only correct on the build it was read from. It is
 * gated on sh_host_is_pinned_rva_build() below and fails closed everywhere else, pending the
 * runtime resolver for that global. Never guess a replacement address for it. */
#define DV_MANAGER_PTR_RVA 0x5557090u
#define DV_PROBE_SLOT      0x78u
/* The RVA of the probe on the pinned Vulkan build (0x17F8A40 on the OpenGL build), recorded for
 * audit and re-derivation. NOT used to gate; the prologue signature below is what identifies the
 * method, and it matches exactly once on both shipped images. */
#define DV_PINNED_PROBE_RVA 0x1806100u
/* The probe's own prologue. The vtable is built at runtime, so the slot's contents cannot be found
 * by scanning -- but once read, they can be CHECKED by scanning: this pattern resolves uniquely on
 * both shipped builds, so requiring the unique match to equal the slot's method proves the slot
 * holds the method this code models, on any link of this source tree. */
#define DV_PROBE_SIGNATURE \
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 78 FF FF FF " \
    "48 81 EC 88 01 00 00 48 C7 44 24 30 FE FF FF FF"
#define DV_PATH_PREFIX     "generated/decls/"
#define DV_KEY_CAP         512

/* The pinned method is GetCacheFileInfo and takes SEVEN arguments:
 *
 *   bool GetCacheFileInfo(self, const char *path, void *out1, void *out2,
 *                         void *out3, unsigned char *out4, unsigned char quiet)
 *
 * It clears all four output slots on entry, and `quiet` (arg 7, read at
 * [rbp+0x100]) decides what a miss costs: non-zero returns false silently, zero
 * raises a fatal engine error naming the path. The engine passes quiet=1 for
 * ordinary probe lookups, so the hook MUST forward all seven arguments. An
 * earlier six-argument typedef left arg 7 as stack garbage and turned routine,
 * silent cache misses into fatal errors that aborted a fully loaded map back to
 * the SnapMap browser. */
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

/* The engine owns `path`, and this runs on its thread during a map load, so a
 * malformed or freed string must not take the process down with it. */
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
    /* The engine's own answer always wins, and its output arguments are left
     * exactly as it wrote them. Only a "does not exist" verdict for an identity
     * this process published is corrected, and only as a boolean: the decl
     * lookup that needs this answer ignores the output slots and falls through
     * to the file system, which the overrides layer already serves. */
    if (original) return original;
    return dv_path_is_published(path) ? (unsigned char)1 : (unsigned char)0;
}

/* Is `probe` the decl-resource existence probe? Answered by scanning the host image for the
 * probe's own prologue and requiring the unique match to be exactly this address. A signature
 * that matches once cannot have matched a different function, so this is a stronger identity
 * proof than the address comparison it replaces -- and it is portable, which the address is not.
 * SIG_OK only: SIG_OK_HOOKED would mean the scan missed and the known-RVA fallback stepped over
 * somebody else's detour, which tells us nothing about what this slot holds. */
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
    if (!dv_safe_read(module_base + DV_MANAGER_PTR_RVA, &manager,
                      sizeof(manager)) || !manager) return 0;
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
    /* The manager is still reached through a raw data RVA, which is a fact about one link output
     * and nothing else. Reading it on any other build would hand us an unrelated pointer that
     * looks exactly as valid, so this fails closed until that global has a runtime resolver. */
    if (!sh_host_is_pinned_rva_build()) {
        backend_log("decl-visibility REFUSED: the decl-resource manager is still located by a "
                    "pinned data RVA, which is only valid on the extraction build");
        return 0;
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

    /* An unidentified method has an unknown argument shape, and forwarding the
     * wrong one would corrupt the engine's stack. Refuse instead. */
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
