/* level_templates_sigs.c -- the signatures of level_templates.c, kept apart so the test links them without the
 * engine side. They cover the two entityDef list copies in idGameLocal::LoadMap's template walk; each matches
 * once in both executables. */
#include <stddef.h>
#include "level_templates.h"

const sig_entry SH_LEVEL_TEMPLATES_SIGNATURES[] = {
    { "SnapshotTemplateWalk",
      "C7 45 A0 00 00 87 00 4C 89 65 90 4C 89 65 98 45 33 C0 48 8D 55 90 48 8D 0D ?? ?? ?? ?? "
      "E8 ?? ?? ?? ?? 45 8B EC 44 39 65 98 0F 8E",
      0x39CD59u, 0x39C669u },
    { "SnapshotTemplateReWalk",
      "8B 05 ?? ?? ?? ?? 39 45 98 74 24 48 8B 56 08 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
      "45 33 C0 48 8D 55 90 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 48 85 DB",
      0x39CE6Bu, 0x39C77Bu },
    { NULL, NULL, 0, 0 }
};
