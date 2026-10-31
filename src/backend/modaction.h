/* modaction.h -- map behaviour carried by a stock String Variable Set node.
 *
 * A SnapMap String Variable "Set" action whose literal value starts with
 * "smp:" is a command for Snapmap+. When a logic signal reaches the node, the
 * backend parses the literal and runs it on the player who caused the signal
 * (the activator), then lets the stock write store the string as usual. A
 * vanilla game, or a game without Snapmap+, only stores the string, so the
 * map stays loadable and playable everywhere.
 *
 * Commands:
 *   smp:event <eventName> [args...]   fire an engine event on the activating
 *                                     player; arguments are typed by the
 *                                     event's own format string.
 *   smp:doomhands <0|1>               set the player's Doom-hands switch (the
 *                                     per-player form of
 *                                     g_weaponDoomMPUseDoomHands) and rebuild
 *                                     the hands and body models. Spawn
 *                                     recomputes the switch, so it lasts
 *                                     until the player respawns; a player
 *                                     who spawned with the multiplayer hands
 *                                     keeps their armour attachment drawn.
 *
 * Commands run on the host only (SnapMap logic never runs on a client) and
 * act on the player the activator stands for.
 *
 * Tokens are separated by spaces or tabs; double quotes group a token that
 * holds spaces. The parser and the argument typing below are pure C and are
 * unit tested without the engine. */
#ifndef SH_MODACTION_H
#define SH_MODACTION_H

#include <stddef.h>

#define SMP_PREFIX        "smp:"
#define SMP_MAX_ARGS      8      /* the engine's idEventArg array holds eight */
#define SMP_MAX_TOKEN     128
#define SMP_MAX_LITERAL   1024

typedef struct smp_command {
    char verb[32];
    int  argc;                                   /* tokens after the verb */
    char argv[SMP_MAX_ARGS + 1][SMP_MAX_TOKEN];  /* argv[0] is the event name for "event" */
} smp_command;

/* One typed event argument. type is the engine's idEventArg type character:
 * 'i' (an int, and a bool, which the engine's argument copy only carries as
 * 'i'), 'f', 's' (text points into the command) or 'e' (text is an entity
 * name the engine side resolves). */
typedef struct smp_arg {
    char        type;
    int         i;
    float       f;
    const char *text;
} smp_arg;

/* 1 when s starts with SMP_PREFIX (case-sensitive). */
int smp_is_command(const char *s);

/* Split a literal into a verb and its arguments. Returns 1, or 0 with a
 * one-line reason in err: not a command, empty verb, unterminated quote, a
 * token or the literal too long, too many tokens. */
int smp_parse(const char *literal, smp_command *out, char *err, size_t err_cap);

/* Type the event's arguments from its compact format string (one character
 * per argument, as the event record holds it: 'i' int, 'f' float, 's'
 * string, 'b' bool, 'e' entity). values/count are the tokens after the event
 * name. Returns the argument count written to out (0..SMP_MAX_ARGS), or -1
 * with a reason in err: wrong argument count, a value that does not parse
 * as its type, or a type character this command cannot build. Bools accept
 * 1/0, true/false, on/off, yes/no. */
int smp_type_args(const char *format, const char values[][SMP_MAX_TOKEN], int count,
                  smp_arg *out, char *err, size_t err_cap);

/* Parse a bool token; returns 1 and sets *v, or 0. */
int smp_parse_bool(const char *s, int *v);

#ifndef SH_MODACTION_PURE
#include <stdint.h>
#include "signatures.h"
#include "commands.h"

/* Hook the String Variable Set action's DoAction on build 13954591 (Vulkan
 * 0x9E26C0, OpenGL 0x9E19B0, prologue bytes checked before the detour).
 * Returns 1 when installed. Logs one line either way. */
int sh_modaction_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Console verbs (commands.c registers them):
 *   sh_smp "<literal>"         run a literal on player1 as if a String Set
 *                              node had fired it (a playtest must be running)
 *   sh_smp on|off|status       the hook's master switch (default on)
 *   sh_event info <name>       an event's number, format and argument types
 *   sh_event list [filter]     the live event registry */
void h_sh_smp(idCmdArgs *a);
void h_sh_event(idCmdArgs *a);

/* The map-level gate. The hook acts only on the host, during a SnapMap
 * session whose logic manager exists, with the master switch on, and when
 * the installed gate (if any) returns nonzero for the open map. NULL (the
 * default) admits every map that reaches the hook, because the literal's
 * prefix is itself the map's opt-in. */
typedef int (*sh_modaction_map_gate_fn)(void);
void sh_modaction_set_map_gate(sh_modaction_map_gate_fn fn);

/* The live event registry as UTF-8 lines "name\tformat\ttypes\tnames\n", for
 * the Timeline tab. Returns the bytes needed excluding the NUL, writing at
 * most cap bytes (always NUL terminated when cap > 0); 0 when the registry
 * is not reachable. Any thread; reads only. */
int sh_modaction_registry_text(char *out, int cap);
#endif

#endif /* SH_MODACTION_H */
