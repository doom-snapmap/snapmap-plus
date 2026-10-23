/* map_capacity: the stored size of a map against DOOM's map-text limit.
 *
 * Maps are built through the real shard envelope (map_shards.c insert, as the
 * package, binding-record and navigation writers use it), so the measured
 * parts are the bytes those writers add. The exact boundary is checked at the
 * engine's own value: 10,485,760 bytes is within, one more byte is not. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map_capacity.h"
#include "map_shards.h"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const char BASE_HEAD[] = "{\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":["
    "{\"info\":{\"name\":\"smpkg.notes\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"an author's own variable\","
    "\"~type\":\"snapVarString_t\"}]},\"entities\":[],\"message\":\"";
static const char BASE_TAIL[] = "\"}";
static const char DIGEST[] = "0123456789abcdef";

/* A map whose native part is BASE_HEAD + `padding` copies of 'x' + BASE_TAIL. */
static char *base_map(size_t padding, size_t *length)
{
    size_t head = sizeof(BASE_HEAD) - 1, tail = sizeof(BASE_TAIL) - 1;
    char *text = HeapAlloc(GetProcessHeap(), 0, head + padding + tail + 1);
    if (!text) return NULL;
    memcpy(text, BASE_HEAD, head);
    memset(text + head, 'x', padding);
    memcpy(text + head + padding, BASE_TAIL, tail + 1);
    *length = head + padding + tail;
    return text;
}

/* Insert `count` shards of `chunk` characters each under the given headers. */
static char *insert(char *text, size_t length, char (*headers)[160], size_t count, const char *chunk,
                    size_t chunk_length, size_t *out_length)
{
    sh_shard_out parts[64];
    char error[256];
    char *next;
    size_t i;
    if (count > 64) return NULL;
    for (i = 0; i < count; i++) {
        parts[i].header = headers[i];
        parts[i].chunk = chunk;
        parts[i].chunk_len = chunk_length;
    }
    next = sh_shard_insert(text, length, parts, count, out_length, error, sizeof(error));
    if (!next) fprintf(stderr, "insert refused: %s\n", error);
    HeapFree(GetProcessHeap(), 0, text);
    return next;
}

/* The complete map: native part, one 3-shard package delivery, one binding
 * record and one navigation shard. */
static char *composed(size_t padding, size_t *native_length, size_t *length)
{
    static char chunk[SH_SHARD_CHARS + 1];
    char headers[3][160];
    char *text = base_map(padding, native_length);
    size_t i;
    memset(chunk, 'A', SH_SHARD_CHARS);
    if (!text) return NULL;
    *length = *native_length;
    for (i = 0; i < 3; i++)
        snprintf(headers[i], sizeof(headers[i]), "smpkg.delivery.%064u.%u.3.%s", 7u, (unsigned)i, DIGEST);
    text = insert(text, *length, headers, 3, chunk, SH_SHARD_CHARS, length);
    if (!text) return NULL;
    snprintf(headers[0], sizeof(headers[0]), "smpbind.0.1.%s", DIGEST);
    text = insert(text, *length, headers, 1, chunk, 64, length);
    if (!text) return NULL;
    snprintf(headers[0], sizeof(headers[0]), "smpref.delivery.%064u.%s", 8u, DIGEST);
    text = insert(text, *length, headers, 1, chunk, 400, length);
    if (!text) return NULL;
    snprintf(headers[0], sizeof(headers[0]), "smnav1.modules/room.aas48.0.1.%s.g1", DIGEST);
    return insert(text, *length, headers, 1, chunk, 512, length);
}

/* A map that only refers to a package (L2) carries no delivery: the reference
 * is counted apart and the map stays carrier-free for the engine. */
static void references_count_apart(void)
{
    static char chunk[401];
    char headers[1][160], line[512];
    sh_map_capacity c;
    size_t native = 0, length = 0;
    char *text = base_map(0, &native);
    if (!text) { failures++; return; }
    memset(chunk, 'B', 400);
    snprintf(headers[0], sizeof(headers[0]), "smpref.local.pack.%s", DIGEST);
    text = insert(text, native, headers, 1, chunk, 400, &length);
    if (!text) { failures++; return; }
    CHECK(sh_map_capacity_measure(text, length, &c));
    CHECK(c.parts_known && c.reference_shards == 1 && !c.package_shards && !c.navigation_shards);
    CHECK(c.total - c.reference_bytes == native && sh_map_capacity_carrier_free(&c));
    sh_map_capacity_describe(&c, line, sizeof(line));
    CHECK(strstr(line, "references ") && strstr(line, " in 1, navigation") && strstr(line, "carries no packages"));
    HeapFree(GetProcessHeap(), 0, text);
    /* A header without the key or the digest is the author's own variable. */
    text = base_map(0, &native);
    if (!text) { failures++; return; }
    snprintf(headers[0], sizeof(headers[0]), "smpref.%s", DIGEST);
    text = insert(text, native, headers, 1, chunk, 400, &length);
    if (!text) { failures++; return; }
    CHECK(sh_map_capacity_measure(text, length, &c) && c.parts_known && !c.reference_shards);
    HeapFree(GetProcessHeap(), 0, text);
}

static void limit_defaults_and_overrides(void)
{
    CHECK(sh_map_capacity_limit() == 10485760u && SH_MAP_TEXT_LIMIT_DEFAULT == 0xA00000u);
    sh_map_capacity_set_limit(4096);
    CHECK(sh_map_capacity_limit() == 4096);
    sh_map_capacity_set_limit(0);
    CHECK(sh_map_capacity_limit() == SH_MAP_TEXT_LIMIT_DEFAULT);
}

static void plain_text_boundary(void)
{
    sh_map_capacity c;
    size_t native = 0, limit = SH_MAP_TEXT_LIMIT_DEFAULT;
    size_t overhead = sizeof(BASE_HEAD) - 1 + sizeof(BASE_TAIL) - 1;
    char *text = base_map(limit - overhead, &native);
    CHECK(!sh_map_capacity_measure(text, native, NULL));
    CHECK(sh_map_capacity_measure(NULL, 0, &c) && c.total == 0 && sh_map_capacity_within(&c));
    if (!text) { failures++; return; }
    CHECK(native == limit);
    CHECK(sh_map_capacity_measure(text, native, &c));
    CHECK(c.total == limit && c.limit == limit && sh_map_capacity_within(&c));
    /* The author's own "smpkg.notes" variable is not a delivery shard. */
    CHECK(c.parts_known && !c.package_shards && !c.navigation_shards && sh_map_capacity_carrier_free(&c));
    HeapFree(GetProcessHeap(), 0, text);
    text = base_map(limit - overhead + 1, &native);
    if (!text) { failures++; return; }
    CHECK(sh_map_capacity_measure(text, native, &c));
    CHECK(c.total == limit + 1 && !sh_map_capacity_within(&c));
    HeapFree(GetProcessHeap(), 0, text);
}

/* Composed maps: the parts are exactly what the writers added, and the
 * boundary holds for the complete stored text. */
static void composed_parts_and_boundary(void)
{
    sh_map_capacity c;
    size_t native = 0, length = 0, padding;
    char line[512];
    char *text = composed(0, &native, &length);
    if (!text) { failures++; return; }
    CHECK(sh_map_capacity_measure(text, length, &c));
    CHECK(c.parts_known && c.package_shards == 4 && c.reference_shards == 1 && c.navigation_shards == 1);
    CHECK(c.total == length && c.total - c.package_bytes - c.reference_bytes - c.navigation_bytes == native);
    CHECK(!sh_map_capacity_carrier_free(&c) && sh_map_capacity_within(&c));
    sh_map_capacity_describe(&c, line, sizeof(line));
    CHECK(strstr(line, "MAP-SIZE: saved map text is ") && strstr(line, "within DOOM's 10485760-byte map limit"));
    CHECK(!strstr(line, "carries no packages"));
    padding = SH_MAP_TEXT_LIMIT_DEFAULT - length;
    HeapFree(GetProcessHeap(), 0, text);

    text = composed(padding, &native, &length);
    if (!text) { failures++; return; }
    CHECK(length == SH_MAP_TEXT_LIMIT_DEFAULT);
    CHECK(sh_map_capacity_measure(text, length, &c) && sh_map_capacity_within(&c));
    CHECK(c.total - c.package_bytes - c.reference_bytes - c.navigation_bytes == native);
    HeapFree(GetProcessHeap(), 0, text);

    text = composed(padding + 1, &native, &length);
    if (!text) { failures++; return; }
    CHECK(length == SH_MAP_TEXT_LIMIT_DEFAULT + 1);
    CHECK(sh_map_capacity_measure(text, length, &c) && !sh_map_capacity_within(&c));
    sh_map_capacity_describe(&c, line, sizeof(line));
    CHECK(strstr(line, "1 bytes over DOOM's 10485760-byte map limit"));
    HeapFree(GetProcessHeap(), 0, text);
}

/* A running image's own bound replaces the default for later measurements. */
static void image_bound_applies(void)
{
    sh_map_capacity c;
    static const char text[] = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}";
    sh_map_capacity_set_limit(sizeof(text) - 1);
    CHECK(sh_map_capacity_measure(text, sizeof(text) - 1, &c) && sh_map_capacity_within(&c));
    sh_map_capacity_set_limit(sizeof(text) - 2);
    CHECK(sh_map_capacity_measure(text, sizeof(text) - 1, &c) && !sh_map_capacity_within(&c));
    CHECK(c.total - c.limit == 1);
    sh_map_capacity_set_limit(0);
}

/* A structure that does not read keeps an exact total and reports no parts. */
static void unreadable_structure(void)
{
    sh_map_capacity c;
    char line[512];
    static const char text[] = "{\"variables\":{\"string\":[{\"info\":{\"name\":\"smpkg.k.0.1.0123456789abcdef\"";
    CHECK(sh_map_capacity_measure(text, sizeof(text) - 1, &c));
    CHECK(c.total == sizeof(text) - 1 && !c.parts_known && !sh_map_capacity_carrier_free(&c));
    sh_map_capacity_describe(&c, line, sizeof(line));
    CHECK(strstr(line, "parts not measured"));
}

int main(void)
{
    limit_defaults_and_overrides();
    plain_text_boundary();
    composed_parts_and_boundary();
    references_count_apart();
    image_bound_applies();
    unreadable_structure();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("map capacity accounting passed");
    return 0;
}
