/* Allowlisted runtime cvar requirements from installed packages. */
#ifndef SNAPMAP_PLUS_PACKAGE_REQUIREMENTS_H
#define SNAPMAP_PLUS_PACKAGE_REQUIREMENTS_H

#include <stddef.h>
#include <stdint.h>

/* Capture compiled package.json requirements.cvars. Only supported idempotent
 * name/value pairs are admitted. Registration needs the installed set while
 * preparing and serving resident resources, including across map changes.
 * Map policy selection is a separate compiler operation. Both poll at RUNNING and publication apply synchronously, with
 * native readback. Capture alone never changes game values.
 */
int sh_package_requirements_install(const char *data_root,
                                    const uint8_t *module_base,
                                    void *cmdsys,
                                    void *buffer_command,
                                    void *execute_command_buffer,
                                    int user_layer_enabled);

/* Apply once regardless of load state. A supplied execute_command_buffer
 * replaces the captured drain callback. Call only at the decl server's
 * quiescent publication boundary. Shares the one-shot state with polling;
 * returns 1 if applied/verified or already applied, or 0 on refusal. Removed
 * requirements restore their displaced values unless changed by another user.
 */
int sh_package_requirements_apply_now(void *execute_command_buffer);

/* Recapture and synchronously apply requirements before runtime decl
 * registration. Blacklist gates must be live before native parsing.
 */
int sh_package_requirements_rearm(const char *data_root, void *execute_command_buffer,
                                  int user_layer_enabled);

/* Called from the engine frame's maintenance pass. It is a no-op until an admitted
 * requirement snapshot exists and the engine has reached RUNNING. */
void sh_package_requirements_poll(void);

#ifdef SH_PACKAGE_REQUIREMENTS_TESTING
void sh_package_requirements_test_reset(void);
void sh_package_requirements_test_set_load_state(volatile int *state);
void sh_package_requirements_test_set_cvar_slot(const void *slot);
size_t sh_package_requirements_test_count(void);
#endif

#endif /* SNAPMAP_PLUS_PACKAGE_REQUIREMENTS_H */
