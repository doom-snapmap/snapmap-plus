#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "snapmap_plus_iface.h"

SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, config_get_json) == 0x2B0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, config_set_json) == 0x2B8);
/* Pin append-only interface offsets so adding slots cannot move existing callbacks. */
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, manipulation_in_progress) == 0x2C0);
/* Cached material lookup. */
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, find_material) == 0x2C8);
/* Consume the decoded preview as a PNG data URI. */
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, get_preview) == 0x2D0);
/* Queue preview decoding for a named material. */
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, request_preview) == 0x2D8);
/* Page the material catalog. */
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, list_materials) == 0x2E0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, list_assets) == 0x2E8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, material_rect) == 0x2F0);
/* Audition a soundshader; NULL stops playback. */
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, sound_preview) == 0x2F8);
/* Hold audition mode for the browser session. */
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, sound_session) == 0x300);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, resolve_prefab_model) == 0x308);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, request_prefab_mesh) == 0x310);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, get_prefab_mesh) == 0x318);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, resolve_prefab_defaults) == 0x320);
/* ext 24/25, appended 2026-09-07: the File menu's RAWMAP file surface -- report the staged load/save
 * paths, and point either side at a caller-chosen file. Append-only again. */
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, rawmap_status) == 0x328);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, rawmap_configure) == 0x330);
/* ext 26, appended 2026-09-07: reload the editor map now, so a staged rawmap actually opens. */
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, rawmap_load_now) == 0x338);
SH_STATIC_ASSERT(sizeof(sh_iface_vtbl) == 0x340);
SH_STATIC_ASSERT(offsetof(sh_iface, sub) == 0x58);
SH_STATIC_ASSERT(sizeof(sh_iface) == 0x60);

static int stub_get(sh_iface *self, const char *key, char *out_json,
                    int out_capacity, unsigned int *out_flags)
{
    static const char value[] = "\"dark\"";
    (void)self;
    (void)key;
    if (out_flags) *out_flags = SH_CONFIG_STATUS_REPAIRED;
    if (out_json && out_capacity >= (int)sizeof(value))
        memcpy(out_json, value, sizeof(value));
    return (int)sizeof(value) - 1;
}

static int stub_set(sh_iface *self, const char *key, const char *value_json)
{
    (void)self;
    return key && value_json ? SH_CONFIG_SET_PERSISTED : SH_CONFIG_SET_REJECTED;
}

int main(void)
{
    sh_iface_config_slots slots;
    sh_iface_engine_slots engine_slots;
    sh_iface *iface;
    char value[16] = {0};
    unsigned int flags = 0;

    memset(&slots, 0, sizeof(slots));
    slots.config_get_json = stub_get;
    slots.config_set_json = stub_set;

    iface = sh_iface_create();
    if (!iface || !iface->vtbl) {
        fprintf(stderr, "iface_config_test: interface creation failed\n");
        return 1;
    }
    sh_iface_bind_config_slots(&slots);
    memset(&engine_slots, 0, sizeof(engine_slots));
    sh_iface_bind_engine_slots(&engine_slots);
    if (iface->vtbl->config_get_json != stub_get ||
        iface->vtbl->config_set_json != stub_set) {
        fprintf(stderr, "iface_config_test: config callbacks were not preserved\n");
        return 1;
    }
    if (iface->vtbl->config_get_json(iface, "theme", value,
                                     (int)sizeof(value), &flags) != 6 ||
        strcmp(value, "\"dark\"") != 0 ||
        flags != SH_CONFIG_STATUS_REPAIRED) {
        fprintf(stderr, "iface_config_test: getter callback contract failed\n");
        return 1;
    }
    if (iface->vtbl->config_set_json(iface, "theme", "\"light\"") !=
        SH_CONFIG_SET_PERSISTED) {
        fprintf(stderr, "iface_config_test: setter callback contract failed\n");
        return 1;
    }

    puts("iface_config_test: OK");
    return 0;
}
