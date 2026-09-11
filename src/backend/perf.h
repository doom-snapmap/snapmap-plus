/* Wall-clock counters for the paths that run inside a DOOM frame or inside a
 * map load. Reported by the sh_perf console command.
 *
 * A sample is two QueryPerformanceCounter reads and one interlocked add, so a
 * counter may sit on a path that runs hundreds of thousands of times.
 */
#ifndef SH_PERF_H
#define SH_PERF_H

#include <windows.h>

typedef enum {
    SH_PERF_OVERRIDE_OPEN = 0,  /* the whole hook: our decision plus the engine's open */
    SH_PERF_OVERRIDE_STAT,      /* the file-exists checks that decision makes */
    SH_PERF_ENGINE_OPEN,        /* the engine's own open, which the hook chains to */
    SH_PERF_BRIDGE_OPEN,        /* the installed-resource lookup the same decision makes */
    SH_PERF_INTERNAL_DECL,      /* the published-decl lookup the same decision makes */
    SH_PERF_MAP_SERIALIZE,      /* the engine writing the open map as JSON */
    SH_PERF_NAV_PARSE,          /* reading navigation volumes out of that JSON */
    SH_PERF_NAV_PREVIEW,        /* building the green preview lines */
    SH_PERF_LOG_WRITE,          /* one line appended to the session log */
    SH_PERF_SLOTS
} sh_perf_slot;

LONGLONG sh_perf_now(void);
void     sh_perf_add(sh_perf_slot slot, LONGLONG ticks);
void     sh_perf_report(void (*out)(const char *fmt, ...));
void     sh_perf_reset(void);

#define SH_PERF_BEGIN(var)       LONGLONG var = sh_perf_now()
#define SH_PERF_END(slot, var)   sh_perf_add((slot), sh_perf_now() - (var))

#endif /* SH_PERF_H */
