/* map_reference_audit: what the text alone establishes about the tables the
 * engine keys by uniqueId.
 *
 * The fixtures are the reference, instance and connection tables (keyValues
 * trimmed to the 73 slots that cover uniqueId 71; the engine's export pads
 * them to 471 with the final total) and each entity's uniqueId and inherit,
 * extracted from live Vulkan exports of one map (R-391):
 *   PASTE_EXPORT   after a native clipboard paste minted uniqueId 67 (opens);
 *   PREFAB_EXPORT  after a prefab placement then minted 68 (refused on open);
 *   WITHOUT_67     the refused map without 67 (still refused);
 *   PREFAB_REPAIRED the refused map with only uniqueId 68's rows removed.
 * The map's rows at 68, 69 and 71 predate both placements: no entity held
 * them, and the uniqueId the editor minted next inherited 68's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map_reference_audit.h"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const char PREFAB_EXPORT[] =
    "{\"entities\":[{\"entityDef\":{\"inherit\":\"snapmaps/visblockers/industrial/cap_05\"},\"uniqueId\":56},{\"entityDef\":{\"i"
    "nherit\":\"snapmaps/visblockers/industrial/cap_10\"},\"uniqueId\":57},{\"entityDef\":{\"inherit\":\"player/coop/player_s"
    "tart\"},\"uniqueId\":61},{\"entityDef\":{\"inherit\":\"snapmaps/listener/start/spawned\"},\"uniqueId\":62},{\"entityDef\":{"
    "\"inherit\":\"snapmaps/audio/2d_speaker\"},\"uniqueId\":63},{\"entityDef\":{\"inherit\":\"snapmaps/action/audio/play\"},\"u"
    "niqueId\":64},{\"entityDef\":{\"inherit\":\"snapmaps/action/audio/play\"},\"uniqueId\":65},{\"entityDef\":{\"inherit\":\"sna"
    "pmaps/action/audio/play\"},\"uniqueId\":66},{\"entityDef\":{\"inherit\":\"snapmaps/audio/2d_speaker\"},\"uniqueId\":67},{"
    "\"entityDef\":{\"inherit\":\"snapmaps/audio/2d_speaker\"},\"uniqueId\":68}],\"instanceEntities\":{\"keyValues\":[0,10,10],"
    "\"values\":[56,57,61,62,63,64,65,66,67,68]},\"references\":{\"entityEntRefs\":{\"keyValues\":[0,0,0,0,0,0,0,0,0,0,0,0,"
    "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,2,2,2,3,3,3,4,5,"
    "6,6,7,7,7,7],\"values\":[-1,-1,-1,-1,-1,-1,-1]},\"entityVarRefs\":{\"keyValues\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
    "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,5,9,13,13,14,1"
    "5,15,16],\"values\":[16777215,16777215,16777215,16777215,16777215,16777215,16777215,16777215,16777215,16777215,1"
    "6777215,16777215,16777215,16777215,16777215,16777215]}},\"targets\":{\"connections\":[-62,62,-63,64,65,66,-65,63,-"
    "66,63,-67,63]}}";

static const char PASTE_EXPORT[] =
    "{\"entities\":[{\"entityDef\":{\"inherit\":\"snapmaps/visblockers/industrial/cap_05\"},\"uniqueId\":56},{\"entityDef\":{\"i"
    "nherit\":\"snapmaps/visblockers/industrial/cap_10\"},\"uniqueId\":57},{\"entityDef\":{\"inherit\":\"player/coop/player_s"
    "tart\"},\"uniqueId\":61},{\"entityDef\":{\"inherit\":\"snapmaps/listener/start/spawned\"},\"uniqueId\":62},{\"entityDef\":{"
    "\"inherit\":\"snapmaps/audio/2d_speaker\"},\"uniqueId\":63},{\"entityDef\":{\"inherit\":\"snapmaps/action/audio/play\"},\"u"
    "niqueId\":64},{\"entityDef\":{\"inherit\":\"snapmaps/action/audio/play\"},\"uniqueId\":65},{\"entityDef\":{\"inherit\":\"sna"
    "pmaps/action/audio/play\"},\"uniqueId\":66},{\"entityDef\":{\"inherit\":\"snapmaps/audio/2d_speaker\"},\"uniqueId\":67}],"
    "\"instanceEntities\":{\"keyValues\":[0,9,9],\"values\":[56,57,61,62,63,64,65,66,67]},\"references\":{\"entityEntRefs\":{"
    "\"keyValues\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0"
    ",0,0,0,0,0,0,0,0,1,2,2,2,2,3,3,3,4,5,6,6,7,7,7,7],\"values\":[-1,-1,-1,-1,-1,-1,-1]},\"entityVarRefs\":{\"keyValues"
    "\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0"
    ",0,0,0,0,0,0,0,0,0,1,1,5,9,13,13,14,15,15,16],\"values\":[16777215,16777215,16777215,16777215,16777215,16777215,"
    "16777215,16777215,16777215,16777215,16777215,16777215,16777215,16777215,16777215,16777215]}},\"targets\":{\"conne"
    "ctions\":[-62,62,-63,64,65,66,-65,63,-66,63,-67,63]}}";

static const char WITHOUT_67[] =
    "{\"entities\":[{\"entityDef\":{\"inherit\":\"snapmaps/visblockers/industrial/cap_05\"},\"uniqueId\":56},{\"entityDef\":{\"i"
    "nherit\":\"snapmaps/visblockers/industrial/cap_10\"},\"uniqueId\":57},{\"entityDef\":{\"inherit\":\"player/coop/player_s"
    "tart\"},\"uniqueId\":61},{\"entityDef\":{\"inherit\":\"snapmaps/listener/start/spawned\"},\"uniqueId\":62},{\"entityDef\":{"
    "\"inherit\":\"snapmaps/audio/2d_speaker\"},\"uniqueId\":63},{\"entityDef\":{\"inherit\":\"snapmaps/action/audio/play\"},\"u"
    "niqueId\":64},{\"entityDef\":{\"inherit\":\"snapmaps/action/audio/play\"},\"uniqueId\":65},{\"entityDef\":{\"inherit\":\"sna"
    "pmaps/action/audio/play\"},\"uniqueId\":66},{\"entityDef\":{\"inherit\":\"snapmaps/audio/2d_speaker\"},\"uniqueId\":68}],"
    "\"instanceEntities\":{\"keyValues\":[0,9,9],\"values\":[56,57,61,62,63,64,65,66,68]},\"references\":{\"entityEntRefs\":{"
    "\"keyValues\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0"
    ",0,0,0,0,0,0,0,0,1,2,2,2,2,3,3,3,4,5,6,6,7,7,7,7],\"values\":[-1,-1,-1,-1,-1,-1,-1]},\"entityVarRefs\":{\"keyValues"
    "\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0"
    ",0,0,0,0,0,0,0,0,0,1,1,5,9,13,13,14,15,15,16],\"values\":[16777215,16777215,16777215,16777215,16777215,16777215,"
    "16777215,16777215,16777215,16777215,16777215,16777215,16777215,16777215,16777215,16777215]}},\"targets\":{\"conne"
    "ctions\":[-62,62,-63,64,65,66,-65,63,-66,63,-67,63]}}";

static const char PREFAB_REPAIRED[] =
    "{\"entities\":[{\"entityDef\":{\"inherit\":\"snapmaps/visblockers/industrial/cap_05\"},\"uniqueId\":56},{\"entityDef\":{\"i"
    "nherit\":\"snapmaps/visblockers/industrial/cap_10\"},\"uniqueId\":57},{\"entityDef\":{\"inherit\":\"player/coop/player_s"
    "tart\"},\"uniqueId\":61},{\"entityDef\":{\"inherit\":\"snapmaps/listener/start/spawned\"},\"uniqueId\":62},{\"entityDef\":{"
    "\"inherit\":\"snapmaps/audio/2d_speaker\"},\"uniqueId\":63},{\"entityDef\":{\"inherit\":\"snapmaps/action/audio/play\"},\"u"
    "niqueId\":64},{\"entityDef\":{\"inherit\":\"snapmaps/action/audio/play\"},\"uniqueId\":65},{\"entityDef\":{\"inherit\":\"sna"
    "pmaps/action/audio/play\"},\"uniqueId\":66},{\"entityDef\":{\"inherit\":\"snapmaps/audio/2d_speaker\"},\"uniqueId\":67},{"
    "\"entityDef\":{\"inherit\":\"snapmaps/audio/2d_speaker\"},\"uniqueId\":68}],\"instanceEntities\":{\"keyValues\":[0,10,10],"
    "\"values\":[56,57,61,62,63,64,65,66,67,68]},\"references\":{\"entityEntRefs\":{\"keyValues\":[0,0,0,0,0,0,0,0,0,0,0,0,"
    "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,2,2,2,3,3,3,4,5,"
    "6,6,6,6,6,6],\"values\":[-1,-1,-1,-1,-1,-1]},\"entityVarRefs\":{\"keyValues\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0"
    ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,5,9,13,13,13,14,1"
    "4,15],\"values\":[16777215,16777215,16777215,16777215,16777215,16777215,16777215,16777215,16777215,16777215,1677"
    "7215,16777215,16777215,16777215,16777215]}},\"targets\":{\"connections\":[-62,62,-63,64,65,66,-65,63,-66,63,-67,63"
    "]}}";

static int audit(const char *json, sh_map_reference_audit *out)
{
    int ok = sh_map_reference_audit_run(json, strlen(json), out);
    printf("  problems %u: %s\n  hazards %u: %s\n", out->problems, out->problem_text, out->hazards, out->hazard_text);
    return ok;
}

/* Orphan rows load, so the paste-only map has hazards and no problem. */
static void paste_export_is_consistent_with_held_hazards(void)
{
    sh_map_reference_audit a;
    CHECK(audit(PASTE_EXPORT, &a) && a.readable);
    CHECK(a.problems == 0);
    CHECK(a.hazards == 3);
    CHECK(strstr(a.hazard_text, "uniqueId 68 holds 1 entity and 1 variable reference row(s) but no entity"));
    CHECK(strstr(a.hazard_text, "uniqueId 69 holds 0 entity and 1 variable"));
    CHECK(strstr(a.hazard_text, "uniqueId 71 holds 0 entity and 1 variable"));
}

/* The refused map: the speaker minted at 68 owns rows its two peers lack. */
static void prefab_export_names_the_speaker_that_inherited_rows(void)
{
    sh_map_reference_audit a;
    CHECK(audit(PREFAB_EXPORT, &a) && a.readable);
    CHECK(a.problems == 1);
    CHECK(strstr(a.problem_text, "uniqueId 68 (snapmaps/audio/2d_speaker) owns 1 entity and 1 variable reference row(s) "
        "where 2 other entities with that inherit own 0 and 0"));
    CHECK(a.hazards == 2 && !strstr(a.hazard_text, "uniqueId 68"));
}

/* Without a majority the disagreement is still reported, naming both. */
static void a_tie_names_both_entities(void)
{
    sh_map_reference_audit a;
    CHECK(audit(WITHOUT_67, &a) && a.readable);
    CHECK(a.problems == 1);
    CHECK(strstr(a.problem_text, "uniqueIds 63 and 68 (snapmaps/audio/2d_speaker) own different reference rows: 0/0 and 1/1"));
}

static void removing_the_inherited_rows_is_consistent(void)
{
    sh_map_reference_audit a;
    CHECK(audit(PREFAB_REPAIRED, &a) && a.readable);
    CHECK(a.problems == 0 && a.hazards == 2);
}

static char *replaced(const char *text, const char *from, const char *to)
{
    const char *at = strstr(text, from);
    size_t head, size;
    char *out;
    if (!at) return NULL;
    head = (size_t)(at - text);
    size = strlen(text) - strlen(from) + strlen(to) + 1;
    if (!(out = malloc(size))) return NULL;
    memcpy(out, text, head); strcpy(out + head, to); strcat(out, at + strlen(from));
    return out;
}

static void structural_faults_are_named(void)
{
    sh_map_reference_audit a;
    char *text;
    /* The reference prefix sums must cover every held uniqueId. */
    text = replaced(PASTE_EXPORT, "\"uniqueId\":67}", "\"uniqueId\":90}");
    CHECK(text && audit(text, &a) && a.problems >= 2);
    CHECK(text && strstr(a.problem_text, "references.entityEntRefs.keyValues has 73 slots and does not cover uniqueId 90"));
    CHECK(text && strstr(a.problem_text, "instanceEntities names uniqueId 67, which no entity holds"));
    free(text);
    /* A total that disagrees with its values. */
    text = replaced(PASTE_EXPORT, "\"values\":[-1,-1,-1,-1,-1,-1,-1]", "\"values\":[-1,-1,-1,-1,-1,-1]");
    CHECK(text && audit(text, &a) && strstr(a.problem_text,
        "references.entityEntRefs.keyValues ends at 7 but references.entityEntRefs.values holds 6"));
    free(text);
    /* A wire naming an entity the map does not hold, and a duplicate uniqueId. */
    text = replaced(PASTE_EXPORT, "-67,63]", "-67,63,99]");
    CHECK(text && audit(text, &a) && a.problems == 1 &&
        strstr(a.problem_text, "targets.connections names target uniqueId 99, which no entity holds"));
    free(text);
    text = replaced(PASTE_EXPORT, "\"uniqueId\":66}", "\"uniqueId\":65}");
    CHECK(text && audit(text, &a) && strstr(a.problem_text, "uniqueId 65 is held by entities[6] and entities[7]"));
    free(text);
}

static void unreadable_text_is_not_audited(void)
{
    sh_map_reference_audit a;
    CHECK(!sh_map_reference_audit_run("{\"entities\":[]}", 15, &a) && !a.readable && !a.problems);
    CHECK(!sh_map_reference_audit_run("{\"entities\":[", 13, &a) && !a.readable);
    CHECK(!sh_map_reference_audit_run(NULL, 0, &a) && !a.readable);
}

int main(void)
{
    paste_export_is_consistent_with_held_hazards();
    prefab_export_names_the_speaker_that_inherited_rows();
    a_tie_names_both_entities();
    removing_the_inherited_rows_is_consistent();
    structural_faults_are_named();
    unreadable_text_is_not_audited();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("map reference audit passed");
    return 0;
}
