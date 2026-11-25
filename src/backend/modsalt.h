/* modsalt.h -- a mod's identity: its salt, eight random characters ([a-z][a-z0-9]{7}) drawn once at the mod's first
 * scan and written in mod.json's "snapmap-plus" block. It is the key everywhere (mods/<salt>/, string prefixes,
 * <salt>@<build hash> in a map; the folder name never carries identity), and the grammar keeps it one spelling in
 * decl words, bank stems (<salt>_<rest>) and the engine's cooked names. Pure: no engine calls. */
#ifndef SNAPMAP_PLUS_MODSALT_H
#define SNAPMAP_PLUS_MODSALT_H

#include <stddef.h>

#define SH_SALT_LEN 8

/* 1 when the first n bytes of s are a salt (exactly SH_SALT_LEN characters). */
int sh_salt_valid(const char *s, size_t n);

/* A new random salt from the system's cryptographic generator. Returns 1 with
 * out NUL-terminated, 0 when the generator fails. */
int sh_salt_generate(char out[SH_SALT_LEN + 1]);

/* sh_salt_generate, redrawn while `held(context, salt)` answers nonzero (the
 * caller's "the root already holds it"). Gives up after 64 draws. held may be
 * NULL. */
typedef int (*sh_salt_held_fn)(void *context, const char *salt);
int sh_salt_generate_except(char out[SH_SALT_LEN + 1], sh_salt_held_fn held, void *context);

#endif /* SNAPMAP_PLUS_MODSALT_H */
