#include "resource_rebind.h"
#include "model_binary.h"
#include "md6_binary.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct rr_edit {
    size_t offset, length, replacement_length;
    char *replacement;
} rr_edit;
typedef struct rr_context {
    sh_resource_binding_resolver resolver;
    void *context;
    size_t length, count, capacity;
    rr_edit *edits;
    sh_resource_binding_encoding encoding;
} rr_context;

static int rr_site(void *context, const char *family, const char *name, size_t offset, size_t length)
{
    rr_context *c = context;
    const char *replacement = NULL;
    size_t n;
    rr_edit edit = {offset, length, 0, NULL};
    int status = c->resolver(c->context, family, name, &replacement);
    if (status < 0) return 0;
    if (!status) return 1;
    if (!replacement || !*replacement) return 0;
    if (!strcmp(replacement, name)) return 1;
    n = strlen(replacement);
    /* The verified readers bound native idStr resource names to 64 KiB. */
    if ((c->encoding == SH_BINDING_IDSTR && (n > 0x10000u || length < 4)) ||
        offset > c->length || !length || length > c->length - offset) return 0;
    if (c->encoding == SH_BINDING_NATIVE_NAME) for (size_t i = 0; i < n; i++)
        if ((unsigned char)replacement[i] < 32 || replacement[i] == '"') return 0;
    if (c->count == c->capacity) {
        size_t next = c->capacity ? c->capacity * 2 : 16;
        rr_edit *edits;
        if (next <= c->capacity || next > SIZE_MAX / sizeof(*edits) ||
            !(edits = realloc(c->edits, next * sizeof(*edits)))) return 0;
        c->edits = edits; c->capacity = next;
    }
    edit.replacement = malloc(n + 1);
    if (!edit.replacement) return 0;
    memcpy(edit.replacement, replacement, n + 1); edit.replacement_length = n;
    c->edits[c->count++] = edit;
    return 1;
}

static int rr_order(const void *left, const void *right)
{
    const rr_edit *a = left, *b = right;
    return a->offset < b->offset ? -1 : a->offset > b->offset;
}

unsigned char *sh_resource_rebind_with(sh_resource_binding_reader reader, void *reader_context,
    sh_resource_binding_encoding encoding, const unsigned char *body, size_t length,
    sh_resource_binding_resolver resolver, void *context,
    size_t *output_length, size_t *changed, char *error, size_t capacity)
{
    rr_context c = {resolver, context, length, 0, 0, NULL, encoding};
    unsigned char *out = NULL;
    size_t i, total = length, previous = 0, used = 0;
    size_t overhead = encoding == SH_BINDING_IDSTR ? 4 : 0;
    if (error && capacity) error[0] = 0;
    if (output_length) *output_length = 0;
    if (changed) *changed = 0;
    if (!body || !resolver || !reader || (encoding != SH_BINDING_IDSTR && encoding != SH_BINDING_NATIVE_NAME) ||
        !reader(reader_context, body, length, rr_site, &c, error, capacity)) goto done;
    if (c.count > 1) qsort(c.edits, c.count, sizeof(*c.edits), rr_order);
    for (i = 0; i < c.count; i++) {
        const rr_edit *edit = &c.edits[i];
        if (edit->offset < previous || edit->length > total) goto done;
        previous = edit->offset + edit->length;
        total -= edit->length;
        if (total > SIZE_MAX - overhead || edit->replacement_length > SIZE_MAX - total - overhead) goto done;
        total += edit->replacement_length + overhead;
    }
    if (total == SIZE_MAX || !(out = malloc(total + 1))) goto done;
    previous = 0;
    for (i = 0; i < c.count; i++) {
        const rr_edit *edit = &c.edits[i];
        size_t prefix = edit->offset - previous, n = edit->replacement_length;
        memcpy(out + used, body + previous, prefix); used += prefix;
        if (overhead) {
            out[used++] = (unsigned char)n; out[used++] = (unsigned char)(n >> 8);
            out[used++] = (unsigned char)(n >> 16); out[used++] = (unsigned char)(n >> 24);
        }
        memcpy(out + used, edit->replacement, n); used += n;
        previous = edit->offset + edit->length;
    }
    memcpy(out + used, body + previous, length - previous); out[total] = 0;
    if (output_length) *output_length = total;
    if (changed) *changed = c.count;
done:
    for (i = 0; i < c.count; i++) free(c.edits[i].replacement);
    free(c.edits);
    if (!out && error && capacity && !*error) snprintf(error, capacity, "resource binding could not be relocated");
    return out;
}

static int rr_binary(void *context, const unsigned char *body, size_t length,
    sh_resource_binding_site visitor, void *visitor_context, char *error, size_t capacity)
{
    switch (*(sh_resource_rebind_format *)context) {
    case SH_REBIND_STATIC_MODEL:
        return sh_model_reference_sites(body, length, visitor, visitor_context, error, capacity);
    case SH_REBIND_MD6_MESH:
        return sh_md6_mesh_reference_sites(body, length, visitor, visitor_context, error, capacity);
    case SH_REBIND_MD6_ANIMATION:
        return sh_md6_anim_reference_sites(body, length, visitor, visitor_context, error, capacity);
    case SH_REBIND_DISCRETE_MODEL:
        return sh_discrete_model_reference_sites(body, length, visitor, visitor_context, error, capacity);
    case SH_REBIND_STATIC_PARTICLE:
        return sh_particle_model_read(body, length, error, capacity);
    default: return 0;
    }
}

unsigned char *sh_resource_rebind(sh_resource_rebind_format format,
    const unsigned char *body, size_t length, sh_resource_binding_resolver resolver,
    void *context, size_t *output_length, size_t *changed, char *error, size_t capacity)
{
    return sh_resource_rebind_with(rr_binary, &format, SH_BINDING_IDSTR, body, length,
        resolver, context, output_length, changed, error, capacity);
}
