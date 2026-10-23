/* Capture the verified script-module metadata API without a game world. */
#ifndef SH_DECL_SCRIPT_NATIVE_H
#define SH_DECL_SCRIPT_NATIVE_H
#include "decl_script_schema.h"
#include <windows.h>

/* Caller holds the engine main-thread preparation boundary. The module must
 * already be loaded. Its supported metadata implementation only compares
 * strings and allocates temporary string storage; no object factory runs.
 * The temporary allocation context is restored even when a query faults. */
sh_decl_script_schema *sh_decl_script_native_capture(HMODULE module, char *error, size_t capacity);
/* Retain an existing supported module, or temporarily load the verified game
 * directory module solely for its metadata API. Release the module reference
 * after copying; the returned schema has no module lifetime dependency. */
sh_decl_script_schema *sh_decl_script_native_capture_default(char *error, size_t capacity);

#ifdef SH_DECL_SCRIPT_NATIVE_TESTING
typedef struct sh_decl_script_api {
    void *object;
    void *(__fastcall *exchange)(void *, void *);
    uintptr_t (__fastcall *names)(void *);
    uintptr_t (__fastcall *fields)(void *, const char *);
} sh_decl_script_api;
sh_decl_script_schema *sh_decl_script_native_test_capture(sh_decl_script_api api, char *error, size_t capacity);
#endif
#endif
