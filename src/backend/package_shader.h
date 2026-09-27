/* Portable inspection of native render-program units and reference records. */
#ifndef SH_PACKAGE_SHADER_H
#define SH_PACKAGE_SHADER_H
#include <stddef.h>
#include <stdint.h>

/* Native shader include opens may spell a virtual-root path with a leading
 * slash. Return its borrowed catalog-relative spelling; this does not resolve
 * host filesystem paths or change authored package path validation. */
static inline const char *sh_package_shader_input_path(const char *path)
{
    if (path) while (*path == '/' || *path == '\\') path++;
    return path;
}

typedef enum sh_package_shader_renderer {
    SH_PACKAGE_SHADER_OPENGL, SH_PACKAGE_SHADER_VULKAN
} sh_package_shader_renderer;
typedef enum sh_package_shader_status {
    SH_PACKAGE_SHADER_ERROR = -1,
    SH_PACKAGE_SHADER_UNSUPPORTED = 0,
    SH_PACKAGE_SHADER_OK = 1
} sh_package_shader_status;
typedef enum sh_package_shader_reference_kind {
    SH_PACKAGE_SHADER_RESOURCE,
    SH_PACKAGE_SHADER_PARAMETER, /* Native/GPU ABI name; never alias in this codec. */
    SH_PACKAGE_SHADER_INCLUDE,   /* Source path and timestamp, not a resource ID. */
    SH_PACKAGE_SHADER_FLAG       /* Native renderProgFlag mask; never alias. */
} sh_package_shader_reference_kind;
typedef struct sh_package_shader_reference {
    sh_package_shader_reference_kind kind;
    const char *family;
    char *name;
    size_t offset, length; /* Complete little-endian length-prefixed string. */
    uint32_t source_stamp; /* Only INCLUDE: native source timestamp, not a hash. */
} sh_package_shader_reference;
typedef struct sh_package_shader_stage {
    size_t offset, length; /* Borrowed input's exact embedded GLSL byte span. */
    uint32_t checksum;
} sh_package_shader_stage;
typedef struct sh_package_shader_program {
    uint32_t declaration_checksum;
    uint64_t permutation_mask;
    uint64_t *permutations;
    size_t permutation_count;
    sh_package_shader_reference *references;
    size_t reference_count;
    sh_package_shader_stage stages[3]; /* Vertex, fragment, compute. */
} sh_package_shader_program;
typedef struct sh_package_shader_schema {
    void *context;
    /* Used only where the native kind determines a value record's width/type.
     * 1 resolves a source-bound native kind (0..18), 0 lacks coverage, -1 aborts.
     * Do not infer a kind from a spelling or another package's loaded object. */
    int (*parameter_kind)(void *context, const char *name, int *kind);
} sh_package_shader_schema;

/* Parse the complete 0xb68800ad envelope used by both renderers. Missing kinds
 * and unverified records are UNSUPPORTED; malformed framing/allocation is ERROR.
 * Fixed parameter-name lists and flag names need no kind to decode; their ABI bindings still
 * require caller validation. Output must start empty and remains empty on
 * failure. Input stays borrowed.
 * This proves record framing/references, not GLSL/SPIR-V validity, parameter ABI
 * compatibility, include timestamps, or driver/native construction acceptance. */
int sh_package_shader_decode(const unsigned char *body, size_t length,
    const sh_package_shader_schema *schema, sh_package_shader_program *out,
    char *error, size_t capacity);
void sh_package_shader_program_clear(sh_package_shader_program *program);

/* Native MD5-fold checksum: XOR the four little-endian digest words. This is
 * only a loader freshness check, never payload equality or a native resource ID.
 * The declaration form trims trailing space/CR/LF, exactly as the native setter.
 * A trailing tab is significant. Nonempty inputs require a non-NULL buffer. */
uint32_t sh_package_shader_checksum(const unsigned char *body, size_t length);
uint32_t sh_package_shader_declaration_checksum(const unsigned char *body, size_t length);

typedef struct sh_package_shader_source { const unsigned char *body; size_t length; } sh_package_shader_source;
/* Return 0 to retain, 1 with a borrowed replacement, or negative to abort.
 * Every record is visited, including separately typed ABI names/include paths.
 * Changing either of those is UNSUPPORTED, not silently accepted. */
typedef int (*sh_package_shader_resolver)(void *context,
    const sh_package_shader_reference *reference, const char **replacement);
/* Preserve all unselected bytes and embedded stages. The caller must prove
 * the new declaration differs only by the same verified reference relocation;
 * changing shader code/state requires a matching recook, not a refreshed hash.
 * Original declaration checksum must match the input. Input is never written.
 * Return status; output is malloc-owned, exact on a no-op, empty on failure. */
int sh_package_shader_rebind(const unsigned char *body, size_t length,
    const sh_package_shader_schema *schema, sh_package_shader_source original_decl,
    sh_package_shader_source linked_decl, sh_package_shader_resolver resolver, void *context,
    unsigned char **output, size_t *output_length, size_t *changed, char *error, size_t capacity);

/* Validate the Vulkan 16-byte stage envelope against the exact GLSL stage.
 * Requires version 4, matching source checksum and a complete little-endian
 * SPIR-V word stream. The third native header word is retained but is not a
 * relocation target. Structural acceptance is not GPU semantic validation. */
int sh_package_shader_spirv(const unsigned char *body, size_t length,
    const unsigned char *source, size_t source_length, char *error, size_t capacity);

typedef enum sh_package_shader_path_kind {
    SH_PACKAGE_SHADER_DECLARATION, SH_PACKAGE_SHADER_BINARY,
    SH_PACKAGE_SHADER_SPIRV, SH_PACKAGE_SHADER_PERMUTATION,
    SH_PACKAGE_SHADER_SOURCE_INCLUDE
} sh_package_shader_path_kind;
/* Paths are borrowed during the callback. PERMUTATION is also a native
 * renderprog identity: check its native ID, capture/decode it, then enumerate
 * its own companions with this same API. INCLUDE paths remain shared here.
 * No optional OpenGL driver cache or generated GLSL debug output is required.
 * Collect paths transactionally: a later failure can follow earlier visits. */
typedef int (*sh_package_shader_path_visitor)(void *context,
    sh_package_shader_path_kind kind, const char *original, const char *linked);
int sh_package_shader_paths(sh_package_shader_renderer renderer,
    const char *name, const char *linked_name, const sh_package_shader_program *program,
    sh_package_shader_path_visitor visitor, void *context, char *error, size_t capacity);
#endif
