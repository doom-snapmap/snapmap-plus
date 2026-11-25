/* rawmap_picker.h -- the file types the File menu's rawmap dialogs offer, free of window state
 * (tests/rawmap_picker_test.cpp). A rawmap is a .json (the map alone) or a .zip export (the map and every mod it
 * uses, modexport.c); Load lists both, and Save As offers .zip first when the open map uses a mod. The Prefabs
 * tab's Export (always a ZIP of <name>.json and its mods, backend/prefab_mods.c) and Import use the same dialogs.
 * Each dialog opens in its last folder, kept in config.json under folder_config_key; without one the prefab dialogs
 * open in Downloads and the rawmap dialogs in %LOCALAPPDATA%\snapmap-plus. */
#ifndef SNAPMAP_PLUS_RAWMAP_PICKER_H
#define SNAPMAP_PLUS_RAWMAP_PICKER_H

#include <string.h>
#include <stdlib.h>
#include <string>

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

/* "mapMods" from the backend's rawmap status object; 0 when absent (or no map is open). */
inline int map_mods_from_status(const char *status_json)
{
    const char *at = status_json ? strstr(status_json, "\"mapMods\":") : nullptr;
    if (!at) return 0;
    return atoi(at + 10);
}

/* The dialogs, as the host numbers them. */
enum { KIND_RAWMAP_LOAD = 0, KIND_RAWMAP_SAVE = 1, KIND_PREFAB_EXPORT = 2, KIND_PREFAB_IMPORT = 3 };
enum { FOLDER_RAWMAPS = 0, FOLDER_DOWNLOADS = 1 };

inline const char *folder_config_key(int kind)
{
    switch (kind) {
    case KIND_RAWMAP_LOAD:   return "dialogs.rawmap_load";
    case KIND_RAWMAP_SAVE:   return "dialogs.rawmap_save";
    case KIND_PREFAB_EXPORT: return "dialogs.prefab_export";
    case KIND_PREFAB_IMPORT: return "dialogs.prefab_import";
    }
    return nullptr;
}

/* Where a dialog opens before it has a kept folder. */
inline int default_folder(int kind)
{
    return (kind == KIND_PREFAB_EXPORT || kind == KIND_PREFAB_IMPORT) ? FOLDER_DOWNLOADS : FOLDER_RAWMAPS;
}

/* The folder part of a picked file's path ("" when it has none); a drive
 * root keeps its separator. */
inline std::wstring folder_of(const std::wstring &path)
{
    size_t cut = path.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return std::wstring();
    if (cut == 2 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, cut);
}

/* The Mods tab's Install: one Open dialog for mod ZIPs and a mod folder's mod.json (a Windows dialog
 * picks files or folders, never both, so a folder is picked by its mod.json). */
static const file_type kModInstall = { L"Mod ZIPs and mod folders (*.zip, mod.json)", L"*.zip;mod.json" };

/* What Install takes for one picked file: a mod.json stands for its folder; anything else (a ZIP) is
 * itself. */
inline std::wstring install_target(const std::wstring &picked)
{
    size_t cut = picked.find_last_of(L"\\/");
    std::wstring leaf = cut == std::wstring::npos ? picked : picked.substr(cut + 1);
    if (_wcsicmp(leaf.c_str(), L"mod.json") == 0 && cut != std::wstring::npos) return folder_of(picked);
    return picked;
}

/* A kept folder as its config value: a JSON string in ASCII, \uXXXX past it. */
inline std::string folder_to_config(const std::wstring &folder)
{
    static const char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (size_t i = 0; i < folder.size(); i++) {
        unsigned c = (unsigned)folder[i];
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c >= 0x20 && c < 0x7f) out += (char)c;
        else {
            out += "\\u";
            out += hex[(c >> 12) & 15]; out += hex[(c >> 8) & 15]; out += hex[(c >> 4) & 15]; out += hex[c & 15];
        }
    }
    out += '"';
    return out;
}

/* A config value back to the folder; false unless it is a nonempty JSON
 * string. UTF-8 and \u escapes both decode (to UTF-16). */
inline bool folder_from_config(const char *json, std::wstring &out)
{
    out.clear();
    if (!json) return false;
    const unsigned char *p = (const unsigned char *)json;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != '"') return false;
    for (;;) {
        unsigned c = *p++;
        if (!c) { out.clear(); return false; }
        if (c == '"') break;
        if (c == '\\') {
            unsigned e = *p++;
            switch (e) {
            case '"': case '\\': case '/': out += (wchar_t)e; break;
            case 'b': out += L'\b'; break;
            case 'f': out += L'\f'; break;
            case 'n': out += L'\n'; break;
            case 'r': out += L'\r'; break;
            case 't': out += L'\t'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4; i++) {
                    unsigned h = *p++, d;
                    if (h >= '0' && h <= '9') d = h - '0';
                    else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                    else { out.clear(); return false; }
                    v = (v << 4) | d;
                }
                out += (wchar_t)v;
                break;
            }
            default: out.clear(); return false;
            }
            continue;
        }
        if (c < 0x80) { out += (wchar_t)c; continue; }
        unsigned cp, more;
        if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; more = 1; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; more = 2; }
        else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; more = 3; }
        else { out.clear(); return false; }
        for (unsigned i = 0; i < more; i++) {
            unsigned b = *p++;
            if ((b & 0xc0) != 0x80) { out.clear(); return false; }
            cp = (cp << 6) | (b & 0x3f);
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (wchar_t)(0xd800 + (cp >> 10));
            out += (wchar_t)(0xdc00 + (cp & 0x3ff));
        } else out += (wchar_t)cp;
    }
    return !out.empty();
}

}  // namespace sh_rawmap_picker

#endif
