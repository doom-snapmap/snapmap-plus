#include "package_shader.h"
#include "native_filename.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t ps_le(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t ps_be(const unsigned char *p)
{ return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24; }
static void ps_put_le(unsigned char *p, uint32_t n)
{ for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(n >> (i * 8)); }
static void ps_put_be(unsigned char *p, uint32_t n)
{ for (unsigned i = 0; i < 4; i++) p[3 - i] = (unsigned char)(n >> (i * 8)); }
static int ps_error(char *error, size_t capacity, int status, const char *reason)
{ if (error && capacity) snprintf(error, capacity, "%s", reason); return status; }

/* Independent implementation of the MD5 algorithm described in RFC 1321.
 * The engine's checksum folds its words; it is not used for equality/security. */
static void ps_md5_block(uint32_t state[4], const unsigned char *block)
{
    static const uint32_t constants[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const unsigned shifts[16] = {7,12,17,22,5,9,14,20,4,11,16,23,6,10,15,21};
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t f, next; unsigned word, shift = shifts[(i / 16) * 4 + i % 4];
        if (i < 16) { f = (b & c) | (~b & d); word = i; }
        else if (i < 32) { f = (d & b) | (~d & c); word = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; word = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); word = (7 * i) % 16; }
        next = a + f + constants[i] + ps_le(block + word * 4);
        next = b + ((next << shift) | (next >> (32 - shift)));
        a = d; d = c; c = b; b = next;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}
uint32_t sh_package_shader_checksum(const unsigned char *body, size_t length)
{
    uint32_t state[4] = {0x67452301,0xefcdab89,0x98badcfe,0x10325476};
    unsigned char tail[128] = {0};
    uint64_t bits = (uint64_t)length * 8;
    size_t remaining = length, size;
    if (!body && length) return 0;
    while (remaining >= 64) { ps_md5_block(state, body); body += 64; remaining -= 64; }
    if (remaining) memcpy(tail, body, remaining);
    tail[remaining] = 0x80; size = remaining < 56 ? 64 : 128;
    for (unsigned i = 0; i < 8; i++) tail[size - 8 + i] = (unsigned char)(bits >> (8 * i));
    ps_md5_block(state, tail);
    if (size == 128) ps_md5_block(state, tail + 64);
    return state[0] ^ state[1] ^ state[2] ^ state[3];
}
uint32_t sh_package_shader_declaration_checksum(const unsigned char *body, size_t length)
{
    if (!body && length) return 0;
    while (length && (body[length - 1] == ' ' || body[length - 1] == '\r' || body[length - 1] == '\n')) length--;
    return sh_package_shader_checksum(body, length);
}

typedef struct ps_reader {
    const unsigned char *body;
    size_t length, at, reference_capacity;
    const sh_package_shader_schema *schema;
    sh_package_shader_program program;
    int status;
    const char *reason;
} ps_reader;
static int ps_fail(ps_reader *r, int status, const char *reason)
{ if (r->status == SH_PACKAGE_SHADER_OK) { r->status = status; r->reason = reason; } return 0; }
static int ps_take(ps_reader *r, size_t count, const unsigned char **out)
{
    if (count > r->length - r->at) return ps_fail(r, SH_PACKAGE_SHADER_ERROR, "truncated cooked shader record");
    if (out) *out = r->body + r->at;
    r->at += count; return 1;
}
static int ps_count(ps_reader *r, uint32_t *out, size_t minimum)
{
    const unsigned char *p;
    if (!ps_take(r, 4, &p)) return 0;
    *out = ps_be(p);
    if (*out > INT_MAX || (minimum && *out > (r->length - r->at) / minimum))
        return ps_fail(r, SH_PACKAGE_SHADER_ERROR, "invalid cooked shader count");
    return 1;
}
static int ps_string(ps_reader *r, size_t *offset, size_t *length)
{
    const unsigned char *p;
    size_t start = r->at;
    if (!ps_take(r, 4, &p)) return 0;
    uint32_t count = ps_le(p);
    if (count > INT_MAX || !ps_take(r, count, &p)) return ps_fail(r, SH_PACKAGE_SHADER_ERROR, "invalid cooked shader string length");
    if (memchr(p, 0, count)) return ps_fail(r, SH_PACKAGE_SHADER_UNSUPPORTED, "embedded NUL in cooked shader string");
    if (offset) *offset = start;
    if (length) *length = count;
    return 1;
}
static int ps_reference(ps_reader *r, sh_package_shader_reference_kind kind, const char *family,
    size_t offset, size_t length, sh_package_shader_reference **out)
{
    if (out) *out = NULL;
    if (!length) return 1;
    for (size_t i = 0; i < length; i++) if (r->body[offset + 4 + i] >= 128)
        return ps_fail(r, SH_PACKAGE_SHADER_UNSUPPORTED, "non-ASCII shader dependency requires native naming coverage");
    if (r->program.reference_count == r->reference_capacity) {
        size_t next = r->reference_capacity ? r->reference_capacity * 2 : 32;
        if (next <= r->reference_capacity || next > SIZE_MAX / sizeof(*r->program.references))
            return ps_fail(r, SH_PACKAGE_SHADER_ERROR, "shader dependency allocation overflow");
        void *grown = realloc(r->program.references, next * sizeof(*r->program.references));
        if (!grown) return ps_fail(r, SH_PACKAGE_SHADER_ERROR, "shader dependency allocation failed");
        r->program.references = grown; r->reference_capacity = next;
    }
    char *name = malloc(length + 1);
    if (!name) return ps_fail(r, SH_PACKAGE_SHADER_ERROR, "shader dependency allocation failed");
    memcpy(name, r->body + offset + 4, length); name[length] = 0;
    sh_package_shader_reference *ref = r->program.references + r->program.reference_count++;
    *ref = (sh_package_shader_reference){kind, family, name, offset, length + 4, 0};
    if (out) *out = ref;
    return 1;
}
static int ps_named(ps_reader *r, sh_package_shader_reference_kind kind, const char *family,
    sh_package_shader_reference **out)
{
    size_t offset, length;
    return ps_string(r, &offset, &length) && ps_reference(r, kind, family, offset, length, out);
}
static int ps_parameter(ps_reader *r, int *kind)
{
    sh_package_shader_reference *ref;
    if (!ps_named(r, SH_PACKAGE_SHADER_PARAMETER, "renderparm", &ref)) return 0;
    if (!ref) return ps_fail(r, SH_PACKAGE_SHADER_ERROR, "empty shader parameter name");
    if (!r->schema || !r->schema->parameter_kind)
        return ps_fail(r, SH_PACKAGE_SHADER_UNSUPPORTED, "source-bound shader parameter kinds are unavailable");
    int status = r->schema->parameter_kind(r->schema->context, ref->name, kind);
    if (status < 0) return ps_fail(r, SH_PACKAGE_SHADER_ERROR, "shader parameter schema lookup failed");
    if (!status || *kind < 0 || *kind > 18)
        return ps_fail(r, SH_PACKAGE_SHADER_UNSUPPORTED, "shader parameter has no verified native kind");
    return 1;
}
static int ps_parameter_block(ps_reader *r)
{
    const unsigned char *flags;
    uint32_t count;
    if (!ps_take(r, 1, NULL) || !ps_count(r, &count, 6)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        int kind = -1;
        if (!ps_take(r, 2, &flags) || !ps_parameter(r, &kind)) return 0;
        unsigned operation = flags[0];
        if (operation > 31 || flags[1] > 15)
            return ps_fail(r, SH_PACKAGE_SHADER_UNSUPPORTED, "shader parameter flags exceed the verified native fields");
        if (!(operation & 0x1d)) {
            if (!kind) { if (!ps_take(r, 16, NULL)) return 0; }
            else if (kind <= 11) {
                const char *family = kind <= 9 ? "image" : kind == 10 ? "sampler" : "renderprog";
                if (!ps_named(r, SH_PACKAGE_SHADER_RESOURCE, family, NULL)) return 0;
            } else if (kind <= 13) {
                size_t length;
                if (!ps_string(r, NULL, &length)) return 0;
                if (kind == 13 && length) return ps_fail(r, SH_PACKAGE_SHADER_UNSUPPORTED, "nonempty shader buffer object has no portable binding");
            } else return ps_fail(r, SH_PACKAGE_SHADER_UNSUPPORTED, "compiled shader constant kind has no verified value record");
        } else {
            if (operation == 20) {
                if (!ps_named(r, SH_PACKAGE_SHADER_RESOURCE, "table", NULL)) return 0;
            } else {
                if ((kind >= 1 && kind <= 9) || kind == 11) { if (!ps_string(r, NULL, NULL)) return 0; }
                else if (!ps_named(r, SH_PACKAGE_SHADER_PARAMETER, "renderparm", NULL)) return 0;
                if (operation == 4 && !ps_take(r, 16, NULL)) return 0;
            }
            if (!ps_named(r, SH_PACKAGE_SHADER_PARAMETER, "renderparm", NULL)) return 0;
        }
    }
    return 1;
}
static int ps_decode(ps_reader *r)
{
    const unsigned char *p;
    uint32_t count;
    if (!ps_take(r, 8, &p)) return 0;
    if (ps_be(p) != UINT32_C(0xb68800ad)) return ps_fail(r, SH_PACKAGE_SHADER_UNSUPPORTED, "unsupported cooked shader format");
    r->program.declaration_checksum = ps_be(p + 4);
    if (!ps_take(r, 4, NULL) || !ps_named(r, SH_PACKAGE_SHADER_RESOURCE, "renderprog", NULL) ||
        !ps_take(r, 8, NULL) || !ps_parameter_block(r)) return 0;
    for (unsigned stage = 0; stage < 3; stage++) for (unsigned group = 0; group < 7; group++) {
        if (!ps_count(r, &count, 4)) return 0;
        for (uint32_t i = 0; i < count; i++) {
            sh_package_shader_reference *ref;
            if (!ps_named(r, SH_PACKAGE_SHADER_PARAMETER, "renderparm", &ref)) return 0;
            if (!ref) return ps_fail(r, SH_PACKAGE_SHADER_ERROR, "empty shader parameter name");
        }
    }
    if (!ps_count(r, &count, 8)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        sh_package_shader_reference *ref;
        if (!ps_named(r, SH_PACKAGE_SHADER_INCLUDE, NULL, &ref) || !ps_take(r, 4, &p)) return 0;
        if (!ref) return ps_fail(r, SH_PACKAGE_SHADER_ERROR, "empty shader include path");
        ref->source_stamp = ps_be(p);
    }
    if (!ps_count(r, &count, 4)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        sh_package_shader_reference *ref;
        if (!ps_named(r, SH_PACKAGE_SHADER_FLAG, "renderprogflag", &ref)) return 0;
        if (!ref) return ps_fail(r, SH_PACKAGE_SHADER_ERROR, "empty shader flag name");
    }
    if (!ps_take(r, 8, &p)) return 0;
    r->program.permutation_mask = (uint64_t)ps_be(p) << 32 | ps_be(p + 4);
    if (!ps_count(r, &count, 8)) return 0;
    if (count > SIZE_MAX / sizeof(*r->program.permutations)) return ps_fail(r, SH_PACKAGE_SHADER_ERROR, "shader permutation allocation overflow");
    r->program.permutations = count ? malloc((size_t)count * sizeof(*r->program.permutations)) : NULL;
    if (count && !r->program.permutations) return ps_fail(r, SH_PACKAGE_SHADER_ERROR, "shader permutation allocation failed");
    r->program.permutation_count = count;
    for (uint32_t i = 0; i < count; i++) {
        if (!ps_take(r, 8, &p)) return 0;
        r->program.permutations[i] = (uint64_t)ps_be(p) << 32 | ps_be(p + 4);
    }
    /* Paired readers consume scalar render state, two 21-word tables, then
     * each stage's two words, short and source. None is a resource-name site. */
    if (!ps_take(r, 192, NULL)) return 0;
    for (unsigned i = 0; i < 3; i++) {
        size_t offset, length;
        if (!ps_take(r, 10, NULL) || !ps_string(r, &offset, &length)) return 0;
        r->program.stages[i] = (sh_package_shader_stage){offset + 4, length,
            sh_package_shader_checksum(r->body + offset + 4, length)};
    }
    if (!ps_take(r, 36, NULL)) return 0;
    if (r->at != r->length) return ps_fail(r, SH_PACKAGE_SHADER_UNSUPPORTED, "uncovered trailing cooked shader data");
    return 1;
}
void sh_package_shader_program_clear(sh_package_shader_program *program)
{
    if (!program) return;
    for (size_t i = 0; i < program->reference_count; i++) free(program->references[i].name);
    free(program->references); free(program->permutations); memset(program, 0, sizeof(*program));
}
int sh_package_shader_decode(const unsigned char *body, size_t length,
    const sh_package_shader_schema *schema, sh_package_shader_program *out, char *error, size_t capacity)
{
    ps_reader r = {0};
    if (error && capacity) *error = 0;
    if (out) memset(out, 0, sizeof(*out));
    if (!body || !out) return ps_error(error, capacity, SH_PACKAGE_SHADER_ERROR, "invalid shader decode input");
    r.body = body; r.length = length; r.schema = schema; r.status = SH_PACKAGE_SHADER_OK;
    if (!ps_decode(&r)) {
        sh_package_shader_program_clear(&r.program);
        return ps_error(error, capacity, r.status, r.reason ? r.reason : "shader decode failed");
    }
    *out = r.program; return SH_PACKAGE_SHADER_OK;
}

int sh_package_shader_rebind(const unsigned char *body, size_t length,
    const sh_package_shader_schema *schema, sh_package_shader_source original_decl,
    sh_package_shader_source linked_decl, sh_package_shader_resolver resolver, void *context,
    unsigned char **output, size_t *output_length, size_t *changed, char *error, size_t capacity)
{
    sh_package_shader_program program = {0};
    char **replacements = NULL;
    unsigned char *result = NULL;
    size_t result_length = length, changes = 0, read_at = 0, write_at = 0;
    int status = SH_PACKAGE_SHADER_ERROR;
    if (output) *output = NULL;
    if (output_length) *output_length = 0;
    if (changed) *changed = 0;
    if (!output || !output_length || !changed || !original_decl.body || !linked_decl.body)
        return ps_error(error, capacity, status, "invalid shader relocation input");
    status = sh_package_shader_decode(body, length, schema, &program, error, capacity);
    if (status != SH_PACKAGE_SHADER_OK) return status;
    if (program.declaration_checksum != sh_package_shader_declaration_checksum(original_decl.body, original_decl.length)) {
        status = ps_error(error, capacity, SH_PACKAGE_SHADER_UNSUPPORTED, "cooked shader does not match its source declaration"); goto done;
    }
    if (program.reference_count > SIZE_MAX / sizeof(*replacements) ||
        !(replacements = calloc(program.reference_count ? program.reference_count : 1, sizeof(*replacements)))) {
        status = ps_error(error, capacity, SH_PACKAGE_SHADER_ERROR, "shader relocation allocation failed"); goto done;
    }
    for (size_t i = 0; resolver && i < program.reference_count; i++) {
        sh_package_shader_reference *ref = program.references + i;
        const char *replacement = NULL;
        int action = resolver(context, ref, &replacement);
        if (action < 0) { status = ps_error(error, capacity, SH_PACKAGE_SHADER_ERROR, "shader binding resolution failed"); goto done; }
        if (!action) continue;
        if (!replacement || !*replacement) { status = ps_error(error, capacity, SH_PACKAGE_SHADER_ERROR, "empty shader replacement identity"); goto done; }
        if (!strcmp(ref->name, replacement)) continue;
        if (ref->kind != SH_PACKAGE_SHADER_RESOURCE) {
            status = ps_error(error, capacity, SH_PACKAGE_SHADER_UNSUPPORTED,
                ref->kind == SH_PACKAGE_SHADER_INCLUDE ? "shader include relocation needs source/timestamp coverage" :
                ref->kind == SH_PACKAGE_SHADER_FLAG ? "shader flag relocation needs native mask ABI coverage" :
                "shader parameter relocation needs native/GPU ABI coverage"); goto done;
        }
        size_t n = strlen(replacement);
        if (n > INT_MAX || n > SIZE_MAX - 4 || result_length < ref->length ||
            n + 4 > SIZE_MAX - (result_length - ref->length)) {
            status = ps_error(error, capacity, SH_PACKAGE_SHADER_ERROR, "shader replacement length overflow"); goto done;
        }
        for (size_t j = 0; j < n; j++) if ((unsigned char)replacement[j] >= 128) {
            status = ps_error(error, capacity, SH_PACKAGE_SHADER_UNSUPPORTED, "shader replacement has unverified native name bytes"); goto done;
        }
        replacements[i] = malloc(n + 1);
        if (!replacements[i]) { status = ps_error(error, capacity, SH_PACKAGE_SHADER_ERROR, "shader relocation allocation failed"); goto done; }
        memcpy(replacements[i], replacement, n + 1); result_length = result_length - ref->length + n + 4; changes++;
    }
    result = malloc(result_length ? result_length : 1);
    if (!result) { status = ps_error(error, capacity, SH_PACKAGE_SHADER_ERROR, "shader relocation allocation failed"); goto done; }
    for (size_t i = 0; i < program.reference_count; i++) if (replacements[i]) {
        sh_package_shader_reference *ref = program.references + i;
        size_t n = strlen(replacements[i]), kept = ref->offset - read_at;
        memcpy(result + write_at, body + read_at, kept); write_at += kept;
        ps_put_le(result + write_at, (uint32_t)n); write_at += 4;
        memcpy(result + write_at, replacements[i], n); write_at += n;
        read_at = ref->offset + ref->length;
    }
    memcpy(result + write_at, body + read_at, length - read_at);
    uint32_t checksum = sh_package_shader_declaration_checksum(linked_decl.body, linked_decl.length);
    if (checksum != program.declaration_checksum) { ps_put_be(result + 4, checksum); changes++; }
    *output = result; *output_length = result_length; *changed = changes;
    result = NULL; status = SH_PACKAGE_SHADER_OK;
done:
    if (replacements) for (size_t i = 0; i < program.reference_count; i++) free(replacements[i]);
    free(replacements); free(result); sh_package_shader_program_clear(&program); return status;
}

int sh_package_shader_spirv(const unsigned char *body, size_t length,
    const unsigned char *source, size_t source_length, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    if (!body || !source || !source_length || length < 36)
        return ps_error(error, capacity, SH_PACKAGE_SHADER_ERROR, "truncated Vulkan shader envelope");
    if (ps_be(body) != 4 || ps_be(body + 4) != sh_package_shader_checksum(source, source_length))
        return ps_error(error, capacity, SH_PACKAGE_SHADER_UNSUPPORTED, "Vulkan shader version/source checksum does not match");
    uint32_t bytes = ps_be(body + 12);
    if (bytes != length - 16 || bytes < 20 || bytes % 4 || ps_le(body + 16) != UINT32_C(0x07230203) ||
        ps_le(body + 20) != UINT32_C(0x00010000) || !ps_le(body + 28) || ps_le(body + 32))
        return ps_error(error, capacity, SH_PACKAGE_SHADER_UNSUPPORTED, "unsupported Vulkan SPIR-V envelope");
    for (size_t at = 36; at < length;) {
        size_t words = ps_le(body + at) >> 16;
        if (!words || words > (length - at) / 4)
            return ps_error(error, capacity, SH_PACKAGE_SHADER_ERROR, "truncated Vulkan SPIR-V instruction");
        at += words * 4;
    }
    return SH_PACKAGE_SHADER_OK;
}

static int ps_path_name(const char *name)
{
    if (!name || !*name || *name == '/') return 0;
    const char *segment = name;
    for (const char *p = name;; p++) {
        unsigned char c = (unsigned char)*p;
        if (!c || c == '/') {
            size_t n = (size_t)(p - segment);
            if (!n || (n == 1 && *segment == '.') || (n == 2 && !memcmp(segment, "..", 2))) return 0;
            if (!c) return 1;
            segment = p + 1;
        } else if (c < 32 || c >= 128 || c == '\\' || c == ':' || c == '"') return 0;
    }
}
static int ps_format(char out[256], const char *prefix, const char *name, const char *suffix)
{
    int n = snprintf(out, 256, "%s%s%s", prefix, name, suffix);
    return n >= 0 && n < 256;
}
static int ps_stem(const char *name, char out[256])
{
    if (!ps_format(out, "", name, "")) return 0;
    char *slash = strrchr(out, '/'), *dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash)) *dot = 0;
    return *out != 0;
}
static int ps_decl_path(const char *name, char out[256])
{
    static const char prefix[] = "generated/renderprogs/permutations/";
    size_t n = strlen(name);
    if (!strncmp(name, prefix, sizeof(prefix) - 1) && n > 5 && !strcmp(name + n - 5, ".decl"))
        return ps_format(out, "", name, "");
    return ps_format(out, "generated/decls/renderprog/", name, ".decl");
}
static int ps_binary_path(sh_package_shader_renderer renderer, const char *name, char out[256])
{
    char stem[256];
    const char *relative = !strncmp(name, "generated/renderprogs/", 22) ? name + 22 : name;
    return ps_stem(relative, stem) && ps_format(out, "generated/renderprogs/", stem,
        renderer == SH_PACKAGE_SHADER_VULKAN ? "_pc_vulkan.bin" : "_pc.bin");
}
static int ps_module_path(const char *name, unsigned stage, char out[256])
{
    static const char *const extensions[] = {".vspv", ".fspv", ".cspv"};
    return sh_native_generated_filename("generated/spirv/", name, extensions[stage], out, 256);
}
static int ps_emit_path(sh_package_shader_path_visitor visitor, void *context,
    sh_package_shader_path_kind kind, const char *original, const char *linked, char *error, size_t capacity)
{
    if (!visitor(context, kind, original, linked)) {
        /* Keep the visitor's specific reason when it gave one. */
        if (error && capacity && *error) return SH_PACKAGE_SHADER_ERROR;
        return ps_error(error, capacity, SH_PACKAGE_SHADER_ERROR, "shader companion visitor rejected a path");
    }
    return SH_PACKAGE_SHADER_OK;
}
int sh_package_shader_paths(sh_package_shader_renderer renderer,
    const char *name, const char *linked_name, const sh_package_shader_program *program,
    sh_package_shader_path_visitor visitor, void *context, char *error, size_t capacity)
{
    char original[256], linked[256];
    if (error && capacity) *error = 0;
    if (!program || !visitor || !ps_path_name(name) || !ps_path_name(linked_name) ||
        (renderer != SH_PACKAGE_SHADER_OPENGL && renderer != SH_PACKAGE_SHADER_VULKAN) ||
        (program->permutation_count && !program->permutations) || (program->reference_count && !program->references))
        return ps_error(error, capacity, SH_PACKAGE_SHADER_ERROR, "invalid shader companion input");
    if (!ps_decl_path(name, original) || !ps_decl_path(linked_name, linked)) goto bound;
    if (ps_emit_path(visitor, context, SH_PACKAGE_SHADER_DECLARATION, original, linked, error, capacity) < 1) return SH_PACKAGE_SHADER_ERROR;
    if (!ps_binary_path(renderer, name, original) || !ps_binary_path(renderer, linked_name, linked)) goto bound;
    if (ps_emit_path(visitor, context, SH_PACKAGE_SHADER_BINARY, original, linked, error, capacity) < 1) return SH_PACKAGE_SHADER_ERROR;
    for (unsigned i = 0; i < 3; i++) if (renderer == SH_PACKAGE_SHADER_VULKAN && program->stages[i].length) {
        if (!ps_module_path(name, i, original) || !ps_module_path(linked_name, i, linked)) goto bound;
        if (ps_emit_path(visitor, context, SH_PACKAGE_SHADER_SPIRV, original, linked, error, capacity) < 1) return SH_PACKAGE_SHADER_ERROR;
    }
    for (size_t i = 0; i < program->permutation_count; i++) {
        uint64_t mask = program->permutations[i];
        int a = snprintf(original, sizeof(original), "generated/renderprogs/permutations/%s_%08x%08x.decl",
            name, (unsigned)(mask >> 32), (unsigned)mask);
        int b = snprintf(linked, sizeof(linked), "generated/renderprogs/permutations/%s_%08x%08x.decl",
            linked_name, (unsigned)(mask >> 32), (unsigned)mask);
        if (a < 0 || a >= (int)sizeof(original) || b < 0 || b >= (int)sizeof(linked)) goto bound;
        if (ps_emit_path(visitor, context, SH_PACKAGE_SHADER_PERMUTATION, original, linked, error, capacity) < 1) return SH_PACKAGE_SHADER_ERROR;
    }
    for (size_t i = 0; i < program->reference_count; i++) {
        const sh_package_shader_reference *ref = program->references + i;
        if (ref->kind != SH_PACKAGE_SHADER_INCLUDE) continue;
        if (!ps_path_name(ref->name)) return ps_error(error, capacity, SH_PACKAGE_SHADER_UNSUPPORTED, "shader include has no supported source path");
        if (ps_emit_path(visitor, context, SH_PACKAGE_SHADER_SOURCE_INCLUDE, ref->name, ref->name, error, capacity) < 1) return SH_PACKAGE_SHADER_ERROR;
    }
    return SH_PACKAGE_SHADER_OK;
bound:
    return ps_error(error, capacity, SH_PACKAGE_SHADER_UNSUPPORTED, "shader companion exceeds the native 256-byte path buffer");
}
