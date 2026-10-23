#ifndef SH_RESOURCE_LATE_MODEL_READS_H
#define SH_RESOURCE_LATE_MODEL_READS_H
#include <stdint.h>
#include "resource_instances.h"

typedef struct sh_resource_late_model_scope {uint64_t context,serial;} sh_resource_late_model_scope;
enum {SH_LATE_MODEL_WATER_APPEND=0,SH_LATE_MODEL_CONSTRUCT=1,SH_LATE_MODEL_HOT_REBUILD=2};
/* Root's existing DERIVED owner remains the graph/lifetime authority. These
 * callbacks never execute native model methods. visible checks the exact scope
 * above SOURCE/presentation and independent nested consumers. input borrows an
 * already held exact origin and its observed source token. It must acquire its
 * own metadata reference/serving hold before returning, and must never consume
 * or release the borrowed reference. The adapter retains its own hold through
 * end. end is called once even for a partial begin; returned distinguishes a
 * normal native return (including false) from an exception. complete additionally
 * requires native success and every local read/identity check. Root preserves
 * partial/faulted inputs and never certifies an entire
 * model or a GPU lifetime solely from this adapter's success. ready checks the
 * current DeclFind/lookup/Materialize multiplex, common destructor and shared Hotbox
 * owner; it must not call this module's ready function recursively. */
typedef struct sh_resource_late_model_api {
    sh_resource_late_model_scope (*begin)(void *model,int mode);
    int (*visible)(sh_resource_late_model_scope scope);
    int (*input)(sh_resource_late_model_scope scope,void *resource,uint64_t token,
                 sh_resource_instance_origin *origin);
    void (*end)(sh_resource_late_model_scope scope,void *model,int returned,int complete);
    int (*ready)(void);
} sh_resource_late_model_api;
int sh_resource_late_model_reads_install(const unsigned char *base,sh_resource_late_model_api api);
int sh_resource_late_model_reads_ready(void);
/* Existing lookup/Materialize owner notifies only after normal native return,
 * restoring its parent SOURCE first. caller is the actual native return PC. */
void sh_resource_late_model_returned(void *resource,uintptr_t caller);
/* Existing HotBoxBuildGeometry owner calls these around its unchanged native
 * call, including exceptional exit. No competing detour is installed. */
sh_resource_late_model_scope sh_resource_late_model_hotspot_begin(void *model);
void sh_resource_late_model_hotspot_end(sh_resource_late_model_scope scope,int returned);
/* Actual editor-image value derived/read by the existing material-size getter.
 * Called inside its active getter bracket, with NULL only for unknown reads. */
void sh_resource_late_model_size_input(void *image);
/* Root's positive common model constructor/dtor own actual lifetime changes.
 * This only retires adapter metadata; failed teardown retains it. */
void sh_resource_late_model_destroyed(void *model,uint64_t token,int completed);
#endif
