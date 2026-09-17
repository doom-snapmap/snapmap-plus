/* Shared legacy conversion policy. No filesystem writes or engine calls.
 * Both disk migration and map delivery feed verified files into this planner.
 * The caller owns copying, resource lookup, backups and atomic publication. */
#ifndef SH_PACKAGE_MIGRATION_H
#define SH_PACKAGE_MIGRATION_H
#include "config_json.h"

typedef struct sh_package_migration sh_package_migration;

/* Missing/empty legacy markers are accepted. IDs receive the descriptor's
 * case/whitespace normalization; other invalid identities are not guessed.
 * fallback_id/name come from the delivery adapter. */
sh_package_migration *sh_package_migration_open(const char *descriptor, size_t length,
    const char *fallback_id, const char *fallback_name, int legacy,
    char *error, size_t capacity);
/* File classification is shared, including partially migrated assets/ trees.
 * native means the installed catalog recognizes that assets-relative path.
 * body is needed only for legacy policy files. Directories may omit it. */
int sh_package_migration_add(sh_package_migration *migration, const char *path,
    int directory, int native, const char *body, size_t length);
/* Returns owned JSON {descriptor: object, files: {source: {target, directory}},
 * imports: {provider: {type,name,origin}}}. No asset bytes are serialized here.
 * Caller checks duplicate output bytes before choosing an identical source. */
char *sh_package_migration_finish(sh_package_migration *migration, size_t *length);
void sh_package_migration_free(sh_package_migration *migration);
/* Shared adapter helpers. The caller frees namespace results with free(). */
char *sh_package_migration_namespace(const char *path);
int sh_package_migration_policy(const char *path);
/* Length-delimited interface for language adapters. Request holds descriptor,
 * id and name strings, legacy boolean, and files keyed by original path with
 * directory/native booleans and optional policy body string. */
char *sh_package_migration_run(const char *request, size_t length,
    size_t *output_length, char *error, size_t capacity);
void sh_package_migration_release(void *memory);
#endif
