/* modmap_test.c -- the open map's entities, wires and Timeline references
 * (modmap.c): each entity's placed names follow the save-time closure's rule
 * (their union is exactly sh_modmanifest_placed_names over the same text),
 * the wire run-length list decodes into pairs, a reference to a set is a wire
 * with one end in it or a Timeline outside it naming one of its entities, and
 * malformed text is refused.
 *
 *   modmap_test.exe
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modmap.h"
#include "modmanifest.h"
#include "modserve.h"

static int failures, checks;
#define CHECK(expr) do { checks++; if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

void backend_log(const char *message) { (void)message; }
int modserve_activate_bundles(const char *const *bundle_hashes, int n) { (void)bundle_hashes; (void)n; return 0; }
int sh_modserve_bundle_serving(const char *bundle_hash, const char *id, sh_modserve_bundle_ref *out)
{ (void)bundle_hash; (void)id; if (out) memset(out, 0, sizeof *out); return 0; }
int sh_modserve_active(void) { return 0; }
size_t modserve_bundles_for_names_visit(const char *const *names, size_t count, sh_modserve_closure_fn visit, void *context)
{
    (void)names; (void)count; (void)visit; (void)context;
    return 0;
}

/* Two mod entities (4 and 7), a relay (5) wired to and from them, a wire
 * between the two (inside any set holding both), a Timeline (9) naming 7, an
 * encounter (8) whose entity reference names 4 (not a Timeline: not
 * reported), a module instance, and edit values that are names too. */
static const char MAP[] =
    "{\"entities\":["
    "{\"displayName\":\"\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/alex.guard/guard\","
    "\"state\":{\"edit\":{\"skin\":\"mods/alex.guard/red\",\"spawnPosition\":{\"x\":1}}}},\"uniqueId\":4},"
    "{\"displayName\":\"Relay \\\"A\\\"\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"snapmaps/logic/relay\"},"
    "\"uniqueId\":5},"
    "{\"displayName\":\"Boss \\u00e9\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/alex.guard/guard\"},"
    "\"uniqueId\":7},"
    "{\"displayName\":\"\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"snapmaps/encounter\"},\"uniqueId\":8},"
    "{\"displayName\":\"Wave 2\",\"entityDef\":{\"className\":\"idTarget_Timeline\",\"inherit\":\"snapmaps/unknown\"},"
    "\"uniqueId\":9}],"
    "\"instances\":[{\"moduleName\":\"maps/modules/mods/alex.guard/room\",\"origin\":{\"x\":0}}],"
    "\"targets\":{\"connections\":[-5,7,-6,4,-8,9],\"~type\":\"idSnapTargets\"},"
    "\"references\":{\"entityEntRefs\":{\"keyValues\":[0,0,0,0,0,0,0,0,0,1,3],\"values\":[4,-1,7]}},"
    "\"variables\":{\"allocCount\":[0,0,0,0,0]}}";

static int has_name(const sh_mm_entity *e, const char *n)
{
    size_t i;
    for (i = 0; i < e->name_count; i++) if (!strcmp(e->names[i], n)) return 1;
    return 0;
}

int main(void)
{
    sh_mm_map m;
    sh_mm_ref *refs = NULL;
    char err[160];
    size_t n, i;

    printf("-- parse: entities, names by the save rule, modules, wires, references\n");
    CHECK(sh_mm_parse(MAP, sizeof MAP - 1, &m, err, sizeof err));
    CHECK(m.ent_count == 5);
    CHECK(sh_mm_find(&m, 4) && sh_mm_find(&m, 9) && !sh_mm_find(&m, 6));
    CHECK(!strcmp(sh_mm_find(&m, 5)->label, "Relay \"A\""));
    CHECK(!strcmp(sh_mm_find(&m, 7)->label, "Boss \xc3\xa9"));
    CHECK(sh_mm_find(&m, 9)->timeline && !sh_mm_find(&m, 8)->timeline);
    CHECK(has_name(sh_mm_find(&m, 4), "mods/alex.guard/guard") && has_name(sh_mm_find(&m, 4), "mods/alex.guard/red"));
    CHECK(!has_name(sh_mm_find(&m, 4), "idSnapEntity"));   /* className is not a placed name */
    CHECK(m.module_count == 1 && !strcmp(m.modules[0], "maps/modules/mods/alex.guard/room"));
    CHECK(m.wire_count == 3);
    CHECK(m.wire_src[0] == 4 && m.wire_dst[0] == 7 && m.wire_src[1] == 5 && m.wire_dst[1] == 4);
    CHECK(m.wire_src[2] == 7 && m.wire_dst[2] == 9);
    {
        /* Every entity's names together are the names the save closure reads,
         * less the module's. */
        char **all = NULL;
        int failed = 0;
        size_t count = sh_modmanifest_placed_names(MAP, sizeof MAP - 1, &all, &failed), found = 0;
        CHECK(!failed && count == 6);
        for (i = 0; i < count; i++) {
            size_t k;
            int in = 0;
            for (k = 0; k < m.ent_count && !in; k++) in = has_name(&m.ents[k], all[i]);
            found += in || !strcmp(all[i], "maps/modules/mods/alex.guard/room");
        }
        CHECK(found == count);
        sh_modmanifest_names_free(all, count);
    }

    printf("-- references to a set: wires with one end in it, Timelines naming it\n");
    {
        /* 4 -> 7 is inside the set; 5 -> 4 and 7 -> 9 cross it; Timeline 9
         * names 7; encounter 8 names 4 but is not a Timeline: not reported. */
        int set[] = { 4, 7 };
        n = sh_mm_references(&m, set, 2, &refs);
        CHECK(n == 3);
        CHECK(n == 3 && refs[0].kind == SH_MM_REF_WIRE && refs[0].from_uid == 5 && refs[0].to_uid == 4);
        CHECK(n == 3 && refs[1].kind == SH_MM_REF_WIRE && refs[1].from_uid == 7 && refs[1].to_uid == 9);
        CHECK(n == 3 && refs[2].kind == SH_MM_REF_TIMELINE && refs[2].from_uid == 9 && refs[2].to_uid == 7);
        free(refs);
    }
    {
        int set[] = { 5 };
        n = sh_mm_references(&m, set, 1, &refs);
        CHECK(n == 1 && refs[0].from_uid == 5 && refs[0].to_uid == 4);
        free(refs);
    }
    sh_mm_free(&m);

    /* The relay wired to a mod entity, both ways. */
    {
        static const char W[] =
            "{\"entities\":[{\"entityDef\":{\"inherit\":\"a\"},\"uniqueId\":0},{\"entityDef\":{\"inherit\":\"b\"},\"uniqueId\":1},"
            "{\"entityDef\":{\"inherit\":\"c\"},\"uniqueId\":2}],\"targets\":{\"connections\":[-1,1,-3,1,0]}}";
        int set[] = { 1 };
        CHECK(sh_mm_parse(W, sizeof W - 1, &m, err, sizeof err));
        n = sh_mm_references(&m, set, 1, &refs);
        CHECK(n == 2 && refs[0].kind == SH_MM_REF_WIRE && refs[0].from_uid == 0 && refs[0].to_uid == 1 &&
              refs[1].from_uid == 2 && refs[1].to_uid == 1);
        free(refs);
        CHECK(m.ref_kv_n == 0 && m.module_count == 0);   /* absent sections read as none */
        sh_mm_free(&m);
    }

    printf("-- refused: malformed text, no entities, a bad wire list\n");
    CHECK(!sh_mm_parse("{\"entities\":[", 13, &m, err, sizeof err) && m.ent_count == 0);
    CHECK(!sh_mm_parse("{}", 2, &m, err, sizeof err) && strstr(err, "entities"));
    CHECK(!sh_mm_parse("{\"entities\":[],\"targets\":{\"connections\":[\"x\"]}}", strlen("{\"entities\":[],\"targets\":{\"connections\":[\"x\"]}}"), &m, err, sizeof err) &&
          strstr(err, "wire"));
    {
        char *d = sh_mm_decode("a\\nb\\u0041\\ud83d\\ude00\\\\");
        CHECK(d && !strcmp(d, "a\nbA\xf0\x9f\x98\x80\\"));
        free(d);
    }

    printf("-- a map-level row (an attached mod) adds no entity, name or reference\n");
    {
        static const char ONE[] =
            "{\"entities\":[{\"displayName\":\"\",\"entityDef\":{\"className\":\"idSnapEntity\","
            "\"inherit\":\"snapmaps/logic/relay\"},\"uniqueId\":3}],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}";
        sh_modmanifest_list rows, back;
        sh_modmanifest_row row;
        char *text;
        size_t tl = 0;
        unsigned rejected = 0;
        memset(&rows, 0, sizeof rows);
        memset(&back, 0, sizeof back);
        memset(&row, 0, sizeof row);
        strcpy_s(row.id, sizeof row.id, "alex.bare-hands");
        strcpy_s(row.version, sizeof row.version, "1.0.0");
        memset(row.bundle, 'c', 64);
        row.size = 77;
        row.attached = 1;
        CHECK(sh_modmanifest_list_add(&rows, &row));
        text = sh_modmanifest_write(ONE, sizeof ONE - 1, &rows, &tl, err, sizeof err);
        CHECK(text != NULL);
        if (text) {
            CHECK(sh_mm_parse(text, tl, &m, err, sizeof err) && m.ent_count == 1 && m.ents[0].uid == 3 &&
                  m.ents[0].name_count == 1 && !strcmp(m.ents[0].names[0], "snapmaps/logic/relay"));
            sh_mm_free(&m);
            CHECK(sh_modmanifest_read(text, tl, &back, &rejected) == 1 && !rejected && back.rows[0].attached == 1);
            HeapFree(GetProcessHeap(), 0, text);
        }
        sh_modmanifest_list_free(&back);
        sh_modmanifest_list_free(&rows);
    }

    printf("modmap_test: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
