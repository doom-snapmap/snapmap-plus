/* prefabpreview.c -- see prefabpreview.h. Pure file/codec work on one bounded worker thread. */
#include <windows.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prefabpreview.h"
#include "imgpreview.h"
#include "backend_log.h"

#define PP_MAX_SOURCE_BYTES (64u * 1024u * 1024u)
#define PP_MAX_VERTICES     250000u
#define PP_MAX_INDICES      750000u
#define PP_MAX_SURFACES     512u
#define PP_NAME_CAP         512u
#define PP_REQUEST_CAP      128u
#define PP_RESULT_CAP       32u

typedef struct pp_vertex {
    float x, y, z;
    unsigned char normal[4];
} pp_vertex;

typedef struct pp_mesh {
    pp_vertex *vertices;
    uint32_t *indices;
    uint32_t vertex_count, vertex_capacity;
    uint32_t index_count, index_capacity;
    float bounds[6];
    int have_bounds;
} pp_mesh;

typedef struct pp_reader {
    const unsigned char *data;
    size_t len, pos;
} pp_reader;

typedef struct pp_request {
    unsigned long generation;
    char name[PP_NAME_CAP];
} pp_request;

typedef struct pp_result {
    unsigned char *blob;
    int bytes;
} pp_result;

static CRITICAL_SECTION g_pp_lock;
static HANDLE g_pp_event;
static HANDLE g_pp_thread;
static int g_pp_installed;
static unsigned long g_pp_generation;
static pp_request g_pp_requests[PP_REQUEST_CAP];
static unsigned g_pp_request_head, g_pp_request_count;
static pp_result g_pp_results[PP_RESULT_CAP];
static unsigned g_pp_result_head, g_pp_result_count;

typedef char pp_vertex_size_must_be_16[(sizeof(pp_vertex) == SH_PREFAB_MESH_VERTEX_STRIDE) ? 1 : -1];
typedef char pp_header_size_must_be_56[(sizeof(sh_prefab_mesh_blob_header) == 56) ? 1 : -1];

static uint16_t pp_be16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t pp_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t pp_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float pp_bef(const unsigned char *p)
{
    uint32_t bits = pp_be32(p);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static int pp_take(pp_reader *r, size_t bytes, const unsigned char **out)
{
    if (!r || bytes > r->len - r->pos) return 0;
    if (out) *out = r->data + r->pos;
    r->pos += bytes;
    return 1;
}

static int pp_u8(pp_reader *r, unsigned *out)
{
    const unsigned char *p;
    if (!pp_take(r, 1, &p)) return 0;
    *out = p[0];
    return 1;
}

static int pp_u16be(pp_reader *r, unsigned *out)
{
    const unsigned char *p;
    if (!pp_take(r, 2, &p)) return 0;
    *out = pp_be16(p);
    return 1;
}

static int pp_u32be(pp_reader *r, uint32_t *out)
{
    const unsigned char *p;
    if (!pp_take(r, 4, &p)) return 0;
    *out = pp_be32(p);
    return 1;
}

static int pp_idstr(pp_reader *r, const unsigned char **out, uint32_t *out_len)
{
    const unsigned char *p;
    if (!pp_take(r, 4, &p)) return 0;
    uint32_t n = pp_le32(p);
    if (n > 1024u * 1024u || !pp_take(r, n, out)) return 0;
    if (out_len) *out_len = n;
    return 1;
}

static void pp_mesh_free(pp_mesh *m)
{
    if (!m) return;
    free(m->vertices);
    free(m->indices);
    memset(m, 0, sizeof *m);
}

static int pp_reserve(pp_mesh *m, uint32_t add_vertices, uint32_t add_indices)
{
    if (!m || add_vertices > PP_MAX_VERTICES - m->vertex_count ||
        add_indices > PP_MAX_INDICES - m->index_count) return 0;
    uint32_t want_v = m->vertex_count + add_vertices;
    uint32_t want_i = m->index_count + add_indices;
    if (want_v > m->vertex_capacity) {
        uint32_t cap = m->vertex_capacity ? m->vertex_capacity : 256u;
        while (cap < want_v && cap < PP_MAX_VERTICES / 2u) cap *= 2u;
        if (cap < want_v) cap = want_v;
        pp_vertex *larger = (pp_vertex *)realloc(m->vertices, (size_t)cap * sizeof *larger);
        if (!larger) return 0;
        m->vertices = larger;
        m->vertex_capacity = cap;
    }
    if (want_i > m->index_capacity) {
        uint32_t cap = m->index_capacity ? m->index_capacity : 768u;
        while (cap < want_i && cap < PP_MAX_INDICES / 2u) cap *= 2u;
        if (cap < want_i) cap = want_i;
        uint32_t *larger = (uint32_t *)realloc(m->indices, (size_t)cap * sizeof *larger);
        if (!larger) return 0;
        m->indices = larger;
        m->index_capacity = cap;
    }
    return 1;
}

static int pp_add_vertex(pp_mesh *m, const unsigned char *disk_vertex)
{
    float x = pp_bef(disk_vertex), y = pp_bef(disk_vertex + 4), z = pp_bef(disk_vertex + 8);
    if (!_finite(x) || !_finite(y) || !_finite(z)) return 0;
    pp_vertex *v = &m->vertices[m->vertex_count++];
    v->x = x; v->y = y; v->z = z;
    memcpy(v->normal, disk_vertex + 0x14, 4);
    if (!m->have_bounds) {
        m->bounds[0] = m->bounds[3] = x;
        m->bounds[1] = m->bounds[4] = y;
        m->bounds[2] = m->bounds[5] = z;
        m->have_bounds = 1;
    } else {
        if (x < m->bounds[0]) m->bounds[0] = x;
        if (y < m->bounds[1]) m->bounds[1] = y;
        if (z < m->bounds[2]) m->bounds[2] = z;
        if (x > m->bounds[3]) m->bounds[3] = x;
        if (y > m->bounds[4]) m->bounds[4] = y;
        if (z > m->bounds[5]) m->bounds[5] = z;
    }
    return 1;
}

static int pp_decode_bmodel(const unsigned char *data, size_t len, pp_mesh *mesh)
{
    if (!data || len < 12 || memcmp(data, "\x1b\x4c\x4d\x42", 4) != 0) return 0;
    pp_reader r = { data, len, 8 };
    uint32_t surfaces = 0;
    if (!pp_u32be(&r, &surfaces) || surfaces == 0 || surfaces > PP_MAX_SURFACES) return 0;

    for (uint32_t s = 0; s < surfaces; ++s) {
        const unsigned char *p;
        if (!pp_take(&r, 4, &p)) return 0;
        uint32_t material_len = pp_le32(p);
        if (material_len > 1024u * 1024u || !pp_take(&r, material_len, NULL) ||
            !pp_take(&r, 16, NULL)) return 0;
        uint32_t nv = 0, ni = 0;
        if (!pp_u32be(&r, &nv) || !pp_u32be(&r, &ni) || !nv || !ni || ni % 3u != 0 ||
            !pp_take(&r, 44, NULL) || !pp_reserve(mesh, nv, ni)) return 0;
        uint32_t base = mesh->vertex_count;
        for (uint32_t i = 0; i < nv; ++i) {
            if (!pp_take(&r, 48, &p) || !pp_add_vertex(mesh, p)) return 0;
        }
        for (uint32_t i = 0; i < ni; ++i) {
            unsigned local = 0;
            if (!pp_u16be(&r, &local) || local >= nv) return 0;
            mesh->indices[mesh->index_count++] = base + local;
        }
        /* Consume each surface's fixed 32-byte cooked metadata block before reading
         * the next surface. Otherwise it is mistaken for the next material-
         * string length.
         */
        if (!pp_take(&r, 32, NULL)) return 0;
    }
    /* The remaining model-level trailer is variable (bounds, joints, and other renderer metadata)
     * and is deliberately not needed for this neutral static preview. */
    return mesh->vertex_count > 0 && mesh->index_count > 0 && len - r.pos >= 24u;
}

static int pp_decode_md6(const unsigned char *data, size_t len, pp_mesh *mesh)
{
    static const unsigned char magic[4] = { 0x2b, 0x02, 0x4d, 0x4d };
    if (!data || len < 16 || memcmp(data, magic, 4) != 0) return 0;
    pp_reader r = { data, len, 4 };
    const unsigned char *p;
    uint32_t n = 0, nv = 0, nt = 0;
    unsigned small_value = 0;
    if (!pp_take(&r, 8, NULL) || !pp_idstr(&r, &p, NULL) || !pp_take(&r, 24, NULL) ||
        !pp_u8(&r, &small_value) || !pp_idstr(&r, &p, NULL) || !pp_u16be(&r, &small_value) ||
        !pp_take(&r, small_value, NULL) || !pp_take(&r, 24, NULL) || !pp_u32be(&r, &n) ||
        n > 256u) return 0;
    for (uint32_t i = 0; i < n; ++i) if (!pp_idstr(&r, &p, NULL)) return 0;
    if (!pp_take(&r, 36, NULL) || !pp_u32be(&r, &n) || n == 0 || n > PP_MAX_SURFACES) return 0;

    for (uint32_t s = 0; s < n; ++s) {
        uint32_t texsets = 0;
        if (!pp_idstr(&r, &p, NULL) || !pp_idstr(&r, &p, NULL) || !pp_u8(&r, &small_value) ||
            !pp_u32be(&r, &texsets) || !pp_u32be(&r, &nv) || !pp_u32be(&r, &nt) ||
            nt > UINT32_MAX / 3u || !pp_take(&r, 24, NULL) ||
            !pp_reserve(mesh, nv, nt * 3u)) return 0;
        uint32_t base = mesh->vertex_count;
        for (uint32_t i = 0; i < nv; ++i) {
            if (!pp_take(&r, 48, &p) || !pp_add_vertex(mesh, p)) return 0;
        }
        for (uint32_t i = 0; i < nt * 3u; ++i) {
            unsigned local = 0;
            if (!pp_u16be(&r, &local) || local >= nv) return 0;
            mesh->indices[mesh->index_count++] = base + local;
        }
        if (!pp_take(&r, 12, NULL) || !pp_u8(&r, &small_value)) return 0;
        unsigned has_morph = small_value;
        if (has_morph) {
            unsigned wide = 0;
            if (!pp_u8(&r, &wide) || nv > SIZE_MAX / (wide ? 8u : 4u) ||
                !pp_take(&r, (size_t)nv * (wide ? 8u : 4u), NULL)) return 0;
        }
        uint32_t blends = 0;
        if (!pp_u32be(&r, &blends)) return 0;
        if (blends && !has_morph) {
            if (blends > 256u || nv > SIZE_MAX / 8u / blends ||
                !pp_take(&r, (size_t)blends * nv * 8u, NULL)) return 0;
        }
    }

    if (!pp_u32be(&r, &n) || n > PP_MAX_SURFACES) return 0;
    for (uint32_t i = 0; i < n; ++i)
        if (!pp_idstr(&r, &p, NULL) || !pp_take(&r, 12, NULL)) return 0;
    if (!pp_take(&r, 4, &p) || memcmp(p, magic, 4) != 0 || r.pos != r.len) return 0;
    return mesh->vertex_count > 0 && mesh->index_count > 0;
}

static int pp_ascii_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int pp_find_decl_mesh(const unsigned char *data, size_t len, char *out, size_t cap)
{
    if (!data || !out || cap < 2) return 0;
    out[0] = '\0';
    for (size_t i = 0; i + 4u <= len; ++i) {
        if (_strnicmp((const char *)data + i, "mesh", 4) != 0) continue;
        unsigned char before = i ? data[i - 1] : ' ';
        unsigned char after = (i + 4u < len) ? data[i + 4u] : ' ';
        if ((before >= 'A' && before <= 'Z') || (before >= 'a' && before <= 'z') || before == '_' ||
            (after >= 'A' && after <= 'Z') || (after >= 'a' && after <= 'z') || after == '_') continue;
        size_t p = i + 4u;
        while (p < len && pp_ascii_space(data[p])) ++p;
        if (p < len && data[p] == '=') { ++p; while (p < len && pp_ascii_space(data[p])) ++p; }
        int quoted = (p < len && data[p] == '"');
        if (quoted) ++p;
        size_t start = p;
        while (p < len && ((quoted && data[p] != '"') ||
               (!quoted && !pp_ascii_space(data[p]) && data[p] != ';' && data[p] != '}'))) ++p;
        size_t n = p - start;
        if (n > 0 && n < cap && n >= 8u) {
            memcpy(out, data + start, n); out[n] = '\0';
            if (n >= 8u && _stricmp(out + n - 8u, ".md6mesh") == 0) return 1;
            out[0] = '\0';
        }
    }
    return 0;
}

static int pp_decl_word(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int pp_decl_token_at(const unsigned char *data, size_t len, size_t at, const char *token)
{
    size_t n = strlen(token);
    if (!data || at + n > len || _strnicmp((const char *)data + at, token, n) != 0) return 0;
    if (at && pp_decl_word(data[at - 1])) return 0;
    if (at + n < len && pp_decl_word(data[at + n])) return 0;
    return 1;
}

static int pp_decl_string_in_range(const unsigned char *data, size_t start, size_t end,
                                   const char *field, char *out, size_t cap)
{
    size_t field_len = strlen(field);
    if (!out || cap < 2 || start > end) return 0;
    out[0] = '\0';
    for (size_t i = start; i < end; ++i) {
        if (!pp_decl_token_at(data, end, i, field)) continue;
        size_t p = i + field_len;
        while (p < end && pp_ascii_space(data[p])) ++p;
        if (p < end && data[p] == '=') { ++p; while (p < end && pp_ascii_space(data[p])) ++p; }
        int quoted = p < end && data[p] == '"';
        if (quoted) ++p;
        size_t value = p;
        while (p < end && ((quoted && data[p] != '"') ||
               (!quoted && !pp_ascii_space(data[p]) && data[p] != ';' && data[p] != '}'))) ++p;
        size_t n = p - value;
        if (n > 0 && n < cap) { memcpy(out, data + value, n); out[n] = '\0'; return 1; }
    }
    return 0;
}

static int pp_decl_block_string(const unsigned char *data, size_t len, const char *block,
                                const char *field, char *out, size_t cap)
{
    size_t block_start = 0, block_end = 0;
    size_t block_len = strlen(block);
    for (size_t i = 0; i < len; ++i) {
        if (!pp_decl_token_at(data, len, i, block)) continue;
        size_t p = i + block_len;
        while (p < len && (pp_ascii_space(data[p]) || data[p] == '=')) ++p;
        if (p >= len || data[p] != '{') continue;
        block_start = ++p;
        int depth = 1, quote = 0, escape = 0;
        for (; p < len; ++p) {
            unsigned char c = data[p];
            if (quote) {
                if (escape) escape = 0;
                else if (c == '\\') escape = 1;
                else if (c == '"') quote = 0;
            } else if (c == '"') quote = 1;
            else if (c == '{') depth++;
            else if (c == '}' && --depth == 0) { block_end = p; break; }
        }
        if (block_end > block_start)
            return pp_decl_string_in_range(data, block_start, block_end, field, out, cap);
    }
    return 0;
}

static int pp_decl_block_range(const unsigned char *data, size_t start, size_t end,
                               const char *block, size_t *out_start, size_t *out_end)
{
    size_t block_len = strlen(block);
    if (!data || !out_start || !out_end || start > end) return 0;
    for (size_t i = start; i < end; ++i) {
        if (!pp_decl_token_at(data, end, i, block)) continue;
        size_t p = i + block_len;
        while (p < end && (pp_ascii_space(data[p]) || data[p] == '=')) ++p;
        if (p >= end || data[p] != '{') continue;
        size_t body = ++p;
        int depth = 1, quote = 0, escape = 0;
        for (; p < end; ++p) {
            unsigned char c = data[p];
            if (quote) {
                if (escape) escape = 0;
                else if (c == '\\') escape = 1;
                else if (c == '"') quote = 0;
            } else if (c == '"') quote = 1;
            else if (c == '{') depth++;
            else if (c == '}' && --depth == 0) {
                *out_start = body; *out_end = p; return 1;
            }
        }
    }
    return 0;
}

static int pp_decl_float_in_range(const unsigned char *data, size_t start, size_t end,
                                  const char *field, float *out)
{
    size_t field_len = strlen(field);
    for (size_t i = start; i < end; ++i) {
        if (!pp_decl_token_at(data, end, i, field)) continue;
        size_t p = i + field_len;
        while (p < end && pp_ascii_space(data[p])) ++p;
        if (p < end && data[p] == '=') { ++p; while (p < end && pp_ascii_space(data[p])) ++p; }
        size_t value = p;
        while (p < end && !pp_ascii_space(data[p]) && data[p] != ';' && data[p] != '}') ++p;
        size_t n = p - value;
        if (!n || n >= 64u) continue;
        char number[64], *tail = NULL;
        memcpy(number, data + value, n); number[n] = '\0';
        double parsed = strtod(number, &tail);
        if (tail && tail != number && *tail == '\0' && _finite(parsed) &&
            parsed >= -FLT_MAX && parsed <= FLT_MAX) {
            *out = (float)parsed; return 1;
        }
    }
    return 0;
}

static void pp_decl_render_scale(const unsigned char *data, size_t len,
                                 float scale[3], unsigned *have)
{
    size_t render_start = 0, render_end = 0, scale_start = 0, scale_end = 0;
    if (!pp_decl_block_range(data, 0, len, "renderModelInfo", &render_start, &render_end) ||
        !pp_decl_block_range(data, render_start, render_end, "scale", &scale_start, &scale_end)) return;
    static const char *fields[3] = { "x", "y", "z" };
    for (unsigned i = 0; i < 3; ++i) {
        float value = 0.0f;
        if (!(*have & (1u << i)) &&
            pp_decl_float_in_range(data, scale_start, scale_end, fields[i], &value)) {
            scale[i] = value; *have |= 1u << i;
        }
    }
}

static int pp_read_entity_decl(const char *name, unsigned char **out, size_t *out_len)
{
    if (sh_imgpreview_read_payload(SH_ASSET_SNAPDEF, name, PP_MAX_SOURCE_BYTES, out, out_len)) return 1;
    return sh_imgpreview_read_payload(SH_ASSET_ENTITYDEF, name, PP_MAX_SOURCE_BYTES, out, out_len);
}

int sh_prefabpreview_resolve_defaults(const char *inherit_name, char *out_model,
                                      size_t out_capacity, float out_scale[3])
{
    char current[PP_NAME_CAP], next[PP_NAME_CAP];
    if (!out_model || out_capacity < 2 || !out_scale) return 0;
    out_model[0] = '\0';
    out_scale[0] = out_scale[1] = out_scale[2] = 1.0f;
    if (!inherit_name || !inherit_name[0] || strlen(inherit_name) >= sizeof current) return 0;
    strcpy_s(current, sizeof current, inherit_name);
    int flags = 0;
    unsigned scale_have = 0;

    /* Derived fields win: collect the first model and first occurrence of each sparse scale component
     * while walking toward the base. A spawner switches to its represented entityStatic before any
     * generic spawner render defaults can masquerade as the physical pickup. */
    for (int depth = 0; depth < 20; ++depth) {
        unsigned char *decl = NULL; size_t len = 0;
        if (!pp_read_entity_decl(current, &decl, &len)) break;
        next[0] = '\0';
        if (pp_decl_block_string(decl, len, "spawnerEntityPair", "entityStatic",
                                 next, sizeof next)) {
            /* Discard any generic spawner defaults gathered before reaching the semantic payload. */
            flags = 0; scale_have = 0; out_model[0] = '\0';
            out_scale[0] = out_scale[1] = out_scale[2] = 1.0f;
            free(decl);
            if (!next[0] || _stricmp(next, current) == 0) break;
            strcpy_s(current, sizeof current, next);
            continue;
        }

        if (!(flags & SH_PREFAB_DEFAULT_MODEL) &&
            pp_decl_block_string(decl, len, "renderModelInfo", "model",
                                 out_model, out_capacity)) flags |= SH_PREFAB_DEFAULT_MODEL;
        pp_decl_render_scale(decl, len, out_scale, &scale_have);
        int inherited = pp_decl_string_in_range(decl, 0, len, "inherit", next, sizeof next);
        free(decl);
        if (!inherited || !next[0] || _stricmp(next, current) == 0) break;
        strcpy_s(current, sizeof current, next);
    }
    if (scale_have) flags |= SH_PREFAB_DEFAULT_SCALE;
    return flags;
}

int sh_prefabpreview_resolve_model(const char *inherit_name, char *out_model, size_t out_capacity)
{
    float scale[3];
    return (sh_prefabpreview_resolve_defaults(inherit_name, out_model, out_capacity, scale) &
            SH_PREFAB_DEFAULT_MODEL) != 0;
}

static int pp_read_logical_model(const char *name, unsigned char **out, size_t *out_len)
{
    int kinds[3] = { SH_ASSET_MODEL, SH_ASSET_BMODEL, SH_ASSET_MODULE };
    for (int i = 0; i < 3; ++i)
        if (sh_imgpreview_read_payload(kinds[i], name, PP_MAX_SOURCE_BYTES, out, out_len)) return 1;
    return sh_imgpreview_read_payload(SH_IMGPREVIEW_BASEMODEL_KIND, name,
                                      PP_MAX_SOURCE_BYTES, out, out_len);
}

static int pp_decode_payload(const unsigned char *data, size_t len, pp_mesh *mesh)
{
    pp_mesh trial;
    memset(&trial, 0, sizeof trial);
    if (pp_decode_bmodel(data, len, &trial)) {
        pp_mesh_free(mesh); *mesh = trial; return 1;
    }
    pp_mesh_free(&trial);
    if (pp_decode_md6(data, len, &trial)) {
        pp_mesh_free(mesh); *mesh = trial; return 1;
    }
    pp_mesh_free(&trial);
    return 0;
}

static int pp_load_model(const char *name, pp_mesh *mesh)
{
    unsigned char *body = NULL, *cooked = NULL;
    size_t len = 0, cooked_len = 0;
    int ok = 0;
    if (!pp_read_logical_model(name, &body, &len)) return 0;
    if (pp_decode_payload(body, len, mesh)) {
        ok = 1;
    } else {
        char mesh_name[PP_NAME_CAP];
        if (pp_find_decl_mesh(body, len, mesh_name, sizeof mesh_name) &&
            sh_imgpreview_read_payload(SH_IMGPREVIEW_BASEMODEL_KIND, mesh_name,
                                       PP_MAX_SOURCE_BYTES, &cooked, &cooked_len)) {
            ok = pp_decode_payload(cooked, cooked_len, mesh);
        }
    }
    free(cooked);
    free(body);
    if (!ok) pp_mesh_free(mesh);
    return ok;
}

static int pp_build_blob(unsigned long generation, const char *name, const pp_mesh *mesh,
                         unsigned char **out_blob, int *out_bytes)
{
    size_t name_len = strlen(name);
    size_t name_padded = (name_len + 3u) & ~(size_t)3u;
    size_t vertices = mesh ? (size_t)mesh->vertex_count * sizeof(pp_vertex) : 0;
    size_t indices = mesh ? (size_t)mesh->index_count * sizeof(uint32_t) : 0;
    size_t total = sizeof(sh_prefab_mesh_blob_header) + name_padded + vertices + indices;
    if (name_len >= PP_NAME_CAP || total > INT_MAX) return 0;
    unsigned char *blob = (unsigned char *)calloc(1, total);
    if (!blob) return 0;
    sh_prefab_mesh_blob_header *h = (sh_prefab_mesh_blob_header *)blob;
    h->magic = SH_PREFAB_MESH_MAGIC;
    h->version = SH_PREFAB_MESH_VERSION;
    h->status = mesh ? SH_PREFAB_MESH_OK : 0;
    h->generation = (uint32_t)generation;
    h->name_bytes = (uint32_t)name_len;
    h->vertex_count = mesh ? mesh->vertex_count : 0;
    h->index_count = mesh ? mesh->index_count : 0;
    h->vertex_stride = SH_PREFAB_MESH_VERTEX_STRIDE;
    if (mesh) memcpy(h->bounds, mesh->bounds, sizeof h->bounds);
    memcpy(blob + sizeof *h, name, name_len);
    if (mesh) {
        unsigned char *dst = blob + sizeof *h + name_padded;
        memcpy(dst, mesh->vertices, vertices);
        memcpy(dst + vertices, mesh->indices, indices);
    }
    *out_blob = blob;
    *out_bytes = (int)total;
    return 1;
}

static void pp_clear_results_locked(void)
{
    while (g_pp_result_count) {
        pp_result *r = &g_pp_results[g_pp_result_head];
        free(r->blob); r->blob = NULL; r->bytes = 0;
        g_pp_result_head = (g_pp_result_head + 1u) % PP_RESULT_CAP;
        g_pp_result_count--;
    }
    g_pp_result_head = 0;
}

static void pp_publish_result(unsigned long generation, unsigned char *blob, int bytes)
{
    EnterCriticalSection(&g_pp_lock);
    if (generation != g_pp_generation || !blob || bytes <= 0) {
        LeaveCriticalSection(&g_pp_lock);
        free(blob);
        return;
    }
    if (g_pp_result_count == PP_RESULT_CAP) {
        pp_result *old = &g_pp_results[g_pp_result_head];
        free(old->blob); old->blob = NULL; old->bytes = 0;
        g_pp_result_head = (g_pp_result_head + 1u) % PP_RESULT_CAP;
        g_pp_result_count--;
    }
    unsigned tail = (g_pp_result_head + g_pp_result_count) % PP_RESULT_CAP;
    g_pp_results[tail].blob = blob;
    g_pp_results[tail].bytes = bytes;
    g_pp_result_count++;
    LeaveCriticalSection(&g_pp_lock);
}

static DWORD WINAPI pp_worker(void *unused)
{
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_pp_event, INFINITE);
        for (;;) {
            pp_request req;
            int have = 0;
            EnterCriticalSection(&g_pp_lock);
            if (g_pp_request_count) {
                req = g_pp_requests[g_pp_request_head];
                g_pp_request_head = (g_pp_request_head + 1u) % PP_REQUEST_CAP;
                g_pp_request_count--;
                have = 1;
            }
            LeaveCriticalSection(&g_pp_lock);
            if (!have) break;

            ULONGLONG started = GetTickCount64();
            pp_mesh mesh; memset(&mesh, 0, sizeof mesh);
            int ok = pp_load_model(req.name, &mesh);
            unsigned char *blob = NULL; int bytes = 0;
            if (!pp_build_blob(req.generation, req.name, ok ? &mesh : NULL, &blob, &bytes)) {
                free(blob); blob = NULL; bytes = 0;
            }
            pp_mesh_free(&mesh);
            pp_publish_result(req.generation, blob, bytes);

            char line[760];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "B2: prefabpreview -- %s '%s' in %llu ms",
                        ok ? "decoded" : "proxy fallback for", req.name,
                        (unsigned long long)(GetTickCount64() - started));
            backend_log(line);
        }
    }
}

int sh_prefabpreview_install(void)
{
    if (g_pp_installed) return g_pp_thread != NULL;
    InitializeCriticalSection(&g_pp_lock);
    g_pp_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (g_pp_event) g_pp_thread = CreateThread(NULL, 0, pp_worker, NULL, 0, NULL);
    g_pp_installed = 1;
    backend_log(g_pp_thread ? "B2: prefabpreview worker ready"
                            : "B2: prefabpreview worker unavailable; Prefabs use proxies");
    return g_pp_thread != NULL;
}

int sh_prefabpreview_request(unsigned long generation, const char *model_name)
{
    if (!g_pp_installed || !g_pp_thread) return 0;
    if (!model_name) model_name = "";
    size_t n = strlen(model_name);
    if (n >= PP_NAME_CAP) return 0;

    EnterCriticalSection(&g_pp_lock);
    if (generation != g_pp_generation) {
        g_pp_generation = generation;
        g_pp_request_head = g_pp_request_count = 0;
        pp_clear_results_locked();
    }
    if (!model_name[0]) { LeaveCriticalSection(&g_pp_lock); return 1; }
    for (unsigned i = 0; i < g_pp_request_count; ++i) {
        unsigned at = (g_pp_request_head + i) % PP_REQUEST_CAP;
        if (g_pp_requests[at].generation == generation &&
            _stricmp(g_pp_requests[at].name, model_name) == 0) {
            LeaveCriticalSection(&g_pp_lock);
            return 1;
        }
    }
    if (g_pp_request_count == PP_REQUEST_CAP) {
        LeaveCriticalSection(&g_pp_lock);
        return 0;
    }
    unsigned tail = (g_pp_request_head + g_pp_request_count) % PP_REQUEST_CAP;
    g_pp_requests[tail].generation = generation;
    memcpy(g_pp_requests[tail].name, model_name, n + 1u);
    g_pp_request_count++;
    LeaveCriticalSection(&g_pp_lock);
    SetEvent(g_pp_event);
    return 1;
}

int sh_prefabpreview_get(void *out_blob, int out_capacity)
{
    if (!g_pp_installed) return 0;
    EnterCriticalSection(&g_pp_lock);
    if (!g_pp_result_count) { LeaveCriticalSection(&g_pp_lock); return 0; }
    pp_result *r = &g_pp_results[g_pp_result_head];
    int needed = r->bytes;
    if (!out_blob || out_capacity < needed) {
        LeaveCriticalSection(&g_pp_lock);
        return -needed;
    }
    memcpy(out_blob, r->blob, (size_t)needed);
    free(r->blob); r->blob = NULL; r->bytes = 0;
    g_pp_result_head = (g_pp_result_head + 1u) % PP_RESULT_CAP;
    g_pp_result_count--;
    LeaveCriticalSection(&g_pp_lock);
    return needed;
}
