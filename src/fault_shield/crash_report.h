/* Persist fault records under <game>\snapmap-plus\crash\pending-*.json.
 * The UI polls these records for notices or report prompts. Recovery paths,
 * terminal engine throws, and fatal handlers can produce records; a Class-B or
 * off-thread record alone does not establish that the process survived.
 * Fatal handlers also attempt a local minidump. Capture does not alter exception
 * disposition, and write failures are ignored. */
#ifndef SHIELD_CRASH_REPORT_H
#define SHIELD_CRASH_REPORT_H

#include <windows.h>
#include <stdint.h>

/* Resolve paths, snapshot the installed version and renderer, and bind dbghelp.
 * Call once outside the loader lock, before arming fatal handlers. */
void crash_report_init(void);

/* Arm the fatal VEH and unhandled filter, then reassert the filter during startup.
 * Direct fail-fast termination may bypass both handlers. Call after initialization. */
void crash_report_arm_fatal_handlers(void);

/* Write one crash record (pending-<stamp>.json, CREATE_NEW, write-through). Crash-safe: static
 * buffers, no CRT heap. Bounded per session so a fault storm cannot spam the directory. */
void crash_report_file(const char *kind, unsigned long code, uintptr_t rip_rva,
                       uintptr_t fault_addr, const char *module_name,
                       const char *stack, const char *engine_text, const char *dump_path);

/* Best-effort minidump for the fatal path (MiniDumpNormal|WithThreadInfo -> snapmap-plus\logs\
 * sh_crash.dmp). Returns the dump path ("" if it could not be written). LOCAL ONLY. */
const char *crash_report_write_dump(EXCEPTION_POINTERS *ep);

#endif /* SHIELD_CRASH_REPORT_H */
