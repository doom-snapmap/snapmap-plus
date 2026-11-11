/* strkeys.h -- which strings keys are string edits (DECISIONS 9).
 *
 * A mod's strings key (mod.json "strings", without the "#str_" a decl writes)
 * that the game's language table or the product's built-in table already
 * defines is an edit of that string: it stays unprefixed, any mod may carry
 * it, and it applies the way that mod's other edits apply (strids.c). Every
 * other key is the mod's own and carries its "<salt>_" prefix.
 *
 * The game's keys are compiled in as hashes (strkeys_game.h, generated from
 * the supported build's English table); the built-in keys are the baked set
 * (strids_baked.h). Pure: no engine calls. Keys compare without case. */
#ifndef SNAPMAP_PLUS_STRKEYS_H
#define SNAPMAP_PLUS_STRKEYS_H

#include <stddef.h>

/* 1 when `key` (`n` bytes, no "#str_") names a string the game or the
 * product defines, else 0. */
int sh_strkey_is_edit(const char *key, size_t n);

#endif
