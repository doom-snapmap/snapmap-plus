/* The collision size a Blocking Box inherits from its entityDef. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "nav_entitydef.h"
#include "nav_regions.h"

#define NED_NAME_CAP    128
#define NED_DEPTH       16
#define NED_CACHE       16
#define NED_TEXT_MAX    (1024 * 1024)

/* The shipped declarations navigation reads, with the clip size each one
 * resolves to. snapmaps/volume/blocking is the only entityDef the navigation
 * reader accepts (16 x 128 x 64, CLIPMODEL_BOX; Snapmap+ does not override
 * it). Consulted before the reader so the load path does no resource I/O. */
static const struct { const char *name; float size[3]; } NED_BUILTIN[] = {
    { "snapmaps/volume/blocking", { 16.0f, 128.0f, 64.0f } },
};

typedef struct ned_entry { char name[NED_NAME_CAP]; int ok; float size[3]; } ned_entry;

static SRWLOCK g_ned_lock = SRWLOCK_INIT;
static sh_nav_entitydef_reader g_ned_reader;
static ned_entry g_ned_cache[NED_CACHE];
static int g_ned_count, g_ned_next;

void sh_nav_entitydef_set_reader(sh_nav_entitydef_reader reader)
{
    AcquireSRWLockExclusive(&g_ned_lock);
    g_ned_reader = reader;
    g_ned_count = g_ned_next = 0;
    ReleaseSRWLockExclusive(&g_ned_lock);
}

void sh_nav_entitydef_reset(void)
{
    AcquireSRWLockExclusive(&g_ned_lock);
    g_ned_count = g_ned_next = 0;
    ReleaseSRWLockExclusive(&g_ned_lock);
}

/* Walk the inherit chain: the nearest level that spells an axis wins. */
static int ned_walk(sh_nav_entitydef_reader reader, const char *name, float size[3])
{
    char current[NED_NAME_CAP], next[NED_NAME_CAP];
    unsigned have = 0;
    int type = 0, depth;
    float result[3] = { 0, 0, 0 };
    if (!reader || strlen(name) >= sizeof current) return 0;
    strcpy_s(current, sizeof current, name);
    for (depth = 0; depth < NED_DEPTH && have != 7; depth++) {
        unsigned char *text = NULL;
        size_t len = 0;
        float level[3];
        unsigned got = 0;
        int level_type = 0, i, ok;
        if (!reader(current, &text, &len) || !text) break;
        ok = len > 0 && len <= NED_TEXT_MAX &&
             sh_nav_regions_decl_level((const char *)text, len, level, &got,
                                       &level_type, next, sizeof next);
        free(text);
        if (!ok) return 0;
        if (!type && level_type) type = level_type;
        for (i = 0; i < 3; i++)
            if ((got & (1u << i)) && !(have & (1u << i))) { result[i] = level[i]; have |= 1u << i; }
        if (!next[0] || !_stricmp(next, current)) break;
        strcpy_s(current, sizeof current, next);
    }
    /* An omitted type is the box default, as in the map reader. */
    if (have != 7 || type < 0) return 0;
    memcpy(size, result, sizeof result);
    return 1;
}

int sh_nav_entitydef_box_size(const char *name, float size[3])
{
    sh_nav_entitydef_reader reader;
    ned_entry entry;
    int i;
    if (!name || !name[0] || !size || strlen(name) >= NED_NAME_CAP) return 0;
    for (i = 0; i < (int)(sizeof NED_BUILTIN / sizeof NED_BUILTIN[0]); i++)
        if (!_stricmp(name, NED_BUILTIN[i].name)) {
            memcpy(size, NED_BUILTIN[i].size, sizeof NED_BUILTIN[i].size);
            return 1;
        }
    AcquireSRWLockShared(&g_ned_lock);
    for (i = 0; i < g_ned_count; i++) if (!_stricmp(g_ned_cache[i].name, name)) {
        int ok = g_ned_cache[i].ok;
        if (ok) memcpy(size, g_ned_cache[i].size, sizeof g_ned_cache[i].size);
        ReleaseSRWLockShared(&g_ned_lock);
        return ok;
    }
    reader = g_ned_reader;
    ReleaseSRWLockShared(&g_ned_lock);

    memset(&entry, 0, sizeof entry);
    strcpy_s(entry.name, sizeof entry.name, name);
    entry.ok = ned_walk(reader, name, entry.size);
    if (!reader) return 0;          /* nothing to remember without a source */
    AcquireSRWLockExclusive(&g_ned_lock);
    if (g_ned_reader == reader) {
        if (g_ned_count < NED_CACHE) g_ned_cache[g_ned_count++] = entry;
        else { g_ned_cache[g_ned_next] = entry; g_ned_next = (g_ned_next + 1) % NED_CACHE; }
    }
    ReleaseSRWLockExclusive(&g_ned_lock);
    if (entry.ok) memcpy(size, entry.size, sizeof entry.size);
    return entry.ok;
}

int sh_nav_entitydef_default_size(const char *inherit, float size[3], void *ctx)
{
    (void)ctx;
    return sh_nav_entitydef_box_size(inherit, size);
}
