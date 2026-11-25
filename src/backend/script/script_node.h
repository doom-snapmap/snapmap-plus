/* script_node.h -- the Script node, where a mod's scripts meet SnapMap logic: a generated Toybox row (entityDef
 * mods/<salt>/script inheriting the stock Relay) whose wired Fire input posts SH_SCRIPT_EV_INPUT (text = the
 * node's name) and whose node(name):fire() raises On Fired. vars.get and vars.set access the map's variables. */
/* Engine, build 13954591 (VK / GL): Relay DoAction 0x140A0C430 / 0x140A0B630, FireOutput(entity, 8, activator)
 * 0x140A09AD0 / 0x140A08CC0, variable-changed(logic manager, type, index) 0x140891C60 / 0x140890BA0. Everything
 * runs on the main thread; SH_SCRIPT_PURE builds the engine-free parts. */
#ifndef SH_SCRIPT_NODE_H
#define SH_SCRIPT_NODE_H

#include <stddef.h>
#include <stdint.h>
#include "../../third_party/lua/lua.h"
#include "script_handles.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SH_SCRIPT_NODE_LEAF        "script"                 /* mods/<salt>/script */
#define SH_SCRIPT_NODE_DEF_PARENT  "snapmaps/logic/relay"   /* the stock Relay entityDef */
#define SH_SCRIPT_NODE_ROW_PARENT  "logic/relay"            /* the stock Relay row */
#define SH_SCRIPT_NODE_FIRED       8                        /* SNAP_EVENT_LOGIC_FIRED */
#define SH_SCRIPT_NODE_FIRST       64                       /* node():fire() first asks for this many; the list grows */

/* Variable types, as the engine numbers them (snapVarType_t). */
#define SH_SCRIPT_VAR_INTEGER 0
#define SH_SCRIPT_VAR_NUMBER  1
#define SH_SCRIPT_VAR_BOOLEAN 2
#define SH_SCRIPT_VAR_STRING  4

/* 1 when the script text registers an input: a call on.input( outside
 * comments and strings (spaces allowed around the dot). Pure. */
int sh_script_node_registers_input(const char *text, size_t len);

/* 1 when `name` (any case, either separator) is mods/<salt>/script, with the
 * salt copied into salt_out. Pure. */
int sh_script_node_is_def(const char *name, char *salt_out, size_t cap);

/* 1 when an entity's entityDef chain holds a Script node: a placed node's entityDef inherits mods/<salt>/script,
 * so every name in the chain is asked. `names` is the chain as the spawn hook reads it; the text form is the
 * chain a script handle records, one name per line. Pure. */
int sh_script_node_in_chain(const char *const *names, int count);
int sh_script_node_in_chain_text(const char *chain);

/* 1 when `request` (a served path, any case, either separator) is one of the
 * two files the row generates for `salt`: an author's own file there would
 * collide with the row, so the builder refuses it. Pure. */
int sh_script_node_reserved(const char *salt, const char *request);

/* The generated row of one mod: served request paths, decl bodies (CRLF), and the mod's own string keys
 * (<salt>_...) with their English text, which the builder adds to the build's strings. */
#define SH_SCRIPT_NODE_ROW_STRINGS 3
typedef struct sh_script_node_row {
    char  def_path[128];     /* generated/decls/entitydef/mods/<salt>/script.decl */
    char  row_path[128];     /* generated/decls/snapeditorentitydef/mods/<salt>/script.decl */
    char *def_text;
    char *row_text;
    int   nstrings;
    char  string_key[SH_SCRIPT_NODE_ROW_STRINGS][64];
    char *string_text[SH_SCRIPT_NODE_ROW_STRINGS];
} sh_script_node_row;

/* Make the row for `salt`; category_tag is the mod's "#str_..." Toybox category, NULL or "" gives the row its
 * own category named mod_name. Returns 1, or 0 for a bad salt or no memory (out is then empty). Pure. */
int  sh_script_node_row_make(const char *salt, const char *mod_name, const char *category_tag,
                             sh_script_node_row *out);
void sh_script_node_row_free(sh_script_node_row *row);

/* Read from the JSON of the map a Play converts (the idSnapMapEdit given to SnapMapEditToSnapBuild) each Script
 * node's uniqueId and display name and the variable names per type, in list order, replacing the table. Returns
 * 1, or 0 with a reason in err (the table is then empty). Main thread, at the Play gate. Pure. */
int  sh_script_node_map_load(const char *json, size_t len, char *err, size_t err_cap);
void sh_script_node_map_clear(void);

/* The display name of the Script node with this uniqueId, malloc'd (caller frees; "" when unnamed), or NULL
 * when uid is no Script node. */
char *sh_script_node_name_dup(int uid);

/* A variable by name (ASCII case-insensitive): 1 with *type and *index, 0 when the map has none, -1 when two
 * or more carry the name (*type gets the first type, *other the second). */
int  sh_script_node_var_find(const char *name, int *type, int *index, int *other);

/* The uniqueId at the end of an entity's game name ("..._61" -> 61), or -1. */
int  sh_script_node_uid_from_name(const char *game_name);

/* A variable value; s is a String variable's text, malloc'd (caller frees) in var_get's output and var_set's
 * stored value, the script's own text in var_set's input. */
typedef struct sh_script_node_value {
    int       type;          /* SH_SCRIPT_VAR_* */
    long long i;             /* Integer */
    double    n;             /* Number */
    int       b;             /* Boolean */
    char     *s;
} sh_script_node_value;

/* What the API asks of the engine (script_node.c; the tests supply fakes). Main thread, inside the tick. */
typedef struct sh_script_node_ops {
    void *context;
    int (*nodes)(void *context, int mod, sh_script_handle *out, int cap);   /* mod's Script node handles; count */
    int (*node_uid)(void *context, sh_script_handle node);                  /* uniqueId from the game name, or -1 */
    int (*fire)(void *context, sh_script_handle node, sh_script_handle activator);   /* FireOutput; 1 if it ran */
    int (*var_count)(void *context, int type);       /* values held, or -1 when the map's logic is not running */
    int (*var_get)(void *context, int type, int index, sh_script_node_value *out);
    /* stores *in as the stock actions do (clamped), notifies on change; 1 with the stored value */
    int (*var_set)(void *context, int type, int index, const sh_script_node_value *in,
                   sh_script_node_value *stored);
} sh_script_node_ops;
void sh_script_node_set_ops(const sh_script_node_ops *ops);

/* Post SH_SCRIPT_EV_INPUT, with the node's name, for the Script node `node` of hooks mod slot `mod` activated
 * by `activator` (0 for none); uid is its uniqueId (-1 unknown). Returns sh_script_queue_post's result. Any thread. */
int  sh_script_node_post_input(int mod, sh_script_handle node, sh_script_handle activator, int uid);

/* Add node(name) and vars to every state opened later (sh_script_api_add, sh_script_api_add_opener). Returns 1;
 * call once, before the first map. */
int  sh_script_node_api_register(void);

/* Set the read-only global `vars` (get and set) in a state being opened; the runtime calls it through
 * sh_script_api_add_opener. */
void sh_script_node_open_vars(lua_State *L);

/* The C functions behind them, for the opener and the tests. */
int  sh_script_node_lua_node(lua_State *L);
int  sh_script_node_lua_vars_get(lua_State *L);
int  sh_script_node_lua_vars_set(lua_State *L);

#include "../signatures.h"
#include "script_hooks.h"

/* The private signatures (NULL-terminated, enum order), for the two-image test. The Relay DoAction signature is
 * script_sigs.c's; its SH_SCRIPT_RELAY_STOLEN-byte detour window is compared before the hook is committed. */
enum sh_script_node_sig { SH_SCRIPT_NODE_SIG_FIRE_OUTPUT, SH_SCRIPT_NODE_SIG_VAR_CHANGED, SH_SCRIPT_NODE_SIG_COUNT };
extern const sig_entry SH_SCRIPT_NODE_SIGNATURES[];
extern const uint8_t SH_SCRIPT_NODE_RELAY_PROLOGUE[SH_SCRIPT_RELAY_STOLEN];

#ifndef SH_SCRIPT_PURE
/* Boot: resolve FireOutput, the variable-changed notification, the Relay DoAction and idStr assignment from
 * the shared results, verify the Relay prologue, register the API and supply the engine operations; commits
 * nothing. Returns 1 when the Script node works on this executable, else 0 with one log line. Main thread. */
int  sh_script_node_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* The Play gate of a map with scripts (main thread, in the build of the Play's level, before its spawns): read
 * the map's names from its JSON and commit the Relay DoAction detour once per session. json may be NULL when the
 * map could not be written: inputs then carry no node name and vars refuse every name. */
void sh_script_node_play_begin(const char *json, size_t len);
#endif

#ifdef __cplusplus
}
#endif
#endif
