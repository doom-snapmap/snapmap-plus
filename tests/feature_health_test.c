/* The feature-health table: one entry per feature, the window's one line, the
 * refusal sentence, and output that never overruns its buffer. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/feature_health.h"
#include "../src/common/snapmap_plus_iface.h"

static int failures;
static char last_log[512];
void backend_log(const char *line) { strncpy_s(last_log, sizeof last_log, line, _TRUNCATE); }
#define CHECK(test) do { if (!(test)) { printf("FAIL line %d: %s\n", __LINE__, #test); failures++; } } while (0)

int main(void)
{
    char line[256], *text;

    sh_feature_health_test_reset();
    CHECK(sh_features_off_text() == NULL && sh_feature_health_slot(NULL) == NULL);
    CHECK(sh_feature_refusal(SH_FEATURE_RAWMAPS, line, sizeof line) == 0 && line[0] == '\0');
    CHECK(!sh_feature_is_off(SH_FEATURE_RAWMAPS));

    sh_feature_off(SH_FEATURE_RAWMAPS, "DeserializeFromJson changed");
    CHECK(strstr(last_log, "Rawmaps: off for this session -- DeserializeFromJson changed") != NULL);
    last_log[0] = '\0';
    sh_feature_off(SH_FEATURE_RAWMAPS, "again");            /* recorded once, logged once */
    CHECK(last_log[0] == '\0');
    sh_feature_off(SH_FEATURE_PREFABS, NULL);
    CHECK(strstr(last_log, "could not be patched") != NULL);
    CHECK(sh_feature_is_off(SH_FEATURE_RAWMAPS) && sh_feature_is_off(SH_FEATURE_PREFABS));
    CHECK(!sh_feature_is_off(SH_FEATURE_MODS));

    text = sh_features_off_text();
    CHECK(text && !strcmp(text, "These features are unavailable in this session: Rawmaps, Prefabs."));
    free(text);
    text = sh_feature_health_slot(NULL);
    CHECK(text && !strcmp(text, "These features are unavailable in this session: Rawmaps, Prefabs."));
    sh_iface_text_free(text);

    sh_feature_off(SH_FEATURE_TIMELINES, NULL);
    CHECK(sh_feature_refusal(SH_FEATURE_TIMELINES, line, sizeof line) > 0);
    CHECK(!strcmp(line, "The Timelines tile is unavailable in this session."));
    CHECK(sh_feature_refusal(SH_FEATURE_MODS, line, sizeof line) == 0 && line[0] == '\0');

    /* Every feature that is off is named, however many: the line grows. */
    sh_feature_health_test_reset();
    for (int i = 0; i < 200; i++) {
        char name[32];
        sprintf_s(name, sizeof name, "feature %03d", i);
        sh_feature_off(name, "test");
    }
    text = sh_features_off_text();
    CHECK(text && strstr(text, "feature 000, feature 001") && strstr(text, "feature 199."));
    free(text);

    printf("feature_health_test: %d failures\n", failures);
    return failures != 0;
}
