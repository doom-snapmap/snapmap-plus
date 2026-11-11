/* The feature-health table: one entry per feature, the window's one line, the
 * refusal sentence, and output that never overruns its buffer. */
#include <stdio.h>
#include <string.h>
#include "../src/backend/feature_health.h"

static int failures;
static char last_log[512];
void backend_log(const char *line) { strncpy_s(last_log, sizeof last_log, line, _TRUNCATE); }
#define CHECK(test) do { if (!(test)) { printf("FAIL line %d: %s\n", __LINE__, #test); failures++; } } while (0)

int main(void)
{
    char line[256], small[24];

    sh_feature_health_test_reset();
    CHECK(sh_features_off_line(line, sizeof line) == 0 && line[0] == '\0');
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

    {
        size_t n = sh_features_off_line(line, sizeof line);
        CHECK(n == strlen(line));
    }
    CHECK(!strcmp(line, "These features are unavailable in this session: Rawmaps, Prefabs."));
    CHECK(sh_feature_health_slot(NULL, line, sizeof line) > 0 &&
          !strcmp(line, "These features are unavailable in this session: Rawmaps, Prefabs."));

    sh_feature_off(SH_FEATURE_TIMELINES, NULL);
    CHECK(sh_feature_refusal(SH_FEATURE_TIMELINES, line, sizeof line) > 0);
    CHECK(!strcmp(line, "The Timelines tile is unavailable in this session."));
    CHECK(sh_feature_refusal(SH_FEATURE_MODS, line, sizeof line) == 0 && line[0] == '\0');

    /* A short buffer is cut, never overrun, and stays terminated. */
    memset(small, 'x', sizeof small);
    CHECK(sh_features_off_line(small, sizeof small) == sizeof small - 1 && small[sizeof small - 1] == '\0');
    CHECK(sh_features_off_line(NULL, 10) == 0);

    printf("feature_health_test: %d failures\n", failures);
    return failures != 0;
}
