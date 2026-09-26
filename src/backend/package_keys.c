#include "package_keys.h"
#include "decl_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pk_constant(void *context, const char *name, uint32_t value)
{
    sh_package_key_space *space = context;
    size_t length = name ? strlen(name) : 0;
    /* Composite constants (all, all-unique, archetype masks) name no key. */
    if (value && !(value & (value - 1))) space->named |= value;
    if (!length || length >= sizeof(space->constants[0].name)) return 1;
    if (space->constant_count == SH_PACKAGE_KEY_CONSTANTS) return 0;
    memcpy(space->constants[space->constant_count].name, name, length + 1);
    space->constants[space->constant_count++].value = value;
    return 1;
}
static int pk_named(const sh_package_key_space *space, const char *text, size_t length, uint32_t *value)
{
    for (size_t i = 0; space && i < space->constant_count; i++)
        if (!strncmp(space->constants[i].name, text, length) && !space->constants[i].name[length]) {
            *value = space->constants[i].value; return 1;
        }
    return 0;
}

int sh_package_key_space_read(sh_decl_native_schema *schema, sh_package_key_space *space)
{
    int status;
    if (!space) return -1;
    memset(space, 0, sizeof(*space));
    status = sh_decl_native_schema_enum_constants(schema, SH_PACKAGE_KEY_TYPE, pk_constant, space);
    if (status != 1) { memset(space, 0, sizeof(*space)); return status; }
    space->pool = ~space->named & 0x7fffffffu;
    return 1;
}

static int pk_digit(char c) { return c >= '0' && c <= '9'; }
static int pk_numeric(const char *text, size_t length) { return length && (text[0] == '-' || pk_digit(text[0])); }

/* The first token of a numeric value as the engine reads it (atoi: optional
 * sign, decimal digits, clamped to the int range). Returns its byte length. */
static size_t pk_number(const char *text, size_t length, uint32_t *value)
{
    size_t at = 0;
    int negative = 0;
    int64_t number = 0;
    if (at < length && text[at] == '-') { negative = 1; at++; }
    for (; at < length && pk_digit(text[at]); at++) {
        number = number * 10 + (text[at] - '0');
        if (number > (int64_t)INT32_MAX + 1) number = (int64_t)INT32_MAX + 1;
    }
    if (negative) number = -number;
    if (number > INT32_MAX) number = INT32_MAX;
    if (number < INT32_MIN) number = INT32_MIN;
    *value = (uint32_t)(int32_t)number;
    return at;
}

/* A value in the engine's own terms (Vulkan 0x141a209b0, which parses every
 * spawn-type field): one that starts with a digit or '-' is the decimal
 * number of its first token and nothing more; any other is the union of its
 * space-separated constant names, and other tokens (decimal numbers
 * included) set nothing. single receives the bits of single-bit names no
 * composite name in the same value already covers: the only bits of a name
 * list a relocation can move. */
static void pk_value(const sh_package_key_space *space, const char *text, size_t length, uint32_t *bits,
    uint32_t *single, int *numeric)
{
    uint32_t composite = 0;
    size_t at = 0;
    *bits = *single = 0;
    *numeric = pk_numeric(text, length);
    if (*numeric) { pk_number(text, length, bits); *single = *bits; return; }
    while (at < length) {
        size_t end;
        uint32_t value = 0;
        while (at < length && text[at] == ' ') at++;
        for (end = at; end < length && text[end] != ' '; end++) {}
        if (end > at && pk_named(space, text + at, end - at, &value)) {
            *bits |= value;
            if (value && !(value & (value - 1))) *single |= value; else composite |= value;
        }
        at = end;
    }
    *single &= ~composite;
}

int sh_package_key_custom_bits(const sh_package_key_space *space, const char *text, size_t length,
    uint32_t *bits)
{
    uint32_t value = 0, single = 0;
    int numeric = 0;
    *bits = 0;
    if (!space || !text) return 0;
    pk_value(space, text, length, &value, &single, &numeric);
    /* A constant never names a key: only a number carries one. */
    if (numeric) *bits = value & ~space->named;
    return 1;
}
int sh_package_key_bits(const sh_package_key_space *space, const char *text, size_t length, uint32_t *bits)
{
    uint32_t single = 0;
    int numeric = 0;
    *bits = 0;
    if (!text) return 0;
    pk_value(space, text, length, bits, &single, &numeric);
    return 1;
}
int sh_package_key_movable_bits(const sh_package_key_space *space, const char *text, size_t length, uint32_t *bits)
{
    uint32_t value = 0;
    int numeric = 0;
    *bits = 0;
    if (!text) return 0;
    pk_value(space, text, length, &value, bits, &numeric);
    return 1;
}

/* ---- Key sets ------------------------------------------------------------- */

size_t sh_package_key_index(uint32_t code)
{
    uint32_t pattern;
    if (!code || (code & ~SH_PACKAGE_KEY_PRIVATE)) return SIZE_MAX;
    if (!(code & SH_PACKAGE_KEY_MARKER)) {
        for (unsigned bit = 19; bit < 28; bit++) if (code == 1u << bit) return bit - 19;
        return SIZE_MAX;
    }
    pattern = (code & SH_PACKAGE_KEY_SINGLE) >> 19;
    return pattern ? 9 + (size_t)pattern - 1 : SIZE_MAX;
}
uint32_t sh_package_key_code(size_t index)
{
    if (index < 9) return 1u << (19 + index);
    if (index < SH_PACKAGE_KEY_CODES) return SH_PACKAGE_KEY_MARKER | ((uint32_t)(index - 8) << 19);
    return 0;
}
int sh_package_key_set_add(sh_package_key_set *set, uint32_t code)
{
    size_t index = sh_package_key_index(code);
    if (!set || index == SIZE_MAX) return 0;
    set->words[index / 64] |= 1ull << (index % 64); return 1;
}
int sh_package_key_set_has(const sh_package_key_set *set, uint32_t code)
{
    size_t index = sh_package_key_index(code);
    return set && index != SIZE_MAX && (set->words[index / 64] >> (index % 64) & 1u);
}
int sh_package_key_set_meets(const sh_package_key_set *left, const sh_package_key_set *right)
{
    for (size_t i = 0; i < sizeof(left->words) / sizeof(left->words[0]); i++)
        if (left->words[i] & right->words[i]) return 1;
    return 0;
}
size_t sh_package_key_set_count(const sh_package_key_set *set)
{
    size_t count = 0;
    for (size_t i = 0; i < sizeof(set->words) / sizeof(set->words[0]); i++)
        for (uint64_t word = set->words[i]; word; word &= word - 1) count++;
    return count;
}
int sh_package_key_set_equal(const sh_package_key_set *left, const sh_package_key_set *right)
{ return !memcmp(left->words, right->words, sizeof(left->words)); }
static void pk_set_union(sh_package_key_set *into, const sh_package_key_set *from)
{
    for (size_t i = 0; i < sizeof(into->words) / sizeof(into->words[0]); i++) into->words[i] |= from->words[i];
}

int sh_package_key_groups_define(sh_package_key_groups *groups, uint32_t code, const sh_package_key_set *members)
{
    uint32_t pattern = (code & SH_PACKAGE_KEY_SINGLE) >> 19;
    if (!groups || !members || !(code & SH_PACKAGE_KEY_MARKER) || sh_package_key_index(code) == SIZE_MAX ||
        groups->defined[pattern] || sh_package_key_set_count(members) < 2) return 0;
    groups->defined[pattern] = 1; groups->members[pattern] = *members; groups->count++;
    return 1;
}
uint32_t sh_package_key_groups_find(const sh_package_key_groups *groups, const sh_package_key_set *members)
{
    for (uint32_t pattern = 1; groups && groups->count && pattern < SH_PACKAGE_KEY_PATTERNS; pattern++)
        if (groups->defined[pattern] && sh_package_key_set_equal(groups->members + pattern, members))
            return SH_PACKAGE_KEY_MARKER | (pattern << 19);
    return 0;
}
void sh_package_key_decode(const sh_package_key_groups *groups, uint32_t value, sh_package_key_set *out)
{
    uint32_t code = value & SH_PACKAGE_KEY_PRIVATE;
    memset(out, 0, sizeof(*out));
    if (!code) return;
    if (code & SH_PACKAGE_KEY_MARKER) {
        uint32_t pattern = (code & SH_PACKAGE_KEY_SINGLE) >> 19;
        if (groups && groups->defined[pattern]) *out = groups->members[pattern];
        else sh_package_key_set_add(out, code);
        return;
    }
    for (unsigned bit = 19; bit < 28; bit++) if (code >> bit & 1u) sh_package_key_set_add(out, 1u << bit);
}
/* The one code a set needs no group for: none, one key, or single-bit keys
 * only (whose union already means each). 0 when it needs a group. */
static int pk_plain_code(const sh_package_key_set *keys, uint32_t *code)
{
    size_t count = 0;
    uint32_t single = 0, only = 0;
    int marked = 0;
    for (size_t i = 0; i < SH_PACKAGE_KEY_CODES; i++) {
        if (!(keys->words[i / 64] >> (i % 64) & 1u)) continue;
        only = sh_package_key_code(i); count++;
        if (only & SH_PACKAGE_KEY_MARKER) marked = 1; else single |= only;
    }
    *code = !count ? 0 : count == 1 ? only : single;
    return count < 2 || !marked;
}

uint32_t sh_package_key_groups_code(sh_package_key_groups *groups, const sh_package_key_groups *other,
    const sh_package_key_set *taken, const sh_package_key_set *members)
{
    uint32_t code = 0;
    if (!members) return 0;
    if (pk_plain_code(members, &code)) return code;
    if ((code = sh_package_key_groups_find(groups, members)) || (code = sh_package_key_groups_find(other, members)))
        return code;
    if (!groups) return 0;
    for (size_t n = 9; (code = sh_package_key_private(n)) != 0; n++) {
        uint32_t pattern = (code & SH_PACKAGE_KEY_SINGLE) >> 19;
        if ((taken && sh_package_key_set_has(taken, code)) || groups->defined[pattern] ||
            (other && other->defined[pattern])) continue;
        return sh_package_key_groups_define(groups, code, members) ? code : 0;
    }
    return 0;
}

void sh_package_key_move_to(sh_package_key_move *move, int private_keys, uint32_t key)
{
    move->kind = SH_PACKAGE_KEY_REPLACE;
    if (private_keys && sh_package_key_set_add(&move->keys, key)) return;
    move->bits |= key;
}
void sh_package_key_move_add(sh_package_key_move *move, unsigned bit, int private_keys, uint32_t key)
{
    if (move->kind != SH_PACKAGE_KEY_REPLACE) { move->kind = SH_PACKAGE_KEY_REPLACE; move->bits = 1u << bit; }
    if (private_keys && sh_package_key_set_add(&move->keys, key)) return;
    move->bits |= key;
}
uint32_t sh_package_key_move_code(const sh_package_key_move *move, unsigned bit, const sh_package_key_groups *groups)
{
    uint32_t code = 0;
    if (move->kind == SH_PACKAGE_KEY_KEEP) return 1u << bit;
    if (move->kind == SH_PACKAGE_KEY_DROP) return 0;
    if (!pk_plain_code(&move->keys, &code) && !(code = sh_package_key_groups_find(groups, &move->keys))) return 0;
    return move->bits | code;
}

char *sh_package_key_relocate(const sh_package_key_space *space, const char *text, size_t length,
    const sh_package_key_moves to, const sh_package_key_encoder *encoder, size_t *output_length, int *failed)
{
    uint32_t value = 0, single = 0, movable, moved = 0, code = 0;
    int numeric = 0, used, composite, changed = 0;
    size_t keep = 0;
    sh_package_key_set keys;
    char number[16], *out;
    *failed = 0;
    if (output_length) *output_length = 0;
    if (!text || length > SIZE_MAX - 32) { *failed = 1; return NULL; }
    pk_value(space, text, length, &value, &single, &numeric);
    /* Under the contract a composite covers exactly the bits it names, so a
     * package's own ALL moves like each of them. */
    composite = encoder && space && (value & SH_PACKAGE_KEY_COMPOSITE) != 0;
    movable = composite ? value : single;
    for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++)
        if ((movable >> bit & 1u) && to[bit].kind != SH_PACKAGE_KEY_KEEP) changed = 1;
    if (!changed) return NULL;
    memset(&keys, 0, sizeof(keys));
    for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++) {
        uint32_t mask = 1u << bit;
        if (!(value & mask)) continue;
        if (!(movable & mask)) { moved |= mask; continue; }
        if (to[bit].kind == SH_PACKAGE_KEY_REPLACE) { moved |= to[bit].bits; pk_set_union(&keys, &to[bit].keys); }
        else if (to[bit].kind == SH_PACKAGE_KEY_KEEP && !(composite && !(space->named & mask))) moved |= mask;
    }
    if (encoder) {
        /* Private bits left in place name their own keys: one multi-bit code,
         * or single-bit keys. The value then carries one code for them all. */
        uint32_t raw = moved & SH_PACKAGE_KEY_PRIVATE;
        moved &= ~SH_PACKAGE_KEY_PRIVATE;
        if (raw & SH_PACKAGE_KEY_MARKER) sh_package_key_set_add(&keys, raw);
        else for (unsigned bit = 19; bit < 28; bit++) if (raw >> bit & 1u) sh_package_key_set_add(&keys, 1u << bit);
        if (!pk_plain_code(&keys, &code)) {
            int status = encoder->group ? encoder->group(encoder->context, &keys, &code) : -1;
            if (status <= 0) { *failed = status ? 1 : 2; return NULL; }
        }
        moved |= code;
    }
    if (moved == value) return NULL;
    /* The engine reads unnamed bits only as one number, so a moved value is
     * written as the decimal of all its bits. A number keeps what follows its
     * first token (the engine ignores it); a name list becomes the number. */
    if (numeric) keep = length - pk_number(text, length, &value);
    used = (moved & 0x80000000u) ? snprintf(number, sizeof(number), "%d", (int)(int32_t)moved) :
        snprintf(number, sizeof(number), "%u", moved);
    if (used <= 0 || !(out = malloc((size_t)used + keep + 1))) { *failed = 1; return NULL; }
    memcpy(out, number, (size_t)used);
    if (keep) memcpy(out + used, text + (length - keep), keep);
    out[used + keep] = 0;
    if (output_length) *output_length = (size_t)used + keep;
    return out;
}

char *sh_package_key_relocate_inspector(const sh_package_key_space *space, const char *text, size_t length,
    const sh_package_key_moves to, const sh_package_key_encoder *encoder, size_t *output_length, int *failed)
{
    sh_decl_node *tree;
    const sh_decl_node *edit, *type, *values;
    const char *literal = NULL;
    size_t literal_length = 0;
    char *out = NULL;
    int changed = 0;
    *failed = 0;
    if (output_length) *output_length = 0;
    if (!(tree = sh_decl_tree_parse((sh_decl_source){text, length}, NULL, 0))) return NULL;
    edit = sh_decl_tree_member(tree, "edit");
    type = edit ? sh_decl_tree_member(edit, "enumType") : NULL;
    values = edit ? sh_decl_tree_member(edit, "values") : NULL;
    if (!type || !values || !sh_decl_tree_literal(type, &literal, &literal_length) ||
        literal_length != strlen(SH_PACKAGE_KEY_TYPE) || memcmp(literal, SH_PACKAGE_KEY_TYPE, literal_length)) goto done;
    for (sh_decl_node *item = values->children; item && !*failed; item = item->next) {
        sh_decl_node *value = item->compound ? (sh_decl_node *)sh_decl_tree_member(item, "enumValue") : NULL;
        size_t moved_length = 0;
        char *moved, *quoted;
        if (!value || !sh_decl_tree_literal(value, &literal, &literal_length)) continue;
        if (!(moved = sh_package_key_relocate(space, literal, literal_length, to, encoder, &moved_length, failed))) continue;
        if (!(quoted = malloc(moved_length + 3))) { free(moved); *failed = 1; break; }
        quoted[0] = '"'; memcpy(quoted + 1, moved, moved_length); quoted[moved_length + 1] = '"'; quoted[moved_length + 2] = 0;
        free(moved); free(value->value); value->value = quoted; changed = 1;
    }
    if (changed && !*failed && !(out = sh_decl_tree_write(tree, output_length))) *failed = 1;
done:
    sh_decl_tree_free(tree);
    return *failed ? (free(out), NULL) : out;
}

/* ---- Private keys ---------------------------------------------------------- */

static int pk_meets(const sh_package_key_groups *groups, uint32_t left, uint32_t right)
{
    sh_package_key_set a, b;
    sh_package_key_decode(groups, left, &a); sh_package_key_decode(groups, right, &b);
    return sh_package_key_set_meets(&a, &b);
}

int sh_package_key_request_matches(const sh_package_key_groups *groups, uint32_t request, uint32_t entry)
{
    if (!(entry & SH_PACKAGE_KEY_PRIVATE)) return (entry & request) != 0;
    if ((request & SH_PACKAGE_KEY_COMPOSITE) || !(request & SH_PACKAGE_KEY_PRIVATE)) return 0;
    return pk_meets(groups, request, entry);
}

int sh_package_key_modifier_matches(const sh_package_key_groups *groups, uint32_t type, uint32_t row)
{
    uint32_t own = type & SH_PACKAGE_KEY_PRIVATE, key = row & SH_PACKAGE_KEY_PRIVATE;
    if (row & SH_PACKAGE_KEY_COMPOSITE) return (row & type) != 0;
    if (!own) return !key && (row & type) != 0;
    if (key) return pk_meets(groups, row, type);
    return (row & type & ~SH_PACKAGE_KEY_PRIVATE) != 0;
}

int sh_package_key_filter_matches(const sh_package_key_groups *groups, uint32_t filter, uint32_t type)
{
    uint32_t own = type & SH_PACKAGE_KEY_PRIVATE, key = filter & SH_PACKAGE_KEY_PRIVATE;
    if (filter & SH_PACKAGE_KEY_COMPOSITE) return (filter & type) != 0;
    if (filter & type & ~SH_PACKAGE_KEY_PRIVATE) return 1;
    return own && key && pk_meets(groups, filter, type);
}

static unsigned pk_popcount(uint32_t value)
{
    unsigned count = 0;
    for (; value; value &= value - 1) count++;
    return count;
}

uint32_t sh_package_key_private(size_t n)
{
    const unsigned first = 19, singles = 9;
    if (n < singles) return 1u << (first + n);
    n -= singles;
    for (unsigned width = 1; width <= singles; width++)
        for (uint32_t subset = 1; subset < (1u << singles); subset++) {
            if (pk_popcount(subset) != width) continue;
            if (!n--) return SH_PACKAGE_KEY_MARKER | (subset << first);
        }
    return 0;
}

int sh_package_key_private_valid(uint32_t key)
{
    if (!key || (key & ~SH_PACKAGE_KEY_PRIVATE)) return 0;
    if (key & SH_PACKAGE_KEY_MARKER) return (key & SH_PACKAGE_KEY_SINGLE) != 0;
    return !(key & (key - 1));
}

int sh_package_key_space_private(const sh_package_key_space *space)
{
    return space && space->named == (0x0007ffffu | 0x40000000u) && space->pool == SH_PACKAGE_KEY_UNNAMED;
}
