/* The vanilla rawmap made by a consented upgrade of one local save. */
#ifndef SH_LOCAL_MAP_COPY_H
#define SH_LOCAL_MAP_COPY_H
#include <stddef.h>

/* Returns the checked JSON (free), or NULL to continue with the original map. */
char *sh_local_map_copy(const char *root, const char *account, const char *slot,
                        const char *source, size_t length, size_t *out_length);
#endif
