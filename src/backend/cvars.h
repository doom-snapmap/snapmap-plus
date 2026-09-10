/* Register the supported Snapmap+ cvars with process-lifetime backing storage.
 * Settings without implemented consumers are omitted. */
#ifndef BACKEND_B2_CVARS_H
#define BACKEND_B2_CVARS_H

/* Register each row once, then insert it into the full lookup list and hash.
 * Never rerun the engine static registration pass or grow its tables here.
 * A missing cvar_register returns 0; a missing module_base skips lookup insertion.
 * Return the number registered, which may exceed the number made findable. */
int sh_cvars_install(void *cvar_register, const void *module_base);

/* Indices match the cvars.c table and select live values below. */
#define B2_CVAR_SH_PRETTY_ON                 0
#define B2_CVAR_SH_COPY_RESLIST_TO_CLIPBOARD 1

/* Read integer/bool value at idCVar+0x30. Return def for an invalid index,
 * an unregistered object, or a memory fault. */
int sh_cvar_value_int(int index, int def);

/* Read table metadata for sh_help. A valid row returns 1 with borrowed strings. */
int sh_cvar_table_count(void);
int sh_cvar_table_row(int index, const char **name, const char **def, const char **desc);

#endif /* BACKEND_B2_CVARS_H */
