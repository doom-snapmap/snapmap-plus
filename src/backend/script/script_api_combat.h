/* script_api_combat.h -- the combat calls of mod scripts (API version 1): script-dealt damage, projectiles,
 * effects, sounds, joint positions and the pain an entity is in, plus the records the damage, death, pain and
 * projectile hooks (script_hooks.c) keep for them.
 *
 * Lua (every decl name is mod-relative and must be the calling mod's own; "mods/<salt>/..." with its own salt
 * is accepted as written):
 *   hitscan(from, dir, damage, [range], [attacker]) -> {pos, normal, entity} or nil
 *   radius_damage(pos, damage, [attacker])
 *   damage(e, damage, [attacker], [from])     e:damage(damage, [attacker], [from])
 *   explode(pos, damage, [effect], [attacker]) -> the effect entity or nil
 *   play_effect(e, condition)                           stop_effect(e, condition)
 *   play_effect(effect, pos, [dir]) -> e                move_effect(e, pos, [dir])   stop_effect(e)
 *   end_effects_at_once([effect]) -> boolean         (effect: an fx decl; none: every fx decl of this mod)
 *   sound(e, sound, [channel])
 *   e:tag(name) -> pos, forward                e:pain() -> reaction or nil
 *     (name: a tag of a demon's md6Def, else a joint)
 *   ai:fire(projectile, tag or joint, target) -> p or nil
 *   ai:launch(projectile, tag or joint, velocity) -> p or nil
 *   e:incoming_projectiles(size) -> { {projectile = p, seconds = t}, ... }, soonest first
 *   ai:can_see_from(other, tag or joint) -> boolean
 *   p:steer(target, [speed])                   p:explode()
 * p:steer and p:explode answer nil once the projectile is gone, as every call on a removed entity does.
 * `attacker` is one of the mod's own entities (none: the damage comes from the world, as a trigger's does).
 * `damage`, `projectile` and `sound` are decls, `effect` an entityDef, `condition` an fx condition (FX_...) of the
 * entity's own fx decl.
 *
 * Engine paths (build 13954591, both renderers):
 *   damage           the entity's `damage` event (244, 'daffeb'); a player hit by a named attacker takes idPlayer::Damage
 *                    (VK 0x140DC3880, GL 0x140DC30A0) with the attacker as inflictor, as the event carries none
 *   radius_damage    RadiusDamage(game, origin, inflictor, attacker, decl + 0x70, 1.0, true, NULL)
 *                    (VK 0x1403A21E0, GL 0x1403A1AF0), which queues the damage the game applies that frame
 *   hitscan          the game trace (script_api_common.h), then the damage event on the entity it hits
 *   fire             ae_launchProjectileAtTarget (300) run at once on the AI; the launch hook catches the projectile,
 *                    and a position target is steered to before the projectile's first frame
 *   launch           the launch fire makes, then setLinearVelocity: the projectile leaves the tag at exactly that speed
 *   incoming_projectiles  entities descending from idProjectile, not launched by e: a ray along the velocity
 *                    relative to e against e's box (size x forward, y left, z up); seconds = distance over speed
 *   steer            setLinearVelocity on the projectile, at its current speed unless one is given
 *   p:gravity        the setGravity event, straight down: the projectile falls at its source game's gravity
 *   p:explode        the projectile's Explode (idProjectile vtable +0xAA0: VK 0x140F38240, GL 0x140F37700)
 *   play_effect(e, cond)  an AI or player: ae_startFX_Networked / ae_stopFX_Networked (1257 / 1258, '4i'); any other
 *                    entity: idEntity StartFX / StopFX (VK 0x14082C270 / 0x14082D1D0, GL 0x14082B430 / 0x14082C390)
 *   play_effect(effect, pos)  the mod's entityDef spawned through spawn() (its caps); stop_effect removes it
 *   end_effects_at_once   detours of the FX manager's stop of one action (VK 0x1416EEEF0, GL 0x1416EF870) and
 *                    request of one action (VK 0x1416ECC80, GL 0x1416ED600), installed at the first call
 *   sound            startSoundShader (200, 'di'): a sound decl on a channel of the entity
 *   tag              getJointHandle / getJointPos / getJointAngle (256 / 259 / 260): the joint in world space
 *   pain             idAI2 +0x5060, the current pain reaction, written by SetPainReaction (VK 0x140748710,
 *                    GL 0x140748020) and cleared to PAIN_NONE when the pain state ends (VK 0x140749F80, GL 0x140749890)
 *
 * Every call runs on the game thread inside the script tick; the record functions (sh_script_combat_note_*) are
 * called by hooks on any thread. */
#ifndef SH_SCRIPT_API_COMBAT_H
#define SH_SCRIPT_API_COMBAT_H

#include <stddef.h>
#include <stdint.h>
#include "script_handles.h"

#ifdef __cplusplus
extern "C" {
#endif

/* painType_t of build 13954591 (the enum table is the same on both renderers: VK 0x1434D4C40, GL 0x14351CAE0). */
#define SH_COMBAT_PAIN_NONE   0
#define SH_COMBAT_PAIN_COUNT  14
/* The engine's name of a pain reaction ("PAIN_FALTER"), NULL out of range. */
const char *sh_script_combat_pain_name(int reaction);

/* soundChannel_t values a script names. */
int sh_script_combat_channel(const char *name);   /* -1 when unknown */

typedef struct sh_script_combat_engine {
    /* the loaded decl `name` of decl type `type` ("damage", "projectile", "sound"), or NULL */
    void *(*decl)(const char *type, const char *name);
    /* radius damage at pos with a damage decl; inflictor and attacker are entities */
    int   (*radius_damage)(const float pos[3], void *decl, void *inflictor, void *attacker);
    /* explode a launched projectile now */
    int   (*explode)(void *projectile);
    /* the AI's current pain reaction (painType_t), -1 when unreadable */
    int   (*pain_reaction)(void *ai);
    /* the entity's name, as the engine finds entities by name */
    int   (*entity_name)(void *ent, char *out, size_t cap);
    /* an fx condition's value by its engine name, -1 when there is none */
    int   (*fx_condition)(const char *name);
    /* the world entity (environmental damage comes from it), NULL when unreadable */
    void *(*world)(void);
    /* start / stop the actions of the entity's own fx decl listed under a condition, by the engine's entity call
     * (an effect entity answers no animation event) */
    int   (*start_fx)(void *ent, int condition);
    int   (*stop_fx)(void *ent, int condition);
    /* damage an actor (a player, a bot or a demon) by its own Damage, the game's damage call, with an inflictor
     * and an attacker, so the game's team rule, its percepts and its kill credit see who hit it: 1 done, 0 when
     * the victim is no actor whose Damage this executable's is (the caller then sends the entity's damage event) */
    int   (*actor_damage)(void *victim, void *decl, void *inflictor, void *attacker, const float dir[3]);
    /* make the FX manager's stop and its request of a sound item follow the hard stop rule for the decls named
     * with sh_fx_hard_add: 1 when either hook is in place, 0 when this executable can serve neither */
    int   (*fx_hard_stops)(void);
    /* 1 when the entity is a projectile (its class descends from idProjectile) */
    int   (*is_projectile)(void *ent);
} sh_script_combat_engine;

void sh_script_combat_set_engine(const sh_script_combat_engine *engine);

/* Add the combat calls to every script state opened afterwards. Idempotent. */
void sh_script_combat_register(void);

/* Forget every record (a new map's states, or the tests) and register the release of records with the handle table. */
void sh_script_combat_reset(void);

/* Under end_effects_at_once a stopped action of a named decl follows DOOM Eternal's FX stop rule, not the game's
 * fade: a model, render parm, blur, screen overlay or screen particle ends in the frame of the stop, and a particle
 * or light stopped inside its delay is never started. A request of a named decl's sound item whose action is
 * requested and started, with the item's byte at +0x26 (forceLoopRestart) clear, is not passed to the game, so a
 * looping sound is not doubled. Names are whole decl names or a prefix ending in '/'; the game's own are never named. */
#define SH_FX_TYPE_LIGHT           0     /* fxActionType_t of build 13954591 (both renderers) */
#define SH_FX_TYPE_PARTICLE        1
#define SH_FX_TYPE_MODEL           4
#define SH_FX_TYPE_SOUND           5
#define SH_FX_TYPE_RENDERPARM      9
#define SH_FX_TYPE_BLUR_RADIAL     13
#define SH_FX_TYPE_BLUR_GAUSSIAN   14
#define SH_FX_TYPE_SCREEN_OVERLAY  20
/* 1 when a stop that is not immediate ends an action of this type in that frame under the rule: `screen_particle`
 * is the item's isScreenPrt, `waiting` says the action's delay has not passed yet. 0: the game's own stop. */
int  sh_fx_hard_ends(int type, int screen_particle, int waiting);
/* 1 when a request of an item of this type is not passed to the game: a sound item whose action's `flags` say
 * requested and started, and `restart_before_end` (the item's byte at +0x26) is 0. 0: the game's own request. */
int  sh_fx_sound_held(int type, unsigned flags, int restart_before_end);
/* Name a decl, or every decl under a prefix that ends in '/'. 1 named (or
 * already named), 0 refused (empty, too many, no memory). Any thread. */
int  sh_fx_hard_add(const char *name);
/* 1 when a decl of this name follows the rule. Any thread. */
int  sh_fx_hard_is(const char *decl_name);
int  sh_fx_hard_count(void);
/* Forget every name (a new map's states, or the tests). */
void sh_fx_hard_reset(void);

/* A record lives as long as its entity: the handle table's sweep (sh_script_handles_on_gone, registered by
 * sh_script_combat_reset) drops the launch, last hit and death records. No record table has a count of its own. */

/* The launch hook: a projectile `owner` (an entity of mod `mod`) launched. Records the projectile's handle and
 * launch and returns the handle (0 without memory: nothing is recorded). Any thread. */
sh_script_handle sh_script_combat_launch(uint32_t projectile_spawn_id, sh_script_handle owner, int mod);
/* Before the engine launches a projectile of `owner`: 1 with the origin and
 * axis (rows forward, left, up) ai:fire asked this launch to start from. */
int sh_script_combat_launch_from(sh_script_handle owner, float origin[3], float axis[9]);

/* The launch record alone, for a projectile whose handle exists. Any thread. */
void sh_script_combat_note_launch(uint32_t projectile_spawn_id, sh_script_handle owner, int mod);

/* 1 while the projectile with this spawn id, launched by an entity of a
 * scripted mod this map, exists, with that mod and launcher. Any thread. */
int  sh_script_combat_launched(uint32_t projectile_spawn_id, int *mod, sh_script_handle *owner);

/* Launch records held now. */
int  sh_script_combat_launches(void);

/* The last hit `victim` took: its damage decl and attacker (0 for none), as the pain hook reports them.
 * *decl gets a malloc'd copy of the whole name (the caller frees it). Any thread. */
void sh_script_combat_note_hit(sh_script_handle victim, const char *decl, sh_script_handle attacker);
int  sh_script_combat_last_hit(sh_script_handle victim, char **decl, sh_script_handle *attacker);

/* The owner effect route shared by play_effect and a native wound opening. */
int sh_script_combat_fx_condition(const char *name);
int sh_script_combat_start_owner_fx(void *ent, int condition);

/* 1 the first time `victim`'s death is reported this map, 0 after (the
 * engine may run its death path again for a gib). Any thread. */
int  sh_script_combat_first_death(sh_script_handle victim);

#ifndef SH_SCRIPT_PURE
#include "../signatures.h"
/* Boot: resolves the signatures, the decl registry and the fx condition table, fills the engine and registers the
 * calls. Returns 1 when usable; 0 with one plain log line (the calls then answer nil). */
int sh_script_combat_install(const sig_result *results, size_t count, const uint8_t *module_base);
#endif

#include "../signatures.h"
/* script_combat_sigs.c: the signatures of the combat engine side and of the damage, pain and projectile hooks
 * (two-image test: tests/script_combat_sigs_test.c). Enum order, NULL-terminated. */
enum sh_combat_sig {
    SH_COMBAT_SIG_DAMAGE_APPLY,      /* idAI2::Damage_Apply(ai, record): detoured */
    SH_COMBAT_SIG_SET_PAIN,          /* idAI2 SetPainReaction(ai, reaction, ms): detoured */
    SH_COMBAT_SIG_PROJECTILE_LAUNCH, /* idProjectile::Launch(projectile, owner, origin, axis, ...): detoured */
    SH_COMBAT_SIG_RADIUS_DAMAGE,     /* idGameLocal::RadiusDamage wrapper: called */
    SH_COMBAT_SIG_ENT_START_FX,      /* idEntity StartFX(entity, condition, extra): called */
    SH_COMBAT_SIG_ENT_STOP_FX,       /* idEntity StopFX(entity, condition, extra): called */
    SH_COMBAT_SIG_PLAYER_DAMAGE,     /* idPlayer::Damage(player, inflictor, attacker, parms, scale, dir, hit): called */
    SH_COMBAT_SIG_FX_ACTION_STOP,    /* idFXManager's stop of one action (manager, index, time, immediate, recycle): detoured */
    SH_COMBAT_SIG_FX_ACTION_REQUEST, /* idFXManager's request of one action (manager, index, six more): detoured */
    SH_COMBAT_SIG_AI_DAMAGE,         /* idAI2's Damage (slot SH_COMBAT_DAMAGE_VSLOT, calls the idAI2 body): compared */
    SH_COMBAT_SIG_COUNT
};
extern const sig_entry SH_COMBAT_SIGNATURES[];
/* Damage(actor, inflictor, attacker, parms, scale, direction*, hit*): the same virtual slot in every actor class,
 * idPlayer and idAI2 alike, in both images. */
#define SH_COMBAT_DAMAGE_VSLOT 130

/* Detour windows (whole, position-independent instructions; the test proves them). */
#define SH_COMBAT_DAMAGE_APPLY_STOLEN 14
#define SH_COMBAT_SET_PAIN_STOLEN     15
#define SH_COMBAT_LAUNCH_STOLEN       15
/* The FX manager's stop of one action: six pushes and sub rsp (whole, position-independent). */
#define SH_COMBAT_FX_STOP_STOLEN      16
static const unsigned char k_combat_fx_stop_prologue[SH_COMBAT_FX_STOP_STOLEN] = {
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x55, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00
};
/* What that function reads (both renderers; the test compares the bytes): the manager's decl and actions, the
 * decl's items, and the item and action fields its fade branch tests. */
#define SH_FX_MGR_DECL            0x18u    /* idFXManager: its idDeclFX* */
#define SH_FX_MGR_ACTIONS         0x58u    /*              idFXActionData array, one per item */
#define SH_FX_DECL_ITEMS          0x80u    /* idDeclFX: idFXSingleAction array */
#define SH_FX_DECL_COUNT          0x88u    /*           and its count */
#define SH_FX_ITEM_SIZE           0x570u
#define SH_FX_ITEM_TYPE           0x14u    /* idFXSingleAction: fxActionType_t */
#define SH_FX_ITEM_SCREEN_PRT     0x20Eu   /*                   particleParms.isScreenPrt */
#define SH_FX_ACTION_SIZE         0x128u
#define SH_FX_ACTION_FLAGS        0x00u    /* idFXActionData: bit 1 requested, bit 4 fading */
#define SH_FX_ACTION_DELAY        0x04u    /*                 the delay drawn at the request, ms */
#define SH_FX_ACTION_REQUEST      0x08u    /*                 the request's time, ms */
#define SH_FX_ACTION_REQUESTED    0x02u
#define SH_FX_ACTION_FADING       0x10u
#define SH_FX_STOP_FADE_BRANCH    0x90u    /* the function's fade branch (the test compares its bytes) */
/* The FX manager's request of one action: seven pushes and lea rbp,[rsp-0x90] (whole, position-independent). */
#define SH_COMBAT_FX_REQUEST_STOLEN   19
static const unsigned char k_combat_fx_request_prologue[SH_COMBAT_FX_REQUEST_STOLEN] = {
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x70, 0xFF, 0xFF, 0xFF
};
/* What the request reads besides the fields above (both renderers; the test compares the bytes): its head
 * returns at once for a requested, looping, not fading action whose item does not force a loop restart. */
#define SH_FX_ITEM_LOOPING        0x25u    /* idFXSingleAction: looping */
#define SH_FX_ITEM_RESTART        0x26u    /*                   forceLoopRestart (DOOM Eternal: allowRestartBeforeEnd) */
#define SH_FX_ACTION_STARTED      0x04u    /* idFXActionData flags: actually started (a sound: posted or due) */
#define SH_FX_ACTION_SOUND        0xB0u    /*                 the sound's handle, overwritten at each start */

/* Engine layout the hooks and the calls read (both renderers). */
#define SH_COMBAT_REC_INFLICTOR   0x000u   /* damage record: idEntity* inflictor */
#define SH_COMBAT_REC_ATTACKER    0x008u   /*                idEntity* attacker */
#define SH_COMBAT_REC_DECL        0x628u   /*                idDeclDamage* (the parms' own decl, parms + 0x618) */
#define SH_COMBAT_REC_SCALE       0x630u   /*                float damage scale */
#define SH_COMBAT_REC_DIR         0x634u   /*                idVec3 direction */
#define SH_COMBAT_REC_HITS        0x648u   /*                int hits */
#define SH_COMBAT_REC_ZONE        0x1920u  /*                damage zone record* per hit (zone name char* at +0) */
#define SH_COMBAT_REC_AMOUNT      0x1A30u  /*                float amount Damage_Calculate settled on */
#define SH_COMBAT_REC_ZONES_MAX   32
#define SH_COMBAT_PARMS_DECL      0x618u   /* idDamageParms: its idDeclDamage* */
#define SH_COMBAT_DECL_PARMS      0x070u   /* idDeclDamage: its idDamageParms */
#define SH_COMBAT_AI_PAIN         0x5060u  /* idAI2: current painType_t */
#define SH_COMBAT_PROJ_DECL       0x4640u  /* idProjectile: its idDeclProjectile* */
#define SH_COMBAT_PROJ_EXPLODE    0xAA0u   /* idProjectile vtable: Explode(projectile, ignore, collision*) */
#define SH_COMBAT_ENT_NAME        0x388u   /* idEntity: name char* */
#define SH_COMBAT_DECL_NAME       0x008u   /* idDecl: name char* */

/* Where a ray from `o` along the unit direction `d` enters and leaves a box
 * (center `c`, axis rows forward, left, up, half sizes `h`): 1 with both
 * distances along the ray (negative behind `o`), 0 when it misses. */
int sh_script_combat_ray_box(const float o[3], const float d[3], const float c[3], const float ax[9],
                             const float h[3], double *enter, double *leave);

/* Finds the engine enum table (entries { char *name, char *value, ... } 0x18 bytes apart) whose first two names
 * are `first` and `second` in an image mapped by RVA; returns its RVA and the count of entries whose names start
 * with `prefix`, or 0. pointer_base is the base the image's pointers were relocated to (0 for a copy at its own). */
uint32_t sh_script_combat_enum_find(const uint8_t *module_base, uint64_t pointer_base, const char *first,
                                    const char *second, const char *prefix, int *count);

#ifdef __cplusplus
}
#endif
#endif
