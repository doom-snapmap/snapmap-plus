#include "package_keys.h"
#include "decl_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pk_constant(void *context, const char *name, uint32_t value)
{
    sh_package_key_space *space = context;
    (void)name;
    /* Composite constants (all, all-unique, archetype masks) name no key. */
    if (value && !(value & (value - 1))) space->named |= value;
    return 1;
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

static int pk_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int pk_digit(char c) { return c >= '0' && c <= '9'; }
static int pk_name(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }

/* One token of a flag list: 1 a decimal number (value set), 2 a name, 3 a
 * negative number (a composite mask), 0 anything else. */
static int pk_token(const char *text, size_t length, uint32_t *value)
{
    uint64_t number = 0;
    if (!length) return 0;
    if (pk_name(text[0])) {
        for (size_t i = 1; i < length; i++)
            if (!pk_name(text[i]) && !pk_digit(text[i])) return 0;
        return 2;
    }
    if (text[0] == '-') {
        if (length < 2) return 0;
        for (size_t i = 1; i < length; i++) if (!pk_digit(text[i])) return 0;
        return 3;
    }
    for (size_t i = 0; i < length; i++) {
        if (!pk_digit(text[i])) return 0;
        number = number * 10u + (uint64_t)(text[i] - '0');
        if (number > UINT32_MAX) return 0;
    }
    *value = (uint32_t)number; return 1;
}

int sh_package_key_custom_bits(const sh_package_key_space *space, const char *text, size_t length,
    uint32_t *bits)
{
    size_t at = 0;
    *bits = 0;
    if (!space || !text) return 0;
    while (at < length) {
        size_t end;
        uint32_t value = 0;
        while (at < length && pk_space(text[at])) at++;
        if (at == length) break;
        for (end = at; end < length && !pk_space(text[end]); end++) {}
        int kind = pk_token(text + at, end - at, &value);
        if (!kind) { *bits = 0; return 0; }
        if (kind == 1) *bits |= value & ~space->named;
        at = end;
    }
    return 1;
}

char *sh_package_key_relocate(const char *text, size_t length, const unsigned char to[SH_PACKAGE_KEY_BITS],
    size_t *output_length, int *failed)
{
    char *out = NULL;
    size_t used = 0, at = 0, capacity;
    int changed = 0;
    *failed = 0;
    if (output_length) *output_length = 0;
    if (!text || length > (SIZE_MAX - 64) / 6) { *failed = 1; return NULL; }
    /* A rewritten token has at most ten digits: a one-digit token and its
     * separator (two bytes) grow to eleven at most. */
    capacity = length * 6 + 64;
    if (!(out = malloc(capacity))) { *failed = 1; return NULL; }
    while (at < length) {
        size_t end;
        uint32_t value = 0, moved = 0;
        if (pk_space(text[at])) { out[used++] = text[at++]; continue; }
        for (end = at; end < length && !pk_space(text[end]); end++) {}
        if (pk_token(text + at, end - at, &value) == 1) {
            for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++) {
                uint32_t mask = 1u << bit;
                if (!(value & mask)) continue;
                moved |= to[bit] < SH_PACKAGE_KEY_BITS ? 1u << to[bit] : mask;
            }
        } else moved = value;
        if (moved != value) {
            used += (size_t)snprintf(out + used, capacity - used, "%u", moved);
            changed = 1;
        } else { memcpy(out + used, text + at, end - at); used += end - at; }
        at = end;
    }
    if (!changed) { free(out); return NULL; }
    out[used] = 0;
    if (output_length) *output_length = used;
    return out;
}

char *sh_package_key_relocate_inspector(const char *text, size_t length,
    const unsigned char to[SH_PACKAGE_KEY_BITS], size_t *output_length, int *failed)
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
        if (!(moved = sh_package_key_relocate(literal, literal_length, to, &moved_length, failed))) continue;
        if (!(quoted = malloc(moved_length + 3))) { free(moved); *failed = 1; break; }
        quoted[0] = '"'; memcpy(quoted + 1, moved, moved_length); quoted[moved_length + 1] = '"'; quoted[moved_length + 2] = 0;
        free(moved); free(value->value); value->value = quoted; changed = 1;
    }
    if (changed && !*failed && !(out = sh_decl_tree_write(tree, output_length))) *failed = 1;
done:
    sh_decl_tree_free(tree);
    return *failed ? (free(out), NULL) : out;
}
