/* Registration keys: the spawn-type bits AI registrations are matched by. */
#ifndef SH_PACKAGE_KEYS_H
#define SH_PACKAGE_KEYS_H

#include <stddef.h>
#include <stdint.h>

#include "decl_native_schema.h"

/* A spawn-type value (spawnAIType_t) is read by the engine (Vulkan 0x141a209b0)
 * either as one decimal number, when its text starts with a digit or '-' (the
 * first token through atoi; the rest is ignored), or as the union of its
 * space-separated constant names (other tokens, decimal numbers included, set
 * nothing). Unnamed bits can therefore only be written as a number. The spawn
 * manager selects every conductor entry whose low 32 bits share a bit with the
 * request and picks one at random (Vulkan 0x141123830, called only from the
 * spawn request constructor 0x14111b160 with spawnRequestData_t.spawnAIType).
 * Every type consumer (global modifiers, kill score, AI filter) classifies an
 * AI by the first conductor row registering its definition (0x141124470).
 * A bit no single-bit constant names is therefore an opaque key its package
 * chose for its own AI; engine code never names it. */
#define SH_PACKAGE_KEY_TYPE "spawnAIType_t"
#define SH_PACKAGE_KEY_BITS 32u
/* Provenance family of one key bit: original is the bit (decimal) its package
 * authored, name the key (decimal) that package holds in a compiled library. */
#define SH_PACKAGE_KEY_FAMILY "spawnaitype"

/* ---- Private keys at native dispatch ----------------------------------------
 * With the backend's spawn-key detours installed (spawn_keys.c) the unnamed
 * bits carry private codes, each naming a set of keys. Bits 19-27 are
 * single-bit keys; a value holding several names each of them, as native bits
 * do. Bit 28 marks a multi-bit code (bit 28 plus bits of 19-27): one key, or a
 * group when the active generation's group table defines it. A value that
 * names several keys of which one is multi-bit takes a group code, since the
 * OR of two multi-bit keys is a third code. Private codes match by set
 * intersection: a conductor row with private bits answers only requests whose
 * codes share a key with it, so a package's registration stays its own while
 * the named bits in the same row still classify it (a row of IMP's bit and a
 * key is an imp to every type consumer, yet stock IMP requests never select
 * it). Bit 29 is set only by the stock composites (all, all unique), which
 * keep their native meaning over stock rows and never select private rows; a
 * package's own composites are relocated over its own registrations. The enum
 * names bits 0-18 and 30 on the supported builds; other layouts use native
 * bits. */
#define SH_PACKAGE_KEY_SINGLE 0x0ff80000u
#define SH_PACKAGE_KEY_MARKER 0x10000000u
#define SH_PACKAGE_KEY_PRIVATE (SH_PACKAGE_KEY_SINGLE | SH_PACKAGE_KEY_MARKER)
#define SH_PACKAGE_KEY_COMPOSITE 0x20000000u
#define SH_PACKAGE_KEY_UNNAMED 0x3ff80000u

/* Private codes by index: the 9 single-bit keys, then the 511 multi-bit codes
 * in bit order. A set of keys is a bitmap over those indices. */
#define SH_PACKAGE_KEY_CODES 520u
#define SH_PACKAGE_KEY_PATTERNS 512u
typedef struct sh_package_key_set { uint64_t words[(SH_PACKAGE_KEY_CODES + 63u) / 64u]; } sh_package_key_set;
size_t sh_package_key_index(uint32_t code);            /* SIZE_MAX when not one private code */
uint32_t sh_package_key_code(size_t index);            /* 0 past the last code */
int sh_package_key_set_add(sh_package_key_set *set, uint32_t code);
int sh_package_key_set_has(const sh_package_key_set *set, uint32_t code);
int sh_package_key_set_meets(const sh_package_key_set *left, const sh_package_key_set *right);
size_t sh_package_key_set_count(const sh_package_key_set *set);
int sh_package_key_set_equal(const sh_package_key_set *left, const sh_package_key_set *right);

/* One generation's group codes, by their bits 19-27. A code the table does not
 * define names itself. */
typedef struct sh_package_key_groups {
    size_t count;
    unsigned char defined[SH_PACKAGE_KEY_PATTERNS];
    sh_package_key_set members[SH_PACKAGE_KEY_PATTERNS];
} sh_package_key_groups;
/* Define code (multi-bit, not yet defined) as a group of at least two keys. */
int sh_package_key_groups_define(sh_package_key_groups *groups, uint32_t code, const sh_package_key_set *members);
/* The group code the table gives exactly this set; 0 when none. */
uint32_t sh_package_key_groups_find(const sh_package_key_groups *groups, const sh_package_key_set *members);
/* The keys a value's private bits name (empty without private bits). */
void sh_package_key_decode(const sh_package_key_groups *groups, uint32_t value, sh_package_key_set *out);
/* The one code a serving generation gives a set: the key itself, a union of
 * single-bit keys, the group code either table already defines for it, or a
 * new group code defined in groups that no key in taken and no code of either
 * table holds. 0 when no code is left (or the set is empty). */
uint32_t sh_package_key_groups_code(sh_package_key_groups *groups, const sh_package_key_groups *other,
    const sh_package_key_set *taken, const sh_package_key_set *members);

/* The native matching contract under the detours, over one generation's
 * groups. Stock rows and stock requests keep the native intersection. A
 * request selects a private row only through a shared key; a modifier row
 * with keys applies only to AI sharing one and a stock modifier row to private
 * AI through named bits alone; a filter names stock types natively and private
 * AI by key. Stock composites keep their native meaning everywhere except
 * that a request never selects private rows through them. */
int sh_package_key_request_matches(const sh_package_key_groups *groups, uint32_t request, uint32_t entry);
int sh_package_key_modifier_matches(const sh_package_key_groups *groups, uint32_t type, uint32_t row);
int sh_package_key_filter_matches(const sh_package_key_groups *groups, uint32_t filter, uint32_t type);

/* The n-th private key in allocation order: single bits 19-27, then marked
 * multi-bit codes with fewer bits first. 0 past the last code. Group codes
 * come from the same order, after the keys a compile holds. */
uint32_t sh_package_key_private(size_t n);
int sh_package_key_private_valid(uint32_t key);

typedef struct sh_package_key_constant { char name[64]; uint32_t value; } sh_package_key_constant;
#define SH_PACKAGE_KEY_CONSTANTS 64u
typedef struct sh_package_key_space {
    uint32_t named;    /* bits a single-bit constant names */
    uint32_t pool;     /* unnamed bits a relocation may allocate */
    /* The enum's constants, single bits and composites, so a flag list that
     * spells a key by name can be read and rewritten. */
    sh_package_key_constant constants[SH_PACKAGE_KEY_CONSTANTS];
    size_t constant_count;
} sh_package_key_space;

/* Read the key enum's constants. The allocation pool excludes the sign bit,
 * whose decimal spelling exceeds the reader's signed range. Returns 1, 0 when
 * the enum is not registered, -1 for unreadable metadata. */
int sh_package_key_space_read(sh_decl_native_schema *schema, sh_package_key_space *space);
/* 1 when the enum leaves exactly bits 19-29 unnamed, the layout the private
 * key contract was verified on. */
int sh_package_key_space_private(const sh_package_key_space *space);

/* The unnamed bits of one value, which only a number can carry; a name list
 * has none. Values follow the engine's reading above. */
int sh_package_key_custom_bits(const sh_package_key_space *space, const char *text, size_t length,
    uint32_t *bits);
/* Every bit one value sets as the engine reads it (a name the enum lacks, or
 * a number after a name, sets none). */
int sh_package_key_bits(const sh_package_key_space *space, const char *text, size_t length, uint32_t *bits);
/* The bits a relocation can move: all of a number's, and a name list's
 * single-bit constants that no composite name in it (all, all unique)
 * already covers. */
int sh_package_key_movable_bits(const sh_package_key_space *space, const char *text, size_t length, uint32_t *bits);

/* Moves of one scope's keys, per bit. SH_PACKAGE_KEY_KEEP (zero) keeps the
 * bit, SH_PACKAGE_KEY_DROP removes it, SH_PACKAGE_KEY_REPLACE replaces it with
 * bits (which may keep the bit itself to add keys beside it) and, under the
 * private contract, with keys. */
enum { SH_PACKAGE_KEY_KEEP = 0, SH_PACKAGE_KEY_REPLACE = 1, SH_PACKAGE_KEY_DROP = 2 };
typedef struct sh_package_key_move { uint32_t bits; unsigned char kind; sh_package_key_set keys; } sh_package_key_move;
typedef sh_package_key_move sh_package_key_moves[SH_PACKAGE_KEY_BITS];
/* Replace a bit by one key; or keep the bit and add a key beside it. Without
 * the contract (private_keys 0) a key is a plain bit. */
void sh_package_key_move_to(sh_package_key_move *move, int private_keys, uint32_t key);
void sh_package_key_move_add(sh_package_key_move *move, unsigned bit, int private_keys, uint32_t key);
/* The single code a move leaves for its bit alone (its bits and one code for
 * its keys), as provenance records it; 0 when it needs a group none defines. */
uint32_t sh_package_key_move_code(const sh_package_key_move *move, unsigned bit, const sh_package_key_groups *groups);

/* How a relocation spells keys under the private contract: a set of several
 * keys that is not a union of single-bit keys asks group() for its code
 * (1 with a code, 0 when none is left, -1 on failure). A NULL encoder writes
 * keys as plain bits (no contract). */
typedef struct sh_package_key_encoder {
    int (*group)(void *context, const sh_package_key_set *keys, uint32_t *code);
    void *context;
} sh_package_key_encoder;

/* Rewrite one value through its moves, as the one decimal number of all its
 * resulting bits (negative when bit 31 is set): a number keeps the text after
 * its first token, which the engine ignores; a name list becomes the number.
 * Under the contract a composite (all, all unique) is relocated like the bits
 * it covers: its unnamed bits without a move are dropped, so a package's ALL
 * names the stock rows it keeps and exactly its own keys. Without the key enum
 * names set nothing. Returns a malloc-owned copy, or NULL when unchanged or on
 * failure (*failed set on failure; 2 when no group code is left). */
char *sh_package_key_relocate(const sh_package_key_space *space, const char *text, size_t length,
    const sh_package_key_moves to, const sh_package_key_encoder *encoder, size_t *output_length, int *failed);

/* A bit-flag property inspector (snappropertyinspector_bitflag) whose enumType
 * names the key enum offers its values[].enumValue strings as the choices an
 * author can set (the stock AI filter's spawnaitypebitflaginspector). Those
 * strings are key sites by that inspector contract, not by reflected type.
 * Returns a malloc-owned rewritten declaration, NULL when unchanged or not
 * such an inspector (*failed set on failure). */
char *sh_package_key_relocate_inspector(const sh_package_key_space *space, const char *text, size_t length,
    const sh_package_key_moves to, const sh_package_key_encoder *encoder, size_t *output_length, int *failed);

#endif
