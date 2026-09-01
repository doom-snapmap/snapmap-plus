/* decl_server_path.h -- pure path-to-decl-identity normalization. */
#ifndef BACKEND_DECL_SERVER_PATH_H
#define BACKEND_DECL_SERVER_PATH_H

#include <stddef.h>

#define SH_DECL_SERVER_TYPE_CAP   64
#define SH_DECL_SERVER_NAME_CAP   512
#define SH_DECL_SERVER_SOURCE_CAP 768

/* One path-derived identity awaiting snapshot admission. The caller owns the
 * pointed-to strings and optional value. Sorting is deterministic and ASCII
 * case-insensitive for type/name identity, with source spelling as the final
 * stable tie-breaker. */
typedef struct sh_decl_server_order_item {
    const char *type;
    const char *name;
    const char *source;
    void *value;
    int duplicate;
    int admitted;
} sh_decl_server_order_item;

/* Convert a path relative to overrides/generated/decls into the two identities
 * the engine API requires. Example:
 *
 *   actormodifier/actormodifier/demon/cacodemon.decl
 *       type   = actormodifier
 *       name   = actormodifier/demon/cacodemon
 *       source = generated/decls/actormodifier/actormodifier/demon/cacodemon.decl
 *
 * Both slash styles are accepted. Absolute paths, traversal, empty segments,
 * whitespace/control bytes, punctuation outside the portable unquoted decl
 * token alphabet, non-.decl files, and truncated outputs are refused. `reason`
 * receives a static diagnostic string. */
int sh_decl_server_identity_from_relative(const char *relative,
                                          char *type, size_t type_cap,
                                          char *name, size_t name_cap,
                                          char *source, size_t source_cap,
                                          const char **reason);

/* Sort a complete discovered metadata set, mark every member of each
 * case-insensitive type/name collision group, then admit up to `limit`
 * non-colliding entries. Admission is frozen before file bodies are read, so
 * later per-file refusals cannot change which identities won the snapshot.
 * Returns the number of admitted entries. */
size_t sh_decl_server_order_and_admit(sh_decl_server_order_item *items,
                                      size_t count, size_t limit);

#endif /* BACKEND_DECL_SERVER_PATH_H */
