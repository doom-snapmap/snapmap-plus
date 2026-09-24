/* Owned script-module metadata for declaration dependency inspection. */
#ifndef SH_DECL_SCRIPT_SCHEMA_H
#define SH_DECL_SCRIPT_SCHEMA_H
#include <stddef.h>
#include <stdint.h>

typedef struct sh_decl_script_field {
    char *name, *type, *ops;
    struct sh_decl_script_field *next;
} sh_decl_script_field;
typedef struct sh_decl_script_type {
    char *name;
    sh_decl_script_field *fields;
    int constructible, has_fields;
    struct sh_decl_script_type *next;
} sh_decl_script_type;
typedef struct sh_decl_script_schema sh_decl_script_schema;
typedef struct sh_decl_script_source {
    void *context;
    int (*read)(void *, uintptr_t, void *, size_t);
    /* NULL-terminated native array of class-name pointers. */
    uintptr_t names;
    /* 1 table, 0 verified absence, -1 failure. Capture may invoke an engine
     * lookup only at its admitted main-thread boundary. A dependency walk
     * never invokes this callback. The caller owns engine context restoration. */
    int (*lookup)(void *, const char *, uintptr_t *);
} sh_decl_script_source;

/* Copy the native 40-byte field records and their transitive script types.
 * Negative lookups are retained too. Partial snapshots are never returned;
 * no resource/field-string pointer remains borrowed from the module. */
sh_decl_script_schema *sh_decl_script_schema_capture(sh_decl_script_source source,
    char *error, size_t capacity);
const sh_decl_script_type *sh_decl_script_schema_find(const sh_decl_script_schema *, const char *);
const sh_decl_script_type *sh_decl_script_schema_first(const sh_decl_script_schema *);
void sh_decl_script_schema_free(sh_decl_script_schema *);
/* Script dispatcher template spelling. Owned trimmed inner spelling; NULL
 * means this is not the named template or its spelling is incomplete. */
char *sh_decl_script_template(const char *name, const char *prefix);
#endif
