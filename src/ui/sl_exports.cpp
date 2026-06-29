/* sl_exports.cpp -- the 9 `sl_*` SuperScript-Lua C exports (thin stubs).
 *
 * FAITHFUL surface: OG snaphakui.dll exports 9 `sl_*` functions (the "SuperScript Lua" bindings) that are
 * plain C helpers over the interface object + the SnapStack store -- they are NOT lua_CFunctions and are
 * NEVER bound to a Lua state in this build (the Lua VM host is DEAD, 0 callers). The spec preserves them
 * as EXPORTS for SuperScript ABI compatibility (so the clone's export surface matches OG's).
 *
 * They ship as thin stubs (return 0 / empty) so the ABI EXPORT SURFACE is complete now; later work fills the
 * bodies (they call interface slots +0x28/+0x38/+0x60/+0x68/+0x1b8 -- the same object + store as `sh` and
 * the tabs). Exported by the OG names (undecorated C) via snaphakui.def.
 *
 * OG RVAs (for the C3 port): sl_is_valid_entityid 0x75cc, sl_get_entity_classname_impl 0x7588,
 * sl_get_entity_inherit_impl 0x7544, sl_get_entity_declsource_impl 0x7660, sl_show_toast_impl 0x75f8,
 * sl_push_entityid_sh 0x7700, sl_pop_entityid_sh 0x76a4, sl_get_group_size 0x770c,
 * sl_get_group_ids_array 0x77d0.
 *
 * Clean-room: our own RE of the OG sl_* bindings. Zero OG bytes.
 */
#include <cstdint>

/* All exported undecorated (extern "C") so snaphakui.def lists the OG names verbatim. The stub signatures
 * are the conservative widest shape (a pointer/int in, an int/pointer out); later work refines each to its real
 * SuperScript signature when the bodies land. Returning 0 / nullptr is the safe inert stub. */
extern "C" {

/* entity-id queries -> interface +0x28/+0x48/+0x50/+0x30. Stub: not-valid / empty. */
__declspec(dllexport) int          sl_is_valid_entityid(int entity_id)            { (void)entity_id; return 0; }
__declspec(dllexport) const char  *sl_get_entity_classname_impl(int entity_id)    { (void)entity_id; return ""; }
__declspec(dllexport) const char  *sl_get_entity_inherit_impl(int entity_id)      { (void)entity_id; return ""; }
__declspec(dllexport) const char  *sl_get_entity_declsource_impl(int entity_id)   { (void)entity_id; return ""; }

/* toast -> interface +0x1b8. Stub: no-op. */
__declspec(dllexport) void         sl_show_toast_impl(const char *label, const char *text)
{ (void)label; (void)text; }

/* SnapStack push/pop -> interface +0x60/+0x68 + the store. Stub: no-op / nothing. */
__declspec(dllexport) void         sl_push_entityid_sh(int stack_n, int entity_id)
{ (void)stack_n; (void)entity_id; }
__declspec(dllexport) int          sl_pop_entityid_sh(int stack_n)                { (void)stack_n; return -1; }

/* group queries -> the SnapStack group store. Stub: empty group. */
__declspec(dllexport) int          sl_get_group_size(const char *group_name)      { (void)group_name; return 0; }
__declspec(dllexport) const int   *sl_get_group_ids_array(const char *group_name) { (void)group_name; return nullptr; }

} /* extern "C" */
