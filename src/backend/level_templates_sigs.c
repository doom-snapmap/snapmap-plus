/* level_templates_sigs.c -- the private signatures of level_templates.c, kept apart so the two-image test
 * (tests/level_templates_sigs_test.c) links them without the engine side.
 *
 * Build 13954591, read in Ghidra (logs/shared-ghidra/ed1/): inside idGameLocal::LoadMap's snapshot-template walk,
 * the first copy of the entityDef resource list (mov [rbp-0x60],0x870000 and the cleared idList, then
 * xor r8d,r8d; lea rdx,[rbp-0x70]; lea rcx,[manager]; call copy; the count test) and the second copy after
 * "When creating template for entity def %s, additional entity defs were loaded" (the count compare, the warning
 * call, the same three argument loads and call, then the template's release). Both sequences are byte-identical in
 * the two executables apart from their relative operands, and each is the only match in each. */
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
