/* rawmap_picker.h -- the file types the File menu's rawmap dialogs offer.
 *
 * A rawmap travels two ways: the .json (the map alone) and the .zip export
 * (the map and every mod it uses, modexport.c). Load Rawmap lists both kinds
 * together. Save Rawmap As offers .zip first when the open map uses a mod,
 * because the .json alone does not load for anyone who lacks the mods, and
 * .json first otherwise. Header-only and free of window state so the choice
 * is testable (tests/rawmap_picker_test.cpp).
 *
 * The Prefabs tab's Export and Import use the same dialogs for a prefab's
 * export (backend/prefab_mods.c): always a ZIP, <name>.json and every mod it
 * records. Import takes that ZIP, or the .json of one that was extracted. */
#ifndef SNAPMAP_PLUS_RAWMAP_PICKER_H
#define SNAPMAP_PLUS_RAWMAP_PICKER_H

#include <string.h>
#include <stdlib.h>

namespace sh_rawmap_picker {

struct file_type {
    const wchar_t *label;
    const wchar_t *pattern;
};

struct choice {
    file_type types[4];
    unsigned count;
    const wchar_t *extension;   /* SetDefaultExtension */
    const wchar_t *file_name;   /* Save only: the name the dialog proposes */
};

static const file_type kZip  = { L"Rawmap with its mods (*.zip)", L"*.zip" };
static const file_type kJson = { L"Rawmap JSON (*.json)", L"*.json" };
static const file_type kJsonOnly = { L"Rawmap JSON only, without its mods (*.json)", L"*.json" };
static const file_type kBoth = { L"Rawmaps (*.json, *.zip)", L"*.json;*.zip" };
static const file_type kAll  = { L"All files (*.*)", L"*.*" };
static const file_type kPrefabZip = { L"Prefab export (*.zip)", L"*.zip" };
static const file_type kPrefabOpen = { L"Prefab exports (*.zip, *.json)", L"*.zip;*.json" };

/* The first type is the one selected when the dialog opens. */
inline choice for_dialog(bool save, int map_mods)
{
    choice c;
    memset(&c, 0, sizeof c);
    if (!save) {
        c.types[0] = kBoth; c.types[1] = kZip; c.types[2] = kJson; c.types[3] = kAll;
        c.count = 4;
        c.extension = L"json";
        return c;
    }
    if (map_mods > 0) {
        c.types[0] = kZip; c.types[1] = kJsonOnly; c.types[2] = kAll;
        c.count = 3;
        c.extension = L"zip";
        c.file_name = L"rawmap.zip";
    } else {
        c.types[0] = kJson; c.types[1] = kZip; c.types[2] = kAll;
        c.count = 3;
        c.extension = L"json";
        c.file_name = L"rawmap.json";
    }
    return c;
}

/* Export Prefab (save): a ZIP; the caller proposes "<name>.zip". Import
 * Prefab (open): a ZIP, or the .json inside an extracted export folder. */
inline choice for_prefab_dialog(bool save)
{
    choice c;
    memset(&c, 0, sizeof c);
    c.types[0] = save ? kPrefabZip : kPrefabOpen; c.types[1] = kAll;
    c.count = 2;
    c.extension = L"zip";
    c.file_name = nullptr;
    return c;
}

/* "mapMods" from the backend's rawmap status object; 0 when absent (an older
 * backend, or no map open). */
inline int map_mods_from_status(const char *status_json)
{
    const char *at = status_json ? strstr(status_json, "\"mapMods\":") : nullptr;
    if (!at) return 0;
    return atoi(at + 10);
}

}  // namespace sh_rawmap_picker

#endif
