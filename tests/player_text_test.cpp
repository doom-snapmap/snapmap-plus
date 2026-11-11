// player_text_test -- the description a player sees (player_text.h): sentences
// about the study of the game's code are left out, an entry that names only
// itself has no description, and every shipped table entry comes out clean.
#include <stdio.h>
#include <string.h>
#include <string>

#include "../src/ui/webview/player_text.h"
#include "../src/ui/sh_entity_desc.h"
#include "../src/ui/sh_event_docs.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

int main()
{
    using sh_player_text::summary;
    using sh_player_text::is_study_text;

    /* Placeholders become no description at all. */
    CHECK(summary("'aas/obstacle' entity. Runtime class: idAASObstacle.") == "");
    CHECK(summary("'ui/hud' entity (gui category). Runtime class: idHud. Editable properties: gui.") == "");
    CHECK(summary("") == "" && summary(nullptr) == "");

    /* Study sentences go, the rest stays in order. */
    CHECK(summary("A navigation obstacle that blocks AI pathfinding. Decompiled handlers manage a flag at offset "
                  "+0xc8c. Config: travelFlags (which AI travel types it blocks). Event 0x1c3 = enable wire.") ==
          "A navigation obstacle that blocks AI pathfinding. Config: travelFlags (which AI travel types it blocks).");
    CHECK(summary("A scriptable animated prop (event 0x14e). Plays a list of animations.") ==
          "A scriptable animated prop. Plays a list of animations.");
    CHECK(summary("Fires the weapon. (Only the empty-slot error path is visible in this decompile.)") == "Fires the weapon.");
    CHECK(summary("Its handler (event 1c3) emits a pulse. The burn lingers.") == "The burn lingers.");
    CHECK(summary("Calls FUN_1405f6230 to re-apply it.") == "");

    /* Ordinary prose, numbers and words that merely look like hex stay. */
    CHECK(summary("Spawns 3 demons at the point. A second wave follows.") == "Spawns 3 demons at the point. A second wave follows.");
    CHECK(!is_study_text("The event added to the face of the cube.") && !is_study_text("Deals 0 damage."));
    CHECK(is_study_text("this+0x5d7a") && is_study_text("via vtable+0xe20") && is_study_text("event id 63e"));

    /* Every shipped entry, entity and event, reads clean; real descriptions survive. */
    int kept = 0, empty = 0;
    for (int i = 0; i < SH_ENTITY_DESCS_N; i++) {
        std::string s = summary(SH_ENTITY_DESCS[i].summary);
        if (s.empty()) { empty++; continue; }
        kept++;
        if (is_study_text(s)) { fprintf(stderr, "entity %s: %s\n", SH_ENTITY_DESCS[i].name, s.c_str()); CHECK(false); }
    }
    CHECK(kept > 700 && empty > 1000);
    for (int i = 0; i < SH_EVENT_DOCS_N; i++) {
        std::string s = summary(SH_EVENT_DOCS[i].summary);
        if (!s.empty() && is_study_text(s)) { fprintf(stderr, "event %s: %s\n", SH_EVENT_DOCS[i].name, s.c_str()); CHECK(false); }
        for (int a = 0; a < SH_EVENT_DOCS[i].nArgs; a++) {
            std::string d = summary(SH_EVENT_DOCS[i].args[a].desc);
            if (!d.empty() && is_study_text(d)) { fprintf(stderr, "arg %s.%s: %s\n", SH_EVENT_DOCS[i].name, SH_EVENT_DOCS[i].args[a].name, d.c_str()); CHECK(false); }
        }
    }

    if (g_failed) { fprintf(stderr, "player_text_test: %d of %d checks failed\n", g_failed, g_checks); return 1; }
    printf("player_text_test: %d checks passed (%d entity descriptions shown, %d without one)\n", g_checks, kept, empty);
    return 0;
}
