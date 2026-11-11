/* The features that are off this session (feature_health.h). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "feature_health.h"
#include "backend_log.h"

#define MAX_OFF  48
#define NAME_MAX 64

static SRWLOCK g_lock = SRWLOCK_INIT;
static char    g_off[MAX_OFF][NAME_MAX];
static int     g_count;

static int find_locked(const char *feature)
{
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_off[i], feature)) return i;
    return -1;
}

void sh_feature_off(const char *feature, const char *why)
{
    char line[320];
    int added = 0;
    if (!feature || !*feature) return;
    AcquireSRWLockExclusive(&g_lock);
    if (find_locked(feature) < 0 && g_count < MAX_OFF) {
        strncpy_s(g_off[g_count], NAME_MAX, feature, _TRUNCATE);
        g_count++;
        added = 1;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!added) return;
    _snprintf_s(line, sizeof line, _TRUNCATE, "%s: off for this session -- %s", feature,
                why && *why ? why : "its engine function could not be patched");
    backend_log(line);
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

/* Append s; stop silently at the end of out (always terminated). */
static void append(char *out, size_t cap, size_t *len, const char *s)
{
    size_t n = strlen(s);
    if (*len + 1 >= cap) return;
    if (n > cap - 1 - *len) n = cap - 1 - *len;
    memcpy(out + *len, s, n);
    *len += n;
    out[*len] = '\0';
}

size_t sh_features_off_line(char *out, size_t cap)
{
    size_t len = 0;
    if (!out || !cap) return 0;
    out[0] = '\0';
    AcquireSRWLockShared(&g_lock);
    if (g_count) {
        append(out, cap, &len, "These features are unavailable in this session: ");
        for (int i = 0; i < g_count; i++) {
            if (i) append(out, cap, &len, ", ");
            append(out, cap, &len, g_off[i]);
        }
        append(out, cap, &len, ".");
    }
    ReleaseSRWLockShared(&g_lock);
    return len;
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

size_t sh_feature_health_slot(struct sh_iface *self, char *out, size_t cap)
{
    (void)self;
    return sh_features_off_line(out, cap);
}

#ifdef SH_FEATURE_HEALTH_TESTING
void sh_feature_health_test_reset(void)
{
    AcquireSRWLockExclusive(&g_lock);
    g_count = 0;
    ReleaseSRWLockExclusive(&g_lock);
}
#endif
