/* Backend log sink: OutputDebugStringA and a rotating session log under the
 * DLL directory. Independent of fault_record.
 */
#ifndef BACKEND_LOG_H
#define BACKEND_LOG_H

#include <windows.h>

/* Set the log path to <dir-of-self>\snapmap-plus\logs\sh_backend.log. Call once from DllMain. */
void backend_set_logpath_from_module(HINSTANCE self);

/* Timestamp + "[snapmap+] " prefix; write to OutputDebugStringA and append to the log file. */
void backend_log(const char *msg);

#endif /* BACKEND_LOG_H */
