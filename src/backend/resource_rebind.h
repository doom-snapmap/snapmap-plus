/* Relocate verified dependency records in cooked resource payloads. */
#ifndef SH_RESOURCE_REBIND_H
#define SH_RESOURCE_REBIND_H
#include <stddef.h>

typedef enum sh_resource_rebind_format {
    SH_REBIND_STATIC_MODEL,
    SH_REBIND_MD6_MESH,
    SH_REBIND_MD6_ANIMATION,
    SH_REBIND_DISCRETE_MODEL,
    SH_REBIND_STATIC_PARTICLE
} sh_resource_rebind_format;

/* 0 retains the authored name, 1 supplies a replacement, negative aborts.
 * Replacement storage is borrowed only until this callback returns. Family
 * is the native catalog identity, not a guessed filename extension. */
typedef int (*sh_resource_binding_resolver)(void *context, const char *family,
    const char *name, const char **replacement);

typedef int (*sh_resource_binding_site)(void *context, const char *family, const char *name,
    size_t offset, size_t length);
typedef int (*sh_resource_binding_reader)(void *context, const unsigned char *body, size_t length,
    sh_resource_binding_site visitor, void *visitor_context, char *error, size_t capacity);
typedef enum sh_resource_binding_encoding {
    SH_BINDING_IDSTR,
    SH_BINDING_NATIVE_NAME /* Exact name bytes inside a verified native token. */
} sh_resource_binding_encoding;

/* Shared transaction for verified custom readers. A native-name site excludes
 * surrounding quotes and suffix operators. The caller retains the native token
 * form and must supply replacements valid in that reader's grammar. */
unsigned char *sh_resource_rebind_with(sh_resource_binding_reader reader, void *reader_context,
    sh_resource_binding_encoding encoding, const unsigned char *body, size_t length,
    sh_resource_binding_resolver resolver, void *context,
    size_t *output_length, size_t *changed, char *error, size_t capacity);

/* Read the selected verified envelope, collect relocations, then publish one
 * malloc-owned payload. Geometry and every unselected byte are retained exactly;
 * authored files are never written. Unknown format, failed reader, bad binding
 * or allocation failure returns NULL and clears length/changed. A no-op returns
 * an exact copy. This preserves format records; it does not certify native
 * material checksums, cache lifetime or later consumers of resource names. */
unsigned char *sh_resource_rebind(sh_resource_rebind_format format,
    const unsigned char *body, size_t length, sh_resource_binding_resolver resolver,
    void *context, size_t *output_length, size_t *changed, char *error, size_t capacity);

#endif
