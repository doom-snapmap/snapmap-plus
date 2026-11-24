/* The engine's two command lists (idCmdSystemLocal): every command, and those the dispatcher searches in developer
 * mode; Snapmap+ keeps every command in both, with the cheat-exempt and developer flags. A list grown on the main
 * thread inside a frame allocates from a heap the engine destroys at the next level load, so room is reserved from a
 * helper thread (process heap) while the caller waits, and the backfill never grows a list. */
#ifndef SH_CMD_LISTS_H
#define SH_CMD_LISTS_H

#include <stdint.h>

/* The engine's idList growth helper; its one argument is the list. */
typedef void (*sh_idlist_grow_fn)(void *idlist);

/* idCmdSystemLocal: two idLists of idCommand pointers. */
#define SH_CMD_FULL_OFF     0x08u   /* array; count +8, capacity +0xC */
#define SH_CMD_DEV_OFF      0x20u
#define SH_CMD_FLAGS_OFF    0x20u   /* idCommand: name 0, handler 8, completion 0x10, help 0x18, flags 0x20 */
#define SH_CMD_BOTH_FLAGS   0x6u    /* 0x2 cheat-exempt | 0x4 developer list membership */

/* Makes room, off the calling thread, for `extra` more commands in the full list and for every command of the
 * full list in the developer list, waiting for the helper thread. Returns 1 when both lists have the room, 0 when
 * growth is unavailable or did not give it (see sh_cmd_lists_room). */
int sh_cmd_lists_reserve(uint8_t *cmdsys, sh_idlist_grow_fn grow, uint32_t extra);

/* Returns 1 when the full list and the developer list both take `extra` more commands without growing. */
int sh_cmd_lists_room(const uint8_t *cmdsys, uint32_t extra);

/* Sets the flags on every command and adds each to the developer list when it is not there. Never grows a list: a
 * command the developer list has no room for is counted in *left_out. Returns the commands walked. */
uint32_t sh_cmd_lists_unlock(uint8_t *cmdsys, uint32_t *left_out);

#endif
