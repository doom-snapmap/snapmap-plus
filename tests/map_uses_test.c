/* map_uses_test -- what a map uses of Snapmap+ itself (map_uses.h; Vanilla stays vanilla). The fixtures are maps the
 * game saved: a cinematic camera, a water volume, two relays wired and a timeline stay vanilla; the same map with its
 * Grid Room resized needs Snapmap+, and so does a Blocking Box marked for AI navigation (flags.noFlood).
 * Usage: map_uses_test <tests\fixtures\maps> */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map_uses.h"
#include "modmanifest.h"
#include "nav_regions.h"
#include "grid_room.h"
#include "mp_json.h"

void backend_log(const char *message) { (void)message; }

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char *read_all(const char *dir, const char *leaf, size_t *len)
{
    char path[MAX_PATH * 2];
    FILE *f = NULL;
    char *buf = NULL;
    long n;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, leaf);
    *len = 0;
    if (fopen_s(&f, path, "rb") || !f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && (buf = (char *)malloc((size_t)n + 1)) != NULL && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        buf[n] = 0;
        *len = (size_t)n;
    } else {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

/* The map's placed names as the save gives them: every entity's inherit and every instance's moduleName. */
static unsigned uses_of(const char *text, size_t len)
{
    mpj *m = mpj_parse(text, len);
    const mpj *ents = m ? mpj_get(m, "entities") : NULL, *inst = m ? mpj_get(m, "instances") : NULL;
    const char *names[64];
    size_t count = 0;
    int i;
    unsigned u;
    for (i = 0; ents && ents->type == MPJ_ARR && i < ents->n && count < 64; i++) {
        const mpj *def = mpj_get(ents->items[i], "entityDef"), *inh = def ? mpj_get(def, "inherit") : NULL;
        if (inh && inh->type == MPJ_STR) names[count++] = inh->text;
    }
    for (i = 0; inst && inst->type == MPJ_ARR && i < inst->n && count < 64; i++) {
        const mpj *mn = mpj_get(inst->items[i], "moduleName");
        if (mn && mn->type == MPJ_STR) names[count++] = mn->text;
    }
    u = sh_map_uses(text, len, names, count);
    mpj_free(m);
    return u;
}

static const char BOX[] =
    "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapBlockingVolume\",\"inherit\":\"snapmaps/volume/blocking\","
    "\"state\":{\"edit\":{\"flags\":{\"noFlood\":%s},\"blockDemons\":true}}},\"uniqueId\":3}],"
    "\"instances\":[{\"moduleName\":\"maps/modules/ind_dlc/ind_totally_blank_room_4x.decl\"}]}";

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "fixtures\\maps";
    size_t len = 0;
    char *text, box[512];
    printf("-- what a map uses of Snapmap+ itself\n");

    text = read_all(dir, "vanilla_camera_water_logic_timeline.json", &len);
    CHECK(text != NULL);
    if (text) CHECK(uses_of(text, len) == 0);   /* camera, water, relays, timeline: vanilla */
    free(text);

    text = read_all(dir, "resized_grid_room.json", &len);
    CHECK(text != NULL);
    if (text) CHECK(uses_of(text, len) == SH_MODMANIFEST_USES_MODULE_SIZES);
    free(text);

    _snprintf_s(box, sizeof box, _TRUNCATE, BOX, "true");
    CHECK(sh_nav_regions_marks(box, strlen(box)) == 1);
    CHECK(uses_of(box, strlen(box)) == SH_MODMANIFEST_USES_NAVIGATION);
    _snprintf_s(box, sizeof box, _TRUNCATE, BOX, "false");   /* Block Demons alone is SnapMap's own */
    CHECK(sh_nav_regions_marks(box, strlen(box)) == 0);
    CHECK(uses_of(box, strlen(box)) == 0);
    CHECK(sh_nav_regions_marks("not a map", 9) == -1);

    printf("map_uses_test: %s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
