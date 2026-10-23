/* Installed native companions used by package resource compilation. */
#ifndef SH_PACKAGE_NATIVE_ASSETS_H
#define SH_PACKAGE_NATIVE_ASSETS_H
#include "package_library.h"

typedef struct sh_package_native_assets sh_package_native_assets;
typedef struct sh_package_shader_preparation sh_package_shader_preparation;
typedef struct sh_package_shader_request {
    const char *family, *name;
    const unsigned char *source;
    size_t source_length;
    uintptr_t manager, resource; /* resource may be zero for an unloaded slot. */
    uint32_t native_id;
    uint64_t task_id; /* Stable for this immutable original path/asset handle. */
} sh_package_shader_request;
/* Missing construction proof is an environment task. Inspect pending before
 * classifying a failed compilation or excluding any package. Taking the batch
 * retains this asset handle and its exact source expectations. Retry the same
 * immutable candidate after a successful run; no source rescanning is implied. */
size_t sh_package_native_assets_shader_pending(sh_package_native_assets *assets);
/* An installed-library compilation brackets itself (on, then off) so the
 * stock shader objects it needed -- those it requested native preparation for
 * and cooked programs it read -- become a later session's preparation hint.
 * Turning recording on starts an empty record. */
void sh_package_native_assets_shader_record(sh_package_native_assets *assets, int on);
/* Queue native preparation of one stock shader object an earlier session
 * needed, before any compilation reads it: the same check a compilation makes,
 * so a ready or already cooked object queues nothing. 1 queued, 0 nothing
 * queued, -1 no installed original (a stale name). A hint only: compilations
 * still verify and request exactly as without it. */
int sh_package_native_assets_shader_prewarm(sh_package_native_assets *assets, const char *family, const char *name);
/* The recorded compilation's needs, sorted malloc-owned "family\tname\n" lines. */
char *sh_package_native_assets_shader_needs(sh_package_native_assets *assets);
/* OpenGL programs cooked here name further programs through program-typed
 * parameters; a compilation reaches those only by reading the cooked output,
 * one level per compilation. Queue the request a compilation would make for
 * each named program that has an installed original and no cooked binary yet,
 * and for each parameter and flag a cooked binary checks that now needs native
 * preparation. Returns how many were queued; names are followed once. */
size_t sh_package_native_assets_shader_follow(sh_package_native_assets *assets);
/* Preparation hint text. format returns header, a body digest line and needs
 * (malloc-owned). read returns the number of entries visited, or -1 without
 * visiting any when the text belongs to another header (renderer, game,
 * product build or installed inventory) or is truncated or corrupt. */
/* The header a shader preparation hint belongs to: renderer, game image (the
 * "game" line of build_identity) and contract revision. 0 if it cannot be formed. */
int sh_package_shader_hint_header(char *out, size_t capacity, int vulkan, const char *build_identity);
char *sh_package_shader_hint_format(const char *header, const char *needs);
/* The framing of both: format returns header + "body <digest> <length>" line +
 * body (malloc-owned); body returns the body inside text, or NULL when the text
 * belongs to another header or is truncated or corrupt. */
char *sh_package_framed_format(const char *header, const char *body);
const char *sh_package_framed_body(const char *text, size_t length, const char *header, size_t *body_length);
int sh_package_shader_hint_read(const char *text, size_t length, const char *header,
    int (*visit)(void *context, const char *family, const char *name), void *context);
/* Bracket one compilation so its shader ABI checks share one native identity
 * snapshot instead of recapturing whole managers per parameter. Each check
 * still revalidates what it read. End revalidates the snapshot and returns 0,
 * with an environment error, when native identities changed: discard that
 * candidate. Nested scopes share the outermost; calls must pair. */
void sh_package_native_assets_shader_scope_begin(sh_package_native_assets *assets);
int sh_package_native_assets_shader_scope_end(sh_package_native_assets *assets, char *error, size_t capacity);
sh_package_shader_preparation *sh_package_native_assets_shader_take(sh_package_native_assets *assets);
size_t sh_package_shader_preparation_count(const sh_package_shader_preparation *batch);
const sh_package_shader_request *sh_package_shader_preparation_request(const sh_package_shader_preparation *batch, size_t index);
/* Diagnostics only: the reason and native receipt/text state recorded when
 * this batch's first request was queued, "" when none was. Never evidence. */
const char *sh_package_shader_preparation_first_need(const sh_package_shader_preparation *batch);
/* Borrowed retained handle: use retain() if the retry outlives the batch. Keep
 * it across retries so task_id stays stable. Seeing a previously successful
 * task again indicates changed native evidence, not monotonic preparation. */
sh_package_native_assets *sh_package_shader_preparation_assets(const sh_package_shader_preparation *batch);
typedef int (*sh_package_shader_prepare_one)(void *context,
    const sh_package_shader_request *request, char *error, size_t capacity);
typedef int (*sh_package_shader_recover)(void *context,
    const sh_package_shader_preparation *batch, char *error, size_t capacity);
/* Both callbacks run outside asset locks. Caller additionally releases refresh/
 * provider locks, admits the native main-thread boundary and owns the existing
 * provider recovery transaction. A callback's success is independently checked
 * against positive receipts; it cannot self-certify the expected input bytes.
 * Returns 1 captured, 0 failure with successful recovery, -1 recovery failure.
 * A failed batch is an environment failure, never invalid-package attribution. */
int sh_package_shader_preparation_run(sh_package_shader_preparation *batch,
    sh_package_shader_prepare_one prepare, sh_package_shader_recover recover,
    void *context, char *error, size_t capacity);
void sh_package_shader_preparation_free(sh_package_shader_preparation *batch);
/* Revoke copied source-cook products when the enclosing native state restore
 * fails after run(). The caller must block publication/retry on that failure. */
void sh_package_shader_preparation_revoke(sh_package_shader_preparation *batch);
/* doom_root is the engine installation directory, above base/. Originals are
 * captured lazily, pinned against writes/deletion and shared across refreshes.
 * This is separate from editable authored sources and never writes game files.
 * Callbacks and retain/release are thread-safe. Each caller owns a reference
 * for its entire operation; capture is serialized across refresh/query users. */
sh_package_native_assets *sh_package_native_assets_open(const char *doom_root);
sh_package_native_assets *sh_package_native_assets_retain(sh_package_native_assets *assets);
void sh_package_native_assets_close(sh_package_native_assets *assets);
/* Configure the process-owned constructor adapter's readiness check before
 * sharing this handle. The callback must not load resources. It is consulted
 * for each admitted family; source observation does not require native hooks.
 * Repeating the same callback is allowed; replacing it on a shared handle is
 * not. This is internal engine wiring, never an authored package option. */
int sh_package_native_assets_configure_consumers(sh_package_native_assets *assets,
    int (*ready)(const char *family));
/* Baseline-reader contract, restricted to native loose VMTR companions. Other
 * paths return absent so archive/product readers keep their existing routes. */
int sh_package_native_assets_read(void *context, const char *path,
    unsigned char **body, size_t *length);
/* Evidenced native source-cook products only. Consult before the installed
 * catalog so freshly generated permutation members remain one coherent unit.
 * Authored scope files still take precedence over this original-reader route. */
int sh_package_native_assets_shader_read(void *context, const char *path,
    unsigned char **body, size_t *length);
/* Immutable installed inputs for synchronous native source preparation. This
 * bypasses cooked products and every authored provider. The caller retains the
 * assets for the entire read/cook. Uses the baseline-reader ownership contract. */
int sh_package_native_assets_shader_original_read(void *context, const char *path,
    unsigned char **body, size_t *length);
/* Configure once before sharing this handle or requesting its adapter. The
 * registry source uses the ADDRESS OF the all-manager head pointer. Callbacks
 * and original_context remain valid until close; original must bypass all
 * authored/product overrides. No engine code is called. Actual reads occur at
 * the caller's stable native resource boundary during compilation. Repeating
 * the identical configuration succeeds; changing it requires a fresh handle.
 * The shader adapter verifies exact original/current source bytes, native
 * readiness, kind-specific parameter index ranges and fixed flag masks. Source
 * proof requires exact retained native text or a successful observed parse
 * receipt for this unchanged object. Already retired objects without a receipt
 * must first pass an admitted native read/parse at the engine boundary. It
 * does not create absent slots, admit unknown source-free names, or certify
 * driver acceptance. Publication must preserve these fixed objects. */
int sh_package_native_assets_configure_shader(sh_package_native_assets *assets,
    sh_package_shader_renderer renderer, sh_native_resource_registry_source registry,
    sh_package_baseline_reader original, void *original_context,
    char *error, size_t capacity);
/* Production convenience: opens/owns an independent installed catalog until
 * assets close. Neither doom_base nor a per-provider catalog is borrowed. Call
 * only on a fresh, unshared handle, before adapter(). */
int sh_package_native_assets_configure_shader_catalog(sh_package_native_assets *assets,
    sh_package_shader_renderer renderer, sh_native_resource_registry_source registry,
    const char *doom_base, char *error, size_t capacity);
/* The same with a catalog the caller already opened: the assets take their own reference to it (released when they
 * close), so the installation's index is held once instead of twice. */
int sh_package_native_assets_configure_shader_catalog_shared(sh_package_native_assets *assets,
    sh_package_shader_renderer renderer, sh_native_resource_registry_source registry,
    sh_resource_catalog *catalog, char *error, size_t capacity);
/* Native library callbacks. Verified engine-created intrinsic images retain
 * their native identity after proven source absence. Fixed ABI sources require
 * exact original bytes and dependencies before certification. Arbitrary missing
 * files, changed shader ABI and unknown private page providers are not admitted. */
sh_package_library_native sh_package_native_assets_adapter(sh_package_native_assets *assets);
/* Private page families refused so far in this process, over every asset
 * handle; a refused family's members serve nothing and its materials keep the
 * engine's provider. reason receives the latest refusal line. A library compile
 * that fails while this count grows is compiled again. */
uint64_t sh_package_native_assets_page_refusals(char *reason, size_t capacity);
/* 1 with the refusal line when this process refused the member at engine_path
 * that the named package supplies, else 0. */
int sh_package_native_assets_page_refusal(const char *package, const char *engine_path, char *reason, size_t capacity);
#endif
