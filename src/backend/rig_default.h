/* Build a retired rig from the engine's built-in data, without reopening its source. */
#ifndef SH_RIG_DEFAULT_H
#define SH_RIG_DEFAULT_H
#include <stddef.h>
#include <stdint.h>
int sh_rig_default_install(const uint8_t *base, size_t size, int renderer);
int sh_rig_default_ready(const char *family);
/* Other native default callbacks are forwarded exactly once. A rig callback uses
 * the verified buffer parser on freshly reconstructed storage. Zero is failure. */
int sh_rig_default_invoke(void *resource, void *actual_callback);
/* Valid only following a successful invocation above in the same ordered frame. */
int sh_rig_default_observed(void *resource, void *actual_callback);
#endif
