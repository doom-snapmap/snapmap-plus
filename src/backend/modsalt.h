/* modsalt.h -- a mod's identity: its salt.
 *
 * Every mod gets eight random characters once, at its first scan, written in
 * the builder's part of mod.json ("snapmap-plus": {"salt": ...}). The salt is
 * the key everywhere: the mod's files live under mods/<salt>/, its strings
 * are prefixed by it, a map records <salt>@<build hash>. The folder name is
 * the author's and never carries identity.
 *
 * Grammar: [a-z][a-z0-9]{7}. The leading letter and the absence of '-' and
 * '_' keep the salt one spelling in decl words, bank stems (<salt>_<rest>)
 * and the engine's cooked names. Pure: no engine calls. */
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
