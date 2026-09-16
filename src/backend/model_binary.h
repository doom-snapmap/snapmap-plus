#ifndef SH_MODEL_BINARY_H
#define SH_MODEL_BINARY_H

#include <stddef.h>

/* Cooked static model (.bmodel) reader, for dependency inspection only.
 *
 * A cooked model binds one or more materials per surface, so a package that
 * supplies a model also needs those materials. This reader walks the envelope
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
 * Older revisions of this container exist (the engine calls them
 * BMODEL_MAGIC_PREVIOUS) and are declined rather than guessed at.
 *
 * The visitor receives "material" identities with the file's own bytes,
 * NUL-terminated. Return 0 to abort. Returns 1 when the whole envelope was read
 * and every visit was accepted; 0 otherwise, with the reason in error. */
typedef int (*sh_model_binary_visitor)(void *context, const char *type, const char *name);

int sh_model_references(const unsigned char *body, size_t length,
    sh_model_binary_visitor visitor, void *context, char *error, size_t capacity);

#endif
