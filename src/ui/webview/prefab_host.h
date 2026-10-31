/* prefab_host.h -- decisions the WebView host makes for the Prefabs tab, kept
 * free of window and engine state so they are testable
 * (tests/prefab_host_test.cpp).
 *
 * Create's precheck: DOOM's copy check (PrefabPopulate) refuses a selection
 * whose hovered entity is not one of the selected ones, the same way it
 * refuses an empty selection, so the host tells the two apart before it asks.
 *
 * A prefab's kind: the file's top-level "~type" decides it, idSnapMap a
 * module prefab and idSnapEntityPrefab an object prefab, with or without the
 * product's snapmapPlus block. The list reads only a tail of each file; the
 * engine writes "~type" second to last, so the last "~type" before the block
 * is the top-level one.
 *
 * The live Limits gate: the host posts the map's usage again only when its
 * text or the held prefab changed. */
#ifndef SNAPMAP_PLUS_PREFAB_HOST_H
#define SNAPMAP_PLUS_PREFAB_HOST_H

#include <string.h>
#include <string>
#include "prefab_block.h"

namespace sh_prefab_host {

enum {
    CREATE_GO = -1,
    CREATE_EMPTY = 0,              /* nothing selected */
    CREATE_NO_HOVER = 2,           /* nothing hovered: the copy check needs a hovered, selected entity */
    CREATE_HOVER_UNSELECTED = 4    /* the hovered entity is not selected */
};

/* hovered is the editor's hovered entity id, -1 for none; have_hover false
 * when this backend cannot say (the copy check then decides alone). */
inline int create_precheck(const int *selected, int count, int hovered, bool have_hover)
{
    if (count <= 0) return CREATE_EMPTY;
    if (!have_hover) return CREATE_GO;
    if (hovered < 0) return CREATE_NO_HOVER;
    for (int i = 0; i < count; i++)
        if (selected[i] == hovered) return CREATE_GO;
    return CREATE_HOVER_UNSELECTED;
}

/* The top-level "~type" of a prefab file from its whole text or its tail;
 * "" when the text names none. */
inline std::string top_type_of_tail(const char *text, size_t len)
{
    if (!text || !len) return std::string();
    size_t end = len;
    sh_prefab_block_span span;
    if (sh_prefab_block_find_tail(text, len, &span) == 1 && span.cut_at <= len) end = span.cut_at;
    std::string region(text, end);
    size_t t = region.rfind("\"~type\"");
    if (t == std::string::npos) return std::string();
    size_t c = region.find(':', t + 7);
    if (c == std::string::npos) return std::string();
    size_t q1 = region.find('"', c + 1);
    if (q1 == std::string::npos) return std::string();
    for (size_t p = c + 1; p < q1; p++)
        if (region[p] != ' ' && region[p] != '\t' && region[p] != '\r' && region[p] != '\n') return std::string();
    size_t q2 = region.find('"', q1 + 1);
    if (q2 == std::string::npos) return std::string();
    return region.substr(q1 + 1, q2 - q1 - 1);
}

/* "modules", "objects", or "" when the type is neither (the caller falls back
 * to the view's "kind"). */
inline const char *kind_of_type(const std::string &type)
{
    if (type == "idSnapMap") return "modules";
    if (type == "idSnapEntityPrefab") return "objects";
    return "";
}

/* The live usage gate. */
struct usage_gate {
    std::string last;
    bool primed = false;
    /* True when (usage, held) differs from what was last posted; records it. */
    bool changed(const std::string &usage, const std::string &held)
    {
        std::string key = usage + '\n' + held;
        if (primed && key == last) return false;
        last = key;
        primed = true;
        return true;
    }
};

}  // namespace sh_prefab_host

#endif
