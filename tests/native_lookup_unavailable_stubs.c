/* These compiler-consumer fixtures never install native executable detours. */
#include <assert.h>
#include "hook.h"
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; assert(0); return NULL; }
sh_patch_status hook_commit(void *trampoline)
{ (void)trampoline; assert(0); return B2_PATCH_REFUSED_BADARG; }
