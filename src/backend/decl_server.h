/* decl_server.h -- restart-time source-catalog registration for new engine decl identities. */
#ifndef BACKEND_DECL_SERVER_H
#define BACKEND_DECL_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Snapshot overrides/generated/decls plus linked installed .decl resources,
 * register an internal command, and enqueue that command through DOOM's command
 * buffer. The command executes exactly once on the engine main thread. Existing
 * source records and live identities remain file-shadow overrides; only absent
 * identities are copied to an immutable exact decltree table and scanned through
 * the engine's native DeclRegisterFile virtual method in dependency order. After every scan,
 * each registered identity is given a live object in its own decl manager in
 * that same order, because the native decl parsers resolve their own inherit
 * and entityDef edges with makeDefault=0 and only find targets that already
 * exist. Eligible new snapEditorEntityDef roots are then held to the native
 * palette validator contract -- a resolved entityDef plus correctly flagged
 * input and output targets -- before the palette rebuild is called;
 * source-only abstract bodies are retained as NON-PALETTE without that call.
 * Each source-path argument is a native 48-byte idStr temporary, constructed
 * and destroyed around its one call.
 *
 * A non-empty snapshot is armed at install, and published from a one-shot
 * detour on the engine's whole-registry resource promotion -- the single pass
 * in idCommonLocal::Init that makes every resource alive at that instant
 * permanent. Publishing immediately before it is what puts new content in the
 * same static set, by the same engine pass, as the shipped editor content it
 * depends on; publishing after it (the former load-state RUNNING trigger) left
 * new content map-scoped, so the first playtest destroyed it. Returns 1 when
 * work was armed or there was nothing to do, 0 when the service was refused. */
int sh_decl_server_install(const sig_result *results, size_t count,
                           const uint8_t *module_base, void *cmdsys);

/* Returns 1 only after the one-shot command has registered every missing
 * candidate, materialized required editor decls, and completed the palette
 * rebuild successfully. DS_STATE_DONE alone is not sufficient: it also
 * represents disabled, empty, or all-shadowed snapshots. */
int sh_decl_server_registration_succeeded(void);

#ifdef SH_DECL_SERVER_TESTING
#include <windows.h>

typedef struct sh_decl_server_test_find_api {
    HANDLE (WINAPI *find_first)(LPCSTR pattern, LPWIN32_FIND_DATAA found);
    BOOL (WINAPI *find_next)(HANDLE search, LPWIN32_FIND_DATAA found);
    BOOL (WINAPI *find_close)(HANDLE search);
    DWORD (WINAPI *get_last_error)(void);
} sh_decl_server_test_find_api;

int sh_decl_server_test_body_is_single_block(const unsigned char *body, size_t length);
int sh_decl_server_test_checked_add(size_t value, size_t add, size_t *result);

void sh_decl_server_test_set_find_api(const sh_decl_server_test_find_api *api);
void sh_decl_server_test_reset_find_api(void);
int sh_decl_server_test_walk(const char *directory, const char *relative,
                             size_t *retained_count);
int sh_decl_server_test_find_first_status(int found, unsigned long error);
int sh_decl_server_test_find_next_status(int found, unsigned long error);
int sh_decl_server_test_root_attributes_status(int found, unsigned long error);

typedef void *(*sh_decl_server_test_idstr_ctor_fn)(void *self, const char *source);
typedef void (*sh_decl_server_test_idstr_dtor_fn)(void *self);
typedef unsigned char (*sh_decl_server_test_register_file_fn)(
    void *registry, const void *source, void *default_type_manager);

typedef void *(*sh_decl_server_test_type_by_name_fn)(void *registry,
                                                     const char *short_name);
typedef void *(*sh_decl_server_test_source_find_fn)(void *type_manager,
                                                     const char *logical_name);
typedef void *(*sh_decl_server_test_find_decl_fn)(void *type_manager,
                                                  const char *logical_name,
                                                  unsigned char make_default);
typedef int (*sh_decl_server_test_palette_refresh_fn)(void);

enum {
    SH_DECL_SERVER_TEST_UNCLASSIFIED = 0,
    SH_DECL_SERVER_TEST_MISSING = 1,
    SH_DECL_SERVER_TEST_SHADOWED = 2,
    SH_DECL_SERVER_TEST_REFUSED = 3
};

/* Optional shadow provenance for materialization tests. Zero keeps the
 * historical test seam behavior (a shadowed item is treated as a source
 * shadow unless a valid live object can be reused). Production classification
 * always supplies one of these explicit values. */
enum {
    SH_DECL_SERVER_TEST_SHADOW_NONE = 0,
    SH_DECL_SERVER_TEST_SHADOW_SOURCE = 1,
    SH_DECL_SERVER_TEST_SHADOW_LIVE = 2
};

enum {
    SH_DECL_SERVER_TEST_CLASSIFY_MISSING = 1,
    SH_DECL_SERVER_TEST_CLASSIFY_SHADOWED_SOURCE = 2,
    SH_DECL_SERVER_TEST_CLASSIFY_SHADOWED_LIVE = 3,
    SH_DECL_SERVER_TEST_CLASSIFY_REFUSED_TYPE = 4,
    SH_DECL_SERVER_TEST_CLASSIFY_TERMINAL = 5
};

enum {
    SH_DECL_SERVER_TEST_PHASE_NONE = 0,
    SH_DECL_SERVER_TEST_PHASE_SCAN = 1,
    SH_DECL_SERVER_TEST_PHASE_MATERIALIZATION = 2,
    SH_DECL_SERVER_TEST_PHASE_PALETTE = 3
};

typedef struct sh_decl_server_test_materialize_item {
    const char *type;
    const char *name;
    const char *source;
    int outcome;
    const unsigned char *body;
    size_t body_length;
    int shadow_kind;
} sh_decl_server_test_materialize_item;

/* Exercise the production source-first classification order. The return value
 * distinguishes an existing native source record from a live decl fallback;
 * SH_DECL_SERVER_TEST_CLASSIFY_TERMINAL represents an engine exception and is
 * terminal for the one-shot command. */
int sh_decl_server_test_classify_candidate(
    const char *type, const char *name, void *registry,
    sh_decl_server_test_type_by_name_fn type_by_name,
    sh_decl_server_test_source_find_fn source_find,
    sh_decl_server_test_find_decl_fn find_decl);

/* Exercise the native idStr marshalling and one-source scanner boundary without
 * touching a live engine. The return value is the internal registration status:
 * 1 success, 0 constructor exception, -1 scanner exception, -2 scanner false,
 * or -3 destructor exception. */
int sh_decl_server_test_register_candidate(
    void *registry,
    const char *source_name,
    sh_decl_server_test_idstr_ctor_fn ctor,
    sh_decl_server_test_idstr_dtor_fn dtor,
    sh_decl_server_test_register_file_fn register_file);

/* Run the production two-phase materialization pass against a test candidate
 * table. This keeps the native make-default=1, state-byte, and terminal-fault
 * contract executable without requiring a live engine. */
int sh_decl_server_test_materialize_missing_sedefs(
    const sh_decl_server_test_materialize_item *items, size_t count,
    void *registry, sh_decl_server_test_type_by_name_fn type_by_name,
    sh_decl_server_test_source_find_fn source_find,
    sh_decl_server_test_find_decl_fn find_decl, int *materialized);

/* Exercise the production scan-then-materialize loop, including its terminal
 * phase result, without requiring a live engine. */
int sh_decl_server_test_scan_and_materialize_missing(
    const sh_decl_server_test_materialize_item *items, size_t count,
    void *registry, sh_decl_server_test_type_by_name_fn type_by_name,
    sh_decl_server_test_source_find_fn source_find,
    sh_decl_server_test_find_decl_fn find_decl,
    sh_decl_server_test_palette_refresh_fn palette_refresh,
    sh_decl_server_test_idstr_ctor_fn ctor,
    sh_decl_server_test_idstr_dtor_fn dtor,
    sh_decl_server_test_register_file_fn register_file,
    int *registered, int *materialized, int *failure_phase);
#endif

#endif /* BACKEND_DECL_SERVER_H */
