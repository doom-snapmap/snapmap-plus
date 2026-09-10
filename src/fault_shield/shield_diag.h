/* Diagnostic-build crash and environment capture. Handlers preserve exception
 * disposition; capture is best-effort and cannot observe every termination path.
 * Writes sh_diag.log and a local dump under snapmap-plus\logs. */
#ifndef SNAPMAP_PLUS_SHIELD_DIAG_H
#define SNAPMAP_PLUS_SHIELD_DIAG_H
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once from DLL_PROCESS_ATTACH to install handlers and start the worker
 * that collects the environment. Repeated calls are ignored. */
void shield_diag_install(HINSTANCE self);

/* Record exception-handler breadcrumbs at DLL_PROCESS_DETACH. */
void shield_diag_detach(void);

#ifdef __cplusplus
}
#endif
#endif /* SNAPMAP_PLUS_SHIELD_DIAG_H */
