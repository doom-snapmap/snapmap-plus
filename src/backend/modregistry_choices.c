/* modregistry_choices.c -- the open map's choices between mods' edits (modregistry.h):
 * validated here, stored by the map manifest (modmanifest.c), which gives
 * them to the serving index. Apart from modregistry.c so that the binaries
 * and tests that link the serving index without the manifest stay as they are. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "modregistry.h"
#include "modmanifest.h"
#include "modsalt.h"

int sh_modregistry_choices_set(const char *slot, const char *key)
{
    if (!sh_modregistry_slot_valid(slot)) return -1;
    if (key && key[0] && !sh_salt_valid(key, strlen(key))) return -1;
    return sh_modmanifest_current_choose(slot, key && key[0] ? key : NULL);
}

int sh_modregistry_choices_get(const char *slot, char *key, size_t cap)
{
    if (key && cap) key[0] = 0;
    if (!sh_modregistry_slot_valid(slot)) return 0;
    return sh_modmanifest_current_choice(slot, key, cap);
}

int sh_modregistry_choose_command(const char *slot, const char *key, char *out, size_t cap)
{
    int r;
    if (!out || !cap) return -1;
    if (!sh_modregistry_slot_valid(slot)) {
        _snprintf_s(out, cap, _TRUNCATE, "MODS: choose: '%s' is not a slot: <game file>/<path>, as the doctor and the Mods "
                    "tab name a clash (entitydef/player/coop/edit/handsModelMPUseDoomHands)", slot ? slot : "");
        return -1;
    }
    if (key && key[0] && !sh_salt_valid(key, strlen(key))) {
        _snprintf_s(out, cap, _TRUNCATE, "MODS: choose: '%s' is not a mod's salt", key);
        return -1;
    }
    r = sh_modregistry_choices_set(slot, key);
    if (r < 0) _snprintf_s(out, cap, _TRUNCATE, "MODS: choose: %s: refused (no map is open, or out of memory)", slot);
    else if (key && key[0])
        _snprintf_s(out, cap, _TRUNCATE, "MODS: choose: %s is served from %s in this map when several of its mods set "
                    "it; applies now; saved with the map's next save (%d bundle(s) changed)", slot, key, r);
    else
        _snprintf_s(out, cap, _TRUNCATE, "MODS: choose: %s: the map's choice is cleared; row order decides (%d bundle(s) "
                    "changed)", slot, r);
    return r;
}
