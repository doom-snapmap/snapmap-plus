/* Fault-log record and formatter. shield_emit adds a timestamp and writes to
 * shield_faults.log (and the debugger stream). JSON crash records are separate. */
#ifndef SHIELD_FAULT_RECORD_H
#define SHIELD_FAULT_RECORD_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

typedef struct shield_fault {
    const char *cls;          /* diagnostic class, e.g. "load", "action", or "offthread" */
    int         severity;     /* engine level, exception status, or -1 when unavailable */
    const char *message;      /* the "why" */
    uintptr_t   faulting_rva; /* rip - base, or 0 */
    uintptr_t   fault_addr;   /* faulting data address, or 0 */
} shield_fault;

/* Pure + deterministic: format the record body (no timestamp). Returns chars written (>=0). */
int  shield_format(char *buf, size_t n, const shield_fault *f);

/* Prepend a local timestamp + "[shield] ", append to shield_faults.log and send to any attached
 * debugger. */
void shield_emit(const shield_fault *f);

/* Set the log path to <dir-of-self>\snapmap-plus\logs\shield_faults.log (a wide path of any
 * length). Call once from DllMain (DLL_PROCESS_ATTACH). */
void shield_set_logpath_from_module(HINSTANCE self);

#endif /* SHIELD_FAULT_RECORD_H */
