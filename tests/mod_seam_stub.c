/* mod_seam_stub.c -- link stand-ins for the serving layer (modserve.c and
 * modserve_engine.c) and the bundle builder entry (modcmd.c), for tests that
 * link the map manifest or the legacy converter without them.
 *
 * The serving index reports no bundle as indexed, so every conversion asks
 * for a rescan; g_mod_stub_rescans counts the requests. Define
 * SH_MOD_STUB_NO_BUILDER when the test supplies its own
 * modbundle_build_folder. The rawmap ZIP export (modexport.c) is not linked:
 * a .zip path is recognised and refused. */
#include <stddef.h>
#include <string.h>

#include "../src/backend/modserve.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modbundle.h"
#include "../src/backend/modexport.h"

int g_mod_stub_rescans;
int g_mod_stub_activations;

/* The registry choices' input (modmanifest.c gives them at every load). */
int sh_modserve_scope_set_choices(const char *const *slots, const char *const *keys, int n, int recompute)
{ (void)slots; (void)keys; (void)n; (void)recompute; return 0; }

int modserve_activate_bundles(const char *const *bundle_hashes, int n)
{
    (void)bundle_hashes;
    if (n > 0) g_mod_stub_activations++;
    return 0;
}

size_t modserve_bundles_for_names_visit(const char *const *names, size_t count,
                                        sh_modserve_closure_fn visit, void *context)
{
    (void)names; (void)count; (void)visit; (void)context;
    return 0;
}

int sh_modserve_bundle_state(const char *bundle_hash, char **cause)
{
    (void)bundle_hash;
    if (cause) *cause = NULL;
    return 0;
}

int sh_modserve_active(void) { return 0; }

/* The build that serves a row (sh_modserve_row_build): none installed. */
int sh_modserve_row_build(const sh_modmanifest_row *row, sh_modserve_bundle_ref *out)
{
    (void)row;
    if (out) memset(out, 0, sizeof *out);
    return 0;
}

void sh_modserve_bundle_ref_free(sh_modserve_bundle_ref *ref)
{
    if (!ref) return;
    free(ref->name);
    free(ref->folder);
    free(ref->place);
    ref->name = ref->folder = ref->place = NULL;
}

/* The loaded map's place (sh_modserve_place_*): a parse sets it, a failed
 * one puts it back. */
static char *g_stub_place, *g_stub_place_prev;
int sh_modserve_place_load(const char *place)
{
    free(g_stub_place_prev);
    g_stub_place_prev = g_stub_place;
    g_stub_place = _strdup(place ? place : "");
    return 0;
}
int sh_modserve_place_restore(void)
{
    free(g_stub_place);
    g_stub_place = g_stub_place_prev;
    g_stub_place_prev = NULL;
    return 1;
}
char *sh_modserve_place_get(void) { return _strdup(g_stub_place ? g_stub_place : ""); }

int sh_modserve_index_rescan(void)
{
    g_mod_stub_rescans++;
    return 0;
}

int modexport_is_zip_path(const char *path)
{
    size_t n = path ? strlen(path) : 0;
    return n > 4 && _stricmp(path + n - 4, ".zip") == 0;
}

/* The rawmap layer hands the export every path in UTF-8: the stand-ins
 * decode it strictly and look for it through the wide calls, so a path that
 * arrived in another encoding is refused here as it would be on disk. `dir`
 * checks the folder the path is in. */
static int stub_utf8_exists(const char *path, int dir)
{
    wchar_t *w, *slash;
    int n, found;
    if (!path || (n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0)) <= 0) return 0;
    if ((w = (wchar_t *)malloc(((size_t)n + 4) * sizeof *w)) == NULL) return 0;
    /* A drive path in its long form, so a path of any depth is found. */
    wcscpy_s(w, (size_t)n + 4, path[0] && path[1] == ':' ? L"\\\\?\\" : L"");
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w + wcslen(w), n);
    if (dir && (slash = wcsrchr(w, L'\\')) != NULL) *slash = 0;
    found = GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
    free(w);
    return found;
}

unsigned long long modexport_write_text(const char *data, size_t len, const char *path)
{
    (void)data; (void)len; (void)path;
    return 0;
}

/* The rawmap layer's companion export: g_mod_stub_export_result is what the
 * write returns (0 refuses with a fixed cause); the calls and the last path
 * are recorded. */
unsigned long long g_mod_stub_export_result;
int g_mod_stub_export_calls;
char *g_mod_stub_export_path;   /* malloc'd */

unsigned long long modexport_write_text_report(const char *data, size_t len, const char *path,
                                               unsigned *bundles, char **cause)
{
    (void)data; (void)len;
    g_mod_stub_export_calls++;
    free(g_mod_stub_export_path);
    g_mod_stub_export_path = _strdup(path ? path : "");
    if (bundles) *bundles = 0;
    if (!stub_utf8_exists(path, 1)) {
        if (cause) *cause = _strdup("the path did not arrive in UTF-8");
        return 0;
    }
    if (g_mod_stub_export_result) return g_mod_stub_export_result;
    if (cause) *cause = _strdup("the rawmap export is not linked into this test");
    return 0;
}

/* The importer's refusal: g_mod_stub_import_cause is what it says (the
 * export is not linked, so every import is refused). */
char g_mod_stub_import_cause[1024] = "the rawmap export is not linked into this test";

/* g_mod_stub_import_text, when set, is the map every import returns; the
 * imports are counted either way. */
const char *g_mod_stub_import_text;
int g_mod_stub_import_calls;

char *modexport_read_text_report(const char *path, size_t *out_len, char **cause)
{
    g_mod_stub_import_calls++;
    if (out_len) *out_len = 0;
    if (g_mod_stub_import_text && !stub_utf8_exists(path, 0)) {
        if (cause) *cause = _strdup("the path did not arrive in UTF-8");
        return NULL;
    }
    if (g_mod_stub_import_text) {
        size_t n = strlen(g_mod_stub_import_text);
        char *heap = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);
        if (heap) { memcpy(heap, g_mod_stub_import_text, n + 1); if (out_len) *out_len = n; }
        return heap;
    }
    if (cause) *cause = _strdup(g_mod_stub_import_cause);
    return NULL;
}

int modexport_validate_source(const char *path, char **out_msg)
{
    (void)path;
    if (out_msg) *out_msg = _strdup("the rawmap export is not linked into this test");
    return 0;
}

#ifndef SH_MOD_STUB_NO_BUILDER
int modbundle_build_folder(const char *folder, char **first_cause)
{
    (void)folder;
    if (first_cause) *first_cause = _strdup("the bundle builder is not linked into this test");
    return 0;
}
#endif
