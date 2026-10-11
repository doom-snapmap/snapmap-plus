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
/* ext 27, appended 2026-09-23: the navigation view the View menu updates. */
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, navmesh_view) == 0x340);
/* ext 28: run a call on DOOM's main thread and wait for it. */
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, run_on_main) == 0x348);
/* ext 29: delete entities through the native Delete Selection action. */
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, delete_entities) == 0x350);
SH_STATIC_ASSERT(sizeof(sh_iface_vtbl) == 0x358);
SH_STATIC_ASSERT(sizeof(sh_delete_result) == 40);
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

static int stub_delete(sh_iface *self, const int *ids, int count, sh_delete_result *out)
{
    (void)self; (void)ids;
    if (out) { memset(out, 0, sizeof *out); out->status = SH_DELETE_DISPATCHED; out->released = count; }
    return SH_DELETE_DISPATCHED;
}

/* A frontend must never read a slot the backend's vtable does not have. */
static int check_vtable_size_contract(sh_iface *iface)
{
    static sh_iface legacy;                     /* an older backend leaves +0x08 zero */
    static unsigned char old_vtbl[sizeof(sh_iface_vtbl)];
    memset(&legacy, 0, sizeof legacy);
    memset(old_vtbl, 0xCC, sizeof old_vtbl);    /* bytes past an old table are not callbacks */
    legacy.vtbl = (const sh_iface_vtbl *)(const void *)old_vtbl;
    if (sh_iface_vtbl_bytes(&legacy) != SH_IFACE_LEGACY_VTBL_BYTES) return 0;
    if (SH_IFACE_HAS_SLOT(&legacy, delete_entities)) return 0;
    if (!SH_IFACE_HAS_SLOT(&legacy, run_on_main)) return 0;   /* nonzero filler counts as bound */
    if (sh_iface_vtbl_bytes(NULL) != 0) return 0;
    if (sh_iface_vtbl_bytes(iface) != (uint32_t)sizeof(sh_iface_vtbl)) return 0;
    return 1;
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

    if (!check_vtable_size_contract(iface)) {
        fprintf(stderr, "iface_config_test: vtable size marker contract failed\n");
        return 1;
    }
    if (SH_IFACE_HAS_SLOT(iface, delete_entities)) {
        fprintf(stderr, "iface_config_test: unbound delete slot reported present\n");
        return 1;
    }
    engine_slots.delete_entities = stub_delete;
    sh_iface_bind_engine_slots(&engine_slots);
    {
        sh_delete_result r;
        int ids[2] = { 4, 5 };
        if (!SH_IFACE_HAS_SLOT(iface, delete_entities) ||
            iface->vtbl->delete_entities(iface, ids, 2, &r) != SH_DELETE_DISPATCHED ||
            r.released != 2) {
            fprintf(stderr, "iface_config_test: delete slot was not bound\n");
            return 1;
        }
    }

    puts("iface_config_test: OK");
    return 0;
}
