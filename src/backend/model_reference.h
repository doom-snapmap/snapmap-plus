/* Native render-model requests can bind several different resource managers. */
#ifndef SH_MODEL_REFERENCE_H
#define SH_MODEL_REFERENCE_H
#include <stddef.h>

typedef struct sh_model_reference {
    const char *family;
    char *name;                 /* owned resource manager identity */
    const char *request_suffix;
    size_t resource_suffix_length;
} sh_model_reference;

/* Read the renderer's common model dispatch contract. 1 names a resource,
 * 0 is a null/runtime-generated model, -1 is unsupported or failed. Requests
 * to particle, cloth, MD6, breakable and hotspot preserve their actual manager
 * spelling rules. Water/automap custom file loaders require their own adapters.
 * No engine calls or filename-existence guesses. Initialize output to zero. */
int sh_model_reference_read(const char *request, sh_model_reference *out);
/* The SnapMap editor's preview dispatcher selects the same managers with the
 * same spelling rules, except that "fx" builds an empty model carrying an
 * effect name (unsupported here) and _empty names the engine's empty model. */
int sh_model_preview_reference_read(const char *request, sh_model_reference *out);
/* Reconstruct a model-field spelling from a replacement manager identity.
 * The caller preserves dispatch-significant suffixes in that identity. */
char *sh_model_reference_bind(const sh_model_reference *reference, const char *replacement);
void sh_model_reference_clear(sh_model_reference *reference);
#endif
