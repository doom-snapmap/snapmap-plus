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
#include "modserve.h"
#include "../common/text.h"

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

int sh_modregistry_choose_command(const char *slot, const char *mod, char **out)
{
    char salt[SH_SALT_LEN + 1] = "", *label = NULL;
    int r;
    if (!out) return -1;
    *out = NULL;
    if (!sh_modregistry_slot_valid(slot)) {
        *out = sh_textf("MODS: choose: '%s' is not a setting two mods disagree on. Copy it from the Mods tab's Choices "
                        "between mods.", slot ? slot : "");
        return -1;
    }
    if (mod && mod[0] && !sh_modserve_listed_mod(mod, salt, sizeof salt, &label)) {
        *out = sh_textf("MODS: choose: no mod named '%s' is used by this map.", mod);
        return -1;
    }
    r = sh_modregistry_choices_set(slot, salt);
    if (r < 0) *out = sh_text("MODS: choose: open a map in the editor first.");
    else if (salt[0])
        *out = sh_textf("MODS: choose: this map now uses %s's value for %s. Save the map to keep the choice.",
                        label ? label : salt, slot);
    else
        *out = sh_textf("MODS: choose: the choice for %s is cleared; the first mod in the map's list wins. Save the map "
                        "to keep this.", slot);
    free(label);
    return r;
}
