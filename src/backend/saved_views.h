#ifndef SH_SAVED_VIEWS_H
#define SH_SAVED_VIEWS_H
#include <stdint.h>

/* Keep formally placed module views when the editor makes a save or Play snapshot. */
int sh_saved_views_install(const uint8_t *base);
#endif
