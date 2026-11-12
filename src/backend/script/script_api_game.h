/* Script calls that reach the whole game, beyond the calls written for one
 * purpose: every engine event, every SnapMap event, every cvar and every
 * decl. With e:get and e:set (script_api_reflect.h) they give a script what
 * the game's own code and SnapMap's wired logic have.
 *
 *   e:event(name, ...)     run an engine event on the entity now and return
 *                          its result (a number, text, a position, an entity,
 *                          a decl's name) or nothing. The values are the
 *                          event's own arguments, in order: whole numbers,
 *                          true/false, numbers, text, positions {x, y, z},
 *                          angles {pitch, yaw, roll}, colors {r, g, b, a},
 *                          entities (nil for none) and decls (from decl()).
 *   e:post(name, ...)      the same, queued for the end of this frame.
 *   event_args(name)       what an event takes, as a list of words ("number",
 *                          "entity", ...), or nil when the game has no such
 *                          event.
 *   e:signal(event[, by])  fire a SnapMap event on the entity, as the game
 *                          does: everything wired to that output of the
 *                          object runs, with `by` as the activator.
 *                          e:signal("ai_killed"), e:signal("trigger_touched", player).
 *   on.signal(event, fn)   (the runtime's) fn(entity, activator) whenever any
 *                          object of the map fires that SnapMap event.
 *   cvar(name)             a cvar's value (a number when it reads as one).
 *   cvar(name, value)      set it for this map; it is put back when the map
 *                          ends.
 *   entities()             every entity of the map now, players first (filter
 *                          with e:def(), e:class() or e:get(...)).
 *   decl(type, name)       a decl of the mod, or of the game with
 *                          game("..."): d:get(path), d:set(path, value),
 *                          d:fields([path]), d:name(), d:type(). What a
 *                          script sets in a decl is put back when the map
 *                          ends.
 *
 * Event names are the engine's ("setOrigin", "activate"); SnapMap events are
 * the editor's outputs, written with or without SNAP_EVENT_ and in any case
 * ("ai_killed"). Every refusal is a sentence naming the call and the part. */
#ifndef SH_SCRIPT_API_GAME_H
#define SH_SCRIPT_API_GAME_H

#include "script_api_common.h"

typedef struct sh_script_game_ops {
    /* The name of a decl; NULL when unreadable. */
    const char *(*decl_name)(void *decl);
    /* A SnapMap event's number from its key (lower case, without
     * snap_event_), -1 when the game has none. */
    int (*signal_number)(const char *event);
    /* Fire it on the entity; 1 when the game ran it. */
    int (*signal_fire)(void *ent, int number, void *activator);
    /* A cvar's value as text: 1, or 0 when the game has no such cvar. */
    int (*cvar_get)(const char *name, char *out, size_t cap);
    /* Set it: 1 set, 0 no such cvar, -1 it cannot be changed while the game runs, -2 the game refused. */
    int (*cvar_set)(const char *name, const char *value);
    /* The decl of that type and full name, NULL when the game has none. */
    void *(*decl)(const char *type, const char *name);
    /* The class that describes decls of that type, NULL when the game describes none. */
    const char *(*decl_class)(const char *type);
} sh_script_game_ops;

/* The engine's operations, or a test's (NULL turns the calls off). */
void sh_script_api_game_set(const sh_script_game_ops *ops);

/* Register the calls (once). */
void sh_script_api_game_register(void);

/* The detour's report, and a test's: `ent` fired SnapMap event `number`. */
void sh_script_game_signal_seen(void *ent, const char *event, void *activator);

#ifndef SH_SCRIPT_PURE
/* Bind to the game (after the script API and the Script node resolved). */
int sh_script_api_game_install(const sig_result *results, size_t count, const uint8_t *module_base);
#endif

#endif
