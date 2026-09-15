/* Resource-free native session settings, private to retained map launches. */
#ifndef SH_MAP_SESSION_JSON_H
#define SH_MAP_SESSION_JSON_H
#include <stddef.h>

/* Validate the complete input, then copy only its four lobby slots into an
 * empty map serialized by the engine. No entities or delivery data reach the preliminary
 * native decoder. The returned malloc allocation must be freed by the caller.
 * This temporary document must never be saved or used to build gameplay. */
char *sh_map_session_json(const char *json, size_t length, const char *native_defaults,
    size_t defaults_length, char *error, size_t capacity);
#endif
