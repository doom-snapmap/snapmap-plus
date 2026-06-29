/* fault_record.h -- the fault-shield's single fault record + its formatter/emitter.
 *
 * The record is the one source of truth the console, any attached debugger, the log file, and
 * the in-game popup all read. shield_format is pure + deterministic (unit-tested off-game); shield_emit
 * stamps a timestamp and writes to OutputDebugStringA + shield_faults.log.
 */
#ifndef SHIELD_FAULT_RECORD_H
#define SHIELD_FAULT_RECORD_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

typedef struct shield_fault {
    const char *cls;          /* "load" | "action" | "unknown" */
    int         severity;     /* engine level (6/7), or -1 for a raw AV */
    const char *message;      /* the "why" */
    uintptr_t   faulting_rva; /* rip - base, or 0 */
    uintptr_t   fault_addr;   /* faulting data address, or 0 */
} shield_fault;

/* Pure + deterministic: format the record body (no timestamp). Returns chars written (>=0). */
int  shield_format(char *buf, size_t n, const shield_fault *f);

/* Prepend a local timestamp + "[shield] " and write to OutputDebugStringA (-> in-game console +
 * any attached debugger) and append to shield_faults.log. */
void shield_emit(const shield_fault *f);

/* Set the log path to <dir-of-self>\shield_faults.log. Call once from DllMain (DLL_PROCESS_ATTACH). */
void shield_set_logpath_from_module(HINSTANCE self);

#endif /* SHIELD_FAULT_RECORD_H */
