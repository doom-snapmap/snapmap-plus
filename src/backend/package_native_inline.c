#include "package_native_inline.h"

#include "config_json.h"
#include "decl_entity_class.h"

#include <stdint.h>
#include <errno.h>
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
typedef struct ni_scan { const char *json; ni_span *spans; size_t count, capacity; int failed; } ni_scan;
static int ni_visit(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    ni_scan *scan = context;
    const sh_json_path *item, *list;
    if (kind != SH_JSON_OBJECT || !path || !path->key || path->key_length != 9 || memcmp(path->key, "entityDef", 9))
        return 1;
    item = path->parent; list = item ? item->parent : NULL;
    if (!item || item->key || !list || !list->key || list->key_length != 8 ||
        memcmp(list->key, "entities", 8) || list->parent) return 1;
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

/* Textual overlay of one layer's members, in the native expanded-text order
 * (existing members update in place; new members append). The equivalence
 * check afterwards proves whether one layer reproduces the chain. */
static const char *ni_overlay(sh_decl_node *target, const sh_decl_node *source, ni_marks *marks)
{
    typedef struct frame { sh_decl_node *to; const sh_decl_node *from; } frame;
    frame *stack = malloc(16 * sizeof(*stack)); size_t count = 0, capacity = 16;
    const char *failure = NULL;
    if (!stack) return "allocation failed";
    stack[count++] = (frame){target, source};
    while (count && !failure) {
        frame current = stack[--count];
        for (const sh_decl_node *child = current.from->children; child && !failure; child = child->next) {
            sh_decl_node **slot = &current.to->children;
            while (*slot && strcmp((*slot)->key, child->key)) {
                if (!_stricmp((*slot)->key, child->key)) { failure = "differently cased member spellings"; break; }
                slot = &(*slot)->next;
            }
            if (failure) break;
            if (!*slot) {
                sh_decl_node *copy = ni_copy(child, marks);
                if (!copy) { failure = "allocation failed"; break; }
                *slot = copy; continue;
            }
            if ((*slot)->compound != child->compound) { failure = "a member changes between block and value"; break; }
            (*slot)->assignment = child->assignment; (*slot)->reset = child->reset;
            if (child->compound) {
                if (count == capacity) {
                    frame *grown = capacity <= SIZE_MAX / 2 / sizeof(*stack) ? realloc(stack, capacity * 2 * sizeof(*stack)) : NULL;
                    if (!grown) { failure = "allocation failed"; break; }
                    stack = grown; capacity *= 2;
                }
                stack[count++] = (frame){*slot, child};
            } else {
                char *value = _strdup(child->value ? child->value : "");
                if (!value) { failure = "allocation failed"; break; }
                free((*slot)->value); (*slot)->value = value;
                if (marks) {
                    ni_mark *mark = ni_mark_find(marks, child);
                    if (mark && !ni_mark_add(marks, *slot, mark->json, strlen(mark->json))) failure = "allocation failed";
                }
            }
        }
    }
    free(stack); return failure;
}

/* Order-insensitive structural equality of two expanded states. */
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
            const sh_decl_node *other = sh_decl_tree_member(current.b, child->key);
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

/* ---- one site ---- */
typedef struct ni_layer { char *name; sh_decl_node *tree; } ni_layer;

static void ni_refuse(sh_native_inline_site *site, sh_decl_carrier_limit limit, const char *format, const char *detail)
{
    site->converted = 0; site->limit = limit;
    snprintf(site->reason, sizeof(site->reason), format, detail ? detail : "");
}

/* The source-tree model does not evaluate native defaults, new list slots,
 * resets or custom readers, so its doubt is never a native limit. Only the
 * native evaluator may accept such a candidate; otherwise the package stays
 * (GAP) with the evaluator's outcome. 1 accepts, 0 refused. */
static int ni_native_verdict(const sh_native_inline_source *source, sh_native_inline_site *site, const char *base,
    const sh_decl_node *target, const sh_decl_node *candidate, const char *model)
{
    char detail[160] = "", reason[240];
    sh_native_eval_status status = SH_NATIVE_EVAL_UNAVAILABLE;
    if (source->evaluate)
        status = source->evaluate(source->context, site->entity, site->from, base, target, candidate, detail, sizeof(detail));
    if (status == SH_NATIVE_EVAL_EQUAL) return 1;
    const char *outcome = status == SH_NATIVE_EVAL_UNEQUAL ? "native evaluation found a different state" :
        status == SH_NATIVE_EVAL_UNSUPPORTED ? "native evaluation does not cover a value" :
        status == SH_NATIVE_EVAL_FAILED ? "native evaluation failed" : "native evaluation is unavailable";
    snprintf(reason, sizeof(reason), "%s (%s)%s%s", model, outcome, detail[0] ? ": " : "", detail);
    ni_refuse(site, SH_DECL_CARRIER_GAP, "%s", reason);
    return 0;
}

static int ni_member_allowed(const sh_decl_node *member, const char **reason)
{
    if (!strcmp(member->key, "inherit") || !strcmp(member->key, "class") ||
        !strcmp(member->key, "editorVars") || !strcmp(member->key, "edit")) return 1;
    if (!strcmp(member->key, "expandInheritance") && !member->compound && member->value &&
        (!strcmp(member->value, "true") || !strcmp(member->value, "1"))) return 1;
    *reason = member->key; return 0;
}

static char *ni_literal(const sh_decl_node *tree, const char *key)
{
    const sh_decl_node *member = sh_decl_tree_member(tree, key);
    const char *text; size_t length;
    if (!member || !sh_decl_tree_literal(member, &text, &length)) return NULL;
    char *copy = malloc(length + 1);
    if (copy) { memcpy(copy, text, length); copy[length] = 0; }
    return copy;
}

/* Returns 1 converted (replacement set), 0 refused (site reason set), -1 failure. */
static int ni_site(const sh_native_inline_source *source, const char *json, const ni_span *span,
    const char *inherit, const sh_json_object *definition, sh_native_inline_site *site, char **replacement)
{
    ni_layer *layers = NULL; size_t layer_count = 0, layer_capacity = 0;
    ni_marks marks = {0};
    ni_walker walker = {0};
    sh_decl_node *instance = NULL, *merged = NULL, *reference = NULL, *expanded_ref = NULL, *expanded_cand = NULL;
    int model_equal = 1;
    char *base = NULL, *state_json = NULL, *replaces = NULL, *materialized = NULL, error[512] = "";
    int folded = 0;
    const char *state = sh_json_object_get(definition, "state");
    const char *class_json = sh_json_object_get(definition, "className");
    int result = -1;
    (void)json; (void)span;
    /* The outer loop has applied the original native version policy before
     * choosing this inherit. The value reader needs state; className inherits
     * when omitted. Accepted output stamps the current version only below. */
    if (!state) { ni_refuse(site, SH_DECL_CARRIER_GAP, "entity value lacks state%s", NULL); result = 0; goto done; }
    int looked = source->looked_up ? source->looked_up(source->context, site->entity, inherit) : 0;
    if (looked < 0) goto done;
    if (looked) {
        ni_refuse(site, SH_DECL_CARRIER_ENGINE, "map logic looks up '%s' by name; inline state has no such identity", inherit);
        result = 0; goto done;
    }
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
        sh_decl_node *tree = sh_decl_tree_parse(text, error, sizeof(error));
        free((char *)text.text);
        if (!tree) { ni_refuse(site, SH_DECL_CARRIER_GAP, "definition does not parse: %s", error); free(name); result = 0; goto done; }
        if (layer_count == layer_capacity) {
            size_t next = layer_capacity ? layer_capacity * 2 : 4;
            ni_layer *grown = realloc(layers, next * sizeof(*grown));
            if (!grown) { sh_decl_tree_free(tree); free(name); goto done; }
            layers = grown; layer_capacity = next;
        }
        layers[layer_count++] = (ni_layer){name, tree};
        for (const sh_decl_node *member = tree->children; member; member = member->next) {
            const char *which = NULL;
            if (!ni_member_allowed(member, &which)) {
                ni_refuse(site, SH_DECL_CARRIER_GAP, "entity definition member '%s' has no inline state meaning", which);
                result = 0; goto done;
            }
        }
        name = ni_literal(tree, "inherit");
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
    for (size_t i = layer_count; i > 0; i--) {
        const sh_decl_node *edit = sh_decl_tree_member(layers[i - 1].tree, "edit");
        if (!edit) continue;
        if (!edit->compound) { ni_refuse(site, SH_DECL_CARRIER_GAP, "'%s' edit is not a block", layers[i - 1].name); result = 0; goto done; }
        sh_decl_node wrapper = {0}; wrapper.compound = 1; wrapper.children = (sh_decl_node *)edit;
        sh_decl_node *saved_next = edit->next; ((sh_decl_node *)edit)->next = NULL;
        const char *failure = ni_overlay(merged, &wrapper, NULL);
        ((sh_decl_node *)edit)->next = saved_next;
        if (failure) { ni_refuse(site, SH_DECL_CARRIER_GAP, "cannot combine the chain's edits: %s", failure); result = 0; goto done; }
    }
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
    /* Source-tree agreement only pre-filters. The native evaluator is the
     * accepting authority; without its EQUAL the complete package stays. */
    if (!ni_native_verdict(source, site, base, reference, merged, model_equal ? "the source model agrees" :
            "the source model of one inline layer differs from this chain's inherited lists or resets")) {
        free(*replacement); *replacement = NULL; result = 0; goto done;
    }
    site->converted = 1; site->limit = SH_DECL_CARRIER_EXACT; site->reason[0] = 0;
    if (!(site->to = _strdup(base))) { free(*replacement); *replacement = NULL; goto done; }
    result = 1;
done:
    for (size_t i = 0; i < layer_count; i++) { free(layers[i].name); sh_decl_tree_free(layers[i].tree); }
    free(layers); free(base); free(state_json); free(replaces); free(materialized);
    ni_marks_free(&marks); ni_walker_free(&walker);
    sh_decl_tree_free(instance); sh_decl_tree_free(merged); sh_decl_tree_free(reference);
    sh_decl_tree_free(expanded_ref); sh_decl_tree_free(expanded_cand);
    return result;
}

void sh_native_inline_report_free(sh_native_inline_report *report)
{
    if (!report) return;
    for (size_t i = 0; i < report->count; i++) { free(report->sites[i].from); free(report->sites[i].to); }
    free(report->sites); memset(report, 0, sizeof(*report));
}

char *sh_native_inline_document(const char *json, size_t length,
    const sh_native_inline_source *source, size_t *output_length,
    sh_native_inline_report *report, char *error, size_t capacity)
{
    ni_scan scan = {json, NULL, 0, 0, 0};
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
    if (!sh_native_json_visit_values(json, length, 256, ni_visit, &scan) || scan.failed) {
        if (error && capacity) snprintf(error, capacity, "map is not native JSON");
        goto done;
    }
    if (scan.count > 1) qsort(scan.spans, scan.count, sizeof(*scan.spans), ni_span_order);
    replacements = calloc(scan.count ? scan.count : 1, sizeof(*replacements));
    sites.sites = calloc(scan.count ? scan.count : 1, sizeof(*sites.sites));
    if (!replacements || !sites.sites) goto done;
    for (size_t i = 0; i < scan.count; i++) {
        sh_json_object original = {0}, normalized = {0};
        const sh_json_object *definition;
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
            site->entity = span->entity;
            const char *kept = source->keep ? source->keep(source->context, span->entity, inherit, &limit) : NULL;
            if (!(site->from = _strdup(inherit))) {
                sh_json_object_free(&original); sh_json_object_free(&normalized); goto done;
            }
            if (upgraded != 1) ni_refuse(site, SH_DECL_CARRIER_GAP, "native entity-version conversion is unavailable%s", NULL);
            else if (kept) ni_refuse(site, limit, "%s", kept);
            else if (ni_site(source, json, span, inherit, definition, site, &replacements[i]) < 0) {
                sh_json_object_free(&original); sh_json_object_free(&normalized);
                if (error && capacity) snprintf(error, capacity, "native representation of entity %zu failed", span->entity);
                goto done;
            }
        }
        sh_json_object_free(&original); sh_json_object_free(&normalized);
    }
    size_t at = 0;
    for (size_t i = 0; i < scan.count; i++) {
        if (!replacements[i]) continue;
        if (!ni_append(&out, json + at, scan.spans[i].offset - at) || !ni_append_text(&out, replacements[i])) goto done;
        at = scan.spans[i].offset + scan.spans[i].length;
    }
    if (!ni_append(&out, json + at, length - at)) goto done;
    ok = 1;
done:
    for (size_t i = 0; replacements && i < scan.count; i++) free(replacements[i]);
    free(replacements); free(scan.spans);
    if (!ok) {
        sh_native_inline_report_free(&sites); free(out.data);
        if (error && capacity && !*error) snprintf(error, capacity, "native representation failed");
        return NULL;
    }
    *report = sites; *output_length = out.length;
    return out.data;
}
