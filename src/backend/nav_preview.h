/* nav_preview.h -- editor-only presentation of accepted navigation surfaces. */
#ifndef SH_NAV_PREVIEW_H
#define SH_NAV_PREVIEW_H
#include "signatures.h"

int sh_nav_preview_install(const sig_result *results, size_t count);
/* The editor thread assembles a complete frame, then publishes it atomically. */
void sh_nav_preview_begin(void *render_world);
void sh_nav_preview_add_line(const float start[3], const float end[3], void *unused);
void sh_nav_preview_publish(void);
void sh_nav_preview_clear(void);

/* 1 while a published set of lines is still being drawn for `world`. A caller
 * that would rebuild the same lines can skip the work while this holds. */
int  sh_nav_preview_published(const void *world);
#endif
