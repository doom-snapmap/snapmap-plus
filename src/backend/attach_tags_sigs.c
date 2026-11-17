/* attach_tags_sigs.c -- the private signature of attach_tags.c (the engine's tag-info lookup), kept apart so
 * the two-image test (tests/attach_tags_sigs_test.c) and detour_windows_test link it without the engine side.
 *
 * Build 13954591 (paired with tools/re/site_pair.py): the lookup AddAttachment calls with the host model's props
 * table (idList at md6Def + 0x210), the attached md6Def's name and the tag's name; it returns a tag record whose
 * int16 at +0x1C is the joint index, -1 for no tag. The 48-byte pattern is the only match in each executable;
 * its 15-byte window (mov [rsp+8],rbx; mov [rsp+0x10],rbp; mov [rsp+0x18],rsi) holds no relative operand. A
 * single site, so no call-target check is needed. */
#include <stddef.h>
#include "attach_tags.h"

const sig_entry SH_ATTACH_TAGS_SIGNATURES[] = {
    { "TagInfoLookup",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 20 33 FF 4D 8B E0 "
      "4C 8B FA 4C 8B F1 8B EF 39 79 08 7E 29",
      0x16CE1E0u, 0x16CEE90u },
    { NULL, NULL, 0, 0 }
};
