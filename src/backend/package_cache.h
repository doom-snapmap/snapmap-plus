/* Reclamation of the package cache's derived storage: the per-map extraction
 * directories below package-cache/maps and the content-addressed files in
 * package-cache/resources. Nothing here reads or writes the overrides library,
 * authored packages or the small state files beside the resources directory. */
#ifndef SH_PACKAGE_CACHE_H
#define SH_PACKAGE_CACHE_H

#include <stddef.h>

/* Owner lock of a live extraction directory: a share-none, delete-on-close
 * file named .owner inside it, held for as long as the directory is in use by
 * this process. hold returns 1 once this process holds it (a directory already
 * held here stays held), 0 when another process holds it or the file cannot be
 * created. drop closes it; the system removes the file with the handle, also
 * after a crash. Drop before deleting the directory. */
int sh_package_cache_directory_hold(const char *directory);
void sh_package_cache_directory_drop(const char *directory);

/* Remove every <data_root>\package-cache\maps\map-<32 hex> directory whose
 * owner lock can be taken, that is, every extraction directory no process is
 * using. Returns the number removed. *held counts directories with a live
 * owner and *failed those whose deletion did not complete (an open handle
 * inside; left for a later sweep). Other names in maps are never touched. */
size_t sh_package_cache_sweep_maps(const char *data_root, size_t *held, size_t *failed);

/* Evict the content-addressed files in <data_root>\package-cache\resources
 * that keep(context, leaf) does not claim: content names (64 hex digits with an
 * optional .N collision suffix) and this product's own temporary names. Names
 * outside that grammar are foreign and left alone (*foreign). A file that
 * cannot be deleted because a handle holds it counts as held (*held), as every
 * sealed cache file in use does. *kept counts the claimed files. Returns the
 * number of files removed and adds their sizes to *bytes_freed. The library
 * directory and the state files beside resources are never entered. */
typedef int (*sh_package_cache_keep)(void *context, const char *leaf);
size_t sh_package_cache_evict_resources(const char *data_root, sh_package_cache_keep keep, void *context,
    size_t *kept, size_t *held, size_t *foreign, unsigned long long *bytes_freed);

#endif
