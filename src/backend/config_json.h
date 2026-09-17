/* Validate bounded JSON values and retain object members as raw fragments.
 * Decode keys and reject duplicates at every nesting level. This lets
 * settings updates preserve unknown values and original number spellings.
 */
#ifndef SH_CONFIG_JSON_H
#define SH_CONFIG_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sh_json_kind {
    SH_JSON_NULL,
    SH_JSON_BOOL,
    SH_JSON_NUMBER,
    SH_JSON_STRING,
    SH_JSON_ARRAY,
    SH_JSON_OBJECT
} sh_json_kind;

typedef struct sh_json_member {
    char *key;
    size_t key_length;
    char *value_json;
} sh_json_member;

typedef struct sh_json_object {
    sh_json_member *members;
    size_t count;
    size_t capacity;
} sh_json_object;

/* Read-only member spans for a postorder object visitor. Spans and decoded
 * keys are valid only during the callback; values reference the input buffer.
 * A zero callback result refuses traversal. No partial result is trustworthy
 * until the complete document validates. */
typedef struct sh_json_field_span {
    char *key;
    size_t key_length;
    const char *value;
    size_t value_length;
    sh_json_kind kind;
} sh_json_field_span;
typedef int (*sh_json_object_visitor)(void *context, const sh_json_field_span *fields,
                                      size_t count, unsigned depth);
int sh_json_visit_objects(const char *json, size_t length, unsigned max_depth,
                          sh_json_object_visitor visitor, void *context);

/* The field filter sees each decoded member key before its value is scanned.
 * Positive visits normally, zero suppresses callbacks throughout that value,
 * and negative aborts. Suppressed values still receive full JSON validation,
 * including duplicate keys and depth checks. depth is the containing object's
 * depth. The same context is passed to both callbacks. */
typedef int (*sh_json_field_filter)(void *context, const char *key,
                                     size_t key_length, unsigned depth);
int sh_json_visit_objects_filtered(const char *json, size_t length, unsigned max_depth,
                                   sh_json_object_visitor visitor,
                                   sh_json_field_filter filter, void *context);

int sh_json_validate(const char *json, size_t length, unsigned max_depth,
                     sh_json_kind *out_kind);
/* Failure positions are zero-based byte offsets into the supplied buffer.
 * reason is static text; neither it nor the input is owned by the result. */
typedef struct sh_json_error {
    size_t offset;
    const char *reason;
} sh_json_error;
int sh_json_validate_ex(const char *json, size_t length, unsigned max_depth,
                        sh_json_kind *out_kind, sh_json_error *error);
int sh_json_parse_object(const char *json, size_t length, unsigned max_depth,
                         sh_json_object *out);
const char *sh_json_object_get(const sh_json_object *object, const char *key);
int sh_json_object_set(sh_json_object *object, const char *key,
                       const char *value_json, unsigned max_depth);
int sh_json_object_set_n(sh_json_object *object, const char *key,
                         size_t key_length, const char *value_json,
                         unsigned max_depth);
int sh_json_decode_string(const char *json, size_t length,
                          char *out, size_t out_capacity, size_t *out_length);
char *sh_json_serialize_object(const sh_json_object *object,
                               unsigned base_indent, size_t *out_length);
void sh_json_object_free(sh_json_object *object);

#ifdef __cplusplus
}
#endif

#endif /* SH_CONFIG_JSON_H */
