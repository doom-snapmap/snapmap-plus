/* Script calls that read and write the game's objects by field name
 * (script_reflect.h is the walk; this is its Lua side and its engine side).
 *
 *   e:get([path])        the value at `path` of the entity (no path: the whole
 *                        entity as a table). Numbers, true/false, text, an
 *                        enum member's name, an entity for a reference, a
 *                        decl's name for a decl pointer, a table for an
 *                        object, a list (1-based) for a list or array.
 *   e:set(path, value)   write it: a number, true/false, text, an enum member
 *                        by name or number, an entity or nil for an entity
 *                        pointer, a decl name for a decl pointer, a table of
 *                        fields for an object, a list of the same length for
 *                        a list or array. Returns true.
 *   e:fields([path])     the fields of the object at `path` (ancestors' too),
 *                        each { name = , type = , kind = }, in the game's
 *                        own order.
 *   e:class()            the entity's class name.
 *   enum(name)           an enum of the game as a table: member -> number.
 *
 * Any entity of the map can be read and written: a script is the map's own
 * logic, as wired logic is. A path that does not exist, a value of the wrong
 * kind, and a field the game does not describe are refused with a sentence
 * that names the part. */
#ifndef SH_SCRIPT_API_REFLECT_H
#define SH_SCRIPT_API_REFLECT_H

#include "script_reflect.h"
#include "script_runtime.h"

/* What values need beyond the walk: the engine's, or a test's. */
typedef struct sh_script_reflect_ops {
    void *ctx;
    /* The class of an entity (its entityDef's class). */
    const char *(*entity_class)(void *ctx, void *ent);
    /* Write n bytes at addr; 0 when the memory cannot be written. */
    int (*write)(void *ctx, void *addr, const void *in, size_t n);
    /* An idStr's text (malloc'd, with its length), NULL when unreadable. */
    char *(*str_read)(void *ctx, const void *idstr, size_t *len);
    /* Assign an idStr through the engine; 0 when unavailable. */
    int (*str_assign)(void *ctx, void *idstr, const char *text);
    /* The name of a decl or other named resource; NULL when the object has none. */
    const char *(*resource_name)(void *ctx, const void *object);
    /* The resource of class `cls` named `name`; NULL when the game has none. */
    void *(*resource_find)(void *ctx, const char *cls, const char *name);
    /* Push the entity object as a script entity (nil when it is none). */
    void (*push_entity)(lua_State *L, void *ctx, void *ent);
    /* The entity object a script value names, NULL when it is no entity. */
    void *(*to_entity)(lua_State *L, void *ctx, int idx);
    /* The index-th member of an enum; 0 past the last or when it is no enum. */
    int (*enum_member)(void *ctx, const char *type, uint32_t index, const char **member, long long *value);
} sh_script_reflect_ops;

/* The types, memory and values the calls use (NULLs turn the calls off). */
void sh_script_api_reflect_set(const sh_rf_host *host, const sh_script_reflect_ops *ops);

/* Register get, set, fields, class and enum (once). */
void sh_script_api_reflect_register(void);

/* Push the value at `loc`; 1, or 0 with a sentence in err. */
int sh_script_reflect_push(lua_State *L, const sh_rf_loc *loc, char *err, size_t err_cap);
/* Store the value at stack index idx into `loc`; 1, or 0 with a sentence in err. */
int sh_script_reflect_store(lua_State *L, const sh_rf_loc *loc, int idx, char *err, size_t err_cap);

/* The same reads and writes for any object of a class the game describes (a
 * decl, the player's inventory): `call` names the script call in a refusal.
 * Each pushes its result or raises. A write with restore_at_map_end is put
 * back when the map ends (for objects that outlive the map). */
int sh_script_reflect_ready(void);
int sh_script_reflect_get(lua_State *L, const void *object, const char *cls, const char *path, const char *call);
int sh_script_reflect_set(lua_State *L, void *object, const char *cls, const char *path, int idx, const char *call,
                          int restore_at_map_end);
int sh_script_reflect_fields(lua_State *L, const void *object, const char *cls, const char *path, const char *call);
/* Put back every write kept for the map's end, newest first. */
void sh_script_reflect_restore(void);

#ifndef SH_SCRIPT_PURE
/* Bind the calls to the game (after the typeinfo and script signatures are
 * resolved). The game's types are asked at each call: they answer once a
 * map is up. */
int sh_script_api_reflect_install(void);
#endif

#endif
