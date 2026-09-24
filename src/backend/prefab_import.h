/* Portable prefab admission on the destination editor's own frame. */
#ifndef SH_PREFAB_IMPORT_H
#define SH_PREFAB_IMPORT_H
#include <stddef.h>

/* Copy a request from the engine main thread. Accepted requests retain their
 * original destination map through consent; success here means queued, not
 * placed. No map load, world reset or installation occurs in this function. */
int sh_prefab_import_request(void *editor, const char *json, size_t length, int place);
/* Only the editor-frame pre-Think boundary drives native admission. */
void sh_prefab_import_poll(void *editor);
/* Tick maintenance can cancel a departed destination without starting native
 * import work. This also withdraws consent after leaving the editor. */
void sh_prefab_import_maintenance(void);
/* Protect a pending install from unrelated global refresh/commit operations. */
int sh_prefab_import_pending(void);
#endif
