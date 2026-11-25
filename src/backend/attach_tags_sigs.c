/* attach_tags_sigs.c -- the signature of the engine's tag-info lookup (build 13954591), the function
 * AddAttachment calls with the host model's props table (idList at md6Def + 0x210). The 48-byte pattern is the
 * only match in each executable and its 15-byte detour window holds no relative operand. */
#include <stddef.h>
#include "attach_tags.h"

const sig_entry SH_ATTACH_TAGS_SIGNATURES[] = {
    { "TagInfoLookup",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 20 33 FF 4D 8B E0 "
      "4C 8B FA 4C 8B F1 8B EF 39 79 08 7E 29",
      0x16CE1E0u, 0x16CEE90u },
    { NULL, NULL, 0, 0 }
};
