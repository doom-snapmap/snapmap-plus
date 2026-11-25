/* strkeys.h -- which mod.json strings keys are edits of an existing string (one the game's table, strkeys_game.h,
 * or the baked set, strids_baked.h, defines): those stay unprefixed; every other key takes its "<salt>_" prefix.
 * Pure, no engine calls; keys compare without case. */
#ifndef SNAPMAP_PLUS_STRKEYS_H
#define SNAPMAP_PLUS_STRKEYS_H

#include <stddef.h>

/* 1 when `key` (`n` bytes, no "#str_") names a string the game or the
 * product defines, else 0. */
int sh_strkey_is_edit(const char *key, size_t n);

#endif
