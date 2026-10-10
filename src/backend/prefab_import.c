#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prefab_import.h"
#include "prefab.h"
#include "apply_engine.h"
#include "backend_log.h"
#include "decl_server.h"
#include "editor_transition.h"
#include "map_package.h"
#include "overrides.h"
#include "package_document.h"
#include "package_runtime.h"
#include "rawmap.h"
#include "typeinfo.h"

enum { PI_PREPARE = 1, PI_CONSENT, PI_READY, PI_NATIVE, PI_CANCEL };
typedef struct pi_request {
    void *editor, *map;
    DWORD thread;
    unsigned long generation;
    int state, place, consent, installed, busy, native_place, reported;
    char root[MAX_PATH], error[2048];
    char *delivery, *native;
    size_t delivery_length, native_length;
    sh_mpkg_context *sources;
    sh_package_map_plan *plan;
    sh_package_occurrence_snapshot *ownership;
    sh_apply_prefab_stage *stage;
} pi_request;
static pi_request *g_import;

int sh_prefab_import_pending(void) { return g_import != NULL; }

static void pi_install_completed(void *context, int outcome)
{
    pi_request *request = context;
    if (g_import != request || request->state != PI_CONSENT) return;
    request->consent = 0;
    request->installed = outcome == 1;
    request->state = outcome == 1 ? PI_READY : PI_CANCEL;
    if (outcome < 0 && !request->error[0])
        strcpy_s(request->error, sizeof(request->error), "The prefab installation did not complete.");
}

static int pi_native_rollback(void *context, char *error, size_t capacity);
static int pi_discard(pi_request *request)
{
    if (!request || request->busy) return 0;
    if (request->native_place && request->stage) {
        /* A native placement owns its stage. A rollback that is deferred or
         * quarantined keeps the stage and its sources, is reported once and is
         * retried quietly on later polls; a later success cleans up normally
         * without reporting again. */
        char detail[256] = "", message[sizeof(request->error) + sizeof(detail) + 64];
        if (pi_native_rollback(request, detail, sizeof(detail)) != 1) {
            if (!request->reported) {
                snprintf(message, sizeof(message), "%s Its native data is retained: %s.",
                    request->error[0] ? request->error : "The prefab could not be placed.",
                    detail[0] ? detail : "cleanup is pending");
                sh_mpkg_report_error(SH_MPKG_PROBLEM_PREFAB, message); request->reported = 1;
            }
            return 0;
        }
    }
    if (request->stage || sh_decl_server_occurrence_recovery_pending(request)) return 0;
    request->state = PI_CANCEL;
    if (request->consent) {
        sh_mpkg_cancel_map_consent(request);
        request->consent = 0;
    }
    /* Only this request's published disk set can be canceled here. A covered
     * import never owns the install service's unrelated global transaction. */
    if (request->installed && !sh_mpkg_activation_cancel()) return 0;
    request->installed = 0;
    if (request->error[0] && !request->reported) sh_mpkg_report_error(SH_MPKG_PROBLEM_PREFAB, request->error);
    sh_package_occurrence_release(request->ownership);
    sh_package_map_plan_free(request->plan);
    sh_mpkg_context_retire(&request->sources);
    if (request->delivery) HeapFree(GetProcessHeap(), 0, request->delivery);
    free(request->native);
    g_import = NULL; free(request); return 1;
}

int sh_prefab_import_request(void *editor, const char *json, size_t length, int place)
{
    void *map;
    pi_request *request;
    if (g_import && g_import->reported) {
        /* A retained native placement still holds its stage and sources. */
        sh_mpkg_report_error(SH_MPKG_PROBLEM_PREFAB, "An earlier prefab placement still holds its native data; prefabs cannot be imported until it is released.");
        return 0;
    }
    if (!editor || !json || !length || length > INT_MAX || memchr(json, 0, length) ||
        g_import || sh_rawmap_defers_install_commit() ||
        !(map = sh_editor_transition_capture(editor))) return 0;
    request = calloc(1, sizeof(*request));
    if (!request) return 0;
    request->delivery = HeapAlloc(GetProcessHeap(), 0, length + 1);
    if (!request->delivery) { free(request); return 0; }
    memcpy(request->delivery, json, length); request->delivery[length] = 0;
    request->delivery_length = length;
    request->editor = editor; request->map = map;
    request->thread = GetCurrentThreadId(); request->generation = sh_rawmap_load_generation();
    request->place = !!place; request->state = PI_PREPARE;
    g_import = request; return 1;
}

/* With user packages off for this launch (sh_user_overrides 0) nothing is
 * served: a prefab that owns no package content is placed natively, without
 * binding, admission or installation, and one that carries packages is refused
 * with its file untouched. */
static int pi_prepare_native(pi_request *request)
{
    sh_prefab_carrier carrier = {0};
    int ok = 0;
    if (!sh_prefab_carrier_read(request->delivery, request->delivery_length,
        &carrier, request->error, sizeof(request->error))) goto done;
    if (carrier.deliveries_present || (carrier.bindings_present && (carrier.bindings.use_count ||
        carrier.bindings.binding_count || carrier.bindings.context_count)) ||
        sh_mpkg_scan(carrier.native_json, carrier.native_length, NULL, 0)) {
        strcpy_s(request->error, sizeof(request->error),
            "this prefab uses packages, and package serving is disabled for this launch (sh_user_overrides 0)");
        goto done;
    }
    request->native = sh_package_runtime_capture_document(NULL, NULL, NULL, 0,
        carrier.native_json, carrier.native_length, &request->native_length,
        &request->ownership, NULL, NULL, request->error, sizeof(request->error));
    if (!request->native || !request->ownership) goto done;
    request->native_place = 1; request->state = PI_NATIVE; ok = 1;
done:
    sh_prefab_carrier_free(&carrier);
    if (!ok && !request->error[0]) strcpy_s(request->error, sizeof(request->error),
        "The prefab could not be prepared for native placement.");
    return ok;
}

static int pi_prepare(pi_request *request)
{
    sh_prefab_carrier carrier = {0};
    sh_package_missing missing = {0};
    sh_decl_registry_source registry = {0};
    sh_package_source_graph_report report = {0};
    int ok = 0;
    if (sh_package_runtime_serving_disabled()) return pi_prepare_native(request);
    if (!sh_overrides_get_root(request->root, sizeof(request->root))) goto done;
    request->sources = sh_mpkg_context_open(request->root, request->delivery,
        request->delivery_length, request->error, sizeof(request->error));
    if (!request->sources || !sh_package_document_bind(request->root, request->sources,
        &request->delivery, &request->delivery_length, &request->plan,
        request->error, sizeof(request->error))) goto done;
    if (!sh_prefab_carrier_read(request->delivery, request->delivery_length,
        &carrier, request->error, sizeof(request->error))) goto done;
    request->native = sh_package_runtime_capture_document(request->plan,
        carrier.bindings_present ? &carrier.bindings : NULL, NULL, 0,
        carrier.native_json, carrier.native_length, &request->native_length,
        &request->ownership, NULL, NULL, request->error, sizeof(request->error));
    if (!request->native || !request->ownership) goto done;
    if (request->plan) {
        uintptr_t reflection = (uintptr_t)sh_typeinfo_get_reflect();
        if (!reflection || !sh_decl_server_registry_source(&registry) ||
            !sh_package_map_plan_usage_missing(request->plan, registry, reflection,
                request->native, request->native_length, &missing, &report,
                request->error, sizeof(request->error))) goto done;
    }
    if (missing.count) {
        request->state = PI_CONSENT; request->consent = 1;
        if (!sh_mpkg_request_map_install(request->delivery, request->delivery_length,
            sh_package_map_plan_compilation(request->plan), &missing.packages,
            pi_install_completed, request, request->error, sizeof(request->error))) {
            request->consent = 0; goto done;
        }
    } else {
        if (!sh_mpkg_activation_ready()) goto done;
        request->state = PI_READY;
    }
    ok = 1;
done:
    sh_prefab_carrier_free(&carrier); sh_package_missing_free(&missing);
    if (!ok && !request->error[0]) strcpy_s(request->error, sizeof(request->error),
        "The prefab's authored sources could not be prepared.");
    return ok;
}

static int pi_native_prepare(void *context, char *error, size_t capacity)
{
    pi_request *request = context;
    return sh_apply_prefab_stage_prepare(request->editor, request->native,
        request->ownership, request->place, &request->stage, error, capacity) == 1;
}
static int pi_native_finalize(void *context, char *error, size_t capacity)
{
    pi_request *request = context;
    if (sh_apply_prefab_stage_finalize(&request->stage,
        request->installed ? sh_mpkg_activation_commit : NULL, error, capacity) != 1) return 0;
    request->installed = 0; return 1;
}
static int pi_native_rollback(void *context, char *error, size_t capacity)
{
    pi_request *request = context;
    return sh_apply_prefab_stage_abort(&request->stage, error, capacity) == 1;
}

void sh_prefab_import_maintenance(void)
{
    pi_request *request = g_import;
    if (!request || request->busy || request->thread != GetCurrentThreadId()) return;
    if (request->generation != sh_rawmap_load_generation() ||
        sh_editor_transition_capture(request->editor) != request->map) request->state = PI_CANCEL;
    if (request->state == PI_CANCEL) pi_discard(request);
}

void sh_prefab_import_poll(void *editor)
{
    sh_decl_server_occurrence_maintenance();
    pi_request *request = g_import;
    if (!request || request->busy || request->thread != GetCurrentThreadId()) return;
    if (request->state == PI_CANCEL) { pi_discard(request); return; }
    if (request->editor != editor || request->generation != sh_rawmap_load_generation() ||
        sh_editor_transition_capture(editor) != request->map) {
        request->state = PI_CANCEL; pi_discard(request); return;
    }
    if (!sh_editor_transition_frame_ready(editor, request->map)) return;
    request->busy = 1;
    __try {
        if (request->state == PI_PREPARE && !pi_prepare(request)) request->state = PI_CANCEL;
        if (request->state == PI_NATIVE) {
            if (pi_native_prepare(request, request->error, sizeof(request->error)) == 1 &&
                pi_native_finalize(request, request->error, sizeof(request->error)) == 1)
                backend_log("MPKG: prefab placed natively; user packages are off for this launch");
            else if (!request->error[0])
                strcpy_s(request->error, sizeof(request->error), "The prefab could not be placed.");
            request->state = PI_CANCEL;
        }
        if (request->state == PI_READY) {
            sh_decl_server_occurrence_hooks hooks = {
                request, pi_native_prepare, pi_native_finalize, pi_native_rollback
            };
            if (request->installed && (!request->plan ||
                !sh_package_map_plan_prepare_inventory(request->plan, request->root,
                    request->error, sizeof(request->error)))) request->state = PI_CANCEL;
            else {
                int admitted = sh_decl_server_admit_occurrences(request->plan,
                    request->ownership, &hooks, request->error, sizeof(request->error));
                if (admitted == SH_PACKAGE_ADMISSION_READY)
                    backend_log("MPKG: portable prefab admitted into the existing editor map");
                request->state = PI_CANCEL;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(request->error, sizeof(request->error),
            "Prefab preparation raised exception %08lx.", GetExceptionCode());
        request->state = PI_CANCEL;
    }
    request->busy = 0;
    if (request->state == PI_CANCEL) pi_discard(request);
}
