/* Cvars held by the open map. All ownership changes run on the game thread. */
#ifndef SH_MAP_CVARS_H
#define SH_MAP_CVARS_H

enum sh_cvar_owner { SH_CVAR_SCRIPT, SH_CVAR_REQUIREMENT };
typedef int (*sh_cvar_setter)(void *context, const char *name, const char *value);

int sh_map_cvar_same(const char *a, const char *b);
int sh_map_cvar_hold(enum sh_cvar_owner owner, const char *name, const char *before, const char *value,
                     sh_cvar_setter set, void *context);
const char *sh_map_cvar_restore_to(enum sh_cvar_owner owner, const char *name, const char *now);
void sh_map_cvar_release(enum sh_cvar_owner owner, const char *name);
void sh_map_cvars_end(enum sh_cvar_owner owner, char *(*read)(const char *), sh_cvar_setter set, void *context);
void sh_map_cvars_writer(int (*write)(const char *json));
void sh_map_cvars_flush(void);

#endif
