#include "package_native_inline.h"
#include "package_native_nameflow.h"

#include "config_json.h"
#include "decl_entity_class.h"
#include "decl_polymorphic.h"
#include "resource_types.h"
#include "map_binding_sites.h"
#include "decl_native_rebind.h"
#include "decl_native_lex.h"
#include "decl_text.h"
#include "decl_graph.h"
#include "decl_graph_dependencies.h"

#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ni_text { char *data; size_t length, capacity; } ni_text;
static int ni_append(ni_text *t, const char *bytes, size_t length)
{
    if (length > SIZE_MAX - t->length - 1) return 0;
    if (t->length + length + 1 > t->capacity) {
        size_t next = t->capacity ? t->capacity : 256;
        while (next < t->length + length + 1) {
            if (next > SIZE_MAX / 2) return 0;
            next *= 2;
        }
        char *grown = realloc(t->data, next);
        if (!grown) return 0;
        t->data = grown; t->capacity = next;
    }
    memcpy(t->data + t->length, bytes, length);
    t->length += length; t->data[t->length] = 0;
    return 1;
}
static int ni_append_text(ni_text *t, const char *text) { return ni_append(t, text, strlen(text)); }
static int ni_json_string_n(ni_text *t, const char *bytes, size_t length)
{
    if (!ni_append(t, "\"", 1)) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c < 0x20) {
            char escape[8]; snprintf(escape, sizeof(escape), "\\u%04x", c);
            if (!ni_append_text(t, escape)) return 0;
        } else if (c == '"' || c == '\\') {
            char escape[2] = {'\\', (char)c};
            if (!ni_append(t, escape, 2)) return 0;
        } else if (!ni_append(t, (const char *)&c, 1)) return 0;
    }
    return ni_append(t, "\"", 1);
}
static int ni_json_string(ni_text *t, const char *bytes) { return ni_json_string_n(t, bytes, strlen(bytes)); }

/* ---- native entity version upgrades ---- */
/* The native reader upgrades a copied value before choosing its inherit.
 * These are the complete finite selectors for entity carrier version 26.
 * They are not field-name heuristics or package-specific conversions. */
typedef enum ni_v_kind {
    NI_V_CLASS, NI_V_IDENTITY, NI_V_INHERIT, NI_V_MILLISECONDS,
    NI_V_STRING, NI_V_NESTED, NI_V_COLOR, NI_V_REMOVE, NI_V_BOOL
} ni_v_kind;
typedef struct ni_v_op {
    int before;
    ni_v_kind kind;
    const char *inherit, *field, *a, *b, *c;
} ni_v_op;
static const ni_v_op ni_v_operations[] = {
    {15, NI_V_CLASS, "snapmaps/moveable_props/battery", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/boot", "idProp", NULL, NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/bottle_b_lg_02_max", "snapmaps/moveable_props/bottle_b_lg_02_max", "idProp", NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/bottle_b_lg_max", "snapmaps/moveable_props/bottle_b_lg_max", "idProp", NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/bottle_c_lg_02_max", "snapmaps/moveable_props/bottle_c_lg_02_max", "idProp", NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/bottle_c_lg_max", "snapmaps/moveable_props/bottle_c_lg_max", "idProp", NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/box01", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/box02", "idProp", NULL, NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/bucket", "snapmaps/moveable_props/bucket", "idProp", NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/can_beer", "snapmaps/moveable_props/can_beer", "idProp", NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/can_soda", "snapmaps/moveable_props/can_soda", "idProp", NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/can_tea_max", "snapmaps/moveable_props/can_tea_max", "idProp", NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/cannister01", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/cart01", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/cart02", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/cleaning_bot", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/computer01", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/computer02", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/computer03", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/computer04", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/dolly", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/drill", "idProp", NULL, NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/fileholder", "snapmaps/moveable_props/fileholder", "idProp", NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/food_tray", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/fuel_can", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/glove", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/hammer", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/hard_hat", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/mop_bucket", "idProp", NULL, NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/noodles", "snapmaps/moveable_props/noodles", "idProp", NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/noodles2", "snapmaps/moveable_props/noodles2", "idProp", NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/noodles2_open", "snapmaps/moveable_props/noodles2_open", "idProp", NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/noodles_open", "snapmaps/moveable_props/noodles_open", "idProp", NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/office_mouse", "snapmaps/moveable_props/office_mouse", "idProp", NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/plasma_crate", "idProp", NULL, NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/bottle_a_lg_02_max", "snapmaps/moveable_props/bottle_a_lg_02_max", "idProp", NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/chip_container", "snapmaps/moveable_props/chip_container", "idProp", NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/printer", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/screwdriver", "idProp", NULL, NULL, NULL},
    {15, NI_V_IDENTITY, "prop/moveable/tin_small", "snapmaps/moveable_props/tin_small", "idProp", NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/trashcan", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/wet_floor_sign", "idProp", NULL, NULL, NULL},
    {15, NI_V_CLASS, "snapmaps/moveable_props/wrench", "idProp", NULL, NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/event/survive", "eventDurationMS", "eventDuration", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/capture_point/demon_signal", "animationTimeMS", "animationTime", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/damageable_props/default", "fxFadeOutMS", "fxFadeOutTime", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/damageable_props/default", "autoRespawnTimeMS", "autoRespawnTime", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/damageable_props/boxes01", "fxFadeOutMS", "fxFadeOutTime", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/damageable_props/boxes01", "autoRespawnTimeMS", "autoRespawnTime", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/damageable_props/boxes02", "fxFadeOutMS", "fxFadeOutTime", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/damageable_props/boxes02", "autoRespawnTimeMS", "autoRespawnTime", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/damageable_props/boxes03", "fxFadeOutMS", "fxFadeOutTime", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/damageable_props/boxes03", "autoRespawnTimeMS", "autoRespawnTime", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/hazards/laser", "damageInterval", "damageInterval", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/logic/repeater", "delayBetweenRepeats", "delayBetweenRepeats", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/explodeable_props/coop_spawner_explodable_barrel", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/explodeable_props/coop_spawner_explodable_barrel_plasma", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/explodeable_props/coop_spawner_explodable_barrel_radiation", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/explodeable_props/coop_spawner_explodable_barrel_sticky", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/explodeable_props/coop_spawner_explodable_barrel_uac", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_ammo_box", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_ammo_small", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_armor_10", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_armor_shield", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_armor_suit", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_become_demon", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_health_10", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_health_25", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_health_50", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_powerup_haste", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_powerup_invisibility", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_powerup_quad_damage", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_powerup_regeneration", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_supercharge", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/pickup_backpack", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_bfg", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_chaingun_mp_base", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_chaingun_mp_gatling", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_combat_shotgun_mp_base", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_combat_shotgun_mp_rocket", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_combat_shotgun_sp_burst", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_flare_rifle_mp_burn", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_gauss_rifle_mp_base", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_gauss_rifle_sp_charged_sniper", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_heavy_assault_rifle_mp_base", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_heavy_assault_rifle_mp_zoom", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_heavy_assault_rifle_sp_detonate", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_lighting_gun_mp_chain", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_plasma_rifle_mp_base", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_plasma_rifle_mp_field", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_plasma_rifle_sp_stun", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_repeater", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_rocket_launcher_mp_base", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_rocket_launcher_mp_detonate", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_super_shotgun", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_vortex_rifle_mp_charge", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_ammo", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_armor", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_backpack", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_droppable", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_health", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_powerup", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_weapon", "initalSpawnDelayMS", "initialSpawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/explodeable_props/coop_spawner_explodable_barrel", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/explodeable_props/coop_spawner_explodable_barrel_plasma", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/explodeable_props/coop_spawner_explodable_barrel_radiation", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/explodeable_props/coop_spawner_explodable_barrel_sticky", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/explodeable_props/coop_spawner_explodable_barrel_uac", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_ammo_box", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_ammo_small", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_armor_10", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_armor_shield", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_armor_suit", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_become_demon", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_health_10", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_health_25", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_health_50", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_powerup_haste", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_powerup_invisibility", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_powerup_quad_damage", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_powerup_regeneration", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/coop_pickup_spawner_supercharge", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/pickup_backpack", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_bfg", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_chaingun_mp_base", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_chaingun_mp_gatling", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_combat_shotgun_mp_base", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_combat_shotgun_mp_rocket", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_combat_shotgun_sp_burst", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_flare_rifle_mp_burn", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_gauss_rifle_mp_base", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_gauss_rifle_sp_charged_sniper", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_heavy_assault_rifle_mp_base", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_heavy_assault_rifle_mp_zoom", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_heavy_assault_rifle_sp_detonate", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_lighting_gun_mp_chain", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_plasma_rifle_mp_base", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_plasma_rifle_mp_field", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_plasma_rifle_sp_stun", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_repeater", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_rocket_launcher_mp_base", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_rocket_launcher_mp_detonate", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_super_shotgun", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_vortex_rifle_mp_charge", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_ammo", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_armor", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_backpack", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_droppable", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_health", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_powerup", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/spawners/spawn_weapon", "respawnDelayMS", "respawnDelay", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/message/message", "messageDuration", "messageDuration", NULL, NULL},
    {16, NI_V_MILLISECONDS, "snapmaps/logic/delay", "timerDurationMS", "timerDuration", NULL, NULL},
    {17, NI_V_STRING, "snapmaps/action/player/give_droppable", "droppableDecl", "power_core/power_core_normal", "flag/flag_blue", NULL},
    {17, NI_V_STRING, "snapmaps/action/player/take_droppable", "droppableDecl", "power_core/power_core_normal", "flag/flag_blue", NULL},
    {18, NI_V_NESTED, "snapmaps/settings/hud_settings", "topLeft", "type", "SNAP_HUD_TOP_LEADERBOARD", "SNAP_HUD_GAME_PAR_TIME"},
    {18, NI_V_NESTED, "snapmaps/settings/hud_settings", "topRight", "type", "SNAP_HUD_TOP_LEADERBOARD", "SNAP_HUD_GAME_PAR_TIME"},
    {18, NI_V_NESTED, "snapmaps/settings/hud_settings", "bottom", "type", "SNAP_HUD_TOP_LEADERBOARD", "SNAP_HUD_GAME_PAR_TIME"},
    {19, NI_V_INHERIT, "snapmaps/event/survive", "snapmaps/event/survive_deprecated", NULL, NULL, NULL},
    {20, NI_V_REMOVE, "snapmaps/spawners/spawn_armor", "spawnOnStart", NULL, NULL, NULL},
    {20, NI_V_REMOVE, "snapmaps/spawners/spawn_backpack", "spawnOnStart", NULL, NULL, NULL},
    {20, NI_V_REMOVE, "snapmaps/spawners/spawn_droppable", "spawnOnStart", NULL, NULL, NULL},
    {20, NI_V_REMOVE, "snapmaps/spawners/spawn_health", "spawnOnStart", NULL, NULL, NULL},
    {20, NI_V_REMOVE, "snapmaps/spawners/spawn_powerup", "spawnOnStart", NULL, NULL, NULL},
    {20, NI_V_REMOVE, "snapmaps/spawners/spawn_weapon", "spawnOnStart", NULL, NULL, NULL},
    {20, NI_V_REMOVE, "snapmaps/spawners/spawn_ammo", "spawnOnStart", NULL, NULL, NULL},
    {21, NI_V_CLASS, "snapmaps/visblockers/industrial/cap_01", "idSnapMapCapEntity", NULL, NULL, NULL},
    {22, NI_V_COLOR, "snapmaps/spawners/spawn_backpack", "colorParam", NULL, NULL, NULL},
    {24, NI_V_STRING, "snapmaps/message/codex_message", "image", "textures/guis/codex_images/uac_personnel/vega", "textures/guis/snapmaps/codex_images/vega", NULL},
    {25, NI_V_BOOL, "snapmaps/volume/blocking", "isVisible", "false", NULL, NULL},
    {26, NI_V_BOOL, "snapmaps/volume/blocking", "isNetworkStatic", "false", NULL, NULL},
};

#ifdef SH_NATIVE_INLINE_TESTING
static long ni_v_fail_after = -1;
void sh_native_inline_test_version_alloc_fail_after(long count) { ni_v_fail_after = count; }
#endif
static void *ni_v_alloc(size_t size)
{
#ifdef SH_NATIVE_INLINE_TESTING
    if (ni_v_fail_after == 0) return NULL;
    if (ni_v_fail_after > 0) ni_v_fail_after--;
#endif
    return malloc(size);
}
static char *ni_v_copy(const char *value, size_t length)
{
    if (length == SIZE_MAX) return NULL;
    char *copy = ni_v_alloc(length + 1);
    if (copy) { memcpy(copy, value, length); copy[length] = 0; }
    return copy;
}
/* Values here have already passed native JSON validation. Using the strict
 * settings setter would reject unchanged native byte strings. */
static int ni_v_set_n(sh_json_object *object, const char *key, size_t length, const char *value)
{
    char *copy = ni_v_copy(value, strlen(value));
    if (!copy) return 0;
    for (size_t i = 0; i < object->count; i++) {
        sh_json_member *member = object->members + i;
        if (member->key_length == length && !memcmp(member->key, key, length)) {
            free(member->value_json); member->value_json = copy; return 1;
        }
    }
    char *name = ni_v_copy(key, length);
    if (!name) { free(copy); return 0; }
    if (object->count == object->capacity) {
        size_t next = object->capacity ? object->capacity * 2 : 8;
        if (next < object->capacity || next > SIZE_MAX / sizeof(*object->members)) {
            free(name); free(copy); return 0;
        }
        sh_json_member *grown = ni_v_alloc(next * sizeof(*grown));
        if (!grown) { free(name); free(copy); return 0; }
        if (object->count) memcpy(grown, object->members, object->count * sizeof(*grown));
        free(object->members); object->members = grown; object->capacity = next;
    }
    object->members[object->count++] = (sh_json_member){name, length, copy};
    return 1;
}
static int ni_v_set(sh_json_object *object, const char *key, const char *value)
{ return ni_v_set_n(object, key, strlen(key), value); }
static void ni_v_remove(sh_json_object *object, const char *key)
{
    size_t length = strlen(key);
    for (size_t i = 0; i < object->count; i++) {
        sh_json_member *member = object->members + i;
        if (member->key_length != length || memcmp(member->key, key, length)) continue;
        free(member->key); free(member->value_json);
        memmove(member, member + 1, (object->count - i - 1) * sizeof(*member));
        object->count--; return;
    }
}
static int ni_v_clone(const sh_json_object *source, sh_json_object *copy)
{
    for (size_t i = 0; i < source->count; i++) {
        const sh_json_member *member = source->members + i;
        if (!ni_v_set_n(copy, member->key, member->key_length, member->value_json)) return 0;
    }
    return 1;
}
static char *ni_v_serialize(const sh_json_object *object)
{
    ni_text out = {0};
    if (!ni_append(&out, "{", 1)) goto fail;
    for (size_t i = 0; i < object->count; i++) {
        const sh_json_member *member = object->members + i;
        if ((i && !ni_append(&out, ",", 1)) || !ni_json_string_n(&out, member->key, member->key_length) ||
            !ni_append(&out, ":", 1) || !ni_append_text(&out, member->value_json)) goto fail;
    }
    if (!ni_append(&out, "}", 1)) goto fail;
    return out.data;
fail:
    free(out.data); return NULL;
}
static int ni_v_string(const char *json, char **out)
{
    size_t length = json ? strlen(json) : 0, decoded = 0;
    *out = NULL;
    if (!length || json[0] != '"') return 0;
    char *value = ni_v_alloc(length + 1);
    if (!value) return -1;
    if (!sh_native_json_decode_string(json, length, value, length + 1, &decoded) || strlen(value) != decoded) {
        free(value); return 0;
    }
    *out = value; return 1;
}
static int ni_v_set_string(sh_json_object *object, const char *key, const char *value)
{
    ni_text json = {0};
    int ok = ni_json_string(&json, value) && ni_v_set(object, key, json.data);
    free(json.data); return ok;
}
/* Json::Value's non-erroring asInt/asDouble domains. Integers and reals have
 * different native range checks; strings are never number coercions. */
static int ni_v_number(const char *json, int as_int, double *number)
{
    if (!json || !strcmp(json, "null") || !strcmp(json, "false")) { *number = 0; return 1; }
    if (!strcmp(json, "true")) { *number = 1; return 1; }
    if ((*json != '-' && (*json < '0' || *json > '9'))) return 0;
    char *end = NULL;
    int real = strpbrk(json, ".eE") != NULL;
    errno = 0;
    if (!real) {
        if (*json == '-') {
            int64_t value = _strtoi64(json, &end, 10);
            if (errno || !end || *end || (as_int && (value < -2147483647LL || value > 2147483646LL))) return 0;
            *number = (double)value;
        } else {
            uint64_t value = _strtoui64(json, &end, 10);
            if (errno || !end || *end || (as_int && value >= 2147483647ULL)) return 0;
            *number = (double)value;
        }
        return 1;
    }
    _locale_t locale = _create_locale(LC_NUMERIC, "C");
    if (!locale) return -1;
    errno = 0;
    double value = _strtod_l(json, &end, locale);
    int conversion_error = errno;
    _free_locale(locale);
    if (conversion_error || !end || *end || !_finite(value) ||
        (as_int && (value < -2147483648.0 || value > 2147483647.0))) return 0;
    *number = as_int ? (double)(int32_t)value : value;
    return 1;
}
/* A selected helper's non-object state is outside the proven mutation domain.
 * It retains the original package route rather than guessing native asserts. */
static int ni_v_object(const char *json, sh_json_object *object)
{
    if (!json || json[0] != '{') return 0;
    return sh_native_json_parse_object(json, strlen(json), 64, object) ? 1 : -1;
}
static int ni_v_edit_op(sh_json_object *definition, const ni_v_op *op)
{
    sh_json_object state = {0}, edit = {0}, outer = {0}, inner = {0};
    char *text = NULL, *value = NULL;
    double number = 0;
    int result = ni_v_object(sh_json_object_get(definition, "state"), &state), changed = 0;
    if (result != 1) goto done;
    result = ni_v_object(sh_json_object_get(&state, "edit"), &edit);
    if (result != 1) goto done;
    const char *old = sh_json_object_get(&edit, op->field);
    result = 1;
    switch (op->kind) {
    case NI_V_REMOVE:
        if (old) { ni_v_remove(&edit, op->field); changed = 1; }
        break;
    case NI_V_BOOL:
        if (old) ni_v_remove(&edit, op->field);
        else if (!ni_v_set(&edit, op->field, op->a)) { result = -1; goto done; }
        changed = 1; break;
    case NI_V_STRING:
        if (!old) {
            if (!ni_v_set_string(&edit, op->field, op->a)) { result = -1; goto done; }
            changed = 1;
        } else {
            result = ni_v_string(old, &value);
            if (result != 1) goto done;
            if (!strcmp(value, op->b)) { ni_v_remove(&edit, op->field); changed = 1; }
        }
        break;
    case NI_V_MILLISECONDS:
        if (!old) break;
        result = ni_v_number(old, 1, &number);
        if (result != 1) goto done;
        {
            char wrapped[64]; snprintf(wrapped, sizeof(wrapped), "{\"milliseconds\":%d}", (int32_t)number);
            ni_v_remove(&edit, op->field);
            if (!ni_v_set(&edit, op->a, wrapped)) { result = -1; goto done; }
        }
        changed = 1; break;
    case NI_V_NESTED:
        if (!old) break;
        result = ni_v_object(old, &outer);
        if (result != 1) goto done;
        old = sh_json_object_get(&outer, op->a);
        if (!old) break;
        result = ni_v_string(old, &value);
        if (result != 1) goto done;
        if (!strcmp(value, op->b)) {
            if (!ni_v_set_string(&outer, op->a, op->c) || !(text = ni_v_serialize(&outer)) ||
                !ni_v_set(&edit, op->field, text)) { result = -1; goto done; }
            free(text); text = NULL; changed = 1;
        }
        break;
    case NI_V_COLOR:
        if (!old) break;
        result = ni_v_object(old, &outer);
        if (result != 1) goto done;
        old = sh_json_object_get(&outer, "constantValue");
        if (!old) break;
        result = ni_v_object(old, &inner);
        if (result != 1) goto done;
        old = sh_json_object_get(&inner, "a");
        if (!old) break;
        result = ni_v_number(old, 0, &number);
        if (result != 1) goto done;
        if (number <= 0) {
            const char *keys[] = {"r", "g", "b", "a"};
            for (size_t i = 0; i < 4; i++)
                if (!ni_v_set(&inner, keys[i], "0")) { result = -1; goto done; }
            if (!(text = ni_v_serialize(&inner)) || !ni_v_set(&outer, "constantValue", text)) { result = -1; goto done; }
            free(text); text = NULL;
            if (!(text = ni_v_serialize(&outer)) || !ni_v_set(&edit, op->field, text)) { result = -1; goto done; }
            free(text); text = NULL; changed = 1;
        }
        break;
    default: result = 0; goto done;
    }
    if (changed) {
        if (!(text = ni_v_serialize(&edit)) || !ni_v_set(&state, "edit", text)) { result = -1; goto done; }
        free(text); text = NULL;
        if (!(text = ni_v_serialize(&state)) || !ni_v_set(definition, "state", text)) { result = -1; goto done; }
    }
done:
    free(text); free(value);
    sh_json_object_free(&state); sh_json_object_free(&edit);
    sh_json_object_free(&outer); sh_json_object_free(&inner);
    return result;
}
/* Exact derived copy: 1; retain original route: 0; allocation failure: -1.
 * This does not stamp the version. Only accepted inheritance conversion does. */
static int ni_v_upgrade(const sh_json_object *original, int target, sh_json_object *copy)
{
    double version_number = 0;
    int result = ni_v_number(sh_json_object_get(original, "~version"), 1, &version_number);
    if (result != 1) return result;
    if (target != 26 || version_number < 0 || version_number > 26) return 0;
    int version = (int)version_number;
    if (!ni_v_clone(original, copy)) return -1;
    if (version == target) return 1;
    char *inherit = NULL;
    result = ni_v_string(sh_json_object_get(copy, "inherit"), &inherit);
    if (result != 1) return result;
    for (size_t i = 0; i < sizeof(ni_v_operations) / sizeof(*ni_v_operations); i++) {
        const ni_v_op *op = ni_v_operations + i;
        if (version >= op->before || strcmp(inherit, op->inherit)) continue;
        switch (op->kind) {
        case NI_V_CLASS:
            if (!ni_v_set_string(copy, "className", op->field)) { result = -1; goto done; }
            break;
        case NI_V_IDENTITY:
            if (!ni_v_set_string(copy, "inherit", op->field) || !ni_v_set_string(copy, "className", op->a)) { result = -1; goto done; }
            free(inherit); inherit = NULL;
            result = ni_v_string(sh_json_object_get(copy, "inherit"), &inherit);
            if (result != 1) goto done;
            break;
        case NI_V_INHERIT:
            if (!ni_v_set_string(copy, "inherit", op->field)) { result = -1; goto done; }
            free(inherit); inherit = NULL;
            result = ni_v_string(sh_json_object_get(copy, "inherit"), &inherit);
            if (result != 1) goto done;
            break;
        default:
            result = ni_v_edit_op(copy, op);
            if (result != 1) goto done;
            break;
        }
    }
done:
    free(inherit); return result;
}
#ifdef SH_NATIVE_INLINE_TESTING
int sh_native_inline_test_version_copy(const char *json, int target, char **copy)
{
    sh_json_object original = {0}, derived = {0};
    *copy = NULL;
    if (!sh_native_json_parse_object(json, strlen(json), 64, &original)) return -1;
    int result = ni_v_upgrade(&original, target, &derived);
    if (result == 1 && !(*copy = ni_v_serialize(&derived))) result = -1;
    sh_json_object_free(&original); sh_json_object_free(&derived);
    return result;
}
#endif

/* ---- entities[i].entityDef object spans ---- */
typedef struct ni_span { size_t entity, offset, length; } ni_span;
/* An entity's uniqueId as the play map prints it (%u), when it is plain. */
typedef struct ni_unique { size_t entity; char text[24]; } ni_unique;
/* instanceEntities is an idIndexMultimap of uniqueIds: group k of keyValues
 * is module instance k, and the group after the last instance is the
 * GlobalModule (play map builder VK 1404ee8e0 / GL 1404ee2b0, module name
 * VK 1405a8a40 / GL 1405a8190). */
typedef struct ni_numbers { size_t *rows, count, capacity; int failed; } ni_numbers;
/* instances[k].moduleName and the loaded module name derived from it (NULL
 * when it is not one string or exceeds the modelled bound). */
typedef struct ni_module { size_t instance; char *name, *declaration; } ni_module;
/* One entry of the plan's holder substitution (ni_sigma_settle): a served
 * entity definition every map construction of which names its first native
 * ancestor, so play reads that ancestor's text in its place; or (drop) a
 * served loot drop that builds a native drop's object under the plan, named
 * in its place wherever a converted holder names it (ni_drop_keeps). */
typedef struct ni_sigma { char *from, *to; int drop; } ni_sigma;
typedef struct ni_scan {
    const char *json; size_t json_length; ni_span *spans; size_t count, capacity; int failed;
    ni_unique *uniques; size_t unique_count, unique_capacity;
    ni_numbers keys, values; size_t instances;
    ni_module *modules; size_t module_count, module_capacity;
    struct ni_served *served;   /* served declarations in actual use, collected once per document */
    struct ni_loot_closure *census;   /* what play reaches, constructs and observes, built once per document */
    ni_sigma *sigma; size_t sigma_count;   /* the current plan's holder substitution */
    char sigma_withdrawn[4096];             /* every entry a round removed, with its concrete reason */
    char plan_notes[4096];                  /* how its loot drop entries were decided, one per line (ni_plan_note) */
    char **sigma_relied; size_t sigma_relied_count;    /* "owner reference" of uses that converted relying on it */
    /* The current round's sites, whose first round_placed are its placed
     * occurrences, decided before its independent uses. */
    const sh_native_inline_report *round; size_t round_placed;
    /* Per served declaration (served->decls), whether play still reads it
     * once this round's converted placed copies replace their chains; NULL
     * when not established (ni_served_remaining). */
    char *round_remaining;
} ni_scan;
/* A loaded module's name (+0x48 of its palette wrapper), from the declaration
 * name that LoadSnapModule (VK 1405b7e80 / GL 1405b7570) loads. A placed
 * instance's moduleName equals that name exactly: the palette lookup (VK
 * 1405b7380 / GL 1405b6a70) compares it case-sensitively and the editor saves
 * it back from the declaration (VK 1405a8760). The loader strips the name's
 * extension (VK 141a02bc0) and every leading "maps/modules/" (VK 141a02cd0),
 * formats "maps/modules/palettes/%s/%s.bmodel" with the palette name into a
 * 16384-byte buffer (VK 1405eb010 / GL 1405eac00) and keeps that path's file
 * base (VK 1419ff880 / GL 1419f1ae0): its last component before the final
 * '.', or the whole component when that '.' starts it. The palette precedes
 * the stripped name's last separator, so only its length can matter; names
 * long enough to reach the buffer bound stay unknown. 1 named, 0 unknown, -1
 * failure. */
static int ni_module_base(const char *declaration, char **out)
{
    static const char prefix[] = "maps/modules/";
    size_t length = strlen(declaration), i = length, start = 0;
    *out = NULL;
    if (length > 8192) return 0;
    while (i && declaration[i - 1] != '/' && declaration[i - 1] != '\\' && declaration[i - 1] != '.') i--;
    if (i && declaration[i - 1] == '.') length = i - 1;
    while (length - start >= sizeof(prefix) - 1 && !strncmp(declaration + start, prefix, sizeof(prefix) - 1))
        start += sizeof(prefix) - 1;
    /* The file base of ".../<stripped>.bmodel". */
    size_t base = length;
    while (base > start && declaration[base - 1] != '/' && declaration[base - 1] != '\\') base--;
    char *name = base == length ? _strdup(".bmodel") : malloc(length - base + 1);
    if (!name) return -1;
    if (base != length) { memcpy(name, declaration + base, length - base); name[length - base] = 0; }
    *out = name;
    return 1;
}
static void ni_module_add(ni_scan *scan, size_t instance, const char *json, size_t length)
{
    char *text = NULL, *name = NULL;
    size_t decoded = 0;
    if (scan->module_count == scan->module_capacity) {
        size_t next = scan->module_capacity ? scan->module_capacity * 2 : 16;
        ni_module *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(scan->modules, next * sizeof(*grown)) : NULL;
        if (!grown) { scan->failed = 1; return; }
        scan->modules = grown; scan->module_capacity = next;
    }
    if (json && length >= 2 && length < SIZE_MAX && (text = malloc(length + 1)) == NULL) { scan->failed = 1; return; }
    if (text && sh_native_json_decode_string(json, length, text, length + 1, &decoded) && strlen(text) == decoded &&
        ni_module_base(text, &name) < 0) scan->failed = 1;
    scan->modules[scan->module_count++] = (ni_module){instance, name, name ? text : NULL};
    if (!name) free(text);
}
static void ni_number_add(ni_numbers *numbers, const char *json, size_t length)
{
    size_t value = 0, i = 0;
    for (; i < length && json[i] >= '0' && json[i] <= '9' && value < SIZE_MAX / 10; i++) value = value * 10 + (size_t)(json[i] - '0');
    if (!length || i != length) { numbers->failed = 1; return; }
    if (numbers->count == numbers->capacity) {
        size_t next = numbers->capacity ? numbers->capacity * 2 : 64;
        size_t *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(numbers->rows, next * sizeof(*grown)) : NULL;
        if (!grown) { numbers->failed = 1; return; }
        numbers->rows = grown; numbers->capacity = next;
    }
    numbers->rows[numbers->count++] = value;
}
static int ni_visit(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    ni_scan *scan = context;
    const sh_json_path *item = path ? path->parent : NULL, *list = item ? item->parent : NULL;
    if (path && !path->key && item && item->key && !item->parent && item->key_length == 9 &&
        !memcmp(item->key, "instances", 9) && kind == SH_JSON_OBJECT) { scan->instances++; return 1; }
    if (path && path->key && path->key_length == 10 && !memcmp(path->key, "moduleName", 10) && item && !item->key &&
        list && list->key && !list->parent && list->key_length == 9 && !memcmp(list->key, "instances", 9)) {
        ni_module_add(scan, item->index, kind == SH_JSON_STRING ? json : NULL, length);
        return !scan->failed;
    }
    if (path && !path->key && list && list->key && !list->parent && list->key_length == 16 &&
        !memcmp(list->key, "instanceEntities", 16) && item->key) {
        ni_numbers *numbers = item->key_length == 9 && !memcmp(item->key, "keyValues", 9) ? &scan->keys :
            item->key_length == 6 && !memcmp(item->key, "values", 6) ? &scan->values : NULL;
        if (numbers) { if (kind == SH_JSON_NUMBER) ni_number_add(numbers, json, length); else numbers->failed = 1; }
        return 1;
    }
    if (!item || item->key || !list || !list->key || list->key_length != 8 ||
        memcmp(list->key, "entities", 8) || list->parent || !path->key) return 1;
    if (kind == SH_JSON_NUMBER && path->key_length == 8 && !memcmp(path->key, "uniqueId", 8)) {
        size_t digits = 0;
        while (digits < length && json[digits] >= '0' && json[digits] <= '9') digits++;
        if (digits != length || !length || length >= sizeof(scan->uniques->text) || (length > 1 && json[0] == '0')) return 1;
        if (scan->unique_count == scan->unique_capacity) {
            size_t next = scan->unique_capacity ? scan->unique_capacity * 2 : 64;
            ni_unique *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(scan->uniques, next * sizeof(*grown)) : NULL;
            if (!grown) { scan->failed = 1; return 0; }
            scan->uniques = grown; scan->unique_capacity = next;
        }
        ni_unique *row = scan->uniques + scan->unique_count++;
        row->entity = item->index; memcpy(row->text, json, length); row->text[length] = 0;
        return 1;
    }
    if (kind != SH_JSON_OBJECT || path->key_length != 9 || memcmp(path->key, "entityDef", 9)) return 1;
    if (scan->count == scan->capacity) {
        size_t next = scan->capacity ? scan->capacity * 2 : 64;
        ni_span *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(scan->spans, next * sizeof(*grown)) : NULL;
        if (!grown) { scan->failed = 1; return 0; }
        scan->spans = grown; scan->capacity = next;
    }
    scan->spans[scan->count++] = (ni_span){item->index, (size_t)(json - scan->json), length};
    return 1;
}
static int ni_span_order(const void *a, const void *b)
{
    const ni_span *x = a, *y = b;
    return x->offset < y->offset ? -1 : x->offset > y->offset;
}

/* ---- declaration trees ---- */
static sh_decl_node *ni_node(const char *key, const char *value, int compound)
{
    sh_decl_node *node = calloc(1, sizeof(*node));
    if (!node) return NULL;
    node->compound = compound; node->assignment = 1;
    /* A root keeps a NULL key, as the declaration writer requires. */
    if ((key && !(node->key = _strdup(key))) || (value && !(node->value = _strdup(value)))) {
        sh_decl_tree_free(node); return NULL;
    }
    return node;
}

/* Instance leaves keep their native JSON; every other leaf is package-authored. */
typedef struct ni_mark { const sh_decl_node *node; char *json; } ni_mark;
typedef struct ni_marks { ni_mark *rows; size_t count, capacity; int sorted; } ni_marks;
static int ni_mark_add(ni_marks *marks, const sh_decl_node *node, const char *json, size_t length)
{
    if (marks->count == marks->capacity) {
        size_t next = marks->capacity ? marks->capacity * 2 : 64;
        ni_mark *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(marks->rows, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        marks->rows = grown; marks->capacity = next;
    }
    char *copy = malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, json, length); copy[length] = 0;
    marks->rows[marks->count++] = (ni_mark){node, copy}; marks->sorted = 0;
    return 1;
}
static int ni_mark_order(const void *a, const void *b)
{
    uintptr_t x = (uintptr_t)((const ni_mark *)a)->node, y = (uintptr_t)((const ni_mark *)b)->node;
    return x < y ? -1 : x > y;
}
static ni_mark *ni_mark_find(ni_marks *marks, const sh_decl_node *node)
{
    if (!marks->sorted && marks->count > 1) qsort(marks->rows, marks->count, sizeof(*marks->rows), ni_mark_order);
    marks->sorted = 1;
    size_t lo = 0, hi = marks->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uintptr_t at = (uintptr_t)marks->rows[mid].node;
        if (at == (uintptr_t)node) return marks->rows + mid;
        if (at < (uintptr_t)node) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
static void ni_marks_free(ni_marks *marks)
{
    for (size_t i = 0; i < marks->count; i++) free(marks->rows[i].json);
    free(marks->rows); memset(marks, 0, sizeof(*marks));
}
static int ni_record(void *context, const sh_decl_node *leaf, const char *json, size_t length)
{ return ni_mark_add(context, leaf, json, length); }

/* Deep copy; instance marks follow copied leaves when marks is supplied. */
static sh_decl_node *ni_copy(const sh_decl_node *source, ni_marks *marks)
{
    typedef struct frame { const sh_decl_node *from; sh_decl_node *to; } frame;
    frame *stack = NULL; size_t count = 0, capacity = 0;
    sh_decl_node *root = ni_node(source->key, source->value, source->compound);
    if (!root) return NULL;
    root->reset = source->reset; root->assignment = source->assignment;
    if (marks && !source->compound) {
        ni_mark *mark = ni_mark_find(marks, source);
        if (mark && !ni_mark_add(marks, root, mark->json, strlen(mark->json))) goto fail;
    }
    if (source->compound) {
        if (!(stack = malloc(16 * sizeof(*stack)))) goto fail;
        capacity = 16; stack[count++] = (frame){source, root};
    }
    while (count) {
        frame current = stack[--count];
        sh_decl_node **tail = &current.to->children;
        for (const sh_decl_node *child = current.from->children; child; child = child->next) {
            sh_decl_node *copy = ni_node(child->key, child->value, child->compound);
            if (!copy) goto fail;
            copy->reset = child->reset; copy->assignment = child->assignment;
            if (child->annotation && !(copy->annotation = _strdup(child->annotation))) { sh_decl_tree_free(copy); goto fail; }
            *tail = copy; tail = &copy->next;
            if (marks && !child->compound) {
                ni_mark *mark = ni_mark_find(marks, child);
                if (mark && !ni_mark_add(marks, copy, mark->json, strlen(mark->json))) goto fail;
            }
            if (child->compound) {
                if (count == capacity) {
                    frame *grown = capacity <= SIZE_MAX / 2 / sizeof(*stack) ? realloc(stack, capacity * 2 * sizeof(*stack)) : NULL;
                    if (!grown) goto fail;
                    stack = grown; capacity *= 2;
                }
                stack[count++] = (frame){child, copy};
            }
        }
    }
    free(stack); return root;
fail:
    free(stack); sh_decl_tree_free(root); return NULL;
}

/* Textual overlay of one later layer's members in the native merge order
 * (VK 0x141a65820 / GL 0x141a57cf0 with the flag set): each occurrence, in
 * source order, updates the first case-sensitively equal member (VK 0x141a63af0
 * / GL 0x141a55fc0) or appends (VK 0x141a65410 / GL 0x141a578e0), and a block's
 * children then merge the same way, depth first, before its next sibling. A
 * new block therefore collapses its own repeats too. The equivalence check
 * afterwards proves whether one layer reproduces the chain. */
static const char *ni_overlay(sh_decl_node *target, const sh_decl_node *source, ni_marks *marks)
{
    typedef struct frame { sh_decl_node *to; const sh_decl_node *next; } frame;
    frame *stack = malloc(16 * sizeof(*stack)); size_t count = 0, capacity = 16;
    const char *failure = NULL;
    if (!stack) return "allocation failed";
    stack[count++] = (frame){target, source->children};
    while (count && !failure) {
        const sh_decl_node *child = stack[count - 1].next;
        sh_decl_node *to = stack[count - 1].to, *into = NULL;
        if (!child) { count--; continue; }
        stack[count - 1].next = child->next;
        sh_decl_node **slot = &to->children;
        while (*slot && strcmp((*slot)->key, child->key)) {
            if (!_stricmp((*slot)->key, child->key)) { failure = "differently cased member spellings"; break; }
            slot = &(*slot)->next;
        }
        if (failure) break;
        if (!*slot) {
            if (!child->compound) {
                sh_decl_node *copy = ni_copy(child, marks);
                if (!copy) { failure = "allocation failed"; break; }
                *slot = copy; continue;
            }
            sh_decl_node *shell = ni_node(child->key, child->value, 1);
            if (!shell || (child->annotation && !(shell->annotation = _strdup(child->annotation)))) {
                sh_decl_tree_free(shell); failure = "allocation failed"; break;
            }
            shell->reset = child->reset; shell->assignment = child->assignment;
            *slot = into = shell;
        } else {
            if ((*slot)->compound != child->compound) { failure = "a member changes between block and value"; break; }
            (*slot)->assignment = child->assignment; (*slot)->reset = child->reset;
            if (!child->compound) {
                char *value = _strdup(child->value ? child->value : "");
                if (!value) { failure = "allocation failed"; break; }
                free((*slot)->value); (*slot)->value = value;
                if (marks) {
                    ni_mark *mark = ni_mark_find(marks, child);
                    if (mark && !ni_mark_add(marks, *slot, mark->json, strlen(mark->json))) failure = "allocation failed";
                }
                continue;
            }
            into = *slot;
        }
        if (count == capacity) {
            frame *grown = capacity <= SIZE_MAX / 2 / sizeof(*stack) ? realloc(stack, capacity * 2 * sizeof(*stack)) : NULL;
            if (!grown) { failure = "allocation failed"; break; }
            stack = grown; capacity *= 2;
        }
        stack[count++] = (frame){into, child->children};
    }
    free(stack); return failure;
}

/* 1 with the path of the first member repeated at any depth below node, 0
 * none, -1 failure. */
static int ni_repeated(const sh_decl_node *node, char *path, size_t capacity)
{
    typedef struct frame { const sh_decl_node *node; char *path; } frame;
    frame *stack = malloc(16 * sizeof(*stack)); size_t count = 0, size = 16;
    int found = 0;
    char *root = _strdup("");
    if (!stack || !root) { free(stack); free(root); return -1; }
    stack[count++] = (frame){node, root};
    while (count && found == 0) {
        frame current = stack[--count];
        for (const sh_decl_node *child = current.node->children; child && !found; child = child->next)
            for (const sh_decl_node *later = child->next; later; later = later->next) if (!strcmp(later->key, child->key)) {
                snprintf(path, capacity, "%s%s%s", current.path, *current.path ? "/" : "", child->key);
                found = 1; break;
            }
        for (const sh_decl_node *child = current.node->children; child && !found; child = child->next) {
            if (!child->compound) continue;
            size_t length = strlen(current.path) + strlen(child->key) + 2;
            char *nested = malloc(length);
            frame *grown = count < size ? stack : size <= SIZE_MAX / 2 / sizeof(*stack) ? realloc(stack, size * 2 * sizeof(*stack)) : NULL;
            if (!nested || !grown) { free(nested); found = -1; break; }
            if (grown != stack || count == size) { stack = grown; size *= 2; }
            snprintf(nested, length, "%s%s%s", current.path, *current.path ? "/" : "", child->key);
            stack[count++] = (frame){child, nested};
        }
        free(current.path);
    }
    while (count) free(stack[--count].path);
    free(stack); return found;
}

/* Structural equality of two expanded states, insensitive to the order of
 * distinct members; repeats of one name pair in occurrence order, since each
 * occurrence is read in turn. */
static int ni_equal(const sh_decl_node *a, const sh_decl_node *b)
{
    typedef struct frame { const sh_decl_node *a, *b; } frame;
    frame *stack = malloc(16 * sizeof(*stack)); size_t count = 0, capacity = 16; int equal = 1;
    if (!stack) return -1;
    stack[count++] = (frame){a, b};
    while (count && equal) {
        frame current = stack[--count];
        size_t left = 0, right = 0;
        if (current.a->compound != current.b->compound || current.a->reset != current.b->reset) { equal = 0; break; }
        if (!current.a->compound) {
            equal = !strcmp(current.a->value ? current.a->value : "", current.b->value ? current.b->value : "");
            continue;
        }
        for (const sh_decl_node *child = current.a->children; child; child = child->next) left++;
        for (const sh_decl_node *child = current.b->children; child; child = child->next) right++;
        if (left != right) { equal = 0; break; }
        for (const sh_decl_node *child = current.a->children; child && equal; child = child->next) {
            size_t occurrence = 0;
            for (const sh_decl_node *earlier = current.a->children; earlier != child; earlier = earlier->next)
                if (!strcmp(earlier->key, child->key)) occurrence++;
            const sh_decl_node *other = current.b->children;
            for (; other; other = other->next) if (!strcmp(other->key, child->key) && !occurrence--) break;
            if (!other) { equal = 0; break; }
            if (count == capacity) {
                frame *grown = capacity <= SIZE_MAX / 2 / sizeof(*stack) ? realloc(stack, capacity * 2 * sizeof(*stack)) : NULL;
                if (!grown) { free(stack); return -1; }
                stack = grown; capacity *= 2;
            }
            stack[count++] = (frame){child, other};
        }
    }
    free(stack); return equal;
}

/* ---- source adapters ---- */
typedef struct ni_class_view { const sh_native_inline_source *source; } ni_class_view;
static int ni_class_read(void *context, const char *name, sh_decl_source *text)
{
    const ni_class_view *view = context; int served = 0;
    return view->source->read(view->source->context, name, text, &served, NULL);
}
static int ni_class_derives(void *context, const char *class_name, const char *base_name)
{
    const ni_class_view *view = context;
    return view->source->derives(view->source->context, class_name, base_name);
}

/* ---- typed walk ---- */
typedef struct ni_type { const sh_decl_node *node; const char *name; } ni_type;
typedef struct ni_gap { char *path; const char *reason; } ni_gap;
typedef struct ni_walker {
    const sh_decl_dependency_schema *inner;
    ni_type *types; size_t type_count, type_capacity;
    ni_gap *gaps; size_t gap_count, gap_capacity;
    int failed;
} ni_walker;
static int ni_describe(void *c, sh_decl_value_type type, sh_decl_value_shape *shape)
{ const ni_walker *w = c; return w->inner->describe(w->inner->context, type, shape); }
static int ni_field(void *c, sh_decl_value_type object, const char *key, sh_decl_value_type *field)
{ const ni_walker *w = c; return w->inner->field(w->inner->context, object, key, field); }
static int ni_dynamic(void *c, sh_decl_value_type base, const char *name, size_t length, sh_decl_value_type *selected)
{ const ni_walker *w = c; return w->inner->dynamic_type ? w->inner->dynamic_type(w->inner->context, base, name, length, selected) : 0; }
static int ni_reference(void *c, const char *path, sh_decl_value_type type, const char *name, size_t length)
{ (void)c; (void)path; (void)type; (void)name; (void)length; return 1; }
static void ni_walk_gap(void *c, const char *path, sh_decl_value_type type, const char *reason)
{
    ni_walker *w = c; (void)type;
    if (w->gap_count == w->gap_capacity) {
        size_t next = w->gap_capacity ? w->gap_capacity * 2 : 16;
        ni_gap *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(w->gaps, next * sizeof(*grown)) : NULL;
        if (!grown) { w->failed = 1; return; }
        w->gaps = grown; w->gap_capacity = next;
    }
    char *copy = _strdup(path ? path : "");
    if (!copy) { w->failed = 1; return; }
    w->gaps[w->gap_count++] = (ni_gap){copy, reason};
}
static int ni_value(void *c, const char *path, sh_decl_value_type type, const sh_decl_node *node)
{
    ni_walker *w = c; (void)path;
    if (w->type_count == w->type_capacity) {
        size_t next = w->type_capacity ? w->type_capacity * 2 : 64;
        ni_type *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(w->types, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        w->types = grown; w->type_capacity = next;
    }
    w->types[w->type_count++] = (ni_type){node, type.ops && *type.ops ? NULL : type.name};
    return 1;
}
static void ni_walker_free(ni_walker *w)
{
    for (size_t i = 0; i < w->gap_count; i++) free(w->gaps[i].path);
    free(w->gaps); free(w->types);
}

typedef struct ni_emit { ni_walker *walker; ni_marks *marks; } ni_emit;
static void ni_leaf(void *context, const sh_decl_node *leaf, sh_decl_carrier_leaf *info)
{
    ni_emit *emit = context;
    ni_mark *mark = ni_mark_find(emit->marks, leaf);
    if (mark) { info->json = mark->json; return; }
    for (size_t i = 0; i < emit->walker->type_count; i++)
        if (emit->walker->types[i].node == leaf) { info->type = emit->walker->types[i].name; return; }
}

/* A package-authored leaf at or below a walk gap cannot be bound after
 * conversion; instance leaves already had that coverage natively. */
typedef struct ni_path_frame { const sh_decl_node *node; size_t length; } ni_path_frame;
static const ni_gap *ni_authored_gap(const sh_decl_node *root, ni_marks *marks, const ni_walker *walker)
{
    char path[512];
    ni_path_frame *stack = malloc(16 * sizeof(*stack)); size_t count = 0, capacity = 16;
    const ni_gap *found = NULL;
    if (!stack) return walker->gap_count ? walker->gaps : NULL;
    for (const sh_decl_node *child = root->children; child; child = child->next)
        if (!strcmp(child->key, "edit")) stack[count++] = (ni_path_frame){child, 0};
    path[0] = 0;
    while (count && !found) {
        ni_path_frame current = stack[--count];
        char own[512];
        snprintf(own, sizeof(own), "%.*s%s%s", (int)current.length, path, current.length ? "." : "", current.node->key);
        if (!current.node->compound) {
            if (!ni_mark_find(marks, current.node))
                for (size_t g = 0; g < walker->gap_count && !found; g++) {
                    size_t gl = strlen(walker->gaps[g].path);
                    if (!strncmp(own, walker->gaps[g].path, gl) && (own[gl] == 0 || own[gl] == '.')) found = walker->gaps + g;
                }
            continue;
        }
        snprintf(path, sizeof(path), "%s", own);
        for (const sh_decl_node *child = current.node->children; child; child = child->next) {
            if (count == capacity) {
                ni_path_frame *grown = realloc(stack, capacity * 2 * sizeof(*stack));
                if (!grown) { free(stack); return walker->gap_count ? walker->gaps : NULL; }
                stack = grown; capacity *= 2;
            }
            stack[count++] = (ni_path_frame){child, strlen(path)};
        }
    }
    free(stack); return found;
}

/* A reflected declaration pointer the converted layer states as map data
 * must name a source in this map's context. Declaration text leaves a name no
 * source defines to the engine's default by name (sg_native_default), but a
 * map's own value binds only to a source (sg_bind_typed), and an isolated
 * package's text never reads another package's definitions, so such a value
 * of the served chain cannot be carried and its use keeps its package; no
 * binding is assumed for it. A by-name or event form, an empty literal and an
 * instance leaf (marks: the map's own value, bound at its own site) are stated
 * as the map states them; a name the declaration search does not list
 * (another reader's resource) is left to the derived document's capture. */
typedef struct ni_absent_walk {
    ni_walker base;             /* first: the ni_* adapters read it */
    const sh_native_inline_source *source; ni_marks *marks;
    char name[256], path[256];  /* the first such value, "" for none */
} ni_absent_walk;
static int ni_absent_site(void *c, const char *path, sh_decl_value_type type, const char *name, size_t length,
    const sh_decl_reference_site *site)
{
    ni_absent_walk *w = c;
    char value[512]; const char *family = NULL;
    if (*w->name || !length || length >= sizeof(value) || !type.name || !site || !site->node ||
        ni_mark_find(w->marks, site->node)) return 1;
    if (type.ops && (!strcmp(type.ops, SH_DECL_EVENT_DECL_USE_OPS) || !strcmp(type.ops, SH_DECL_BY_NAME_DECL_OPS))) return 1;
    memcpy(value, name, length); value[length] = 0;
    if (strlen(value) != length || !w->source->resolve_decl ||
        w->source->resolve_decl(w->source->context, type.name, value, &family) != 0) return 1;
    snprintf(w->name, sizeof(w->name), "%s", value);
    snprintf(w->path, sizeof(w->path), "%s", path ? path : "");
    return 1;
}

/* ---- one site ---- */
typedef struct ni_layer { char *name; sh_decl_node *tree; } ni_layer;

static void ni_refuse(sh_native_inline_site *site, sh_decl_carrier_limit limit, const char *format, const char *detail)
{
    site->converted = 0; site->limit = limit; site->conflict = 0;
    snprintf(site->reason, sizeof(site->reason), format, detail ? detail : "");
}

/* The source-tree model does not evaluate native defaults, new list slots,
 * resets or custom readers, so its doubt is never a native limit. Only the
 * native evaluator may accept such a candidate; otherwise the package stays
 * (GAP) with the evaluator's outcome. 1 accepts, 0 refused. */
static int ni_native_verdict(const sh_native_inline_source *source, sh_native_inline_site *site, const char *base,
    const sh_decl_node *target, const sh_decl_node *candidate, const sh_native_eval_reduction *reduction, const char *model)
{
    char detail[160] = "", reason[240];
    sh_native_eval_status status = SH_NATIVE_EVAL_UNAVAILABLE;
    if (source->evaluate)
        status = source->evaluate(source->context, site->entity, site->from, base, target, candidate, reduction, detail,
            sizeof(detail));
    if (status == SH_NATIVE_EVAL_EQUAL) return 1;
    const char *outcome = status == SH_NATIVE_EVAL_UNEQUAL ? "native evaluation found a different state" :
        status == SH_NATIVE_EVAL_UNSUPPORTED ? "native evaluation does not cover a value" :
        status == SH_NATIVE_EVAL_FAILED ? "native evaluation failed" : "native evaluation is unavailable";
    snprintf(reason, sizeof(reason), "%s (%s)%s%s", model, outcome, detail[0] ? ": " : "", detail);
    ni_refuse(site, SH_DECL_CARRIER_GAP, "%s", reason);
    return 0;
}

/* The native entity carrier (value writer VK 1409d1990 / GL 1409d1150,
 * reader VK 1409c6160 / GL 1409c59b0) stores only name, inherit, className
 * and state. Its reader sets className (VK 1417ae460) and rebuilds the text
 * (VK 1417ae560 / GL 1417a1040), which writes class only where it differs
 * from the inherited one, then reparses. State the class a value loads with
 * in the tree evaluated for it, as that rebuild does. 1 done, 0 failure. */
static int ni_state_class(sh_decl_node *tree, const char *class_name, const sh_decl_entity_class_source *classes)
{
    char error[256], *inherited = sh_decl_entity_tree_class(tree, classes, error, sizeof(error));
    int same = inherited && !strcmp(inherited, class_name);
    ni_text quoted = {0};
    sh_decl_node *node;
    free(inherited);
    if (same) return 1;
    if (!(node = ni_node("class", NULL, 0)) || !ni_append(&quoted, "\"", 1) || !ni_append_text(&quoted, class_name) ||
        !ni_append(&quoted, "\"", 1)) {
        sh_decl_tree_free(node); free(quoted.data); return 0;
    }
    node->value = quoted.data;
    node->next = tree->children->next; tree->children->next = node;
    return 1;
}

/* member is one of definition's positional headers (sh_decl_entity_split). */
static int ni_member_allowed(const sh_decl_node *definition, const sh_decl_node *member, const char **reason)
{
    if (!strcmp(member->key, "inherit") || !strcmp(member->key, "class") ||
        !strcmp(member->key, "editorVars") || !strcmp(member->key, "edit")) return 1;
    /* A definition's own pool settings: Parse does not inherit them, and the
     * entity carrier above does not store them, so they never reach an
     * inheriting map entity. Named uses of the definition keep it. */
    if ((!strcmp(member->key, "poolCount") || !strcmp(member->key, "poolGranularity")) && !member->compound &&
        member->value) return 1;
    /* Its own expansion mode likewise: it selects only the definition's own
     * prepared text. The carrier rebuild (VK 0x1417ae560 / GL 0x1417a1040)
     * never writes the flag, so a placed copy keeps the constructor's 1, and
     * its root-first merge (VK 0x1417aec10 / GL 0x1417a16f0) reads every
     * ancestor's own state slice without consulting the ancestor's flag. The
     * fold below is that merge. A value native Parse refuses, or one it would
 * hand to the preprocessor (sh_decl_entity_expands -1, -2), stays unconverted. */
    if (!strcmp(member->key, "expandInheritance") && sh_decl_entity_expands(definition) >= 0) return 1;
    *reason = member->key; return 0;
}

/* An entity definition's parent: its positional inherit header
 * (sh_decl_entity_split). A repeated or later inherit is state text. */
static char *ni_parent(const sh_decl_node *tree)
{
    sh_decl_entity_headers headers;
    const char *text; size_t length;
    sh_decl_entity_split(tree, &headers);
    if (!headers.inherit || !sh_decl_tree_literal(headers.inherit, &text, &length)) return NULL;
    char *copy = malloc(length + 1);
    if (copy) { memcpy(copy, text, length); copy[length] = 0; }
    return copy;
}

/* ---- the world's AI spawn pool (R-203, R-204) ---- */
/* SnapMap's play map builds its world from the literal
 * `world { inherit = "worldspawn"; edit = { } }` (VK 1404ee8e0 / GL 1404ee2b0),
 * so the world's aiSpawnPoolDecl is the effective worldspawn chain's; the
 * idWorldspawn constructor leaves it unset. Resurrection and spawn targets
 * (VK 14088b5e0 / 1412cff00) look up an enabled pool's first spawnPoolEntries
 * entityDef in a declaration's ancestry (VK 140691a00 / GL 140690980), unless
 * its own name is suppressed, then reuse that base's pooled instance and read
 * each layer below the base separately, root-first (R-202). They take that
 * route only while the cvars g_entityPoolMode (default 1) and ai_pool_enable
 * (default 0) allow it. Both are session settings another player can change,
 * so a configured pool must stay equivalent on either route. Unknown pool
 * scope is a product gap, never proof that no pool exists.
 * The base's instances come from its per-definition pool (world setup VK
 * 14069d930 -> VK 140828c30 / GL 140827a80 -> pool creation VK 1408293d0 / GL
 * 140828220): spawned one by one, or, while the cvar g_entityPoolTypeInfoClone
 * (default 0) is set, cloned from one template. Both read the base's prepared
 * state (+0x140), which the two forms share. Generic spawn's own-pool routes
 * (modes 1 and 2, including mode 2's per-type templates VK 1408291e0 / GL
 * 140828030) need the declaration's own poolCount, which a placed copy never
 * carries, so they reach neither form of a site. */
typedef enum ni_pool_state { NI_POOL_NONE, NI_POOL_SET, NI_POOL_UNRESOLVED } ni_pool_state;
typedef struct ni_pool {
    int resolved;
    ni_pool_state state;
    char **entries, **suppressed;      /* entityDef names in list order */
    size_t entry_count, suppressed_count;
    char reason[200];
} ni_pool;
static void ni_pool_free(ni_pool *pool)
{
    for (size_t i = 0; i < pool->entry_count; i++) free(pool->entries[i]);
    for (size_t i = 0; i < pool->suppressed_count; i++) free(pool->suppressed[i]);
    free(pool->entries); free(pool->suppressed);
    memset(pool, 0, sizeof(*pool));
}
static char *ni_node_literal(const sh_decl_node *node)
{
    const char *text; size_t length;
    if (!node || node->compound || node->reset || !sh_decl_tree_literal(node, &text, &length)) return NULL;
    char *copy = malloc(length + 1);
    if (copy) { memcpy(copy, text, length); copy[length] = 0; }
    return copy;
}
static int ni_has_reset(const sh_decl_node *node, int depth)
{
    if (depth > 64) return 1;
    for (; node; node = node->next) if (node->reset || ni_has_reset(node->children, depth + 1)) return 1;
    return 0;
}

/* The pool declaration's object. idDeclAIPoolNumbers starts enabled with
 * empty lists (constructor VK 14076e870). Its generic typeinfo Parse
 * (VK 1417bc4d0) finds an inherit with makeDefault 0, copies a found
 * parent's complete object by writing and reading it, then reads its own
 * edit over that copy: named members only; a list resizes on num, keeps its
 * prefix and clears new slots, and an item names an existing slot. */
typedef struct ni_slots { char **rows; size_t count; } ni_slots;
typedef struct ni_pool_object { int enabled; ni_slots entries, suppressed, projectiles; } ni_pool_object;
static void ni_slots_free(ni_slots *slots)
{
    for (size_t i = 0; i < slots->count; i++) free(slots->rows[i]);
    free(slots->rows); slots->rows = NULL; slots->count = 0;
}
static void ni_pool_object_free(ni_pool_object *object)
{
    ni_slots_free(&object->entries); ni_slots_free(&object->suppressed); ni_slots_free(&object->projectiles);
}
static int ni_slots_resize(ni_slots *slots, size_t count)
{
    for (size_t i = count; i < slots->count; i++) { free(slots->rows[i]); slots->rows[i] = NULL; }
    if (count > slots->count) {
        char **grown = realloc(slots->rows, count * sizeof(*grown));
        if (!grown) return -1;
        memset(grown + slots->count, 0, (count - slots->count) * sizeof(*grown));
        slots->rows = grown;
    }
    slots->count = count;
    return 1;
}
/* One own list value over the inherited slots, as the idList hook reads it
 * (VK 1409cd610 / GL 1409ccdd0): it scans the block for the first num member
 * and resizes before any item is read (a count below 1 empties the list);
 * the array reader (VK 141a202d0 / GL 141a12510) then overlays item[N] slots,
 * skips num members, and warns about and skips an index outside the count.
 * field names the declaration member of a record element, or NULL for plain
 * items; other lists only one of members. 1 done, 0 outside the model
 * (reason set), -1 failure. */
static int ni_pool_read_list(const sh_decl_node *list, const char *field, const char *other, ni_slots *slots,
    char *reason, size_t capacity)
{
    /* The hook expects "{" directly, so the native parser rejects a reset here. */
    if (list->reset) { snprintf(reason, capacity, "'%.40s' carries a reset, which the native list reader rejects", list->key); return 0; }
    if (!list->compound) { snprintf(reason, capacity, "'%.40s' is not a list", list->key); return 0; }
    for (const sh_decl_node *item = list->children; item; item = item->next) {
        if (strcmp(item->key, "num")) continue;
        char *text = item->compound || item->reset ? NULL : ni_node_literal(item), *end = NULL;
        long value = text ? strtol(text, &end, 10) : 0;
        int plain = text && *text && end && !*end && value <= 4096;
        free(text);
        if (!plain) { snprintf(reason, capacity, "'%.40s' num is not a plain count", list->key); return 0; }
        if (ni_slots_resize(slots, value < 1 ? 0 : (size_t)value) < 0) return -1;
        break;
    }
    for (const sh_decl_node *item = list->children; item; item = item->next) {
        char *end = NULL;
        if (!strcmp(item->key, "num")) continue;
        int indexed = !strncmp(item->key, "item[", 5);
        unsigned long index = indexed ? strtoul(item->key + 5, &end, 10) : 0;
        if (!indexed || !end || end == item->key + 5 || *end != ']' || end[1]) {
            snprintf(reason, capacity, "'%.40s' member '%.40s' is not modelled", list->key, item->key); return 0;
        }
        if (index >= slots->count) continue; /* "array element %d out of range": skipped */
        const sh_decl_node *value = item;
        if (field) {
            /* A record element is a class value: a reset before it has no
             * effect and its block overlays the slot's members. */
            if (!item->compound) { snprintf(reason, capacity, "'%.40s' item %lu is not a record", list->key, index); return 0; }
            value = NULL;
            for (const sh_decl_node *member = item->children; member; member = member->next) {
                if (!strcmp(member->key, field)) value = member;
                else if (!other || strcmp(member->key, other) || member->compound || member->reset) {
                    snprintf(reason, capacity, "'%.40s' record member '%.40s' is not modelled", list->key, member->key);
                    return 0;
                }
            }
            if (!value) continue;
        }
        if (value->reset || value->compound) {
            snprintf(reason, capacity, "'%.40s' item %lu is not a plain name", list->key, index); return 0;
        }
        const char *name; size_t length;
        if (!sh_decl_reference_literal(value, &name, &length)) {
            snprintf(reason, capacity, "'%.40s' item %lu is not a plain name", list->key, index); return 0;
        }
        char *copy = NULL;
        if (length) {
            if (!(copy = malloc(length + 1))) return -1;
            memcpy(copy, name, length); copy[length] = 0;
        }
        free(slots->rows[index]); slots->rows[index] = copy; /* an unquoted NULL clears the slot */
    }
    return 1;
}
/* The named pool declaration's effective object. depth 0 is the world's
 * pool, which must exist; an absent parent leaves the defaults, as Parse's
 * makeDefault-0 lookup does. Parse (VK 1417bc4d0 / GL 1417aefe0) reads a
 * leading inherit, then its own text only when that opens with `edit =` or
 * `data =` (matcher VK 141a24880 / GL 141a16c40); any other opening leaves
 * the copied or default object, and nothing after that block is read.
 * 1 resolved, 0 outside the model (reason set), -1 failure. */
static int ni_pool_object_read(const sh_native_inline_source *source, const char *name, int depth,
    ni_pool_object *object, char *reason, size_t capacity)
{
    sh_decl_source text = {0}; int served = 0, result = 0; char error[256];
    sh_decl_node *tree = NULL;
    if (depth > 32) { snprintf(reason, capacity, "AI spawn pool '%.120s' inherits without end", name); return 0; }
    int read = source->read_decl ? source->read_decl(source->context, "aipoolnumbers", name, &text, &served) : -1;
    if (read <= 0) {
        if (read == 0 && depth) { object->enabled = 1; return 1; }
        snprintf(reason, capacity, "the world names AI spawn pool '%.120s', which %s", name,
            read < 0 ? "cannot be read here" : "is absent");
        return 0;
    }
    tree = sh_decl_tree_parse(text, error, sizeof(error));
    free((char *)text.text);
    if (!tree) { snprintf(reason, capacity, "AI spawn pool '%.120s' does not parse", name); return 0; }
    const sh_decl_node *edit = NULL, *first = tree->children;
    object->enabled = 1;
    if (first && !strcmp(first->key, "inherit")) {
        char *parent = first->compound || first->reset ? NULL : ni_node_literal(first);
        if (!parent) { snprintf(reason, capacity, "AI spawn pool '%.120s' inherit is not a plain name", name); goto done; }
        int inherited = *parent ? ni_pool_object_read(source, parent, depth + 1, object, reason, capacity) : 1;
        free(parent);
        if (inherited <= 0) { result = inherited; goto done; }
        first = first->next;
    }
    if (first && (!strcmp(first->key, "edit") || !strcmp(first->key, "data"))) {
        if (!first->compound || first->reset) {
            snprintf(reason, capacity, "AI spawn pool '%.120s' %.10s is not a plain block", name, first->key); goto done;
        }
        edit = first;
    } else if (first && (!_stricmp(first->key, "edit") || !_stricmp(first->key, "data") || !strcmp(first->key, "inherit"))) {
        /* Keyword case and a repeated inherit are not established. */
        snprintf(reason, capacity, "AI spawn pool '%.120s' member '%.40s' is not modelled", name, first->key); goto done;
    }
    for (const sh_decl_node *member = edit ? edit->children : NULL; member; member = member->next) {
        int done = 1;
        if (!strcmp(member->key, "enabled")) {
            char *flag = member->compound || member->reset ? NULL : ni_node_literal(member);
            int on = flag && (!strcmp(flag, "true") || !strcmp(flag, "1")), off = flag && (!strcmp(flag, "false") || !strcmp(flag, "0"));
            free(flag);
            if (!on && !off) { snprintf(reason, capacity, "AI spawn pool '%.120s' enabled is not a plain flag", name); goto done; }
            object->enabled = on;
        } else if (!strcmp(member->key, "spawnPoolEntries")) {
            done = ni_pool_read_list(member, "entityDef", "poolSize", &object->entries, reason, capacity);
        } else if (!strcmp(member->key, "suppressedEntityDefNames")) {
            done = ni_pool_read_list(member, NULL, NULL, &object->suppressed, reason, capacity);
        } else if (!strcmp(member->key, "projectileEnts")) {
            done = ni_pool_read_list(member, NULL, NULL, &object->projectiles, reason, capacity);
        } else {
            snprintf(reason, capacity, "AI spawn pool '%.120s' member '%.40s' is not modelled", name, member->key);
            goto done;
        }
        if (done <= 0) { result = done; goto done; }
    }
    result = 1;
done:
    sh_decl_tree_free(tree);
    return result;
}
/* Nonempty slots in list order: an empty entry derives nothing and an empty
 * name is no declaration's name. 1 done, -1 failure. */
static int ni_pool_names(const ni_slots *slots, char ***out, size_t *count)
{
    *out = NULL; *count = 0;
    if (!slots->count) return 1;
    if (!(*out = calloc(slots->count, sizeof(**out)))) return -1;
    for (size_t i = 0; i < slots->count; i++)
        if (slots->rows[i] && *slots->rows[i] && !((*out)[(*count)++] = _strdup(slots->rows[i]))) return -1;
    return 1;
}
/* 1 resolved (state set), -1 operational failure. */
static int ni_pool_resolve(const sh_native_inline_source *source, ni_pool *pool)
{
    char *name = _strdup("worldspawn"), *pool_name = NULL, error[256];
    sh_decl_node *tree = NULL;
    ni_pool_object object = {0};
    int result = 1;
    pool->resolved = 1; pool->state = NI_POOL_UNRESOLVED;
    if (!name) return -1;
    {
        sh_decl_source text = {0}; int served = 0;
        int read = source->read(source->context, name, &text, &served, NULL);
        free((char *)text.text);
        if (read < 0) { free(name); return -1; }
        if (!read) { snprintf(pool->reason, sizeof(pool->reason), "world definition '%.120s' is unavailable", name); goto done; }
    }
    /* The world's state is its literal's expanded chain: positional headers,
     * the native merge order and only the first prepared statement. Its
     * reader reads each aiSpawnPoolDecl occurrence in turn; the last stays. */
    {
        static const char world[] = "{ inherit = \"worldspawn\"; edit = { } }";
        ni_class_view view = {source};
        sh_decl_entity_class_source classes = {&view, ni_class_read, ni_class_derives};
        sh_decl_node *literal = sh_decl_tree_parse_ordered((sh_decl_source){world, sizeof(world) - 1}, error, sizeof(error));
        tree = literal ? sh_decl_entity_expanded_state(literal, &classes, error, sizeof(error)) : NULL;
        sh_decl_tree_free(literal);
        if (!tree) { snprintf(pool->reason, sizeof(pool->reason), "the world chain is not modelled: %.180s", error); goto done; }
        const sh_decl_node *edit = tree->children;
        if (edit && (!edit->compound || edit->reset)) {
            snprintf(pool->reason, sizeof(pool->reason), "the world chain replaces its inherited state"); goto done;
        }
        for (const sh_decl_node *decl = edit ? edit->children : NULL; decl; decl = decl->next) {
            if (strcmp(decl->key, "aiSpawnPoolDecl")) continue;
            free(pool_name);
            if (!(pool_name = decl->compound || decl->reset ? NULL : ni_node_literal(decl))) {
                snprintf(pool->reason, sizeof(pool->reason), "the world's aiSpawnPoolDecl is not a plain name"); goto done;
            }
        }
    }
    free(name); name = NULL;
    if (!pool_name || !*pool_name) { pool->state = NI_POOL_NONE; goto done; }
    int read = ni_pool_object_read(source, pool_name, 0, &object, pool->reason, sizeof(pool->reason));
    if (read < 0) { result = -1; goto done; }
    if (!read) goto done;
    if (ni_pool_names(&object.entries, &pool->entries, &pool->entry_count) < 0 ||
        ni_pool_names(&object.suppressed, &pool->suppressed, &pool->suppressed_count) < 0) { result = -1; goto done; }
    /* Disabled, or with no entry no declaration finds a pool base. */
    pool->state = !object.enabled || !pool->entry_count ? NI_POOL_NONE : NI_POOL_SET;
    snprintf(pool->reason, sizeof(pool->reason), "%.120s", pool_name);
done:
    free(name); free(pool_name); sh_decl_tree_free(tree); ni_pool_object_free(&object);
    return result;
}
/* A placed copy's declaration is named `%u_%s/%s_%u` (module index, module,
 * inherit, the entity's uniqueId; MapEntityForSnapEntity VK 1404f1350 /
 * GL 1404f0c50; session dumps show `0_<module>/<inherit>_<uniqueId>`), so
 * converting its inherit renames it. A listed name can be this copy's only
 * when its inherit part and, when the uniqueId is known, its number match. */
/* A placed copy's identity parts the map states. module_index is NULL when
 * the map does not place the uniqueId in exactly one module group;
 * module_name is NULL when that instance's loaded module name is unknown. */
typedef struct ni_identity { const char *unique, *module_index, *module_name; char index_text[24]; } ni_identity;
static int ni_generated_name(const char *name, const char *inherit, const ni_identity *identity)
{
    const char *unique = identity ? identity->unique : NULL;
    size_t length = strlen(name), inherit_length = strlen(inherit), end = length;
    if (!inherit_length || !length || name[0] < '0' || name[0] > '9') return 0;
    while (end && name[end - 1] >= '0' && name[end - 1] <= '9') end--;
    if (end == length || !end || name[end - 1] != '_') return 0;
    if (unique && strcmp(name + end, unique)) return 0;
    end--;
    if (end < inherit_length + 1 || name[end - inherit_length - 1] != '/' ||
        _strnicmp(name + end - inherit_length, inherit, inherit_length)) return 0;
    const char *at = name;
    while (*at >= '0' && *at <= '9') at++;
    if (*at != '_' || (size_t)(at - name) >= end - inherit_length - 1) return 0;
    if (identity && identity->module_index &&
        ((size_t)(at - name) != strlen(identity->module_index) || strncmp(name, identity->module_index, (size_t)(at - name))))
        return 0;
    size_t module_length = end - inherit_length - 1 - (size_t)(at + 1 - name);
    return !identity || !identity->module_name || (strlen(identity->module_name) == module_length &&
        !_strnicmp(at + 1, identity->module_name, module_length));
}

/* ---- per-layer reads under a pooled base ---- */
/* Each folded layer's writes, by member path and native reader. The struct
 * reader (VK 141a209b0) assigns only the members a text names. A member read
 * by the generic scalar reader is a plain assignment; a block of a type with
 * no registered reader overlays its members. A registered reader assigns a
 * token form completely (ASSIGN, sh_decl_value_shape.assigns), changes only
 * its own field otherwise (FIELD), or also has effects outside it (GLOBAL,
 * sh_decl_value_shape.effects); the generic dispatch skips some forms with no
 * effect (SKIP); readers not modelled yet are refused.
 * SetEntityEditState (VK 1409d1ef0) reads each layer with its own call and
 * afterwards publishes the alias handles read during that call (only VK
 * 1409ca410 adds them, through 1409c5e90) against the render models the
 * idRenderModel pointer reader read during the same call (only VK 1409cdd20
 * adds them, through 1409c5f30). */
typedef enum ni_role { NI_ROLE_PLAIN, NI_ROLE_OBJECT, NI_ROLE_LIST, NI_ROLE_ASSIGN, NI_ROLE_FIELD, NI_ROLE_GLOBAL,
    NI_ROLE_SKIP, NI_ROLE_ACCESSOR } ni_role;
/* The bits a read stores, from the entity object, when every member on its
 * path is stored inline with a described footprint (R-213) or a verified
 * bitfield setter store (R-214); else unknown. */
typedef struct ni_extent { int known; long long start, length; } ni_extent;
typedef struct ni_write { char *path; size_t owner; ni_role role; size_t sequence; ni_extent extent; } ni_write;
typedef struct ni_writes { ni_write *rows; size_t count, capacity; } ni_writes;
/* A decimal number token as the lexer reads it (optional minus, digits, and
 * for a float an optional fraction); exponents and other forms stay readers. */
static int ni_decimal(const char *text, int integer)
{
    const char *at = text + (*text == '-');
    size_t digits = 0, fraction = 0;
    int dot = 0;
    while (*at >= '0' && *at <= '9') { at++; digits++; }
    if (!integer && *at == '.') for (dot = 1, at++; *at >= '0' && *at <= '9'; at++) fraction++;
    return digits && !*at && (!dot || fraction);
}
/* The node is the token form its reader assigns totally (sh_decl_assign_form):
 * the read's only effect is the assignment, so repeats keep the last. */
static int ni_assigned(const sh_decl_value_shape *shape, const sh_decl_node *node)
{
    const char *text = NULL; size_t length = 0;
    if (node->compound || node->reset || node->annotation || !node->value) return 0;
    switch (shape->assigns) {
    case SH_DECL_ASSIGN_QUOTED: return node->value[0] == '"' && sh_decl_tree_literal(node, &text, &length);
    case SH_DECL_ASSIGN_NUMBER: return ni_decimal(node->value, 0);
    case SH_DECL_ASSIGN_INTEGER: return ni_decimal(node->value, 1);
    case SH_DECL_ASSIGN_TOKEN: return node->value[0] != 0;
    case SH_DECL_ASSIGN_NULL: return !strcmp(node->value, "NULL");
    case SH_DECL_ASSIGN_BOOLEAN: return !strcmp(node->value, "true") || !strcmp(node->value, "false");
    default: return 0;
    }
}
static void ni_writes_free(ni_writes *writes)
{
    for (size_t i = 0; i < writes->count; i++) free(writes->rows[i].path);
    free(writes->rows); memset(writes, 0, sizeof(*writes));
}
static int ni_writes_add(ni_writes *writes, const char *path, size_t owner, ni_role role, ni_extent extent)
{
    if (!writes) return 1;
    if (writes->count == writes->capacity) {
        size_t next = writes->capacity ? writes->capacity * 2 : 16;
        ni_write *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(writes->rows, next * sizeof(*grown)) : NULL;
        if (!grown) return -1;
        writes->rows = grown; writes->capacity = next;
    }
    char *copy = _strdup(path);
    if (!copy) return -1;
    writes->rows[writes->count] = (ni_write){copy, owner, role, writes->count, extent};
    writes->count++;
    return 1;
}
/* ---- proved complete-assignment reductions (R-212) ----
 * The member walker (VK 0x141a209b0) reads every occurrence of a repeated
 * member in order, and the entity carrier keeps one value per key. Dropping
 * an earlier occurrence is equivalent only when the last one assigns its
 * field completely from its token (a plain scalar, or the ASSIGN form of its
 * registered reader), every dropped one is such an assignment or a native
 * no-op, and nothing read between them can observe that storage: the reads
 * there must themselves only assign their own storage from their tokens,
 * since distinct names are not proved to occupy distinct storage. A count or
 * indexed item of the inheritance post-pass (VK 0x141a65fa0 / GL 0x141a58470)
 * is never reduced. */
typedef enum ni_effect { NI_EFFECT_OTHER, NI_EFFECT_INERT, NI_EFFECT_TOTAL, NI_EFFECT_BLOCK, NI_EFFECT_LIST } ni_effect;
/* The post-pass counts a block's first member passing the case-sensitive
 * three-character compare with "num" (VK 1419fec20 / GL 1419f0e80): an
 * appended layer counts its first occurrence, the reduction would keep the
 * last. It then removes each member passing the same compare with "item" and
 * ending in ']' whose index is at or above the count; other names, such as a
 * plain itemCount or Item[0], are never pruned and reduce like any member. */
static int ni_counted(const char *key)
{
    size_t length = strlen(key);
    return !strncmp(key, "num", 3) || (!strncmp(key, "item", 4) && key[length - 1] == ']');
}
static int ni_counts(const sh_decl_node *block)
{
    for (const sh_decl_node *child = block->children; child; child = child->next) if (ni_counted(child->key)) return 1;
    return 0;
}
/* The member's storage (sh_decl_dependency_schema.storage): 1 with a usable
 * extent, 2 a bitfield setter (its extent cleared unless its verified bits
 * lie within it), 0 not a member, -1 unknown. */
#define NI_STORAGE_MAX (LLONG_MAX / 64)
static int ni_storage(const sh_decl_dependency_schema *schema, sh_decl_value_type type, const char *key,
    sh_decl_member_storage *storage)
{
    memset(storage, 0, sizeof(*storage));
    int stored = schema->storage ? schema->storage(schema->context, type, key, storage) : -1;
    int usable = storage->offset >= 0 && storage->extent > 0 && storage->offset <= NI_STORAGE_MAX &&
        storage->extent <= NI_STORAGE_MAX;
    if (stored == 1 && !usable) stored = -1;
    if (stored == 2 && (!usable || storage->bit_first < 0 || storage->bit_count <= 0 ||
        storage->bit_first > storage->extent * 8 - storage->bit_count))
        memset(storage, 0, sizeof(*storage));
    return stored;
}
/* A bitfield token its verified setter assigns completely: the storage is a
 * proved masked store and the token is the form its reader parses. */
static int ni_setter_assigns(const sh_decl_member_storage *storage, const sh_decl_node *node)
{
    sh_decl_value_shape form = {0};
    form.assigns = storage->assigns;
    return storage->extent > 0 && storage->assigns && ni_assigned(&form, node);
}
/* The bits a member stores inside an object stored at within: a member's
 * whole bytes, or a verified setter's own bits. Unknown when within is, when
 * the member has no usable storage, or when the sum would overflow. */
static ni_extent ni_member_extent(ni_extent within, int stored, const sh_decl_member_storage *storage)
{
    long long start, length;
    if (!within.known || (stored != 1 && (stored != 2 || !storage->extent))) return (ni_extent){0};
    start = storage->offset * 8 + (stored == 2 ? storage->bit_first : 0);
    length = stored == 2 ? storage->bit_count : storage->extent * 8;
    if (within.start > LLONG_MAX - start - length) return (ni_extent){0};
    return (ni_extent){1, within.start + start, length};
}
static ni_effect ni_effect_of(const sh_decl_dependency_schema *schema, sh_decl_value_type type, const sh_decl_node *node,
    sh_decl_value_type *field, sh_decl_value_shape *shape)
{
    memset(field, 0, sizeof(*field)); memset(shape, 0, sizeof(*shape));
    int found = schema->field(schema->context, type, node->key, field);
    if (found < 0) return NI_EFFECT_OTHER;
    if (!found) {
        /* The struct reader skips a name its type lacks, but passes a bitfield
         * token to that member's setter (VK 0x141a20600 / GL 0x141a12840),
         * whose effect the metadata does not describe. */
        sh_decl_member_storage storage;
        if (ni_storage(schema, type, node->key, &storage) != 2) return NI_EFFECT_INERT;
        return ni_setter_assigns(&storage, node) ? NI_EFFECT_TOTAL : NI_EFFECT_OTHER;
    }
    if (schema->describe(schema->context, *field, shape) <= 0 || shape->kind == SH_DECL_VALUE_UNKNOWN ||
        node->reset || node->annotation) return NI_EFFECT_OTHER;
    if (node->compound) return shape->kind == SH_DECL_VALUE_OBJECT ? NI_EFFECT_BLOCK :
        shape->kind == SH_DECL_VALUE_COLLECTION ? NI_EFFECT_LIST : NI_EFFECT_OTHER;
    if (shape->kind == SH_DECL_VALUE_POLYMORPHIC) return NI_EFFECT_OTHER;
    if ((shape->kind == SH_DECL_VALUE_IGNORE && shape->plain) || ni_assigned(shape, node)) return NI_EFFECT_TOTAL;
    /* A skipped form (no pointer reader, not NULL) leaves the field alone. */
    if (shape->kind == SH_DECL_VALUE_IGNORE && shape->effects == SH_DECL_EFFECTS_NONE) return NI_EFFECT_INERT;
    return NI_EFFECT_OTHER;
}
/* A read that changes at most its own storage, from its tokens alone. */
static int ni_pure(const sh_decl_dependency_schema *schema, sh_decl_value_type type, const sh_decl_node *node, int depth)
{
    sh_decl_value_type field; sh_decl_value_shape shape;
    ni_effect effect = ni_effect_of(schema, type, node, &field, &shape);
    if (effect == NI_EFFECT_INERT || effect == NI_EFFECT_TOTAL) return 1;
    if (effect != NI_EFFECT_BLOCK || depth > 64) return 0;
    for (const sh_decl_node *child = node->children; child; child = child->next)
        if (!ni_pure(schema, field, child, depth + 1)) return 0;
    return 1;
}
/* Reduce one block read by the struct reader of type, in place. marks, when
 * given, are the instance's leaves, which are never dropped. 1 done with no
 * repeat left, 0 outside the proof (reason set), -1 failure. */
static int ni_reduce_block(const sh_decl_dependency_schema *schema, sh_decl_node *block, sh_decl_value_type type,
    const char *path, ni_marks *marks, size_t *removed, char *reason, size_t capacity, int depth)
{
    sh_decl_value_type field, other; sh_decl_value_shape shape, other_shape;
    if (depth > 64) { snprintf(reason, capacity, "'%.120s' nests too deeply", path); return 0; }
    /* Adjacent blocks of a type with no registered reader read the same
     * object in turn; their concatenation keeps every read in order. */
    for (sh_decl_node *node = block->children; node; node = node->next)
        while (node->compound && node->next && node->next->compound && !strcmp(node->key, node->next->key) &&
            node->assignment == node->next->assignment && !ni_counts(node) && !ni_counts(node->next) &&
            ni_effect_of(schema, type, node, &field, &shape) == NI_EFFECT_BLOCK &&
            ni_effect_of(schema, type, node->next, &other, &other_shape) == NI_EFFECT_BLOCK) {
            sh_decl_node *later = node->next, **tail = &node->children;
            while (*tail) tail = &(*tail)->next;
            *tail = later->children; later->children = NULL;
            node->next = later->next; later->next = NULL;
            sh_decl_tree_free(later); (*removed)++;
        }
    for (sh_decl_node *node = block->children; node; node = node->next) {
        if (!node->compound) continue;
        ni_effect effect = ni_effect_of(schema, type, node, &field, &shape);
        size_t length = strlen(path) + strlen(node->key) + 2;
        char *nested = malloc(length);
        int result = 1;
        if (!nested) return -1;
        snprintf(nested, length, "%s/%s", path, node->key);
        if (effect == NI_EFFECT_BLOCK) {
            result = ni_reduce_block(schema, node, field, nested, marks, removed, reason, capacity, depth + 1);
        } else if (effect == NI_EFFECT_LIST) {
            /* The list reader reads num and each item by name; repeats of
             * those are list semantics, not modelled here. Items of an object
             * element are read by the struct reader like any block. */
            sh_decl_value_shape element = {0};
            for (const sh_decl_node *child = node->children; child && result > 0; child = child->next)
                for (const sh_decl_node *later = child->next; later; later = later->next) if (!strcmp(later->key, child->key)) {
                    snprintf(reason, capacity, "the repeated '%.100s/%.60s' is read by a list reader, whose repeated reads "
                        "are not modelled", nested, child->key);
                    result = 0; break;
                }
            int objects = shape.element.name && schema->describe(schema->context, shape.element, &element) > 0 &&
                element.kind == SH_DECL_VALUE_OBJECT;
            for (sh_decl_node *item = node->children; result > 0 && item; item = item->next) {
                if (!item->compound || (shape.count_key && !strcmp(item->key, shape.count_key))) continue;
                if (objects) result = ni_reduce_block(schema, item, shape.element, nested, marks, removed, reason, capacity, depth + 1);
                else if (sh_decl_tree_repeats(item)) {
                    snprintf(reason, capacity, "items of '%.120s' repeat members under a reader whose ordered reads are "
                        "not modelled", nested);
                    result = 0;
                }
            }
        } else if (sh_decl_tree_repeats(node)) {
            snprintf(reason, capacity, "'%.120s' repeats members under a reader whose ordered reads are not modelled", nested);
            result = 0;
        }
        free(nested);
        if (result <= 0) return result;
    }
    for (sh_decl_node **slot = &block->children; *slot;) {
        sh_decl_node *node = *slot, *last = NULL;
        for (sh_decl_node *later = node->next; later; later = later->next) if (!strcmp(later->key, node->key)) last = later;
        if (!last) { slot = &node->next; continue; }
        ni_effect effect = ni_effect_of(schema, type, node, &field, &shape);
        ni_effect final = ni_effect_of(schema, type, last, &other, &other_shape);
        const char *why = NULL;
        if (ni_counted(node->key)) why = "the inheritance post-pass counts num-prefixed members and prunes indexed items";
        else if (node->compound || last->compound) why = "its blocks are separated by other members, so one block would reorder their reads";
        else if (effect != NI_EFFECT_TOTAL && effect != NI_EFFECT_INERT)
            why = "an earlier occurrence is neither a complete assignment nor a native no-op";
        else if (final != NI_EFFECT_TOTAL && !(final == NI_EFFECT_INERT && effect == NI_EFFECT_INERT))
            why = "its last occurrence is not a complete assignment by its reader";
        for (const sh_decl_node *between = node->next; !why && between != last; between = between->next)
            if (!ni_pure(schema, type, between, 0)) why = "a member read between its occurrences is not proved to leave that storage alone";
        if (!why && marks && ni_mark_find(marks, node)) why = "an earlier occurrence holds the instance's own value, which a later one hides";
        if (why) { snprintf(reason, capacity, "the repeated '%.100s/%.60s' is not a proved reduction: %s", path, node->key, why); return 0; }
        *slot = node->next; node->next = NULL;
        sh_decl_tree_free(node); (*removed)++;
    }
    return 1;
}
int sh_native_inline_reduce(const sh_decl_dependency_schema *schema, sh_decl_node *edit, const char *class_name,
    size_t *removed, char *reason, size_t capacity)
{
    char ignored[8];
    if (!reason || !capacity) { reason = ignored; capacity = sizeof(ignored); }
    reason[0] = 0;
    if (!schema || !edit || !edit->compound || !class_name || !removed) return -1;
    return ni_reduce_block(schema, edit, (sh_decl_value_type){class_name, ""}, "edit", NULL, removed, reason, capacity, 0);
}
/* The evaluator's side of a reduction: the same proof on one prepared state
 * text, whose edit block the class's struct reader reads. Every other part of
 * the text stays as written; the canonical writer keeps each token. */
typedef struct ni_reduction_view { const sh_decl_dependency_schema *schema; const char *class_name; } ni_reduction_view;
/* A prepared state text (entityDef +0x130) is a statement list, not a braced
 * declaration: with inheritance expanded, Parse's preparation (VK 0x1417ae9d0
 * / GL 0x1417a14b0) prints the merged tree's top-level nodes one after
 * another (VK 0x141a670d0 / GL 0x141a595a0), each its header with its value
 * or block, and nothing around them. It parses as the members of one block;
 * the reductions' texts are compared only with each other, so they keep the
 * canonical writer's braces. NULL with reason set, naming the text's first
 * bytes (an own-text slice, which is not this shape, refuses here). */
static sh_decl_node *ni_prepared_parse(const char *state, size_t length, char *reason, size_t capacity)
{
    char error[160] = "", head[41];
    size_t shown = 0;
    char *braced = state && length <= SIZE_MAX - 4 ? malloc(length + 4) : NULL;
    sh_decl_node *tree = NULL;
    if (braced) {
        braced[0] = '{'; memcpy(braced + 1, state, length); memcpy(braced + 1 + length, "\n}", 3);
        tree = sh_decl_tree_parse_ordered((sh_decl_source){braced, length + 3}, error, sizeof(error));
        free(braced);
    } else snprintf(error, sizeof(error), "%s", state ? "allocation failed" : "no prepared state");
    if (tree) return tree;
    for (; state && shown < length && shown < sizeof(head) - 1; shown++) {
        unsigned char ch = (unsigned char)state[shown];
        head[shown] = ch >= 32 && ch < 127 && ch != '\'' ? (char)ch : ' ';
    }
    head[shown] = 0;
    snprintf(reason, capacity, "the prepared state does not parse as statements: %s (it begins '%s')", error, head);
    return NULL;
}
static int ni_reduce_prepared(void *context, const char *state, size_t length, char **reduced, size_t *reduced_length,
    size_t *removed, char *reason, size_t capacity)
{
    const ni_reduction_view *view = context;
    int result = 1;
    *reduced = NULL; *reduced_length = 0;
    sh_decl_node *tree = ni_prepared_parse(state, length, reason, capacity);
    if (!tree) return 0;
    for (sh_decl_node *member = tree->children; member && result > 0; member = member->next) {
        for (const sh_decl_node *later = member->next; later; later = later->next) if (!strcmp(later->key, member->key)) {
            snprintf(reason, capacity, "the prepared state repeats its member '%.80s'", member->key); result = 0; break;
        }
        if (result > 0 && member->compound && !strcmp(member->key, "edit"))
            result = ni_reduce_block(view->schema, member, (sh_decl_value_type){view->class_name, ""}, "edit", NULL, removed,
                reason, capacity, 0);
        else if (result > 0 && sh_decl_tree_repeats(member)) {
            snprintf(reason, capacity, "the prepared state's '%.80s' repeats members outside the edit block", member->key);
            result = 0;
        }
    }
    if (result > 0 && !(*reduced = sh_decl_tree_write(tree, reduced_length))) result = -1;
    sh_decl_tree_free(tree);
    return result;
}
static int ni_layer_walk(const sh_decl_dependency_schema *schema, const sh_decl_node *node, sh_decl_value_type type,
    const char *prefix, ni_extent within, size_t owner, ni_writes *writes, ni_writes *order, int *reader, char *reason,
    size_t capacity, int depth);
/* An idTypeInfoObjectPtr value: 2 a form its reader (VK 1409d0660 / GL
 * 1409cfe20) skips, keeping the destination; 1 a selected class, constructed
 * fresh in either form, whose object block every nested reader models; 0
 * refused (reason set); -1 failure. Nested members are one read of the block,
 * so they record no separate writes or order. */
static int ni_polymorphic(const sh_decl_dependency_schema *schema, const sh_decl_node *node,
    const sh_decl_value_shape *shape, const char *path, size_t owner, int *reader, char *reason, size_t capacity,
    int depth)
{
    sh_decl_polymorphic_state state;
    const char *why = NULL;
    if (shape->null_keeps && sh_decl_polymorphic_unchanged(node)) return 2;
    if (sh_decl_polymorphic_read(node, shape->element, schema, &state, &why) != 1) {
        snprintf(reason, capacity, "'%.120s': %s", path, why ? why : "unsupported object form"); return 0;
    }
    return state.object ? ni_layer_walk(schema, state.object->children, state.type, path, (ni_extent){0}, owner, NULL,
        NULL, reader, reason, capacity, depth + 1) : 1;
}
/* A fixed-array item key: the member's name, '[', decimal digits, ']' (the
 * spelling the dependency walker accepts for an array member). */
static int ni_array_index(const char *key, const char *name, size_t *index)
{
    size_t prefix = strcspn(name, "["), length = strlen(key), value = 0;
    if (length < prefix + 3 || memcmp(key, name, prefix) || key[prefix] != '[' || key[length - 1] != ']') return 0;
    for (size_t i = prefix + 1; i < length - 1; i++) {
        if (key[i] < '0' || key[i] > '9' || value > (SIZE_MAX - 9) / 10) return 0;
        value = value * 10 + (size_t)(key[i] - '0');
    }
    *index = value; return 1;
}
/* A fixed array (no count member) is read by the array reader (VK 0x141a202d0
 * / GL 0x141a12510): each item in range stores its element at index * size(T)
 * from the array, other slots keep their values, and nothing resizes. Its
 * items are therefore separate writes, merged by key like members: a scalar
 * by its element reader's role, an inline object by its members. 2 when an
 * element form is not one of those (the single-writer list rule applies); 1,
 * 0 (reason set) or -1 otherwise. */
static int ni_array_walk(const sh_decl_dependency_schema *schema, const sh_decl_node *node,
    const sh_decl_value_shape *shape, const char *path, ni_extent extent, size_t owner, ni_writes *writes,
    ni_writes *order, int *reader, char *reason, size_t capacity, int depth)
{
    sh_decl_value_shape element = {0};
    long long stride = 0;
    int result;
    if (!writes || !shape->element.name || schema->describe(schema->context, shape->element, &element) <= 0 ||
        element.kind == SH_DECL_VALUE_UNKNOWN || element.kind == SH_DECL_VALUE_POLYMORPHIC ||
        element.kind == SH_DECL_VALUE_COLLECTION || element.effects == SH_DECL_EFFECTS_GLOBAL) return 2;
    if (element.kind != SH_DECL_VALUE_OBJECT && !(element.kind == SH_DECL_VALUE_IGNORE && element.plain) &&
        !element.assigns && element.effects != SH_DECL_EFFECTS_FIELD && element.effects != SH_DECL_EFFECTS_NONE) return 2;
    if (extent.known && shape->count && extent.length % (long long)shape->count == 0)
        stride = extent.length / (long long)shape->count;
    result = ni_writes_add(writes, path, owner, NI_ROLE_OBJECT, extent);
    for (const sh_decl_node *item = node->children; result > 0 && item; item = item->next) {
        size_t index, length = strlen(path) + strlen(item->key) + 2;
        char *item_path;
        if (!ni_array_index(item->key, node->key, &index) || index >= shape->count) {
            snprintf(reason, capacity, "'%.120s' item '%.60s' is outside the array's native grammar or count", path,
                item->key);
            return 0;
        }
        if (!(item_path = malloc(length))) return -1;
        snprintf(item_path, length, "%s/%s", path, item->key);
        ni_extent at = stride ? (ni_extent){1, extent.start + (long long)index * stride, stride} : (ni_extent){0};
        if (element.kind == SH_DECL_VALUE_OBJECT && item->compound) {
            result = ni_writes_add(writes, item_path, owner, NI_ROLE_OBJECT, at);
            if (result > 0) result = ni_layer_walk(schema, item->children, shape->element, item_path, at, owner, writes,
                order, reader, reason, capacity, depth + 1);
        } else if (element.kind == SH_DECL_VALUE_IGNORE && element.plain && !item->compound) {
            result = ni_writes_add(writes, item_path, owner, NI_ROLE_PLAIN, at);
        } else if (ni_assigned(&element, item)) {
            result = ni_writes_add(writes, item_path, owner, NI_ROLE_ASSIGN, at);
        } else if (element.effects == SH_DECL_EFFECTS_FIELD) {
            result = ni_writes_add(writes, item_path, owner, NI_ROLE_FIELD, at);
        } else if (element.effects == SH_DECL_EFFECTS_NONE) {
            result = ni_writes_add(writes, item_path, owner, NI_ROLE_SKIP, at);
        } else {
            snprintf(reason, capacity, "the element reader of '%.120s' has effects not modelled yet", item_path);
            result = 0;
        }
        free(item_path);
    }
    return result;
}
/* Classify the members a layer writes under an object of type, stored inline
 * at within. writes is NULL inside a list or a selected object, whose single
 * writer is checked by path; *reader is set when a member is read by a GLOBAL
 * registered reader, whose reads order lists. A bitfield's setter is a write
 * too (ACCESSOR). 1 done, 0 outside the model (reason set), -1 failure. */
static int ni_layer_walk(const sh_decl_dependency_schema *schema, const sh_decl_node *node, sh_decl_value_type type,
    const char *prefix, ni_extent within, size_t owner, ni_writes *writes, ni_writes *order, int *reader, char *reason,
    size_t capacity, int depth)
{
    if (depth > 64) { snprintf(reason, capacity, "'%.120s' nests too deeply", prefix); return 0; }
    for (; node; node = node->next) {
        sh_decl_value_type field = {0};
        sh_decl_value_shape shape = {0};
        size_t length = strlen(prefix) + strlen(node->key) + 2;
        char *path = malloc(length);
        int result = 1;
        if (!path) return -1;
        snprintf(path, length, "%s%s%s", prefix, *prefix ? "/" : "", node->key);
        int found = schema->field(schema->context, type, node->key, &field);
        sh_decl_member_storage storage;
        int stored = found >= 0 ? ni_storage(schema, type, node->key, &storage) : -1;
        ni_extent extent = ni_member_extent(within, stored, &storage);
        if (!found) {
            result = stored != 2 ? 1 : ni_writes_add(writes, path, owner,
                ni_setter_assigns(&storage, node) ? NI_ROLE_ASSIGN : NI_ROLE_ACCESSOR, extent);
            free(path);
            if (result <= 0) return result;
            continue; /* the native reader skips any other name */
        }
        if (found < 0 || schema->describe(schema->context, field, &shape) <= 0 || shape.kind == SH_DECL_VALUE_UNKNOWN) {
            snprintf(reason, capacity, "the reader of '%.120s' is not resolved", path); free(path); return 0;
        }
        if (shape.kind == SH_DECL_VALUE_OBJECT && node->compound) {
            /* An inline object's members are stored at its own offset; a
             * pointer's object is separate storage. */
            ni_extent nested = extent.known && field.ops && !*field.ops ? extent : (ni_extent){0};
            result = ni_writes_add(writes, path, owner, NI_ROLE_OBJECT, extent);
            if (result > 0) result = ni_layer_walk(schema, node->children, field, path, nested, owner, writes, order, reader,
                reason, capacity, depth + 1);
        } else if (shape.kind == SH_DECL_VALUE_COLLECTION && node->compound && !shape.count_key && shape.count_known &&
            (result = ni_array_walk(schema, node, &shape, path, extent, owner, writes, order, reader, reason, capacity,
            depth)) != 2) {
            /* each fixed-array item is its own write */
        } else if (shape.kind == SH_DECL_VALUE_COLLECTION && node->compound) {
            /* A list's header is its member storage; the list reader resizes
             * its heap elements (VK 0x1409cd610), which are never inline. */
            result = ni_writes_add(writes, path, owner, NI_ROLE_LIST, extent);
            sh_decl_value_shape element = {0};
            int element_known = shape.element.name && schema->describe(schema->context, shape.element, &element) > 0;
            for (const sh_decl_node *item = node->children; result > 0 && item; item = item->next) {
                if (shape.count_key && !strcmp(item->key, shape.count_key)) continue;
                if (!element_known || element.kind == SH_DECL_VALUE_UNKNOWN) {
                    snprintf(reason, capacity, "the element reader of '%.120s' is not resolved", path); result = 0;
                } else if (element.kind == SH_DECL_VALUE_OBJECT && item->compound) {
                    result = ni_layer_walk(schema, item->children, shape.element, path, (ni_extent){0}, owner, NULL, order,
                        reader, reason, capacity, depth + 1);
                } else if (element.kind == SH_DECL_VALUE_POLYMORPHIC &&
                    (result = ni_polymorphic(schema, item, &element, path, owner, reader, reason, capacity, depth)) != 1) {
                    if (result == 2) result = 1; /* the item keeps its slot's object */
                } else if (element.kind != SH_DECL_VALUE_POLYMORPHIC && ((element.kind == SH_DECL_VALUE_IGNORE &&
                    element.plain && !item->compound) || ni_assigned(&element, item) ||
                    element.effects == SH_DECL_EFFECTS_FIELD || element.effects == SH_DECL_EFFECTS_NONE)) {
                    /* The list's single writer reads the item once in both forms. */
                } else if (element.effects != SH_DECL_EFFECTS_GLOBAL) {
                    snprintf(reason, capacity, "the registered reader of '%.120s' items has effects not modelled yet", path);
                    result = 0;
                } else {
                    *reader = 1;
                    size_t item_length = strlen(path) + strlen(item->key) + 2;
                    char *item_path = malloc(item_length);
                    if (!item_path) result = -1;
                    else {
                        snprintf(item_path, item_length, "%s/%s", path, item->key);
                        result = ni_writes_add(order, item_path, owner, NI_ROLE_GLOBAL, (ni_extent){0});
                        free(item_path);
                    }
                }
            }
        } else if (shape.kind == SH_DECL_VALUE_POLYMORPHIC) {
            /* A selected class reconstructs the object, so another writer's
             * block cannot merge into it as the fold would merge them. */
            result = ni_polymorphic(schema, node, &shape, path, owner, reader, reason, capacity, depth);
            if (result == 2) result = ni_writes_add(writes, path, owner, NI_ROLE_FIELD, extent);
            else if (result > 0) {
                *reader = 1;
                result = ni_writes_add(writes, path, owner, NI_ROLE_GLOBAL, extent);
                if (result > 0) result = ni_writes_add(order, path, owner, NI_ROLE_GLOBAL, extent);
            }
        } else if (shape.kind == SH_DECL_VALUE_IGNORE && shape.plain && !node->compound) {
            result = ni_writes_add(writes, path, owner, NI_ROLE_PLAIN, extent);
        } else if (ni_assigned(&shape, node)) {
            result = ni_writes_add(writes, path, owner, NI_ROLE_ASSIGN, extent);
        } else if (shape.effects == SH_DECL_EFFECTS_FIELD) {
            result = ni_writes_add(writes, path, owner, NI_ROLE_FIELD, extent);
        } else if (shape.effects == SH_DECL_EFFECTS_GLOBAL) {
            *reader = 1;
            result = ni_writes_add(writes, path, owner, NI_ROLE_GLOBAL, extent);
            if (result > 0) result = ni_writes_add(order, path, owner, NI_ROLE_GLOBAL, extent);
        } else if (shape.effects == SH_DECL_EFFECTS_NONE) {
            result = ni_writes_add(writes, path, owner, NI_ROLE_SKIP, extent);
        } else {
            snprintf(reason, capacity, "the registered reader of '%.120s' has effects not modelled yet", path);
            result = 0;
        }
        free(path);
        if (result <= 0) return result;
    }
    return 1;
}
static int ni_write_order(const void *a, const void *b)
{
    const ni_write *x = a, *y = b;
    int order = _stricmp(x->path, y->path);
    if (order) return order;
    if (x->owner != y->owner) return x->owner < y->owner ? -1 : 1;
    /* One layer's repeats keep their occurrence order. */
    return x->sequence < y->sequence ? -1 : x->sequence > y->sequence;
}
/* ---- storage order of the fold (R-213) ----
 * The fold reads each member once, where the merge put its first occurrence,
 * with its last value; the pool reads each layer's own writes in turn,
 * farthest first and the instance last. Writes to different members commute
 * only when their stored bytes are proved disjoint (ni_layer_walk extents).
 * Otherwise the fold must keep their effective order: complete assignments by
 * their last writes, and a partial read (FIELD, GLOBAL, list, selected object
 * or bitfield setter) on the same side of every write of the other. */
typedef struct ni_unit { const ni_write *row; size_t first, last, fold; int complete; } ni_unit;
static int ni_fold_paths(const sh_decl_node *node, const char *prefix, ni_writes *paths, int depth)
{
    for (; node; node = node->next) {
        size_t length = strlen(prefix) + strlen(node->key) + 2;
        char *path = malloc(length);
        if (!path) return -1;
        snprintf(path, length, "%s/%s", prefix, node->key);
        int result = ni_writes_add(paths, path, 0, NI_ROLE_PLAIN, (ni_extent){0});
        if (result > 0 && node->compound && depth < 64) result = ni_fold_paths(node->children, path, paths, depth + 1);
        free(path);
        if (result <= 0) return result;
    }
    return 1;
}
static int ni_disjoint(ni_extent a, ni_extent b)
{ return a.known && b.known && (a.start + a.length <= b.start || b.start + b.length <= a.start); }
/* writes sorted by path, owner and sequence. 1 kept, 0 refused (reason set),
 * -1 failure. */
static int ni_fold_order(const ni_writes *writes, const sh_decl_node *merged, size_t layer_count, const char *chosen,
    char *reason, size_t capacity)
{
    const sh_decl_node *edit = merged ? sh_decl_tree_member(merged, "edit") : NULL;
    ni_writes paths = {0};
    ni_unit *units = writes->count ? calloc(writes->count, sizeof(*units)) : NULL;
    size_t count = 0;
    int result = writes->count && !units ? -1 : 1;
    if (result > 0 && edit) result = ni_fold_paths(edit->children, "edit", &paths, 0);
    for (size_t i = 0; result > 0 && i < writes->count; i++) {
        const ni_write *row = writes->rows + i;
        if (row->role == NI_ROLE_OBJECT || row->role == NI_ROLE_SKIP) continue; /* no stored bytes of its own */
        size_t rank = row->owner == layer_count ? layer_count : layer_count - 1 - row->owner;
        size_t at = rank * (writes->count + 1) + row->sequence;
        int complete = row->role == NI_ROLE_PLAIN || row->role == NI_ROLE_ASSIGN;
        ni_unit *unit = count && !_stricmp(units[count - 1].row->path, row->path) ? units + count - 1 : NULL;
        if (unit) {
            if (at < unit->first) unit->first = at;
            if (at > unit->last) unit->last = at;
            unit->complete &= complete;
            continue;
        }
        unit = units + count++;
        *unit = (ni_unit){row, at, at, SIZE_MAX, complete};
        for (size_t j = 0; j < paths.count; j++) if (!_stricmp(paths.rows[j].path, row->path)) { unit->fold = j; break; }
    }
    for (size_t i = 0; result > 0 && i < count; i++)
        for (size_t j = i + 1; result > 0 && j < count; j++) {
            const ni_unit *a = units + i, *b = units + j;
            if (ni_disjoint(a->row->extent, b->row->extent)) continue;
            int before = a->fold < b->fold, kept;
            if (a->fold == SIZE_MAX || b->fold == SIZE_MAX) kept = 0;
            else if (a->complete && b->complete) kept = (a->last < b->last) == before;
            else if (a->last < b->first) kept = before;
            else if (b->last < a->first) kept = !before;
            else kept = 0;
            if (kept) continue;
            snprintf(reason, capacity, "the world's AI spawn pool reads each layer below '%.80s' separately, and the fold "
                "changes the order of the writes to '%.90s' and '%.90s', whose storage is not proved disjoint", chosen,
                a->row->path, b->row->path);
            result = 0;
        }
    free(units); ni_writes_free(&paths);
    return result;
}
/* Sequential own-state reads of the layers equal one read of their fold when
 * no layer resets a value (the merge applies a reset; these reads consume it
 * with no effect), every list has one writer (lists resize on num and keep
 * other slots), a path several layers write is a plain value, a block with no
 * registered reader or an ASSIGN token, GLOBAL reads occur in one layer only,
 * the fold keeps that layer's GLOBAL reads in their order, every
 * registered reader's effects are modelled, and the fold keeps the order of
 * writes to storage not proved disjoint (ni_fold_order).
 * Every layer call and the fold's call parse with the same arguments
 * (VK 14088b5e0 / 1412cff00, GL 14088ac10 / 1412c2000 build them with VK
 * 141a24240 and pass mode 1), and the struct reader dispatches while the
 * per-dispatch bypass byte (+0x12) is clear (R-198/R-199 scope). So:
 * - an ASSIGN read's only effect is its assignment, which hides earlier ones;
 * - a FIELD read changes its own field from its tokens, that field's earlier
 *   value, those arguments and static tables; with one writer the earlier
 *   value is the base's in both forms, so its layer and order do not matter,
 *   while a second writer would replay or lose an earlier partial read;
 * - a GLOBAL read also creates, loads, interns, allocates, resolves world
 *   names or adds pending registrations. Only the alias-handle reader (VK
 *   1409ca410 / GL 1409c9bd0) and the idRenderModel pointer reader (VK
 *   1409cdd20 / GL 1409cd4e0) add pending registrations; each call binds its
 *   new aliases to its new models (manager +0xa8) or, with none, interns them
 *   globally (+0x90, +0x98). In one layer the GLOBAL reads share one call in
 *   both forms, with the same order and no GLOBAL read in another call, while
 *   ASSIGN, FIELD and plain reads touch nothing they use.
 * 1 equivalent, 0 refused (reason set), -1 failure. Owners
 * 0..layer_count-1 are the layers, layer_count the instance. */
static int ni_pool_layers(const sh_native_inline_source *source, const char *class_name, const char *chosen,
    const ni_layer *layers, size_t layer_count, const sh_decl_node *instance, const sh_decl_node *merged, int kept,
    const char *chain_state, size_t opener, char *reason, size_t capacity)
{
    ni_writes writes = {0}, order = {0}, folded = {0};
    size_t reader_owner = SIZE_MAX;
    int result = 1;
    char detail[200] = "", deferred[320] = "";
    for (size_t owner = 0; owner <= layer_count && result > 0; owner++) {
        const sh_decl_node *root = instance;
        const char *name = owner < layer_count ? layers[owner].name : "the instance";
        int reader = 0;
        if (owner < layer_count) {
            /* A layer read alone parses its own text from decl+0x100 (VK
             * 0x1417ad1f0 / GL 0x14179fcc0), its positional state text, and
             * SetEntityEditState reads only that text's first statement. That
             * must be what the fold took from this layer: the opening layer's
             * first statement, or a later layer's one statement of the chain's
             * state name. Later statements are read by neither. */
            sh_decl_entity_headers headers;
            sh_decl_entity_split(layers[owner].tree, &headers);
            root = headers.state;
            int agrees = !root || owner == opener;
            if (!agrees && chain_state && !strcmp(root->key, chain_state)) {
                agrees = 1;
                for (const sh_decl_node *later = root->next; later; later = later->next)
                    if (!strcmp(later->key, chain_state)) agrees = 0;
            }
            if (root && (!root->compound || !root->assignment)) {
                snprintf(reason, capacity, "the world's AI spawn pool reads each layer below '%.80s' separately, and the "
                    "state text of '%.80s' does not begin with a block, whose separate read is not modelled", chosen, name);
                result = 0; break;
            }
            if (!agrees) {
                snprintf(reason, capacity, "the world's AI spawn pool reads each layer below '%.80s' separately, and a "
                    "separate read of '%.80s' applies its first state statement '%.80s', which differs from the statements "
                    "the inherited merge applies", chosen, name, root->key);
                result = 0; break;
            }
        }
        if (!root) continue;
        if ((owner < layer_count && root->reset) || ni_has_reset(root->children, 0)) {
            snprintf(reason, capacity, "the world's AI spawn pool reads each layer below '%.80s' separately, where a "
                "reset in '%.80s' has no effect", chosen, name);
            result = 0; break;
        }
        const sh_decl_node *members = root->children;
        if (owner == layer_count) {
            /* The instance's carrier state is { edit = { ... } }. */
            for (const sh_decl_node *member = root->children; member; member = member->next) if (strcmp(member->key, "edit")) {
                snprintf(reason, capacity, "instance member '%.80s' is not modelled under a pooled base", member->key);
                result = 0; break;
            }
            const sh_decl_node *edit = result > 0 ? sh_decl_tree_member(root, "edit") : NULL;
            members = edit ? edit->children : NULL;
        }
        if (result > 0)
            result = ni_layer_walk(source->schema, members, (sh_decl_value_type){class_name, ""}, "edit", (ni_extent){1, 0, 0},
                owner, &writes, &order, &reader, detail, sizeof(detail), 0);
        if (result == 0)
            snprintf(reason, capacity, "the world's AI spawn pool reads each layer below '%.80s' separately; in '%.80s' %s",
                chosen, name, detail);
        /* A layer's own read of its repeats is sequential like the prepared
         * text's and needs the same reduction proof (R-212). */
        if (result > 0 && owner < layer_count) {
            int repeats = sh_decl_tree_repeats(root);
            sh_decl_node *copy = repeats > 0 ? ni_copy(root, NULL) : NULL;
            size_t dropped = 0;
            if (repeats < 0 || (repeats > 0 && !copy)) result = -1;
            else if (copy) {
                char why[200] = "";
                int proved = ni_reduce_block(source->schema, copy, (sh_decl_value_type){class_name, ""}, "edit", NULL,
                    &dropped, why, sizeof(why), 0);
                sh_decl_tree_free(copy);
                if (proved < 0) result = -1;
                /* Decided after the per-path checks, which name more specific causes. */
                else if (!proved && !deferred[0])
                    snprintf(deferred, sizeof(deferred), "the world's AI spawn pool reads each layer below '%.80s' "
                        "separately; in '%.80s' %s", chosen, name, why);
            }
        }
        if (result > 0 && reader) {
            if (reader_owner != SIZE_MAX) {
                snprintf(reason, capacity, "the world's AI spawn pool reads each layer below '%.80s' separately, and "
                    "registered readers in '%.60s' and '%.60s' read in separate calls, which changes how their global "
                    "lookups, interning and pending registrations are grouped and ordered",
                    chosen, reader_owner < layer_count ? layers[reader_owner].name : "the instance", name);
                result = 0;
            }
            reader_owner = owner;
        }
    }
    if (result > 0 && writes.count > 1) qsort(writes.rows, writes.count, sizeof(*writes.rows), ni_write_order);
    /* A layer's own read applies each occurrence of a repeated member in turn
     * (VK 0x141a209b0), while the fold keeps one; repeats within one layer
     * are therefore writers like those of several layers, in occurrence order. */
    for (size_t i = 1; result > 0 && i < writes.count; i++) {
        const ni_write *a = writes.rows + i - 1, *b = writes.rows + i;
        if (_stricmp(a->path, b->path)) continue;
        /* Where the fold kept a first stateful layer's repeats (kept), it
         * updates a repeated path's first occurrence, which its later one
         * hides, while the pool reads a later layer's write last. */
        if (kept && a->owner == b->owner) {
            /* Layers are nearest first: a nearer layer or the instance is read
             * after this one. */
            int later = 0;
            for (size_t j = 0; j < writes.count && !later; j++)
                later = !_stricmp(writes.rows[j].path, a->path) && writes.rows[j].owner != a->owner &&
                    (writes.rows[j].owner < a->owner || writes.rows[j].owner == layer_count);
            if (later) {
                snprintf(reason, capacity, "the world's AI spawn pool reads each layer below '%.80s' separately, and '%.120s' "
                    "repeats in one layer and is written by a later one, whose fold update lands on the hidden first "
                    "occurrence", chosen, a->path);
                result = 0; break;
            }
        }
        if (a->role == b->role && (a->role == NI_ROLE_PLAIN || a->role == NI_ROLE_OBJECT || a->role == NI_ROLE_ASSIGN))
            continue;
        if (a->role == NI_ROLE_ACCESSOR || b->role == NI_ROLE_ACCESSOR) {
            snprintf(reason, capacity, "the world's AI spawn pool reads each layer below '%.80s' separately, and '%.120s' "
                "is written more than once through its bitfield setter, whose effect is not described", chosen, a->path);
            result = 0; break;
        }
        /* A skipped form keeps an earlier stored NULL, but the fold reads only
         * the last writer's form: a skip may follow NULL only when a later
         * NULL ends the path again. */
        if ((a->role == NI_ROLE_SKIP || b->role == NI_ROLE_SKIP) && (a->role == NI_ROLE_SKIP || a->role == NI_ROLE_ASSIGN) &&
            (b->role == NI_ROLE_SKIP || b->role == NI_ROLE_ASSIGN)) {
            size_t later = i + 1;
            if (a->role == NI_ROLE_SKIP || b->role == NI_ROLE_ASSIGN) continue;
            while (later < writes.count && !_stricmp(writes.rows[later].path, b->path) &&
                writes.rows[later].role != NI_ROLE_ASSIGN) later++;
            if (later < writes.count && !_stricmp(writes.rows[later].path, b->path)) continue;
            snprintf(reason, capacity, "the world's AI spawn pool reads each layer below '%.80s' separately, and the native "
                "reader skips the last value of '%.120s', keeping an earlier NULL the fold would drop", chosen, a->path);
            result = 0; break;
        }
        snprintf(reason, capacity, a->role == NI_ROLE_LIST || b->role == NI_ROLE_LIST ?
            "the world's AI spawn pool reads each layer below '%.80s' separately, and '%.120s' has more than one folded writer" :
            "the world's AI spawn pool reads each layer below '%.80s' separately, and '%.120s' is read more than once "
            "by a registered reader whose earlier read the fold would drop", chosen, a->path);
        result = 0;
    }
    if (result > 0 && deferred[0]) { snprintf(reason, capacity, "%s", deferred); result = 0; }
    /* The fold reads that layer's registered values in its own sequence. */
    if (result > 0 && reader_owner != SIZE_MAX) {
        const sh_decl_node *edit = merged ? sh_decl_tree_member(merged, "edit") : NULL;
        int ignored = 0;
        result = ni_layer_walk(source->schema, edit ? edit->children : NULL, (sh_decl_value_type){class_name, ""}, "edit",
            (ni_extent){0}, layer_count + 1, NULL, &folded, &ignored, detail, sizeof(detail), 0);
        if (result == 0)
            snprintf(reason, capacity, "the world's AI spawn pool reads each layer below '%.80s' separately; in the fold %s",
                chosen, detail);
        size_t at = 0;
        for (size_t i = 0; result > 0 && i < order.count; i++) {
            if (order.rows[i].owner != reader_owner) continue;
            if (at >= folded.count || _stricmp(order.rows[i].path, folded.rows[at].path)) {
                snprintf(reason, capacity, "the world's AI spawn pool reads each layer below '%.80s' separately, and the fold "
                    "reorders the registered read of '%.120s'", chosen, order.rows[i].path);
                result = 0;
            }
            at++;
        }
        if (result > 0 && at != folded.count) {
            snprintf(reason, capacity, "the world's AI spawn pool reads each layer below '%.80s' separately, and the fold "
                "has other registered reads", chosen);
            result = 0;
        }
    }
    if (result > 0) result = ni_fold_order(&writes, merged, layer_count, chosen, reason, capacity);
    ni_writes_free(&writes); ni_writes_free(&order); ni_writes_free(&folded);
    return result;
}
/* 1 when no definition of the native chain from base has state text after
 * its positional headers (sh_decl_entity_split), so the state tree is still
 * empty when the first served layer merges; 0 when one does, with the
 * root-most such layer's first statement name, which later layers update by
 * first match, in *state_name; 2 unreadable; -1 failure. */
static int ni_state_empty(const sh_native_inline_source *source, const char *base, char **state_name)
{
    char *name = _strdup(base);
    *state_name = NULL;
    if (!name) return -1;
    for (size_t depth = 0; name && *name; depth++) {
        sh_decl_source text = {0}; int served = 0;
        int read = depth < 64 ? source->read(source->context, name, &text, &served, NULL) : 0;
        free(name); name = NULL;
        if (read < 0) { free(*state_name); *state_name = NULL; return -1; }
        if (!read) { free(*state_name); *state_name = NULL; return 2; }
        sh_decl_node *tree = sh_decl_tree_parse_ordered(text, NULL, 0);
        free((char *)text.text);
        if (!tree) { free(*state_name); *state_name = NULL; return 2; }
        sh_decl_entity_headers headers;
        sh_decl_entity_split(tree, &headers);
        if (headers.state) {
            free(*state_name);
            if (!(*state_name = _strdup(headers.state->key))) { sh_decl_tree_free(tree); return -1; }
        }
        name = ni_parent(tree);
        sh_decl_tree_free(tree);
    }
    free(name);
    return *state_name ? 0 : 1;
}

/* The chain's first stateful layer is merged with the flag clear and keeps
 * every repeat, nested ones included (VK 0x141a65560 / 0x141a65410, GL
 * 0x141a578e0); the member walker then reads each occurrence. The entity
 * carrier cannot spell distinct occurrences: its reader (VK 0x141a86d60 /
 * GL 0x141a79220) lists the state object's keys and resolves each value by
 * key text through Json::Value::operator[] (VK 0x141a41aa0 -> 0x141a41b80 /
 * GL 0x141a33fb0), so every occurrence of a name reads one value. Classify
 * by the readers of the repeated paths. Always 0 (refused) or -1. */
static int ni_root_repeats(const sh_native_inline_source *source, const sh_decl_node *edit, const char *class_name,
    const char *repeated, const char *why, sh_native_inline_site *site)
{
    ni_writes writes = {0}, order = {0};
    int reader = 0, global = 0, total = 1;
    char detail[200] = "", reason[320];
    int walked = ni_layer_walk(source->schema, edit->children, (sh_decl_value_type){class_name, ""}, "edit", (ni_extent){1, 0, 0},
        0, &writes, &order, &reader, detail, sizeof(detail), 0);
    if (walked < 0) { ni_writes_free(&writes); ni_writes_free(&order); return -1; }
    if (walked > 0) {
        if (writes.count > 1) qsort(writes.rows, writes.count, sizeof(*writes.rows), ni_write_order);
        for (size_t i = 1; i < writes.count; i++) {
            const ni_write *a = writes.rows + i - 1, *b = writes.rows + i;
            if (_stricmp(a->path, b->path)) continue;
            if (a->role == NI_ROLE_GLOBAL || b->role == NI_ROLE_GLOBAL) global = 1;
            else if (a->role != b->role || (a->role != NI_ROLE_ASSIGN && a->role != NI_ROLE_PLAIN &&
                a->role != NI_ROLE_OBJECT)) total = 0;
        }
        /* Registered item reads of a list block read twice repeat as well. */
        if (order.count > 1) qsort(order.rows, order.count, sizeof(*order.rows), ni_write_order);
        for (size_t i = 1; i < order.count; i++) if (!_stricmp(order.rows[i - 1].path, order.rows[i].path)) global = 1;
    }
    if (walked == 0)
        snprintf(reason, sizeof(reason), "the chain's root-most layer keeps the repeated '%.100s', which one inline layer "
            "cannot spell; %s", repeated, detail);
    else if (global)
        snprintf(reason, sizeof(reason), "the chain's root-most layer keeps the repeated '%.100s' and a registered reader "
            "reads each occurrence; the entity carrier resolves a state member by its key, and whether one read can "
            "reproduce the earlier reads' effects is not established", repeated);
    else if (total)
        snprintf(reason, sizeof(reason), "the chain's root-most layer keeps the repeated '%.100s', whose complete "
            "assignments do not reduce here: %s", repeated, why && *why ? why : "the reduction is not proved");
    else
        snprintf(reason, sizeof(reason), "the chain's root-most layer keeps the repeated '%.100s', read by a reader "
            "whose ordered reads are not modelled%s%s", repeated, why && *why ? ": " : "", why && *why ? why : "");
    /* The carrier cannot spell distinct occurrences, but that alone does not
     * prove the reads irreducible: unproven equivalence is a product gap. */
    ni_refuse(site, SH_DECL_CARRIER_GAP, "%s", reason);
    ni_writes_free(&writes); ni_writes_free(&order);
    return 0;
}

/* The site's use under the world's pool: 1 unaffected or equivalent, 0
 * refused (reason set), -1 failure. Layers are P..S(excl), nearest first. */
static int ni_pool_site(const sh_native_inline_source *source, const ni_pool *pool, sh_native_inline_site *site,
    const char *inherit, const char *base, const ni_layer *layers, size_t layer_count, const sh_decl_node *instance,
    const sh_decl_node *merged, const char *class_name, const ni_identity *identity, int kept,
    const char *chain_state, size_t opener)
{
    char **ancestry = NULL; size_t ancestry_count = 0;
    const char *chosen = NULL; int in_package = 0, result = -1;
    if (pool->state == NI_POOL_NONE) return 1;
    if (pool->state == NI_POOL_UNRESOLVED) {
        ni_refuse(site, SH_DECL_CARRIER_GAP, "the world's AI spawn pool is not resolved (%s); pooled spawns read each "
            "definition layer separately", pool->reason);
        return 0;
    }
    /* Entries resolve to declarations and suppression compares the
     * declaration's own name, so only this copy's generated name, before or
     * after conversion, can tie a listed name to this use. */
    for (size_t list = 0; list < 2; list++) {
        char **names = list ? pool->suppressed : pool->entries;
        size_t count = list ? pool->suppressed_count : pool->entry_count;
        for (size_t i = 0; i < count; i++)
            if (ni_generated_name(names[i], inherit, identity) || ni_generated_name(names[i], base, identity)) {
                ni_refuse(site, SH_DECL_CARRIER_GAP, "AI spawn pool names '%s', which can be this placed copy's generated "
                    "declaration name; converting renames it", names[i]);
                return 0;
            }
    }
    /* The native ancestry S.. the converted value keeps. */
    for (char *name = *base ? _strdup(base) : NULL; name; ) {
        sh_decl_source text = {0}; int served = 0; char error[256];
        char **grown = ancestry_count < 64 ? realloc(ancestry, (ancestry_count + 1) * sizeof(*grown)) : NULL;
        if (!grown) {
            free(name);
            if (ancestry_count < 64) goto done;
            ni_refuse(site, SH_DECL_CARRIER_GAP, "the native ancestry of '%s' does not end", base); result = 0; goto done;
        }
        ancestry = grown; ancestry[ancestry_count++] = name;
        int read = source->read(source->context, name, &text, &served, NULL);
        if (read < 0) goto done;
        if (!read) { ni_refuse(site, SH_DECL_CARRIER_GAP, "ancestor '%s' is unavailable", name); result = 0; goto done; }
        sh_decl_node *tree = sh_decl_tree_parse_ordered(text, error, sizeof(error));
        free((char *)text.text);
        if (!tree) { ni_refuse(site, SH_DECL_CARRIER_GAP, "ancestor '%s' does not parse", name); result = 0; goto done; }
        name = ni_parent(tree);
        sh_decl_tree_free(tree);
        if (name && !*name) { free(name); name = NULL; }
    }
    /* The first entry the declaration derives from selects the base. */
    for (size_t i = 0; i < pool->entry_count && !chosen; i++) {
        for (size_t j = 0; j < layer_count && !chosen; j++)
            if (!_stricmp(pool->entries[i], layers[j].name)) { chosen = pool->entries[i]; in_package = 1; }
        for (size_t j = 0; j < ancestry_count && !chosen; j++)
            if (!_stricmp(pool->entries[i], ancestry[j])) chosen = pool->entries[i];
    }
    if (!chosen) { result = 1; goto done; } /* neither form finds a base: both spawn fresh */
    if (in_package) {
        ni_refuse(site, SH_DECL_CARRIER_ENGINE, "the world's AI spawn pool selects '%s' of this package chain as the "
            "pooled base; a native inherit cannot derive from it", chosen);
        result = 0; goto done;
    }
    /* Both forms take the same base and read the layers below it one by one;
     * the shared native part is identical. */
    {
        char reason[300];
        result = ni_pool_layers(source, class_name, chosen, layers, layer_count, instance, merged, kept, chain_state, opener,
            reason, sizeof(reason));
        if (result == 0) ni_refuse(site, SH_DECL_CARRIER_GAP, "%s", reason);
    }
done:
    for (size_t i = 0; i < ancestry_count; i++) free(ancestry[i]);
    free(ancestry);
    return result;
}

/* ---- other uses of the same definition ----
 * Each other map use of a placed entity's definition is decided by what its
 * consumer does with the name, not by the co-reference alone. A SnapMap
 * spawner (VK 0x141284120 / GL 0x1412760b0) constructs a new instance of its
 * spawnerEntityPair entityStatic (+0xC90) or entityPinata (+0xC98) definition
 * by name through the game spawn (vtable +0x450). Nothing on that path selects
 * or counts existing entities by definition: the pre-spawn slot +0x8f0 (VK
 * 0x14127d810 / GL 0x14126f780) removes only the spawner's own previous spawn,
 * held in its handle at +0xCF0, and multiSpawnMode's capacity check (VK
 * 0x1411c1030 / GL 0x1411b2d60) compares the SnapMap manager's count of
 * handles that multi-mode spawns registered (VK 0x1411c73e0 / GL 0x1411b9110)
 * with 100. Such a use needs the definition, not this occurrence: the
 * occurrence may convert while the named use keeps its package.
 *
 * The engine selects existing entities by definition through one ancestry
 * test (VK 0x1417ad430 / GL 0x14179ff10): a declaration is the named one or
 * reaches it through its parent link (+0x160). Its callers pass an entity's
 * own declaration (+0x6d0). A placed entity's declaration is its registered
 * play copy (MapEntityForSnapEntity VK 0x1404f1350 / GL 0x1404f0c50), so
 * pointer equality with a named definition never selects it, but the ancestry
 * test does: before conversion the copy descends from P and every served layer
 * the conversion folds, afterwards only from the native base S. A consumer of
 * that kind, naming any folded layer, would stop selecting this occurrence.
 * When its exact match set is this occurrence alone (ni_sole_descendant), the
 * filter is rewritten to name the converted copy, which selects exactly that
 * set; otherwise the occurrence keeps the package as a product gap, not a
 * native limit, since naming S instead would broaden the match and one copy
 * cannot stand for several matches or future constructions. Each row names
 * the reflected owner class and typed site of a traced consumer (R-248,
 * R-278, R-281); a list row decides each entry. A consumer not traced leaves
 * the occurrence's identity undecided. */
/* NI_CONSUMER_IDENTITY compares an entity's own declaration (+0x6d0) with the
 * named definition exactly, with no ancestry (R-295). A placed occurrence's
 * declaration is its registered copy (R-201), which such a filter never names;
 * an independent use's instances carry the constructed definition itself. */
typedef enum ni_consumer_kind {
    NI_CONSUMER_CONSTRUCTS, NI_CONSUMER_ANCESTRY, NI_CONSUMER_IDENTITY,
    NI_CONSUMER_CONSTRUCTS_IDENTITY
} ni_consumer_kind;
static const struct ni_consumer {
    const char *owner_class, *reference, *what;
    ni_consumer_kind kind;
    int list;   /* an entityDef list: each item[N] is one filter entry */
    /* NULL for an entity class row; otherwise the declaration family whose own
     * text holds reference (owner_class is then NULL). In reference, [*] is
     * any one item index. */
    const char *family;
} ni_consumers[] = {
    {"idSnapMapGameEntity_Spawner", "/k:entityDef/k:state/k:edit/k:spawnerEntityPair/k:entityStatic",
        "a spawner's pair", NI_CONSUMER_CONSTRUCTS, 0},
    {"idSnapMapGameEntity_Spawner", "/k:entityDef/k:state/k:edit/k:spawnerEntityPair/k:entityPinata",
        "a spawner's pair", NI_CONSUMER_CONSTRUCTS, 0},
    /* The activation tests (VK 0x140938c00, 0x14093d1f0, 0x14093af50 / GL
     * 0x1409382c0, 0x14093c8c0, 0x14093a649) admit an activator whose own
     * declaration is typeCanActivate (+0xd20) or descends from it. */
    {"idTrigger", "/k:entityDef/k:state/k:edit/k:typeCanActivate", "a trigger's activator filter", NI_CONSUMER_ANCESTRY, 0},
    /* A dying entity drops the loot of the idRandomLoot whose lootClass
     * (+0xca0) its own declaration descends from (VK 0x14079b170, call
     * 0x14079b1e3 / GL call 0x14079a233). */
    {"idRandomLoot", "/k:entityDef/k:state/k:edit/k:lootClass", "a random loot class", NI_CONSUMER_ANCESTRY, 0},
    /* An idGameChallenge tests an entity's own declaration (+0x6d0) against
     * each entry of these lists and its demonPickupSpawn (VK 0x140832f6e,
     * 0x140832fae, 0x140832ffe and helper 0x1408360e0 from 0x14083612a;
     * callers VK 0x1408c67f6 / 0x1408c7e33, R-278). */
    {"idGameChallenge", "/k:entityDef/k:state/k:edit/k:allowedSpawnableItems", "a game challenge's item filter",
        NI_CONSUMER_ANCESTRY, 1},
    {"idGameChallenge", "/k:entityDef/k:state/k:edit/k:powerWeaponDefList", "a game challenge's power weapon filter",
        NI_CONSUMER_ANCESTRY, 1},
    {"idGameChallenge", "/k:entityDef/k:state/k:edit/k:powerUpsDefList", "a game challenge's power-up filter",
        NI_CONSUMER_ANCESTRY, 1},
    {"idGameChallenge", "/k:entityDef/k:state/k:edit/k:necessaryPickupsToSpawn", "a game challenge's pickup filter",
        NI_CONSUMER_ANCESTRY, 1},
    {"idGameChallenge", "/k:entityDef/k:state/k:edit/k:demonPickupSpawn", "a game challenge's demon pickup filter",
        NI_CONSUMER_ANCESTRY, 0},
    /* An entity's point-of-interest style is that of the first
     * pointOfInterestStyles entry whose pickupDecls list holds its own
     * declaration (+0x6d0) exactly (VK 0x14083a2b0, caller 0x1408c7f8e / GL
     * 0x140839520, R-295). */
    {"idGameChallenge", "/k:entityDef/k:state/k:edit/k:pointOfInterestStyles/k:item[*]/k:pickupDecls/k:item[*]",
        "a game challenge's exact point-of-interest pickup list", NI_CONSUMER_IDENTITY, 0},
    /* The boomerang tests a touched entity's declaration (+0x6d0) against
     * each allowablePickups entry (VK 0x14138b19e, caller 0x1408ca71f). */
    {"idProjectile_Boomerang", "/k:entityDef/k:state/k:edit/k:allowablePickups", "a boomerang's pickup filter",
        NI_CONSUMER_ANCESTRY, 1},
    /* idScannerBot_FindItem slot 28 (VK 0x1410c89c0 / GL 0x1410ba430) accepts
     * a candidate whose own declaration (+0x6d0) descends from any entry of the
     * searching AI's aiEditable (+0xbe38) scannerBotInterestEntities (+0x380)
     * list (R-285); its interest types list filters by class, not by name. */
    {"idAI2", "/k:entityDef/k:state/k:edit/k:aiEditable/k:scannerBotInterestEntities", "a scanner bot's item filter",
        NI_CONSUMER_ANCESTRY, 1},
    /* idTrigger_Damage admits a damage event only when the attacker's and the
     * inflictor's own declarations (+0x6d0) are exactly attackerDef (+0xd58)
     * and inflictorDef (+0xd60), when set (VK 0x140938ab0 / GL 0x140938170). */
    {"idTrigger_Damage", "/k:entityDef/k:state/k:edit/k:attackerDef", "a damage trigger's exact attacker filter",
        NI_CONSUMER_IDENTITY, 0},
    {"idTrigger_Damage", "/k:entityDef/k:state/k:edit/k:inflictorDef", "a damage trigger's exact inflictor filter",
        NI_CONSUMER_IDENTITY, 0},
    /* idGameChallenge_Bloodthirst acts on each tracked entity whose own
     * declaration is exactly soulEntityDef (+0x7730) (VK 0x14119fed0 / GL
     * 0x141191a20). */
    {"idGameChallenge_Bloodthirst", "/k:entityDef/k:state/k:edit/k:soulEntityDef",
        "a bloodthirst challenge's exact soul filter", NI_CONSUMER_IDENTITY, 0},
    /* The AI conductor classifies an AI by the first aiTypeList row whose
     * entityDef is exactly the AI's own declaration (+0x6d0) (VK 0x141124470 /
     * GL 0x141116250); global modifiers, kill score and AI filters all read
     * that class (package_keys.h). */
    {"idAIConductor", "/k:entityDef/k:state/k:edit/k:aiTypeList/k:item[*]/k:entityDef",
        "the AI conductor's type registration", NI_CONSUMER_CONSTRUCTS_IDENTITY, 0},
    /* The suit's autoExecGloryKill equipment modifier (idEnvironmentSuit +0x3c0)
     * admits an entity whose own declaration is exactly one of its
     * syncInteractionEntityDefs (+0x258) (VK 0x140623410, 0x140630a50). */
    {NULL, "/k:edit/k:modifiersEquipment/k:item[*]/k:syncInteractionEntityDefs/k:item[*]",
        "an equipment upgrade's exact glory-kill target list", NI_CONSUMER_IDENTITY, 0, "upgrade"},
    /* A loot drop spawns its item definition by name; the pending-item count
     * its spawn amount reads is decided by ni_loot_counted. */
    {NULL, "/k:edit/k:entityDef", "a loot drop's item", NI_CONSUMER_CONSTRUCTS, 0, "lootdrop"},
};
/* The native conductor copies these rows into its spawn manager, request
 * and goal, then constructs row.entityDef. The same ordered rows classify
 * entities by exact declaration identity. Both effects must survive one
 * coordinated substitution; neither alone describes this carrier. */
static int ni_consumer_constructs(const struct ni_consumer *consumer)
{
    return consumer && (consumer->kind == NI_CONSUMER_CONSTRUCTS ||
        consumer->kind == NI_CONSUMER_CONSTRUCTS_IDENTITY);
}
static int ni_consumer_identity(const struct ni_consumer *consumer)
{
    return consumer && (consumer->kind == NI_CONSUMER_IDENTITY ||
        consumer->kind == NI_CONSUMER_CONSTRUCTS_IDENTITY);
}
static int ni_consumer_is(const struct ni_consumer *consumer, ni_consumer_kind kind)
{
    return kind == NI_CONSUMER_CONSTRUCTS ? ni_consumer_constructs(consumer) :
        kind == NI_CONSUMER_IDENTITY ? ni_consumer_identity(consumer) : consumer && consumer->kind == kind;
}
static const struct ni_consumer ni_event_constructor = {NULL, NULL, "a verified event declaration argument",
    NI_CONSUMER_CONSTRUCTS, 0, NULL};
static int ni_event_map_constructs(const sh_native_inline_source *, const ni_scan *, const sh_native_inline_use *,
    const char *, int *);
static int ni_event_tree_constructs(const sh_native_inline_source *, const sh_decl_node *, const char *,
    const sh_decl_node *, int *);
/* A typed value path at a consumer's site: the scalar path itself, or one
 * item[N] entry of a list consumer. Map binding sites escape the brackets
 * (item%5bN%5d); a served declaration's own keys do not. */
static int ni_consumer_path(const struct ni_consumer *consumer, const char *reference)
{
    if (strstr(consumer->reference, "[*]")) {
        for (const char *pattern = consumer->reference; *pattern;) {
            if (!strncmp(pattern, "[*]", 3)) {
                int escaped = !_strnicmp(reference, "%5b", 3);
                if (!escaped && *reference != '[') return 0;
                reference += escaped ? 3 : 1;
                if (*reference < '0' || *reference > '9') return 0;
                while (*reference >= '0' && *reference <= '9') reference++;
                if (escaped ? _strnicmp(reference, "%5d", 3) : *reference != ']') return 0;
                reference += escaped ? 3 : 1; pattern += 3;
            } else if (_strnicmp(pattern++, reference++, 1)) return 0;
        }
        return !*reference;
    }
    size_t length = strlen(consumer->reference);
    if (_strnicmp(reference, consumer->reference, length)) return 0;
    if (!consumer->list) return !reference[length];
    const char *at = reference + length;
    int escaped = !_strnicmp(at, "/k:item%5b", 10);
    if (!escaped && strncmp(at, "/k:item[", 8)) return 0;
    at += escaped ? 10 : 8;
    if (*at < '0' || *at > '9') return 0;
    while (*at >= '0' && *at <= '9') at++;
    return escaped ? !_stricmp(at, "%5d") : !strcmp(at, "]");
}
/* The class entity's value loads with (its className, or its inherit chain's
 * class when it states none): 1 with class_name set, 0 unknown, -1 failure. */
/* The class a definition named name loads with, through its chain. */
static int ni_name_class(const sh_native_inline_source *source, const char *name, char *class_name, size_t capacity)
{
    char text[516], error[256], *found = NULL;
    size_t length = strlen(name);
    sh_decl_node *tree = NULL;
    int result = 0;
    if (!length || length >= sizeof(text) - 3) return 0;
    text[0] = '"'; memcpy(text + 1, name, length); text[length + 1] = '"'; text[length + 2] = 0;
    if (!(tree = ni_node(NULL, NULL, 1)) || !(tree->children = ni_node("inherit", text, 0))) {
        sh_decl_tree_free(tree);
        return -1;
    }
    {
        ni_class_view view = {source};
        sh_decl_entity_class_source classes = {&view, ni_class_read, ni_class_derives};
        found = sh_decl_entity_tree_class(tree, &classes, error, sizeof(error));
    }
    if (found && strlen(found) < capacity) { memcpy(class_name, found, strlen(found) + 1); result = 1; }
    free(found); sh_decl_tree_free(tree);
    return result;
}
static int ni_entity_class(const sh_native_inline_source *source, const ni_scan *scan, size_t entity,
    char *class_name, size_t capacity)
{
    const ni_span *span = NULL;
    sh_json_object original = {0}, normalized = {0};
    char text[512];
    size_t length = 0;
    int result = 0;
    for (size_t i = 0; i < scan->count && !span; i++) if (scan->spans[i].entity == entity) span = scan->spans + i;
    if (!span || !sh_native_json_parse_object(scan->json + span->offset, span->length, 64, &original)) return 0;
    int upgraded = ni_v_upgrade(&original, source->version, &normalized);
    if (upgraded != 1) { result = upgraded < 0 ? -1 : 0; goto done; }
    const char *value = sh_json_object_get(&normalized, "className");
    if (value) {
        result = sh_native_json_decode_string(value, strlen(value), class_name, capacity, &length) &&
            length && length < capacity && strlen(class_name) == length;
        goto done;
    }
    value = sh_json_object_get(&normalized, "inherit");
    if (!value || !sh_native_json_decode_string(value, strlen(value), text, sizeof(text), &length) ||
        !length || length >= sizeof(text) || strlen(text) != length) goto done;
    result = ni_name_class(source, text, class_name, capacity);
done:
    sh_json_object_free(&original); sh_json_object_free(&normalized);
    return result;
}
/* The traced consumer of one map use, or NULL when it is not traced; *failed
 * on failure. A use matches a row by its exact typed site and the native
 * ancestry of its owner entity's class; a reference's type alone never does. */
static const struct ni_consumer *ni_use_consumer(const sh_native_inline_source *source, const ni_scan *scan,
    const sh_native_inline_use *use, int *failed)
{
    const char *owner = use->owner && *use->owner ? use->owner : NULL;
    const char *reference = use->reference && *use->reference ? use->reference : NULL;
    char class_name[256], *end = NULL;
    int known = 0;
    if (reference && owner && !strncmp(owner, "entity/", 7) && owner[7] >= '0' && owner[7] <= '9') {
        unsigned long long index = strtoull(owner + 7, &end, 10);
        known = end && !*end ? ni_entity_class(source, scan, (size_t)index, class_name, sizeof(class_name)) : 0;
        if (known < 0) { *failed = 1; return NULL; }
    }
    for (size_t c = 0; known && c < sizeof(ni_consumers) / sizeof(*ni_consumers); c++)
        if (!ni_consumers[c].family && ni_consumer_path(ni_consumers + c, reference) &&
            source->derives(source->context, class_name, ni_consumers[c].owner_class) == 1)
            return ni_consumers + c;
    if (known && source->event_decl_constructs && ni_event_map_constructs(source, scan, use, class_name, failed))
        return &ni_event_constructor;
    return NULL;
}
static int ni_name_equal(const char *a, const char *b);
static int ni_name_char(char c);
/* The first node at or after node, or below one, holding a string literal
 * naming name, other than skip (an inherit header); NULL when none does. */
static const sh_decl_node *ni_tree_mentions(const sh_decl_node *node, const sh_decl_node *skip, const char *name)
{
    for (; node; node = node->next) {
        const char *text = NULL; size_t length = 0; char value[512];
        if (node != skip && sh_decl_tree_literal(node, &text, &length) && length < sizeof(value)) {
            memcpy(value, text, length); value[length] = 0;
            if (strlen(value) == length && ni_name_equal(value, name)) return node;
        }
        const sh_decl_node *found = ni_tree_mentions(node->children, skip, name);
        if (found) return found;
    }
    return NULL;
}
/* Walks the served chain from start (spelled as an inherit) to its first
 * unserved ancestor: *descends when a link is name, *mentions when a served
 * layer's text names it outside its inherit header. 1 walked, 0 not
 * established (reason set), -1 failure. */
static int ni_chain_names(const sh_native_inline_source *source, const char *start, const char *name,
    int *descends, int *mentions, char *reason, size_t capacity)
{
    char *link = _strdup(start), *replaces = NULL, error[256];
    int result = -1;
    if (!link) return -1;
    for (int depth = 0;; depth++) {
        sh_decl_source text = {0}; int served = 0;
        if (ni_name_equal(link, name)) { *descends = 1; result = 1; break; }
        if (depth == 64) { snprintf(reason, capacity, "'%s' has no finite inheritance chain", start); result = 0; break; }
        free(replaces); replaces = NULL;
        int read = source->read(source->context, link, &text, &served, &replaces);
        if (read < 0) { free((char *)text.text); break; }
        if (!read || !served) {
            free((char *)text.text);
            if (!read) snprintf(reason, capacity, "ancestor '%s' is unavailable", link);
            result = read ? 1 : 0; break;
        }
        sh_decl_node *tree = sh_decl_tree_parse_ordered(text, error, sizeof(error));
        free((char *)text.text);
        if (!tree) { snprintf(reason, capacity, "'%s' does not parse", link); result = 0; break; }
        sh_decl_entity_headers headers;
        sh_decl_entity_split(tree, &headers);
        if (ni_tree_mentions(tree->children, headers.inherit, name)) *mentions = 1;
        free(link);
        link = ni_parent(tree);
        sh_decl_tree_free(tree);
        if (!link && replaces) { link = replaces; replaces = NULL; }
        if (!link || !*link) { result = 1; break; }
    }
    free(link); free(replaces);
    return result;
}
/* An entity value's inherit: 1 set, 0 unknown, -1 failure. */
static int ni_entity_inherit(const sh_native_inline_source *source, const ni_scan *scan, size_t entity,
    char *inherit, size_t capacity)
{
    const ni_span *span = NULL;
    sh_json_object original = {0}, normalized = {0};
    size_t length = 0;
    int result = 0;
    for (size_t i = 0; i < scan->count && !span; i++) if (scan->spans[i].entity == entity) span = scan->spans + i;
    if (!span || !sh_native_json_parse_object(scan->json + span->offset, span->length, 64, &original)) return 0;
    int upgraded = ni_v_upgrade(&original, source->version, &normalized);
    if (upgraded == 1) {
        const char *value = sh_json_object_get(&normalized, "inherit");
        result = value && sh_native_json_decode_string(value, strlen(value), inherit, capacity, &length) &&
            length && length < capacity && strlen(inherit) == length;
    } else result = upgraded < 0 ? -1 : 0;
    sh_json_object_free(&original); sh_json_object_free(&normalized);
    return result;
}
/* 1 when every other use of name is an ancestry filter (selecting, never
 * constructing), 0 otherwise, -1 failure. */
static int ni_uses_select(const sh_native_inline_source *source, const ni_scan *scan, size_t entity, const char *name)
{
    int count = source->looked_up ? source->looked_up(source->context, entity, name, NULL, 0) : 0;
    int result = -1, failed = 0;
    sh_native_inline_use *uses = NULL;
    if (count <= 0) return count < 0 ? -1 : 1;
    if (!(uses = calloc((size_t)count, sizeof(*uses)))) return -1;
    if (source->looked_up(source->context, entity, name, uses, (size_t)count) == count) {
        result = 1;
        for (int i = 0; i < count && result == 1; i++) {
            const struct ni_consumer *consumer = ni_use_consumer(source, scan, uses + i, &failed);
            if (failed) result = -1;
            else if (!consumer || consumer->kind != NI_CONSUMER_ANCESTRY) result = 0;
        }
    }
    free(uses);
    return result;
}

/* ---- served declarations in actual use ----
 * A served declaration is not a map site, and its own consumers run only for
 * an instance play creates from it: as a layer of a placed occurrence's
 * chain, under a name map logic holds, as a replaced stock identity stock
 * code still names, or through another such declaration naming or inheriting
 * it. A map value can hold a name outside every typed entityDef field (a
 * by-name idEntityDefRef object, a string field, an event argument), so any
 * map string spelling a served name is a root too. A served definition
 * nothing instantiates selects nothing (a palette row
 * only places: source->placement decides it). The set is collected once per
 * document, when a conversion first needs it, and each use is decided by the
 * same typed consumer rows as map sites (ni_consumers). */
typedef struct ni_served_decl {
    char *family, *name, *parent;   /* parent: an entityDef's inherit, or NULL */
    sh_decl_node *tree;
    const sh_decl_node *inherit;    /* the inherit header, which is not a use */
    int live, visited;
    int stock;                      /* an unchanged native original (ni_constructed_chain) */
    char why[160];                  /* how play instantiates it */
    int replaces;                   /* a package replacement of a stock name: stock code can name it */
    int words;                      /* tree is its words: the generic grammar does not read it (ni_word_tree) */
} ni_served_decl;
typedef struct ni_served {
    int state;                      /* 0 not collected, 1 collected, 2 unknown (reason), -1 failed */
    char reason[400];
    ni_served_decl *decls; size_t count, capacity;
    size_t *slots, slot_mask;       /* decls by folded name (index + 1, 0 empty), once all are read */
} ni_served;
static void ni_served_free(ni_served *index)
{
    for (size_t i = 0; i < index->count; i++) {
        free(index->decls[i].family); free(index->decls[i].name); free(index->decls[i].parent);
        sh_decl_tree_free(index->decls[i].tree);
    }
    free(index->decls); free(index->slots);
    memset(index, 0, sizeof(*index));
}
/* ni_name_equal's folding: one leading '/' dropped, '\' as '/', ASCII case. */
static uint64_t ni_name_hash(const char *name)
{
    uint64_t hash = 14695981039346656037ull;
    if (*name == '/' && name[1] != '/') name++;
    for (; *name; name++) { hash ^= (unsigned char)ni_name_char(*name); hash *= 1099511628211ull; }
    return hash;
}
/* The name index ni_served_find and ni_served_mark probe: a served
 * declaration's words can number tens of thousands, each looked up. 0 on
 * allocation failure. */
static int ni_served_slots(ni_served *index)
{
    size_t capacity = 64;
    while (capacity < index->count * 2) capacity *= 2;
    size_t *slots = calloc(capacity, sizeof(*slots));
    if (!slots) return 0;
    for (size_t i = 0; i < index->count; i++) {
        size_t at = (size_t)ni_name_hash(index->decls[i].name) & (capacity - 1);
        while (slots[at]) at = (at + 1) & (capacity - 1);
        slots[at] = i + 1;
    }
    free(index->slots);
    index->slots = slots; index->slot_mask = capacity - 1;
    return 1;
}
/* Copies a literal node's text: 1 when it is one complete name. */
static int ni_literal_name(const sh_decl_node *node, char *value, size_t capacity)
{
    const char *text = NULL; size_t length = 0;
    if (!sh_decl_tree_literal(node, &text, &length) || !length || length >= capacity) return 0;
    memcpy(value, text, length); value[length] = 0;
    return strlen(value) == length;
}
/* The first served declaration (in collection order) spelled name. */
static const ni_served_decl *ni_served_find(const ni_served *index, const char *family, const char *name)
{
    if (index->slots) {
        size_t first = 0;
        for (size_t at = (size_t)ni_name_hash(name) & index->slot_mask; index->slots[at]; at = (at + 1) & index->slot_mask) {
            const ni_served_decl *decl = index->decls + index->slots[at] - 1;
            if ((!first || index->slots[at] < first) && (!family || !_stricmp(family, decl->family)) &&
                ni_name_equal(decl->name, name)) first = index->slots[at];
        }
        return first ? index->decls + first - 1 : NULL;
    }
    for (size_t i = 0; i < index->count; i++)
        if ((!family || !_stricmp(family, index->decls[i].family)) && ni_name_equal(index->decls[i].name, name))
            return index->decls + i;
    return NULL;
}
static int ni_served_mark_one(ni_served_decl *decl, const char *family, const char *name, const char *how,
    const char *detail)
{
    if (decl->live || (family && _stricmp(family, decl->family)) || !ni_name_equal(decl->name, name)) return 0;
    decl->live = 1;
    snprintf(decl->why, sizeof(decl->why), "%s%s", how, detail ? detail : "");
    return 1;
}
/* Marks every served declaration spelled name (of family, or any) live. */
static int ni_served_mark(ni_served *index, const char *family, const char *name, const char *how, const char *detail)
{
    int marked = 0;
    if (index->slots) {
        for (size_t at = (size_t)ni_name_hash(name) & index->slot_mask; index->slots[at]; at = (at + 1) & index->slot_mask)
            marked |= ni_served_mark_one(index->decls + index->slots[at] - 1, family, name, how, detail);
        return marked;
    }
    for (size_t i = 0; i < index->count; i++) marked |= ni_served_mark_one(index->decls + i, family, name, how, detail);
    return marked;
}
static int ni_served_names(ni_served *index, const sh_decl_node *node, const sh_decl_node *skip, const char *owner)
{
    int marked = 0;
    char value[512], detail[540];
    snprintf(detail, sizeof(detail), "%s'", owner);
    for (; node; node = node->next) {
        if (node == skip) continue;
        if (ni_literal_name(node, value, sizeof(value)))
            marked |= ni_served_mark(index, NULL, value, "named by served '", detail);
        marked |= ni_served_names(index, node->children, NULL, owner);
    }
    return marked;
}
static int ni_served_map_value(void *context, const sh_json_path *path, const char *json, size_t length,
    sh_json_kind kind)
{
    char value[512]; size_t decoded = 0;
    (void)path;
    if (kind == SH_JSON_STRING && sh_native_json_decode_string(json, length, value, sizeof(value), &decoded) &&
        decoded && decoded < sizeof(value) && strlen(value) == decoded)
        ni_served_mark(context, NULL, value, "spelled by a map value", NULL);
    return 1;
}
/* A declaration text of family as its native Parse reads it. idDeclFX Parse
 * (VK 0x14143f990 / GL 0x141433880) resets its state and calls the shared
 * reflected parser (VK 0x1417bc4d0 / GL 0x1417aefe0), for which end of file
 * may close the outermost brace (sh_decl_text_open_body_family; stock
 * fx/weapons/fists/fists_1st omits its final brace; docs/engine/fx-
 * declaration-parsing-delegates-to-the-shared-reflected-parser.md). Its FX
 * follow-up (VK 0x14143fa00 / GL 0x1414338f0) is not modelled: the walks read
 * the reflected fields only. Every other family's text is complete. */
static sh_decl_node *ni_family_parse(const char *family, sh_decl_source text, char *error, size_t capacity)
{
    return family && sh_decl_text_open_body_family(family) ? sh_decl_tree_parse_reflected_body(text, 1, error, capacity) :
        sh_decl_tree_parse_ordered(text, error, capacity);
}
/* A served declaration the generic grammar does not read (the block-grammar,
 * md6 and material families) as a tree of its words, which every walk below
 * reads as it reads a parsed tree: each name, number, quoted string and
 * literal the declaration lexer reads, and each run of tokens written with no
 * separation and no delimiter between them (a family reader whose name
 * characters are wider reads one name there), once each. A word holding a
 * double quote can spell nothing a walk compares. Words have no key, so no
 * typed consumer row applies: one naming a served declaration makes it live,
 * one naming a layer is a use whose consumer is not traced, and a generated
 * form is seen. NULL with error set when the lexer refuses the text. */
typedef struct ni_word { const char *text; size_t length; } ni_word;
static int ni_word_joins(const sh_decl_tokens *tokens, size_t i)
{
    const sh_decl_token *token = tokens->items + i;
    if (token->kind == SH_DECL_TOKEN_NAME || token->kind == SH_DECL_TOKEN_NUMBER) return 1;
    if (token->kind != SH_DECL_TOKEN_PUNCTUATION) return 0;
    for (size_t at = token->begin; at < token->end; at++) if (strchr("{}()[];,=", tokens->source.text[at])) return 0;
    return 1;
}
static sh_decl_node *ni_word_tree(sh_decl_source text, char *error, size_t capacity)
{
    sh_decl_tokens tokens = {0};
    if (!sh_decl_native_lex(text, &tokens, error, capacity)) return NULL;
    size_t limit = tokens.count * 2 + 1, slot_count = 64, count = 0;
    while (slot_count < limit * 2) slot_count *= 2;
    ni_word *words = malloc(limit * sizeof(*words));
    size_t *slots = calloc(slot_count, sizeof(*slots));
    sh_decl_node *root = calloc(1, sizeof(*root)), **tail = root ? &root->children : NULL;
    int failed = !words || !slots || !root;
    if (root) root->compound = 1;
    for (size_t i = 0; !failed && i < tokens.count; i++) {
        const sh_decl_token *token = tokens.items + i;
        ni_word candidates[2] = {{0}};
        size_t n = 0;
        if (token->kind == SH_DECL_TOKEN_STRING || token->kind == SH_DECL_TOKEN_LITERAL)
            candidates[n++] = (ni_word){text.text + token->begin + 1, token->end - token->begin - 2};
        else if (token->kind != SH_DECL_TOKEN_PUNCTUATION)
            candidates[n++] = (ni_word){text.text + token->begin, token->end - token->begin};
        /* The run this token begins, when the one before does not continue it. */
        if (ni_word_joins(&tokens, i) && !(i && ni_word_joins(&tokens, i - 1) && tokens.items[i - 1].end == token->begin)) {
            size_t last = i;
            while (last + 1 < tokens.count && ni_word_joins(&tokens, last + 1) &&
                tokens.items[last + 1].begin == tokens.items[last].end) last++;
            if (last > i) candidates[n++] = (ni_word){text.text + token->begin, tokens.items[last].end - token->begin};
        }
        for (size_t c = 0; !failed && c < n; c++) {
            ni_word word = candidates[c];
            if (!word.length || memchr(word.text, '"', word.length)) continue;
            uint64_t hash = 14695981039346656037ull;
            for (size_t k = 0; k < word.length; k++) { hash ^= (unsigned char)word.text[k]; hash *= 1099511628211ull; }
            size_t at = (size_t)hash & (slot_count - 1);
            int seen = 0;
            for (; slots[at] && !seen; at = (at + 1) & (slot_count - 1))
                seen = words[slots[at] - 1].length == word.length && !memcmp(words[slots[at] - 1].text, word.text, word.length);
            if (seen) continue;
            sh_decl_node *node = calloc(1, sizeof(*node));
            char *value = word.length < SIZE_MAX - 3 ? malloc(word.length + 3) : NULL;
            if (!node || !value) { free(node); free(value); failed = 1; break; }
            value[0] = '"'; memcpy(value + 1, word.text, word.length); value[word.length + 1] = '"'; value[word.length + 2] = 0;
            node->value = value;
            *tail = node; tail = &node->next;
            words[count] = word; slots[at] = ++count;
        }
    }
    free(words); free(slots); sh_decl_tokens_free(&tokens);
    if (failed) {
        sh_decl_tree_free(root);
        snprintf(error, capacity, "allocation failed");
        return NULL;
    }
    return root;
}
static int ni_served_collect(const sh_native_inline_source *source, const ni_scan *scan, ni_served *index)
{
    char error[256], inherit[512], detail[540];
    if (index->state) return index->state;
    index->state = -1;
    for (size_t i = 0; source->served_decl; i++) {
        const char *family = NULL, *name = NULL;
        int listed = source->served_decl(source->context, i, &family, &name);
        if (listed < 0) {
            snprintf(index->reason, sizeof(index->reason), "the product's served declarations are not all known");
            return index->state = 2;
        }
        if (!listed) break;
        if (!family || !name || !_stricmp(family, "snapeditorentitydef")) continue;
        sh_decl_source text = {0}; int served = 0, entitydef = !_stricmp(family, "entitydef");
        char *replaces = NULL;
        int read = entitydef ? source->read(source->context, name, &text, &served, &replaces) :
            source->read_decl ? source->read_decl(source->context, family, name, &text, &served) : -1;
        if (read <= 0 || !served) {
            free((char *)text.text); free(replaces);
            if (read < 0) {
                snprintf(index->reason, sizeof(index->reason), "served declaration '%s' cannot be read", name);
                return index->state = 2;
            }
            continue;
        }
        sh_decl_node *tree = ni_family_parse(entitydef ? NULL : family, text, error, sizeof(error));
        int words = 0;
        /* An entityDef's walks read its typed fields; another family's read
         * only its names, which its words hold. */
        if (!tree && !entitydef) words = (tree = ni_word_tree(text, error, sizeof(error))) != NULL;
        free((char *)text.text);
        if (!tree) {
            free(replaces);
            snprintf(index->reason, sizeof(index->reason), entitydef ? "served declaration '%s' does not parse" :
                "served declaration '%s' does not parse, and the declaration lexer does not read it: %s", name, error);
            return index->state = 2;
        }
        if (index->count == index->capacity) {
            size_t next = index->capacity ? index->capacity * 2 : 32;
            ni_served_decl *grown = realloc(index->decls, next * sizeof(*grown));
            if (!grown) { sh_decl_tree_free(tree); free(replaces); return -1; }
            index->decls = grown; index->capacity = next;
        }
        ni_served_decl *decl = index->decls + index->count++;
        memset(decl, 0, sizeof(*decl));
        decl->tree = tree; decl->words = words;
        decl->family = _strdup(family); decl->name = _strdup(name);
        if (!decl->family || !decl->name) { free(replaces); return -1; }
        if (entitydef) {
            sh_decl_entity_headers headers;
            sh_decl_entity_split(tree, &headers);
            decl->inherit = headers.inherit;
            decl->parent = ni_parent(tree);
        }
        if (replaces) {
            decl->live = decl->replaces = 1;
            snprintf(decl->why, sizeof(decl->why), "it replaces the stock '%s', which stock code can name", replaces);
        }
        free(replaces);
    }
    if (!ni_served_slots(index)) return -1;
    /* Each placed entity instantiates its inherit as saved and, when a
     * supported version upgrade renames it, as upgraded. */
    for (size_t i = 0; i < scan->count; i++) {
        const ni_span *span = scan->spans + i;
        sh_json_object original = {0}, normalized = {0};
        size_t length = 0;
        if (!sh_native_json_parse_object(scan->json + span->offset, span->length, 64, &original)) return -1;
        int upgraded = ni_v_upgrade(&original, source->version, &normalized);
        snprintf(detail, sizeof(detail), "%zu", span->entity);
        for (int pass = 0; pass < 2 && upgraded >= 0; pass++) {
            const char *value = sh_json_object_get(pass ? &normalized : &original, "inherit");
            if ((pass && upgraded != 1) || !value) continue;
            if (sh_native_json_decode_string(value, strlen(value), inherit, sizeof(inherit), &length) && length &&
                length < sizeof(inherit) && strlen(inherit) == length)
                ni_served_mark(index, "entitydef", inherit, "placed as entity/", detail);
        }
        sh_json_object_free(&original); sh_json_object_free(&normalized);
        if (upgraded < 0) return -1;
    }
    for (size_t i = 0; source->typed_name; i++) {
        const char *family = NULL, *name = NULL;
        int listed = source->typed_name(source->context, i, &family, &name);
        if (listed < 0) {
            snprintf(index->reason, sizeof(index->reason), "the map's typed declaration names are incomplete");
            return index->state = 2;
        }
        if (!listed) break;
        if (family && name) ni_served_mark(index, family, name, "named by map logic", NULL);
    }
    if (!sh_native_json_visit_values(scan->json, scan->json_length, 256, ni_served_map_value, index)) {
        snprintf(index->reason, sizeof(index->reason), "the map's values cannot all be read");
        return index->state = 2;
    }
    for (int changed = 1; changed;) {
        changed = 0;
        for (size_t i = 0; i < index->count; i++) {
            ni_served_decl *decl = index->decls + i;
            if (!decl->live || decl->visited) continue;
            decl->visited = 1;
            snprintf(detail, sizeof(detail), "%s'", decl->name);
            if (decl->parent && *decl->parent &&
                ni_served_mark(index, "entitydef", decl->parent, "inherited by served '", detail)) changed = 1;
            if (ni_served_names(index, decl->tree->children, decl->inherit, decl->name)) changed = 1;
        }
    }
    return index->state = 1;
}
/* Served declarations play still reads once this round's converted placed
 * copies replace their chains (H1): ni_served_collect's roots over the
 * document as the round writes it. A converted copy's inherit is no longer a
 * root and every value its replacement states is one; every other root is as
 * collected. Sets scan->round_remaining, or leaves it NULL when the index is
 * not established. 1, -1 failure. */
typedef struct ni_remaining_walk { ni_served *index; const char *base; const ni_span *spans; const char *skip; size_t count; } ni_remaining_walk;
static int ni_remaining_value(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    const ni_remaining_walk *w = context;
    size_t at = (size_t)(json - w->base);
    for (size_t i = 0; i < w->count; i++)
        if (w->skip[i] && at >= w->spans[i].offset && at < w->spans[i].offset + w->spans[i].length) return 1;
    return ni_served_map_value(w->index, path, json, length, kind);
}
static int ni_served_remaining(const sh_native_inline_source *source, ni_scan *scan, char *const *replacements)
{
    ni_served *index = scan->served;
    free(scan->round_remaining); scan->round_remaining = NULL;
    int converted = 0;
    for (size_t k = 0; scan->round && k < scan->round_placed && !converted; k++)
        converted = scan->round->sites[k].converted;
    if (!converted) return 1;
    int collected = index ? ni_served_collect(source, scan, index) : 2;
    if (collected < 0) return -1;
    if (collected != 1 || !index->count) return 1;
    size_t n = index->count;
    int *saved = malloc(n * 2 * sizeof(*saved));
    char (*why)[160] = malloc(n * sizeof(*why)), *remaining = calloc(n, 1), *skip = calloc(scan->count ? scan->count : 1, 1);
    char inherit[512], detail[540];
    int result = saved && why && remaining && skip ? 1 : -1;
    for (size_t i = 0; result == 1 && i < n; i++) {
        ni_served_decl *decl = index->decls + i;
        saved[2 * i] = decl->live; saved[2 * i + 1] = decl->visited; memcpy(why[i], decl->why, sizeof(why[i]));
        decl->live = decl->replaces; decl->visited = 0;
    }
    for (size_t s = 0; result == 1 && s < scan->count; s++) {
        const ni_span *span = scan->spans + s;
        for (size_t k = 0; scan->round && k < scan->round_placed && !skip[s]; k++)
            skip[s] = scan->round->sites[k].entity == span->entity && scan->round->sites[k].converted && replacements[s];
        if (skip[s]) {
            if (!sh_native_json_visit_values(replacements[s], strlen(replacements[s]), 256, ni_served_map_value, index))
                result = -1;
            continue;
        }
        /* As collected: the saved inherit and, renamed by a supported upgrade, the upgraded one. */
        sh_json_object original = {0}, normalized = {0};
        size_t length = 0;
        if (!sh_native_json_parse_object(scan->json + span->offset, span->length, 64, &original)) { result = -1; break; }
        int upgraded = ni_v_upgrade(&original, source->version, &normalized);
        snprintf(detail, sizeof(detail), "%zu", span->entity);
        for (int pass = 0; pass < 2 && upgraded >= 0; pass++) {
            const char *value = sh_json_object_get(pass ? &normalized : &original, "inherit");
            if ((pass && upgraded != 1) || !value) continue;
            if (sh_native_json_decode_string(value, strlen(value), inherit, sizeof(inherit), &length) && length &&
                length < sizeof(inherit) && strlen(inherit) == length)
                ni_served_mark(index, "entitydef", inherit, "placed as entity/", detail);
        }
        sh_json_object_free(&original); sh_json_object_free(&normalized);
        if (upgraded < 0) result = -1;
    }
    /* Typed names, except the inherit of a converted copy that no other map value spells. */
    for (size_t i = 0; result == 1 && source->typed_name; i++) {
        const char *family = NULL, *name = NULL;
        int listed = source->typed_name(source->context, i, &family, &name);
        if (listed < 0) result = -1;
        if (listed <= 0) break;
        int replaced = 0;
        for (size_t k = 0; scan->round && family && name && k < scan->round_placed && !replaced; k++)
            replaced = !_stricmp(family, "entitydef") && scan->round->sites[k].converted &&
                ni_name_equal(scan->round->sites[k].from, name);
        if (family && name && !replaced) ni_served_mark(index, family, name, "named by map logic", NULL);
    }
    ni_remaining_walk walk = {index, scan->json, scan->spans, skip, scan->count};
    if (result == 1 && !sh_native_json_visit_values(scan->json, scan->json_length, 256, ni_remaining_value, &walk)) result = -1;
    for (int changed = 1; result == 1 && changed;) {
        changed = 0;
        for (size_t i = 0; i < n; i++) {
            ni_served_decl *decl = index->decls + i;
            if (!decl->live || decl->visited) continue;
            decl->visited = 1;
            snprintf(detail, sizeof(detail), "%s'", decl->name);
            if (decl->parent && *decl->parent &&
                ni_served_mark(index, "entitydef", decl->parent, "inherited by served '", detail)) changed = 1;
            if (ni_served_names(index, decl->tree->children, decl->inherit, decl->name)) changed = 1;
        }
    }
    for (size_t i = 0; saved && why && i < n; i++) {
        ni_served_decl *decl = index->decls + i;
        if (remaining) remaining[i] = (char)decl->live;
        decl->live = saved[2 * i]; decl->visited = saved[2 * i + 1]; memcpy(decl->why, why[i], sizeof(why[i]));
    }
    free(saved); free(why); free(skip);
    if (result == 1) scan->round_remaining = remaining; else free(remaining);
    return result;
}
/* A served declaration play reads only through this round's converted placed
 * copies: their one inline layers state its text under the plan, as a
 * holder the plan converts states its native ancestor's (ni_site). */
static int ni_placed_only(const ni_scan *scan, size_t i)
{
    return scan->round_remaining && scan->served && i < scan->served->count && scan->served->decls[i].live &&
        !scan->round_remaining[i];
}
/* One served declaration's own uses of a folded layer, each decided by the
 * consumer row of its owner class and typed value path. */
typedef struct ni_served_use {
    const sh_native_inline_source *source;
    const ni_served_decl *decl;
    const char *name;
    char class_name[256];
    int class_state;                /* 0 unresolved, 1 known, 2 unknown */
    int failed;
    int independent;                /* deciding an independent use (R-293), not a placed copy */
    int identity_only;              /* name is the native target: only exact filters change */
    char reason[720];
} ni_served_use;
static void ni_served_classify(ni_served_use *use, const char *path, const sh_decl_node *node)
{
    const ni_served_decl *decl = use->decl;
    const struct ni_consumer *consumer = NULL;
    char full[600];
    if (!_stricmp(decl->family, "entitydef")) {
        if (!use->class_state) {
            int known = ni_name_class(use->source, decl->name, use->class_name, sizeof(use->class_name));
            if (known < 0) { use->failed = 1; return; }
            use->class_state = known ? 1 : 2;
        }
        snprintf(full, sizeof(full), "/k:entityDef/k:state%s", path);
        for (size_t c = 0; use->class_state == 1 && !consumer && c < sizeof(ni_consumers) / sizeof(*ni_consumers); c++)
            if (!ni_consumers[c].family && ni_consumer_path(ni_consumers + c, full) &&
                use->source->derives(use->source->context, use->class_name, ni_consumers[c].owner_class) == 1)
                consumer = ni_consumers + c;
        if (!consumer && use->class_state == 1 && use->source->event_decl_constructs &&
            ni_event_tree_constructs(use->source, decl->tree, use->class_name, node, &use->failed))
            consumer = &ni_event_constructor;
    } else if (!decl->words) {
        for (size_t c = 0; !consumer && c < sizeof(ni_consumers) / sizeof(*ni_consumers); c++)
            if (ni_consumers[c].family && !_stricmp(ni_consumers[c].family, decl->family) &&
                ni_consumer_path(ni_consumers + c, path))
                consumer = ni_consumers + c;
    }
    const char *origin = decl->stock ? "stock" : "served";
    /* A construction creates a new instance by name and selects no
     * occurrence; ni_sole_descendant counts it among a filter's matches. */
    if (ni_consumer_constructs(consumer) && !ni_consumer_identity(consumer)) return;
    /* Naming the native target itself, an ancestry filter or a construction
     * selects the same instances before and after; an exact filter does not. */
    if (use->identity_only && (!ni_consumer_identity(consumer))) return;
    if (ni_consumer_identity(consumer) && !use->independent) return; /* no placed copy (R-201) */
    if (ni_consumer_identity(consumer))
        snprintf(use->reason, sizeof(use->reason), "%s %s declaration '%s', which play instantiates (%s), has %s "
            "at %s, which matches only entities whose own declaration is '%s'; %s", origin, decl->family, decl->name,
            decl->why, consumer->what, path, use->name, use->identity_only ? "instances this use constructs would start "
            "matching it under the native name" : "an instance constructed under the native name would stop matching it");
    else if (consumer)
        snprintf(use->reason, sizeof(use->reason), "%s %s declaration '%s', which play instantiates (%s), has %s "
            "at %s, which selects entities descending from '%s'; %s", origin, decl->family, decl->name, decl->why,
            consumer->what, path, use->name, use->independent ? "an instance constructed under the native name would "
            "leave that served filter's match set" : "converting takes this placed copy out of that served filter's "
            "match set, and the served filter is not rewritten");
    else if (decl->words)
        snprintf(use->reason, sizeof(use->reason), "%s %s declaration '%s', which play instantiates (%s), names "
            "'%s' in text the generic grammar does not read (indexed by its words), and whether that reader selects "
            "this %s is not traced", origin, decl->family, decl->name, decl->why, use->name,
            use->independent ? "use's instances" : "occurrence");
    else
        snprintf(use->reason, sizeof(use->reason), "%s %s declaration '%s', which play instantiates (%s), names "
            "'%s' at %s, and whether that consumer selects this %s is not traced", origin, decl->family, decl->name,
            decl->why, use->name, path, use->independent ? "use's instances" : "occurrence");
}
static void ni_served_walk(ni_served_use *use, const sh_decl_node *node, const sh_decl_node *skip, char *path,
    size_t used, size_t capacity)
{
    char value[512];
    for (; node && !use->failed && !*use->reason; node = node->next) {
        if (node == skip) continue;
        size_t end = used;
        if (node->key) {
            int written = snprintf(path + used, capacity - used, "/k:%s", node->key);
            if (written < 0 || (size_t)written >= capacity - used) {
                path[used] = 0;
                snprintf(use->reason, sizeof(use->reason), "served %s declaration '%s' nests a value too deeply to "
                    "classify", use->decl->family, use->decl->name);
                return;
            }
            end = used + (size_t)written;
        }
        if (ni_literal_name(node, value, sizeof(value)) && ni_name_equal(value, use->name)) ni_served_classify(use, path, node);
        ni_served_walk(use, node->children, NULL, path, end, capacity);
        path[used] = 0;
    }
}
/* Whether a served declaration play instantiates names name, or a served
 * definition descending from it, in its own text: 1 with reason, 0 none, -1
 * failure. Either use can add instances an ancestry filter on name matches. */
static int ni_served_names_descendant(const sh_native_inline_source *source, const ni_served *index,
    const ni_served_decl *decl, const sh_decl_node *node, const sh_decl_node *skip, const char *name,
    char *reason, size_t capacity)
{
    char value[512], detail[256];
    for (; node; node = node->next) {
        if (node == skip) continue;
        if (ni_literal_name(node, value, sizeof(value))) {
            if (ni_name_equal(value, name)) {
                snprintf(reason, capacity, "served %s '%s' (%s) names it", decl->family, decl->name, decl->why);
                return 1;
            }
            if (ni_served_find(index, "entitydef", value)) {
                int descends = 0, mentions = 0;
                int walked = ni_chain_names(source, value, name, &descends, &mentions, detail, sizeof(detail));
                if (walked < 0) return -1;
                if (!walked || descends) {
                    snprintf(reason, capacity, "served %s '%s' (%s) names '%s', which %s", decl->family, decl->name,
                        decl->why, value, walked ? "descends from it" : detail);
                    return 1;
                }
            }
        }
        int found = ni_served_names_descendant(source, index, decl, node->children, NULL, name, reason, capacity);
        if (found) return found;
    }
    return 0;
}
/* An ancestry filter naming name selects exactly this placed occurrence when
 * no other entity's chain reaches name, no definition map logic names (other
 * than by an inherit or another such filter) descends from it, no served
 * layer reached from them names it, and no served declaration play
 * instantiates names it or a served definition descending from it
 * (ni_served_collect); stock definitions cannot name a package's served
 * definitions. The filter naming this converted copy then
 * selects the same set: the play builder registers every placed copy before
 * any entity spawns (MapEntityForSnapEntity), and only map logic naming the
 * copy can construct from it (ni_generated_uses).
 * 1 sole, 0 not (reason set), -1 failure. */
static int ni_sole_descendant(const sh_native_inline_source *source, const ni_scan *scan,
    const sh_native_inline_site *site, const char *name, char *reason, size_t capacity)
{
    char inherit[512];
    int descends = 0, mentions = 0;
    int walked = ni_chain_names(source, site->from, name, &descends, &mentions, reason, capacity);
    if (walked <= 0) return walked;
    if (mentions) { snprintf(reason, capacity, "this occurrence's own definition text names it"); return 0; }
    for (size_t i = 0; i < scan->count; i++) {
        size_t entity = scan->spans[i].entity;
        if (entity == site->entity) continue;
        int known = ni_entity_inherit(source, scan, entity, inherit, sizeof(inherit));
        if (known < 0) return -1;
        if (!known) { snprintf(reason, capacity, "entity/%zu's definition is not known", entity); return 0; }
        descends = mentions = 0;
        if ((walked = ni_chain_names(source, inherit, name, &descends, &mentions, reason, capacity)) <= 0) return walked;
        if (descends || mentions) {
            snprintf(reason, capacity, "entity/%zu %s it", entity, descends ? "also descends from" : "has a definition naming");
            return 0;
        }
    }
    /* A served declaration play instantiates that names name, or a served
     * definition descending from it, adds instances (a construction) or is
     * itself such a filter (decided by ni_internal_uses). */
    {
        ni_served *index = scan->served;
        int collected = index ? ni_served_collect(source, scan, index) : -1;
        if (collected < 0) return -1;
        if (collected == 2) { snprintf(reason, capacity, "%s", index->reason); return 0; }
        for (size_t i = 0; i < index->count; i++) {
            const ni_served_decl *decl = index->decls + i;
            if (!decl->live) continue;
            int found = ni_served_names_descendant(source, index, decl, decl->tree->children, decl->inherit, name,
                reason, capacity);
            if (found) return found < 0 ? -1 : 0;
        }
    }
    if (!source->typed_name) { snprintf(reason, capacity, "the map's typed declaration names are unavailable"); return 0; }
    for (size_t i = 0;; i++) {
        const char *family = NULL, *typed = NULL;
        int listed = source->typed_name(source->context, i, &family, &typed);
        if (listed < 0) { snprintf(reason, capacity, "the map's typed declaration names are incomplete"); return 0; }
        if (!listed) break;
        if (!family || !typed) continue;
        if (_stricmp(family, "entitydef")) {
            sh_decl_source text = {0}; int served = 0;
            int read = source->read_decl ? source->read_decl(source->context, family, typed, &text, &served) : -1;
            free((char *)text.text);
            if (read < 0 || (read && served)) {
                snprintf(reason, capacity, "map logic names the served %s declaration '%s'", family, typed);
                return 0;
            }
            continue;
        }
        if (ni_name_equal(typed, name)) continue;
        descends = mentions = 0;
        if ((walked = ni_chain_names(source, typed, name, &descends, &mentions, reason, capacity)) <= 0) return walked;
        int selects = descends ? ni_uses_select(source, scan, site->entity, typed) : 1;
        if (selects < 0) return -1;
        if (!selects || mentions) {
            snprintf(reason, capacity, "map logic names '%s', which %s it", typed, mentions ? "names" : "descends from");
            return 0;
        }
    }
    return 1;
}
/* Served declarations are not map sites, so looked_up never lists a consumer
 * inside one, yet a served declaration's own text can name a definition this
 * conversion folds: an idTrigger definition's typeCanActivate, a loot or
 * spawn table, a population entry. Only a declaration play instantiates runs
 * that consumer (ni_served_collect); a dormant one selects nothing. Each use
 * in an instantiated one is decided by its owner class and typed value path
 * (ni_consumers): a construction selects no occurrence, and is counted among
 * a filter's matches by ni_sole_descendant; an ancestry filter would stop
 * selecting the converted copy, and a served filter is not rewritten; any
 * other use is not traced. Those keep the package as precise preservation,
 * not a native limit. 1 none, 0 refused (reason set), -1 failure. */
static int ni_internal_uses(const sh_native_inline_source *source, const ni_scan *scan, sh_native_inline_site *site,
    const ni_layer *layers, size_t layer_count)
{
    ni_served *index = scan->served;
    int collected = index ? ni_served_collect(source, scan, index) : -1;
    if (collected < 0) return -1;
    if (collected == 2) { ni_refuse(site, SH_DECL_CARRIER_GAP, "%s", index->reason); return 0; }
    for (size_t i = 0; i < index->count; i++) {
        const ni_served_decl *decl = index->decls + i;
        if (!decl->live) continue;
        for (size_t j = 0; j < layer_count; j++) {
            ni_served_use use = {source, decl, layers[j].name};
            char path[512] = "";
            ni_served_walk(&use, decl->tree->children, decl->inherit, path, 0, sizeof(path));
            if (use.failed) return -1;
            if (*use.reason) { ni_refuse(site, SH_DECL_CARRIER_GAP, "%s", use.reason); return 0; }
        }
    }
    return 1;
}
/* 1 when every other map use of name, the definition this occurrence inherits
 * or a served layer its conversion folds, is a traced consumer that never
 * selects this occurrence (ni_consumers), or an ancestry filter whose exact
 * match set is this occurrence alone (ni_sole_descendant), recorded in
 * site->retargets to name the converted copy; 0 refused (reason set);
 * -1 failure. */
static int ni_other_uses(const sh_native_inline_source *source, const ni_scan *scan, sh_native_inline_site *site,
    const char *name)
{
    sh_native_inline_use *uses = NULL;
    int count = source->looked_up ? source->looked_up(source->context, site->entity, name, NULL, 0) : 0;
    int result = -1, failed = 0, constructs = 0, selects = 0, sole = 0;
    const struct ni_consumer *filter = NULL;
    const sh_native_inline_use *first = NULL;
    char reason[640], detail[320] = "";
    if (count <= 0) return count < 0 ? -1 : 1;
    if (!(uses = calloc((size_t)count, sizeof(*uses)))) return -1;
    if (source->looked_up(source->context, site->entity, name, uses, (size_t)count) != count) goto done;
    for (int i = 0; i < count; i++) {
        const sh_native_inline_use *use = uses + i;
        const struct ni_consumer *consumer = ni_use_consumer(source, scan, use, &failed);
        if (failed) goto done;
        if (ni_consumer_constructs(consumer)) { constructs = 1; continue; } /* a new instance by name */
        if (ni_consumer_identity(consumer)) continue; /* never names a placed copy (R-201) */
        if (consumer) { if (!selects++) { filter = consumer; first = use; } continue; }
        const char *owner = use->owner && *use->owner ? use->owner : NULL;
        const char *reference = use->reference && *use->reference ? use->reference : NULL;
        snprintf(reason, sizeof(reason), "%s%s%s also names '%s', and whether that consumer selects existing "
            "occurrences of it is not traced", owner ? owner : "a historical binding", reference ? " " : "",
            reference ? reference : "", name);
        ni_refuse(site, SH_DECL_CARRIER_GAP, "%s", reason);
        result = 0; goto done;
    }
    if (!selects) { result = 1; goto done; }
    if (constructs) snprintf(detail, sizeof(detail), "a use constructs further instances of it");
    else if ((sole = ni_sole_descendant(source, scan, site, name, detail, sizeof(detail))) < 0) goto done;
    if (!sole) {
        snprintf(reason, sizeof(reason), "%s %s is %s, which selects entities descending from '%s' (%s); converting "
            "takes this placed copy out of that ancestry, and no equivalent native filter is established",
            first->owner, first->reference, filter->what, name, detail);
        ni_refuse(site, SH_DECL_CARRIER_GAP, "%s", reason);
        result = 0; goto done;
    }
    for (int i = 0; i < count; i++) {
        struct sh_native_inline_retarget *grown = realloc(site->retargets, (site->retargeted + 1) * sizeof(*grown));
        if (!grown) goto done;
        site->retargets = grown;
        struct sh_native_inline_retarget *row = grown + site->retargeted++;
        row->owner = _strdup(uses[i].owner); row->reference = _strdup(uses[i].reference); row->name = _strdup(name);
        if (!row->owner || !row->reference || !row->name) goto done;
    }
    result = 1;
done:
    free(uses);
    return result;
}
/* Declaration names compare as the engine's lookup normalizes them: ASCII case
 * and separators fold, and one leading '/' is dropped (VK 0x141801380). */
static int ni_name_char(char c) { return c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ni_name_equal(const char *a, const char *b)
{
    if (*a == '/' && a[1] != '/') a++;
    if (*b == '/' && b[1] != '/') b++;
    for (; *a && *b; a++, b++) if (ni_name_char(*a) != ni_name_char(*b)) return 0;
    return !*a && !*b;
}
/* This placed copy's play declaration name under inherit (malloc-owned), or
 * NULL when a part is unknown; *failed on allocation failure. */
static char *ni_copy_name(const ni_identity *identity, const char *inherit, int *failed)
{
    if (!identity || !identity->unique || !identity->module_index || !identity->module_name || !inherit || !*inherit)
        return NULL;
    size_t size = strlen(identity->module_index) + strlen(identity->module_name) + strlen(inherit) +
        strlen(identity->unique) + 4;
    char *name = malloc(size);
    if (!name) { *failed = 1; return NULL; }
    snprintf(name, size, "%s_%s/%s_%s", identity->module_index, identity->module_name, inherit, identity->unique);
    return name;
}
/* A typed map name that can be this placed copy's generated declaration name
 * ties its consumer (a spawner pair spawning that copy's declaration, for one)
 * to this occurrence, and converting renames the copy. A use the map binder
 * bound to exactly this occurrence (local_use) follows it: the converted map
 * names the renamed copy at that same typed site. The play builder registers
 * every copy, each built from its entity's value, before any entity spawns
 * (MapEntityForSnapEntity), so the use still finds this occurrence's copy and
 * forward and cyclic uses stay valid. Any other name that can be this copy's,
 * before or after conversion (a declaration supplied under that name, a name
 * no occurrence route bound, or one that would start to name the renamed
 * copy), keeps the package. 1 allowed with *follow uses to rewrite, 0 refused
 * (reason set), -1 failure. */
static int ni_generated_uses(const sh_native_inline_source *source, sh_native_inline_site *site, const char *inherit,
    const char *base, const ni_identity *identity, size_t *follow)
{
    char *before = NULL, *after = NULL;
    int failed = 0, result = 0;
    *follow = 0;
    for (size_t i = 0; source->typed_name; i++) {
        const char *family = NULL, *name = NULL;
        int listed = source->typed_name(source->context, i, &family, &name);
        if (listed < 0) {
            ni_refuse(site, SH_DECL_CARRIER_GAP, "the map's typed declaration names are incomplete, so a name could be "
                "this placed copy's generated declaration%s", NULL);
            return 0;
        }
        if (!listed) break;
        if (!family || !name || _stricmp(family, "entitydef")) continue;
        if (ni_generated_name(name, inherit, identity) || (*base && ni_generated_name(name, base, identity))) {
            ni_refuse(site, SH_DECL_CARRIER_GAP, "map logic names '%s', which can be this placed copy's generated "
                "declaration name; converting renames it", name);
            return 0;
        }
    }
    before = ni_copy_name(identity, inherit, &failed);
    after = before ? ni_copy_name(identity, base, &failed) : NULL;
    if (failed) { result = -1; goto done; }
    for (size_t i = 0; source->local_use; i++) {
        const char *owner = NULL, *reference = NULL, *name = NULL;
        int listed = source->local_use(source->context, i, &owner, &reference, &name);
        if (listed < 0) {
            ni_refuse(site, SH_DECL_CARRIER_GAP, "the map's map-local declaration uses are incomplete, so one could name "
                "this placed copy%s", NULL);
            goto done;
        }
        if (!listed) break;
        if (!name || !owner || !reference) continue;
        int names = before ? ni_name_equal(name, before) : ni_generated_name(name, inherit, identity);
        int renamed = after ? ni_name_equal(name, after) : *base && ni_generated_name(name, base, identity);
        if (!names && !renamed) continue;
        if (names && after && !renamed) { (*follow)++; continue; }
        ni_refuse(site, SH_DECL_CARRIER_GAP, names ? "map logic names this placed copy's generated declaration '%s', and "
            "its converted copy's name is not modelled" : "map logic names '%s', which converting would make this placed "
            "copy's generated declaration name", name);
        goto done;
    }
    result = 1;
done:
    free(before); free(after);
    return result;
}

/* The plan's holder substitution, defined with the independent route below. */
typedef struct ni_sigma_view { const sh_native_inline_source *source; const ni_scan *scan; const char *class_name; } ni_sigma_view;
static int ni_sigma_rewrite(const sh_native_inline_source *source, const ni_scan *scan, sh_decl_node *node,
    const char *class_name, int alone, ni_marks *skip, size_t *substituted);
static int ni_sigma_reduce(void *context, const char *state, size_t length, char **reduced, size_t *reduced_length,
    size_t *removed, char *reason, size_t capacity);
/* Two reductions the evaluator applies in turn to each prepared text. */
typedef struct ni_reduction_pair { const sh_native_eval_reduction *first, *second; } ni_reduction_pair;
static int ni_reduce_pair(void *context, const char *state, size_t length, char **reduced, size_t *reduced_length,
    size_t *removed, char *reason, size_t capacity)
{
    const ni_reduction_pair *pair = context;
    char *middle = NULL; size_t middle_length = 0, ignored = 0;
    int result = pair->first->reduce(pair->first->context, state, length, &middle, &middle_length, removed, reason, capacity);
    if (result > 0) result = pair->second->reduce(pair->second->context, middle, middle_length, reduced, reduced_length,
        &ignored, reason, capacity);
    free(middle);
    return result;
}
/* The class a placed value loads with, as the typed walk in ni_site derives
 * it: its className, else its chain's. 1 known, 0 not (that walk refuses). */
static int ni_site_class(const sh_native_inline_source *source, const sh_decl_node *reference, const char *class_json,
    char *out, size_t capacity)
{
    size_t length = 0;
    if (class_json)
        return sh_native_json_decode_string(class_json, strlen(class_json), out, capacity, &length) && length < capacity &&
            strlen(out) == length;
    ni_class_view view = {source};
    sh_decl_entity_class_source classes = {&view, ni_class_read, ni_class_derives};
    char error[160], *inherited = sh_decl_entity_tree_class(reference, &classes, error, sizeof(error));
    int known = inherited && strlen(inherited) < capacity;
    if (known) snprintf(out, capacity, "%s", inherited);
    free(inherited);
    return known;
}

/* Returns 1 converted (replacement set), 0 refused (site reason set), -1 failure.
 * Decided under the plan's current holder substitution (scan->sigma). */
static int ni_site(const sh_native_inline_source *source, const ni_pool *pool, const ni_scan *scan,
    const char *inherit, const ni_identity *identity, const sh_json_object *definition, sh_native_inline_site *site,
    char **replacement)
{
    ni_layer *layers = NULL; size_t layer_count = 0, layer_capacity = 0;
    ni_marks marks = {0};
    ni_walker walker = {0};
    sh_decl_node *instance = NULL, *merged = NULL, *reference = NULL, *expanded_ref = NULL, *expanded_cand = NULL;
    int model_equal = 1;
    size_t substituted = 0, layered = 0;    /* references the plan renames: compared, and in the layer */
    char *base = NULL, *state_json = NULL, *replaces = NULL, *materialized = NULL, error[512] = "", loaded_class[256] = "";
    char *chain_state = NULL;
    size_t opener = SIZE_MAX;
    int folded = 0;
    const char *state = sh_json_object_get(definition, "state");
    const char *class_json = sh_json_object_get(definition, "className");
    int result = -1;
    /* The outer loop has applied the original native version policy before
     * choosing this inherit. The value reader needs state; className inherits
     * when omitted. Accepted output stamps the current version only below. */
    if (!state) { ni_refuse(site, SH_DECL_CARRIER_GAP, "entity value lacks state%s", NULL); result = 0; goto done; }
    int others = ni_other_uses(source, scan, site, inherit);
    if (others <= 0) { result = others; goto done; }
    /* Chain P..S(excl), nearest to P first. A package replacement of a stock
     * identity folds over that stock original; exactness is decided below. */
    char *name = _strdup(inherit);
    if (!name) goto done;
    for (;;) {
        sh_decl_source text = {0}; int served = 0;
        free(replaces); replaces = NULL;
        int read = source->read(source->context, name, &text, &served, &replaces);
        if (read < 0) { free(name); goto done; }
        if (!read) { ni_refuse(site, SH_DECL_CARRIER_GAP, "ancestor '%s' is unavailable", name); free(name); result = 0; goto done; }
        if (!served) { free((char *)text.text); base = name; break; }
        for (size_t i = 0; i < layer_count; i++) if (!strcmp(layers[i].name, name)) {
            ni_refuse(site, SH_DECL_CARRIER_GAP, "cyclic inheritance through '%s'", name);
            free((char *)text.text); free(name); result = 0; goto done;
        }
        /* Native reads accept a repeated member: the member walker (VK
         * 0x141a209b0) reads every occurrence in order, so each layer keeps its
         * occurrences and the fold below applies the merge's order. */
        sh_decl_node *tree = sh_decl_tree_parse_ordered(text, error, sizeof(error));
        free((char *)text.text);
        if (!tree) { ni_refuse(site, SH_DECL_CARRIER_GAP, "definition does not parse: %s", error); free(name); result = 0; goto done; }
        if (layer_count == layer_capacity) {
            size_t next = layer_capacity ? layer_capacity * 2 : 4;
            ni_layer *grown = realloc(layers, next * sizeof(*grown));
            if (!grown) { sh_decl_tree_free(tree); free(name); goto done; }
            layers = grown; layer_capacity = next;
        }
        layers[layer_count++] = (ni_layer){name, tree};
        /* Parse consumes each header once, by position (sh_decl_entity_split);
         * everything after them, a repeated or out-of-order header included,
         * is state text, which the fold below merges. */
        {
            sh_decl_entity_headers headers;
            sh_decl_entity_split(tree, &headers);
            for (const sh_decl_node *member = tree->children; member != headers.state; member = member->next) {
                const char *which = NULL;
                if (!ni_member_allowed(tree, member, &which)) {
                    ni_refuse(site, SH_DECL_CARRIER_GAP, "entity definition member '%s' has no inline state meaning", which);
                    result = 0; goto done;
                }
            }
        }
        name = ni_parent(tree);
        if (!name && replaces) { name = replaces; replaces = NULL; folded = 1; }
        /* A chain ending in a served definition that inherits nothing folds
         * over an empty inherit, which the native reader accepts without a
         * lookup (VK FUN_1417ae380, GL 1417a0e60; D7 N4/U3). */
        if (!name || !*name) {
            free(name);
            if (!(base = _strdup(""))) goto done;
            break;
        }
    }
    {
        int internal = ni_internal_uses(source, scan, site, layers, layer_count);
        if (internal <= 0) { result = internal; goto done; }
    }
    /* The converted copy no longer descends from any served layer it folds,
     * so the other uses of each are decided as P's were (ni_consumers). */
    for (size_t i = 1; i < layer_count; i++) {
        int others = ni_other_uses(source, scan, site, layers[i].name);
        if (others <= 0) { result = others; goto done; }
    }
    size_t follow = 0;
    {
        int allowed = ni_generated_uses(source, site, inherit, base, identity, &follow);
        if (allowed <= 0) { result = allowed; goto done; }
    }
    /* The converted entity must itself pass SnapMap's map validation. */
    if (source->placement) {
        sh_decl_carrier_limit limit = SH_DECL_CARRIER_GAP;
        const char *why = source->placement(source->context, inherit, base, &limit);
        if (why) { ni_refuse(site, limit, "%s", why); result = 0; goto done; }
    }
    /* Loaded instance state, with its native JSON kept per leaf. */
    instance = sh_decl_carrier_tree(state, strlen(state), ni_record, &marks, error, sizeof(error));
    if (!instance) { ni_refuse(site, SH_DECL_CARRIER_GAP, "instance state: %s", error); result = 0; goto done; }
    /* One inline layer: the chain's edits from S outward, then the instance. */
    if (!(merged = ni_node(NULL, NULL, 1))) goto done;
    {
        sh_decl_node *inherit_node = ni_node("inherit", NULL, 0);
        ni_text quoted = {0};
        if (!inherit_node || !ni_append(&quoted, "\"", 1) || !ni_append_text(&quoted, base) || !ni_append(&quoted, "\"", 1)) {
            sh_decl_tree_free(inherit_node); free(quoted.data); goto done;
        }
        inherit_node->value = quoted.data; merged->children = inherit_node;
    }
    /* Each layer merges with the flag set only once the state tree has a node
     * (VK 0x141a65560 passes *tree != 0); before that, every occurrence is
     * appended (VK 0x141a65410 / GL 0x141a578e0). Only the prepared text's
     * first statement is state (SetEntityEditState), so the first stateful
     * layer contributes its first statement, repeats inside it kept, and its
     * later statements are inert. Later layers update that statement by first
     * match: only their statements of its name reach it, each in order, and
     * their sequential application equals one application of the fold.
     * SetEntityEditState checks no statement name, so the first statement
     * keeps its own name through the fold. The instance carrier states edit:
     * over a stateless native base the inline layer is the first stateful
     * one, whose name nothing reads, so it is named edit after the fold; over
     * a native base whose state has another name no inline update reaches it. */
    int root_open = 0, fold = 1, opened = 0;
    char *state_name = NULL;
    {
        int empty = *base ? ni_state_empty(source, base, &state_name) : 1;
        if (empty < 0) goto done;
        if (empty == 2) { ni_refuse(site, SH_DECL_CARRIER_GAP, "the native ancestry of '%s' is not readable", base); result = 0; goto done; }
        root_open = opened = empty;
    }
    for (size_t i = layer_count; i > 0 && fold > 0; i--) {
        sh_decl_entity_headers headers;
        sh_decl_entity_split(layers[i - 1].tree, &headers);
        for (const sh_decl_node *edit = headers.state; edit && fold > 0; edit = edit->next) {
            if (!root_open && strcmp(edit->key, state_name)) continue;
            if (!edit->compound || !edit->assignment) {
                ni_refuse(site, SH_DECL_CARRIER_GAP, "'%s' state statement is not a block", layers[i - 1].name);
                fold = 0; break;
            }
            if (!opened && strcmp(edit->key, "edit")) {
                ni_refuse(site, SH_DECL_CARRIER_GAP, "the chain's prepared state statement is named '%.80s'; one inline "
                    "layer states edit", edit->key);
                fold = 0; break;
            }
            if (root_open) {
                sh_decl_node *copy = ni_copy(edit, NULL), **tail = &merged->children;
                if (!copy || !(state_name = _strdup(edit->key))) { sh_decl_tree_free(copy); fold = -1; break; }
                while (*tail) tail = &(*tail)->next;
                *tail = copy;
                root_open = 0; opener = i - 1;
                break;
            }
            sh_decl_node wrapper = {0}; wrapper.compound = 1; wrapper.children = (sh_decl_node *)edit;
            sh_decl_node *saved_next = edit->next; ((sh_decl_node *)edit)->next = NULL;
            const char *failure = ni_overlay(merged, &wrapper, NULL);
            ((sh_decl_node *)edit)->next = saved_next;
            if (failure) { ni_refuse(site, SH_DECL_CARRIER_GAP, "cannot combine the chain's edits: %s", failure); fold = 0; }
        }
    }
    if (fold > 0 && opened && state_name && strcmp(state_name, "edit")) {
        /* The instance's edit merges by first match under this name too, so
         * its values are inert in the original; one inline layer would make
         * them effective or drop them. */
        for (const sh_decl_node *member = instance->children; member && fold > 0; member = member->next)
            if (!member->compound || member->children) {
                ni_refuse(site, SH_DECL_CARRIER_GAP, "the chain's prepared state statement is named '%.80s', under which "
                    "this instance's values are inert; one inline layer cannot keep them", state_name);
                fold = 0;
            }
        for (sh_decl_node *member = merged->children; fold > 0 && member; member = member->next)
            if (!strcmp(member->key, state_name)) {
                char *renamed = _strdup("edit");
                if (!renamed) { fold = -1; break; }
                free(member->key); member->key = renamed; break;
            }
    }
    chain_state = state_name;
    if (fold == 0) { result = 0; goto done; }
    if (fold < 0) goto done;
    {
        const char *failure = ni_overlay(merged, instance, &marks);
        if (failure) { ni_refuse(site, SH_DECL_CARRIER_GAP, "cannot combine the instance values: %s", failure); result = 0; goto done; }
    }
    /* Exactness of the single layer: the same expanded state as P's chain. */
    {
        ni_class_view view = {source};
        sh_decl_entity_class_source classes = {&view, ni_class_read, ni_class_derives};
        if (!(reference = ni_copy(instance, NULL))) goto done;
        sh_decl_node *inherit_node = ni_node("inherit", NULL, 0);
        ni_text quoted = {0};
        if (!inherit_node || !ni_append(&quoted, "\"", 1) || !ni_append_text(&quoted, inherit) || !ni_append(&quoted, "\"", 1)) {
            sh_decl_tree_free(inherit_node); free(quoted.data); goto done;
        }
        inherit_node->value = quoted.data; inherit_node->next = reference->children; reference->children = inherit_node;
        expanded_ref = sh_decl_entity_expanded_state(reference, &classes, error, sizeof(error));
        if (!expanded_ref) { ni_refuse(site, SH_DECL_CARRIER_GAP, "chain state is not modelled: %s", error); result = 0; goto done; }
        expanded_cand = sh_decl_entity_expanded_state(merged, &classes, error, sizeof(error));
        if (!expanded_cand) {
            /* A failed source expansion is product coverage, not a native limit. */
            ni_refuse(site, SH_DECL_CARRIER_GAP, "the inline layer is not modelled: %s", error);
            result = 0; goto done;
        }
        /* Compared under the plan's holder substitution, as an independent
         * use is: on both sides each typed entityDef reference to a served
         * holder the plan converts names its native ancestor, and each loot
         * drop reference to a drop the plan substitutes names its native drop,
         * as the converted layer (below) and the plan's other uses will. */
        if (scan->sigma_count) {
            char sigma_class[256];
            size_t renamed = 0;
            if (ni_site_class(source, reference, class_json, sigma_class, sizeof(sigma_class)) &&
                ((expanded_ref->children && ni_sigma_rewrite(source, scan, expanded_ref->children, sigma_class, 0, NULL,
                    &substituted) < 0) ||
                (expanded_cand->children && ni_sigma_rewrite(source, scan, expanded_cand->children, sigma_class, 0, NULL,
                    &renamed) < 0))) goto done;
            substituted += renamed;
        }
        int equal = ni_equal(expanded_ref, expanded_cand);
        if (equal < 0) goto done;
        if (!equal && folded) {
            /* Values the replacement omits could be restored as explicit
             * defaults; the product does not model that yet. */
            ni_refuse(site, SH_DECL_CARRIER_GAP, "one inline layer over stock '%s' does not reproduce its package "
                "replacement; restoring omitted stock values is not modelled", base);
            result = 0; goto done;
        }
        /* Inequality here is a source-model doubt (inherited lists, resets,
         * defaults); the native verdict below decides. */
        model_equal = equal;
    }
    /* Typed walk of the converted layer under the instance's class: its own
     * className, or without one the class its chain gives (the native default).
     * Converting replaces that chain, and expanded edit state does not carry
     * the class: a value without className whose new base gives another class
     * (a served layer selected one, or the chain has no native ancestor, so
     * the base gives none) states the original class as className, which the
     * native value reader reads (D7). Checked by deriving the converted
     * value's class again. */
    {
        char class_name[256]; size_t class_length = 0;
        if (class_json) {
            if (!sh_native_json_decode_string(class_json, strlen(class_json), class_name, sizeof(class_name), &class_length) ||
                class_length >= sizeof(class_name) || strlen(class_name) != class_length) {
                ni_refuse(site, SH_DECL_CARRIER_GAP, "className is not a plain class name%s", NULL); result = 0; goto done;
            }
        } else {
            ni_class_view view = {source};
            sh_decl_entity_class_source classes = {&view, ni_class_read, ni_class_derives};
            char *inherited = sh_decl_entity_tree_class(reference, &classes, error, sizeof(error)), *converted = NULL;
            if (!inherited) { ni_refuse(site, SH_DECL_CARRIER_GAP, "the inherited class is not modelled: %s", error); result = 0; goto done; }
            if (strlen(inherited) >= sizeof(class_name)) { free(inherited); ni_refuse(site, SH_DECL_CARRIER_GAP, "class name too long%s", NULL); result = 0; goto done; }
            snprintf(class_name, sizeof(class_name), "%s", inherited);
            if (*base) {
                char ignored[256];
                converted = sh_decl_entity_tree_class(merged, &classes, ignored, sizeof(ignored));
            }
            if (!converted || strcmp(converted, inherited)) materialized = inherited;
            else free(inherited);
            free(converted);
        }
        snprintf(loaded_class, sizeof(loaded_class), "%s", class_name);
        walker.inner = source->schema;
        sh_decl_dependency_schema schema = {&walker, ni_describe, ni_field, ni_reference, ni_walk_gap, ni_dynamic, NULL, ni_value};
        sh_decl_dependency_result walked = {0};
        sh_decl_entity_dependencies(merged, (sh_decl_value_type){class_name, ""}, &schema, &walked);
        if (walker.failed || walked.aborted) goto done;
        const ni_gap *gap = ni_authored_gap(merged, &marks, &walker);
        if (gap) {
            snprintf(error, sizeof(error), "%s (%s)", gap->path, gap->reason ? gap->reason : "unverified");
            ni_refuse(site, SH_DECL_CARRIER_GAP, "authored field has no verified native reader: %s", error); result = 0; goto done;
        }
    }
    /* Repeats a root-most layer kept ride in one inline layer only through a
     * proved complete-assignment reduction (R-212), decided on a copy so a
     * refusal classifies the unreduced layer. The evaluator applies the same
     * proof to both prepared texts; authored texts and the reference stay. */
    size_t reduced = 0;
    {
        sh_decl_node *edit = (sh_decl_node *)sh_decl_tree_member(merged, "edit");
        char repeated[200], why[320] = "";
        int found = edit ? ni_repeated(edit, repeated, sizeof(repeated)) : 0;
        if (found < 0) goto done;
        if (found) {
            sh_decl_node *copy = ni_copy(edit, &marks);
            if (!copy) goto done;
            int proved = ni_reduce_block(source->schema, copy, (sh_decl_value_type){loaded_class, ""}, "edit", &marks,
                &reduced, why, sizeof(why), 0);
            if (proved > 0) {
                char left[200];
                int again = ni_repeated(copy, left, sizeof(left));
                if (again) { proved = again < 0 ? -1 : 0; snprintf(why, sizeof(why), "'%.120s' still repeats", left); }
            }
            if (proved > 0) { sh_decl_node *children = edit->children; edit->children = copy->children; copy->children = children; }
            sh_decl_tree_free(copy);
            if (proved < 0) goto done;
            if (!proved) { result = ni_root_repeats(source, edit, loaded_class, repeated, why, site); goto done; }
        }
    }
    /* The converted layer names each holder the plan converts by its native
     * ancestor, and each substituted loot drop by its native drop, as its
     * comparison did, so the output no longer reads them. Instance values keep
     * their own native JSON: each is a map use the plan decides at its own site
     * (ni_follows_plan). */
    if (scan->sigma_count) {
        sh_decl_node *edit = (sh_decl_node *)sh_decl_tree_member(merged, "edit");
        int walked = edit ? ni_sigma_rewrite(source, scan, edit, loaded_class, 1, &marks, &layered) : 1;
        if (walked < 0) goto done;
        if (!walked) {
            ni_refuse(site, SH_DECL_CARRIER_GAP, "the typed walk of the converted layer did not complete, so the plan's "
                "holder substitution cannot be applied to it%s", NULL);
            result = 0; goto done;
        }
    }
    site->sigma = substituted > 0 || layered > 0;
    /* Every declaration value the converted layer states names a source a
     * map value binds to (ni_absent_site). */
    {
        ni_absent_walk absent = {{source->schema}, source, &marks, "", ""};
        sh_decl_dependency_schema schema = {&absent, ni_describe, ni_field, ni_reference, ni_walk_gap, ni_dynamic,
            ni_absent_site, NULL};
        sh_decl_dependency_result walked = {0};
        sh_decl_entity_dependencies(merged, (sh_decl_value_type){loaded_class, ""}, &schema, &walked);
        int failed = absent.base.failed;
        ni_walker_free(&absent.base);
        if (failed) goto done;
        if (*absent.name) {
            site->converted = 0; site->limit = SH_DECL_CARRIER_GAP; site->conflict = 0;
            snprintf(site->reason, sizeof(site->reason), "the converted layer would state '%s' at %s as map data, which "
                "no declaration in this map's context defines: its declaration text leaves that name to the engine's "
                "default, but a map's own value must name a source", absent.name, absent.path);
            result = 0; goto done;
        }
    }
    /* Carrier JSON of every member except the inherit header. */
    {
        sh_decl_node *inherit_node = merged->children;
        sh_decl_carrier_refusal refusal;
        ni_emit emit = {&walker, &marks};
        merged->children = inherit_node->next;
        state_json = sh_decl_carrier_state(merged, ni_leaf, &emit, NULL, &refusal);
        merged->children = inherit_node;
        if (!state_json) {
            if (refusal.limit == SH_DECL_CARRIER_EXACT) goto done;
            snprintf(error, sizeof(error), "%s: %s", refusal.path, refusal.reason);
            ni_refuse(site, refusal.limit, "%s", error); result = 0; goto done;
        }
    }
    /* The entity value with inherit and state replaced and, where the class
     * would change, className stated first, as native values state it; other
     * members exact. */
    {
        ni_text out = {0};
        int ok = ni_append(&out, "{", 1);
        size_t emitted = 0;
        if (ok && materialized)
            ok = ni_json_string(&out, "className") && ni_append(&out, ":", 1) && ni_json_string(&out, materialized);
        if (materialized) emitted++;
        for (size_t i = 0; ok && i < definition->count; i++) {
            const sh_json_member *member = definition->members + i;
            if (emitted++ && !ni_append(&out, ",", 1)) { ok = 0; break; }
            const char *key = member->key;
            ok = ni_json_string_n(&out, key, member->key_length) && ni_append(&out, ":", 1);
            if (ok && member->key_length == 7 && !memcmp(key, "inherit", 7)) ok = ni_json_string(&out, base);
            else if (ok && member->key_length == 5 && !memcmp(key, "state", 5)) ok = ni_append_text(&out, state_json);
            else if (ok && member->key_length == 8 && !memcmp(key, "~version", 8)) ok = ni_append_text(&out, "26");
            else if (ok) ok = ni_append_text(&out, member->value_json);
        }
        if (ok && !sh_json_object_get(definition, "~version"))
            ok = (!emitted || ni_append(&out, ",", 1)) && ni_append_text(&out, "\"~version\":26");
        if (!ok || !ni_append(&out, "}", 1)) { free(out.data); goto done; }
        *replacement = out.data;
    }
    /* Both values are evaluated as they load: the original states its own
     * className, if any; the converted one states the class it carries. */
    {
        ni_class_view view = {source};
        sh_decl_entity_class_source classes = {&view, ni_class_read, ni_class_derives};
        if ((class_json && !ni_state_class(reference, loaded_class, &classes)) ||
            !ni_state_class(merged, loaded_class, &classes)) {
            free(*replacement); *replacement = NULL; goto done;
        }
    }
    /* The prepared-text evaluation below covers fresh spawns; a configured
     * world pool also reads the layers one by one. */
    {
        int pooled = ni_pool_site(source, pool, site, inherit, base, layers, layer_count, instance, merged,
            loaded_class, identity, reduced > 0, chain_state, opener);
        if (pooled <= 0) { free(*replacement); *replacement = NULL; result = pooled; goto done; }
    }
    /* Source-tree agreement only pre-filters. The native evaluator is the
     * accepting authority; without its EQUAL the complete package stays. */
    /* Under the plan's holder substitution both prepared texts are renamed as
     * the comparison was, after any repeat reduction. */
    ni_reduction_view reduction_view = {source->schema, loaded_class};
    sh_native_eval_reduction reduction = {&reduction_view, ni_reduce_prepared};
    ni_sigma_view sigma_view = {source, scan, loaded_class};
    sh_native_eval_reduction sigma_reduction = {&sigma_view, ni_sigma_reduce};
    ni_reduction_pair pair = {&reduction, &sigma_reduction};
    sh_native_eval_reduction both = {&pair, ni_reduce_pair};
    const sh_native_eval_reduction *applied = reduced && site->sigma ? &both : reduced ? &reduction :
        site->sigma ? &sigma_reduction : NULL;
    if (!ni_native_verdict(source, site, base, reference, merged, applied, model_equal ? (site->sigma ?
            "the source model agrees under the plan's holder substitution" : "the source model agrees") :
            "the source model of one inline layer differs from this chain's inherited lists or resets")) {
        free(*replacement); *replacement = NULL; result = 0; goto done;
    }
    site->converted = 1; site->limit = SH_DECL_CARRIER_EXACT; site->reason[0] = 0; site->renamed = follow; site->conflict = 0;
    if (!(site->to = _strdup(base))) { free(*replacement); *replacement = NULL; goto done; }
    result = 1;
done:
    for (size_t i = 0; i < layer_count; i++) { free(layers[i].name); sh_decl_tree_free(layers[i].tree); }
    free(layers); free(base); free(state_json); free(replaces); free(materialized); free(chain_state);
    ni_marks_free(&marks); ni_walker_free(&walker);
    sh_decl_tree_free(instance); sh_decl_tree_free(merged); sh_decl_tree_free(reference);
    sh_decl_tree_free(expanded_ref); sh_decl_tree_free(expanded_cand);
    return result;
}

/* The entity's uniqueId text, or NULL when it is absent or not one plain value. */
static const char *ni_unique_text(const ni_scan *scan, size_t entity)
{
    const char *text = NULL;
    for (size_t i = 0; i < scan->unique_count; i++) if (scan->uniques[i].entity == entity) {
        if (text && strcmp(text, scan->uniques[i].text)) return NULL;
        text = scan->uniques[i].text;
    }
    return text;
}

/* The copy's identity parts: its uniqueId and, when instanceEntities places
 * that uniqueId in exactly one of instances + 1 groups, the module index. */
static const ni_identity *ni_identity_of(const ni_scan *scan, size_t entity, ni_identity *identity)
{
    memset(identity, 0, sizeof(*identity));
    identity->unique = ni_unique_text(scan, entity);
    const ni_numbers *keys = &scan->keys, *values = &scan->values;
    if (!identity->unique || keys->failed || values->failed || keys->count != scan->instances + 2 ||
        keys->rows[0] != 0 || keys->rows[keys->count - 1] != values->count) return identity;
    for (size_t k = 1; k < keys->count; k++) if (keys->rows[k] < keys->rows[k - 1]) return identity;
    size_t found = SIZE_MAX, unique = (size_t)strtoull(identity->unique, NULL, 10);
    for (size_t j = 0; j < values->count; j++) if (values->rows[j] == unique) {
        if (found != SIZE_MAX) return identity; /* listed twice: ambiguous */
        found = j;
    }
    if (found == SIZE_MAX) return identity;
    size_t group = 0;
    while (group + 1 < keys->count && keys->rows[group + 1] <= found) group++;
    snprintf(identity->index_text, sizeof(identity->index_text), "%zu", group);
    identity->module_index = identity->index_text;
    if (group == scan->instances) identity->module_name = "GlobalModule";
    else {
        const ni_module *module = NULL;
        for (size_t i = 0; i < scan->module_count; i++) if (scan->modules[i].instance == group) {
            if (module) return identity; /* moduleName stated twice */
            module = scan->modules + i;
        }
        identity->module_name = module ? module->name : NULL;
    }
    return identity;
}

static void ni_scan_free(ni_scan *scan)
{
    free(scan->spans); free(scan->uniques); free(scan->keys.rows); free(scan->values.rows);
    for (size_t i = 0; i < scan->module_count; i++) { free(scan->modules[i].name); free(scan->modules[i].declaration); }
    free(scan->modules); memset(scan, 0, sizeof(*scan));
}
static char *ni_strdup_or_null(const char *text, int *failed)
{
    char *copy = text ? _strdup(text) : NULL;
    if (text && !copy) *failed = 1;
    return copy;
}
void sh_native_inline_generated_free(sh_native_inline_generated_set *set)
{
    if (!set) return;
    for (size_t i = 0; i < set->count; i++) {
        sh_native_inline_generated *row = set->rows + i;
        free(row->unique); free(row->module_index); free(row->module_name); free(row->inherit);
    }
    free(set->rows); memset(set, 0, sizeof(*set));
}
int sh_native_inline_generated_read(const char *json, size_t length, sh_native_inline_generated_set *out)
{
    ni_scan scan = {0};
    int result = -1, failed = 0;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!json) return 0;
    scan.json = json; scan.json_length = length;
    /* A document the native visitor rejects states no placed copy. */
    if (!sh_native_json_visit_values(json, length, 256, ni_visit, &scan) || scan.failed) { result = 0; goto done; }
    if (scan.count > 1) qsort(scan.spans, scan.count, sizeof(*scan.spans), ni_span_order);
    if (!(out->rows = calloc(scan.count ? scan.count : 1, sizeof(*out->rows)))) goto done;
    for (size_t i = 0; i < scan.count; i++) {
        const ni_span *span = scan.spans + i;
        sh_json_object definition = {0};
        ni_identity identity;
        sh_native_inline_generated *row = out->rows + out->count++;
        char inherit[512]; size_t inherit_length = 0;
        row->entity = span->entity;
        ni_identity_of(&scan, span->entity, &identity);
        row->unique = ni_strdup_or_null(identity.unique, &failed);
        row->module_index = ni_strdup_or_null(identity.module_index, &failed);
        row->module_name = ni_strdup_or_null(identity.module_name, &failed);
        if (sh_native_json_parse_object(json + span->offset, span->length, 64, &definition)) {
            const char *value = sh_json_object_get(&definition, "inherit");
            if (value && sh_native_json_decode_string(value, strlen(value), inherit, sizeof(inherit), &inherit_length) &&
                inherit_length < sizeof(inherit) && strlen(inherit) == inherit_length && inherit_length)
                row->inherit = ni_strdup_or_null(inherit, &failed);
        }
        sh_json_object_free(&definition);
        if (failed) goto done;
    }
    result = 1;
done:
    ni_scan_free(&scan);
    if (result != 1) sh_native_inline_generated_free(out);
    return result;
}
char *sh_native_inline_generated_name(const sh_native_inline_generated *row, const char *inherit)
{
    if (!row || !row->unique || !row->module_index || !row->module_name || !inherit || !*inherit) return NULL;
    size_t size = strlen(row->module_index) + strlen(row->module_name) + strlen(inherit) + strlen(row->unique) + 4;
    char *name = malloc(size);
    if (name) snprintf(name, size, "%s_%s/%s_%s", row->module_index, row->module_name, inherit, row->unique);
    return name;
}

void sh_native_inline_report_free(sh_native_inline_report *report)
{
    if (!report) return;
    for (size_t i = 0; i < report->count; i++) {
        sh_native_inline_site *site = report->sites + i;
        for (size_t j = 0; j < site->retargeted; j++) {
            free(site->retargets[j].owner); free(site->retargets[j].reference); free(site->retargets[j].name);
        }
        free(site->retargets); free(site->from); free(site->to); free(site->owner); free(site->reference);
    }
    for (size_t i = 0; i < report->substitution_count; i++) {
        free(report->substitutions[i].from); free(report->substitutions[i].to);
    }
    free(report->substitutions); free(report->sites); free(report->notes); memset(report, 0, sizeof(*report));
}

/* ---- map-local uses that follow a converted occurrence ---- */
typedef struct ni_follow {
    const char *owner, *reference;
    char *before, *after;
    size_t entity, offset, length, found;
} ni_follow;
typedef struct ni_follows { ni_follow *rows; size_t count; const char *json; int failed; } ni_follows;
static void ni_follows_free(ni_follows *follows)
{
    for (size_t i = 0; i < follows->count; i++) { free(follows->rows[i].before); free(follows->rows[i].after); }
    free(follows->rows); memset(follows, 0, sizeof(*follows));
}
static int ni_follow_order(const void *a, const void *b)
{
    const ni_follow *x = a, *y = b;
    return x->offset < y->offset ? -1 : x->offset > y->offset;
}
/* Each typed site a follow names holds one plain string in the output; it is
 * recorded when its decoded value is the occurrence's old copy name, or, for a
 * retargeted ancestry filter (before NULL), whatever spelling the binder bound
 * to the folded definition. */
static int ni_follow_visit(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    ni_follows *follows = context;
    const sh_json_path *root = path, *item = NULL;
    if (kind != SH_JSON_STRING || !path) return 1;
    while (root->parent) { item = root; root = root->parent; }
    if (!item || item->key || !root->key || root->key_length != 8 || memcmp(root->key, "entities", 8)) return 1;
    int wanted = 0;
    for (size_t i = 0; i < follows->count && !wanted; i++) wanted = follows->rows[i].entity == item->index;
    if (!wanted) return 1;
    sh_package_provenance_site site;
    if (!sh_map_binding_site_at(path, &site)) { follows->failed = 1; return 0; }
    for (size_t i = 0; i < follows->count; i++) {
        ni_follow *row = follows->rows + i;
        if (row->entity != item->index || strcmp(site.owner, row->owner) || strcmp(site.reference, row->reference)) continue;
        char *value = length < SIZE_MAX ? malloc(length + 1) : NULL;
        size_t decoded = 0;
        if (!value) { follows->failed = 1; break; }
        if (sh_native_json_decode_string(json, length, value, length + 1, &decoded) && strlen(value) == decoded &&
            (!row->before || ni_name_equal(value, row->before))) {
            row->found++; row->offset = (size_t)(json - follows->json); row->length = length;
        }
        free(value);
    }
    free((void *)site.owner); free((void *)site.reference);
    return !follows->failed;
}
/* The uses of every converted occurrence with follow sites, from the source's
 * map-local uses. 1 planned (possibly none), -1 failure. */
static int ni_follows_plan(const sh_native_inline_source *source, const ni_scan *scan,
    const sh_native_inline_report *sites, ni_follows *follows)
{
    for (size_t k = 0; k < sites->count; k++) {
        const sh_native_inline_site *site = sites->sites + k;
        if (site->owner) {
            /* A converted independent use names the native ancestor at its own site. */
            if (!site->converted) continue;
            int listed = 0;
            for (size_t i = 0; i < follows->count && !listed; i++)
                listed = follows->rows[i].entity == site->entity && !strcmp(follows->rows[i].owner, site->owner) &&
                    !strcmp(follows->rows[i].reference, site->reference);
            ni_follow *grown; char *b, *a;
            if (!listed) {
                grown = realloc(follows->rows, (follows->count + 1) * sizeof(*grown));
                b = _strdup(site->from); a = _strdup(site->to);
                if (!grown || !b || !a) { if (grown) follows->rows = grown; free(b); free(a); return -1; }
                follows->rows = grown;
                follows->rows[follows->count++] = (ni_follow){site->owner, site->reference, b, a, site->entity, 0, 0, 0};
            }
            /* ... and each exact-identity operand renamed with its group, once
             * however many members list it. */
            for (size_t r = 0; r < site->retargeted; r++) {
                const struct sh_native_inline_retarget *operand = site->retargets + r;
                unsigned long long entity = 0; char *end = NULL;
                if (strncmp(operand->owner, "entity/", 7) || operand->owner[7] < '0' || operand->owner[7] > '9' ||
                    (entity = strtoull(operand->owner + 7, &end, 10), !end || *end)) return -1;
                int listed = 0;
                for (size_t i = 0; i < follows->count && !listed; i++)
                    listed = follows->rows[i].entity == entity && !strcmp(follows->rows[i].owner, operand->owner) &&
                        !strcmp(follows->rows[i].reference, operand->reference);
                if (listed) continue;
                grown = realloc(follows->rows, (follows->count + 1) * sizeof(*grown));
                b = _strdup(operand->name); a = _strdup(site->to);
                if (!grown || !b || !a) { if (grown) follows->rows = grown; free(b); free(a); return -1; }
                follows->rows = grown;
                follows->rows[follows->count++] = (ni_follow){operand->owner, operand->reference, b, a, (size_t)entity, 0, 0, 0};
            }
            continue;
        }
        if (!site->converted || (!site->renamed && !site->retargeted)) continue;
        ni_identity identity;
        int failed = 0;
        size_t matched = 0;
        char *before = ni_copy_name(ni_identity_of(scan, site->entity, &identity), site->from, &failed);
        char *after = before ? ni_copy_name(&identity, site->to, &failed) : NULL;
        if (!before || !after) { free(before); free(after); return -1; }
        for (size_t i = 0; source->local_use; i++) {
            const char *owner = NULL, *reference = NULL, *name = NULL;
            int listed = source->local_use(source->context, i, &owner, &reference, &name);
            if (listed < 0) { free(before); free(after); return -1; }
            if (!listed) break;
            unsigned long long entity = 0; char *end = NULL;
            if (!name || !owner || !reference || !ni_name_equal(name, before)) continue;
            if (strncmp(owner, "entity/", 7) || owner[7] < '0' || owner[7] > '9' ||
                (entity = strtoull(owner + 7, &end, 10), !end || *end)) { free(before); free(after); return -1; }
            ni_follow *grown = realloc(follows->rows, (follows->count + 1) * sizeof(*grown));
            char *b = _strdup(before), *a = _strdup(after);
            if (!grown || !b || !a) { if (grown) follows->rows = grown; free(b); free(a); free(before); free(after); return -1; }
            follows->rows = grown;
            follows->rows[follows->count++] = (ni_follow){owner, reference, b, a, (size_t)entity, 0, 0, 0};
            matched++;
        }
        if (matched != site->renamed) { free(before); free(after); return -1; }
        /* A retargeted filter names this converted copy, exactly its former
         * match set (ni_sole_descendant). */
        for (size_t i = 0; i < site->retargeted; i++) {
            const struct sh_native_inline_retarget *retarget = site->retargets + i;
            unsigned long long entity = 0; char *end = NULL;
            if (strncmp(retarget->owner, "entity/", 7) || retarget->owner[7] < '0' || retarget->owner[7] > '9' ||
                (entity = strtoull(retarget->owner + 7, &end, 10), !end || *end)) { free(before); free(after); return -1; }
            ni_follow *grown = realloc(follows->rows, (follows->count + 1) * sizeof(*grown));
            char *a = _strdup(after);
            if (!grown || !a) { if (grown) follows->rows = grown; free(a); free(before); free(after); return -1; }
            follows->rows = grown;
            follows->rows[follows->count++] = (ni_follow){retarget->owner, retarget->reference, NULL, a, (size_t)entity, 0, 0, 0};
        }
        free(before); free(after);
    }
    return 1;
}
/* The output with every follow site rewritten, or NULL when a site does not
 * hold exactly one such string (the walk then failed) or on allocation failure. */
static char *ni_follows_apply(ni_follows *follows, const char *json, size_t length, size_t *output_length)
{
    ni_text out = {0};
    size_t at = 0;
    follows->json = json;
    if (!sh_native_json_visit_values(json, length, 256, ni_follow_visit, follows) || follows->failed) return NULL;
    for (size_t i = 0; i < follows->count; i++) if (follows->rows[i].found != 1) return NULL;
    if (follows->count > 1) qsort(follows->rows, follows->count, sizeof(*follows->rows), ni_follow_order);
    for (size_t i = 0; i < follows->count; i++) {
        const ni_follow *row = follows->rows + i;
        if (row->offset < at || !ni_append(&out, json + at, row->offset - at) || !ni_json_string(&out, row->after)) {
            free(out.data); return NULL;
        }
        at = row->offset + row->length;
    }
    if (!ni_append(&out, json + at, length - at)) { free(out.data); return NULL; }
    *output_length = out.length;
    return out.data;
}

/* ---- independent typed uses (R-293) ----
 * A typed map site other than an entity's own inherit can name a served
 * definition P itself. A traced construction (ni_consumers: a SnapMap
 * spawner's pair) creates a new instance of P by name, which needs P and no
 * placed occurrence, so such a use is decided at its own site. It names P's
 * first native ancestor S instead only when constructing S is constructing
 * P; a changed P stays independent and keeps its package.
 * Construction by name uses the named declaration's Parse result. Parse
 * inherits only the class and editor variables and prepares the root-first
 * merge of the chain's own state slices; poolCount, poolGranularity and
 * expandInheritance stay the named declaration's own (R-226 section 8). A
 * positive own poolCount selects generic spawn's pooled routes for that
 * definition (VK 0x140344f20 / GL 0x140344e60), and expandInheritance =
 * false limits the prepared text to the declaration's own state, so neither
 * P nor S may state them. The native evaluator then compares what a fresh
 * construction reads, {inherit P} against {inherit S}: class, instantiable
 * flag and prepared text (R-191). Identical prepared text also makes the
 * same GLOBAL reads (alias handles, render models) in the same order; reads
 * that differ per layer arise only where a pool applies the layers one by
 * one, which the own-pool rule and the world pool check below exclude.
 * The new instance's declaration (+0x6d0) becomes S. An ancestry consumer
 * naming P or a folded layer would stop selecting it, so every other use of
 * those names, at a map site or in a served declaration play instantiates,
 * must itself be a construction; one naming S or its ancestors selects both.
 * The world AI pool selects by the same ancestry. The rewrite changes no
 * entity's inherit and no site holding a placed copy's generated name, so
 * the play-copy names (%u_%s/%s_%u) are unchanged. Consumers outside the
 * traced rows that observe the constructed instance's declaration, or a name
 * derived from it (spawned names <declaration>_<n> were observed, R-240), are
 * not traced; as for placed conversion, they stay undecided coverage. */
static void ni_refusef(sh_native_inline_site *site, sh_decl_carrier_limit limit, const char *format, ...)
{
    va_list args;
    site->converted = 0; site->limit = limit; site->conflict = 0;
    va_start(args, format);
    vsnprintf(site->reason, sizeof(site->reason), format, args);
    va_end(args);
}
/* A declaration's own pool or expansion setting among its positional headers
 * (sh_decl_entity_split), or NULL. */
static const char *ni_own_setting(const sh_decl_node *tree)
{
    sh_decl_entity_headers headers;
    sh_decl_entity_split(tree, &headers);
    for (const sh_decl_node *member = tree->children; member && member != headers.state; member = member->next)
        if (member->key && (!strcmp(member->key, "poolCount") || !strcmp(member->key, "poolGranularity") ||
            !strcmp(member->key, "expandInheritance"))) return member->key;
    return NULL;
}
/* { inherit = "name" }, or NULL on allocation failure. */
static sh_decl_node *ni_inherit_tree(const char *name)
{
    ni_text quoted = {0};
    sh_decl_node *tree = ni_node(NULL, NULL, 1), *inherit = ni_node("inherit", NULL, 0);
    if (!tree || !inherit || !ni_append(&quoted, "\"", 1) || !ni_append_text(&quoted, name) || !ni_append(&quoted, "\"", 1)) {
        sh_decl_tree_free(tree); sh_decl_tree_free(inherit); free(quoted.data);
        return NULL;
    }
    inherit->value = quoted.data; tree->children = inherit;
    return tree;
}
/* 1 when the use's typed site holds name as one plain string in the document;
 * 0 otherwise (a map-local use holds its occurrence's generated name). */
static int ni_site_holds(const char *json, size_t length, const sh_native_inline_use *use, size_t entity, const char *name)
{
    ni_follow row = {use->owner, use->reference, (char *)name, NULL, entity, 0, 0, 0};
    ni_follows follows = {&row, 1, json, 0};
    if (!sh_native_json_visit_values(json, length, 256, ni_follow_visit, &follows) || follows.failed) return 0;
    return row.found == 1;
}
/* A map value at an exact-identity consumer naming the native target: the
 * consumer would start matching instances an independent use constructs. */
typedef struct ni_identity_scan {
    const sh_native_inline_source *source; const ni_scan *scan; const char *name;
    int failed; char reason[512];
} ni_identity_scan;
static int ni_identity_visit(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    ni_identity_scan *found = context;
    const sh_json_path *root = path, *item = NULL;
    const struct ni_consumer *row = NULL;
    if (kind != SH_JSON_STRING || !path || *found->reason) return 1;
    while (root->parent) { item = root; root = root->parent; }
    if (!item || item->key || !root->key || root->key_length != 8 || memcmp(root->key, "entities", 8)) return 1;
    sh_package_provenance_site site;
    if (!sh_map_binding_site_at(path, &site)) return 1;
    for (size_t c = 0; !row && c < sizeof(ni_consumers) / sizeof(*ni_consumers); c++)
        if (!ni_consumers[c].family && ni_consumer_identity(ni_consumers + c) &&
            ni_consumer_path(ni_consumers + c, site.reference))
            row = ni_consumers + c;
    char *value = row && length < SIZE_MAX ? malloc(length + 1) : NULL;
    size_t decoded = 0;
    if (row && !value) found->failed = 1;
    if (value && sh_native_json_decode_string(json, length, value, length + 1, &decoded) && strlen(value) == decoded &&
        ni_name_equal(value, found->name)) {
        char class_name[256];
        int known = ni_entity_class(found->source, found->scan, item->index, class_name, sizeof(class_name));
        if (known < 0) found->failed = 1;
        else if (!known || found->source->derives(found->source->context, class_name, row->owner_class) == 1)
            snprintf(found->reason, sizeof(found->reason), "%s %s is %s naming native '%s'%s; it matches only entities "
                "whose own declaration is exactly that definition, so instances this use constructs would start "
                "matching it under the native name", site.owner, site.reference, row->what, found->name,
                known ? "" : " (its class is not resolved)");
    }
    free(value); free((void *)site.owner); free((void *)site.reference);
    return !found->failed;
}
/* Definitions play constructs with no map site naming them, read as play
 * reads them (a served copy, else the native original). Stock code constructs
 * SnapMap's AI conductor by this literal name when the world has none (VK
 * 0x14030c9c0 / GL 0x14030c1d0, through the entityDef manager's by-name
 * lookup VK 0x1417b5250). After a map's entities spawn, a game with no
 * challenge entity creates its default one from the session
 * (CreateDefaultChallengeEntityFromSession VK 0x14030cb20 / GL 0x14030c330,
 * called from VK 0x140324bc8), by name through the entityDef lookup (VK
 * 0x1417b36f0 / GL 0x1417a61b0): SnapMap's game type 1 selects the coop
 * challenge. Its chain (mp/modes/pvp/game_challenge) sets exact-identity and
 * ancestry consumers over mp pickups, and its SP-loop drops replace a dead
 * AI's list while useSPLoopDropComponent is set (idAI2::Damage VK
 * 0x140624910). The R-295 stock index of exact-identity consumers covers
 * SnapMap's own definitions besides these two. */
static const char *const ni_code_constructed[] = {"ai/conductor/coop/snapmap_default", "mp/modes/coop/game_challenge"};
/* Walks name's chain for a use of a P layer or of S (base) at a consumer, as
 * a live served declaration's uses are decided: 1 refused (site reason set),
 * 0 none, -1 failure. */
static int ni_constructed_chain(const sh_native_inline_source *source, const char *name, const ni_layer *layers,
    size_t layer_count, const char *base, sh_native_inline_site *site)
{
    char *link = _strdup(name), error[256];
    int result = link ? 0 : -1;
    for (int depth = 0; !result && link && *link; depth++) {
        sh_decl_source text = {0}; int served = 0;
        int read = depth < 64 ? source->read(source->context, link, &text, &served, NULL) : -1;
        if (read <= 0) { free((char *)text.text); if (read < 0) result = -1; break; }
        sh_decl_node *tree = sh_decl_tree_parse_ordered(text, error, sizeof(error));
        free((char *)text.text);
        if (!tree) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "'%s', which stock code constructs by name, does not parse", link);
            result = 1; break;
        }
        sh_decl_entity_headers headers;
        sh_decl_entity_split(tree, &headers);
        ni_served_decl decl = {"entitydef", link, NULL, tree, headers.inherit, 1, 1, !served,
            "stock code constructs it by name for SnapMap play"};
        for (size_t j = 0; j <= layer_count && !result; j++) {
            ni_served_use use = {source, &decl, j < layer_count ? layers[j].name : base};
            char path[512] = "";
            use.independent = 1; use.identity_only = j == layer_count;
            ni_served_walk(&use, tree->children, headers.inherit, path, 0, sizeof(path));
            if (use.failed) result = -1;
            else if (*use.reason) { ni_refusef(site, SH_DECL_CARRIER_GAP, "%s", use.reason); result = 1; }
        }
        char *parent = result ? NULL : ni_parent(tree);
        sh_decl_tree_free(tree);
        free(link); link = parent;
    }
    free(link);
    return result;
}
/* ---- the loot counter (R-295) ----
 * Useable health and co-op backpack components add their owner's pending
 * pickups to idPlayer.lootspawns under the owner's own declaration (VK
 * 0x1408df7f0, 0x1408df740 / GL 0x1408dec80); objective components add one
 * (VK 0x1408dfe10). Nothing else adds to it. A loot drop's spawn amount adds
 * the count for its item definition exactly while loot_tracking is set, its
 * default (VK 0x14073bad0 reads idDeclLootDrop +0x88 and 0x140764720 an
 * idDroppableLootDef's +0x10 / GL 0x14073ad00, 0x1407639d0). A use that names
 * S instead of P moves its instances from P's count to S's, which only a drop
 * naming either observes. Both sides are read from effective sources: served
 * declarations, else the native originals, through their inheritance. */
static const char *const ni_counting_components[] = {"idUseableHealthComponent", "idUseableCoopBackpackComponent",
    "idUseableObjectiveComponent"};
/* The literal one edit/<key> member states in a declaration's own text: 1
 * copied, 0 not stated, 2 stated in another form or with differing values. */
static int ni_edit_literal(const sh_decl_node *tree, const char *key, char *value, size_t capacity)
{
    int found = 0;
    for (const sh_decl_node *edit = tree ? tree->children : NULL; edit; edit = edit->next) {
        if (!edit->key || strcmp(edit->key, "edit")) continue;
        for (const sh_decl_node *member = edit->children; member; member = member->next) {
            char text[512];
            if (!member->key || _stricmp(member->key, key)) continue;
            if (!ni_literal_name(member, text, sizeof(text)) || strlen(text) >= capacity ||
                (found && !ni_name_equal(text, value))) return 2;
            memcpy(value, text, strlen(text) + 1); found = 1;
        }
    }
    return found;
}
/* A declaration's parent: an entity definition's positional inherit header,
 * another family's top-level inherit member. */
static char *ni_decl_parent(const sh_decl_node *tree, const char *family)
{
    if (!family) return ni_parent(tree);
    for (const sh_decl_node *member = tree->children; member; member = member->next) {
        char text[512];
        if (member->key && !strcmp(member->key, "inherit") && ni_literal_name(member, text, sizeof(text)))
            return _strdup(text);
    }
    return NULL;
}
/* The edit/<key> literal of name's chain, nearest declaration first, read as
 * play reads it (family NULL: an entity definition): 1 found, 0 the chain
 * states none, 2 not established (reason set), 3 name does not read, -1
 * failure. */
static int ni_chain_literal(const sh_native_inline_source *source, const char *family, const char *name,
    const char *key, char *value, size_t capacity, char *reason, size_t reason_capacity)
{
    char *link = _strdup(name), error[256];
    int result = link ? 0 : -1;
    for (int depth = 0; !result && link && *link; depth++) {
        sh_decl_source text = {0}; int served = 0;
        if (depth == 64) {
            snprintf(reason, reason_capacity, "'%s' has no finite inheritance chain", name);
            result = 2; break;
        }
        int read = family ? (source->read_decl ? source->read_decl(source->context, family, link, &text, &served) : -1) :
            source->read(source->context, link, &text, &served, NULL);
        if (read <= 0) {
            free((char *)text.text);
            if (read < 0) result = -1;
            else if (!depth) result = 3;
            else { snprintf(reason, reason_capacity, "ancestor '%s' of '%s' is unavailable", link, name); result = 2; }
            break;
        }
        sh_decl_node *tree = ni_family_parse(family, text, error, sizeof(error));
        free((char *)text.text);
        if (!tree) { snprintf(reason, reason_capacity, "'%s' does not parse", link); result = 2; break; }
        int found = ni_edit_literal(tree, key, value, capacity);
        if (found == 2) {
            snprintf(reason, reason_capacity, "'%s' states %s in a form the product does not resolve", link, key);
            result = 2;
        } else if (found) result = 1;
        char *parent = result ? NULL : ni_decl_parent(tree, family);
        sh_decl_tree_free(tree);
        free(link); link = parent;
    }
    free(link);
    return result;
}
/* One reflected reference a typed walk reports (sh_decl_reference_site);
 * form 0 is a declaration pointer, 1 an event-call decl argument
 * (SH_DECL_EVENT_DECL_USE_OPS), 2 a by-name reference (SH_DECL_BY_NAME_DECL_OPS). */
typedef struct ni_typed_ref {
    char *name, *type, *owner, *member, *path; int form;
    char *family;   /* a native grammar's site (ni_grammar_site): its family, else NULL */
    int constructs; /* verified event declaration argument */
} ni_typed_ref;
typedef struct ni_ref_walk {
    const sh_decl_dependency_schema *inner;
    ni_typed_ref *refs; size_t count, capacity;
    size_t gaps; char gap[320];
    int failed;
    char klass[256];    /* an entity definition's class, for its consumer rows */
    const sh_native_inline_source *source;
} ni_ref_walk;
static int ni_event_decl_constructor(ni_ref_walk *w, sh_decl_value_type type, const sh_decl_reference_site *site)
{
    char *event;
    int constructs;
    if (!w->source || !w->source->event_decl_constructs || !site || !site->event_name || !site->event_name_length ||
        !site->event_argument_signature || !type.name || !type.ops || strcmp(type.ops, SH_DECL_EVENT_DECL_USE_OPS) ||
        site->event_name_length == SIZE_MAX) return 0;
    if (!(event = malloc(site->event_name_length + 1))) { w->failed = 1; return 0; }
    memcpy(event, site->event_name, site->event_name_length); event[site->event_name_length] = 0;
    constructs = strlen(event) == site->event_name_length &&
        w->source->event_decl_constructs(w->source->context, event, site->event_argument_index,
            site->event_argument_count, site->event_argument_signature, type.name) == 1;
    free(event); return constructs;
}
static int ni_rw_describe(void *c, sh_decl_value_type type, sh_decl_value_shape *shape)
{ const ni_ref_walk *w = c; return w->inner->describe(w->inner->context, type, shape); }
static int ni_rw_field(void *c, sh_decl_value_type object, const char *key, sh_decl_value_type *field)
{ const ni_ref_walk *w = c; return w->inner->field(w->inner->context, object, key, field); }
static int ni_rw_dynamic(void *c, sh_decl_value_type base, const char *name, size_t length, sh_decl_value_type *selected)
{ const ni_ref_walk *w = c; return w->inner->dynamic_type ? w->inner->dynamic_type(w->inner->context, base, name, length, selected) : 0; }
static int ni_rw_empty(void *c, sh_decl_value_type type)
{ const ni_ref_walk *w = c; return w->inner->empty_lookup ? w->inner->empty_lookup(w->inner->context, type) : 0; }
static int ni_rw_reference(void *c, const char *path, sh_decl_value_type type, const char *name, size_t length)
{ (void)c; (void)path; (void)type; (void)name; (void)length; return 1; }
static void ni_rw_gap(void *c, const char *path, sh_decl_value_type type, const char *reason)
{
    ni_ref_walk *w = c; (void)type;
    if (!w->gaps++) snprintf(w->gap, sizeof(w->gap), "%s (%s)", path ? path : "", reason ? reason : "unverified");
}
static char *ni_text_copy(const char *text, size_t length)
{
    char *copy = length < SIZE_MAX ? malloc(length + 1) : NULL;
    if (copy) { memcpy(copy, text, length); copy[length] = 0; }
    return copy;
}
static int ni_rw_site(void *c, const char *path, sh_decl_value_type type, const char *name, size_t length,
    const sh_decl_reference_site *site)
{
    ni_ref_walk *w = c;
    if (!length || !type.name) return 1;
    if (w->count == w->capacity) {
        size_t next = w->capacity ? w->capacity * 2 : 32;
        ni_typed_ref *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(w->refs, next * sizeof(*grown)) : NULL;
        if (!grown) { w->failed = 1; return 0; }
        w->refs = grown; w->capacity = next;
    }
    const char *owner = site ? site->owner_type : NULL, *member = site ? site->member : NULL;
    ni_typed_ref ref = {ni_text_copy(name, length), _strdup(type.name), owner ? _strdup(owner) : NULL,
        member ? _strdup(member) : NULL, _strdup(path ? path : ""),
        type.ops && !strcmp(type.ops, SH_DECL_EVENT_DECL_USE_OPS) ? 1 : type.ops && !strcmp(type.ops, SH_DECL_BY_NAME_DECL_OPS) ? 2 : 0};
    ref.constructs = ni_event_decl_constructor(w, type, site);
    if (!ref.name || !ref.type || !ref.path || (owner && !ref.owner) || (member && !ref.member)) {
        free(ref.name); free(ref.type); free(ref.owner); free(ref.member); free(ref.path);
        w->failed = 1; return 0;
    }
    w->refs[w->count++] = ref;
    return 1;
}
static void ni_rw_free(ni_ref_walk *w)
{
    for (size_t i = 0; i < w->count; i++) {
        free(w->refs[i].name); free(w->refs[i].type); free(w->refs[i].owner); free(w->refs[i].member);
        free(w->refs[i].path); free(w->refs[i].family);
    }
    free(w->refs); w->refs = NULL; w->count = w->capacity = 0;
}
/* Classify the exact argument site the native event reader selected. Raw
 * event-shaped objects and other occurrences of the same string do not count. */
typedef struct ni_event_target {
    ni_ref_walk base;
    const ni_scan *scan;
    const sh_native_inline_use *use;
    const sh_decl_node *node;
    const char *klass;
    size_t matches;
} ni_event_target;
static int ni_event_target_site(void *c, const char *path, sh_decl_value_type type, const char *name, size_t length,
    const sh_decl_reference_site *site)
{
    ni_event_target *w = c;
    (void)name; (void)length;
    (void)path;
    if (!site || !ni_event_decl_constructor(&w->base, type, site)) return !w->base.failed;
    if (w->node && site->node == w->node) w->matches++;
    if (w->use && site->json && site->json_length) {
        sh_map_binding_span span = {site->json, site->json_length};
        char error[128];
        if (!sh_map_binding_sites_locate(w->scan->json, w->scan->json_length, &span, 1, error, sizeof(error))) {
            w->base.failed = 1; return 0;
        }
        if (span.site.owner && span.site.reference && w->use->owner && w->use->reference &&
            !strcmp(span.site.owner, w->use->owner) && !strcmp(span.site.reference, w->use->reference)) w->matches++;
        sh_map_binding_sites_clear(&span, 1);
    }
    return 1;
}
static int ni_event_tree_constructs(const sh_native_inline_source *source, const sh_decl_node *tree, const char *klass,
    const sh_decl_node *node, int *failed)
{
    ni_event_target w = {{source->schema}};
    sh_decl_dependency_schema schema = {&w, ni_rw_describe, ni_rw_field, ni_rw_reference, ni_rw_gap, ni_rw_dynamic,
        ni_event_target_site, NULL, NULL, ni_rw_empty};
    sh_decl_dependency_result walked = {0};
    w.base.source = source; w.node = node;
    sh_decl_entity_dependencies(tree, (sh_decl_value_type){klass, ""}, &schema, &walked);
    if (w.base.failed) *failed = 1;
    return !w.base.failed && !walked.aborted && !walked.gaps && w.matches == 1;
}
static void ni_json_key(const sh_json_path *, char *, size_t);
static int ni_event_map_visit(void *c, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    ni_event_target *w = c;
    char key[128], parent[128], grand[128];
    const sh_json_path *definition, *element, *entities;
    sh_package_provenance_site where = {0};
    if (kind != SH_JSON_OBJECT || !path || !path->parent || !(definition = path->parent->parent) ||
        !(element = definition->parent) || !(entities = element->parent) || entities->parent ||
        element->key || !entities->key || entities->key_length != 8 || memcmp(entities->key, "entities", 8)) return 1;
    ni_json_key(path, key, sizeof(key)); ni_json_key(path->parent, parent, sizeof(parent));
    ni_json_key(definition, grand, sizeof(grand));
    if (strcmp(key, "edit") || strcmp(parent, "state") || strcmp(grand, "entityDef")) return 1;
    if (!sh_map_binding_site_at(path, &where)) { w->base.failed = 1; return 0; }
    int selected = where.owner && w->use->owner && !strcmp(where.owner, w->use->owner);
    free((void *)where.owner); free((void *)where.reference);
    if (selected) {
        sh_decl_dependency_schema schema = {w, ni_rw_describe, ni_rw_field, ni_rw_reference, ni_rw_gap, ni_rw_dynamic,
            ni_event_target_site, NULL, NULL, ni_rw_empty};
        sh_decl_dependency_result walked = {0};
        sh_decl_json_state_dependencies(json, length, (sh_decl_value_type){w->klass, ""}, &schema, &walked);
        if (walked.aborted || walked.gaps) w->matches = 0;
    }
    return !w->base.failed;
}
static int ni_event_map_constructs(const sh_native_inline_source *source, const ni_scan *scan,
    const sh_native_inline_use *use, const char *klass, int *failed)
{
    ni_event_target w = {{source->schema}, scan, use, NULL, klass};
    w.base.source = source;
    if (!sh_native_json_visit_values(scan->json, scan->json_length, 256, ni_event_map_visit, &w) || w.base.failed) {
        *failed = 1; return 0;
    }
    return w.matches == 1;
}

/* ---- holder substitution (R-295 segment 4) ---- */
/* The native name the plan gives a served entity definition, or NULL. */
static const char *ni_sigma_image(const ni_scan *scan, const char *name)
{
    for (size_t i = 0; scan && name && i < scan->sigma_count; i++)
        if (!scan->sigma[i].drop && ni_name_equal(scan->sigma[i].from, name)) return scan->sigma[i].to;
    return NULL;
}
/* The native loot drop the plan names in a served loot drop's place, or NULL. */
static const char *ni_drop_image(const ni_scan *scan, const char *name)
{
    for (size_t i = 0; scan && name && i < scan->sigma_count; i++)
        if (scan->sigma[i].drop && ni_name_equal(scan->sigma[i].from, name)) return scan->sigma[i].to;
    return NULL;
}
/* Queues each reference the declaration-pointer reader resolves (VK
 * 0x1409ca890 = GL 0x1409ca050: one token, FindByName in the pointee class's
 * list, the pointer stored and no name kept, R-287 section 3) whose family is
 * entitydef, or lootdrop, and whose name the plan maps in that family. A
 * by-name or event form, another family, an unresolved name and every untyped
 * literal stay as written, so a served name there still differs from the
 * native text. Nodes change only after the walk. */
typedef struct ni_sigma_walk {
    ni_ref_walk base;           /* first: the ni_rw_* adapters read it */
    const sh_native_inline_source *source; const ni_scan *scan;
    sh_decl_node **nodes; const char **images; size_t count, capacity;
    ni_marks *skip;             /* leaves kept as their own native JSON */
} ni_sigma_walk;
static int ni_sigma_site(void *c, const char *path, sh_decl_value_type type, const char *name, size_t length,
    const sh_decl_reference_site *site)
{
    ni_sigma_walk *w = c;
    char value[512]; const char *resolved = NULL;
    (void)path;
    if (!length || length >= sizeof(value) || !type.name || !site || !site->node) return 1;
    if (type.ops && (!strcmp(type.ops, SH_DECL_BY_NAME_DECL_OPS) ||
        (!strcmp(type.ops, SH_DECL_EVENT_DECL_USE_OPS) && !ni_event_decl_constructor(&w->base, type, site))))
        return !w->base.failed;
    memcpy(value, name, length); value[length] = 0;
    const char *entity = strlen(value) == length ? ni_sigma_image(w->scan, value) : NULL,
        *drop = strlen(value) == length ? ni_drop_image(w->scan, value) : NULL, *image = NULL;
    if ((!entity && !drop) || (w->skip && ni_mark_find(w->skip, site->node))) return 1;
    int found = w->source->resolve_decl ? w->source->resolve_decl(w->source->context, type.name, value, &resolved) : -1;
    if (found < 0) { w->base.failed = 1; return 0; }
    if (found != 1 || !resolved) return 1;
    image = !_stricmp(resolved, "entitydef") ? entity : !_stricmp(resolved, "lootdrop") ? drop : NULL;
    if (!image) return 1;
    if (w->count == w->capacity) {
        size_t next = w->capacity ? w->capacity * 2 : 8;
        sh_decl_node **nodes = next <= SIZE_MAX / sizeof(*nodes) ? realloc(w->nodes, next * sizeof(*nodes)) : NULL;
        if (nodes) w->nodes = nodes;
        const char **images = nodes ? realloc(w->images, next * sizeof(*images)) : NULL;
        if (!images) { w->base.failed = 1; return 0; }
        w->images = images; w->capacity = next;
    }
    w->nodes[w->count] = (sh_decl_node *)site->node; w->images[w->count++] = image;
    return 1;
}
/* Renames the plan's references in the state at node, under class_name: the
 * node's siblings too unless alone, and never a leaf skip marks. 1 walked
 * (*substituted counts the renamed references), 0 the walk did not complete
 * (nothing renamed), -1 failure. */
static int ni_sigma_rewrite(const sh_native_inline_source *source, const ni_scan *scan, sh_decl_node *node,
    const char *class_name, int alone, ni_marks *skip, size_t *substituted)
{
    ni_sigma_walk w = {{source->schema}, source, scan};
    w.base.source = source;
    w.skip = skip;
    sh_decl_dependency_schema schema = {&w, ni_rw_describe, ni_rw_field, ni_rw_reference, ni_rw_gap, ni_rw_dynamic,
        ni_sigma_site, NULL, NULL, ni_rw_empty};
    sh_decl_dependency_result walked = {0};
    sh_decl_node *next = node->next;
    int result = 1;
    *substituted = 0;
    if (alone) node->next = NULL;
    sh_decl_state_dependencies(node, (sh_decl_value_type){class_name, ""}, &schema, &walked);
    if (alone) node->next = next;
    if (w.base.failed) result = -1;
    else if (walked.aborted) result = 0;
    for (size_t i = 0; result == 1 && i < w.count; i++) {
        size_t length = strlen(w.images[i]) + 3;
        char *quoted = malloc(length);
        if (!quoted) { result = -1; break; }
        snprintf(quoted, length, "\"%s\"", w.images[i]);
        free(w.nodes[i]->value); w.nodes[i]->value = quoted; (*substituted)++;
    }
    free(w.nodes); free(w.images); ni_rw_free(&w.base);
    return result;
}
/* The evaluator's side of the substitution: the same renaming in one
 * prepared state text's edit block, which the class's struct reader reads,
 * every other token as written. A stock definition's text names no served
 * definition, so only the target's text changes; the evaluator's class,
 * instantiable, pool and expansion checks stay in force. */
static int ni_sigma_reduce(void *context, const char *state, size_t length, char **reduced, size_t *reduced_length,
    size_t *removed, char *reason, size_t capacity)
{
    const ni_sigma_view *view = context;
    size_t substituted = 0;
    int result = 1;
    (void)removed;
    *reduced = NULL; *reduced_length = 0;
    sh_decl_node *tree = ni_prepared_parse(state, length, reason, capacity);
    if (!tree) return 0;
    for (sh_decl_node *member = tree->children; member && result > 0; member = member->next) {
        if (!member->compound || !member->key || strcmp(member->key, "edit")) continue;
        int walked = ni_sigma_rewrite(view->source, view->scan, member, view->class_name, 1, NULL, &substituted);
        if (walked < 0) result = -1;
        else if (!walked) { snprintf(reason, capacity, "the typed walk of the prepared state did not complete"); result = 0; }
    }
    if (result > 0 && !(*reduced = sh_decl_tree_write(tree, reduced_length))) result = -1;
    sh_decl_tree_free(tree);
    return result;
}
/* The reflected class of a declaration family (SH_RESOURCE_TYPES), or NULL. */
static const char *ni_family_class(const char *family)
{
    for (size_t i = 0; i < sizeof(SH_RESOURCE_TYPES) / sizeof(*SH_RESOURCE_TYPES); i++)
        if (!_stricmp(SH_RESOURCE_TYPES[i].type, family)) return SH_RESOURCE_TYPES[i].class_name;
    return NULL;
}
/* Copies one node and its members (not its siblings); NULL on failure or
 * nesting deeper than 256 levels. */
static sh_decl_node *ni_node_clone(const sh_decl_node *node, int depth)
{
    sh_decl_node *copy = depth <= 256 ? calloc(1, sizeof(*copy)) : NULL;
    if (!copy) return NULL;
    copy->compound = node->compound; copy->reset = node->reset; copy->assignment = node->assignment;
    if ((node->key && !(copy->key = _strdup(node->key))) || (node->value && !(copy->value = _strdup(node->value))) ||
        (node->annotation && !(copy->annotation = _strdup(node->annotation)))) { sh_decl_tree_free(copy); return NULL; }
    sh_decl_node **tail = &copy->children;
    for (const sh_decl_node *child = node->children; child; child = child->next) {
        if (!(*tail = ni_node_clone(child, depth + 1))) { sh_decl_tree_free(copy); return NULL; }
        tail = &(*tail)->next;
    }
    return copy;
}
static int ni_node_overlay(sh_decl_node *into, const sh_decl_node *edit, int depth);
/* The member one statement makes of an object: a scalar's copy, or a block
 * whose own statements apply in order over an empty object, so a member the
 * block repeats keeps its last value as the native reader leaves it (a block
 * with unkeyed members is copied as written). NULL on failure. */
static sh_decl_node *ni_node_member(const sh_decl_node *statement, int depth)
{
    int keyed = 1;
    for (const sh_decl_node *child = statement->children; child && keyed; child = child->next) keyed = child->key != NULL;
    if (!statement->compound || !keyed) return ni_node_clone(statement, depth);
    sh_decl_node *member = depth <= 256 ? calloc(1, sizeof(*member)) : NULL;
    if (!member) return NULL;
    member->compound = 1; member->reset = statement->reset; member->assignment = statement->assignment;
    if ((statement->key && !(member->key = _strdup(statement->key))) ||
        (statement->value && !(member->value = _strdup(statement->value))) ||
        (statement->annotation && !(member->annotation = _strdup(statement->annotation))) ||
        !ni_node_overlay(member, statement, depth + 1)) { sh_decl_tree_free(member); return NULL; }
    return member;
}
/* Applies one layer's statements, in order, over the object its parent left:
 * a scalar or reset member replaces the member, a block overlays the member's
 * object, and num drops the list items at or past that count. 1 applied, 0
 * failure or nesting deeper than 256 levels. */
static int ni_node_overlay(sh_decl_node *into, const sh_decl_node *edit, int depth)
{
    if (depth > 256) return 0;
    for (const sh_decl_node *statement = edit->children; statement; statement = statement->next) {
        if (!statement->key) continue;
        sh_decl_node **at = &into->children;
        while (*at && (!(*at)->key || strcmp((*at)->key, statement->key))) at = &(*at)->next;
        /* A replaced member takes the statement's member in its own place (unlinking
         * it alone would leave *at on the next member, which is not this one). */
        if (*at && (statement->reset || !statement->compound || !(*at)->compound)) {
            sh_decl_node *old = *at, *copy = ni_node_member(statement, depth + 1);
            if (!copy) return 0;
            copy->next = old->next; old->next = NULL; *at = copy; sh_decl_tree_free(old);
        } else if (*at) { if (!ni_node_overlay(*at, statement, depth + 1)) return 0; }
        else {
            sh_decl_node **tail = &into->children;
            while (*tail) tail = &(*tail)->next;
            if (!(*tail = ni_node_member(statement, depth + 1))) return 0;
        }
        char *end = NULL;
        unsigned long long count = !statement->compound && !strcmp(statement->key, "num") && statement->value ?
            strtoull(statement->value, &end, 10) : 0;
        if (!end || *end || end == statement->value) continue;
        for (sh_decl_node **item = &into->children; *item;) {
            unsigned long long position = 0; char *close = NULL;
            int indexed = (*item)->key && !strncmp((*item)->key, "item[", 5) &&
                (position = strtoull((*item)->key + 5, &close, 10), close && !strcmp(close, "]"));
            if (indexed && position >= count) {
                sh_decl_node *old = *item; *item = old->next; old->next = NULL; sh_decl_tree_free(old);
            } else item = &(*item)->next;
        }
    }
    return 1;
}
/* A reflected declaration's effective state, as an owned "edit" node. The
 * generic typeinfo Parse (VK 0x1417bc4d0 / GL 0x1417aefe0) looks a leading
 * inherit up with makeDefault 0 (absent or empty: no parent), copies the found
 * parent's whole object, and then reads only the text after its own edit
 * keyword over that copy (docs/engine/aipoolnumbers-parse-copies-the-parent-
 * object-then-reads-its-own-text.md, aipoolnumbers-parse-reads-own-text-only-
 * after-an-edit-or-data-keyword.md). Layers therefore apply root first through
 * ni_node_overlay. served reports whether this compilation serves the named
 * declaration itself. 1 folded, 0 absent, 2 not established (reason), -1
 * failure. */
static int ni_family_state(const sh_native_inline_source *source, const char *family, const char *name,
    sh_decl_node **state, int *served, char *reason, size_t capacity)
{
    sh_decl_node **layers = NULL; char **names = NULL; size_t count = 0;
    char *current = _strdup(name), error[256];
    int result = current ? 1 : -1;
    *state = NULL;
    while (current && *current && result == 1) {
        for (size_t i = 0; i < count; i++)
            if (ni_name_equal(names[i], current)) {
                snprintf(reason, capacity, "%s '%s' inherits itself through '%s'", family, name, current); result = 2;
            }
        if (result != 1) break;
        sh_decl_source text = {0}; int layer_served = 0;
        int read = source->read_decl ? source->read_decl(source->context, family, current, &text, &layer_served) : -1;
        if (read < 0) {
            snprintf(reason, capacity, "%s '%s' is not readable", family, current); free((char *)text.text); result = 2; break;
        }
        if (!read) { free((char *)text.text); if (!count) result = 0; break; }
        if (!count && served) *served = layer_served;
        sh_decl_node *tree = ni_family_parse(family, text, error, sizeof(error));
        free((char *)text.text);
        if (!tree) { snprintf(reason, capacity, "%s '%s' does not parse: %s", family, current, error); result = 2; break; }
        if (!sh_decl_tree_member(tree, "edit") && sh_decl_tree_member(tree, "data")) {
            snprintf(reason, capacity, "%s '%s' states its body after data, which the walk does not model", family, current);
            sh_decl_tree_free(tree); result = 2; break;
        }
        sh_decl_node **grown_layers = realloc(layers, (count + 1) * sizeof(*layers));
        char **grown_names = grown_layers ? realloc(names, (count + 1) * sizeof(*names)) : NULL;
        if (grown_layers) layers = grown_layers;
        if (!grown_names) { sh_decl_tree_free(tree); result = -1; break; }
        names = grown_names; layers[count] = tree; names[count++] = current;
        current = ni_decl_parent(tree, family);
    }
    if (result == 1 && !(*state = calloc(1, sizeof(**state)))) result = -1;
    if (result == 1) {
        (*state)->compound = 1;
        if (!((*state)->key = _strdup("edit"))) result = -1;
    }
    for (size_t i = count; result == 1 && i-- > 0;) {
        const sh_decl_node *edit = sh_decl_tree_member(layers[i], "edit");
        if (edit && edit->compound && !ni_node_overlay(*state, edit, 0)) {
            snprintf(reason, capacity, "%s '%s' nests deeper than the walk models", family, names[i]); result = 2;
        }
    }
    if (result != 1) { sh_decl_tree_free(*state); *state = NULL; }
    for (size_t i = 0; i < count; i++) { sh_decl_tree_free(layers[i]); free(names[i]); }
    free(layers); free(names); free(current);
    return result;
}
/* The reflected references of one declaration's effective state: an entity
 * definition's expanded chain (sh_decl_entity_expanded_state: overlays, reset
 * markers, num truncation) under its class; another family's folded chain
 * (ni_family_state) under that family's class. served reports whether this
 * compilation serves the declaration. 1 walked, 0 absent, 2 not established
 * (reason), -1 failure. */
/* A declaration the native reads with its own grammar rather than as
 * reflected typeinfo (sh_decl_native_binding_family: materials, render
 * parameters, model definitions, tables, articulated figures, breakables,
 * animation webs, environments, cloth). Its references are the sites that
 * grammar's reader proves (sh_decl_native_reference_sites), each with its
 * family; the census follows them as unclassified edges, so what they lead to
 * is never asserted and never ignored. A text the reader refuses keeps the
 * census incomplete. Render parameters' kinds and tables, which the material
 * and environment grammars look up to classify a name, come from the
 * effective sources (ni_render_resolve). 1 walked, 0 absent, 2 not
 * established (reason), -1 failure. */
typedef struct ni_render_view { const sh_native_inline_source *source; int failed; } ni_render_view;
static int ni_render_resolve(void *context, const char *family, sh_decl_source name, int *kind)
{
    ni_render_view *view = context;
    sh_decl_source text = {0};
    char copy[512];
    int served = 0, read;
    *kind = 0;
    if (!strcmp(family, "image") || !strcmp(family, "renderprog") || !strcmp(family, "sampler")) return 1;
    if ((strcmp(family, "renderparm") && strcmp(family, "table")) || !name.text || name.length >= sizeof(copy)) return 0;
    memcpy(copy, name.text, name.length); copy[name.length] = 0;
    read = view->source->read_decl ? view->source->read_decl(view->source->context, family, copy, &text, &served) : -1;
    if (read > 0 && !strcmp(family, "renderparm") && !sh_decl_material_parameter_kind(text, kind, NULL, 0)) read = 0;
    free((char *)text.text);
    if (read < 0) view->failed = 1;
    return read < 0 ? -1 : read > 0;
}
typedef struct ni_grammar_walk { ni_ref_walk *w; int failed; } ni_grammar_walk;
static int ni_grammar_site(void *context, const char *family, const char *name, size_t offset, size_t length)
{
    ni_grammar_walk *g = context;
    ni_ref_walk *w = g->w;
    (void)offset; (void)length;
    /* A sampler names renderer state, not a resource (sg_material_references). */
    if (!family || !name || !*name || !strcmp(family, "sampler")) return 1;
    if (w->count == w->capacity) {
        size_t next = w->capacity ? w->capacity * 2 : 32;
        ni_typed_ref *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(w->refs, next * sizeof(*grown)) : NULL;
        if (!grown) { g->failed = 1; return 0; }
        w->refs = grown; w->capacity = next;
    }
    ni_typed_ref ref = {_strdup(name), _strdup(family), NULL, NULL, _strdup("its native text"), 0, _strdup(family)};
    if (!ref.name || !ref.type || !ref.path || !ref.family) {
        free(ref.name); free(ref.type); free(ref.path); free(ref.family);
        g->failed = 1; return 0;
    }
    w->refs[w->count++] = ref;
    return 1;
}
static int ni_grammar_refs(const sh_native_inline_source *source, const char *family, sh_decl_native_binding_format format,
    const char *name, ni_ref_walk *w, int *served_out, char *reason, size_t capacity)
{
    sh_decl_source text = {0};
    ni_render_view view = {source, 0};
    sh_decl_material_schema schema = {&view, ni_render_resolve};
    ni_grammar_walk walk = {w, 0};
    char error[256] = "";
    int served = 0, ok;
    int read = source->read_decl ? source->read_decl(source->context, family, name, &text, &served) : -1;
    if (read <= 0) { free((char *)text.text); return read; }
    if (served_out) *served_out = served;
    ok = sh_decl_native_reference_sites(format, text, &schema, ni_grammar_site, &walk, error, sizeof(error));
    free((char *)text.text);
    if (walk.failed || view.failed) return -1;
    if (!ok) { snprintf(reason, capacity, "%s '%s' does not read: %s", family, name, error); return 2; }
    return 1;
}
/* A render program and a skin read with their own declaration grammars, and
 * their sites are followed as unclassified edges. A render program
 * (sh_decl_render_program_sites) names its inherited program and its parms
 * block's render parameters. Its compiled program units are not read here.
 * The shader codec (sh_package_shader_decode) gives their references as render
 * parameters, images, samplers, render programs and program flags only, and
 * those families' own readers name only each other (over every shipped
 * declaration: m129 frontier-catalog-h2d), so no unit reference leads to an
 * entity or a loot count. A skin (sh_decl_skin_sites) names materials only;
 * one its reader finds no source for is the default material, which names
 * nothing. 1 walked, 0 absent, 2 not established (reason), -1 failure. */
static int ni_reader_refs(const sh_native_inline_source *source, const char *family, const char *name, ni_ref_walk *w,
    int *served_out, char *reason, size_t capacity)
{
    sh_decl_source text = {0};
    ni_render_view view = {source, 0};
    sh_decl_material_schema schema = {&view, ni_render_resolve};
    ni_grammar_walk walk = {w, 0};
    char error[256] = "";
    int served = 0, ok, skin = !_stricmp(family, "skins");
    int read = source->read_decl ? source->read_decl(source->context, skin ? "skins" : "renderprog", name, &text,
        &served) : -1;
    if (read <= 0) { free((char *)text.text); return read; }
    if (served_out) *served_out = served;
    ok = skin ? sh_decl_skin_sites(text, ni_grammar_site, &walk, error, sizeof(error)) :
        sh_decl_render_program_sites(text, &schema, ni_grammar_site, &walk, error, sizeof(error));
    free((char *)text.text);
    if (walk.failed || view.failed) return -1;
    if (!ok) { snprintf(reason, capacity, "%s '%s' does not read: %s", skin ? "skins" : "renderprog", name, error); return 2; }
    return 1;
}
/* A graph declaration (an idDeclTypeInfoGraph family read by the graph
 * envelope's own object reader: sh_decl_native_schema_graph_type) reads with
 * the native graph reader's records (sh_decl_graph_open) under their
 * reflected classes (sh_decl_graph_dependencies_with): its root object and
 * every subgraph, node and link object. Its inherited graph is a typed
 * reference the census follows as its own declaration, so its records are
 * walked there rather than expanded here. A record whose class is unavailable
 * or incompatible is a gap. 1 walked, 0 absent, 2 not established (reason),
 * -1 failure. */
static int ni_graph_refs(const sh_native_inline_source *source, const char *family, const char *name,
    const sh_decl_dependency_schema *schema, ni_ref_walk *w, int *served_out, char *reason, size_t capacity)
{
    sh_decl_source text = {0};
    char error[256] = "";
    int served = 0;
    int read = source->read_decl ? source->read_decl(source->context, family, name, &text, &served) : -1;
    if (read <= 0) { free((char *)text.text); return read; }
    if (served_out) *served_out = served;
    sh_decl_graph *graph = sh_decl_graph_open(text, error, sizeof(error));
    if (!graph) {
        free((char *)text.text);
        snprintf(reason, capacity, "%s '%s' does not read as a graph: %s", family, name, error); return 2;
    }
    sh_decl_dependency_result walked = {0};
    (void)sh_decl_graph_dependencies_with(graph, family, source->native_schema, schema, &walked);
    sh_decl_graph_close(graph); free((char *)text.text);
    if (w->failed) return -1;
    if (walked.aborted) { snprintf(reason, capacity, "the typed walk of %s '%s' did not complete", family, name); return 2; }
    return 1;
}
static int ni_typed_refs(const sh_native_inline_source *source, const char *family, const char *name,
    ni_ref_walk *w, int *served_out, char *reason, size_t capacity)
{
    w->source = source;
    sh_decl_native_binding_format grammar;
    if (!_stricmp(family, "renderprog") || !_stricmp(family, "skins"))
        return ni_reader_refs(source, family, name, w, served_out, reason, capacity);
    if (sh_decl_native_binding_family(family, &grammar))
        return ni_grammar_refs(source, family, grammar, name, w, served_out, reason, capacity);
    char error[256];
    sh_decl_source text = {0}; int served = 0, result = 1;
    int entity = !_stricmp(family, "entitydef");
    const char *class_name = entity ? NULL : ni_family_class(family);
    if (!entity && !class_name) {
        /* A resource a grammar site names (a mesh, an animation, a particle
         * file) has no declaration source: nothing to walk. A declaration
         * source of a family with no reflected class is not established. */
        int present = source->read_decl ? source->read_decl(source->context, family, name, &text, &served) : -1;
        free((char *)text.text);
        if (present <= 0) return present;
        snprintf(reason, capacity, "%s '%s' has no reflected class", family, name); return 2;
    }
    int read = entity ? source->read(source->context, name, &text, &served, NULL) : 1;
    if (read <= 0) { free((char *)text.text); return read; }
    if (served_out) *served_out = served;
    sh_decl_node *tree = NULL, *literal = NULL, *state = NULL;
    char *entity_class = NULL, header[600];
    sh_decl_dependency_schema schema = {w, ni_rw_describe, ni_rw_field, ni_rw_reference, ni_rw_gap, ni_rw_dynamic,
        ni_rw_site, NULL, NULL, ni_rw_empty};
    sh_decl_dependency_result walked = {0};
    if (entity) {
        free((char *)text.text);
        if (strchr(name, '"') || strchr(name, '\\') ||
            (size_t)snprintf(header, sizeof(header), "{ inherit = \"%s\"; edit = { } }", name) >= sizeof(header)) {
            snprintf(reason, capacity, "'%s' is not a name the walk can state", name); return 2;
        }
        ni_class_view view = {source};
        sh_decl_entity_class_source classes = {&view, ni_class_read, ni_class_derives};
        literal = sh_decl_tree_parse_ordered((sh_decl_source){header, strlen(header)}, error, sizeof(error));
        entity_class = literal ? sh_decl_entity_tree_class(literal, &classes, error, sizeof(error)) : NULL;
        state = entity_class ? sh_decl_entity_expanded_state(literal, &classes, error, sizeof(error)) : NULL;
        if (!state) { snprintf(reason, capacity, "the state of '%s' is not modelled: %s", name, error); result = 2; goto done; }
        snprintf(w->klass, sizeof(w->klass), "%s", entity_class);
        if (state->children)
            sh_decl_state_dependencies(state->children, (sh_decl_value_type){entity_class, ""}, &schema, &walked);
    } else {
        sh_decl_value_type state_type = {0};
        int graph = source->native_schema &&
            sh_decl_native_schema_decl_type(source->native_schema, family, &state_type) == 1 ?
            sh_decl_native_schema_graph_type(source->native_schema, state_type) : 0;
        if (graph < 0) {
            snprintf(reason, capacity, "whether %s '%s' is a graph declaration is not readable", family, name);
            result = 2; goto done;
        }
        if (graph) { result = ni_graph_refs(source, family, name, &schema, w, served_out, reason, capacity); goto done; }
        int folded = ni_family_state(source, family, name, &state, served_out, reason, capacity);
        if (folded != 1) { result = folded; goto done; }
        sh_decl_state_dependencies(state, (sh_decl_value_type){class_name, ""}, &schema, &walked);
    }
    if (w->failed) result = -1;
    else if (walked.aborted) { snprintf(reason, capacity, "the typed walk of '%s' did not complete", name); result = 2; }
done:
    sh_decl_tree_free(tree); sh_decl_tree_free(literal); sh_decl_tree_free(state); free(entity_class);
    return result;
}
/* Whether instances constructed under name add to idPlayer.lootspawns: 1
 * they do, or it is not established (reason says which), 0 they do not, -1
 * failure. Only a useable component registers: a declaration the entity's
 * effective state references whose componentTypeInfo, nearest layer first,
 * is one of ni_counting_components. */
static int ni_counts_pickups(const sh_native_inline_source *source, const char *name, char *reason, size_t capacity)
{
    ni_ref_walk w = {source->schema};
    int walked = ni_typed_refs(source, "entitydef", name, &w, NULL, reason, capacity), result = 0;
    if (walked < 0 || walked == 2) { ni_rw_free(&w); return walked < 0 ? -1 : 1; }
    for (size_t i = 0; walked == 1 && !result && i < w.count; i++) {
        const char *resolved = NULL; char family[64], type[512];
        int found = source->resolve_decl ? source->resolve_decl(source->context, w.refs[i].type, w.refs[i].name,
            &resolved) : -1;
        if (found < 0) {
            snprintf(reason, capacity, "the declaration '%s' its state names at %s is not resolved", w.refs[i].name,
                w.refs[i].path);
            result = 1; break;
        }
        if (!found || !resolved || !_stricmp(resolved, "entitydef") || strlen(resolved) >= sizeof(family)) continue;
        snprintf(family, sizeof(family), "%s", resolved);
        /* componentTypeInfo is a reflected member of a typeinfo declaration:
         * a family whose class is not one (a material, table or render
         * parameter, which derive idDecl only) states none. */
        const char *family_class = ni_family_class(family);
        if (family_class && source->derives(source->context, family_class, "idDeclTypeInfo") == 0) continue;
        int typed = ni_chain_literal(source, family, w.refs[i].name, "componentTypeInfo", type, sizeof(type),
            reason, capacity);
        if (typed < 0) { result = -1; break; }
        if (typed == 2) { result = 1; break; }
        if (typed != 1) continue;
        for (size_t c = 0; c < sizeof(ni_counting_components) / sizeof(*ni_counting_components); c++)
            if (!_stricmp(type, ni_counting_components[c]) ||
                source->derives(source->context, type, ni_counting_components[c]) == 1) {
                snprintf(reason, capacity, "its %s component '%s' at %s is an %s", family, w.refs[i].name,
                    w.refs[i].path, type);
                result = 1; break;
            }
    }
    if (!result && w.gaps) {
        snprintf(reason, capacity, "its state has a field the typed walk does not read: %s", w.gap);
        result = 1;
    }
    ni_rw_free(&w);
    return result;
}
/* The reflected members whose entity definition a drop computation reads the
 * pending count for exactly (R-295). */
static const struct ni_loot_observer { const char *owner_type, *member, *what; } ni_loot_observers[] = {
    {"idDeclLootDrop", "entityDef", "a loot drop's item (idDeclLootDrop +0x88, VK 0x14073bad0 / GL 0x14073ad00)"},
    {"idDroppableLootDef", "entityDef", "a droppable item (idDroppableLootDef +0x10, VK 0x140764720 / GL 0x1407639d0)"},
};
static const struct ni_loot_observer *ni_loot_row(const sh_native_inline_source *source, const ni_typed_ref *ref)
{
    for (size_t i = 0; ref->owner && ref->member && i < sizeof(ni_loot_observers) / sizeof(*ni_loot_observers); i++)
        if (!strcmp(ref->member, ni_loot_observers[i].member) && (!strcmp(ref->owner, ni_loot_observers[i].owner_type) ||
            source->derives(source->context, ref->owner, ni_loot_observers[i].owner_type) == 1))
            return ni_loot_observers + i;
    return NULL;
}
typedef enum ni_loot_context {
    NI_LOOT_LIVE = 1,       /* instances are constructed (a placed or spawned holder) */
    NI_LOOT_EVALUATED,      /* a live holder's drop computation runs this drop */
    NI_LOOT_UNSURE          /* reached only through a reference no rule classifies */
} ni_loot_context;
typedef struct ni_loot_node { char *family, *name, *via; ni_loot_context context; } ni_loot_node;
/* A count observer operand on the way: the definition it names, the site
 * naming it, whether play evaluates it and whether it is served text, and the
 * declaration (family, name) and path holding it. */
typedef struct ni_loot_seen { char *name, *text; int evaluated, served; char *family, *holder, *path; } ni_loot_seen;
/* A reference play follows to a loot drop: the drop, the declaration (family,
 * name) or placed entity ("entity", "entity/N") naming it, the path and the
 * context the drop is reached in. */
typedef struct ni_loot_ref { char *drop, *family, *holder, *path; ni_loot_context context; } ni_loot_ref;
/* The document's census (ni_scan.census): every declaration play reaches,
 * with its context, every count observer operand on the way and every
 * reference to a loot drop, built once from the map's placed entities, its
 * typed names and stock code's roots; the first reason it is incomplete. */
typedef struct ni_hint_record {
    size_t module, record;
    unsigned int kinds; /* each selector is its own shared uniqueness batch */
    int category;
    char *parent, *prepared;
    char **choices; size_t count;
} ni_hint_record;
typedef struct ni_hint_witness { char *family, *name, *text; } ni_hint_witness;
typedef struct ni_loot_closure {
    const sh_native_inline_source *source;
    ni_loot_node *nodes; size_t count, capacity;
    /* ni_loot_push's index over nodes: buckets of node indices (plus one) by
     * ni_loot_key, chained through next, so a push compares only nodes whose
     * family and name can be equal. Sized with nodes, rebuilt on growth. */
    size_t *buckets, *next, bucket_count;
    ni_loot_seen *seen; size_t seen_count, seen_capacity;
    ni_loot_ref *refs; size_t ref_count, ref_capacity;
    ni_hint_record *hints; size_t hint_count, hint_capacity;
    ni_hint_witness *hint_sources; size_t hint_source_count, hint_source_capacity;
    int built, failed, hint_incomplete;
    char incomplete[600];
    char failure[600];      /* why it could not be built, when a read failed (empty: allocation) */
    /* Every other distinct reason it is incomplete, one per line, while they
     * fit (diagnostic: the plan notes name them all at once). */
    char more[2400]; size_t incomplete_count;
    /* A coop challenge's SP-loop drops (ni_sp_literal): the census holds them
     * back unless their holder's own state enables them (sp_held: some were),
     * and is rebuilt with all of them (sp_loot) when a Gameplay Settings entity
     * play reaches may enable them (sp_settings says which). */
    int sp_loot, sp_held;
    char sp_settings[600];
} ni_loot_closure;
/* The census read for one use naming P (from) over native S (base): the first
 * evaluated observer of S and of P, and the first of either that play reaches
 * only through an unclassified reference. */
typedef struct ni_loot_query {
    int from_served;
    char base_seen[900], from_seen[900], unsure[900];
} ni_loot_query;
static int ni_loot_seen_add(ni_loot_closure *c, const char *name, const char *text, int evaluated, int served,
    const char *family, const char *holder, const char *path)
{
    if (c->seen_count == c->seen_capacity) {
        size_t next = c->seen_capacity ? c->seen_capacity * 2 : 32;
        ni_loot_seen *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(c->seen, next * sizeof(*grown)) : NULL;
        if (!grown) { c->failed = 1; return -1; }
        c->seen = grown; c->seen_capacity = next;
    }
    ni_loot_seen seen = {_strdup(name), _strdup(text), evaluated, served, _strdup(family), _strdup(holder), _strdup(path)};
    if (!seen.name || !seen.text || !seen.family || !seen.holder || !seen.path) {
        free(seen.name); free(seen.text); free(seen.family); free(seen.holder); free(seen.path);
        c->failed = 1; return -1;
    }
    c->seen[c->seen_count++] = seen;
    return 1;
}
static int ni_loot_ref_add(ni_loot_closure *c, const char *drop, const char *family, const char *holder,
    const char *path, ni_loot_context context)
{
    if (c->ref_count == c->ref_capacity) {
        size_t next = c->ref_capacity ? c->ref_capacity * 2 : 16;
        ni_loot_ref *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(c->refs, next * sizeof(*grown)) : NULL;
        if (!grown) { c->failed = 1; return -1; }
        c->refs = grown; c->ref_capacity = next;
    }
    ni_loot_ref ref = {_strdup(drop), _strdup(family), _strdup(holder), _strdup(path), context};
    if (!ref.drop || !ref.family || !ref.holder || !ref.path) {
        free(ref.drop); free(ref.family); free(ref.holder); free(ref.path); c->failed = 1; return -1;
    }
    c->refs[c->ref_count++] = ref;
    return 1;
}
static void ni_census_free(ni_loot_closure *c)
{
    for (size_t i = 0; i < c->count; i++) { free(c->nodes[i].family); free(c->nodes[i].name); free(c->nodes[i].via); }
    for (size_t i = 0; i < c->seen_count; i++) {
        free(c->seen[i].name); free(c->seen[i].text); free(c->seen[i].family); free(c->seen[i].holder);
        free(c->seen[i].path);
    }
    for (size_t i = 0; i < c->ref_count; i++) {
        free(c->refs[i].drop); free(c->refs[i].family); free(c->refs[i].holder); free(c->refs[i].path);
    }
    for (size_t i = 0; i < c->hint_count; i++) {
        ni_hint_record *r = c->hints + i;
        free(r->parent); free(r->prepared);
        for (size_t j = 0; j < r->count; j++) free(r->choices[j]);
        free(r->choices);
    }
    for (size_t i = 0; i < c->hint_source_count; i++) {
        free(c->hint_sources[i].family); free(c->hint_sources[i].name); free(c->hint_sources[i].text);
    }
    free(c->hints); free(c->hint_sources);
    free(c->buckets); free(c->next);
    free(c->nodes); free(c->seen); free(c->refs); memset(c, 0, sizeof(*c));
}
/* A hash equal for every family _stricmp finds equal and every name
 * ni_name_equal does: ASCII case folded, a family's non-ASCII bytes all one
 * (a locale may fold them), the name's '\' as '/', and one leading '/'
 * dropped unless it starts "//". */
static size_t ni_loot_key(const char *family, const char *name)
{
    size_t h = (size_t)14695981039346656037ull;
    for (const unsigned char *at = (const unsigned char *)family; *at; at++)
        h = (h ^ (*at >= 0x80 ? 0x80u : *at >= 'A' && *at <= 'Z' ? *at + 32u : *at)) * 1099511628211ull;
    h = (h ^ 0xffu) * 1099511628211ull;
    if (*name == '/' && name[1] != '/') name++;
    for (const char *at = name; *at; at++) h = (h ^ (unsigned char)ni_name_char(*at)) * 1099511628211ull;
    return h;
}
/* Work since the last reset (sh_native_inline_stats_take): documents, their
 * placed entities and sites, census builds, node pushes and the nodes those
 * pushes compared. Diagnostics only, on the thread doing the work. */
static sh_native_inline_stats g_ni_stats;
void sh_native_inline_stats_take(sh_native_inline_stats *out, int reset)
{
    if (out) *out = g_ni_stats;
    if (reset) memset(&g_ni_stats, 0, sizeof(g_ni_stats));
}
#ifdef SH_NATIVE_INLINE_TESTING
void sh_native_inline_test_census_counts(size_t *builds, size_t *pushes, size_t *compared, int reset)
{
    if (builds) *builds = g_ni_stats.census_builds;
    if (pushes) *pushes = g_ni_stats.census_pushes;
    if (compared) *compared = g_ni_stats.census_compared;
    if (reset) g_ni_stats.census_builds = g_ni_stats.census_pushes = g_ni_stats.census_compared = 0;
}
#endif
/* Room for one more node, with its index sized to it (load at most 1/2). */
static int ni_loot_reserve(ni_loot_closure *c)
{
    if (c->count < c->capacity) return 1;
    size_t capacity = c->capacity ? c->capacity * 2 : 64, buckets = capacity * 2;
    ni_loot_node *grown = capacity <= SIZE_MAX / sizeof(*grown) / 2 ? realloc(c->nodes, capacity * sizeof(*grown)) : NULL;
    if (!grown) return 0;
    c->nodes = grown;
    size_t *heads = calloc(buckets, sizeof(*heads)), *chain = malloc(capacity * sizeof(*chain));
    if (!heads || !chain) { free(heads); free(chain); return 0; }
    for (size_t i = 0; i < c->count; i++) {
        size_t b = ni_loot_key(c->nodes[i].family, c->nodes[i].name) & (buckets - 1);
        chain[i] = heads[b]; heads[b] = i + 1;
    }
    free(c->buckets); free(c->next);
    c->buckets = heads; c->next = chain; c->bucket_count = buckets; c->capacity = capacity;
    return 1;
}
/* Adds a declaration play reaches in a context: 1 added, 0 already, -1
 * failure. A constructed or evaluated visit subsumes an unclassified one. */
static int ni_loot_push(ni_loot_closure *c, const char *family, const char *name, const char *via,
    ni_loot_context context)
{
    /* Only nodes in the key's bucket can hold an equal family and name; the
     * test is the full comparison, so the index changes no decision. */
    size_t key = ni_loot_key(family, name);
    g_ni_stats.census_pushes++;
    for (size_t at = c->bucket_count ? c->buckets[key & (c->bucket_count - 1)] : 0; at; at = c->next[at - 1]) {
        const ni_loot_node *n = c->nodes + (at - 1);
        g_ni_stats.census_compared++;
        if (!_stricmp(n->family, family) && ni_name_equal(n->name, name) &&
            (n->context == context || (context == NI_LOOT_UNSURE && n->context != NI_LOOT_UNSURE)))
            return 0;
    }
    if (!ni_loot_reserve(c)) { c->failed = 1; return -1; }
    ni_loot_node node = {_strdup(family), _strdup(name), _strdup(via), context};
    if (!node.family || !node.name || !node.via) {
        free(node.family); free(node.name); free(node.via); c->failed = 1; return -1;
    }
    size_t b = key & (c->bucket_count - 1);
    c->next[c->count] = c->buckets[b]; c->buckets[b] = c->count + 1;
    c->nodes[c->count++] = node;
    return 1;
}
/* The consumer row tracing a reference inside an entity definition's state of
 * class klass, matched as the same site of a placed instance would be. */
static const struct ni_consumer *ni_loot_consumer(const sh_native_inline_source *source, const char *klass,
    const char *path)
{
    char reference[768]; size_t dots = 0, length = strlen(path);
    for (const char *at = path; *at; at++) dots += *at == '.';
    if (!*klass || length + 2 * dots + 24 >= sizeof(reference)) return NULL;
    char *out = reference + snprintf(reference, sizeof(reference), "/k:entityDef/k:state/k:");
    for (const char *at = path; *at; at++)
        if (*at == '.') { memcpy(out, "/k:", 3); out += 3; } else *out++ = *at;
    *out = 0;
    for (size_t c = 0; c < sizeof(ni_consumers) / sizeof(*ni_consumers); c++)
        if (!ni_consumers[c].family && ni_consumer_path(ni_consumers + c, reference) &&
            source->derives(source->context, klass, ni_consumers[c].owner_class) == 1)
            return ni_consumers + c;
    return NULL;
}
/* ---- the SP-loop drops (R-295 h2l) ----
 * A coop challenge's spDroppableItems (+0x8440) replace a dying AI's drops
 * only while its useSPLoopDropComponent (+0x8438) is set (idAI2::Damage VK
 * 0x140624910); otherwise only a copy and the destructor touch the list. The
 * challenge's constructor clears the flag; it comes from the challenge's own
 * state or from a Gameplay Settings entity, whose apply (vtable +0x8e8, VK
 * 0x1412802e0 / GL 0x141272250) copies its own useSPLootDroppbleItems (+0xc91,
 * cleared by its constructor VK 0x14166e000 / GL 0x1416603e0) into the game's
 * challenge. Either literal, stated as ni_edit_literal or ni_chain_literal
 * report it: 0 false or not stated (the constructor's false), 1 set or not
 * established. */
static int ni_sp_literal(int stated, const char *value)
{
    if (!stated) return 0;
    return stated != 1 || (_stricmp(value, "false") && strcmp(value, "0"));
}
/* Notes an entity of class klass play reaches (what names it) whose
 * useSPLootDroppbleItems (stated as ni_sp_literal reads it) may enable every
 * coop challenge's SP-loop drops: a Gameplay Settings entity, or one whose
 * class is not established. */
static void ni_sp_settings(ni_loot_closure *c, const char *klass, int stated, const char *value, const char *what)
{
    if (*c->sp_settings) return;
    int settings = *klass ? c->source->derives(c->source->context, klass, "idSnapMapGameEntity_GameplaySettings") : -1;
    if (!settings || (settings == 1 && !ni_sp_literal(stated, value))) return;
    if (settings < 0)
        snprintf(c->sp_settings, sizeof(c->sp_settings), "whether %s (class '%s') is a Gameplay Settings entity is not "
            "established", what, klass);
    else snprintf(c->sp_settings, sizeof(c->sp_settings), "%s sets useSPLootDroppbleItems%s", what,
        stated == 1 ? "" : " in a form the product does not resolve");
}
static void ni_loot_incomplete(ni_loot_closure *c, const char *text)
{
    if (!*c->incomplete) { snprintf(c->incomplete, sizeof(c->incomplete), "%s", text); c->incomplete_count = 1; return; }
    if (!strncmp(c->incomplete, text, sizeof(c->incomplete) - 1) || strstr(c->more, text)) return;
    c->incomplete_count++;
    size_t used = strlen(c->more), length = strlen(text);
    if (used + length + 2 < sizeof(c->more)) snprintf(c->more + used, sizeof(c->more) - used, "%s\n", text);
}
/* The five hint lists are built from loaded module declarations, not placed
 * idSnapItemHint map values. Module text selects an actual palette row by its
 * first lexical inherit occurrence. A fresh entityDef parses that same text;
 * its prepared parent key selects the bank's equal range. Keep native input
 * order as the sorting witness: equal-key permutations are never invented. */
typedef struct ni_hint_enum_value { const char *token; uint32_t value; int found; } ni_hint_enum_value;
static int ni_hint_enum_constant(void *context, const char *name, uint32_t value)
{
    ni_hint_enum_value *candidate = context;
    if (!strcmp(candidate->token, name)) { candidate->value = value; candidate->found = 1; }
    return 1;
}
/* Only fields the paired selection path actually consumes enter equality.
 * Generic editor declaration inheritance and repeated scalar assignments use
 * their effective last own value, then the nearest parent's value. Source
 * callbacks still read every reached ancestor in this same delivery view. */
static int ni_hint_projection(ni_loot_closure *c, const char *family, const char *name, char **out)
{
    static const char *const bank_keys[] = {"hintPickupsLarge", "hintPickupsMedium", "hintPickupsSmall", "hintPowerup", "hintWeapon"};
    static const char *const row_keys[] = {"entityDef", "moduleAutomation"};
    int bank = !strcmp(family, "snapeditorentitybank");
    const char *const *keys = bank ? bank_keys : row_keys; size_t count = bank ? 5 : 2;
    char *values[5] = {0}, **visited = NULL, *link = _strdup(name); size_t visited_count = 0;
    ni_text projection = {0}; int result = link ? 1 : -1; *out = NULL;
    while (result == 1 && link && *link) {
        for (size_t i = 0; i < visited_count; i++) if (ni_name_equal(visited[i], link)) { result = 0; break; }
        if (result != 1) break;
        if (visited_count == SIZE_MAX / sizeof(*visited)) { result = -1; break; }
        char **grown = realloc(visited, (visited_count + 1) * sizeof(*grown));
        if (!grown) { result = -1; break; }
        visited = grown; visited[visited_count++] = link; link = NULL;
        sh_decl_source text = {0}; int served = 0;
        int read = c->source->read_decl ? c->source->read_decl(c->source->context, family,
            visited[visited_count - 1], &text, &served) : -1;
        sh_decl_node *tree = read == 1 ? ni_family_parse(family, text, NULL, 0) : NULL;
        free((char *)text.text);
        if (!tree) { result = 0; break; }
        sh_decl_entity_headers headers; sh_decl_entity_split(tree, &headers);
        char *own[5] = {0};
        for (const sh_decl_node *edit = tree->children; edit && result == 1; edit = edit->next) {
            if (strcmp(edit->key, "edit")) continue;
            if (!edit->compound || edit->reset || edit->annotation) { result = 0; break; }
            for (const sh_decl_node *field = edit->children; field; field = field->next) {
                for (size_t k = 0; k < count; k++) if (!_stricmp(field->key, keys[k])) {
                    char *literal = !field->compound && !field->reset && !field->annotation ? ni_node_literal(field) : NULL;
                    if (!literal) { result = 0; break; }
                    if (!bank && k == 1 && field->value[0] != '"') {
                        char *end = NULL; errno = 0; long long number = strtoll(literal, &end, 10);
                        if (!errno && end != literal && !*end && number >= INT32_MIN && number <= INT32_MAX) {
                            char normalized[32]; snprintf(normalized, sizeof(normalized), "%d", (int32_t)number);
                            free(literal); literal = _strdup(normalized);
                        } else if (c->source->native_schema) {
                            ni_hint_enum_value value = {literal, 0, 0};
                            int enumerated = sh_decl_native_schema_enum_constants(c->source->native_schema,
                                "snapModuleAutomation_t", ni_hint_enum_constant, &value);
                            if (enumerated < 0) { free(literal); result = 0; break; }
                            if (value.found) {
                                char normalized[32]; snprintf(normalized, sizeof(normalized), "%d", (int32_t)value.value);
                                free(literal); literal = _strdup(normalized);
                            }
                        }
                    }
                    if (!literal) { result = -1; break; }
                    const char *tag = (!bank && k == 1) ? (field->value[0] == '"' ? "quoted:" : "enum:") :
                        (field->value[0] != '"' && !strcmp(literal, "NULL") ? "null:" : "resource:");
                    size_t tag_length = strlen(tag), literal_length = strlen(literal);
                    char *tagged = literal_length <= SIZE_MAX - tag_length - 1 ? malloc(tag_length + literal_length + 1) : NULL;
                    if (!tagged) { free(literal); result = -1; break; }
                    memcpy(tagged, tag, tag_length); memcpy(tagged + tag_length, literal, literal_length + 1); free(literal);
                    free(own[k]); own[k] = tagged;
                }
                if (result != 1) break;
            }
        }
        for (size_t k = 0; k < count; k++) {
            if (!values[k]) { values[k] = own[k]; own[k] = NULL; }
            free(own[k]);
        }
        if (result == 1 && headers.inherit) {
            link = ni_node_literal(headers.inherit);
            if (!link) result = 0;
        }
        sh_decl_tree_free(tree);
    }
    for (size_t k = 0; result == 1 && k < count; k++) {
        char prefix[96];
        int n = snprintf(prefix, sizeof(prefix), "%s=%s%zu:", keys[k], values[k] ? "" : "absent/", values[k] ? strlen(values[k]) : 0);
        if (!ni_append(&projection, prefix, (size_t)n) ||
            (values[k] && !ni_append(&projection, values[k], strlen(values[k]))) || !ni_append(&projection, ";", 1)) result = -1;
    }
    if (result == 1) { *out = projection.data; projection.data = NULL; }
    free(link); for (size_t i = 0; i < visited_count; i++) free(visited[i]); free(visited);
    for (size_t k = 0; k < count; k++) free(values[k]); free(projection.data); return result;
}
static int ni_hint_source(ni_loot_closure *c, const char *family, const char *name)
{
    for (size_t i = 0; i < c->hint_source_count; i++)
        if (!strcmp(c->hint_sources[i].family, family) && !strcmp(c->hint_sources[i].name, name)) return 1;
    char *projection = NULL; int read = ni_hint_projection(c, family, name, &projection);
    if (read != 1) { if (read < 0) c->failed = 1; return read; }
    if (c->hint_source_count == c->hint_source_capacity) {
        size_t next = c->hint_source_capacity ? c->hint_source_capacity * 2 : 16;
        ni_hint_witness *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(c->hint_sources, next * sizeof(*grown)) : NULL;
        if (!grown) { free(projection); c->failed = 1; return -1; }
        c->hint_sources = grown; c->hint_source_capacity = next;
    }
    ni_hint_witness witness = {_strdup(family), _strdup(name), projection};
    if (!witness.family || !witness.name) {
        free(witness.family); free(witness.name); free(witness.text); c->failed = 1; return -1;
    }
    c->hint_sources[c->hint_source_count++] = witness; return 1;
}
static void ni_hint_gap(ni_loot_closure *c, const char *module, size_t record, const char *detail)
{
    char reason[600];
    snprintf(reason, sizeof(reason), "module '%s' hint record %zu: %s", module ? module : "?", record, detail);
    c->hint_incomplete = 1; ni_loot_incomplete(c, reason);
}
static char *ni_hint_lexical(const char *text)
{
    const char *at = strstr(text, "inherit"), *first = at ? strchr(at, '"') : NULL;
    const char *last = first ? strchr(first + 1, '"') : NULL;
    if (!last) return _strdup(""); /* the native empty key still performs a lookup */
    size_t length = (size_t)(last - first - 1); char *key = malloc(length + 1);
    if (key) { memcpy(key, first + 1, length); key[length] = 0; }
    return key;
}
static int ni_hint_choice(ni_loot_closure *c, ni_hint_record *record, const char *name, const char *via)
{
    sh_decl_source text = {0}; int served = 0; char *replaces = NULL;
    int read = c->source->read(c->source->context, name, &text, &served, &replaces);
    free((char *)text.text); free(replaces);
    if (read < 0) return 0;
    if (!read) return 1; /* native makeDefault=0 filters an absent definition */
    if (record->count == SIZE_MAX / sizeof(*record->choices)) { c->failed = 1; return -1; }
    char **grown = realloc(record->choices, (record->count + 1) * sizeof(*grown));
    if (!grown) { c->failed = 1; return -1; }
    record->choices = grown;
    if (!(record->choices[record->count] = _strdup(name))) { c->failed = 1; return -1; }
    record->count++;
    return ni_loot_push(c, "entitydef", name, via, NI_LOOT_LIVE) < 0 ? -1 : 1;
}
static int ni_hint_record_add(ni_loot_closure *c, size_t module, size_t index, int category,
    unsigned int kinds, char *parent, sh_decl_node *state, const char *via)
{
    if (c->hint_count == c->hint_capacity) {
        size_t next = c->hint_capacity ? c->hint_capacity * 2 : 16;
        ni_hint_record *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(c->hints, next * sizeof(*grown)) : NULL;
        if (!grown) { c->failed = 1; return -1; }
        c->hints = grown; c->hint_capacity = next;
    }
    ni_hint_record *record = c->hints + c->hint_count++;
    memset(record, 0, sizeof(*record)); record->module = module; record->record = index;
    record->category = category; record->kinds = kinds; record->parent = _strdup(parent);
    /* The resulting hint record carries only position and pointer choices.
     * Other entity/edit metadata never reaches this consumer. The fresh native
     * constructor zeroes +288..290 before its position helper fills components. */
    ni_text position = {0};
    const sh_decl_node *pos = sh_decl_tree_member(sh_decl_tree_member(state, "edit"), "spawnPosition");
    static const char *const axes[] = {"x", "y", "z"};
    if (kinds && pos && (!pos->compound || pos->reset)) return 0;
    for (size_t k = 0; kinds && k < 3; k++) {
        const sh_decl_node *component = sh_decl_tree_member(pos, axes[k]);
        float value = 0; uint32_t bits = 0;
        if (component) {
            if (component->compound || component->reset || component->annotation || !component->value ||
                component->value[0] == '"') { free(position.data); return 0; }
            double numeric = 0; int parsed = ni_v_number(component->value, 0, &numeric);
            if (parsed != 1 || !_finite((float)numeric)) { free(position.data); return parsed < 0 ? -1 : 0; }
            value = (float)numeric;
        }
        memcpy(&bits, &value, sizeof(bits)); char formatted[16];
        int n = snprintf(formatted, sizeof(formatted), "%08x;", bits);
        if (!ni_append(&position, formatted, (size_t)n)) { free(position.data); c->failed = 1; return -1; }
    }
    record->prepared = position.data ? position.data : _strdup("");
    if (!record->parent || !record->prepared) { c->failed = 1; return -1; }
    if (!kinds) return 1; /* still witnesses the sort's complete key sequence */
    const sh_decl_node *list = sh_decl_tree_member(sh_decl_tree_member(state, "edit"), "spawnableEntities");
    if (!list) return 1; /* GetStringList finds no num and returns an empty list */
    const sh_decl_node *num = sh_decl_tree_member(list, "num"); size_t count = 0;
    if (!num) return 1;
    if (!list->compound || list->reset || !sh_decl_tree_list_count(num, &count)) return 0;
    for (size_t i = 0; i < count; i++) {
        char key[64]; snprintf(key, sizeof(key), "item[%zu]", i);
        const sh_decl_node *item = sh_decl_tree_member(list, key);
        char *name = item ? ni_node_literal(item) : _strdup("");
        if (!name || (item && (item->compound || item->reset))) { free(name); return 0; }
        /* Native GetStringList trims the returned idStr before FindByName. */
        size_t start = 0, end = strlen(name);
        while (start < end && (unsigned char)name[start] <= ' ') start++;
        while (end > start && (unsigned char)name[end - 1] <= ' ') end--;
        name[end] = 0;
        int result = ni_hint_choice(c, record, name + start, via); free(name);
        if (result != 1) return result;
    }
    return 1;
}
/* Snap-module idStr fields use escaped string tokens (retained native module
 * fixtures). Protect those tokens while the ordinary declaration grammar is
 * parsed; its entityDef lexer deliberately has different escape flags. The
 * original token is restored before any field is interpreted. */
static sh_decl_node *ni_hint_module_tree(sh_decl_source source, char *error, size_t capacity)
{
    ni_text projected = {0}; char **strings = NULL; size_t count = 0, allocated = 0, start = 0, at = 0;
    sh_decl_node *tree = NULL; sh_decl_node **stack = NULL; size_t used = 0, stack_capacity = 0;
    if (!source.text || memchr(source.text, 0, source.length)) goto done;
    while (at < source.length) {
        if (source.text[at] == '/' && at + 1 < source.length && source.text[at + 1] == '/') {
            at += 2; while (at < source.length && source.text[at] != '\n') at++; continue;
        }
        if (source.text[at] == '/' && at + 1 < source.length && source.text[at + 1] == '*') {
            at += 2;
            while (at + 1 < source.length && (source.text[at] != '*' || source.text[at + 1] != '/')) at++;
            if (at + 1 == source.length) goto done;
            at += 2; continue;
        }
        if (source.text[at] != '"') { at++; continue; }
        size_t first = at++;
        while (at < source.length && source.text[at] != '"') {
            if (source.text[at++] == '\\') { if (at == source.length) goto done; at++; }
        }
        if (at == source.length) goto done;
        at++;
        if (count == allocated) {
            size_t next = allocated ? allocated * 2 : 32;
            char **grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(strings, next * sizeof(*grown)) : NULL;
            if (!grown) goto done;
            strings = grown; allocated = next;
        }
        char *token = malloc(at - first + 1);
        if (!token) goto done;
        memcpy(token, source.text + first, at - first); token[at - first] = 0; strings[count] = token;
        char placeholder[80]; int n = snprintf(placeholder, sizeof(placeholder), "\"smpModuleString%zu\"", count++);
        if (!ni_append(&projected, source.text + start, first - start) || !ni_append(&projected, placeholder, (size_t)n)) goto done;
        start = at;
    }
    if (!ni_append(&projected, source.text + start, source.length - start) ||
        !(tree = sh_decl_tree_parse_ordered((sh_decl_source){projected.data, projected.length}, error, capacity))) goto done;
    stack_capacity = 32; stack = malloc(stack_capacity * sizeof(*stack)); if (!stack) goto invalid;
    stack[used++] = tree;
    while (used) {
        sh_decl_node *node = stack[--used];
        if (node->key && !strncmp(node->key, "smpModuleString", 15)) {
            char *end = NULL; unsigned long long index = strtoull(node->key + 15, &end, 10);
            if (!end || *end || index >= count) goto invalid;
            size_t length = strlen(strings[(size_t)index]), decoded = 0;
            char *key = malloc(length + 1);
            if (!key || !sh_native_json_decode_string(strings[(size_t)index], length, key, length + 1, &decoded) ||
                strlen(key) != decoded) { free(key); goto invalid; }
            free(node->key); node->key = key;
        }
        if (node->value && !strncmp(node->value, "\"smpModuleString", 16)) {
            char *end = NULL; unsigned long long index = strtoull(node->value + 16, &end, 10);
            if (!end || strcmp(end, "\"") || index >= count) goto invalid;
            char *value = _strdup(strings[(size_t)index]); if (!value) goto invalid;
            free(node->value); node->value = value;
        }
        for (sh_decl_node *child = node->children; child; child = child->next) {
            if (used == stack_capacity) {
                size_t next = stack_capacity * 2;
                sh_decl_node **grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(stack, next * sizeof(*grown)) : NULL;
                if (!grown) goto invalid;
                stack = grown; stack_capacity = next;
            }
            stack[used++] = child;
        }
    }
    goto done;
invalid:
    sh_decl_tree_free(tree); tree = NULL;
done:
    for (size_t i = 0; i < count; i++) free(strings[i]);
    free(strings); free(projected.data); free(stack); return tree;
}
/* Project only the entity text storage consumed by the native module record
 * constructor. Repeated object/list/item assignments apply in source order;
 * each list scans its first num before overlaying any items. Shrink/grow must
 * discard retired text instead of merging a stale item back into the list. */
static int ni_hint_entities(const sh_decl_node *root, const sh_decl_node ***out, size_t *count)
{
    const sh_decl_node **texts = NULL; size_t size = 0;
    *out = NULL; *count = 0;
    for (const sh_decl_node *edit = root->children; edit; edit = edit->next) {
        if (strcmp(edit->key, "edit")) continue;
        if (!edit->compound || edit->reset || edit->annotation) goto unsupported;
        for (const sh_decl_node *module = edit->children; module; module = module->next) {
            if (strcmp(module->key, "module")) continue;
            if (!module->compound || module->reset || module->annotation) goto unsupported;
            for (const sh_decl_node *list = module->children; list; list = list->next) {
                if (strcmp(list->key, "entities")) continue;
                if (!list->compound || list->reset || list->annotation) goto unsupported;
                const sh_decl_node *num = sh_decl_tree_member(list, "num"); size_t next = size;
                if (num && !sh_decl_tree_list_count(num, &next)) goto unsupported;
                if (next != size) {
                    if (next > SIZE_MAX / sizeof(*texts)) goto unsupported;
                    if (!next) { free(texts); texts = NULL; }
                    else {
                        const sh_decl_node **grown = realloc(texts, next * sizeof(*grown));
                        if (!grown) { free(texts); return -1; }
                        texts = grown;
                        if (next > size) memset(texts + size, 0, (next - size) * sizeof(*texts));
                    }
                    size = next;
                }
                for (const sh_decl_node *item = list->children; item; item = item->next) {
                    if (!strcmp(item->key, "num")) continue;
                    char *end = NULL; unsigned long long index;
                    if (strncmp(item->key, "item[", 5)) continue; /* native array warns/skips an unknown field */
                    index = strtoull(item->key + 5, &end, 10);
                    if (end == item->key + 5 || !end || strcmp(end, "]")) goto unsupported;
                    if (index >= size) continue;
                    if (!item->compound || item->annotation) goto unsupported;
                    for (const sh_decl_node *field = item->children; field; field = field->next)
                        if (!strcmp(field->key, "text")) {
                            if (field->compound || field->reset || field->annotation || !field->value) goto unsupported;
                            texts[(size_t)index] = field;
                        }
                }
            }
        }
    }
    *out = texts; *count = size; return 1;
unsupported:
    free(texts); return 0;
}
static void ni_hint_module(ni_loot_closure *c, const ni_module *module, const sh_native_inline_hint_bank *bank)
{
    sh_decl_source text = {0}; int served = 0; char error[256] = "";
    sh_decl_node *root = NULL; const sh_decl_node **texts = NULL;
    if (!module->declaration || !c->source->read_asset ||
        c->source->read_asset(c->source->context, module->declaration, &text, &served) != 1 ||
        !(root = ni_hint_module_tree(text, error, sizeof(error)))) {
        ni_hint_gap(c, module->declaration, 0, "the actual loaded module source is unavailable"); goto done;
    }
    size_t count = 0; int projected = ni_hint_entities(root, &texts, &count);
    if (projected != 1) {
        if (projected < 0) c->failed = 1;
        ni_hint_gap(c, module->declaration, 0, "the native entity list is not complete"); goto done;
    }
    for (size_t i = 0; i < count && !c->failed; i++) {
        char via[700]; const sh_decl_node *value = texts[i];
        const char *encoded = value ? value->value : "\"\"";
        char *decoded = malloc(strlen(encoded) + 1);
        char *lookup = NULL, *parent = NULL, *klass = NULL; sh_decl_node *literal = NULL, *state = NULL;
        const sh_native_inline_hint_selector *row = NULL; size_t length = 0;
        if (!decoded || !sh_native_json_decode_string(encoded, strlen(encoded), decoded,
            strlen(encoded) + 1, &length) || strlen(decoded) != length || !(lookup = ni_hint_lexical(decoded))) {
            ni_hint_gap(c, module->declaration, i, "the native source text or lexical key is unavailable"); goto record_done;
        }
        int found = c->source->hint_row ? c->source->hint_row(c->source->context, lookup, &row) : -1;
        if (found < 0 || (found && (!row || !row->row || !row->entity))) {
            ni_hint_gap(c, module->declaration, i, "the actual editor palette selection is unavailable"); goto record_done;
        }
        if (!found) goto record_done; /* native miss keeps only an unclassified raw module record */
        if (ni_hint_source(c, "snapeditorentitydef", row->row) != 1) {
            ni_hint_gap(c, module->declaration, i, "the selected editor row source is unavailable"); goto record_done;
        }
        if (row->category < 0 || row->category > 3) goto record_done;
        literal = sh_decl_tree_parse_ordered((sh_decl_source){decoded, length}, error, sizeof(error));
        parent = literal ? ni_parent(literal) : NULL;
        ni_class_view view = {c->source}; sh_decl_entity_class_source classes = {&view, ni_class_read, ni_class_derives};
        klass = literal ? sh_decl_entity_tree_class(literal, &classes, error, sizeof(error)) : NULL;
        state = klass ? sh_decl_entity_expanded_state(literal, &classes, error, sizeof(error)) : NULL;
        if (!parent || !state) {
            ni_hint_gap(c, module->declaration, i, "the fresh record declaration's prepared state is unavailable");
            goto record_done;
        }
        unsigned int kinds = 0;
        for (size_t k = 0; k < 5; k++) {
            const sh_native_inline_hint_selector *selector = bank->selectors + k;
            int category = selector->category;
            int bucket = category == 0 ? 0 : category == 1 ? 1 : (category == 2 || category == 3) ? 3 : -1;
            if (row->category == bucket && !strcmp(parent, selector->entity)) kinds |= 1u << k;
        }
        snprintf(via, sizeof(via), "module '%s' record %zu is in hint batch mask %u", module->declaration, i, kinds);
        int added = ni_hint_record_add(c, module->instance, i, row->category, kinds, parent, state, via);
        if (added < 0) c->failed = 1;
        if (!added) ni_hint_gap(c, module->declaration, i, "the selected hint position or choice list is not modelled");
        if (c->source->hint_row(c->source->context, lookup, &row) != 1)
            ni_hint_gap(c, module->declaration, i, "the editor row changed while reading the record");
record_done:
        free(decoded); free(lookup); free(parent); free(klass); sh_decl_tree_free(literal); sh_decl_tree_free(state);
    }
done:
    free(texts); sh_decl_tree_free(root); free((char *)text.text);
}
static void ni_hint_census(ni_loot_closure *c, const ni_scan *scan)
{
    if (!scan->module_count) return;
    const sh_native_inline_hint_bank *bank = NULL;
    if (!c->source->hint_bank || c->source->hint_bank(c->source->context, &bank) != 1 || !bank || !bank->name) {
        c->hint_incomplete = 1; ni_loot_incomplete(c, "the actual loaded module hint bank is unavailable"); return;
    }
    if (ni_hint_source(c, "snapeditorentitybank", bank->name) != 1) {
        c->hint_incomplete = 1; ni_loot_incomplete(c, "the actual module hint bank's source is unavailable"); return;
    }
    for (size_t k = 0; k < 5; k++)
        if (!bank->selectors[k].row || !bank->selectors[k].entity ||
            ni_hint_source(c, "snapeditorentitydef", bank->selectors[k].row) != 1) {
            c->hint_incomplete = 1; ni_loot_incomplete(c, "an actual module hint selector's source is unavailable"); return;
        }
    for (size_t m = 0; m < scan->module_count && !c->failed; m++) ni_hint_module(c, scan->modules + m, bank);
    if (c->source->hint_bank(c->source->context, &bank) != 1) {
        c->hint_incomplete = 1; ni_loot_incomplete(c, "the loaded module hint cohort changed during saving");
    }
}
/* Walks one reached declaration's effective state. Transfer rules: a live
 * entity's reflected idDeclLootDrop pointer is a drop its loot component
 * evaluates (idLootDropComponent::DropLoot, VK 0x14073c6f0 / GL 0x14073b920),
 * while a drop an event-call argument or a by-name reference names is not
 * evaluated on that evidence; an evaluated drop's item and a live holder's droppable item are
 * observers (each reads the pending count of exactly that definition) and
 * constructions of that item; an entity reference a CONSTRUCTS consumer row
 * traces constructs its target, and one an identity or ancestry row traces
 * only tests it; every other reference keeps the context unclassified, so
 * what it leads to is never asserted and never ignored. */
static void ni_loot_edges(ni_loot_closure *c, const char *family, const char *name, const char *from_via,
    ni_loot_context context, int entity, int served, const ni_ref_walk *walk, int sp_own)
{
    const sh_native_inline_source *source = c->source;
    const ni_ref_walk w = *walk;
    char reason[600], via[900];
    if (w.gaps) {
        snprintf(reason, sizeof(reason), "%s '%s' has a field the typed walk does not read: %s", family, name, w.gap);
        ni_loot_incomplete(c, reason);
    }
    for (size_t i = 0; i < w.count && !c->failed; i++) {
        const ni_typed_ref *ref = w.refs + i;
        const char *resolved = ref->family; char target[64];
        /* A coop challenge's SP-loop drops (ni_sp_literal) while nothing sets
         * its flag: its own state (sp_own: a placed holder's, else read here
         * from the declaration's chain) or a Gameplay Settings entity. */
        if (!c->sp_loot && entity && *w.klass && ref->path && !_strnicmp(ref->path, "edit.spDroppableItems.", 22) &&
            source->derives(source->context, w.klass, "idGameChallenge_Coop") == 1) {
            if (sp_own < 0) {
                char value[64];
                int stated = ni_chain_literal(source, NULL, name, "useSPLoopDropComponent", value, sizeof(value), reason,
                    sizeof(reason));
                if (stated < 0) { c->failed = 1; break; }
                sp_own = ni_sp_literal(stated, value);
            }
            if (!sp_own) { c->sp_held = 1; continue; }
        }
        /* A grammar site names its family itself; a reflected pointer is
         * resolved as its reader resolves it. */
        int found = ref->family ? 1 : source->resolve_decl ? source->resolve_decl(source->context, ref->type, ref->name,
            &resolved) : -1;
        if (found < 0 || (found && (!resolved || strlen(resolved) >= sizeof(target)))) {
            snprintf(reason, sizeof(reason), "the declaration '%s' that %s '%s' names at %s is not resolved", ref->name,
                family, name, ref->path);
            ni_loot_incomplete(c, reason); continue;
        }
        if (!found) continue;   /* the reader finds nothing: no declaration */
        snprintf(target, sizeof(target), "%s", resolved);
        const struct ni_loot_observer *row = !_stricmp(target, "entitydef") ? ni_loot_row(source, ref) : NULL;
        int evaluated = row && (entity ? context == NI_LOOT_LIVE : context == NI_LOOT_EVALUATED);
        if (row) {
            char text[900];
            snprintf(text, sizeof(text), "%s '%s' (%s) names '%s' at %s as %s, which reads idPlayer.lootspawns' "
                "count for that definition exactly", family, name, from_via, ref->name, ref->path, row->what);
            if (ni_loot_seen_add(c, ref->name, text, evaluated, served, family, name, ref->path) < 0) break;
        }
        const struct ni_consumer *traced = !row && entity && !_stricmp(target, "entitydef") ?
            (ref->constructs ? &ni_event_constructor : ni_loot_consumer(source, w.klass, ref->path)) : NULL;
        if (traced && !ni_consumer_constructs(traced)) continue;
        ni_loot_context next = row ? (evaluated ? NI_LOOT_LIVE : NI_LOOT_UNSURE) :
            traced ? (context == NI_LOOT_LIVE ? NI_LOOT_LIVE : NI_LOOT_UNSURE) :
            !_stricmp(target, "lootdrop") && !ref->form && entity && context == NI_LOOT_LIVE ? NI_LOOT_EVALUATED :
            NI_LOOT_UNSURE;
        /* An unclassified chain keeps naming the edge that left the classified rules. */
        if (next == NI_LOOT_UNSURE && context == NI_LOOT_UNSURE) snprintf(via, sizeof(via), "%s", from_via);
        else snprintf(via, sizeof(via), "'%s' names it at %s", name, ref->path);
        if (!_stricmp(target, "lootdrop") && ni_loot_ref_add(c, ref->name, family, name, ref->path, next) < 0) break;
        if (ni_loot_push(c, target, ref->name, via, next) < 0) break;
    }
}
static void ni_loot_visit(ni_loot_closure *c, size_t index)
{
    char reason[600], family[64], name[512], from_via[900];
    if (strlen(c->nodes[index].family) >= sizeof(family) || strlen(c->nodes[index].name) >= sizeof(name)) {
        ni_loot_incomplete(c, "a reached declaration name is too long"); return;
    }
    snprintf(family, sizeof(family), "%s", c->nodes[index].family);
    snprintf(name, sizeof(name), "%s", c->nodes[index].name);
    snprintf(from_via, sizeof(from_via), "%s", c->nodes[index].via);
    ni_loot_context context = c->nodes[index].context;
    int served = 0;
    ni_ref_walk w = {c->source->schema};
    int walked = ni_typed_refs(c->source, family, name, &w, &served, reason, sizeof(reason));
    if (walked < 0) {
        c->failed = 1;
        if (!*c->failure) snprintf(c->failure, sizeof(c->failure), "the typed references of %s '%s' could not be read",
            family, name);
    }
    else if (walked == 2) ni_loot_incomplete(c, reason);
    else if (walked) {
        int entity = !_stricmp(family, "entitydef");
        if (entity && !*c->sp_settings && (!*w.klass ||
            c->source->derives(c->source->context, w.klass, "idSnapMapGameEntity_GameplaySettings") != 0)) {
            char value[64], what[600];
            int stated = ni_chain_literal(c->source, NULL, name, "useSPLootDroppbleItems", value, sizeof(value), reason,
                sizeof(reason));
            snprintf(what, sizeof(what), "'%s' (%.400s)", name, from_via);
            if (stated < 0) c->failed = 1;
            else ni_sp_settings(c, w.klass, stated, value, what);
        }
        if (!c->failed) ni_loot_edges(c, family, name, from_via, context, entity, served, &w, -1);
    }
    ni_rw_free(&w);
}
/* Walks one placed entity as play constructs it: its inherit chain expanded
 * with its own instance state as the last layer (sh_decl_carrier_tree), under
 * its class. Its inherit is stored in placed (malloc-owned, or NULL). */
static void ni_loot_placed(ni_loot_closure *c, const ni_scan *scan, size_t index, char **placed)
{
    const sh_native_inline_source *source = c->source;
    const ni_span *span = scan->spans + index;
    sh_json_object original = {0}, normalized = {0};
    sh_decl_node *literal = NULL, *instance = NULL, *state = NULL;
    char inherit[512], header[600], error[256], label[64], reason[700], *klass = NULL;
    size_t inherit_length = 0;
    ni_marks marks = {0};
    ni_ref_walk w = {source->schema};
    w.source = source;
    *placed = NULL;
    snprintf(label, sizeof(label), "entity/%zu", span->entity);
    if (!sh_native_json_parse_object(scan->json + span->offset, span->length, 64, &original)) {
        snprintf(reason, sizeof(reason), "placed %s is not native JSON", label); ni_loot_incomplete(c, reason); goto done;
    }
    const sh_json_object *definition = ni_v_upgrade(&original, source->version, &normalized) == 1 ? &normalized : &original;
    const char *value = sh_json_object_get(definition, "inherit"), *instance_state = sh_json_object_get(definition, "state");
    if (!value || !sh_native_json_decode_string(value, strlen(value), inherit, sizeof(inherit), &inherit_length) ||
        inherit_length >= sizeof(inherit) || strlen(inherit) != inherit_length || strchr(inherit, '"') ||
        strchr(inherit, '\\') ||
        (size_t)snprintf(header, sizeof(header), "{ inherit = \"%s\"; }", inherit) >= sizeof(header)) {
        snprintf(reason, sizeof(reason), "the inherit of placed %s is not a name the walk can state", label);
        ni_loot_incomplete(c, reason); goto done;
    }
    if (!(*placed = _strdup(inherit))) { c->failed = 1; goto done; }
    ni_class_view view = {source};
    sh_decl_entity_class_source classes = {&view, ni_class_read, ni_class_derives};
    literal = sh_decl_tree_parse_ordered((sh_decl_source){header, strlen(header)}, error, sizeof(error));
    if (literal && instance_state) {
        instance = sh_decl_carrier_tree(instance_state, strlen(instance_state), ni_record, &marks, error, sizeof(error));
        if (!instance) {
            snprintf(reason, sizeof(reason), "the instance state of placed %s: %s", label, error);
            ni_loot_incomplete(c, reason); goto done;
        }
        sh_decl_node **tail = &literal->children;
        while (*tail) tail = &(*tail)->next;
        *tail = instance->children; instance->children = NULL;
    }
    klass = literal ? sh_decl_entity_tree_class(literal, &classes, error, sizeof(error)) : NULL;
    state = klass ? sh_decl_entity_expanded_state(literal, &classes, error, sizeof(error)) : NULL;
    if (!state) {
        snprintf(reason, sizeof(reason), "the state of placed %s is not modelled: %s", label, error);
        ni_loot_incomplete(c, reason); goto done;
    }
    sh_decl_dependency_schema schema = {&w, ni_rw_describe, ni_rw_field, ni_rw_reference, ni_rw_gap, ni_rw_dynamic,
        ni_rw_site, NULL, NULL, ni_rw_empty};
    sh_decl_dependency_result walked = {0};
    snprintf(w.klass, sizeof(w.klass), "%s", klass);
    if (state->children)
        sh_decl_state_dependencies(state->children, (sh_decl_value_type){klass, ""}, &schema, &walked);
    if (w.failed) {
        c->failed = 1;
        if (!*c->failure) snprintf(c->failure, sizeof(c->failure), "the typed walk of placed %s failed", label);
    }
    else if (walked.aborted) {
        snprintf(reason, sizeof(reason), "the typed walk of placed %s did not complete", label); ni_loot_incomplete(c, reason);
    } else {
        /* Its own flags, read from its constructed state (ni_sp_literal). */
        char value[64], what[96];
        int sp_own = 0;
        if (source->derives(source->context, klass, "idGameChallenge_Coop") == 1)
            sp_own = ni_sp_literal(ni_edit_literal(state, "useSPLoopDropComponent", value, sizeof(value)), value);
        snprintf(what, sizeof(what), "placed %s", label);
        int stated = ni_edit_literal(state, "useSPLootDroppbleItems", value, sizeof(value));
        ni_sp_settings(c, klass, stated, value, what);
        ni_loot_edges(c, "entity", label, "the map places it", NI_LOOT_LIVE, 1, 0, &w, sp_own);
    }
done:
    ni_rw_free(&w); ni_marks_free(&marks);
    sh_decl_tree_free(literal); sh_decl_tree_free(instance); sh_decl_tree_free(state); free(klass);
    sh_json_object_free(&original); sh_json_object_free(&normalized);
}
/* idPlayer.lootspawns (R-295): 1 the use keeps its package (reason set), 0
 * the conversion leaves every count play observes as it was, -1 failure.
 * Converting moves this use's instances from P's count to S's. The closure
 * covers the map's placed entities and the declarations its typed sites name
 * and those stock code constructs by name, then follows the transfer rules of
 * ni_loot_edges. The
 * map's placed entities are walked as constructed with their instance
 * states, and the other sites naming a definition by their consumer rows. The partition is unchanged only when play evaluates no
 * observer of P or S. An
 * observer of S would count this use's instances. An observer of P would stop
 * counting them, and naming S there instead would be the coherent rewrite, but
 * such an observer is served declaration text, which this carrier does not
 * rewrite (it rewrites the map's own typed sites only). */
/* Builds the document's census once: 1, -1 on failure. The partition
 * covers the map's placed entities, the declarations its typed sites name
 * and those stock code constructs by name, then follows the transfer rules
 * of ni_loot_edges. */
static int ni_census_build(const sh_native_inline_source *source, const ni_scan *scan)
{
    ni_loot_closure *c = scan->census;
    if (!c) return -1;
    if (c->built) return c->failed ? -1 : 1;
    c->built = 1; c->source = source;
    g_ni_stats.census_builds++;
    char **placed = scan->count ? calloc(scan->count, sizeof(*placed)) : NULL;
    if (scan->count && !placed) { c->failed = 1; return -1; }
    for (size_t i = 0; i < scan->count && !c->failed; i++) ni_loot_placed(c, scan, i, placed + i);
    /* A typed entityDef name is constructed where a CONSTRUCTS row traces one
     * of its sites, only tested where an identity or ancestry row does, and
     * unclassified elsewhere; a name with no listed site is a placed inherit,
     * walked above with its instance state. Other families' names are drops
     * a placed state already holds or sites no rule classifies. */
    for (size_t i = 0; source->typed_name && !c->failed; i++) {
        const char *family = NULL, *borrowed = NULL;
        int listed = source->typed_name(source->context, i, &family, &borrowed);
        if (listed < 0) { ni_loot_incomplete(c, "the map's typed declaration names are not all known"); break; }
        if (!listed) break;
        if (!family || !borrowed || !*borrowed) continue;
        char name[512], kind[64];
        if (strlen(borrowed) >= sizeof(name) || strlen(family) >= sizeof(kind)) {
            ni_loot_incomplete(c, "a typed declaration name is too long"); continue;
        }
        snprintf(name, sizeof(name), "%s", borrowed); snprintf(kind, sizeof(kind), "%s", family);
        if (_stricmp(kind, "entitydef")) {
            if (_stricmp(kind, "lootdrop")) ni_loot_push(c, kind, name, "the map names it at a typed site", NI_LOOT_UNSURE);
            continue;
        }
        sh_native_inline_use uses[64];
        int sites = source->looked_up ? source->looked_up(source->context, SIZE_MAX, name, uses,
            sizeof(uses) / sizeof(*uses)) : -1;
        if (sites < 0 || sites > (int)(sizeof(uses) / sizeof(*uses))) {
            char text[600];
            snprintf(text, sizeof(text), "the map's sites naming '%s' are not all known", name);
            ni_loot_incomplete(c, text); continue;
        }
        int inherited = 0;
        for (size_t p = 0; p < scan->count && !inherited; p++) inherited = placed[p] && ni_name_equal(placed[p], name);
        if (!sites && !inherited)
            ni_loot_push(c, "entitydef", name, "the map names it at a typed site", NI_LOOT_UNSURE);
        for (int u = 0; u < sites && !c->failed; u++) {
            int failed = 0;
            const struct ni_consumer *row = ni_use_consumer(source, scan, uses + u, &failed);
            if (failed) { c->failed = 1; break; }
            if (row && !ni_consumer_constructs(row)) continue;
            char via[700];
            snprintf(via, sizeof(via), "the map's %s names it at %s", uses[u].owner ? uses[u].owner : "site",
                uses[u].reference ? uses[u].reference : "");
            ni_loot_push(c, "entitydef", name, via, row ? NI_LOOT_LIVE : NI_LOOT_UNSURE);
        }
    }
    for (size_t i = 0; i < scan->count; i++) free(placed[i]);
    free(placed);
    for (size_t i = 0; i < sizeof(ni_code_constructed) / sizeof(*ni_code_constructed) && !c->failed; i++)
        ni_loot_push(c, "entitydef", ni_code_constructed[i], "stock code constructs it by name", NI_LOOT_LIVE);
    ni_hint_census(c, scan);
    for (size_t i = 0; i < c->count && !c->failed; i++) ni_loot_visit(c, i);
    /* A Gameplay Settings entity play reaches may set any challenge's flag:
     * the census is rebuilt with every SP-loop drop. */
    if (!c->failed && c->sp_held && *c->sp_settings && !c->sp_loot) {
        char why[sizeof(c->sp_settings)];
        snprintf(why, sizeof(why), "%s", c->sp_settings);
        ni_census_free(c);
        c->sp_loot = 1;
        snprintf(c->sp_settings, sizeof(c->sp_settings), "%s", why);
        return ni_census_build(source, scan);
    }
    return c->failed ? -1 : 1;
}
static void ni_census_query(const ni_loot_closure *c, const char *from, const char *base, ni_loot_query *q)
{
    memset(q, 0, sizeof(*q));
    for (size_t i = 0; i < c->seen_count; i++) {
        const ni_loot_seen *seen = c->seen + i;
        int is_base = ni_name_equal(seen->name, base);
        if (!is_base && !ni_name_equal(seen->name, from)) continue;
        char *slot = !seen->evaluated ? q->unsure : is_base ? q->base_seen : q->from_seen;
        if (!*slot) snprintf(slot, sizeof(q->unsure), "%s", seen->text);
        if (seen->evaluated && !is_base && seen->served) q->from_served = 1;
    }
}
/* The first entity definition the census reaches named name: a declaration
 * play constructs as name (LIVE) or may (UNSURE), or NULL when none. */
static const ni_loot_node *ni_census_source(const ni_loot_closure *c, const char *name)
{
    for (size_t i = 0; i < c->count; i++)
        if (!_stricmp(c->nodes[i].family, "entitydef") && ni_name_equal(c->nodes[i].name, name)) return c->nodes + i;
    return NULL;
}
/* Whether a placed entity's own map JSON names name anywhere. */
typedef struct ni_names_scan { const char *name; int found, failed; } ni_names_scan;
static int ni_names_visit(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    ni_names_scan *names = context;
    (void)path;
    if (kind != SH_JSON_STRING || names->found) return 1;
    char *value = length < SIZE_MAX ? malloc(length + 1) : NULL;
    size_t decoded = 0;
    if (!value) { names->failed = 1; return 0; }
    if (sh_native_json_decode_string(json, length, value, length + 1, &decoded) && strlen(value) == decoded &&
        ni_name_equal(value, names->name)) names->found = 1;
    free(value);
    return 1;
}
/* A count observer of from the census found at placed entity holder
 * ("entity/N") that the plan's converted copy of that occurrence renames:
 * this round's placed site converted under the substitution, and the
 * occurrence's own map JSON never names from, so the observer is chain text
 * its one inline layer states under the image (ni_site). */
static int ni_placed_carries(const ni_scan *scan, const char *holder, const char *from)
{
    unsigned long long entity = 0; char *end = NULL;
    if (!scan->round || !holder || strncmp(holder, "entity/", 7) || holder[7] < '0' || holder[7] > '9' ||
        (entity = strtoull(holder + 7, &end, 10), !end || *end)) return 0;
    const sh_native_inline_site *site = NULL;
    for (size_t k = 0; k < scan->round_placed && !site; k++)
        if (scan->round->sites[k].entity == entity) site = scan->round->sites + k;
    if (!site || !site->converted || !site->sigma) return 0;
    for (size_t i = 0; i < scan->count; i++) {
        if (scan->spans[i].entity != entity) continue;
        ni_names_scan names = {from};
        return sh_native_json_visit_values(scan->json + scan->spans[i].offset, scan->spans[i].length, 256, ni_names_visit,
            &names) && !names.failed && !names.found;
    }
    return 0;
}
/* An own-state read that leaves the inherited native value unchanged under
 * the plan. Ordinary reflected objects recurse by member; plain values and
 * registered total-assignment forms compare exact tokens. Only a verified
 * declaration-pointer field may substitute a declaration identity. In
 * particular an idStr, by-name reference or event argument retains its name.
 * Lists, resets and custom reader forms need their own equivalence proof.
 * Every accepted statement leaves the baseline unchanged, so repeats and
 * several such inherited layers remain idempotent in their native order.
 * 1 equal, 0 different/unmodelled (why set), -1 failure. */
static int ni_native_idempotent(const sh_native_inline_source *source, const ni_scan *scan,
    sh_decl_value_type type, const sh_decl_node *edit, const sh_decl_node *state,
    const char *from, const char *base, const char *native, const char *path,
    char *why, size_t capacity, int depth)
{
    if (depth > 256) {
        snprintf(why, capacity, "states %s deeper than this comparison reads", path); return 0;
    }
    sh_decl_value_shape object = {0};
    if (!source->schema || !source->schema->field || !source->schema->describe ||
        source->schema->describe(source->schema->context, type, &object) != 1 || object.kind != SH_DECL_VALUE_OBJECT) {
        snprintf(why, capacity, "states %s through a reader this comparison does not model", path); return 0;
    }
    for (const sh_decl_node *member = edit->children; member; member = member->next) {
        sh_decl_value_type field = {0}; sh_decl_value_shape shape = {0};
        char at[1024];
        int length = snprintf(at, sizeof(at), "%s.%s", path, member->key ? member->key : "?");
        if (!member->key || length < 0 || (size_t)length >= sizeof(at) || member->reset || member->annotation ||
            !source->schema || !source->schema->field || !source->schema->describe ||
            source->schema->field(source->schema->context, type, member->key, &field) != 1 ||
            source->schema->describe(source->schema->context, field, &shape) != 1 || shape.kind == SH_DECL_VALUE_UNKNOWN) {
            snprintf(why, capacity, "states %s in a form this comparison does not read", at); return 0;
        }
        const sh_decl_node *held = sh_decl_tree_member(state, member->key);
        if (!held) { snprintf(why, capacity, "differs from native '%s' at %s", native, at); return 0; }
        if (member->compound) {
            if (shape.kind != SH_DECL_VALUE_OBJECT || !held->compound || held->reset || held->annotation) {
                snprintf(why, capacity, "states %s in a form this comparison does not read", at); return 0;
            }
            int equal = ni_native_idempotent(source, scan, field, member, held, from, base, native, at,
                why, capacity, depth + 1);
            if (equal != 1) return equal;
            continue;
        }
        if (held->compound || held->reset || held->annotation || !member->value || !held->value) {
            snprintf(why, capacity, "differs from native '%s' at %s", native, at); return 0;
        }
        if ((shape.kind == SH_DECL_VALUE_IGNORE && shape.plain) || ni_assigned(&shape, member)) {
            if (!strcmp(member->value, held->value)) continue;
        } else if (shape.kind == SH_DECL_VALUE_REFERENCE && field.ops && !strcmp(field.ops, "*")) {
            char value[512], inherited[512]; const char *family = NULL;
            /* NULL clears a pointer; quoted "NULL" names a declaration. An
             * empty quoted name can resolve _emptyName, so preserve its token
             * too instead of identifying it with the null pointer. */
            if (!strcmp(member->value, "NULL") || !strcmp(held->value, "NULL")) {
                if (!strcmp(member->value, held->value)) continue;
            } else if (ni_literal_name(member, value, sizeof(value)) && ni_literal_name(held, inherited, sizeof(inherited))) {
                int resolved = source->resolve_decl ?
                    source->resolve_decl(source->context, field.name, value, &family) : -1;
                if (resolved < 0) {
                    snprintf(why, capacity, "cannot resolve the declaration reader at %s", at); return 0;
                }
                if (resolved == 1 && family) {
                    const char *image = !_stricmp(family, "entitydef") ?
                        (from && ni_name_equal(value, from) ? base : ni_sigma_image(scan, value)) :
                        !_stricmp(family, "lootdrop") ? ni_drop_image(scan, value) : NULL;
                    if (image ? ni_name_equal(image, inherited) : !strcmp(value, inherited)) continue;
                }
            }
        } else {
            snprintf(why, capacity, "states %s in a form this comparison does not read", at); return 0;
        }
        snprintf(why, capacity, "differs from native '%s' at %s", native, at); return 0;
    }
    return 1;
}
/* Whether served loot drop holder, an observer of from (NULL: the plan's
 * images only), builds its native parent's object under the plan. Loot drops
 * parse through the generic typeinfo Parse on both images (idDeclLootDrop
 * vtable +0x98: VK 0x1407859e0 / GL 0x140784bb0, each calling VK 0x1417bc4d0
 * / GL 0x1417aefe0), which copies a found parent's object and then reads only
 * the declaration's own edit block over it, by named member. A served drop
 * whose served inheritance layers each leave the first native ancestor's
 * state unchanged under that typed comparison therefore builds the native
 * drop's object. 1 equal (native set), 0 not (why set), 2 not
 * established (why set), -1 failure. */
static const char ni_drop_unserved[] = "is not served text here";
static int ni_lootdrop_native(const sh_native_inline_source *source, const ni_scan *scan, const char *holder,
    const char *from, const char *base, char *native, size_t native_capacity, char *why, size_t why_capacity)
{
    sh_decl_node **layers = NULL, *state = NULL;
    char **names = NULL, *current = _strdup(holder), error[256];
    size_t count = 0;
    int folded, result = current ? 0 : -1;
    *native = 0;
    while (current) {
        sh_decl_source text = {0}; int served = 0;
        for (size_t i = 0; i < count; i++) if (ni_name_equal(names[i], current)) {
            snprintf(why, why_capacity, "inherits itself through loot drop '%s'", current); goto done;
        }
        int read = source->read_decl ? source->read_decl(source->context, "lootdrop", current, &text, &served) : -1;
        if (read < 0) { free((char *)text.text); result = -1; goto done; }
        if (read != 1 || !served) {
            free((char *)text.text);
            if (!count) { snprintf(why, why_capacity, "%s", ni_drop_unserved); result = 2; goto done; }
            if (read != 1) { snprintf(why, why_capacity, "inherits '%s', which no source provides", current); goto done; }
            break;
        }
        sh_decl_node *tree = ni_family_parse("lootdrop", text, error, sizeof(error));
        free((char *)text.text);
        if (!tree) { snprintf(why, why_capacity, "does not parse: %s", error); result = 2; goto done; }
        sh_decl_node **grown = realloc(layers, (count + 1) * sizeof(*grown));
        if (grown) layers = grown;
        char **named = grown ? realloc(names, (count + 1) * sizeof(*named)) : NULL;
        if (!named) { sh_decl_tree_free(tree); result = -1; goto done; }
        names = named; layers[count] = tree; names[count++] = current; current = NULL;
        const sh_decl_node *edit = NULL;
        char parent[512] = "";
        for (const sh_decl_node *member = tree->children; member; member = member->next) {
            if (member == tree->children && member->key && !strcmp(member->key, "inherit") &&
                !member->reset && !member->annotation && ni_literal_name(member, parent, sizeof(parent))) continue;
            if (member->key && !strcmp(member->key, "edit") && member->compound && !member->reset &&
                !member->annotation && !edit) { edit = member; continue; }
            snprintf(why, why_capacity, "states '%s' beside one inherit and one edit block", member->key ? member->key : "?");
            goto done;
        }
        if (!*parent) { snprintf(why, why_capacity, "inherits no loot drop, so no native loot drop stands for it"); goto done; }
        current = _strdup(parent);
        if (!current) { result = -1; goto done; }
    }
    if (!current) goto done;
    folded = ni_family_state(source, "lootdrop", current, &state, NULL, why, why_capacity);
    if (folded < 0) { result = -1; goto done; }
    if (folded != 1) {
        if (!folded) snprintf(why, why_capacity, "inherits '%s', which does not read", current);
        result = 2; goto done;
    }
    for (size_t i = count; i-- > 0;) {
        const sh_decl_node *edit = sh_decl_tree_member(layers[i], "edit");
        if (!edit) continue;
        result = ni_native_idempotent(source, scan, (sh_decl_value_type){"idDeclLootDrop", ""}, edit, state,
            from, base, current, "edit", why, why_capacity, 0);
        if (result != 1) goto done;
    }
    if (strlen(current) >= native_capacity) { snprintf(why, why_capacity, "has a native ancestor name beyond this comparison"); result = 0; goto done; }
    snprintf(native, native_capacity, "%s", current);
    result = 1;
done:
    free(current); sh_decl_tree_free(state);
    for (size_t i = 0; i < count; i++) { sh_decl_tree_free(layers[i]); free(names[i]); }
    free(layers); free(names);
    return result;
}
#ifdef SH_NATIVE_INLINE_TESTING
int sh_native_inline_test_lootdrop(const sh_native_inline_source *source, const char *holder,
    const sh_native_inline_substitution *substitutions, size_t count, char *native, size_t native_capacity,
    char *reason, size_t capacity)
{
    ni_scan scan = {0};
    scan.sigma = calloc(count ? count : 1, sizeof(*scan.sigma));
    if (!scan.sigma) return -1;
    scan.sigma_count = count;
    for (size_t i = 0; i < count; i++)
        scan.sigma[i] = (ni_sigma){substitutions[i].from, substitutions[i].to, substitutions[i].drop};
    int result = ni_lootdrop_native(source, &scan, holder, NULL, NULL, native, native_capacity, reason, capacity);
    free(scan.sigma); return result;
}
#endif
/* The reason a round withdrew name's plan entry, from scan->sigma_withdrawn
 * ("'name' (reason); ..."), or empty. */
static void ni_withdrawn_reason(const ni_scan *scan, const char *name, char *out, size_t capacity)
{
    char needle[600];
    *out = 0;
    if (!name || (size_t)snprintf(needle, sizeof(needle), "'%s' (", name) >= sizeof(needle)) return;
    const char *at = strstr(scan->sigma_withdrawn, needle);
    if (!at) return;
    at += strlen(needle);
    size_t depth = 1, length = 0;
    while (at[length] && depth) {
        if (at[length] == '(') depth++;
        else if (at[length] == ')' && !--depth) break;
        length++;
    }
    snprintf(out, capacity, "%.*s", (int)length, at);
}
static int ni_loot_counted(const sh_native_inline_source *source, const ni_scan *scan, const char *from,
    const char *base, sh_native_inline_site *site)
{
    char counts[600] = "";
    int registers = ni_counts_pickups(source, from, counts, sizeof(counts));
    if (registers <= 0) return registers;
    if (ni_census_build(source, scan) < 0) return -1;
    const ni_loot_closure *c = scan->census;
    ni_loot_query q;
    ni_census_query(c, from, base, &q);
    /* Holder substitution: when every evaluated observer of from sits in a
     * served entity definition the plan converts to its native ancestor, whose
     * same typed site names base (ni_independent's substituted comparison),
     * in the chain text of a placed occurrence the plan converts, whose one
     * inline layer names base there, or in a served loot drop the plan names
     * the native drop in place of (ni_drop_keeps: that drop's object, with
     * from read as base, and every holder naming it converts), those observers
     * count the instances of base afterwards, which are exactly the converted
     * instances of from while nothing else play reaches constructs base. */
    const char *image = ni_sigma_image(scan, from);
    if (*q.from_seen && !*q.base_seen && !*q.unsure && !*c->incomplete && image && ni_name_equal(image, base)) {
        size_t carried = 0; int all = 1;
        for (size_t i = 0; i < c->seen_count && all; i++) {
            const ni_loot_seen *seen = c->seen + i;
            if (!seen->evaluated || !ni_name_equal(seen->name, from)) continue;
            if ((!_stricmp(seen->family, "entitydef") && ni_sigma_image(scan, seen->holder)) ||
                (!_stricmp(seen->family, "entity") && ni_placed_carries(scan, seen->holder, from)) ||
                (!_stricmp(seen->family, "lootdrop") && ni_drop_image(scan, seen->holder))) carried++;
            else all = 0;
        }
        const ni_loot_node *other = all && carried ? ni_census_source(c, base) : NULL;
        if (all && carried && !other) { site->carried = carried; return 0; }
        if (all && carried) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "%s; that holder converts to its native ancestor with this use, whose "
                "observer would then also count the instances play %s as '%s' (%s)", q.from_seen,
                other->context == NI_LOOT_LIVE ? "constructs" : "may construct", base, other->via);
            return 1;
        }
    }
    int result = *q.base_seen || *q.from_seen || *q.unsure || *c->incomplete ? 1 : 0;
    if (result != 1) ;
    else if (*q.base_seen)
        ni_refusef(site, SH_DECL_CARRIER_GAP, "%s; instances of '%s' count there (%s), so instances this use "
            "constructs would join that count", q.base_seen, from, counts);
    else if (*q.from_seen) {
        /* The first observer of from, when it is a served loot drop: whether
         * it builds its native parent's object under the plan. */
        const ni_loot_seen *drop = NULL;
        for (size_t i = 0; i < c->seen_count; i++) {
            const ni_loot_seen *seen = c->seen + i;
            if (!seen->evaluated || !ni_name_equal(seen->name, from) || strcmp(seen->text, q.from_seen)) continue;
            if (seen->served && !_stricmp(seen->family, "lootdrop")) drop = seen;
            break;
        }
        char native[512] = "", why[600] = "", plan[600] = "";
        int equal = drop ? ni_lootdrop_native(source, scan, drop->holder, from, base, native, sizeof(native), why,
            sizeof(why)) : 0;
        if (equal < 0) return -1;
        if (drop && equal == 1) {
            /* Why the plan's drop substitution does not carry it. */
            const char *named = ni_drop_image(scan, drop->holder);
            if (named && !image) snprintf(plan, sizeof(plan), "the plan names '%s' in its place, but '%s' itself is not "
                "in the plan", named, from);
            else if (named && !ni_name_equal(image, base)) snprintf(plan, sizeof(plan), "the plan names '%s' in its place, "
                "but it gives '%s' the image '%s'", named, from, image);
            else if (named && *q.unsure) snprintf(plan, sizeof(plan), "the plan names '%s' in its place, but another "
                "observer is reached only through a reference no transfer rule classifies: %s", named, q.unsure);
            else if (named && *c->incomplete) snprintf(plan, sizeof(plan), "the plan names '%s' in its place, but the map "
                "census is incomplete: %s", named, c->incomplete);
            else if (named) snprintf(plan, sizeof(plan), "the plan names '%s' in its place, but another evaluated observer "
                "of '%s' is not carried with it", named, from);
            else {
                char reason[400] = "";
                ni_withdrawn_reason(scan, drop->holder, reason, sizeof(reason));
                snprintf(plan, sizeof(plan), "the plan does not name '%s' in its place%s%s", native, *reason ? ": " : "",
                    reason);
            }
            ni_refusef(site, SH_DECL_CARRIER_GAP, "%s; instances of '%s' count there (%s), so instances this use "
                "constructs would leave that count. That served loot drop builds native '%s''s object under the plan "
                "(its parent's object with its own edit block), and %s", q.from_seen, from, counts, native, plan);
        }
        else if (drop)
            ni_refusef(site, SH_DECL_CARRIER_GAP, "%s; instances of '%s' count there (%s), so instances this use "
                "constructs would leave that count, and that served loot drop %s", q.from_seen, from, counts, why);
        else
            ni_refusef(site, SH_DECL_CARRIER_GAP, "%s; instances of '%s' count there (%s), so instances this use "
                "constructs would leave that count, and %s", q.from_seen, from, counts, q.from_served ?
                "the observer is served declaration text, which the map carrier cannot rewrite to name the native "
                "ancestor" : "no rewrite of that observer is established");
    }
    else if (*q.unsure)
        ni_refusef(site, SH_DECL_CARRIER_GAP, "%s; instances of '%s' count there (%s), and whether play evaluates that "
            "observer is not established: it is reached only through a reference no transfer rule classifies", q.unsure,
            from, counts);
    else
        ni_refusef(site, SH_DECL_CARRIER_GAP, "instances of '%s' count in idPlayer.lootspawns (%s), and whether a "
            "loot drop play reaches observes that count is not established: %s", from, counts, c->incomplete);
    return result;
}
/* Whether name contains text, ignoring case. */
static int ni_name_contains(const char *name, const char *text)
{
    size_t length = strlen(text);
    for (; name && *name; name++) if (!_strnicmp(name, text, length)) return 1;
    return 0;
}
/* Generated instance names (R-295 segment 4). The spawn finish (VK
 * 0x14081e970 / GL 0x14081d7b0) calls SetName (VK 0x14081d760 / GL
 * 0x14081c5a0) with an empty name for an instance its spawn arguments do not
 * name, as a use's construction leaves them. SetName names it '<own
 * declaration name>_<k>' from its entity number k upward, past every name
 * FindEntity (game vtable +0x498: VK 0x1403161b0 / GL 0x1403159c0) already
 * finds, and renames an explicit name FindEntity finds to '<name>_renamed_<k>'.
 * FindEntity hashes and compares names ignoring ASCII case on both images
 * (VK 0x141a00480 / 0x141a00920, GL 0x1419f26e0 / 0x1419f2b80). Converting a
 * use moves its instances' names from '<P>_<k>' to '<S>_<k>', so an authored
 * string of either form, a lookup key or another entity's name, can observe
 * the move. Which readers take a string as an entity name is not enumerated,
 * so every map string, and every served string outside a declaration-pointer
 * site (ni_pointer_sites), counts. This is containment while those readers
 * stay open, not coverage; 1 when value has such a form. */
static int ni_generated_form(const char *value, size_t length, const char *name)
{
    size_t n = name ? strlen(name) : 0, i = n + 1, digits;
    if (!n || length <= i || _strnicmp(value, name, n) || value[n] != '_') return 0;
    for (digits = i; i < length && value[i] >= '0' && value[i] <= '9'; i++) ;
    if (i == digits) return 0;
    if (i == length) return 1;
    if (length - i <= 9 || _strnicmp(value + i, "_renamed_", 9)) return 0;
    for (i += 9, digits = i; i < length && value[i] >= '0' && value[i] <= '9'; i++) ;
    return i > digits && i == length;
}
/* Typed runtime-name sites (genname-evidence.md, name events): the idEntity
 * dispatcher (vtable +0x20, VK 0x1416136f0 / GL 0x141605a20) sends findEntity
 * (eventnum 159) to a handler that passes its string unchanged to FindEntity
 * (VK 0x14080fe00 / GL 0x14080ec40), setName (161) to one that passes it
 * unchanged to SetName (VK 0x1408115d0 / GL 0x1408103f0), and getName (160)
 * to one returning the receiver's name (VK 0x140810380 / GL 0x14080f1c0). An
 * `entity` argument names its entity through the unresolved-entity-name
 * table. A placed entity's explicit name is registered by SetName. */
enum { NI_NAME_UNKNOWN, NI_NAME_LOOKUP, NI_NAME_WRITE, NI_NAME_ENTITY_REF, NI_NAME_EXPLICIT, NI_NAME_COMPUTED,
    NI_NAME_SCRIPT, NI_NAME_UNAVAILABLE_SCRIPT, NI_NAME_SCRIPT_PROJECTED, NI_NAME_SCRIPT_INACTIVE };
typedef struct ni_name_site { const void *at; int kind; char *key;size_t key_length; } ni_name_site;
static void ni_name_sites_free(ni_name_site *sites,size_t count)
{for(size_t i=0;i<count;i++)free(sites[i].key);free(sites);}
typedef struct ni_map_call { char *path; char event[128]; } ni_map_call;
typedef struct ni_generated_scan {
    const char *from, *to; int failed; char found[300];
    const sh_decl_node *const *exempt; size_t exempt_count;    /* ni_pointer_sites of the tree scanned */
    const ni_name_site *names; size_t name_count;
    const char *const *map_exempt; size_t map_exempt_count;    /* typed map pointer sites (ni_map_pointer_visit) */
    int kind;                       /* the NAME kind of found */
    const ni_map_call *calls; size_t call_count;    /* each map object's literal eventDef (ni_map_pointer_visit) */
    char getname[300];              /* where a getName call was seen */
} ni_generated_scan;
static const char *ni_name_kind_text(int kind)
{
    switch (kind) {
    case NI_NAME_LOOKUP: return "a findEntity lookup key (its handler passes the key unchanged to FindEntity, which folds "
        "ASCII case, and writes only its result; the posted-event and eventCallInfo services discard that result, but "
        "whether this call's receiver dispatches it to that handler with no override, forwarding dispatcher or listener "
        "that keeps it is not established: unfinished support, not a requirement)";
    case NI_NAME_WRITE: return "a setName name (its handler passes the name unchanged to SetName, which renames on a "
        "case-folded collision)";
    case NI_NAME_ENTITY_REF: return "an entity argument (resolved by name through the unresolved-entity-name table)";
    case NI_NAME_EXPLICIT: return "an explicit entity name (SetName registers it and renames it on a case-folded "
        "collision)";
    case NI_NAME_COMPUTED: return "a name argument that is not a literal, so its value is not known here";
    case NI_NAME_SCRIPT: return "a CallScriptFunction entity lookup key (the first-dot prefix reaches FindEntity; "
        "the function suffix is preserved)";
    case NI_NAME_UNAVAILABLE_SCRIPT: return "a script-call member whose native reader/storage is not established";
    default: return "a string whose reader is not enumerated";
    }
}
static int ni_name_kind_of(const ni_generated_scan *named, const void *at)
{
    for (size_t i = 0; at && i < named->name_count; i++) if (named->names[i].at == at) return named->names[i].kind;
    return NI_NAME_UNKNOWN;
}
static const ni_name_site *ni_name_site_of(const ni_generated_scan *named,const void *at)
{for(size_t i=0;at && i<named->name_count;i++)if(named->names[i].at==at)return named->names+i;return NULL;}
/* A JSON member key as the native reader matches it (sh_decl_event_member
 * strips a writer's type annotation); "" for an array element or root. */
static void ni_json_key(const sh_json_path *path, char *out, size_t capacity)
{
    out[0] = 0;
    if (!path || !path->key || path->key_length >= capacity) return;
    memcpy(out, path->key, path->key_length); out[path->key_length] = 0;
    const char *member = sh_decl_event_member(out);
    if (member != out) memmove(out, member, strlen(member) + 1);
}
/* The chain of keys and indices from the document root; 0 when too deep. */
static int ni_json_path_text(const sh_json_path *path, char *out, size_t capacity)
{
    const sh_json_path *chain[96];
    size_t depth = 0, used = 0;
    out[0] = 0;
    for (; path; path = path->parent) { if (depth == 96) return 0; chain[depth++] = path; }
    while (depth--) {
        const sh_json_path *at = chain[depth];
        int written = at->key ? snprintf(out + used, capacity - used, "/k:%.*s", (int)at->key_length, at->key) :
            snprintf(out + used, capacity - used, "/i:%zu", at->index);
        if (written < 0 || (size_t)written >= capacity - used) { out[0] = 0; return 0; }
        used += (size_t)written;
    }
    return 1;
}
/* The NAME kind of a map value from its JSON position: a placed entity's
 * explicit name; an argument of a findEntity or setName call, or any entity
 * argument, whose call object's literal eventDef was seen before its args
 * (the writer's order; anything else stays unknown). *exempt for a placed
 * entity's inherit header, a typed entityDef reference Parse looks up by name. */
static int ni_map_name_kind(ni_generated_scan *named, const sh_json_path *path, int *exempt)
{
    char key[128], parent[128], grand[128], call[512];
    *exempt = 0;
    ni_json_key(path, key, sizeof(key));
    ni_json_key(path ? path->parent : NULL, parent, sizeof(parent));
    if (!strcmp(parent, "entityDef") && !strcmp(key, "name")) return NI_NAME_EXPLICIT;
    if (!strcmp(parent, "entityDef") && !strcmp(key, "inherit")) { *exempt = 1; return NI_NAME_UNKNOWN; }
    if (!path || !path->parent || !path->parent->parent || strncmp(parent, "item[", 5)) return NI_NAME_UNKNOWN;
    ni_json_key(path->parent->parent, grand, sizeof(grand));
    const char *event = NULL;
    if (!strcmp(grand, "args") && ni_json_path_text(path->parent->parent->parent, call, sizeof(call)))
        for (size_t i = 0; i < named->call_count && !event; i++)
            if (!strcmp(named->calls[i].path, call)) event = named->calls[i].event;
    if (!event) return NI_NAME_UNKNOWN;
    if (!_stricmp(key, "entity")) return NI_NAME_ENTITY_REF;
    if (_stricmp(key, "string")) return NI_NAME_UNKNOWN;
    if (!_stricmp(event, "findEntity")) return NI_NAME_LOOKUP;
    if (!_stricmp(event, "setName")) return NI_NAME_WRITE;
    return NI_NAME_UNKNOWN;
}
static int ni_generated_visit(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    ni_generated_scan *named = context;
    char key[128];
    int exempt = 0, name_kind;
    if (*named->found) return 1;
    ni_json_key(path, key, sizeof(key));
    name_kind = ni_map_name_kind(named, path, &exempt);
    int typed_kind = ni_name_kind_of(named, json);
    if (typed_kind != NI_NAME_UNKNOWN) name_kind = typed_kind;
    for (size_t e = 0; !exempt && e < named->map_exempt_count; e++) exempt = named->map_exempt[e] == json;
    if (exempt) return 1;
    if(name_kind==NI_NAME_SCRIPT_INACTIVE)return 1;
    if(name_kind==NI_NAME_SCRIPT_PROJECTED){
        const ni_name_site *site=ni_name_site_of(named,json);
        if(site && (ni_generated_form(site->key,site->key_length,named->from) || ni_generated_form(site->key,site->key_length,named->to))){
            snprintf(named->found,sizeof named->found,"%.*s",(int)site->key_length,site->key);named->kind=NI_NAME_SCRIPT;
        }return 1;
    }
    if (name_kind == NI_NAME_UNAVAILABLE_SCRIPT) {
        snprintf(named->found, sizeof(named->found), "an unavailable native script-call lookup key");
        named->kind = name_kind; return 1;
    }
    if (kind != SH_JSON_STRING) {
        /* A name argument the writer did not spell as a literal string. */
        if (name_kind != NI_NAME_UNKNOWN && name_kind != NI_NAME_EXPLICIT && kind != SH_JSON_NULL) {
            snprintf(named->found, sizeof(named->found), "a non-literal value");
            named->kind = NI_NAME_COMPUTED;
        }
        return 1;
    }
    char *value = length < SIZE_MAX ? malloc(length + 1) : NULL;
    size_t decoded = 0;
    if (!value) { named->failed = 1; return 0; }
    int literal = sh_native_json_decode_string(json, length, value, length + 1, &decoded) && strlen(value) == decoded;
    if (name_kind == NI_NAME_SCRIPT) {
        const char *lookup = NULL; size_t lookup_length = 0;
        int projected = literal ? sh_native_nameflow_key(SH_NATIVE_NAMEFLOW_SCRIPT_CALL, value, decoded,
            &lookup, &lookup_length) : -1;
        if (projected < 0) {
            snprintf(named->found, sizeof(named->found), "an unavailable script-call lookup key");
            named->kind = NI_NAME_COMPUTED;
        } else if (projected && (ni_generated_form(lookup, lookup_length, named->from) ||
            ni_generated_form(lookup, lookup_length, named->to))) {
            snprintf(named->found, sizeof(named->found), "%.*s", (int)lookup_length, lookup);
            named->kind = NI_NAME_SCRIPT;
        }
        free(value); return 1;
    }
    if (literal && !strcmp(key, "eventDef") && !_stricmp(value, "getName") && !*named->getname) {
        char at[256];
        if (!ni_json_path_text(path ? path->parent : NULL, at, sizeof(at))) snprintf(at, sizeof(at), "an event call");
        snprintf(named->getname, sizeof(named->getname), "the map document at %s", at);
    }
    if (literal && !exempt && (ni_generated_form(value, decoded, named->from) || ni_generated_form(value, decoded, named->to))) {
        snprintf(named->found, sizeof(named->found), "%s", value);
        named->kind = name_kind;
    }
    free(value);
    return 1;
}
/* One live served entity definition's typed sites, from its class's walk.
 * Declaration pointers: their reader (VK 0x1409ca890 / GL 0x1409ca050, R-287
 * section 3) looks the token up as a declaration name, stores the pointer and
 * keeps no name, so such a value is not an entity-name input; by-name and
 * event forms are not exempt. Event values (event_value): NAME sites for
 * findEntity and setName strings and entity arguments, and any getName call.
 * Every reported site is verified even when the walk stops later. 1 walked, 0
 * class or walk not established, -1 failure; the caller frees the arrays and
 * base. */
typedef struct ni_pointer_walk {
    ni_ref_walk base;
    const sh_native_inline_source *source;
    const sh_decl_node **nodes; size_t count, capacity;
    ni_name_site *names; size_t name_count, name_capacity;
    char getname[300];
    sh_decl_node *name_state;
    sh_native_nameflow_scope name_scope;
    const char *from,*to;
} ni_pointer_walk;
static int ni_discarded_name_event(const sh_native_inline_source *source, const char *event, size_t length)
{
    char name[11];
    if (!source || !source->discarded_name_event || !event || length >= sizeof(name)) return 0;
    memcpy(name, event, length); name[length] = 0;
    return source->discarded_name_event(source->context, name) == 1;
}
static int ni_pointer_exempt(ni_pointer_walk *w, const sh_decl_node *node)
{
    if (w->count == w->capacity) {
        size_t next = w->capacity ? w->capacity * 2 : 8;
        const sh_decl_node **grown = next <= SIZE_MAX / sizeof(*grown) ? realloc((void *)w->nodes, next * sizeof(*grown)) : NULL;
        if (!grown) { w->base.failed = 1; return 0; }
        w->nodes = grown; w->capacity = next;
    }
    w->nodes[w->count++] = node;
    return 1;
}
static int ni_pointer_site(void *c, const char *path, sh_decl_value_type type, const char *name, size_t length,
    const sh_decl_reference_site *site)
{
    ni_pointer_walk *w = c;
    (void)path; (void)name;
    if (!length || !type.name || !site || !site->node) return 1;
    if (type.ops && (!strcmp(type.ops, SH_DECL_BY_NAME_DECL_OPS) ||
        (!strcmp(type.ops, SH_DECL_EVENT_DECL_USE_OPS) && !ni_event_decl_constructor(&w->base, type, site))))
        return !w->base.failed;
    return ni_pointer_exempt(w, site->node);
}
static int ni_name_event(void *c, const char *path, const char *event, size_t event_length, size_t index,
    const char *key, const sh_decl_reference_site *site)
{
    ni_pointer_walk *w = c;
    int kind = NI_NAME_UNKNOWN;
    if (!key) {
        if (event && event_length == 7 && !_strnicmp(event, "getName", 7) && !*w->getname &&
            !ni_discarded_name_event(w->source, event, event_length))
            snprintf(w->getname, sizeof(w->getname), "%s", path && *path ? path : "an event call");
        return 1;
    }
    if (!_stricmp(key, "entity")) kind = NI_NAME_ENTITY_REF;
    else if (!_stricmp(key, "string") && event && event_length == 10 && !_strnicmp(event, "findEntity", 10))
        kind = NI_NAME_LOOKUP;
    else if (!_stricmp(key, "string") && event && event_length == 7 && !_strnicmp(event, "setName", 7))
        kind = NI_NAME_WRITE;
    if (kind == NI_NAME_UNKNOWN || !site || !site->node) return 1;
    /* Only a literal key at the handler's string argument is proved here.
     * A compound argument stays open; it can have its own value semantics. */
    if (kind == NI_NAME_LOOKUP && index == 0 && !site->node->compound &&
        ni_discarded_name_event(w->source, event, event_length))
        return ni_pointer_exempt(w, site->node);
    if (site->node->compound) kind = NI_NAME_COMPUTED;
    if (w->name_count == w->name_capacity) {
        size_t next = w->name_capacity ? w->name_capacity * 2 : 8;
        ni_name_site *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(w->names, next * sizeof(*grown)) : NULL;
        if (!grown) { w->base.failed = 1; return 0; }
        w->names = grown; w->name_capacity = next;
    }
    w->names[w->name_count++] = (ni_name_site){site->node, kind};
    return 1;
}
static int ni_name_add(ni_name_site **names, size_t *count, size_t *capacity, const void *at, int kind)
{
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 8;
        ni_name_site *grown = next > *capacity && next <= SIZE_MAX / sizeof(*grown) ?
            realloc(*names, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        *names = grown; *capacity = next;
    }
    (*names)[(*count)++] = (ni_name_site){at, kind}; return 1;
}
static int ni_name_enum(void *context,sh_decl_value_type type,const sh_decl_node *token,int32_t *value)
{
    const sh_native_inline_source *source=context;const char *literal=NULL;size_t length=0;
    if(!sh_decl_tree_literal(token,&literal,&length) || !length || length==SIZE_MAX)return 0;
    char *text=malloc(length+1);if(!text)return 0;memcpy(text,literal,length);text[length]=0;
    int ok=0;if(token->value && token->value[0]!='"'){
        char *end=NULL;errno=0;long long number=strtoll(text,&end,10);
        if(!errno && end!=text && !*end && number>=INT32_MIN && number<=INT32_MAX){*value=(int32_t)number;ok=1;}
    }
    if(!ok && source->native_schema){ni_hint_enum_value found={text,0,0};
        if(sh_decl_native_schema_enum_constants(source->native_schema,type.name,ni_hint_enum_constant,&found)==1 && found.found){*value=(int32_t)found.value;ok=1;}}
    free(text);return ok;
}
static int ni_name_project(const sh_native_inline_source *source,const sh_native_nameflow_scope *scope,
    const char *from,const char *to,const char *path,sh_decl_value_type type,const sh_decl_reference_site *site,
    ni_name_site **names,size_t *count,size_t *capacity)
{
    const char *effective=NULL,*key=NULL;size_t length=0,key_length=0;
    sh_native_nameflow_kind flow=sh_native_nameflow_scoped_member(source->schema,source->context,source->derives,
        type,site,path,scope,&effective,&length);
    if(flow==SH_NATIVE_NAMEFLOW_NONE)return 1;
    int kind=flow==SH_NATIVE_NAMEFLOW_INACTIVE?NI_NAME_SCRIPT_INACTIVE:
        flow==SH_NATIVE_NAMEFLOW_SCRIPT_CALL?NI_NAME_SCRIPT:NI_NAME_UNAVAILABLE_SCRIPT;
    if(effective && flow==SH_NATIVE_NAMEFLOW_SCRIPT_CALL){
        int projected=sh_native_nameflow_key(flow,effective,length,&key,&key_length);
        kind=projected>0?NI_NAME_SCRIPT_PROJECTED:projected==0?NI_NAME_SCRIPT_INACTIVE:NI_NAME_UNAVAILABLE_SCRIPT;
    }
    /* An unavailable new record witness matters only if this actual literal
     * can target a namespace the selected substitution changes. */
    if(kind==NI_NAME_UNAVAILABLE_SCRIPT && site->member && !strcmp(site->member,"scriptFunction")){
        const char *literal=NULL;size_t n=0;char *decoded=NULL;
        if(site->node)sh_decl_tree_literal(site->node,&literal,&n);
        else if(site->json && site->json_length<SIZE_MAX){decoded=malloc(site->json_length+1);
            if(decoded && sh_native_json_decode_string(site->json,site->json_length,decoded,site->json_length+1,&n) && strlen(decoded)==n)literal=decoded;}
        int projected=literal?sh_native_nameflow_key(SH_NATIVE_NAMEFLOW_SCRIPT_CALL,literal,n,&key,&key_length):-1;
        int affected=projected<0 || (projected && (ni_generated_form(key,key_length,from) || ni_generated_form(key,key_length,to))) ||
            (literal && (ni_generated_form(literal,n,from) || ni_generated_form(literal,n,to)));
        free(decoded);if(!affected)return 1;key=NULL;
    }
    if(site->node && site->node->compound && kind==NI_NAME_SCRIPT)kind=NI_NAME_COMPUTED;
    if(!ni_name_add(names,count,capacity,site->node?(const void *)site->node:(const void *)site->json,kind))return 0;
    if(kind==NI_NAME_SCRIPT_PROJECTED){char *copy=malloc(key_length+1);if(!copy)return 0;
        memcpy(copy,key,key_length);copy[key_length]=0;(*names)[*count-1].key=copy;(*names)[*count-1].key_length=key_length;}
    return 1;
}
static int ni_pointer_value_site(void *c, const char *path, sh_decl_value_type type, const sh_decl_reference_site *site)
{
    ni_pointer_walk *w = c;
    if (!site || !site->node) return 1;
    if (!ni_name_project(w->source,&w->name_scope,w->from,w->to,path,type,site,&w->names,&w->name_count,&w->name_capacity)) {
        w->base.failed = 1; return 0;
    }
    return 1;
}
static int ni_pointer_sites(const sh_native_inline_source *source, const ni_served_decl *decl, ni_pointer_walk *w)
{
    char class_name[256];
    int entity=!_stricmp(decl->family,"entitydef");
    const char *family_class=entity?NULL:ni_family_class(decl->family);
    int known=entity?ni_name_class(source,decl->name,class_name,sizeof class_name):family_class?1:0;
    if(!entity && family_class)snprintf(class_name,sizeof class_name,"%s",family_class);
    if (known <= 0) return known;
    sh_decl_dependency_schema schema = {w, ni_rw_describe, ni_rw_field, ni_rw_reference, ni_rw_gap, ni_rw_dynamic,
        ni_pointer_site, NULL, NULL, ni_rw_empty, ni_name_event, ni_pointer_value_site};
    /* New non-entity scope adds only the proved script-name consumers. It
     * does not grant pointer exemptions to a different native grammar. */
    if(!entity){schema.reference_site=NULL;schema.event_value=NULL;schema.empty_lookup=NULL;}
    sh_decl_dependency_result walked = {0};
    w->base.inner = source->schema;
    w->base.source = source;
    w->source = source;
    if(entity){ni_class_view view={source};sh_decl_entity_class_source classes={&view,ni_class_read,ni_class_derives};
        w->name_state=sh_decl_entity_expanded_state(decl->tree,&classes,NULL,0);
        w->name_scope.state=w->name_state?w->name_state->children:NULL;
    }else{char reason[256];int served=0;
        ni_family_state(source,decl->family,decl->name,&w->name_state,&served,reason,sizeof reason);
        w->name_scope.state=w->name_state;
    }
    w->name_scope.type=(sh_decl_value_type){class_name,""};w->name_scope.context=(void *)source;w->name_scope.enum_value=ni_name_enum;
    if(entity)sh_decl_entity_dependencies(decl->tree, (sh_decl_value_type){class_name, ""}, &schema, &walked);
    else sh_decl_envelope_dependencies(decl->tree,(sh_decl_value_type){class_name,""},(sh_decl_value_type){"idDeclTypeInfo","*"},&schema,&walked);
    return w->base.failed ? -1 : !walked.aborted;
}
/* The map's declaration-pointer sites, by the same reader contract: each
 * placed entity's state edit walked over its own JSON span
 * (sh_decl_json_state_dependencies) as its className, or without one as the
 * class its inherit chain gives (ni_name_class). Values are visited after
 * their children, so an entity is walked when its entityDef object closes,
 * whatever order its members take; an entity whose class is not established
 * exempts nothing. The same pass records every object's literal eventDef by
 * path, so arguments classify by their own call in any member order. */
typedef struct ni_map_entity { char *path; char klass[256], inherit[256]; const char *edit; size_t edit_length; } ni_map_entity;
typedef struct ni_map_pointer_walk {
    ni_ref_walk base;
    const sh_native_inline_source *source;
    const char **spans; size_t count, capacity;
    ni_map_entity *entities; size_t entity_count, entity_capacity;    /* entityDef objects not yet closed */
    ni_map_call *calls; size_t call_count, call_capacity;
    ni_name_site *names; size_t name_count, name_capacity;
    sh_native_nameflow_scope name_scope;
    const char *from,*to;
} ni_map_pointer_walk;
static ni_map_entity *ni_map_entity_at(ni_map_pointer_walk *w, const char *path, int add)
{
    for (size_t i = 0; i < w->entity_count; i++) if (!strcmp(w->entities[i].path, path)) return w->entities + i;
    if (!add) return NULL;
    if (w->entity_count == w->entity_capacity) {
        size_t next = w->entity_capacity ? w->entity_capacity * 2 : 4;
        ni_map_entity *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(w->entities, next * sizeof(*grown)) : NULL;
        if (!grown) { w->base.failed = 1; return NULL; }
        w->entities = grown; w->entity_capacity = next;
    }
    ni_map_entity *entity = w->entities + w->entity_count;
    memset(entity, 0, sizeof(*entity));
    if (!(entity->path = _strdup(path))) { w->base.failed = 1; return NULL; }
    w->entity_count++;
    return entity;
}
static void ni_map_pointer_free(ni_map_pointer_walk *w)
{
    for (size_t i = 0; i < w->entity_count; i++) free(w->entities[i].path);
    for (size_t i = 0; i < w->call_count; i++) free(w->calls[i].path);
    free(w->entities); free(w->calls); ni_name_sites_free(w->names,w->name_count); free((void *)w->spans); ni_rw_free(&w->base);
}
static int ni_map_pointer_exempt(ni_map_pointer_walk *w, const char *json)
{
    if (w->count == w->capacity) {
        size_t next = w->capacity ? w->capacity * 2 : 8;
        const char **grown = next <= SIZE_MAX / sizeof(*grown) ? realloc((void *)w->spans, next * sizeof(*grown)) : NULL;
        if (!grown) { w->base.failed = 1; return 0; }
        w->spans = grown; w->capacity = next;
    }
    w->spans[w->count++] = json;
    return 1;
}
static int ni_map_pointer_site(void *c, const char *path, sh_decl_value_type type, const char *name, size_t length,
    const sh_decl_reference_site *site)
{
    ni_map_pointer_walk *w = c;
    (void)path; (void)name;
    if (!length || !type.name || !site || !site->json) return 1;
    if (type.ops && (!strcmp(type.ops, SH_DECL_BY_NAME_DECL_OPS) ||
        (!strcmp(type.ops, SH_DECL_EVENT_DECL_USE_OPS) && !ni_event_decl_constructor(&w->base, type, site))))
        return !w->base.failed;
    return ni_map_pointer_exempt(w, site->json);
}
static int ni_map_name_event(void *c, const char *path, const char *event, size_t event_length, size_t index,
    const char *key, const sh_decl_reference_site *site)
{
    ni_map_pointer_walk *w = c;
    (void)path;
    if (!site || !site->json || site->json_length < 2 || site->json[0] != '"') return 1;
    if ((!key && event && event_length == 7 && !_strnicmp(event, "getName", 7)) ||
        (key && !_stricmp(key, "string") && index == 0 && event && event_length == 10 &&
            !_strnicmp(event, "findEntity", 10))) {
        if (ni_discarded_name_event(w->source, event, event_length))
            return ni_map_pointer_exempt(w, site->json);
    }
    return 1;
}
static int ni_map_value_site(void *c, const char *path, sh_decl_value_type type, const sh_decl_reference_site *site)
{
    ni_map_pointer_walk *w = c;
    if (!site || !site->json) return 1;
    if (!ni_name_project(w->source,&w->name_scope,w->from,w->to,path,type,site,&w->names,&w->name_count,&w->name_capacity)) {
        w->base.failed = 1; return 0;
    }
    return 1;
}
static int ni_map_pointer_visit(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    ni_map_pointer_walk *w = context;
    char key[128], parent[128], grand[128], at[512];
    ni_json_key(path, key, sizeof(key));
    ni_json_key(path ? path->parent : NULL, parent, sizeof(parent));
    if (kind == SH_JSON_STRING && path && (!strcmp(key, "eventDef") ||
            (!strcmp(parent, "entityDef") && (!strcmp(key, "className") || !strcmp(key, "inherit"))))) {
        char text[256]; size_t decoded = 0;
        if (length >= sizeof(text) || !sh_native_json_decode_string(json, length, text, sizeof(text), &decoded) ||
            strlen(text) != decoded || !ni_json_path_text(path->parent, at, sizeof(at))) return 1;
        if (!strcmp(key, "eventDef")) {
            if (decoded >= sizeof(w->calls->event)) return 1;
            if (w->call_count == w->call_capacity) {
                size_t next = w->call_capacity ? w->call_capacity * 2 : 8;
                ni_map_call *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(w->calls, next * sizeof(*grown)) : NULL;
                if (!grown) { w->base.failed = 1; return 0; }
                w->calls = grown; w->call_capacity = next;
            }
            ni_map_call *call = w->calls + w->call_count;
            if (!(call->path = _strdup(at))) { w->base.failed = 1; return 0; }
            snprintf(call->event, sizeof(call->event), "%s", text);
            w->call_count++;
            return 1;
        }
        ni_map_entity *entity = ni_map_entity_at(w, at, 1);
        if (!entity) return 0;
        snprintf(!strcmp(key, "className") ? entity->klass : entity->inherit, sizeof(entity->klass), "%s", text);
        return 1;
    }
    if (kind != SH_JSON_OBJECT || !path) return 1;
    if (!strcmp(key, "edit") && !strcmp(parent, "state") && path->parent->parent) {
        ni_json_key(path->parent->parent, grand, sizeof(grand));
        if (strcmp(grand, "entityDef") || !ni_json_path_text(path->parent->parent, at, sizeof(at))) return 1;
        ni_map_entity *entity = ni_map_entity_at(w, at, 1);
        if (!entity) return 0;
        entity->edit = json; entity->edit_length = length;
        return 1;
    }
    if (strcmp(key, "entityDef") || !ni_json_path_text(path, at, sizeof(at))) return 1;
    ni_map_entity *entity = ni_map_entity_at(w, at, 0);
    if (!entity) return 1;
    char klass[256] = "";
    int known = 0;
    if (*entity->klass) { snprintf(klass, sizeof(klass), "%s", entity->klass); known = 1; }
    else if (*entity->inherit) known = ni_name_class(w->source, entity->inherit, klass, sizeof(klass));
    if (known < 0) w->base.failed = 1;
    else if (known > 0 && entity->edit) {
        w->base.source = w->source;
        ni_class_view view={w->source};sh_decl_entity_class_source classes={&view,ni_class_read,ni_class_derives};
        sh_decl_node *literal=ni_inherit_tree(entity->inherit),*instance=sh_decl_carrier_tree(entity->edit,entity->edit_length,NULL,NULL,NULL,0),*effective=NULL;
        if(literal && instance){sh_decl_node *edit=ni_node("edit",NULL,1);
            if(edit){edit->children=instance->children;instance->children=NULL;sh_decl_node **tail=&literal->children;while(*tail)tail=&(*tail)->next;*tail=edit;
                effective=sh_decl_entity_expanded_state(literal,&classes,NULL,0);}}
        w->name_scope=(sh_native_nameflow_scope){effective?effective->children:NULL,{klass,""},(void *)w->source,ni_name_enum};
        sh_decl_dependency_schema schema = {w, ni_rw_describe, ni_rw_field, ni_rw_reference, ni_rw_gap, ni_rw_dynamic,
            ni_map_pointer_site, NULL, NULL, ni_rw_empty, ni_map_name_event, ni_map_value_site};
        sh_decl_dependency_result walked = {0};
        sh_decl_json_state_dependencies(entity->edit, entity->edit_length, (sh_decl_value_type){klass, ""}, &schema, &walked);
        w->name_scope=(sh_native_nameflow_scope){0};sh_decl_tree_free(effective);sh_decl_tree_free(instance);sh_decl_tree_free(literal);
    }
    free(entity->path);
    *entity = w->entities[--w->entity_count];
    return !w->base.failed;
}
/* Values are read as the declaration parser's literal (sh_decl_tree_literal);
 * the raw token is matched as well, so the check only widens. A computed NAME
 * site counts whatever it holds. */
static int ni_generated_tree(const sh_decl_node *node, ni_generated_scan *named)
{
    for (; node && !*named->found; node = node->next) {
        int exempt = 0, kind = ni_name_kind_of(named, node);
        for (size_t e = 0; e < named->exempt_count && !exempt; e++) exempt = named->exempt[e] == node;
        if(kind==NI_NAME_SCRIPT_INACTIVE)continue;
        if(kind==NI_NAME_SCRIPT_PROJECTED){const ni_name_site *site=ni_name_site_of(named,node);
            if(site && (ni_generated_form(site->key,site->key_length,named->from) || ni_generated_form(site->key,site->key_length,named->to))){
                snprintf(named->found,sizeof named->found,"%.*s",(int)site->key_length,site->key);named->kind=NI_NAME_SCRIPT;}
            continue;}
        if (kind == NI_NAME_COMPUTED || kind == NI_NAME_UNAVAILABLE_SCRIPT) {
            snprintf(named->found, sizeof(named->found), "%s", kind == NI_NAME_COMPUTED ?
                "a non-literal value" : "an unavailable native script-call lookup key");
            named->kind = kind;
            break;
        }
        const char *value = exempt ? NULL : node->value, *text = NULL;
        size_t length = value ? strlen(value) : 0, decoded = 0;
        if (!exempt && kind == NI_NAME_SCRIPT) {
            const char *lookup = NULL; size_t lookup_length = 0;
            int projected = value && sh_decl_tree_literal(node, &text, &decoded) ?
                sh_native_nameflow_key(SH_NATIVE_NAMEFLOW_SCRIPT_CALL, text, decoded, &lookup, &lookup_length) : -1;
            if (projected < 0) {
                snprintf(named->found, sizeof(named->found), "an unavailable script-call lookup key");
                named->kind = NI_NAME_COMPUTED;
            } else if (projected && (ni_generated_form(lookup, lookup_length, named->from) ||
                ni_generated_form(lookup, lookup_length, named->to))) {
                snprintf(named->found, sizeof(named->found), "%.*s", (int)lookup_length, lookup);
                named->kind = NI_NAME_SCRIPT;
            }
            continue;
        }
        if (value && sh_decl_tree_literal(node, &text, &decoded) &&
            (ni_generated_form(text, decoded, named->from) || ni_generated_form(text, decoded, named->to))) {
            snprintf(named->found, sizeof(named->found), "%.*s", (int)decoded, text);
            named->kind = kind;
            break;
        }
        if (length >= 2 && value[0] == '"' && value[length - 1] == '"') { value++; length -= 2; }
        if (value && (ni_generated_form(value, length, named->from) || ni_generated_form(value, length, named->to))) {
            snprintf(named->found, sizeof(named->found), "%.*s", (int)length, value);
            named->kind = kind;
        }
        else if (node->children) ni_generated_tree(node->children, named);
    }
    return *named->found != 0;
}
/* Name namespaces a constructing use's instances share (R-295 N7). An
 * unnamed instance is named '<own declaration>_<k>' from its entity number k
 * upward past every name FindEntity finds (SetName VK 0x14081d760 / GL
 * 0x14081c5a0); converting the use moves those names from P's namespace into
 * S's. Of SetName's 45 call sites (39 functions, paired on both images), two
 * kinds put other names of that form into a definition's namespace:
 * - stock code composing '<literal>_<n>': the console light commands (VK
 *   0x1409d5f00, 0x1409dbb50, 0x1409dbd50 'light_%d'; 0x1409db720
 *   'spawned_light_%d'), which take the lowest n FindEntity misses; the
 *   dossier carousel (0x140a3f2b0 'WeaponCarouselItem_%d'); the demon player
 *   bodies (0x1412ffd40 'thirdPersonBody_demon_%d', 'transformHands_demon_%d');
 *   and the podium avatars' FindEntity keys 'info_mp_intro_start_pos_<n>' (VK
 *   0x140a84fe0, 0x140a87ad0). A P or S equal to such a literal moves this
 *   use's names into or out of a namespace stock code numbers and probes, so
 *   the native form cannot carry them (only 'light' is a shipped entity
 *   definition);
 * - composed '<definition>_<spawn count>' names (SpawnSingleEntity VK
 *   0x1412cff00, the AI spawn queue's 0x141122d70, resurrection 0x14088b5e0)
 *   for definitions a spawn target (idTarget_Spawn_Parent) names in an
 *   entityDefs or initialEntityDefs entry (idSpawnerEntityDef: name, text,
 *   bounds, def). An entity number and a spawn count can be equal, and
 *   SetName would then resolve a collision the package never had. A map
 *   entity or served definition of a spawn-target class naming S, or stating
 *   an inline text whose definition name is not established, therefore shares
 *   S's namespace. Shipped definitions are not checked here: in the shipped
 *   text the catalog reader selects and reads, the three spawn-target
 *   definitions (target/spawn, target/snapmap/spawnnode,
 *   target/spawners/spawn_manager) and the module spawn nodes state no entries
 *   (a lexical scan; about 125 indexed entity definitions do not read). How
 *   the conductor supplies spawn nodes' definitions at run time is not traced;
 *   its aiTypeList is an exact-identity consumer the code-constructed chains
 *   decide.
 * Owner-number names (a player's third-person body '<body>_<player number>',
 * VK 0x140b783c0) hold a live owner's entity number, which no unnamed instance
 * holds at the same time, and player, avatar and console names come from
 * outside the package. */
static const char *const ni_code_name_prefixes[] = {"light", "spawned_light", "info_mp_intro_start_pos",
    "WeaponCarouselItem", "thirdPersonBody_demon", "transformHands_demon"};
static const char *const ni_spawn_entry_sites[] = {
    "/k:entityDef/k:state/k:edit/k:entityDefs/k:item[*]/k:def", "/k:entityDef/k:state/k:edit/k:entityDefs/k:item[*]/k:name",
    "/k:entityDef/k:state/k:edit/k:entityDefs/k:item[*]/k:text",
    "/k:entityDef/k:state/k:edit/k:initialEntityDefs/k:item[*]/k:def",
    "/k:entityDef/k:state/k:edit/k:initialEntityDefs/k:item[*]/k:name",
    "/k:entityDef/k:state/k:edit/k:initialEntityDefs/k:item[*]/k:text"};
/* 1 a definition named by def or name, 2 an inline text, 0 another site. */
static int ni_spawn_entry_kind(const char *reference)
{
    for (size_t i = 0; reference && i < sizeof(ni_spawn_entry_sites) / sizeof(*ni_spawn_entry_sites); i++) {
        struct ni_consumer row = {NULL, ni_spawn_entry_sites[i], NULL, NI_CONSUMER_CONSTRUCTS, 0, NULL};
        if (ni_consumer_path(&row, reference)) return i % 3 == 2 ? 2 : 1;
    }
    return 0;
}
typedef struct ni_spawn_scan {
    const sh_native_inline_source *source; const ni_scan *scan; const char *name;
    int failed; char reason[512];
} ni_spawn_scan;
static int ni_spawn_visit(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    ni_spawn_scan *found = context;
    const sh_json_path *root = path, *item = NULL;
    if (kind != SH_JSON_STRING || !path || *found->reason) return 1;
    while (root->parent) { item = root; root = root->parent; }
    if (!item || item->key || !root->key || root->key_length != 8 || memcmp(root->key, "entities", 8)) return 1;
    sh_package_provenance_site site;
    if (!sh_map_binding_site_at(path, &site)) { found->failed = 1; return 0; }
    int entry = ni_spawn_entry_kind(site.reference);
    char *value = entry && length < SIZE_MAX ? malloc(length + 1) : NULL;
    size_t decoded = 0;
    if (entry && !value) found->failed = 1;
    int literal = value && sh_native_json_decode_string(json, length, value, length + 1, &decoded) && strlen(value) == decoded;
    if (value && (entry == 2 ? !literal || decoded : !literal || ni_name_equal(value, found->name))) {
        char class_name[256], what[320];
        int known = ni_entity_class(found->source, found->scan, item->index, class_name, sizeof(class_name));
        if (entry == 2) snprintf(what, sizeof(what), "stating an inline definition whose name is not established");
        else if (!literal) snprintf(what, sizeof(what), "naming a definition that is not a plain string");
        else snprintf(what, sizeof(what), "naming native '%s'", found->name);
        if (known < 0) found->failed = 1;
        else if (!known || found->source->derives(found->source->context, class_name, "idTarget_Spawn_Parent") == 1)
            snprintf(found->reason, sizeof(found->reason), "%s %s is a spawn target's entry %s%s; its spawns are named "
                "'<definition>_<spawn count>' (SpawnSingleEntity VK 0x1412cff00 / GL 0x1412c2000), in the namespace this "
                "use's instances would enter as '%s_<entity number>', so SetName would resolve collisions between them "
                "that the package never had", site.owner, site.reference, what, known ? "" : " (its class is not resolved)",
                found->name);
    }
    free(value); free((void *)site.owner); free((void *)site.reference);
    return !found->failed;
}
/* A served definition's own entityDefs/initialEntityDefs entries: 1 one names
 * name or states an inline text or a non-literal (reason set), 0 none. */
static int ni_spawn_tree(const sh_decl_node *tree, const char *decl, const char *name, char *reason, size_t capacity)
{
    for (const sh_decl_node *edit = tree ? tree->children : NULL; edit; edit = edit->next) {
        if (!edit->key || strcmp(edit->key, "edit")) continue;
        for (const sh_decl_node *list = edit->children; list; list = list->next) {
            if (!list->key || (strcmp(list->key, "entityDefs") && strcmp(list->key, "initialEntityDefs"))) continue;
            for (const sh_decl_node *entry = list->children; entry; entry = entry->next) {
                if (!entry->key || strncmp(entry->key, "item[", 5)) continue;
                for (const sh_decl_node *member = entry->children; member; member = member->next) {
                    char text[512];
                    int field = member->key && (!strcmp(member->key, "def") || !strcmp(member->key, "name")) ? 1 :
                        member->key && !strcmp(member->key, "text") ? 2 : 0;
                    if (!field) continue;
                    int literal = ni_literal_name(member, text, sizeof(text));
                    if (field == 2 ? !literal || *text : !literal || ni_name_equal(text, name)) {
                        snprintf(reason, capacity, "served spawn target '%s' %s.%s.%s %s; its spawns are named "
                            "'<definition>_<spawn count>' (SpawnSingleEntity VK 0x1412cff00 / GL 0x1412c2000), in the "
                            "namespace this use's instances would enter as '%s_<entity number>', so SetName would resolve "
                            "collisions between them that the package never had", decl, list->key, entry->key,
                            member->key, field == 2 ? "states an inline definition whose name is not established" :
                            literal ? "names the native target" : "is not a plain name", name);
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}
/* 1 refused (reason set), 0 none, -1 failure. */
static int ni_shared_namespace(const sh_native_inline_source *source, const ni_scan *scan, const char *from,
    const char *base, sh_native_inline_site *site)
{
    for (size_t i = 0; i < sizeof(ni_code_name_prefixes) / sizeof(*ni_code_name_prefixes); i++) {
        int before = ni_name_equal(from, ni_code_name_prefixes[i]), after = ni_name_equal(base, ni_code_name_prefixes[i]);
        if (!before && !after) continue;
        ni_refusef(site, SH_DECL_CARRIER_ENGINE, "stock code names and looks up entities '%s_<n>' (console light commands, "
            "menu and player avatars; SetName caller census R-295 N7), and this use's unnamed instances are named '%s_<n>' "
            "before and '%s_<n>' after conversion, which would move them %s that namespace", ni_code_name_prefixes[i],
            from, base, after ? "into" : "out of");
        return 1;
    }
    ni_spawn_scan found = {source, scan, base, 0, ""};
    if (!sh_native_json_visit_values(scan->json, scan->json_length, 256, ni_spawn_visit, &found) || found.failed) return -1;
    if (*found.reason) { ni_refusef(site, SH_DECL_CARRIER_GAP, "%s", found.reason); return 1; }
    ni_served *index = scan->served;
    int collected = index ? ni_served_collect(source, scan, index) : -1;
    if (collected < 0) return -1;
    if (collected == 2) { ni_refusef(site, SH_DECL_CARRIER_GAP, "%s", index->reason); return 1; }
    for (size_t i = 0; i < index->count; i++) {
        const ni_served_decl *decl = index->decls + i;
        char class_name[256], reason[600];
        if (_stricmp(decl->family, "entitydef")) continue;
        int known = ni_name_class(source, decl->name, class_name, sizeof(class_name));
        if (known < 0) return -1;
        if (known && source->derives(source->context, class_name, "idTarget_Spawn_Parent") != 1) continue;
        if (ni_spawn_tree(decl->tree, decl->name, base, reason, sizeof(reason))) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "%s%s", reason, known ? "" : " (its class is not resolved)");
            return 1;
        }
    }
    return 0;
}
/* Decides one independent use naming site->from: 1 it names the native
 * ancestor site->to, 0 it keeps its package (reason set), 2 not this route
 * (not a traced construction, or the definition changes state), -1 failure. */
static int ni_independent(const sh_native_inline_source *source, ni_pool *pool, const ni_scan *scan,
    const sh_native_inline_use *use, const char *kept, sh_decl_carrier_limit kept_limit, sh_native_inline_site *site)
{
    ni_layer *layers = NULL; size_t layer_count = 0, layer_capacity = 0;
    sh_decl_node *reference = NULL, *candidate = NULL, *expanded_ref = NULL, *expanded_cand = NULL;
    char *link = NULL, *base = NULL, *replaces = NULL, *ref_class = NULL, *cand_class = NULL;
    char **ancestry = NULL; size_t ancestry_count = 0;
    char error[256] = "";
    int failed = 0, result = -1;
    size_t substituted = 0;     /* P's own references the plan renames (ni_sigma_rewrite) */
    /* Exact-identity operands naming P, renamed to S with P's whole group. */
    struct sh_native_inline_retarget *operands = NULL; size_t operand_count = 0;
    const char *operand_what = NULL;
    const struct ni_consumer *consumer = ni_use_consumer(source, scan, use, &failed);
    if (failed) return -1;
    if (!ni_consumer_constructs(consumer)) return 2;
    /* Chain P..S(excl), nearest first, read as a placed occurrence's is. */
    if (!(link = _strdup(site->from))) return -1;
    for (;;) {
        sh_decl_source text = {0}; int served = 0;
        free(replaces); replaces = NULL;
        int read = source->read(source->context, link, &text, &served, &replaces);
        if (read < 0) goto done;
        if (!read || !served) {
            free((char *)text.text);
            if (!layer_count) { result = 2; goto done; }
            if (!read) { ni_refusef(site, SH_DECL_CARRIER_GAP, "ancestor '%s' is unavailable", link); result = 0; goto done; }
            base = link; link = NULL;
            break;
        }
        for (size_t i = 0; i < layer_count; i++) if (ni_name_equal(layers[i].name, link)) {
            free((char *)text.text);
            ni_refusef(site, SH_DECL_CARRIER_GAP, "cyclic inheritance through '%s'", link); result = 0; goto done;
        }
        sh_decl_node *tree = sh_decl_tree_parse_ordered(text, error, sizeof(error));
        free((char *)text.text);
        if (!tree) { ni_refusef(site, SH_DECL_CARRIER_GAP, "'%s' does not parse: %s", link, error); result = 0; goto done; }
        if (layer_count == layer_capacity) {
            size_t next = layer_capacity ? layer_capacity * 2 : 4;
            ni_layer *grown = realloc(layers, next * sizeof(*grown));
            if (!grown) { sh_decl_tree_free(tree); goto done; }
            layers = grown; layer_capacity = next;
        }
        layers[layer_count++] = (ni_layer){link, tree}; link = NULL;
        {
            sh_decl_entity_headers headers;
            const char *which = NULL;
            sh_decl_entity_split(tree, &headers);
            for (const sh_decl_node *member = tree->children; member != headers.state; member = member->next)
                if (!ni_member_allowed(tree, member, &which)) break;
            if (which) {
                ni_refusef(site, SH_DECL_CARRIER_GAP, "entity definition member '%s' of '%s' is not modelled for a "
                    "construction by name", which, layers[layer_count - 1].name);
                result = 0; goto done;
            }
        }
        const char *own = layer_count == 1 ? ni_own_setting(tree) : NULL;
        if (own) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "'%s' states its own %s, which construction by its name reads and "
                "its native ancestor's name does not carry", site->from, own);
            result = 0; goto done;
        }
        if (replaces) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "'%s' is a package replacement of stock '%s'; naming its native "
                "ancestor instead is not modelled", layers[layer_count - 1].name, replaces);
            result = 0; goto done;
        }
        link = ni_parent(tree);
        if (!link || !*link) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "'%s' has no native ancestor to name", site->from);
            result = 0; goto done;
        }
    }
    /* The source model: the same class and expanded state, or P changes it. */
    {
        ni_class_view view = {source};
        sh_decl_entity_class_source classes = {&view, ni_class_read, ni_class_derives};
        if (!(reference = ni_inherit_tree(site->from)) || !(candidate = ni_inherit_tree(base))) goto done;
        expanded_ref = sh_decl_entity_expanded_state(reference, &classes, error, sizeof(error));
        if (expanded_ref) expanded_cand = sh_decl_entity_expanded_state(candidate, &classes, error, sizeof(error));
        if (expanded_cand) ref_class = sh_decl_entity_tree_class(reference, &classes, error, sizeof(error));
        if (ref_class) cand_class = sh_decl_entity_tree_class(candidate, &classes, error, sizeof(error));
        if (!cand_class) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "the state of '%s' over native '%s' is not modelled: %s", site->from,
                base, error);
            result = 0; goto done;
        }
        /* Compared under the plan's holder substitution: P's own typed
         * entityDef references to served definitions the plan converts name
         * their native ancestors, and its loot drop references to drops the
         * plan substitutes name their native drops, as they will once the plan
         * holds; nothing else is rewritten. */
        if (scan->sigma_count && expanded_ref->children &&
            ni_sigma_rewrite(source, scan, expanded_ref->children, ref_class, 0, NULL, &substituted) < 0) goto done;
        int equal = ni_equal(expanded_ref, expanded_cand);
        if (equal < 0) goto done;
        if (!equal || strcmp(ref_class, cand_class)) { result = 2; goto done; }
        site->sigma = substituted > 0;
    }
    if (kept) { ni_refusef(site, kept_limit, "%s", kept); result = 0; goto done; }
    /* S's own settings and ancestry, by name. */
    for (char *name = _strdup(base); name; ) {
        sh_decl_source text = {0}; int served = 0;
        char **grown = ancestry_count < 64 ? realloc(ancestry, (ancestry_count + 1) * sizeof(*grown)) : NULL;
        if (!grown) {
            free(name);
            if (ancestry_count < 64) goto done;
            ni_refusef(site, SH_DECL_CARRIER_GAP, "the native ancestry of '%s' does not end", base); result = 0; goto done;
        }
        ancestry = grown; ancestry[ancestry_count++] = name;
        int read = source->read(source->context, name, &text, &served, NULL);
        if (read < 0) goto done;
        sh_decl_node *tree = read ? sh_decl_tree_parse_ordered(text, error, sizeof(error)) : NULL;
        free((char *)text.text);
        if (!tree) { ni_refusef(site, SH_DECL_CARRIER_GAP, "native ancestor '%s' is not readable", name); result = 0; goto done; }
        const char *own = ancestry_count == 1 ? ni_own_setting(tree) : NULL;
        if (own) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "native '%s' states its own %s, so constructing it by name differs from "
                "constructing '%s'", base, own, site->from);
            sh_decl_tree_free(tree); result = 0; goto done;
        }
        name = ni_parent(tree);
        sh_decl_tree_free(tree);
        if (name && !*name) { free(name); name = NULL; }
    }
    /* Stock code compares an entity's own declaration name with a literal
     * definition: VK 0x14032cd40 / GL 0x14032c720 test the +0x6d0 name (+8)
     * against the static VK 0x1456fff88 / GL 0x144002588, initialized from the
     * literal below at 0x1400fc2d0 on both builds (R-295). */
    {
        static const char *const literals[] = {
            "zion/objects/props_set/mecharm/sarcophagus_arm_right_touchscreen_idanim"};
        for (size_t i = 0; i < sizeof(literals) / sizeof(*literals); i++) if (ni_name_equal(base, literals[i])) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "stock code compares an entity's own declaration name with '%s' "
                "exactly, so instances constructed under the native name would be treated as that definition", base);
            result = 0; goto done;
        }
    }
    /* AI attack selection tests whether a target's own declaration name
     * contains a literal, ignoring case (VK 0x140635ac0 call 0x140636277 / GL
     * 0x140635dd0 call 0x140636587, R-295); the served and native names must
     * agree on it. */
    {
        static const char *const contained[] = {"sentrybot"};
        for (size_t i = 0; i < sizeof(contained) / sizeof(*contained); i++)
            if (!ni_name_contains(site->from, contained[i]) != !ni_name_contains(base, contained[i])) {
                ni_refusef(site, SH_DECL_CARRIER_GAP, "stock code tests whether an entity's own declaration name contains "
                    "'%s', ignoring case, and '%s' and '%s' differ on it, so instances constructed under the native name "
                    "would be treated differently", contained[i], site->from, base);
                result = 0; goto done;
            }
    }
    /* Every other map use of P or a folded layer must construct. */
    for (size_t i = 0;; i++) {
        sh_native_inline_use other = {0};
        const char *other_name = NULL, *why = NULL;
        sh_decl_carrier_limit ignored = SH_DECL_CARRIER_GAP;
        int listed = source->bound_use(source->context, i, &other, &other_name, &why, &ignored);
        if (listed < 0) { ni_refusef(site, SH_DECL_CARRIER_GAP, "the map's typed uses are not all known"); result = 0; goto done; }
        if (!listed) break;
        if (!other_name) continue;
        if (other.owner && other.reference && !strcmp(other.owner, use->owner) && !strcmp(other.reference, use->reference) && !ni_consumer_identity(consumer))
            continue;
        size_t folded = layer_count;
        for (size_t j = 0; j < layer_count && folded == layer_count; j++) if (ni_name_equal(other_name, layers[j].name)) folded = j;
        if (folded == layer_count) continue;
        const struct ni_consumer *other_consumer = ni_use_consumer(source, scan, &other, &failed);
        if (failed) goto done;
        if (ni_consumer_constructs(other_consumer) && !ni_consumer_identity(other_consumer)) continue;
        /* An exact-identity consumer compares an entity's own declaration, and
         * this use's instances are P's: one naming a folded parent layer never
         * matched them, and one naming P is an operand the coordinated plan
         * renames to S with every construction of P (ni_couple), when the map
         * states it as one plain string at its own site. */
        if (ni_consumer_identity(other_consumer) && folded) continue;
        if (ni_consumer_identity(other_consumer) && other.owner && other.reference) {
            unsigned long long at = 0; char *end = NULL;
            if (!strncmp(other.owner, "entity/", 7) && other.owner[7] >= '0' && other.owner[7] <= '9' &&
                (at = strtoull(other.owner + 7, &end, 10), end && !*end) &&
                ni_site_holds(scan->json, scan->json_length, &other, (size_t)at, other_name)) {
                struct sh_native_inline_retarget *grown = realloc(operands, (operand_count + 1) * sizeof(*grown));
                if (!grown) goto done;
                operands = grown;
                struct sh_native_inline_retarget *row = operands + operand_count++;
                *row = (struct sh_native_inline_retarget){_strdup(other.owner), _strdup(other.reference), _strdup(other_name)};
                if (!row->owner || !row->reference || !row->name) goto done;
                if (!operand_what) operand_what = other_consumer->what;
                continue;
            }
        }
        if (other_consumer)
            ni_refusef(site, SH_DECL_CARRIER_GAP, "%s %s is %s naming '%s', which would stop selecting instances this "
                "use constructs under the native name", other.owner, other.reference, other_consumer->what,
                layers[folded].name);
        else
            ni_refusef(site, SH_DECL_CARRIER_GAP, "%s%s%s also names '%s', and whether that consumer observes the "
                "declaration of instances this use constructs is not traced", other.owner ? other.owner :
                "a historical binding", other.reference ? " " : "", other.reference ? other.reference : "",
                layers[folded].name);
        result = 0; goto done;
    }
    /* ... and every use in a served declaration play instantiates. */
    {
        ni_served *index = scan->served;
        int collected = index ? ni_served_collect(source, scan, index) : -1;
        if (collected < 0) goto done;
        if (collected == 2) { ni_refusef(site, SH_DECL_CARRIER_GAP, "%s", index->reason); result = 0; goto done; }
        for (size_t i = 0; i < index->count; i++) {
            const ni_served_decl *decl = index->decls + i;
            if (!decl->live) continue;
            /* A holder the plan converts is not read once every use of it
             * names its native ancestor, whose same sites name S (its own
             * substituted comparison; ni_sigma_settle keeps both or neither). */
            if (!_stricmp(decl->family, "entitydef") && (ni_sigma_image(scan, decl->name) || ni_placed_only(scan, i))) {
                for (size_t j = 0; j <= layer_count && !site->sigma; j++)
                    site->sigma = ni_tree_mentions(decl->tree->children, decl->inherit, j < layer_count ? layers[j].name : base) != NULL;
                continue;
            }
            for (size_t j = 0; j <= layer_count; j++) {
                ni_served_use served_use = {source, decl, j < layer_count ? layers[j].name : base};
                char path[512] = "";
                served_use.independent = 1; served_use.identity_only = j == layer_count;
                ni_served_walk(&served_use, decl->tree->children, decl->inherit, path, 0, sizeof(path));
                if (served_use.failed) goto done;
                if (*served_use.reason) { ni_refusef(site, SH_DECL_CARRIER_GAP, "%s", served_use.reason); result = 0; goto done; }
            }
        }
    }
    /* ... and no map value names S itself at an exact-identity consumer. */
    {
        ni_identity_scan found = {source, scan, base, 0, ""};
        if (!sh_native_json_visit_values(scan->json, scan->json_length, 256, ni_identity_visit, &found) || found.failed)
            goto done;
        if (*found.reason) { ni_refusef(site, SH_DECL_CARRIER_GAP, "%s", found.reason); result = 0; goto done; }
    }
    /* ... and neither name's namespace holds names stock code numbers or a
     * spawn target composes (ni_shared_namespace). */
    {
        int shared = ni_shared_namespace(source, scan, site->from, base, site);
        if (shared < 0) goto done;
        if (shared) { result = 0; goto done; }
    }
    /* ... and no authored string has the generated name of an instance this
     * use constructs, before or after (ni_generated_form). */
    {
        ni_generated_scan named = {site->from, base, 0, ""};
        char where[300] = "the map document";
        ni_map_pointer_walk typed = {{source->schema}, source};
        typed.from=site->from;typed.to=base;
        int visited = sh_native_json_visit_values(scan->json, scan->json_length, 256, ni_map_pointer_visit, &typed) &&
            !typed.base.failed;
        named.map_exempt = typed.spans; named.map_exempt_count = visited ? typed.count : 0;
        named.calls = typed.calls; named.call_count = visited ? typed.call_count : 0;
        named.names = typed.names; named.name_count = visited ? typed.name_count : 0;
        visited = visited && sh_native_json_visit_values(scan->json, scan->json_length, 256, ni_generated_visit, &named) &&
            !named.failed;
        named.map_exempt = NULL; named.map_exempt_count = 0; named.calls = NULL; named.call_count = 0;
        named.names = NULL; named.name_count = 0;
        ni_map_pointer_free(&typed);
        if (!visited) goto done;
        if (!*named.found) {
            ni_served *index = scan->served;
            int collected = index ? ni_served_collect(source, scan, index) : -1;
            if (collected < 0) goto done;
            if (collected == 2) { ni_refusef(site, SH_DECL_CARRIER_GAP, "%s", index->reason); result = 0; goto done; }
            for (size_t i = 0; i < index->count && !*named.found; i++) {
                if (!index->decls[i].live) continue;
                ni_pointer_walk pointers = {{0}};
                pointers.from=site->from;pointers.to=base;
                int walked = ni_pointer_sites(source, index->decls + i, &pointers);
                named.exempt = walked < 0 ? NULL : pointers.nodes; named.exempt_count = walked < 0 ? 0 : pointers.count;
                named.names = walked < 0 ? NULL : pointers.names; named.name_count = walked < 0 ? 0 : pointers.name_count;
                if (walked >= 0 && ni_generated_tree(index->decls[i].tree->children, &named))
                    snprintf(where, sizeof(where), "served %s declaration '%s'", index->decls[i].family,
                        index->decls[i].name);
                if (walked >= 0 && *pointers.getname && !*named.getname)
                    snprintf(named.getname, sizeof(named.getname), "served %s declaration '%s' at %s",
                        index->decls[i].family, index->decls[i].name, pointers.getname);
                named.exempt = NULL; named.exempt_count = 0; named.names = NULL; named.name_count = 0;
                free((void *)pointers.nodes); ni_name_sites_free(pointers.names,pointers.name_count); sh_decl_tree_free(pointers.name_state); ni_rw_free(&pointers.base);
                if (walked < 0) goto done;
            }
        }
        if (*named.found && named.kind == NI_NAME_COMPUTED) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "%s passes %s; whether it names an instance this use constructs, "
                "whose names would move from '%s_<n>' to '%s_<n>', is not known (unfinished support, not a "
                "requirement)", where, ni_name_kind_text(named.kind), site->from, base);
            result = 0; goto done;
        }
        if (*named.found) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "%s holds '%s', which can name an instance this use constructs: an "
                "unnamed instance is named '<own declaration>_<entity number>' (SetName VK 0x14081d760 / GL 0x14081c5a0) "
                "and names match ignoring case (FindEntity VK 0x1403161b0 / GL 0x1403159c0), so this use would move its "
                "instances' names from '%s_<n>' to '%s_<n>'; the site is %s, and a name-preserving rewrite of authored "
                "names is not supported", where, named.found, site->from, base, ni_name_kind_text(named.kind));
            result = 0; goto done;
        }
        /* getName exports the runtime name itself, with no authored literal. */
        if (*named.getname) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "%s calls getName, whose handler (VK 0x140810380 / GL 0x14080f1c0) "
                "only writes its receiver's runtime name as the result, which would change from '%s_<n>' to '%s_<n>' for a "
                "converted receiver; the posted-event and eventCallInfo services discard that result (VK 0x1409c5c10 / GL "
                "0x1409c5460, VK 0x1409c36f0 / GL 0x1409c2ef0), but the call's receiver and whether its class dispatch "
                "overrides, forwards or keeps the result are not established (unfinished support, not a requirement)",
                named.getname, site->from, base);
            result = 0; goto done;
        }
    }
    /* ... and none in a definition stock code constructs by name. */
    for (size_t i = 0; i < sizeof(ni_code_constructed) / sizeof(*ni_code_constructed); i++) {
        int named = ni_constructed_chain(source, ni_code_constructed[i], layers, layer_count, base, site);
        if (named < 0) goto done;
        if (named) { result = 0; goto done; }
    }
    /* ... and no loot drop play can reach counts what this use constructs. */
    {
        int counted = ni_loot_counted(source, scan, site->from, base, site);
        if (counted < 0) goto done;
        if (counted) { result = 0; goto done; }
    }
    /* The world AI pool selects a base by ancestry and reads the layers below
     * it one by one; with no entry in either chain both construct fresh. */
    if (!pool->resolved && ni_pool_resolve(source, pool) < 0) goto done;
    if (pool->state == NI_POOL_UNRESOLVED) {
        ni_refusef(site, SH_DECL_CARRIER_GAP, "the world's AI spawn pool is not resolved (%s)", pool->reason);
        result = 0; goto done;
    }
    for (size_t i = 0; pool->state == NI_POOL_SET && i < pool->entry_count; i++) {
        int listed = 0;
        for (size_t j = 0; j < layer_count && !listed; j++) listed = !_stricmp(pool->entries[i], layers[j].name);
        for (size_t j = 0; j < ancestry_count && !listed; j++) listed = !_stricmp(pool->entries[i], ancestry[j]);
        if (listed) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "the world's AI spawn pool lists '%s' in this definition's ancestry; "
                "its pooled routes read each layer separately", pool->entries[i]);
            result = 0; goto done;
        }
    }
    /* An AI also takes the pooled settings of the first pool entry that is or
     * descends from its own declaration (VK 0x140697b50 from 0x14065205d / GL
     * 0x140696ad0 from 0x140651ead; table filled by VK 0x14069d930). An entry
     * below S or P is selected differently once instances are S (R-295). */
    /* Each entry's chain is walked to its end with every name visited once: a
     * chain that returns to a visited name or cannot be read leaves the
     * relation unknown, which keeps the package; no depth bound decides it
     * (the declaration set is finite and no native nesting bound is
     * established for already loaded parents). */
    for (size_t i = 0; pool->state == NI_POOL_SET && i < pool->entry_count; i++) {
        char *name = _strdup(pool->entries[i]), **walked = NULL, at[512] = "";
        size_t walked_count = 0;
        const char *related = NULL;
        int state = name ? 1 : -1;   /* 1 walking, 0 unreadable, 2 cyclic, -1 failure */
        while (state == 1 && name && !related) {
            for (size_t j = 0; j < layer_count && !related; j++) if (ni_name_equal(name, layers[j].name)) related = layers[j].name;
            if (!related && ni_name_equal(name, base)) related = base;
            for (size_t j = 0; j < walked_count && !related && state == 1; j++) if (ni_name_equal(walked[j], name)) state = 2;
            if (related || state != 1) break;
            char **grown = realloc(walked, (walked_count + 1) * sizeof(*grown));
            if (!grown) { state = -1; break; }
            walked = grown; walked[walked_count++] = name; name = NULL;
            sh_decl_source text = {0}; int served = 0;
            int read = source->read(source->context, walked[walked_count - 1], &text, &served, NULL);
            sh_decl_node *tree = read > 0 ? sh_decl_tree_parse_ordered(text, error, sizeof(error)) : NULL;
            free((char *)text.text);
            if (read < 0) { state = -1; break; }
            if (!tree) { state = 0; break; }
            name = ni_parent(tree);
            sh_decl_tree_free(tree);
            if (name && !*name) { free(name); name = NULL; }
        }
        if (state == 2 && name) snprintf(at, sizeof(at), "%s", name);
        if (!state && walked_count) snprintf(at, sizeof(at), "%s", walked[walked_count - 1]);
        free(name);
        for (size_t j = 0; j < walked_count; j++) free(walked[j]);
        free(walked);
        if (state < 0) goto done;
        if (state != 1) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "the world's AI spawn pool lists '%s', whose ancestry %s '%s', so "
                "whether it descends from this definition is not known", pool->entries[i],
                state ? "returns to" : "is not readable at", at);
            result = 0; goto done;
        }
        if (related) {
            ni_refusef(site, SH_DECL_CARRIER_GAP, "the world's AI spawn pool lists '%s', which descends from '%s'; an AI "
                "takes the pooled settings of the first pool entry descending from its own declaration, which instances "
                "constructed under the native name would select", pool->entries[i], related);
            result = 0; goto done;
        }
    }
    /* Source agreement only pre-filters; the native evaluator accepts, with
     * the same substitution applied to both prepared texts when the
     * comparison needed it. */
    {
        ni_sigma_view sigma_view = {source, scan, ref_class};
        sh_native_eval_reduction sigma_reduction = {&sigma_view, ni_sigma_reduce};
        if (!ni_native_verdict(source, site, base, reference, candidate, substituted ? &sigma_reduction : NULL,
            substituted ? "the source model agrees under the plan's holder substitution" : "the source model agrees")) {
            result = 0; goto done;
        }
    }
    /* Renamed with this use, each operand keeps its match set only while
     * nothing else play reaches is, or may be, constructed as S; the census
     * holds every construction play reaches, and the other uses converting
     * into S are decided with this one (ni_couple). */
    if (operand_count) {
        if (ni_census_build(source, scan) < 0) goto done;
        const ni_loot_closure *census = scan->census;
        const ni_loot_node *other = ni_census_source(census, base);
        if (other)
            ni_refusef(site, SH_DECL_CARRIER_GAP, "%s %s is %s naming '%s'; renamed to native '%s' with this use, it "
                "would also match the instances play %s as '%s' (%s)", operands[0].owner, operands[0].reference,
                operand_what, site->from, base, other->context == NI_LOOT_LIVE ? "constructs" : "may construct", base,
                other->via);
        else if (*census->incomplete)
            ni_refusef(site, SH_DECL_CARRIER_GAP, "%s %s is %s naming '%s'; whether play constructs native '%s' "
                "elsewhere, which that operand renamed with this use would match, is not established: %s",
                operands[0].owner, operands[0].reference, operand_what, site->from, base, census->incomplete);
        if (other || *census->incomplete) { result = 0; goto done; }
        site->retargets = operands; site->retargeted = operand_count; operands = NULL; operand_count = 0;
    }
    if (!(site->to = _strdup(base))) goto done;
    site->converted = 1; site->limit = SH_DECL_CARRIER_EXACT; site->reason[0] = 0; site->conflict = 0;
    result = 1;
done:
    for (size_t i = 0; i < operand_count; i++) { free(operands[i].owner); free(operands[i].reference); free(operands[i].name); }
    free(operands);
    for (size_t i = 0; i < layer_count; i++) { free(layers[i].name); sh_decl_tree_free(layers[i].tree); }
    for (size_t i = 0; i < ancestry_count; i++) free(ancestry[i]);
    free(layers); free(ancestry); free(link); free(base); free(replaces); free(ref_class); free(cand_class);
    sh_decl_tree_free(reference); sh_decl_tree_free(candidate);
    sh_decl_tree_free(expanded_ref); sh_decl_tree_free(expanded_cand);
    return result;
}

/* A converted use whose operands are renamed is one member of its group:
 * every map construction of the identity those operands observe (P, compared
 * exactly). The group converts whole or not at all, since a member keeping
 * its package keeps P instances the renamed operand would stop matching.
 * Groups converting into one native S merge their instances there, which a
 * renamed operand would then match; that conflict is decided for the whole
 * set, never by visiting order: the plan converts the option freeing the most
 * typed sites (one coordinated group, or every group with no operand), ties
 * going to the lowest class key, and the others keep their packages as a plan
 * conflict, not a native limit. 1, -1 failure. */
static void ni_retain(sh_native_inline_site *site)
{
    free(site->to); site->to = NULL; site->converted = 0; site->limit = SH_DECL_CARRIER_GAP; site->conflict = 0;
    site->sigma = 0; site->carried = 0;
    for (size_t i = 0; i < site->retargeted; i++) {
        free(site->retargets[i].owner); free(site->retargets[i].reference); free(site->retargets[i].name);
    }
    free(site->retargets); site->retargets = NULL; site->retargeted = 0;
}
void sh_native_inline_withdraw(sh_native_inline_report *report, sh_decl_carrier_limit limit, const char *reason)
{
    if (!report) return;
    for (size_t k = 0; k < report->count; k++) {
        sh_native_inline_site *site = report->sites + k;
        if (!site->converted) continue;
        ni_retain(site); site->renamed = 0; site->limit = limit;
        snprintf(site->reason, sizeof(site->reason), "%s", reason ? reason : "");
    }
    for (size_t i = 0; i < report->substitution_count; i++) {
        free(report->substitutions[i].from); free(report->substitutions[i].to);
    }
    free(report->substitutions); report->substitutions = NULL; report->substitution_count = 0;
}
static int ni_converting(const sh_native_inline_site *site) { return site->owner && site->converted; }
static int ni_couple(const sh_native_inline_source *source, const ni_scan *scan, sh_native_inline_report *sites)
{
    for (size_t k = 0; k < sites->count; k++) {
        sh_native_inline_site *site = sites->sites + k;
        if (!ni_converting(site) || !site->retargeted) continue;
        char missing[700] = "";
        for (size_t i = 0; source->bound_use && !*missing; i++) {
            sh_native_inline_use use = {0};
            const char *name = NULL, *why = NULL;
            sh_decl_carrier_limit limit = SH_DECL_CARRIER_GAP;
            int listed = source->bound_use(source->context, i, &use, &name, &why, &limit);
            if (listed < 0) { snprintf(missing, sizeof(missing), "a typed use the map does not list"); break; }
            if (!listed) break;
            if (!name || !use.owner || !use.reference || !ni_name_equal(name, site->from)) continue;
            int failed = 0;
            const struct ni_consumer *consumer = ni_use_consumer(source, scan, &use, &failed);
            if (failed) return -1;
            if (!ni_consumer_constructs(consumer)) continue;
            int converted = 0;
            for (size_t j = 0; j < sites->count && !converted; j++) {
                const sh_native_inline_site *member = sites->sites + j;
                converted = ni_converting(member) && !strcmp(member->owner, use.owner) &&
                    !strcmp(member->reference, use.reference);
            }
            if (!converted) snprintf(missing, sizeof(missing), "%s %s", use.owner, use.reference);
        }
        if (!*missing) continue;
        char from[512], operand[700];
        snprintf(from, sizeof(from), "%s", site->from);
        snprintf(operand, sizeof(operand), "%s %s", site->retargets[0].owner, site->retargets[0].reference);
        for (size_t j = 0; j < sites->count; j++) {
            sh_native_inline_site *member = sites->sites + j;
            if (!ni_converting(member) || !ni_name_equal(member->from, from)) continue;
            ni_retain(member);
            ni_refusef(member, SH_DECL_CARRIER_GAP, "%s keeps its package and constructs '%s', whose instances %s "
                "matches exactly; every construction of '%s' converts with that operand's rename, or none does",
                missing, from, operand, from);
            member->conflict = 1;
        }
    }
    for (size_t k = 0; k < sites->count; k++) {
        if (!ni_converting(sites->sites + k)) continue;
        int earlier = 0;
        for (size_t j = 0; j < k && !earlier; j++)
            earlier = ni_converting(sites->sites + j) && ni_name_equal(sites->sites[j].to, sites->sites[k].to);
        if (earlier) continue;
        char base[512];
        snprintf(base, sizeof(base), "%s", sites->sites[k].to);
        const char *best = NULL, *plain = NULL;
        size_t best_weight = 0, plain_weight = 0, groups = 0;
        for (size_t j = 0; j < sites->count; j++) {
            const sh_native_inline_site *member = sites->sites + j;
            if (!ni_converting(member) || !ni_name_equal(member->to, base)) continue;
            int seen = 0;
            for (size_t i = 0; i < j && !seen; i++)
                seen = ni_converting(sites->sites + i) && ni_name_equal(sites->sites[i].to, base) &&
                    ni_name_equal(sites->sites[i].from, member->from);
            if (seen) continue;
            size_t weight = 0;
            for (size_t i = 0; i < sites->count; i++)
                weight += ni_converting(sites->sites + i) && ni_name_equal(sites->sites[i].from, member->from);
            groups++;
            /* A count observer carried by holder substitution names S once
             * the plan holds, as a renamed operand does. */
            if (member->retargeted || member->carried) {
                weight += member->retargeted + member->carried;
                if (!best || weight > best_weight || (weight == best_weight && _stricmp(member->from, best) < 0)) {
                    best = member->from; best_weight = weight;
                }
            } else {
                plain_weight += weight;
                if (!plain || _stricmp(member->from, plain) < 0) plain = member->from;
            }
        }
        if (groups < 2 || !best) continue;
        int plain_wins = plain && (plain_weight > best_weight || (plain_weight == best_weight && _stricmp(plain, best) < 0));
        char chosen[512];
        snprintf(chosen, sizeof(chosen), "%s", plain_wins ? plain : best);
        for (size_t j = 0; j < sites->count; j++) {
            sh_native_inline_site *member = sites->sites + j;
            if (!ni_converting(member) || !ni_name_equal(member->to, base)) continue;
            if (plain_wins ? !member->retargeted && !member->carried : ni_name_equal(member->from, chosen)) continue;
            ni_retain(member);
            ni_refusef(member, SH_DECL_CARRIER_GAP, "uses of '%s' and of '%s' would all construct native '%s', and an "
                "exact-identity operand renamed (or a count observer carried) with one of them would then match the "
                "other's instances; this plan converts the uses of '%s' (a plan conflict, not a native limit)",
                member->from, chosen, base, chosen);
            member->conflict = 1;
        }
    }
    return 1;
}
/* Map string values at sites of consumer rows of one kind naming name, in
 * entities whose class the row applies to (or whose class is not resolved). */
typedef struct ni_count_scan {
    const sh_native_inline_source *source; const ni_scan *scan; const char *name; ni_consumer_kind kind;
    size_t found; int failed;
} ni_count_scan;
static int ni_count_visit(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    ni_count_scan *count = context;
    const sh_json_path *root = path, *item = NULL;
    const struct ni_consumer *row = NULL;
    if (kind != SH_JSON_STRING || !path) return 1;
    while (root->parent) { item = root; root = root->parent; }
    if (!item || item->key || !root->key || root->key_length != 8 || memcmp(root->key, "entities", 8)) return 1;
    sh_package_provenance_site site;
    if (!sh_map_binding_site_at(path, &site)) return 1;
    for (size_t c = 0; !row && c < sizeof(ni_consumers) / sizeof(*ni_consumers); c++)
        if (!ni_consumers[c].family && ni_consumer_is(ni_consumers + c, count->kind) && ni_consumer_path(ni_consumers + c, site.reference))
            row = ni_consumers + c;
    char *value = row && length < SIZE_MAX ? malloc(length + 1) : NULL;
    size_t decoded = 0;
    if (row && !value) count->failed = 1;
    if (value && sh_native_json_decode_string(json, length, value, length + 1, &decoded) && strlen(value) == decoded &&
        ni_name_equal(value, count->name)) {
        char class_name[256];
        int known = ni_entity_class(count->source, count->scan, item->index, class_name, sizeof(class_name));
        if (known < 0) count->failed = 1;
        else if (!known || count->source->derives(count->source->context, class_name, row->owner_class) == 1) count->found++;
    }
    free(value); free((void *)site.owner); free((void *)site.reference);
    return !count->failed;
}
/* The output re-read for every group whose operands were renamed: each
 * member and each operand holds S at its own site, no exact-identity site or
 * construction still names P, and the map's exact-identity sites and
 * constructions naming S are exactly those operands and members. 1 verified
 * (also with no such group), 0 violated, -1 failure. */
static int ni_couple_verify(const sh_native_inline_source *source, const ni_scan *scan, const char *json, size_t length,
    const sh_native_inline_report *sites)
{
    for (size_t k = 0; k < sites->count; k++) {
        const sh_native_inline_site *site = sites->sites + k;
        if (!ni_converting(site) || !site->retargeted) continue;
        size_t members = 0;
        for (size_t j = 0; j < sites->count; j++) {
            const sh_native_inline_site *member = sites->sites + j;
            if (!ni_converting(member) || !ni_name_equal(member->to, site->to)) continue;
            if (!ni_name_equal(member->from, site->from)) return 0;
            sh_native_inline_use use = {member->owner, member->reference};
            if (!ni_site_holds(json, length, &use, member->entity, site->to)) return 0;
            members++;
        }
        for (size_t r = 0; r < site->retargeted; r++) {
            const struct sh_native_inline_retarget *operand = site->retargets + r;
            sh_native_inline_use use = {operand->owner, operand->reference};
            unsigned long long entity = 0; char *end = NULL;
            if (strncmp(operand->owner, "entity/", 7) || (entity = strtoull(operand->owner + 7, &end, 10), !end || *end) ||
                !ni_site_holds(json, length, &use, (size_t)entity, site->to)) return 0;
        }
        ni_count_scan old = {source, scan, site->from, NI_CONSUMER_IDENTITY}, renamed = {source, scan, site->to,
            NI_CONSUMER_IDENTITY}, built = {source, scan, site->to, NI_CONSUMER_CONSTRUCTS}, stale = {source, scan,
            site->from, NI_CONSUMER_CONSTRUCTS};
        if (!sh_native_json_visit_values(json, length, 256, ni_count_visit, &stale) || stale.failed || stale.found) {
            if (stale.failed) return -1;
            return stale.found ? 0 : -1;
        }
        if (!sh_native_json_visit_values(json, length, 256, ni_count_visit, &old) || old.failed ||
            !sh_native_json_visit_values(json, length, 256, ni_count_visit, &renamed) || renamed.failed ||
            !sh_native_json_visit_values(json, length, 256, ni_count_visit, &built) || built.failed) return -1;
        if (old.found || renamed.found != site->retargeted || built.found != members) return 0;
    }
    return 1;
}

/* ---- the plan's holder substitution (R-295 segment 4) ----
 * One plan decides every independent use at once. Its substitution maps
 * served entity definitions to their first native ancestors; a use whose own
 * typed references name mapped definitions is compared, and natively
 * evaluated, with those references renamed (ni_independent), and a count
 * observer of a mapped definition that sits in a mapped holder is carried
 * (ni_loot_counted). The plan starts from every candidate and only removes
 * (ni_sigma_settle): an entry leaves when a construction naming it keeps its
 * package, a placed occurrence reads it, a served declaration play
 * instantiates names it and is not itself an entry, or an entry's text names
 * it while the census is incomplete or shows another source of its image. A
 * served loot drop that builds a native drop's object under the plan is an
 * entry too, to that drop, while ni_drop_keeps holds.
 * Each of those only grows as entries leave, and a use's comparison only
 * fails as they leave (stock text names no served definition, so renaming
 * more of the target's references never makes a comparison fail), so the
 * removals reach the same largest stable plan in any order; cycles (H names
 * P, P names H) stay together while both hold. The coupling and conflict
 * selection of ni_couple runs inside each round and its removals feed the
 * same loop. */
static void ni_sigma_free(ni_scan *scan)
{
    for (size_t i = 0; i < scan->sigma_count; i++) { free(scan->sigma[i].from); free(scan->sigma[i].to); }
    free(scan->sigma); scan->sigma = NULL; scan->sigma_count = 0;
    for (size_t i = 0; i < scan->sigma_relied_count; i++) free(scan->sigma_relied[i]);
    free(scan->sigma_relied); scan->sigma_relied = NULL; scan->sigma_relied_count = 0;
    free(scan->round_remaining); scan->round_remaining = NULL;
}
/* A removal's concrete reason, kept across rounds so the final report can
 * say that a later refusal followed a plan conflict, not a native limit. */
/* A converted site whose decision used the plan's holder substitution. */
static int ni_relies(const sh_native_inline_site *site) { return site->converted && (site->sigma || site->carried); }
/* The key a site's reliance on the plan is kept under: an independent use's
 * own site, or a placed occurrence's inherit, as the binder lists it. */
static const char *ni_site_key(const sh_native_inline_site *site, char *owner, size_t capacity, const char **reference)
{
    if (site->owner) { *reference = site->reference; return site->owner; }
    snprintf(owner, capacity, "entity/%zu", site->entity);
    *reference = "/k:entityDef/k:inherit";
    return owner;
}
static void ni_sigma_withdraw(ni_scan *scan, const char *from, const char *why)
{
    size_t used = strlen(scan->sigma_withdrawn);
    if (used + 1 < sizeof(scan->sigma_withdrawn))
        snprintf(scan->sigma_withdrawn + used, sizeof(scan->sigma_withdrawn) - used, "%s'%s' (%s)", used ? "; " : "", from,
            *why ? why : "its removal condition failed");
}
/* One line of how the plan's loot drop entries were decided (report notes). */
static void ni_plan_note(ni_scan *scan, const char *format, ...)
{
    size_t used = strlen(scan->plan_notes);
    va_list args;
    if (used + 2 >= sizeof(scan->plan_notes)) return;
    va_start(args, format);
    int written = vsnprintf(scan->plan_notes + used, sizeof(scan->plan_notes) - used - 1, format, args);
    va_end(args);
    if (written < 0) { scan->plan_notes[used] = 0; return; }
    used = strlen(scan->plan_notes);
    scan->plan_notes[used] = '\n'; scan->plan_notes[used + 1] = 0;
}
static int ni_sigma_relied_has(const ni_scan *scan, const char *owner, const char *reference)
{
    size_t owner_length = owner ? strlen(owner) : 0;
    for (size_t i = 0; owner && reference && i < scan->sigma_relied_count; i++)
        if (!strncmp(scan->sigma_relied[i], owner, owner_length) && scan->sigma_relied[i][owner_length] == ' ' &&
            !strcmp(scan->sigma_relied[i] + owner_length + 1, reference)) return 1;
    return 0;
}
static int ni_sigma_relied_add(ni_scan *scan, const char *owner, const char *reference)
{
    if (!owner || !reference || ni_sigma_relied_has(scan, owner, reference)) return 1;
    size_t length = strlen(owner) + strlen(reference) + 2;
    char **grown = realloc(scan->sigma_relied, (scan->sigma_relied_count + 1) * sizeof(*grown)), *text = malloc(length);
    if (grown) scan->sigma_relied = grown;
    if (!grown || !text) { free(text); return 0; }
    snprintf(text, length, "%s %s", owner, reference);
    scan->sigma_relied[scan->sigma_relied_count++] = text;
    return 1;
}
/* The first native ancestor of a served definition, each link visited once:
 * 1 (*base malloc-owned), 0 none (unreadable, cyclic, no ancestor), -1
 * failure. */
static int ni_native_base(const sh_native_inline_source *source, const char *name, char **base)
{
    char **walked = NULL, *link = _strdup(name), error[160];
    size_t count = 0;
    int result = link ? 0 : -1;
    *base = NULL;
    while (link && *link) {
        int seen = 0;
        for (size_t i = 0; i < count && !seen; i++) seen = ni_name_equal(walked[i], link);
        if (seen) break;
        sh_decl_source text = {0}; int served = 0;
        int read = source->read(source->context, link, &text, &served, NULL);
        if (read < 0) { free((char *)text.text); result = -1; break; }
        if (!read) { free((char *)text.text); break; }
        if (!served) { free((char *)text.text); if (count) { *base = link; link = NULL; result = 1; } break; }
        sh_decl_node *tree = sh_decl_tree_parse_ordered(text, error, sizeof(error));
        free((char *)text.text);
        char **grown = realloc(walked, (count + 1) * sizeof(*grown));
        if (!grown) { sh_decl_tree_free(tree); result = -1; break; }
        walked = grown; walked[count++] = link;
        link = tree ? ni_parent(tree) : NULL;
        sh_decl_tree_free(tree);
    }
    free(link);
    for (size_t i = 0; i < count; i++) free(walked[i]);
    free(walked);
    return result;
}
/* Whether the document can hold a site at all: a typed use the source lists,
 * or a placed entity whose inherit is served (or cannot be read, which the
 * rounds then report). Without one every round decides nothing, and the
 * plan's entries and the loot census are read by nothing but its notes,
 * which a report without sites does not carry. */
static int ni_sites_possible(const sh_native_inline_source *source, const ni_scan *scan)
{
    sh_native_inline_use use = {0};
    const char *name = NULL, *kept = NULL;
    sh_decl_carrier_limit limit = SH_DECL_CARRIER_GAP;
    if (source->bound_use && source->bound_use(source->context, 0, &use, &name, &kept, &limit) != 0) return 1;
    for (size_t i = 0; i < scan->count; i++) {
        sh_json_object original = {0}, normalized = {0};
        char inherit[512]; size_t inherit_length = 0;
        const ni_span *span = scan->spans + i;
        if (!sh_native_json_parse_object(scan->json + span->offset, span->length, 64, &original)) return 1;
        const sh_json_object *definition = ni_v_upgrade(&original, source->version, &normalized) == 1 ? &normalized : &original;
        const char *value = sh_json_object_get(definition, "inherit");
        sh_decl_source text = {0}; int served = 0, read = 0;
        if (value && sh_native_json_decode_string(value, strlen(value), inherit, sizeof(inherit), &inherit_length) &&
            inherit_length < sizeof(inherit) && strlen(inherit) == inherit_length && inherit_length)
            read = source->read(source->context, inherit, &text, &served, NULL);
        free((char *)text.text);
        sh_json_object_free(&original); sh_json_object_free(&normalized);
        if (read < 0 || (read && served)) return 1;
    }
    return 0;
}
/* The plan's start: every served definition a traced map construction
 * names, to its first native ancestor. 1, -1 failure. */
static int ni_sigma_seed(const sh_native_inline_source *source, ni_scan *scan)
{
    for (size_t u = 0; source->bound_use; u++) {
        sh_native_inline_use use = {0};
        const char *name = NULL, *kept = NULL;
        sh_decl_carrier_limit limit = SH_DECL_CARRIER_GAP;
        int listed = source->bound_use(source->context, u, &use, &name, &kept, &limit), failed = 0;
        if (listed <= 0) break;
        if (!name || !*name || ni_sigma_image(scan, name)) continue;
        const struct ni_consumer *consumer = ni_use_consumer(source, scan, &use, &failed);
        if (failed) return -1;
        if (!ni_consumer_constructs(consumer)) continue;
        char *base = NULL;
        int found = ni_native_base(source, name, &base);
        if (found < 0) return -1;
        if (!found) continue;
        ni_sigma *grown = realloc(scan->sigma, (scan->sigma_count + 1) * sizeof(*grown));
        char *from = grown ? _strdup(name) : NULL;
        if (grown) scan->sigma = grown;
        if (!from) { free(base); return -1; }
        scan->sigma[scan->sigma_count++] = (ni_sigma){from, base, 0};
    }
    /* Served loot drops play reaches that build a native drop's object under
     * the plan, to that drop, when the product serves a loot drop at all and
     * some site could use an entry (the census reads the whole map). */
    if (!ni_sites_possible(source, scan)) return 1;
    int drops = 0;
    for (size_t i = 0; source->served_decl && !drops; i++) {
        const char *family = NULL, *name = NULL;
        int listed = source->served_decl(source->context, i, &family, &name);
        if (listed < 0) {   /* not known: no drop entries; their uses keep their packages */
            ni_plan_note(scan, "no served loot drop is a plan entry: the product's served declarations are not all known");
            return 1;
        }
        if (!listed) break;
        drops = family && !_stricmp(family, "lootdrop");
    }
    /* A census that cannot be built seeds no drop entry; a use that needs it
     * later meets the same failure there, as it did before drops were seeded. */
    if (!drops) return 1;
    if (ni_census_build(source, scan) < 0) {
        ni_plan_note(scan, "no served loot drop is a plan entry: the loot census could not be built (%s)",
            *scan->census->failure ? scan->census->failure : "allocation failed");
        return 1;
    }
    const ni_loot_closure *c = scan->census;
    if (*c->incomplete) {
        size_t listed = 1;
        ni_plan_note(scan, "the loot census is incomplete: %s", c->incomplete);
        for (const char *line = c->more, *end; *line && (end = strchr(line, '\n')); line = end + 1, listed++)
            ni_plan_note(scan, "the loot census is also incomplete: %.*s", (int)(end - line), line);
        if (c->incomplete_count > listed)
            ni_plan_note(scan, "the loot census is incomplete for %zu more reasons", c->incomplete_count - listed);
    }
    size_t reached = 0;
    for (size_t i = 0; i < c->count; i++) {
        if (_stricmp(c->nodes[i].family, "lootdrop") || ni_drop_image(scan, c->nodes[i].name)) continue;
        char native[512] = "", why[600] = "";
        int equal = ni_lootdrop_native(source, scan, c->nodes[i].name, NULL, NULL, native, sizeof(native), why, sizeof(why));
        if (equal < 0) return -1;
        if (equal == 2 && !strcmp(why, ni_drop_unserved)) continue;   /* a native drop */
        reached++;
        if (equal != 1) {
            ni_plan_note(scan, "served loot drop '%s' (%.200s) is no plan entry: it %s", c->nodes[i].name, c->nodes[i].via, why);
            continue;
        }
        ni_sigma *grown = realloc(scan->sigma, (scan->sigma_count + 1) * sizeof(*grown));
        char *from = grown ? _strdup(c->nodes[i].name) : NULL, *to = from ? _strdup(native) : NULL;
        if (grown) scan->sigma = grown;
        if (!to) { free(from); return -1; }
        scan->sigma[scan->sigma_count++] = (ni_sigma){from, to, 1};
        ni_plan_note(scan, "served loot drop '%s' (%.200s) is a plan entry for native '%s'", c->nodes[i].name,
            c->nodes[i].via, native);
    }
    if (!reached) ni_plan_note(scan, "play reaches no served loot drop");
    return 1;
}
/* Whether a served loot drop entry stays: it still builds its native drop's
 * object under the plan; the census is complete; the native drop is reached
 * nowhere else in play and is no other entry's image, so naming it in the
 * served drop's place joins no two loot drop identities a list could hold; no
 * map site names the served drop (the carrier renames none there); and every
 * reference play follows to it is a live entity's reflected pointer the loot
 * component evaluates (no event or by-name form, no unclassified holder), from
 * a served entity definition the plan converts or a placed occurrence that
 * converts under the plan without naming it in its own map state. A holder
 * the plan converts names the native drop in its comparison and its native
 * evaluation (ni_sigma_site), so nothing play constructs afterwards reads the
 * served drop. 1 stays, 0 leaves (why set), -1 failure. */
static int ni_drop_keeps(const sh_native_inline_source *source, const ni_scan *scan, const ni_sigma *entry,
    char *why, size_t capacity)
{
    char native[512] = "", reason[600] = "";
    int equal = ni_lootdrop_native(source, scan, entry->from, NULL, NULL, native, sizeof(native), reason, sizeof(reason));
    if (equal < 0) return -1;
    if (equal != 1 || !ni_name_equal(native, entry->to)) {
        snprintf(why, capacity, "it no longer builds native '%s''s object under the plan%s%s", entry->to,
            *reason ? ": it " : "", reason);
        return 0;
    }
    if (ni_census_build(source, scan) < 0) return -1;
    const ni_loot_closure *c = scan->census;
    if (*c->incomplete) { snprintf(why, capacity, "the map census is incomplete: %s", c->incomplete); return 0; }
    for (size_t i = 0; i < c->count; i++)
        if (!_stricmp(c->nodes[i].family, "lootdrop") && ni_name_equal(c->nodes[i].name, entry->to)) {
            snprintf(why, capacity, "its image '%s' is reached elsewhere in play (%s)", entry->to, c->nodes[i].via);
            return 0;
        }
    for (size_t k = 0; k < scan->sigma_count; k++)
        if (scan->sigma[k].drop && scan->sigma + k != entry && ni_name_equal(scan->sigma[k].to, entry->to)) {
            snprintf(why, capacity, "served loot drop '%s' has the same image", scan->sigma[k].from);
            return 0;
        }
    for (size_t i = 0; source->typed_name; i++) {
        const char *family = NULL, *name = NULL;
        int listed = source->typed_name(source->context, i, &family, &name);
        if (listed < 0) { snprintf(why, capacity, "the map's typed declaration names are not all known"); return 0; }
        if (!listed) break;
        if (family && name && !_stricmp(family, "lootdrop") && ni_name_equal(name, entry->from)) {
            snprintf(why, capacity, "the map names it at a typed site, which the carrier does not rename");
            return 0;
        }
    }
    size_t edges = 0;
    for (size_t i = 0; i < c->ref_count; i++) {
        const ni_loot_ref *ref = c->refs + i;
        if (!ni_name_equal(ref->drop, entry->from)) continue;
        edges++;
        if (ref->context != NI_LOOT_EVALUATED) {
            snprintf(why, capacity, "%s '%s' names it at %s, where no classified holder evaluates it", ref->family,
                ref->holder, ref->path);
            return 0;
        }
        if (!_stricmp(ref->family, "entitydef") && ni_sigma_image(scan, ref->holder)) continue;
        if (!_stricmp(ref->family, "entity") && ni_placed_carries(scan, ref->holder, entry->from)) continue;
        snprintf(why, capacity, "%s '%s' names it at %s and does not convert with the plan", ref->family, ref->holder,
            ref->path);
        return 0;
    }
    if (!edges) { snprintf(why, capacity, "play reaches it through no classified holder"); return 0; }
    return 1;
}
/* One removal round (see above): 1 some entry left, 0 the plan is stable,
 * -1 failure. A site entry leaving names its reason nowhere: the uses it
 * carried are decided again under the smaller plan and state their own. */
static int ni_sigma_shrink(const sh_native_inline_source *source, ni_scan *scan, const sh_native_inline_report *sites)
{
    ni_served *index = scan->served;
    int collected = index ? ni_served_collect(source, scan, index) : 2;
    if (collected < 0) return -1;
    char *stays = calloc(scan->sigma_count ? scan->sigma_count : 1, 1), reason[300];
    if (!stays) return -1;
    int result = 0;
    for (size_t k = 0; k < scan->sigma_count && result >= 0; k++) {
        const ni_sigma *entry = scan->sigma + k;
        int keep = collected == 1, named = 0;
        char why[700] = "";
        if (!keep) snprintf(why, sizeof(why), "the served declarations could not be collected");
        /* Every traced construction naming it converted to its image. */
        for (size_t u = 0; keep && !entry->drop && source->bound_use; u++) {
            sh_native_inline_use use = {0};
            const char *name = NULL, *kept = NULL;
            sh_decl_carrier_limit limit = SH_DECL_CARRIER_GAP;
            int listed = source->bound_use(source->context, u, &use, &name, &kept, &limit), failed = 0;
            if (listed < 0) { keep = 0; snprintf(why, sizeof(why), "a bound use could not be listed"); break; }
            if (!listed) break;
            if (!name || !use.owner || !use.reference || !ni_name_equal(name, entry->from)) continue;
            const struct ni_consumer *consumer = ni_use_consumer(source, scan, &use, &failed);
            if (failed) { result = -1; break; }
            if (!ni_consumer_constructs(consumer)) continue;
            int converted = 0;
            for (size_t j = 0; j < sites->count && !converted; j++) {
                const sh_native_inline_site *site = sites->sites + j;
                converted = ni_converting(site) && !strcmp(site->owner, use.owner) &&
                    !strcmp(site->reference, use.reference) && ni_name_equal(site->to, entry->to);
            }
            keep = converted;
            if (!keep) snprintf(why, sizeof(why), "%s %s constructs it and does not convert to '%s'", use.owner, use.reference,
                entry->to);
        }
        /* No placed occurrence still reads it: one descending from it either
         * converts to its image, whose one inline layer no longer reads the
         * entry, or keeps its package and the entry's definition with it. */
        for (size_t j = 0; keep && !entry->drop && result >= 0 && j < sites->count; j++) {
            const sh_native_inline_site *site = sites->sites + j;
            int descends = 0, mentions = 0;
            if (site->owner) continue;
            int walked = ni_chain_names(source, site->from, entry->from, &descends, &mentions, reason, sizeof(reason));
            if (walked < 0) result = -1;
            else if (!walked) {
                keep = 0; snprintf(why, sizeof(why), "the chain of placed occurrence '%s' could not be read", site->from);
            } else if (descends && !(site->converted && site->to && ni_name_equal(site->to, entry->to))) {
                keep = 0;
                if (site->converted)
                    snprintf(why, sizeof(why), "placed occurrence %zu of '%s' descends from it and converts to '%s'",
                        site->entity, site->from, site->to);
                else snprintf(why, sizeof(why), "placed occurrence %zu of '%s' descends from it and keeps its package",
                    site->entity, site->from);
            }
        }
        /* Every served declaration play instantiates that names it, or
         * inherits it, is itself an entry; an entry's own text naming it
         * needs a complete census with no other source of its image. */
        for (size_t i = 0; keep && result >= 0 && i < index->count; i++) {
            const ni_served_decl *decl = index->decls + i;
            int entitydef = !_stricmp(decl->family, "entitydef"), drop = !_stricmp(decl->family, "lootdrop");
            if (!decl->live || ((entry->drop ? drop : entitydef) && ni_name_equal(decl->name, entry->from))) continue;
            if (!(entitydef && !entry->drop && decl->parent && ni_name_equal(decl->parent, entry->from)) &&
                !ni_tree_mentions(decl->tree->children, decl->inherit, entry->from)) continue;
            if ((entitydef && (ni_sigma_image(scan, decl->name) || ni_placed_only(scan, i))) ||
                (drop && ni_drop_image(scan, decl->name))) named = 1;
            else {
                keep = 0;
                snprintf(why, sizeof(why), "served %s declaration '%s' names or inherits it and is not itself substituted",
                    decl->family, decl->name);
            }
        }
        if (keep && entry->drop && result >= 0) {
            int kept = ni_drop_keeps(source, scan, entry, why, sizeof(why));
            if (kept < 0) result = -1; else keep = kept;
        } else if (keep && named && result >= 0) {
            if (ni_census_build(source, scan) < 0) result = -1;
            else if (*scan->census->incomplete) {
                keep = 0; snprintf(why, sizeof(why), "the map census is incomplete: %s", scan->census->incomplete);
            } else if (ni_census_source(scan->census, entry->to)) {
                keep = 0; snprintf(why, sizeof(why), "its image '%s' has another source in the map", entry->to);
            }
        }
        if (!keep && result >= 0) ni_sigma_withdraw(scan, entry->from, why);
        stays[k] = (char)keep;
    }
    if (result >= 0) {
        size_t at = 0;
        for (size_t k = 0; k < scan->sigma_count; k++) {
            if (stays[k]) { scan->sigma[at++] = scan->sigma[k]; continue; }
            free(scan->sigma[k].from); free(scan->sigma[k].to); result = 1;
        }
        scan->sigma_count = at;
    }
    free(stays);
    return result;
}
/* An independent site cleared for the next round. */
static void ni_site_clear(sh_native_inline_site *site)
{
    for (size_t j = 0; j < site->retargeted; j++) {
        free(site->retargets[j].owner); free(site->retargets[j].reference); free(site->retargets[j].name);
    }
    free(site->retargets); free(site->from); free(site->to); free(site->owner); free(site->reference);
    memset(site, 0, sizeof(*site));
}
/* The census of a document's placed entities, as play constructs them, with
 * the declarations they reach (typed map names other than entity state are
 * left out on both sides of the comparison below). 1, -1 failure. */
static int ni_census_of(const sh_native_inline_source *source, const char *json, size_t length, ni_loot_closure *out)
{
    sh_native_inline_source view = *source;
    ni_scan scan = {0};
    ni_served served = {0};
    int result = -1;
    view.typed_name = NULL;
    scan.json = json; scan.json_length = length; scan.served = &served; scan.census = out;
    if (sh_native_json_visit_values(json, length, 256, ni_visit, &scan) && !scan.failed) {
        if (scan.count > 1) qsort(scan.spans, scan.count, sizeof(*scan.spans), ni_span_order);
        result = ni_census_build(&view, &scan);
    }
    out->source = NULL;
    free(scan.spans); free(scan.uniques); free(scan.keys.rows); free(scan.values.rows);
    for (size_t i = 0; i < scan.module_count; i++) { free(scan.modules[i].name); free(scan.modules[i].declaration); }
    free(scan.modules); ni_served_free(&served);
    return result;
}
static int ni_sigma_related(const ni_scan *scan, const char *name)
{
    if (ni_sigma_image(scan, name)) return 1;
    for (size_t i = 0; name && i < scan->sigma_count; i++)
        if (!scan->sigma[i].drop && ni_name_equal(scan->sigma[i].to, name)) return 1;
    return 0;
}
static int ni_drop_related(const ni_scan *scan, const char *name)
{
    if (ni_drop_image(scan, name)) return 1;
    for (size_t i = 0; name && i < scan->sigma_count; i++)
        if (scan->sigma[i].drop && ni_name_equal(scan->sigma[i].to, name)) return 1;
    return 0;
}
/* The plan's census comparison: the evaluated count observers of before
 * involving the substitution are exactly after's, each with the substitution
 * applied (an observer held by a substituted loot drop is held by its native
 * drop), and after neither reaches nor counts a substituted definition, nor
 * reaches a substituted loot drop. 1 equal, 0 different (reason set when
 * given), -1 failure. */
typedef struct ni_hint_identity { const char *from, *to; unsigned int kinds; } ni_hint_identity;
static int ni_hint_name_order(const char *a, const char *b)
{
    if (*a == '/' && a[1] != '/') a++;
    if (*b == '/' && b[1] != '/') b++;
    while (*a && *b) {
        unsigned char x = (unsigned char)ni_name_char(*a++), y = (unsigned char)ni_name_char(*b++);
        if (x != y) return x < y ? -1 : 1;
    }
    return *a ? 1 : *b ? -1 : 0;
}
static int ni_hint_identity_order(const void *left, const void *right)
{
    const ni_hint_identity *a = left, *b = right;
    int order = ni_hint_name_order(a->to, b->to); return order ? order : ni_hint_name_order(a->from, b->from);
}
static int ni_hint_injective(const ni_scan *scan, const ni_loot_closure *before, char *reason, size_t capacity)
{
    size_t count = 0;
    for (size_t i = 0; i < before->hint_count; i++) {
        if (before->hints[i].count > SIZE_MAX - count) return -1;
        count += before->hints[i].count;
    }
    if (count > SIZE_MAX / sizeof(ni_hint_identity)) return -1;
    ni_hint_identity *index = count ? malloc(count * sizeof(*index)) : NULL;
    if (count && !index) return -1;
    size_t at = 0;
    for (size_t i = 0; i < before->hint_count; i++)
        for (size_t j = 0; j < before->hints[i].count; j++) {
            const char *from = before->hints[i].choices[j], *to = ni_sigma_image(scan, from);
            index[at++] = (ni_hint_identity){from, to ? to : from, before->hints[i].kinds};
        }
    if (count > 1) qsort(index, count, sizeof(*index), ni_hint_identity_order);
    unsigned int used = 0; const char *target = NULL;
    for (size_t i = 0; i < count;) {
        if (!target || !ni_name_equal(target, index[i].to)) { target = index[i].to; used = 0; }
        size_t j = i + 1; unsigned int kinds = index[i].kinds;
        while (j < count && ni_name_equal(index[i].to, index[j].to) && ni_name_equal(index[i].from, index[j].from))
            kinds |= index[j++].kinds;
        if (used & kinds) {
            if (reason && capacity) snprintf(reason, capacity,
                "distinct choices collapse to '%s' in one module hint uniqueness batch", target);
            free(index); return 0;
        }
        used |= kinds; i = j;
    }
    free(index); return 1;
}
static int ni_hint_match(const ni_scan *scan, const ni_loot_closure *before, const ni_loot_closure *after,
    char *reason, size_t capacity)
{
    if (before->hint_incomplete || after->hint_incomplete) {
        if (reason && capacity) snprintf(reason, capacity, "the actual module hint cohort is incomplete: %s",
            before->hint_incomplete ? before->incomplete : after->incomplete);
        return 0;
    }
    int injective = ni_hint_injective(scan, before, reason, capacity);
    if (injective != 1) return injective;
    if (before->hint_source_count != after->hint_source_count || before->hint_count != after->hint_count) goto changed;
    for (size_t i = 0; i < before->hint_source_count; i++) {
        const ni_hint_witness *a = before->hint_sources + i; int found = 0;
        for (size_t j = 0; j < after->hint_source_count; j++) {
            const ni_hint_witness *b = after->hint_sources + j;
            if (!strcmp(a->family, b->family) && !strcmp(a->name, b->name) && !strcmp(a->text, b->text)) { found = 1; break; }
        }
        if (!found) goto changed; /* no invented source rewrite for native selectors */
    }
    for (size_t i = 0; i < before->hint_count; i++) {
        const ni_hint_record *a = before->hints + i, *b = after->hints + i;
        if (a->module != b->module || a->record != b->record || a->category != b->category || a->kinds != b->kinds ||
            strcmp(a->parent, b->parent) || strcmp(a->prepared, b->prepared) || a->count != b->count) goto changed;
        for (size_t j = 0; j < a->count; j++) {
            const char *image = ni_sigma_image(scan, a->choices[j]);
            if (!ni_name_equal(image ? image : a->choices[j], b->choices[j])) goto changed;

        }
    }
    return 1;
changed:
    if (reason && capacity) snprintf(reason, capacity,
        "the derived source changes the actual module hint selection, record order or ordered choice multiplicity");
    return 0;
}
static int ni_census_match(const ni_scan *scan, const ni_loot_closure *before, const ni_loot_closure *after,
    char *reason, size_t capacity)
{
    int hints = ni_hint_match(scan, before, after, reason, capacity);
    if (hints != 1) return hints;
    for (size_t i = 0; i < after->count; i++)
        if ((!_stricmp(after->nodes[i].family, "entitydef") && ni_sigma_image(scan, after->nodes[i].name)) ||
            (!_stricmp(after->nodes[i].family, "lootdrop") && ni_drop_image(scan, after->nodes[i].name))) {
            if (reason && capacity)
                snprintf(reason, capacity, "play still reaches %s '%s', which the plan substitutes", after->nodes[i].family,
                    after->nodes[i].name);
            return 0;
        }
    char *used = calloc(after->seen_count ? after->seen_count : 1, 1);
    if (!used) return -1;
    size_t expected = 0, matched = 0, related = 0;
    int result = 1;
    for (size_t i = 0; i < before->seen_count; i++) {
        const ni_loot_seen *seen = before->seen + i;
        int entity = !_stricmp(seen->family, "entitydef"), drop = !_stricmp(seen->family, "lootdrop");
        if (!seen->evaluated || (!ni_sigma_related(scan, seen->name) && !(entity && ni_sigma_related(scan, seen->holder)) &&
            !(drop && ni_drop_related(scan, seen->holder))))
            continue;
        const char *name = ni_sigma_image(scan, seen->name), *holder = entity ? ni_sigma_image(scan, seen->holder) :
            drop ? ni_drop_image(scan, seen->holder) : NULL;
        if (!name) name = seen->name;
        if (!holder) holder = seen->holder;
        expected++;
        int found = 0;
        for (size_t j = 0; j < after->seen_count && !found; j++) {
            const ni_loot_seen *now = after->seen + j;
            if (used[j] || !now->evaluated || _stricmp(now->family, seen->family) || !ni_name_equal(now->name, name) ||
                !ni_name_equal(now->holder, holder) || strcmp(now->path, seen->path)) continue;
            used[j] = 1; matched++; found = 1;
        }
        if (!found && result == 1) {
            result = 0;
            if (reason && capacity)
                snprintf(reason, capacity, "the count observer of '%s' in '%s' at %s is not observed as one of '%s' in '%s'",
                    seen->name, seen->holder ? seen->holder : "?", seen->path, name, holder ? holder : "?");
        }
    }
    for (size_t j = 0; j < after->seen_count; j++) {
        const ni_loot_seen *now = after->seen + j;
        if (now->evaluated && (ni_sigma_related(scan, now->name) ||
            (!_stricmp(now->family, "entitydef") && ni_sigma_related(scan, now->holder)) ||
            (!_stricmp(now->family, "lootdrop") && ni_drop_related(scan, now->holder)))) related++;
        if (now->evaluated && ni_sigma_image(scan, now->name) && result == 1) {
            result = 0;
            if (reason && capacity)
                snprintf(reason, capacity, "'%s' at %s still counts '%s', which the plan substitutes",
                    now->holder ? now->holder : "?", now->path, now->name);
        }
    }
    if (result == 1 && (matched != expected || related != expected)) {
        result = 0;
        if (reason && capacity)
            snprintf(reason, capacity, "%zu count observers involve the plan where %zu were expected", related, expected);
    }
    free(used);
    return result;
}
/* The output re-read under the final plan, whenever a converted use depends
 * on it: no map site still constructs or exactly names a mapped definition,
 * every converted construction of one names its image, the output's census
 * reaches no mapped definition, and its evaluated count observers involving
 * the plan are exactly the input's with the plan applied (each counts the
 * images of what it counted). 1 verified, 0 violated, -1 failure. */
static int ni_sigma_verify(const sh_native_inline_source *source, const ni_scan *scan, const char *input,
    size_t input_length, const char *json, size_t length, const sh_native_inline_report *sites)
{
    int depends = 0;
    for (size_t k = 0; k < sites->count; k++) if (ni_relies(sites->sites + k)) depends = 1;
    if (!depends) return 1;
    for (size_t k = 0; k < sites->count; k++) {
        const sh_native_inline_site *site = sites->sites + k;
        if (!site->owner && ni_relies(site)) {
            /* A placed occurrence converted under the plan inherits its base. */
            const char *reference = NULL; char owner[32];
            sh_native_inline_use use = {ni_site_key(site, owner, sizeof(owner), &reference), NULL};
            use.reference = reference;
            if (!ni_site_holds(json, length, &use, site->entity, site->to)) return 0;
            continue;
        }
        const char *image = site->owner ? ni_sigma_image(scan, site->from) : NULL;
        if (site->carried && !image) return 0;
        if (!ni_converting(site) || !image) continue;
        sh_native_inline_use use = {site->owner, site->reference};
        if (!ni_name_equal(site->to, image) || !ni_site_holds(json, length, &use, site->entity, image)) return 0;
    }
    for (size_t i = 0; i < scan->sigma_count; i++) {
        ni_count_scan built = {source, scan, scan->sigma[i].from, NI_CONSUMER_CONSTRUCTS},
            exact = {source, scan, scan->sigma[i].from, NI_CONSUMER_IDENTITY};
        if (!sh_native_json_visit_values(json, length, 256, ni_count_visit, &built) || built.failed ||
            !sh_native_json_visit_values(json, length, 256, ni_count_visit, &exact) || exact.failed) return -1;
        if (built.found || exact.found) return 0;
    }
    ni_loot_closure before = {0}, after = {0};
    int result = ni_census_of(source, input, input_length, &before) < 0 || ni_census_of(source, json, length, &after) < 0 ?
        -1 : 1;
    if (result == 1) result = ni_census_match(scan, &before, &after, NULL, 0);
    ni_census_free(&before); ni_census_free(&after);
    return result;
}
int sh_native_inline_observations(const sh_native_inline_source *source, const char *input, size_t input_length,
    const sh_native_inline_report *plan, const sh_native_inline_source *derived_source, const char *derived,
    size_t derived_length, char *reason, size_t capacity)
{
    if (reason && capacity) *reason = 0;
    if (!plan || !plan->substitution_count) return 1;
    if (!source || !derived_source || !input || !derived) return -1;
    ni_sigma *sigma = calloc(plan->substitution_count, sizeof(*sigma));
    if (!sigma) return -1;
    for (size_t i = 0; i < plan->substitution_count; i++) {
        sigma[i].from = plan->substitutions[i].from; sigma[i].to = plan->substitutions[i].to;
        sigma[i].drop = plan->substitutions[i].drop;
    }
    ni_scan scan = {0};
    scan.sigma = sigma; scan.sigma_count = plan->substitution_count;
    ni_loot_closure before = {0}, after = {0};
    int result = ni_census_of(source, input, input_length, &before) < 0 ||
        ni_census_of(derived_source, derived, derived_length, &after) < 0 ? -1 : 1;
    if (result == 1) result = ni_census_match(&scan, &before, &after, reason, capacity);
    ni_census_free(&before); ni_census_free(&after); free(sigma);
    return result;
}
char *sh_native_inline_document(const char *json, size_t length,
    const sh_native_inline_source *source, size_t *output_length,
    sh_native_inline_report *report, char *error, size_t capacity)
{
    ni_scan scan = {0};
    ni_pool pool = {0};
    ni_served served = {0};
    ni_loot_closure census = {0};
    sh_native_inline_report sites = {0};
    char **replacements = NULL;
    ni_text out = {0};
    int ok = 0;
    if (error && capacity) *error = 0;
    if (output_length) *output_length = 0;
    if (report) memset(report, 0, sizeof(*report));
    if (!json || !source || !source->read || !source->derives || !source->schema || !output_length || !report) {
        if (error && capacity) snprintf(error, capacity, "invalid native representation request");
        return NULL;
    }
    scan.json = json; scan.json_length = length;
    scan.served = &served; scan.census = &census;
    if (!sh_native_json_visit_values(json, length, 256, ni_visit, &scan) || scan.failed) {
        if (error && capacity) snprintf(error, capacity, "map is not native JSON");
        goto done;
    }
    if (scan.count > 1) qsort(scan.spans, scan.count, sizeof(*scan.spans), ni_span_order);
    g_ni_stats.documents++; g_ni_stats.placed += scan.count;
    replacements = calloc(scan.count ? scan.count : 1, sizeof(*replacements));
    sites.sites = calloc(scan.count ? scan.count : 1, sizeof(*sites.sites));
    if (!replacements || !sites.sites) goto done;
    /* Each round decides the placed occurrences, then the independent typed
     * uses of served definitions, each at its own site, all under one plan
     * whose holder substitution only shrinks until it is stable
     * (ni_sigma_shrink): at most one round per starting entry. A use whose
     * site holds another spelling is not one. */
    if (ni_sigma_seed(source, &scan) < 0) {
        if (error && capacity) snprintf(error, capacity, "the plan's holder substitution could not be started");
        goto done;
    }
    size_t placed = 0;
    for (size_t round = 0;; round++) {
        while (sites.count) ni_site_clear(sites.sites + --sites.count);
        for (size_t i = 0; i < scan.count; i++) { free(replacements[i]); replacements[i] = NULL; }
        for (size_t i = 0; i < scan.count; i++) {
            sh_json_object original = {0}, normalized = {0};
            const sh_json_object *definition;
            ni_identity identity = {0};
            char inherit[512]; size_t inherit_length = 0;
            const ni_span *span = scan.spans + i;
            if (!sh_native_json_parse_object(json + span->offset, span->length, 64, &original)) goto done;
            int upgraded = ni_v_upgrade(&original, source->version, &normalized);
            /* Refused normalization must look up the original identity and keep its
             * complete package use. No partial derived state is ever published. */
            definition = upgraded == 1 ? &normalized : &original;
            const char *value = sh_json_object_get(definition, "inherit");
            sh_decl_source text = {0}; int served = 0, read = 0;
            if (value && sh_native_json_decode_string(value, strlen(value), inherit, sizeof(inherit), &inherit_length) &&
                inherit_length < sizeof(inherit) && strlen(inherit) == inherit_length && inherit_length)
                read = source->read(source->context, inherit, &text, &served, NULL);
            free((char *)text.text);
            if (read < 0) { sh_json_object_free(&original); sh_json_object_free(&normalized); goto done; }
            if (read && served) {
                sh_native_inline_site *site = sites.sites + sites.count++;
                sh_decl_carrier_limit limit = SH_DECL_CARRIER_GAP;
                memset(site, 0, sizeof(*site));
                site->entity = span->entity;
                const char *kept = source->keep ? source->keep(source->context, span->entity, inherit, &limit) : NULL;
                if (!(site->from = _strdup(inherit))) {
                    sh_json_object_free(&original); sh_json_object_free(&normalized); goto done;
                }
                /* The world is resolved once, when the first served use needs it. */
                if (upgraded == 1 && !kept && !pool.resolved && ni_pool_resolve(source, &pool) < 0) {
                    sh_json_object_free(&original); sh_json_object_free(&normalized);
                    if (error && capacity) snprintf(error, capacity, "the world's AI spawn pool could not be read");
                    goto done;
                }
                if (upgraded != 1) ni_refuse(site, SH_DECL_CARRIER_GAP, "native entity-version conversion is unavailable%s", NULL);
                else if (kept) ni_refuse(site, limit, "%s", kept);
                else if (ni_site(source, &pool, &scan, inherit, ni_identity_of(&scan, span->entity, &identity), definition, site,
                    &replacements[i]) < 0) {
                    sh_json_object_free(&original); sh_json_object_free(&normalized);
                    if (error && capacity) snprintf(error, capacity, "native representation of entity %zu failed", span->entity);
                    goto done;
                }
            }
            sh_json_object_free(&original); sh_json_object_free(&normalized);
        }
        placed = sites.count;
        scan.round = &sites; scan.round_placed = placed;
        if (ni_served_remaining(source, &scan, replacements) < 0) {
            if (error && capacity) snprintf(error, capacity, "the reads left by converted placed copies cannot be established");
            goto done;
        }
        for (size_t u = 0; source->bound_use; u++) {
            sh_native_inline_use use = {0};
            const char *name = NULL, *kept = NULL;
            sh_decl_carrier_limit limit = SH_DECL_CARRIER_GAP;
            unsigned long long entity = 0; char *end = NULL;
            int listed = source->bound_use(source->context, u, &use, &name, &kept, &limit);
            if (listed <= 0) break;
            if (!name || !*name || !use.owner || !use.reference || strncmp(use.owner, "entity/", 7) ||
                use.owner[7] < '0' || use.owner[7] > '9' || (entity = strtoull(use.owner + 7, &end, 10), !end || *end) ||
                !ni_site_holds(json, length, &use, (size_t)entity, name)) continue;
            sh_native_inline_site *grown = realloc(sites.sites, (sites.count + 1) * sizeof(*grown));
            if (!grown) goto done;
            sites.sites = grown;
            sh_native_inline_site *site = sites.sites + sites.count;
            memset(site, 0, sizeof(*site));
            site->entity = (size_t)entity; site->limit = SH_DECL_CARRIER_GAP;
            if (!(site->from = _strdup(name)) || !(site->owner = _strdup(use.owner)) || !(site->reference = _strdup(use.reference))) {
                ni_site_clear(site); goto done;
            }
            int decided = ni_independent(source, &pool, &scan, &use, kept, limit, site);
            /* A use that converted under an entry a later round withdrew stays
             * reported, so the withdrawal's reason is not lost. */
            if (decided == 2 && *scan.sigma_withdrawn && ni_sigma_relied_has(&scan, use.owner, use.reference)) {
                ni_retain(site);
                snprintf(site->reason, sizeof(site->reason), "compared without the holder substitution it relied on, '%s' "
                    "no longer reproduces its native ancestor", site->from);
                decided = 0;
            }
            if (decided < 0 || decided == 2) {
                ni_site_clear(site);
                if (decided == 2) continue;
                if (error && capacity) snprintf(error, capacity, "native representation of %s %s failed", use.owner, use.reference);
                goto done;
            }
            sites.count++;
        }
        if (ni_couple(source, &scan, &sites) < 0) {
            if (error && capacity) snprintf(error, capacity, "the coordinated uses could not be decided");
            goto done;
        }
        /* A smaller plan changes only decisions that relied on it (a use or
         * placed occurrence whose comparison renamed a reference, a use whose
         * served holder was set aside or whose count observer was carried);
         * with none, these decisions hold. */
        int depends = 0;
        for (size_t k = 0; k < sites.count && !depends; k++) depends = ni_relies(sites.sites + k);
        for (size_t k = 0; depends && k < sites.count; k++) {
            const sh_native_inline_site *site = sites.sites + k;
            const char *reference = NULL; char owner[32];
            const char *key = ni_site_key(site, owner, sizeof(owner), &reference);
            if (ni_relies(site) && !ni_sigma_relied_add(&scan, key, reference)) goto done;
        }
        int shrunk = depends ? ni_sigma_shrink(source, &scan, &sites) : 0;
        if (shrunk < 0) {
            if (error && capacity) snprintf(error, capacity, "the plan's holder substitution could not be decided");
            goto done;
        }
        if (!shrunk) break;
    }
    /* A use or placed occurrence that converted in an earlier round only
     * under an entry a later round withdrew is kept by that plan conflict:
     * its final reason says so, and it is never classified as a native limit. */
    for (size_t k = 0; *scan.sigma_withdrawn && k < sites.count; k++) {
        sh_native_inline_site *site = sites.sites + k;
        const char *reference = NULL; char owner[32];
        const char *key = ni_site_key(site, owner, sizeof(owner), &reference);
        if (site->converted || !ni_sigma_relied_has(&scan, key, reference)) continue;
        size_t used = strlen(site->reason);
        snprintf(site->reason + used, sizeof(site->reason) - used, "%sthe plan withdrew the holder substitution this use "
            "relied on in an earlier round: %s", used ? "; " : "", scan.sigma_withdrawn);
        site->limit = SH_DECL_CARRIER_GAP; site->conflict = 1;
    }
    if (*scan.sigma_withdrawn) ni_plan_note(&scan, "the plan withdrew %s", scan.sigma_withdrawn);
    /* What the census decided about a coop challenge's SP-loop drops. */
    if (census.built && !census.failed && census.sp_loot)
        ni_plan_note(&scan, "the loot census reads the coop challenge's SP-loop drops: %s", census.sp_settings);
    else if (census.built && !census.failed && census.sp_held)
        ni_plan_note(&scan, "the loot census leaves out the coop challenge's SP-loop drops: nothing play reaches sets "
            "useSPLoopDropComponent");
    /* A converted placed copy whose own state still names a loot drop entry
     * (its typed walk did not rename that reference). */
    for (size_t i = 0; i < scan.count; i++)
        for (size_t k = 0; replacements[i] && k < scan.sigma_count; k++) {
            if (!scan.sigma[k].drop) continue;
            ni_names_scan names = {scan.sigma[k].from};
            if (!sh_native_json_visit_values(replacements[i], strlen(replacements[i]), 256, ni_names_visit, &names) ||
                names.failed) goto done;
            if (names.found) ni_plan_note(&scan, "placed entity %zu converts with its state still naming '%s', the plan's "
                "entry for native '%s'", scan.spans[i].entity, scan.sigma[k].from, scan.sigma[k].to);
        }
    size_t at = 0;
    for (size_t i = 0; i < scan.count; i++) {
        if (!replacements[i]) continue;
        if (!ni_append(&out, json + at, scan.spans[i].offset - at) || !ni_append_text(&out, replacements[i])) goto done;
        at = scan.spans[i].offset + scan.spans[i].length;
    }
    if (!ni_append(&out, json + at, length - at)) goto done;
    /* Map-local uses of a converted occurrence name its renamed copy at their
     * own typed sites, inside converted values too. */
    {
        ni_follows follows = {0};
        int planned = ni_follows_plan(source, &scan, &sites, &follows);
        char *followed = NULL; size_t followed_length = 0;
        if (planned > 0 && follows.count && !(followed = ni_follows_apply(&follows, out.data, out.length, &followed_length))) {
            planned = -1;
            if (error && capacity) snprintf(error, capacity, "a map-local use of a converted placed copy could not be rewritten");
        }
        ni_follows_free(&follows);
        if (planned < 0) goto done;
        if (followed) { free(out.data); out.data = followed; out.length = followed_length; }
    }
    /* Nothing is published unless the output shows the coordinated plan. */
    {
        int verified = ni_couple_verify(source, &scan, out.data, out.length, &sites);
        if (verified < 0) goto done;
        if (!verified) {
            if (error && capacity) snprintf(error, capacity, "the coordinated identity rewrite did not verify on the output");
            goto done;
        }
        verified = ni_sigma_verify(source, &scan, json, length, out.data, out.length, &sites);
        if (verified < 0) goto done;
        if (!verified) {
            if (error && capacity) snprintf(error, capacity, "the plan's holder substitution did not verify on the output");
            goto done;
        }
    }
    /* The substitution converted sites relied on, for the derived re-read. */
    int relied = 0;
    for (size_t k = 0; k < sites.count; k++) relied |= ni_relies(sites.sites + k);
    if (relied && scan.sigma_count) {
        sites.substitutions = calloc(scan.sigma_count, sizeof(*sites.substitutions));
        if (!sites.substitutions) goto done;
        for (; sites.substitution_count < scan.sigma_count; sites.substitution_count++) {
            sh_native_inline_substitution *row = sites.substitutions + sites.substitution_count;
            row->from = _strdup(scan.sigma[sites.substitution_count].from);
            row->to = _strdup(scan.sigma[sites.substitution_count].to);
            row->drop = scan.sigma[sites.substitution_count].drop;
            if (!row->from || !row->to) { sites.substitution_count++; goto done; }
        }
    }
    if (*scan.plan_notes && !(sites.notes = _strdup(scan.plan_notes))) goto done;
    ok = 1;
done:
    for (size_t i = 0; replacements && i < scan.count; i++) free(replacements[i]);
    free(replacements); free(scan.spans); free(scan.uniques); free(scan.keys.rows); free(scan.values.rows); ni_pool_free(&pool);
    ni_served_free(&served); ni_census_free(&census); ni_sigma_free(&scan);
    for (size_t i = 0; i < scan.module_count; i++) { free(scan.modules[i].name); free(scan.modules[i].declaration); }
    free(scan.modules);
    if (!ok) {
        sh_native_inline_report_free(&sites); free(out.data);
        if (error && capacity && !*error) snprintf(error, capacity, "native representation failed");
        return NULL;
    }
    g_ni_stats.sites += sites.count;
    if (census.count > g_ni_stats.census_nodes) g_ni_stats.census_nodes = census.count;
    *report = sites; *output_length = out.length;
    return out.data;
}
