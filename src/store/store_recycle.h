/* store_recycle.h -- the store tool's delete: the Windows Recycle Bin.
 *
 * sh_store_recycle moves one file or folder to the Recycle Bin with
 * SHFileOperationW (FO_DELETE, FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT,
 * and FOF_NOERRORUI so a console run never shows a dialog). Links are never
 * followed: a link (a junction or symbolic link, the path itself or one inside
 * a folder) is removed as a link first, which deletes nothing it points at.
 * It refuses, deleting nothing, a path on a drive that is not a fixed local
 * disk (no Recycle Bin to receive it) and a path too long for the shell.
 * Returns 1 when the path is gone, else 0 with GetLastError set. It is an
 * sh_ms_discard_fn (modstore.h); `context` may be an sh_store_recycle_stats.
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
