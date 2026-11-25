#pragma once
#include <stdint.h>
#include "modui.h"

/* idSnapMapEdit and its placed records, build 13954591 on both renderers. */
#define ARR_ENT_ARRAY_OFF      0x6a0        /* arrObj+0x6a0 -> entity-ptr array (8-byte entries) */
#define ARR_ENT_COUNT_OFF      0x6a8        /* arrObj+0x6a8 -> entity count (u32) */
#define LM_MODTABLE_OFF        0x750        /* loaded-map+0x750 -> module table (stride 0x98) */
#define MOD_STRIDE             0x98         /* module-table entry stride */
#define MOD_NAME_OFF           0x48         /* module entry+0x48 -> module name char* */
#define LM_INSTANCES_CNT_OFF   0x758        /* loaded-map+0x758 -> instances(modules) COUNT (== the no-module sentinel) */
#define ENT_VALID_OFF          0x8          /* entity[id]+8 != 0 => valid (the +0x28 rule) */
#define ENT_DEFSUB_OFF         0x158        /* entity[id]+0x158 -> def sub-object */
#define DECL_BLOB_B_OFF        0x38         /* Resolved source text pointer. */
#define DEFSUB_INHERIT_OFF      0x58       /* Interned inherit pointer slot. */
#define ENT_COUNT_CAP         1000000u      /* sanity cap on the entity array count */

const char *sh_editor_module_name(const uint8_t *records, int index);
/* Emit the placed entities and modules from the live edit. A failed read leaves the existing scope in force. */
int sh_editor_placed_names(const uint8_t *map, sh_modui_name_fn emit, void *context);
