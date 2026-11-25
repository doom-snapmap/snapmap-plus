#ifndef SH_MODVT_RESIDENT_H
#define SH_MODVT_RESIDENT_H

#include "modvt.h"
#include "signatures.h"

#define SH_VT_MAX_LOCK_PAGES 8192
typedef struct sh_modvt_pages {
    uint64_t count, reserved[5];
    uint64_t id[SH_VT_MAX_LOCK_PAGES];
} sh_modvt_pages;

typedef struct sh_modvt_cells {
    const uint64_t *ids;
    int count;
} sh_modvt_cells;

/* The provider lists actual cells from the root down. Share each mip across records before the next mip. */
int sh_modvt_coarse_order(const sh_modvt_cells cells[SH_MODVT_SLOTS], uint64_t *out, int capacity);
int sh_modvt_lock_budget(int pages, int percent);
int sh_modvt_served_cell(uint64_t id, unsigned squares);
/* Consult the same physical hash as the renderer. A collision with another owner returns -1. */
int sh_modvt_page_state(const uint8_t *pool, uint64_t id);
int sh_modvt_coarse_batch(const uint8_t *pool, int budget, const uint64_t *ids, int count,
                          uint32_t trusted, uint64_t *out, int capacity);
typedef char (*sh_modvt_feedback_fn)(void *, const uint64_t *, int *, char, char);
char sh_modvt_feedback_with_prefix(sh_modvt_feedback_fn consume, void *sys, const uint64_t *input,
                                   int *cursor, const uint64_t *prefix, int extra);
void sh_modvt_resident_install(const sig_result *feedback, const sig_result *provider, const uint8_t *base);
void sh_modvt_resident_frame(void);

#endif
