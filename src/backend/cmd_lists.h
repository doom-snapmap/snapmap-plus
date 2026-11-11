/* The engine's two command lists (idCmdSystemLocal): every command, and the
 * commands the dispatcher looks through while a buffer runs in developer mode.
 * Snapmap+ keeps every command in both, with the cheat-exempt and developer
 * flags, so god, noclip, give and its own commands answer in either mode.
 *
 * Where the lists' memory comes from matters. A list grows through the
 * engine's allocator, which on the game's main thread inside a frame takes
 * the top of that thread's heap stack: a heap the engine destroys at the next
 * level load. An array grown there is freed under the list, and the next
 * command dispatched in developer mode (a key binding in a match, such as
 * Esc) reads freed memory. A thread with no heap stack allocates from the
 * process heap, which lasts. So room is reserved from a helper thread while
 * the caller waits (nothing else touches the lists meanwhile), and the
 * backfill itself never grows a list. */
#ifndef SH_CMD_LISTS_H
#define SH_CMD_LISTS_H

#include <stdint.h>

/* The engine's idList growth helper (one argument, the list). */
typedef void (*sh_idlist_grow_fn)(void *idlist);

/* idCmdSystemLocal: two idLists of idCommand pointers. */
#define SH_CMD_FULL_OFF     0x08u   /* array; count +8, capacity +0xC */
#define SH_CMD_DEV_OFF      0x20u
#define SH_CMD_FLAGS_OFF    0x20u   /* idCommand: name 0, handler 8, completion 0x10, help 0x18, flags 0x20 */
#define SH_CMD_BOTH_FLAGS   0x6u    /* 0x2 cheat-exempt | 0x4 developer list membership */

/* Make room, off the calling thread, for `extra` more commands in the full
 * list and for every command of the full list in the developer list. The
 * caller waits for the helper thread. Returns 1 when both lists have the
 * room, 0 when growth is unavailable or did not give it (then nothing may be
 * registered into a full list: see sh_cmd_lists_room). */
int sh_cmd_lists_reserve(uint8_t *cmdsys, sh_idlist_grow_fn grow, uint32_t extra);

/* 1 when the full list takes `extra` more commands and the developer list
 * takes them too, both without growing. */
int sh_cmd_lists_room(const uint8_t *cmdsys, uint32_t extra);

/* Set the flags on every command and add each to the developer list when it
 * is not there. Never grows a list: a command the developer list has no room
 * for is counted in *left_out and stays usable outside developer mode.
 * Returns the commands walked. */
uint32_t sh_cmd_lists_unlock(uint8_t *cmdsys, uint32_t *left_out);

#endif
