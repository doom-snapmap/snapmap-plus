/* The features that are off this session (feature_health.h). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "feature_health.h"
#include "backend_log.h"
#include "snapmap_plus_iface.h"
#include "../common/text.h"

/* As many features as are off, each name its own copy. */
static SRWLOCK g_lock = SRWLOCK_INIT;
static char  **g_off;
static int     g_count, g_capacity;

static int find_locked(const char *feature)
{
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_off[i], feature)) return i;
    return -1;
}

void sh_feature_off(const char *feature, const char *why)
{
    char *line, *name;
    int added = 0;
    if (!feature || !*feature || (name = sh_text(feature)) == NULL) return;
    AcquireSRWLockExclusive(&g_lock);
    if (find_locked(feature) < 0) {
        if (g_count == g_capacity) {
            int capacity = g_capacity ? g_capacity * 2 : 16;
            char **grown = (char **)realloc(g_off, (size_t)capacity * sizeof *grown);
            if (grown) { g_off = grown; g_capacity = capacity; }
        }
        if (g_count < g_capacity) { g_off[g_count++] = name; name = NULL; added = 1; }
    }
    ReleaseSRWLockExclusive(&g_lock);
    free(name);
    if (!added) return;
    line = sh_textf("%s: off for this session -- %s", feature,
                    why && *why ? why : "its engine function could not be patched");
    backend_log(line ? line : feature);
    free(line);
}

int sh_feature_is_off(const char *feature)
{
    int off;
    if (!feature) return 0;
    AcquireSRWLockShared(&g_lock);
    off = find_locked(feature) >= 0;
    ReleaseSRWLockShared(&g_lock);
    return off;
}

#ifdef SH_DIAG
char *sh_feature_off_name(int index)
{
    char *out = NULL;
    AcquireSRWLockShared(&g_lock);
    if (index >= 0 && index < g_count) out = _strdup(g_off[index]);
    ReleaseSRWLockShared(&g_lock);
    return out;
}
#endif

/* The line built with `copy` (sh_text or the interface's process-heap copy). */
static char *features_off_line(char *(*copy)(const char *))
{
    size_t len = 0;
    char *text, *out = NULL;
    static const char head[] = "These features are unavailable in this session: ";
    AcquireSRWLockShared(&g_lock);
    if (g_count) {
        for (int i = 0; i < g_count; i++) len += strlen(g_off[i]) + 2;
        text = (char *)malloc(sizeof head + len + 1);
        if (text) {
            strcpy_s(text, sizeof head + len + 1, head);
            for (int i = 0; i < g_count; i++) {
                if (i) strcat_s(text, sizeof head + len + 1, ", ");
                strcat_s(text, sizeof head + len + 1, g_off[i]);
            }
            strcat_s(text, sizeof head + len + 1, ".");
            out = copy(text);
            free(text);
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return out;
}

char *sh_features_off_text(void)
{
    return features_off_line(sh_text);
}

size_t sh_feature_refusal(const char *feature, char *out, size_t cap)
{
    int n;
    if (!out || !cap) return 0;
    out[0] = '\0';
    if (!sh_feature_is_off(feature)) return 0;
    n = _snprintf_s(out, cap, _TRUNCATE, "%s is unavailable in this session.", feature);
    if (n < 0) n = (int)strlen(out);
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
    return (size_t)n;
}

char *sh_feature_health_slot(struct sh_iface *self)
{
    (void)self;
    return features_off_line(sh_iface_text);
}

#ifdef SH_FEATURE_HEALTH_TESTING
void sh_feature_health_test_reset(void)
{
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_count; i++) free(g_off[i]);
    free(g_off);
    g_off = NULL;
    g_count = g_capacity = 0;
    ReleaseSRWLockExclusive(&g_lock);
}
#endif
