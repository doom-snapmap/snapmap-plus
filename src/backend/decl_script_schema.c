#include "decl_script_schema.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sh_decl_script_schema { sh_decl_script_type *types, **tail; };
const sh_decl_script_type *sh_decl_script_schema_first(const sh_decl_script_schema *schema)
{ return schema ? schema->types : NULL; }

static char *sc_copy(const char *value, size_t size)
{
    char *copy;
    if (size == SIZE_MAX || !(copy = malloc(size + 1))) return NULL;
    memcpy(copy, value, size); copy[size] = 0; return copy;
}
char *sh_decl_script_template(const char *name, const char *prefix)
{
    size_t length, start, end;
    if (!name || !prefix) return NULL;
    length = strlen(name); start = strlen(prefix);
    if (length <= start || strncmp(name, prefix, start) || name[length - 1] != '>') return NULL;
    end = length - 1;
    while (start < end && (name[start] == ' ' || name[start] == '\t')) start++;
    while (end > start && (name[end - 1] == ' ' || name[end - 1] == '\t')) end--;
    return sc_copy(name + start, end - start);
}
const sh_decl_script_type *sh_decl_script_schema_find(const sh_decl_script_schema *schema, const char *name)
{
    const sh_decl_script_type *type;
    if (!schema || !name) return NULL;
    for (type = schema->types; type; type = type->next)
        if (!strcmp(type->name, name)) return type;
    return NULL;
}
static sh_decl_script_type *sc_add(sh_decl_script_schema *schema, const char *name)
{
    sh_decl_script_type *type = (sh_decl_script_type *)sh_decl_script_schema_find(schema, name);
    if (type) return type;
    type = calloc(1, sizeof(*type));
    if (!type) return NULL;
    if (!(type->name = sc_copy(name, strlen(name)))) { free(type); return NULL; }
    *schema->tail = type; schema->tail = &type->next; return type;
}
static char *sc_string(sh_decl_script_source *source, uintptr_t address)
{
    char *text = NULL; size_t size = 0, capacity = 0;
    if (!address) return NULL;
    for (;;) {
        char c;
        if (size > UINTPTR_MAX - address || !source->read(source->context, address + size, &c, 1)) break;
        if (size == capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            char *grown;
            if (next < capacity || !(grown = realloc(text, next))) break;
            text = grown; capacity = next;
        }
        text[size++] = c;
        if (!c) return text;
    }
    free(text); return NULL;
}
static int sc_read(sh_decl_script_source *source, uintptr_t address, void *out, size_t size)
{ return address && size <= UINTPTR_MAX - address && source->read(source->context, address, out, size); }

sh_decl_script_schema *sh_decl_script_schema_capture(sh_decl_script_source source, char *error, size_t capacity)
{
    sh_decl_script_schema *schema = NULL;
    sh_decl_script_type *type;
    uintptr_t cursor = source.names, pointer;
    const char *reason = "script metadata source is unavailable";
    if (error && capacity) *error = 0;
    if (!source.names || !source.read || !source.lookup || !(schema = calloc(1, sizeof(*schema)))) goto fail;
    schema->tail = &schema->types;
    for (;;) {
        char *name;
        reason = "script class names are unreadable";
        if (!sc_read(&source, cursor, &pointer, sizeof(pointer))) goto fail;
        if (!pointer) break;
        name = sc_string(&source, pointer);
        if (!name || !*name) { free(name); goto fail; }
        type = sc_add(schema, name); free(name);
        if (!type) goto fail;
        type->constructible = 1;
        if (cursor > UINTPTR_MAX - sizeof(pointer)) goto fail;
        cursor += sizeof(pointer);
    }
    /* Appending dependencies to the queue also bounds cycles and repeated
     * spellings: each type, including an absent one, is queried exactly once. */
    for (type = schema->types; type; type = type->next) {
        sh_decl_script_field **tail = &type->fields;
        int found;
        cursor = 0;
        reason = "script field lookup failed";
        found = source.lookup(source.context, type->name, &cursor);
        if (found < 0 || found > 1 || (found && !cursor) || (!found && cursor)) goto fail;
        type->has_fields = found != 0;
        if (!found) continue;
        for (;;) {
            uintptr_t row[5];
            sh_decl_script_field *field;
            char *inner;
            reason = "script field table is unreadable";
            if (!sc_read(&source, cursor, row, sizeof(row[0]))) goto fail;
            if (!row[0]) break;
            if (!sc_read(&source, cursor, row, sizeof(row))) goto fail;
            field = calloc(1, sizeof(*field));
            if (!field) goto fail;
            *tail = field; tail = &field->next;
            field->type = sc_string(&source, row[0]);
            field->ops = sc_string(&source, row[1]);
            field->name = sc_string(&source, row[2]);
            if (!field->type || !*field->type || !field->ops || !field->name || !*field->name) goto fail;
            if (!sc_add(schema, field->type)) goto fail;
            inner = sh_decl_script_template(field->type, "ssList<");
            while (inner) {
                char *next;
                if (!*inner || !sc_add(schema, inner)) { free(inner); goto fail; }
                next = sh_decl_script_template(inner, "ssList<"); free(inner); inner = next;
            }
            if (cursor > UINTPTR_MAX - sizeof(row)) goto fail;
            cursor += sizeof(row);
        }
    }
    return schema;
fail:
    if (error && capacity) snprintf(error, capacity, "%s", reason);
    sh_decl_script_schema_free(schema); return NULL;
}
void sh_decl_script_schema_free(sh_decl_script_schema *schema)
{
    if (!schema) return;
    while (schema->types) {
        sh_decl_script_type *type = schema->types;
        schema->types = type->next;
        while (type->fields) {
            sh_decl_script_field *field = type->fields;
            type->fields = field->next;
            free(field->name); free(field->type); free(field->ops); free(field);
        }
        free(type->name); free(type);
    }
    free(schema);
}
