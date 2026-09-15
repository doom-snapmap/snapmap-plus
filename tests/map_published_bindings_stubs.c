/* Image binding must never enter runtime map preparation or install hooks. */
#include <assert.h>
#include "rawmap.h"
#include "hook.h"
#include "map_render.h"
#include "resource_catalog.h"
const sh_resource_catalog *sh_package_runtime_catalog(void) { assert(0); return NULL; }
int sh_decl_server_map_boundary_safe(void) { assert(0); return 0; }
int sh_decl_server_map_preparation_ready(void) { assert(0); return 0; }
int sh_decl_server_map_browser_present(void) { assert(0); return 0; }
int sh_map_render_capture(void *snapshot, sh_map_render *settings)
{ (void)snapshot; (void)settings; assert(0); return 0; }
void sh_map_render_select(const sh_map_render *settings) { (void)settings; assert(0); }
void sh_mpkg_report_error(const char *error) { (void)error; assert(0); }
void sh_rawmap_inspection_enter(void) { assert(0); }
void sh_rawmap_inspection_leave(void) { assert(0); }
void sh_rawmap_read_enter(sh_rawmap_read_scope *scope, const void *caller)
{ (void)scope; (void)caller; assert(0); }
void sh_rawmap_read_leave(sh_rawmap_read_scope *scope) { (void)scope; assert(0); }
int sh_rawmap_cancel_pending_map(void) { assert(0); return 0; }
int sh_rawmap_preflight_request(const char *json, size_t length, const sh_rawmap_request *request, char *error, size_t capacity)
{ (void)json; (void)length; (void)request; (void)error; (void)capacity; assert(0); return 0; }
int sh_rawmap_preflight_loading_request(const char *json, size_t length, const sh_rawmap_request *request,
    const sh_rawmap_loading *loading, char *error, size_t capacity)
{ (void)json; (void)length; (void)request; (void)loading; (void)error; (void)capacity; assert(0); return 0; }
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; assert(0); return NULL; }
void *hook_prepare_relative_call(void *target, void *detour, size_t stolen, size_t offset)
{ (void)target; (void)detour; (void)stolen; (void)offset; assert(0); return NULL; }
sh_patch_status hook_commit(void *trampoline) { (void)trampoline; assert(0); return B2_PATCH_REFUSED_BADARG; }
int hook_unpatch(void *trampoline) { (void)trampoline; assert(0); return 0; }
