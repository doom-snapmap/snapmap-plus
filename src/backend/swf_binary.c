#include "swf_binary.h"
#include "native_filename.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sh_swf_binary_paths_for(const char *name, sh_swf_binary_paths *out)
{
    char *source = NULL, *dot, *slash;
    size_t length, prefix;
    int ok = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!name || !*name) return 0;
    length = strlen(name); prefix = strncmp(name, "swf/", 4) ? 4 : 0;
    if (length > SIZE_MAX - prefix - 5 || !(source = malloc(length + prefix + 5))) return 0;
    if (prefix) memcpy(source, "swf/", prefix);
    memcpy(source + prefix, name, length + 1);
    for (size_t i = 0; i < prefix + length; i++) {
        unsigned char c = (unsigned char)source[i];
        source[i] = c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? (char)(c + 32) : (char)c;
    }
    slash = strrchr(source, '/'); dot = strrchr(source, '.');
    if (dot && (!slash || dot > slash)) *dot = 0;
    memcpy(source + strlen(source), ".swf", 5);
    if (!sh_native_generated_filename("generated/", source, ".bimage", out->atlas, sizeof(out->atlas))) goto done;
    memcpy(out->movie, out->atlas, strlen(out->atlas) + 1);
    /* The native constructor replaces the first .bimage occurrence and ends
     * the string there. Using .bswf directly changes long-name partitioning. */
    dot = strstr(out->movie, ".bimage");
    if (!dot) goto done;
    memcpy(dot, ".bswf", 6); ok = 1;
done:
    free(source);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

typedef struct sw_cursor {
    const unsigned char *body;
    size_t length, at;
    const sh_swf_binary_observer *observer;
    char *error;
    size_t capacity;
} sw_cursor;

static int sw_fail(sw_cursor *s, const char *reason)
{
    if (s->error && s->capacity && !*s->error)
        snprintf(s->error, s->capacity, "UI movie byte %zu: %s", s->at, reason);
    return 0;
}
static int sw_take(sw_cursor *s, size_t n)
{
    if (n > s->length - s->at) return sw_fail(s, "truncated record");
    s->at += n; return 1;
}
static int sw_word(sw_cursor *s, uint32_t *value)
{
    if (!sw_take(s, 4)) return 0;
    const unsigned char *p = s->body + s->at - 4;
    *value = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
        (uint32_t)p[2] << 8 | p[3];
    return 1;
}
static int sw_count(sw_cursor *s, size_t minimum, uint32_t *count)
{
    if (!sw_word(s, count)) return 0;
    if (*count > INT_MAX || (minimum && *count > (s->length - s->at) / minimum))
        return sw_fail(s, "invalid record count");
    return 1;
}
static int sw_array(sw_cursor *s, size_t stride)
{
    uint32_t count;
    return sw_count(s, stride, &count) && sw_take(s, (size_t)count * stride);
}
static int sw_string(sw_cursor *s, const char *family)
{
    size_t offset = s->at;
    uint32_t n;
    if (!sw_take(s, 4)) return 0;
    const unsigned char *p = s->body + offset;
    n = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    if (n > INT_MAX || !sw_take(s, n)) return sw_fail(s, "invalid string length");
    if (!family || !s->observer || !s->observer->reference ||
        (!strcmp(family, "material") && n && p[4] == '.')) return 1;
    /* Only reference records become C identities. Local/movie text is opaque. */
    if (memchr(p + 4, 0, n)) return sw_fail(s, "embedded NUL in resource identity");
    char *name = malloc((size_t)n + 1);
    if (!name) return sw_fail(s, "cannot allocate resource identity");
    memcpy(name, p + 4, n); name[n] = 0;
    int ok = s->observer->reference(s->observer->context, family, name, offset, (size_t)n + 4);
    free(name);
    return ok ? 1 : sw_fail(s, "resource visitor refused reference");
}
static int sw_commands(sw_cursor *s, size_t dictionary, int initialization)
{
    uint32_t count;
    if (!sw_count(s, initialization ? 4 : 8, &count)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t tag = 0, n;
        if ((!initialization && !sw_word(s, &tag)) || !sw_count(s, 1, &n)) return 0;
        size_t offset = s->at;
        if (!sw_take(s, n)) return 0;
        if (s->observer && s->observer->command &&
            !s->observer->command(s->observer->context, dictionary, initialization,
                tag, s->body + offset, offset, n)) return sw_fail(s, "command visitor refused record");
    }
    return 1;
}
static int sw_sprite(sw_cursor *s, size_t dictionary)
{
    uint32_t labels;
    if (!sw_take(s, 2) || !sw_array(s, 4) || !sw_count(s, 8, &labels)) return 0;
    for (uint32_t i = 0; i < labels; i++)
        if (!sw_take(s, 4) || !sw_string(s, NULL)) return 0;
    return sw_commands(s, dictionary, 0) && sw_commands(s, dictionary, 1);
}
static int sw_shape(sw_cursor *s)
{
    uint32_t fills, lines;
    if (!sw_take(s, 32) || !sw_count(s, 77, &fills)) return 0;
    for (uint32_t i = 0; i < fills; i++) {
        /* Two type bytes, two colors, two matrices, then inline gradient data. */
        if (!sw_take(s, 58) || !sw_take(s, 1)) return 0;
        unsigned gradients = s->body[s->at - 1];
        if (gradients > 16) return sw_fail(s, "gradient exceeds native inline storage");
        if (!sw_take(s, (size_t)gradients * 10) || !sw_take(s, 6) ||
            !sw_array(s, 8) || !sw_array(s, 8) || !sw_array(s, 2)) return 0;
    }
    if (!sw_count(s, 24, &lines)) return 0;
    for (uint32_t i = 0; i < lines; i++)
        if (!sw_take(s, 12) || !sw_array(s, 8) || !sw_array(s, 8) || !sw_array(s, 2)) return 0;
    return 1;
}
static int sw_font(sw_cursor *s)
{
    uint32_t glyphs;
    if (!sw_string(s, "font") || !sw_take(s, 6) || !sw_count(s, 12, &glyphs)) return 0;
    for (uint32_t i = 0; i < glyphs; i++)
        if (!sw_take(s, 4) || !sw_array(s, 8) || !sw_array(s, 2)) return 0;
    return 1;
}
int sh_swf_binary_read(const unsigned char *body, size_t length,
    const sh_swf_binary_observer *observer, char *error, size_t capacity)
{
    sw_cursor s = {body, length, 0, observer, error, capacity};
    uint32_t magic, entries;
    if (error && capacity) *error = 0;
    if (!body) return sw_fail(&s, "missing payload");
    if (!sw_word(&s, &magic)) return 0;
    if (magic != 0x42535717u) return sw_fail(&s, "unsupported cooked movie envelope");
    /* Source stamp, stage dimensions, frame rate, atlas dimensions. */
    if (!sw_take(&s, 22) || !sw_sprite(&s, SIZE_MAX) || !sw_count(&s, 4, &entries)) return 0;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t type;
        if (!sw_word(&s, &type)) return 0;
        switch (type) {
        case 0: break;
        case 1:
            if (!sw_string(&s, "material") || !sw_take(&s, 32)) return 0;
            break;
        case 2: case 3:
            if (!sw_shape(&s)) return 0;
            break;
        case 4:
            if (!sw_sprite(&s, i)) return 0;
            break;
        case 5:
            if (!sw_font(&s)) return 0;
            break;
        case 6:
            if (!sw_take(&s, 40) || !sw_array(&s, 15) || !sw_array(&s, 8)) return 0;
            break;
        case 7:
            if (!sw_take(&s, 42) || !sw_string(&s, NULL) || !sw_string(&s, NULL)) return 0;
            break;
        default: return sw_fail(&s, "unknown dictionary record");
        }
    }
    return s.at == length ? 1 : sw_fail(&s, "trailing data");
}
int sh_swf_binary_reference_sites(void *reader_context,
    const unsigned char *body, size_t length, sh_resource_binding_site visitor,
    void *context, char *error, size_t capacity)
{
    sh_swf_binary_observer observer = {context, visitor, NULL};
    (void)reader_context;
    return sh_swf_binary_read(body, length, &observer, error, capacity);
}
