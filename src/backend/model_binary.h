#ifndef SH_MODEL_BINARY_H
#define SH_MODEL_BINARY_H

#include <stddef.h>

/* Cooked static model (.bmodel) reader, for dependency inspection only.
 *
 * A cooked model binds one or more materials per surface, so a package that
 * supplies a model also needs those materials. The main surface material and
 * trailing material bindings are dependencies too, even without a patch table.
 * This reader walks the envelope
 * far enough to report them and to prove the file is the format it claims; it
 * does not decode geometry and it never writes.
 *
 * Layout from the engine's own writer (idStaticModel::WriteStaticBModel, Vulkan
 * FUN_1414d0be0): magic 0x424d4c1b, a big-endian stamp, a big-endian surface
 * count, then per surface an idStr name, three big-endian fields, a big-endian
 * material count, that many idStr material names, an opaque geometry block, and
 * the magic again as the surface terminator; then a big-endian trailing count
 * and that many (idStr name + three big-endian fields) records, ending exactly
 * at end of file. idStr is a little-endian length followed by unterminated
 * bytes. Because the geometry block is opaque, a candidate terminator is
 * accepted only when the remainder of the file parses and ends exactly.
 *
 * Recognized older containers keep their verified shorter surface headers.
 *
 * The visitor receives "material" identities with the file's own bytes,
 * NUL-terminated. Return 0 to abort. Returns 1 when the whole envelope was read
 * and every visit was accepted; 0 otherwise, with the reason in error. */
typedef int (*sh_model_binary_visitor)(void *context, const char *type, const char *name);

int sh_model_references(const unsigned char *body, size_t length,
    sh_model_binary_visitor visitor, void *context, char *error, size_t capacity);

/* Same verified walk, reporting the original little-endian idStr record span
 * (including its four-byte length). Queue edits until the complete walk passes;
 * the source buffer stays borrowed and must not change during traversal. */
typedef int (*sh_model_binary_site_visitor)(void *context, const char *type, const char *name,
    size_t offset, size_t length);
int sh_model_reference_sites(const unsigned char *body, size_t length,
    sh_model_binary_site_visitor visitor, void *context, char *error, size_t capacity);

/* Discrete-animation .dmodel envelope: collision hulls, material table,
 * transforms, geometry surfaces, material remaps and optional base collision.
 * Geometry stays opaque. Only verified material/cm idStr records are exposed;
 * the optional map collision request is resolved as its native loader does. */
int sh_discrete_model_references(const unsigned char *body, size_t length,
    sh_model_binary_visitor visitor, void *context, char *error, size_t capacity);
int sh_discrete_model_reference_sites(const unsigned char *body, size_t length,
    sh_model_binary_site_visitor visitor, void *context, char *error, size_t capacity);

/* Static particle .pmodel data contains a source stamp and counted 48-byte
 * vertices, with no resource references. Validate the complete native envelope
 * before retaining it as a leaf. Vertex bytes and the stamp stay untouched. */
int sh_particle_model_read(const unsigned char *body, size_t length,
    char *error, size_t capacity);

#endif
