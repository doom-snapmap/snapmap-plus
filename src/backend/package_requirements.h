/* Allowlisted runtime cvar requirements from installed packages. */
#ifndef SNAPMAP_PLUS_PACKAGE_REQUIREMENTS_H
#define SNAPMAP_PLUS_PACKAGE_REQUIREMENTS_H

#include <stddef.h>
#include <stdint.h>

/* Capture each installed package's requirements/*.requirements. Rows use
 * cvar<TAB>name<TAB>value; only audited idempotent pairs are admitted. The
 * poll queues them at RUNNING, while declaration publication uses the
 * synchronous apply entry point.
 */
int sh_package_requirements_install(const char *data_root,
                                    const uint8_t *module_base,
                                    void *cmdsys,
                                    void *buffer_command,
                                    int user_layer_enabled);

/* Apply once regardless of load state. When supplied, execute_command_buffer
 * drains queued settings before returning. Call only at the decl server's
 * quiescent publication boundary. Shares the one-shot state with polling;
 * returns 1 if applied/already applied, or 0 on refusal.
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
size_t sh_package_requirements_test_count(void);
#endif

#endif /* SNAPMAP_PLUS_PACKAGE_REQUIREMENTS_H */
