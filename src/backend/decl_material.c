#include "decl_material.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct mt_range { size_t begin, end; } mt_range;

static int mt_parameter_kind(const sh_decl_tokens *parsed, int *kind,
    char *error, size_t capacity)
{
    static const struct { const char *name; int kind; } types[] = {
        {"vec", 0}, {"tex", 1}, {"tex2d", 1}, {"tex3d", 2},
        {"texcube", 3}, {"environment", 3}, {"texshadow2d", 4},
        {"texshadow3d", 5}, {"texshadowcube", 6}, {"texarray2d", 7},
        {"texarraycube", 8}, {"texmultisample2d", 9}, {"sampler", 10},
        {"program", 11}, {"string", 12}, {"structuredbuffer", 13},
        {"struct", 14}, {"uniformbuffer", 15}, {"imagebuffer", 16},
        {"imagestorebuffer2d", 16}, {"imagestorebuffer2darray", 17},
        {"imagestorebuffer3d", 18}
    };
    sh_decl_tokens tokens = *parsed; int ok = 0;
    if (kind) *kind = -1;
    if (error && capacity) *error = 0;
    if (!kind) return 0;
    if (tokens.count < 3 || !sh_decl_token_is(&tokens, 0, "{") ||
        tokens.items[0].close != tokens.count - 1 || tokens.items[1].kind != SH_DECL_TOKEN_NAME) goto done;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        size_t length = tokens.items[1].end - tokens.items[1].begin;
        const char *name = tokens.source.text + tokens.items[1].begin;
        if (length == strlen(types[i].name) && !_strnicmp(name, types[i].name, length)) {
            *kind = types[i].kind; ok = 1; break;
        }
    }
done:
    if (!ok && error && capacity && !*error) snprintf(error, capacity, "renderparm has no supported native type prefix");
    return ok;
}

int sh_decl_material_parameter_kind(sh_decl_source source, int *kind,
    char *error, size_t capacity)
{
    sh_decl_tokens tokens = {0}; int ok;
    if (kind) *kind = -1;
    if (!kind || !sh_decl_native_lex(source, &tokens, error, capacity)) return 0;
    ok = mt_parameter_kind(&tokens, kind, error, capacity);
    sh_decl_tokens_free(&tokens); return ok;
}

typedef struct mt_context {
    sh_decl_material *out;
    const sh_decl_material_schema *schema;
    char *error;
    size_t capacity, write_capacity, reference_capacity;
    mt_range *pending;
    size_t pending_count, pending_capacity;
    size_t temporaries;
    const char *reason;
} mt_context;

static int mt_temporary(mt_context *c)
{
    /* Both native readers abort at their fixed temp1..temp128 register table.
     * This validates an engine format constraint, not a package payload quota. */
    if (c->temporaries == 128) { c->reason = "expression requires more than the engine's 128 temporary registers"; return 0; }
    c->temporaries++; return 1;
}

static int mt_grow(void **data, size_t *capacity, size_t count, size_t size)
{
    size_t next; void *grown;
    if (count < *capacity) return 1;
    next = *capacity ? *capacity * 2 : 16;
    if (next <= *capacity || next > SIZE_MAX / size) return 0;
    grown = realloc(*data, next * size);
    if (!grown) return 0;
    *data = grown; *capacity = next; return 1;
}
static int mt_is(mt_context *c, size_t i, const char *text)
{ return sh_decl_token_is(&c->out->tokens, i, text); }
static sh_decl_source mt_slice(mt_context *c, size_t first, size_t end)
{
    const sh_decl_tokens *t = &c->out->tokens;
    return (sh_decl_source){t->source.text + t->items[first].begin,
        t->items[end - 1].end - t->items[first].begin};
}
static sh_decl_source mt_name(mt_context *c, size_t i)
{
    sh_decl_source name = mt_slice(c, i, i + 1);
    if (c->out->tokens.items[i].kind == SH_DECL_TOKEN_STRING) { name.text++; name.length -= 2; }
    return name;
}
static int mt_same(sh_decl_source text, const char *word)
{
    if (text.length != strlen(word)) return 0;
    for (size_t i = 0; i < text.length; i++) {
        unsigned char a = (unsigned char)text.text[i], b = (unsigned char)word[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return 1;
}
static int mt_reference(mt_context *c, const char *family, sh_decl_source name, int *kind)
{
    int found;
    if (!name.length) { c->reason = "empty resource name"; return -1; }
    found = c->schema->resolve(c->schema->context, family, name, kind);
    if (found <= 0) return found;
    if (!strcmp(family, "renderparm") && (*kind < 0 || *kind > 18)) {
        c->reason = "invalid renderparm kind"; return -1;
    }
    if (!mt_grow((void **)&c->out->references, &c->reference_capacity,
        c->out->reference_count, sizeof(*c->out->references))) return -1;
    c->out->references[c->out->reference_count++] =
        (sh_decl_material_reference){family, name, c->out->count, *kind};
    return 1;
}
static int mt_pending(mt_context *c, size_t begin, size_t end)
{
    if (begin >= end) { c->reason = "empty numeric expression"; return 0; }
    if (!mt_grow((void **)&c->pending, &c->pending_capacity, c->pending_count, sizeof(*c->pending))) return 0;
    c->pending[c->pending_count++] = (mt_range){begin, end}; return 1;
}
static int mt_number(mt_context *c, size_t *p, size_t end, sh_decl_source *value)
{
    size_t start = *p;
    if (*p < end && mt_is(c, *p, "-")) (*p)++;
    if (*p >= end || c->out->tokens.items[*p].kind != SH_DECL_TOKEN_NUMBER) return 0;
    (*p)++; *value = mt_slice(c, start, *p); return 1;
}
static int mt_vector(mt_context *c, size_t *p, size_t end, sh_decl_source components[4])
{
    size_t start = *p, count = 0;
    for (size_t i = 0; i < 4; i++) components[i] = (sh_decl_source){"0", 1};
    if (!mt_is(c, start, "{")) {
        if (!mt_number(c, p, end, &components[0])) return 0;
        for (size_t i = 1; i < 4; i++) components[i] = components[0];
        return 1;
    }
    (*p)++;
    while (*p < end && count < 4) {
        if (!mt_number(c, p, end, &components[count++])) return 0;
        if (mt_is(c, *p, "}")) {
            (*p)++;
            if (count == 1) for (size_t i = 1; i < 4; i++) components[i] = components[0];
            return 1;
        }
        if (!mt_is(c, *p, ",")) return 0;
        (*p)++;
    }
    return 0;
}
static int mt_binary(mt_context *c, size_t i)
{
    static const char *operators[] = {"*", "/", "%", "+", "-", ">", ">=", "<", "<=", "==", "!=", "&&", "||"};
    for (size_t n = 0; n < sizeof(operators) / sizeof(operators[0]); n++) if (mt_is(c, i, operators[n])) return 1;
    return 0;
}
static int mt_leaf(mt_context *c, size_t *p, size_t end, sh_decl_source components[4], int *constant)
{
    sh_decl_tokens *tokens = &c->out->tokens;
    size_t start = *p;
    sh_decl_source name;
    int found, kind = -1, swizzle = 0;
    *constant = 0;
    if (start >= end) return 0;
    if (mt_is(c, start, "{") || mt_is(c, start, "-") || tokens->items[start].kind == SH_DECL_TOKEN_NUMBER) {
        *constant = 1; return mt_vector(c, p, end, components) && mt_temporary(c);
    }
    if (mt_is(c, start, "(")) {
        size_t close = tokens->items[start].close;
        if (close >= end || !mt_pending(c, start + 1, close)) return 0;
        *p = close + 1; return 1;
    }
    if (tokens->items[start].kind != SH_DECL_TOKEN_NAME && tokens->items[start].kind != SH_DECL_TOKEN_STRING) return 0;
    name = mt_name(c, start);
    if (mt_same(name, "dot3") || mt_same(name, "dot4")) {
        size_t open = start + 1, close, comma = SIZE_MAX, cursor;
        if (open >= end || !mt_is(c, open, "(")) return 0;
        close = tokens->items[open].close;
        if (close >= end) return 0;
        for (cursor = open + 1; cursor < close; cursor++) {
            if (mt_is(c, cursor, ",")) { if (comma != SIZE_MAX) return 0; comma = cursor; }
            if (tokens->items[cursor].close != SIZE_MAX) cursor = tokens->items[cursor].close;
        }
        if (comma == SIZE_MAX || !mt_pending(c, open + 1, comma) || !mt_pending(c, comma + 1, close)) return 0;
        *p = close + 1; return mt_temporary(c);
    }
    /* A source swizzle is part of the name token, not a destination mask. */
    for (size_t i = 0; i < name.length; i++) if (name.text[i] == '.') {
        size_t width = name.length - i - 1;
        if (!i || !width || width > 4) return 0;
        for (size_t n = i + 1; n < name.length; n++)
            if (!strchr("xyzwrgbaXYZWRGBA", name.text[n])) return 0;
        name.length = i; swizzle = 1; break;
    }
    found = mt_reference(c, "renderparm", name, &kind);
    if (found < 0) return 0;
    (*p)++;
    if (found) {
        if (kind != 0) { c->reason = "numeric expression references a non-vector renderparm"; return 0; }
        return !swizzle || mt_temporary(c);
    }
    if (*p >= end || !mt_is(c, *p, "[")) { c->reason = "unresolved numeric renderparm"; return 0; }
    found = mt_reference(c, "table", name, &kind);
    if (found <= 0) { c->reason = "unresolved numeric table"; return 0; }
    start = *p;
    if (tokens->items[start].close >= end || !mt_pending(c, start + 1, tokens->items[start].close)) return 0;
    *p = tokens->items[start].close + 1; return mt_temporary(c) && (!swizzle || mt_temporary(c));
}
static int mt_expression(mt_context *c, size_t *p, size_t end, sh_decl_source components[4], int *constant)
{
    int leaf_constant;
    if (!mt_leaf(c, p, end, components, constant)) return 0;
    while (*p < end && mt_binary(c, *p)) {
        sh_decl_source ignored[4];
        *constant = 0; (*p)++;
        if (!mt_leaf(c, p, end, ignored, &leaf_constant) || !mt_temporary(c)) return 0;
    }
    return 1;
}
static int mt_image_option(mt_context *c, size_t p)
{
    static const char *options[] = {"nearest", "linear", "clamp", "borderClamp", "mirror", "clampS", "clampT",
        "uncompressed", "YCoCgDXT5", "hqcompress", "hqcompressNormal", "bc4", "bc7", "bc6h", "LuminanceAlpha",
        "intensity", "alpha", "float", "pad2", "pad4", "pad8", "pad16", "fullScaleBias"};
    sh_decl_source name = mt_name(c, p);
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); i++) if (mt_same(name, options[i])) return 1;
    return 0;
}
static int mt_value(mt_context *c, size_t *p, size_t end, sh_decl_material_write *write)
{
    size_t start = *p;
    sh_decl_tokens *tokens = &c->out->tokens;
    if (start >= end) return 0;
    if (write->kind == 0) {
        if (!mt_expression(c, p, end, write->components, &write->constant)) return 0;
        while (c->pending_count) {
            mt_range range = c->pending[--c->pending_count];
            sh_decl_source ignored[4]; int constant;
            if (!mt_expression(c, &range.begin, range.end, ignored, &constant) || range.begin != range.end) return 0;
        }
    } else if (write->kind >= 1 && write->kind <= 11) {
        int kind = -1, found;
        const char *family = write->kind <= 9 ? "image" : write->kind == 10 ? "sampler" : "renderprog";
        if (write->kind <= 9) while (*p < end && mt_image_option(c, *p)) (*p)++;
        if (*p >= end || (tokens->items[*p].kind != SH_DECL_TOKEN_NAME && tokens->items[*p].kind != SH_DECL_TOKEN_STRING)) return 0;
        /* interactionProgram is consumed but ignored by the native block reader. */
        found = write->kind == 11 && mt_same(write->name, "interactionProgram") ? 1 :
            mt_reference(c, family, mt_name(c, *p), &kind);
        if (found <= 0) { c->reason = "unresolved typed material resource"; return 0; }
        (*p)++;
    } else if (write->kind == 12) {
        /* The native String helper stops before a token crossing a source line.
         * A closing brace on that same line belongs to the value and is invalid
         * as this block's terminator. Never move it onto a new line silently. */
        if (tokens->items[start].line != write->line) { c->reason = "missing same-line String value"; return 0; }
        while (*p < end && tokens->items[*p].line == write->line) (*p)++;
        if (*p == end && tokens->items[end].line == write->line) {
            c->reason = "String value consumes the block terminator on its line"; return 0;
        }
    } else { c->reason = "custom buffer renderparm requires its native reader"; return 0; }
    if (*p == start) return 0;
    write->value = mt_slice(c, start, *p); return 1;
}
void sh_decl_renderparm_free(sh_decl_renderparm *parameter)
{
    if (!parameter) return;
    sh_decl_tokens_free(&parameter->tokens); free(parameter->references); free(parameter->type_name);
    memset(parameter, 0, sizeof(*parameter));
}

static int mt_buffer_type(mt_context *c, size_t *p, size_t end, char **out)
{
    static const char *flags[] = {"writable", "writable_cs", "coherent", "early_fragment_tests"};
    size_t capacity = c->out->tokens.source.length, length = 0;
    char *name = capacity < SIZE_MAX ? malloc(capacity + 1) : NULL;
    int kind = -1, found;
    *out = NULL;
    if (!name) return 0;
    for (; *p < end; (*p)++) {
        sh_decl_source token = mt_name(c, *p); int flag = 0;
        if (mt_same(token, "{") || mt_same(token, "}")) goto bad;
        for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
            if (token.length == strlen(flags[i]) && !memcmp(token.text, flags[i], token.length)) {
            /* The native StructuredBuffer flag comparison is case-sensitive. */
            flag = 1; break;
        }
        if (flag) continue;
        if (token.length > capacity - length) goto bad;
        memcpy(name + length, token.text, token.length); length += token.length;
    }
    if (!length) { c->reason = "StructuredBuffer has no type"; goto bad; }
    name[length] = 0;
    found = mt_reference(c, "renderparm", (sh_decl_source){name, length}, &kind);
    if (found < 0 || (found > 0 && kind != 14)) {
        c->reason = "StructuredBuffer type is unreadable or not a struct renderparm"; goto bad;
    }
    /* Proven absence means native inline shader type text, not a missing file.
     * Named struct renderparms retain an explicit source dependency instead. */
    *out = name; return 1;
bad:
    free(name); return 0;
}

static int mt_store_format(mt_context *c, size_t p)
{
    static const char *words[] = {
        "early_fragment_tests", "rgba32f", "rgba16f", "rg32f", "rg16f",
        "r11f_g11f_b10f", "r32f", "r16f", "rgba32ui", "rgba16ui",
        "rgb10_a2ui", "rgba8ui", "rg32ui", "rg16ui", "rg8ui", "r32ui",
        "r16ui", "r8ui", "rgba32i", "rgba16i", "rgba8i", "rg32i", "rg16i",
        "rg8i", "r32i", "r16i", "r8i", "rgba16", "rgb10_a2", "rgba8",
        "rg16", "rg8", "r16", "r8", "rgba16_snorm", "rgba8_snorm",
        "rg16_snorm", "rg8_snorm", "r16_snorm", "r8_snorm"
    };
    sh_decl_source token = mt_name(c, p);
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) if (mt_same(token, words[i])) return 1;
    return 0;
}

int sh_decl_renderparm_read(sh_decl_source source, const sh_decl_material_schema *schema,
    sh_decl_renderparm *out, char *error, size_t capacity)
{
    sh_decl_material temporary = {0}; sh_decl_material_write value = {0};
    mt_context c = {0}; size_t p = 2, end; char *type_name = NULL;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (error && capacity) *error = 0;
    c.out = &temporary; c.schema = schema; c.error = error; c.capacity = capacity;
    c.reason = "unsupported or malformed renderparm default";
    if (!schema || !schema->resolve) goto bad;
    if (!sh_decl_native_lex(source, &temporary.tokens, error, capacity) ||
        !mt_parameter_kind(&temporary.tokens, &value.kind, error, capacity)) goto bad;
    end = temporary.tokens.count - 1;
    if (value.kind == 0) {
        if (!mt_vector(&c, &p, end, value.components)) goto bad;
    } else if ((value.kind == 10 || value.kind == 11) && p < end && mt_same(mt_name(&c, p), "0")) {
        p++; /* Only defaults use this native null sentinel. */
    } else if (value.kind == 12) {
        for (; p < end; p++) if (mt_same(mt_name(&c, p), "}")) goto bad;
    } else if (value.kind == 13) {
        if (!mt_buffer_type(&c, &p, end, &type_name)) goto bad;
    } else if (value.kind == 14) {
        size_t close;
        if (p >= end || !mt_is(&c, p, "{") || (close = temporary.tokens.items[p].close) >= end) goto bad;
        for (size_t i = p + 1; i < close; i++)
            if (mt_same(mt_name(&c, i), "{") || mt_same(mt_name(&c, i), "}")) goto bad;
        p = close + 1; /* Native stores raw shader structure text, no file loads. */
    } else if (value.kind == 15) {
        if (p >= end || temporary.tokens.items[p].kind != SH_DECL_TOKEN_NUMBER) goto bad;
        p++; /* Buffer byte count, not a resource name. */
    } else if (value.kind >= 16 && value.kind <= 18) {
        while (p < end && mt_store_format(&c, p)) p++;
        /* Native format/flag enums have no image-file reference. An unrecognized
         * token is left for the declaration's trailing edit-specifier reader. */
    } else if (!mt_value(&c, &p, end, &value)) goto bad;
    out->tokens = temporary.tokens; memset(&temporary.tokens, 0, sizeof(temporary.tokens));
    out->kind = value.kind;
    /* Empty String defaults are valid. Slices include original formatting. */
    if (p > 2) {
        size_t begin = out->tokens.items[2].begin;
        out->value = (sh_decl_source){source.text + begin, out->tokens.items[p - 1].end - begin};
    }
    if (p < end) {
        size_t begin = out->tokens.items[p].begin;
        out->edit_specifiers = (sh_decl_source){source.text + begin, out->tokens.items[end - 1].end - begin};
    }
    out->references = temporary.references; out->reference_count = temporary.reference_count;
    out->type_name = type_name;
    free(c.pending); return 1;
bad:
    if (error && capacity && !*error) snprintf(error, capacity, "renderparm: %s", c.reason);
    free(type_name); free(c.pending); sh_decl_material_free(&temporary); return 0;
}

static int mt_destination(mt_context *c, size_t p, sh_decl_material_write *write)
{
    const sh_decl_tokens *tokens = &c->out->tokens;
    size_t dot;
    if (tokens->items[p].kind != SH_DECL_TOKEN_NAME && tokens->items[p].kind != SH_DECL_TOKEN_STRING) return 0;
    write->name = mt_name(c, p); write->mask = 15; write->line = tokens->items[p].line;
    for (dot = 0; dot < write->name.length && write->name.text[dot] != '.'; dot++) {}
    if (dot < write->name.length) {
        unsigned next = 0; size_t i = dot + 1;
        write->mask = 0;
        for (; i < write->name.length; i++) {
            while (next < 4 && write->name.text[i] != "xyzw"[next] && write->name.text[i] != "rgba"[next]) next++;
            if (next == 4) { c->reason = "invalid ordered destination mask"; return 0; }
            write->mask |= 1u << next++;
        }
        if (!write->mask) return 0;
        write->name.length = dot;
    }
    if (mt_reference(c, "renderparm", write->name, &write->kind) <= 0) {
        c->reason = "unresolved destination renderparm"; return 0;
    }
    return 1;
}
void sh_decl_material_free(sh_decl_material *material)
{
    if (material) {
        sh_decl_tokens_free(&material->tokens); free(material->writes); free(material->references);
        memset(material, 0, sizeof(*material));
    }
}
int sh_decl_material_read(sh_decl_source source, const sh_decl_material_schema *schema,
    sh_decl_material *out, char *error, size_t capacity)
{
    mt_context context = {0}; size_t p = 1, end = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (error && capacity) *error = 0;
    context.out = out; context.schema = schema; context.error = error; context.capacity = capacity;
    context.reason = "malformed material parameter block";
    if (!schema || !schema->resolve) goto bad;
    if (!sh_decl_native_lex(source, &out->tokens, error, capacity)) return 0;
    if (out->tokens.count < 2 || !mt_is(&context, 0, "{") || out->tokens.items[0].close != out->tokens.count - 1) goto bad;
    end = out->tokens.count - 1;
    while (p < end) {
        sh_decl_material_write write = {0};
        write.first_reference = out->reference_count;
        if (!mt_destination(&context, p++, &write)) goto bad;
        if (mt_is(&context, p, "=")) p++;
        if (!mt_value(&context, &p, end, &write)) goto bad;
        if (mt_is(&context, p, ";")) p++;
        write.reference_count = out->reference_count - write.first_reference;
        if (!mt_grow((void **)&out->writes, &context.write_capacity, out->count, sizeof(*out->writes))) goto bad;
        out->writes[out->count++] = write;
    }
    free(context.pending); return 1;
bad:
    if (error && capacity && !*error) snprintf(error, capacity, "material line %zu: %s",
        p && p <= out->tokens.count ? out->tokens.items[p - 1].line : 1, context.reason);
    free(context.pending); sh_decl_material_free(out); return 0;
}
