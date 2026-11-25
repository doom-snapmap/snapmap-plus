/* The generated display tables keep blank descriptions, lookup keys and argument positions. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include "../src/ui/sh_entity_desc.h"
#include "../src/ui/sh_event_docs.h"

int main()
{
    int shown = 0, blank = 0, args = 0;
    for (const auto &entity : SH_ENTITY_DESCS) {
        assert(entity.name[0] && entity.summary);
        if (entity.summary[0]) shown++;
        else blank++;
        if (!strcmp(entity.name, "aas/obstacle")) assert(!entity.summary[0]);
    }
    for (const auto &event : SH_EVENT_DOCS) {
        assert(event.name[0] && event.summary && (event.args != nullptr) == (event.nArgs > 0));
        for (int i = 0; i < event.nArgs; i++) {
            assert(event.args[i].name && event.args[i].desc);
            args++;
        }
    }
    assert(shown > 700 && blank > 1000 && args > 2000);
    printf("player_text_test: %d descriptions, %d blank and %d event arguments\\n", shown, blank, args);
    return 0;
}
