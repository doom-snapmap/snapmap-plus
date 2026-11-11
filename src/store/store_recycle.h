/* store_recycle.h -- the store tool's delete: the Windows Recycle Bin.
 *
 * sh_store_recycle moves one file or folder to the Recycle Bin with
 * IFileOperation (FOFX_RECYCLEONDELETE, no confirmation, no progress and no
 * error dialogs, so a console run never shows one). It never deletes for good:
 * when the shell reports that the bin cannot take the item (the bin is turned
 * off for that drive, or the item is larger than it), the operation is stopped
 * and the item stays in place. Links are never followed: a link (a junction or
 * symbolic link, the path itself or one inside a folder) is removed as a link
 * first, which deletes nothing it points at. It also refuses, deleting
 * nothing, a path on a drive that is not a fixed local disk and a path too
 * long for the shell. Returns 1 when the path is gone, else 0 with
 * GetLastError set (ERROR_NOT_SUPPORTED when the bin could not take it). It is
 * an sh_ms_discard_fn (modstore.h); `context` may be an sh_store_recycle_stats,
 * whose `refused` counts the paths left in place.
 */
#ifndef SNAPMAP_PLUS_STORE_RECYCLE_H
#define SNAPMAP_PLUS_STORE_RECYCLE_H

#include <wchar.h>

typedef struct sh_store_recycle_stats {
    unsigned recycled;          /* paths moved to the Recycle Bin */
    unsigned refused;           /* paths left in place */
} sh_store_recycle_stats;

int sh_store_recycle(void *context, const wchar_t *path);

#endif /* SNAPMAP_PLUS_STORE_RECYCLE_H */
