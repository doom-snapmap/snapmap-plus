/* Compatibility exports for the original sl_* surface. These are inert stubs;
 * this build does not host a Lua VM. Export names are pinned by the .def file. */
#include <cstdint>

/* Keep undecorated C linkage. Signatures are compatibility placeholders and
 * results remain zero/null until these operations are implemented. */
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
