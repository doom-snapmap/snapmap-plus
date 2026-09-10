/* Asynchronous cooked-geometry previews for the Prefab Details viewport.
 * Resolve installed resources and decode positions, normals and indices into
 * a bounded neutral mesh for WebView2. Payloads are read on demand; this path
 * does not decode textures, skeletons or animation.
 */
#ifndef BACKEND_PREFABPREVIEW_H
#define BACKEND_PREFABPREVIEW_H

#include <stddef.h>
#include <stdint.h>

#define SH_PREFAB_MESH_MAGIC   0x314D5053u /* little-endian bytes "SPM1" */
#define SH_PREFAB_MESH_VERSION 1u
#define SH_PREFAB_MESH_OK      1u

#pragma pack(push, 1)
typedef struct sh_prefab_mesh_blob_header {
    uint32_t magic;
    uint16_t version;
    uint16_t status;
    uint32_t generation;
    uint32_t name_bytes;
    uint32_t vertex_count;
    uint32_t index_count;
    float bounds[6];
    uint32_t vertex_stride;
    uint32_t reserved;
} sh_prefab_mesh_blob_header;
#pragma pack(pop)

/* Vertex payload immediately after the padded UTF-8 name: XYZ float32 + packed normal XYZW bytes. */
#define SH_PREFAB_MESH_VERTEX_STRIDE 16u

#ifndef SH_PREFAB_DEFAULT_MODEL
#define SH_PREFAB_DEFAULT_MODEL 0x1
#define SH_PREFAB_DEFAULT_SCALE 0x2
#endif

/* Starts one bounded worker. Resource indexes and payloads remain lazy until the first request. */
int sh_prefabpreview_install(void);

/* Queue one model for a prefab generation. A new generation atomically discards stale queued/completed
 * work. An empty model name performs only that reset/cancellation. Duplicate queued requests are ignored. */
int sh_prefabpreview_request(unsigned long generation, const char *model_name);

/* File-only fallback for inherited preview geometry. It follows installed snapEditorEntityDef /
 * entityDef inheritance and spawnerEntityPair.entityStatic links, which expose the pickup model that
 * a SnapMap spawner represents even though the spawner decl itself has no render model. */
int sh_prefabpreview_resolve_model(const char *inherit_name, char *out_model, size_t out_capacity);

/* Resolve the model plus the inherited renderModelInfo.scale defaults that the saved prefab stores
 * only as a sparse override. Returns SH_PREFAB_DEFAULT_* bits; out_scale always starts at {1,1,1}.
 * Derived decl components win over base components, including semantic pickup entityStatic links. */
int sh_prefabpreview_resolve_defaults(const char *inherit_name, char *out_model,
                                      size_t out_capacity, float out_scale[3]);

/* Consume the oldest completion. Returns 0 when none exists, -required_bytes for a size query or short
 * destination, and positive bytes copied when the completion was consumed. */
int sh_prefabpreview_get(void *out_blob, int out_capacity);

#endif /* BACKEND_PREFABPREVIEW_H */
