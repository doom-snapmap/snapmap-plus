/* Portable import transaction ordering, consent and retained recovery. */
#include "../src/backend/prefab_import.c"
#include <assert.h>

static int editor_object, map_object, other_map, source_object, plan_object, owner_object, stage_object;
static void *current_map;
static unsigned long generation;
static int frame, map_pending, with_plan, missing_resources, bind_ok, capture_ok, inventory_ok, expected_place;
static int prompt_ok, sync_answer, ready, cancel_ok, commit_ok, prepare_status, finalize_status, abort_status;
static int prompts, cancels, commits, inventories, captures, preparations, finalizations, aborts, reports;
static int source_releases, plan_releases, owner_releases, logs, maintenance;
static sh_mpkg_install_completion answer;
static void *answer_context;
static sh_decl_server_occurrence_hooks retained_hooks;
static char events[128];

static void event(char value) { size_t n = strlen(events); assert(n + 1 < sizeof(events)); events[n] = value; events[n + 1] = 0; }
void backend_log(const char *message) { assert(message && *message); logs++; }
void *sh_editor_transition_capture(void *editor) { return editor == &editor_object ? current_map : NULL; }
int sh_editor_transition_frame_ready(void *editor, void *map) { return frame && editor == &editor_object && map == current_map; }
unsigned long sh_rawmap_load_generation(void) { return generation; }
int sh_rawmap_defers_install_commit(void) { return map_pending || sh_prefab_import_pending(); }
int sh_overrides_get_root(char *out, size_t capacity) { return strcpy_s(out, capacity, "test-root") == 0; }
sh_mpkg_context *sh_mpkg_context_open(const char *root, const char *json, size_t length, char *error, size_t capacity)
{ (void)error; (void)capacity; assert(!strcmp(root, "test-root") && length == strlen(json)); return (void *)&source_object; }
void sh_mpkg_context_retire(sh_mpkg_context **context)
{ if (!*context) return; assert(*context == (void *)&source_object); source_releases++; *context = NULL; }
/* sh_user_overrides off for this launch; carried package content in the prefab. */
static int serving_off, scan_packages, carrier_bindings, carrier_deliveries;
int sh_package_runtime_serving_disabled(void) { return serving_off; }
size_t sh_mpkg_scan(const char *json, size_t length, sh_mpkg_decl *out, size_t capacity)
{ (void)json; (void)length; (void)out; (void)capacity; return (size_t)scan_packages; }
int sh_package_document_bind(const char *root, const sh_mpkg_context *sources, char **json, size_t *length,
    sh_package_map_plan **plan, char *error, size_t capacity)
{
    (void)root; (void)error; (void)capacity;
    assert(sources == (void *)&source_object && json && *length == strlen(*json));
    if (!bind_ok) return 0;
    *plan = with_plan ? (void *)&plan_object : NULL; event('B'); return 1;
}
void sh_package_map_plan_free(sh_package_map_plan *plan) { if (plan) { assert(plan == (void *)&plan_object); plan_releases++; } }
int sh_prefab_carrier_read(const char *json, size_t length, sh_prefab_carrier *out, char *error, size_t capacity)
{
    (void)error; (void)capacity;
    out->bindings_present = carrier_bindings != 0; out->bindings.binding_count = carrier_bindings > 0;
    out->deliveries_present = carrier_deliveries;
    out->native_json = _strdup(json); out->native_length = length; return out->native_json != NULL;
}
void sh_prefab_carrier_free(sh_prefab_carrier *value) { free(value->native_json); memset(value, 0, sizeof(*value)); }
char *sh_package_runtime_capture_document(const sh_package_map_plan *plan, const sh_package_provenance *saved,
    const sh_package_bundle_binding *additional, size_t count, const char *json, size_t length,
    size_t *output_length, sh_package_occurrence_snapshot **ownership, struct sh_mpkg_used **deliveries,
    size_t *delivery_count, char *error, size_t capacity)
{
    (void)additional; (void)count; (void)deliveries; (void)delivery_count; (void)error; (void)capacity;
    assert(plan == (with_plan ? (void *)&plan_object : NULL) && (!serving_off || !saved)); captures++;
    if (!capture_ok) return NULL;
    *ownership = (void *)&owner_object; *output_length = length; event('C'); return _strdup(json);
}
void sh_package_occurrence_release(sh_package_occurrence_snapshot *ownership)
{ if (ownership) { assert(ownership == (void *)&owner_object); owner_releases++; } }
/* Exact-row carries record the captures before them and the carrier bindings
 * they received; refusal is the fail-closed path. */
static int carries, carry_captures, carry_bindings_seen, carry_failed;
int sh_package_occurrence_carry_rows(sh_package_occurrence_snapshot *ownership, const sh_package_provenance *from)
{
    assert(ownership == (void *)&owner_object && from);
    carries++; carry_captures = captures; carry_bindings_seen = (int)from->binding_count; event('R');
    return !carry_failed;
}
void *sh_typeinfo_get_reflect(void) { return &owner_object; }
int sh_decl_server_registry_source(sh_decl_registry_source *source) { memset(source, 0, sizeof(*source)); return 1; }
int sh_package_map_plan_usage_missing(const sh_package_map_plan *plan, sh_decl_registry_source registry,
    uintptr_t reflection, const char *json, size_t length, sh_package_missing *out,
    sh_package_source_graph_report *report, char *error, size_t capacity)
{
    (void)registry; (void)report; (void)error; (void)capacity;
    assert(plan == (void *)&plan_object && reflection && json && length);
    out->count = missing_resources; event('U'); return 1;
}
void sh_package_missing_free(sh_package_missing *missing) { memset(missing, 0, sizeof(*missing)); }
const sh_package_compilation *sh_package_map_plan_compilation(const sh_package_map_plan *plan)
{ assert(plan == (void *)&plan_object); return (void *)&plan_object; }
int sh_mpkg_request_map_install(const char *json, size_t length, const sh_package_compilation *candidate,
    const sh_package_owners *owners, sh_mpkg_install_completion callback, void *context, char *error, size_t capacity)
{
    (void)owners; (void)error; (void)capacity;
    assert(candidate == (void *)&plan_object && json && length && !answer);
    prompts++; event('Q'); if (!prompt_ok) return 0;
    if (sync_answer != 99) callback(context, sync_answer);
    else { answer = callback; answer_context = context; }
    return 1;
}
void sh_mpkg_cancel_map_consent(void *context)
{
    assert(answer && answer_context == context); sh_mpkg_install_completion callback = answer;
    answer = NULL; answer_context = NULL; event('W'); callback(context, 0);
}
int sh_mpkg_activation_ready(void) { return ready; }
int sh_mpkg_activation_cancel(void) { cancels++; event('X'); return cancel_ok; }
int sh_mpkg_activation_commit(char *error, size_t capacity)
{ (void)error; (void)capacity; commits++; event('K'); return commit_ok; }
void sh_mpkg_report_error(sh_mpkg_problem problem, const char *message) { (void)problem; assert(message && *message); reports++; }
int sh_package_map_plan_prepare_inventory(sh_package_map_plan *plan, const char *root, char *error, size_t capacity)
{ (void)error; (void)capacity; assert(plan == (void *)&plan_object && !strcmp(root, "test-root")); inventories++; event('I'); return inventory_ok; }
static const char *expected_native;
int sh_apply_prefab_stage_prepare(void *editor, const char *json, const sh_package_occurrence_snapshot *ownership,
    int place, sh_apply_prefab_stage **out, char *error, size_t capacity)
{
    (void)error; (void)capacity; assert(editor == &editor_object && json && ownership == (void *)&owner_object && place == expected_place);
    assert(!expected_native || !strcmp(json, expected_native));
    preparations++; event('P'); if (prepare_status) *out = (void *)&stage_object; return prepare_status;
}
int sh_apply_prefab_stage_finalize(sh_apply_prefab_stage **stage, int (*commit)(char *, size_t), char *error, size_t capacity)
{
    assert(*stage == (void *)&stage_object); finalizations++; event('F');
    if (finalize_status != 1 || (commit && !commit(error, capacity))) return finalize_status == 1 ? 0 : finalize_status;
    *stage = NULL; return 1;
}
int sh_apply_prefab_stage_abort(sh_apply_prefab_stage **stage, char *error, size_t capacity)
{ (void)error; (void)capacity; aborts++; event('A'); if (abort_status == 1) *stage = NULL; return abort_status; }
int sh_decl_server_occurrence_recovery_pending(void *context) { return retained_hooks.context == context; }
void sh_decl_server_occurrence_maintenance(void)
{
    maintenance++;
    if (retained_hooks.context && abort_status == 1) {
        char error[80]; assert(retained_hooks.rollback(retained_hooks.context, error, sizeof(error)) == 1);
        memset(&retained_hooks, 0, sizeof(retained_hooks));
    }
}
int sh_decl_server_admit_occurrences(sh_package_map_plan *plan, const sh_package_occurrence_snapshot *document,
    const sh_decl_server_occurrence_hooks *hooks, char *error, size_t capacity)
{
    assert(plan == (with_plan ? (void *)&plan_object : NULL) && document == (void *)&owner_object && frame);
    event('D');
    if (hooks->prepare(hooks->context, error, capacity) == 1 && hooks->finalize(hooks->context, error, capacity) == 1)
        return SH_PACKAGE_ADMISSION_READY;
    if (hooks->rollback(hooks->context, error, capacity) != 1) retained_hooks = *hooks;
    return SH_PACKAGE_ADMISSION_FAILED;
}

static void reset(void)
{
    assert(!g_import && !answer && !retained_hooks.context);
    current_map = &map_object; generation = 1; frame = 1; map_pending = 0;
    expected_place = 1;
    with_plan = bind_ok = capture_ok = inventory_ok = prompt_ok = ready = cancel_ok = commit_ok = 1;
    prepare_status = finalize_status = abort_status = 1; missing_resources = 0; sync_answer = 99;
    prompts = cancels = commits = inventories = captures = preparations = finalizations = aborts = reports = 0;
    source_releases = plan_releases = owner_releases = logs = maintenance = 0; events[0] = 0;
    serving_off = scan_packages = carrier_bindings = carrier_deliveries = 0; expected_native = NULL;
    carries = carry_captures = carry_bindings_seen = carry_failed = 0;
}
static void request(void) { assert(sh_prefab_import_request(&editor_object, "{}", 2, 1)); assert(sh_prefab_import_pending()); }
static void poll(void) { sh_prefab_import_poll(&editor_object); }
static void respond(int result)
{
    assert(answer); sh_mpkg_install_completion callback = answer; void *context = answer_context;
    answer = NULL; answer_context = NULL; callback(context, result);
}
int main(void)
{
    reset(); request(); assert(!captures && !prompts && !preparations);
    assert(!sh_prefab_import_request(&editor_object, "{}", 2, 1));
    frame = 0; poll(); assert(!captures); frame = 1; poll();
    assert(!g_import && !prompts && !commits && !cancels && !inventories && logs == 1);
    assert(!strcmp(events, "BCUDPF") && owner_releases == 1 && plan_releases == 1 && source_releases == 1);

    reset(); with_plan = 0; request(); poll();
    assert(!strcmp(events, "BCDPF") && !prompts && !inventories && !plan_releases && logs == 1 && !carries);

    /* A carrier's exact editor rows reach the captured ownership before usage
     * is checked or anything is staged; a refused carry stages nothing. */
    reset(); carrier_bindings = 1; request(); poll();
    assert(!g_import && !strcmp(events, "BCRUDPF") && logs == 1 && !reports);
    assert(carries == 1 && carry_captures == 1 && carry_bindings_seen == 1 && owner_releases == 1);
    reset(); carrier_bindings = 1; carry_failed = 1; request(); poll();
    assert(!g_import && !strcmp(events, "BCR") && reports == 1 && carries == 1 && !preparations);
    assert(!prompts && !commits && owner_releases == 1);

    reset(); expected_place = 0;
    assert(sh_prefab_import_request(&editor_object, "{}", 2, 0)); poll();
    assert(!g_import && logs == 1 && !prompts && !cancels);

    reset(); missing_resources = 1; request(); poll();
    assert(g_import && answer && prompts == 1 && !preparations && !commits);
    respond(1); poll(); assert(!g_import && !cancels && commits == 1 && inventories == 1);
    assert(!strcmp(events, "BCUQIDPFK"));

    reset(); missing_resources = 1; sync_answer = 1; request(); poll();
    assert(!g_import && commits == 1 && !cancels && logs == 1);

    reset(); missing_resources = 1; request(); poll(); respond(0); poll();
    assert(!g_import && !commits && !cancels && !preparations && !reports);
    request(); poll(); assert(prompts == 2); respond(1); poll(); assert(commits == 1 && logs == 1);

    reset(); missing_resources = 1; request(); poll(); generation++; sh_prefab_import_maintenance();
    assert(!g_import && !answer && !preparations && !cancels && strchr(events, 'W'));

    reset(); missing_resources = 1; request(); poll(); respond(1); current_map = &other_map; poll();
    assert(!g_import && cancels == 1 && !preparations && !commits);

    reset(); missing_resources = 1; request(); poll(); respond(1); inventory_ok = 0; poll();
    assert(!g_import && inventories == 1 && cancels == 1 && !preparations);

    reset(); missing_resources = 1; sync_answer = 1; commit_ok = 0; request(); poll();
    assert(!g_import && commits == 1 && cancels == 1 && aborts == 1 && !logs);
    assert(!strcmp(events, "BCUQIDPFKAX"));

    reset(); missing_resources = 1; sync_answer = 1; prepare_status = 0; cancel_ok = 0; request(); poll();
    assert(g_import && cancels == 1 && !commits && !source_releases);
    cancel_ok = 1; poll(); assert(!g_import && cancels == 2 && source_releases == 1);

    reset(); missing_resources = 1; sync_answer = 1; prepare_status = abort_status = -1; request(); poll();
    assert(g_import && g_import->stage && retained_hooks.context == g_import && !cancels && !source_releases);
    poll(); assert(g_import && !source_releases && !cancels);
    abort_status = 1; poll(); assert(!g_import && !retained_hooks.context && cancels == 1 && source_releases == 1);

    reset(); finalize_status = -1; request(); poll();
    assert(!g_import && aborts == 1 && !logs && !commits && !cancels);

    reset(); bind_ok = 0; request(); poll(); assert(!g_import && reports == 1 && !captures && !preparations);
    reset(); capture_ok = 0; request(); poll(); assert(!g_import && reports == 1 && !owner_releases && !preparations);
    reset(); missing_resources = 1; prompt_ok = 0; request(); poll(); assert(!g_import && !answer && reports == 1);
    reset(); map_pending = 1; assert(!sh_prefab_import_request(&editor_object, "{}", 2, 1));
    reset(); assert(!sh_prefab_import_request(&editor_object, "\0", 1, 1));
    assert(!sh_prefab_import_request(&other_map, "{}", 2, 1));
    poll(); assert(maintenance == 1);
    /* User packages off for this launch: a prefab without package content (an
     * empty ownership envelope included) is placed natively, captured whole,
     * staged and finalized, with no binding, admission, readiness or install.
     * Carried bindings, deliveries or package shards are refused with the reason
     * and nothing is captured or staged. A failed native stage is rolled back
     * before the request ends, again on later polls until the rollback finishes. */
    for (int envelope = 0; envelope <= 1; envelope++) {
        reset(); serving_off = 1; with_plan = 0; ready = 0; carrier_bindings = envelope ? -1 : 0;
        request(); poll();
        assert(!g_import && !strcmp(events, "CPF") && logs == 1 && !reports && !source_releases);
        assert(captures == 1 && owner_releases == 1 && !prompts && !inventories && !commits && !cancels);
    }
    {
        /* The native document is staged byte for byte, strings included. */
        static const char text[] = "{\"name\":\"caf\xc3\xa9 \\\"q\\\" \\\\ end\"}";
        reset(); serving_off = 1; with_plan = 0; expected_native = text;
        assert(sh_prefab_import_request(&editor_object, text, sizeof(text) - 1, 1)); poll();
        assert(!g_import && !strcmp(events, "CPF") && preparations == 1 && !reports);
    }
    for (int carried = 0; carried < 3; carried++) {
        reset(); serving_off = 1; with_plan = 0;
        if (carried == 0) carrier_bindings = 1; else if (carried == 1) carrier_deliveries = 1; else scan_packages = 1;
        request(); poll();
        assert(!g_import && !events[0] && reports == 1 && !captures && !preparations && !prompts);
    }
    /* A deferred rollback (0) and a quarantined one (-1) keep the stage and
     * sources, report once, retry quietly, refuse new imports with the reason,
     * and clean up normally once the rollback completes. */
    for (int quarantine = 0; quarantine <= 1; quarantine++) {
        reset(); serving_off = 1; with_plan = 0;
        finalize_status = quarantine ? -1 : 0; abort_status = quarantine ? -1 : 0;
        request(); poll();
        assert(g_import && g_import->stage && aborts == 1 && !logs && reports == 1 && !owner_releases);
        for (int i = 0; i < 5; i++) poll();
        assert(g_import && g_import->stage && aborts == 6 && reports == 1 && !owner_releases);
        assert(!sh_prefab_import_request(&editor_object, "{}", 2, 1) && reports == 2);
        abort_status = 1; poll();
        assert(!g_import && aborts == 7 && reports == 2 && owner_releases == 1 && !commits);
        request(); assert(g_import); poll(); assert(!g_import);
    }
    /* Serving expected but not ready still refuses through the ordinary route. */
    reset(); ready = 0; request(); poll();
    assert(!g_import && reports == 1 && !preparations && !strcmp(events, "BCU"));
    puts("prefab import transaction tests passed"); return 0;
}
