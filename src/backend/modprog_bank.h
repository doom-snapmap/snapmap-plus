/* modprog_bank.h -- a mod's material decl in the bank's names (modprog.h, "The bank").
 *
 * An author writes the product's material programs' parameters under their readable names (layerstilingfactor,
 * eternalparticle, ...); the programs read them from the bank parms snapmapplusvec<k>. sh_modprog_bank gives the text
 * the engine is served instead:
 *   - the program it draws with: the multilayer blend when its ambientprogram names snapmapplus/multilayer/<base>
 *     (quoted or not, any case), else the particle program when it writes at least one of that program's keys
 *     (readable or bank); a material that names neither is not the product's to change;
 *   - each key (the first word of a line, any case) that is a readable name of that program becomes its bank name,
 *     the rest of the line as it was; a bank key stays;
 *   - a multilayer material gains every bank key it does not write, with the blend's default, and a particle material
 *     the others of its four, with the shading's: one line each, `snapmapplusvec<k>\t{ x, y, z, w }`, before the
 *     closing brace, in the text's own line ends. No key of one program's meaning is left to another's default.
 * Idempotent. Pure: no engine access. */
#ifndef SNAPMAP_PLUS_MODPROG_BANK_H
#define SNAPMAP_PLUS_MODPROG_BANK_H

#include <stddef.h>

#define SH_MODPROG_BANK_SAME     0     /* nothing to do: the text is served byte for byte as it is */
#define SH_MODPROG_BANK_CHANGED  1     /* *out holds the text to serve (malloc'd, NUL-terminated, *out_len bytes) */
#define SH_MODPROG_BANK_OOM    (-1)

/* The program a material decl draws with, as above. */
#define SH_MODPROG_BANK_NONE       0
#define SH_MODPROG_BANK_MULTILAYER 1
#define SH_MODPROG_BANK_PARTICLE   2
int sh_modprog_bank_program(const char *text, size_t len);

int sh_modprog_bank(const char *text, size_t len, char **out, size_t *out_len);

#endif /* SNAPMAP_PLUS_MODPROG_BANK_H */
