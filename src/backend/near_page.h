/* One read-write page within rel32 reach of an instruction, for the product
 * data or relay code an engine instruction is repointed at (nav_play.c,
 * object_entry.c, world_area_cell.c). */
#ifndef SH_NEAR_PAGE_H
#define SH_NEAR_PAGE_H
#include <windows.h>
#include <stdint.h>

#define SH_NEAR_PAGE_BYTES 4096u

/* Whether every byte of [page, page + SH_NEAR_PAGE_BYTES) is a rel32 away from `from`. */
static int sh_near_page_reaches(uintptr_t from, uintptr_t page)
{
    intptr_t lo = (intptr_t)page - (intptr_t)from;
    intptr_t hi = (intptr_t)(page + SH_NEAR_PAGE_BYTES) - (intptr_t)from;
    return lo >= INT32_MIN && hi <= INT32_MAX;
}

/* Commit the free allocation-granularity slot nearest `from` that it reaches,
 * read-write. `from` is the address a rel32 operand is relative to (the end of
 * its instruction). Release with VirtualFree(page, 0, MEM_RELEASE). NULL when
 * no slot within reach is free. */
static void *sh_near_page_alloc(uintptr_t from)
{
    SYSTEM_INFO info;
    uintptr_t step, center, delta;
    GetSystemInfo(&info);
    step = info.dwAllocationGranularity;
    center = from & ~(step - 1);
    for (delta = step; delta < 0x80000000u; delta += step) {
        uintptr_t candidates[2] = { center >= delta ? center - delta : 0, center + delta };
        int i;
        for (i = 0; i < 2; i++) {
            MEMORY_BASIC_INFORMATION mbi;
            uintptr_t a = candidates[i];
            void *page;
            if (!a || a < (uintptr_t)info.lpMinimumApplicationAddress ||
                a > (uintptr_t)info.lpMaximumApplicationAddress || !sh_near_page_reaches(from, a))
                continue;
            if (!VirtualQuery((void *)a, &mbi, sizeof mbi) || mbi.State != MEM_FREE) continue;
            page = VirtualAlloc((void *)a, SH_NEAR_PAGE_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (page) return page;
        }
    }
    return NULL;
}

#endif
