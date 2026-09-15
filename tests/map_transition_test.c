/* Actual handoff implementation with native call-order and failure fixtures. */
#include <assert.h>
#include <stdio.h>
#include "../src/backend/map_transition.c"

static unsigned char image[4096], manager[0x2c78], parameters[0x17100], common[0xf580];
static void *common_vtable[6];
static DWORD thread_id;
static int has_resource, force_full, pending_result, activation_result, mode;
static void *pending_resource;
static int checks_pending, activations, finishes, finished_result, allocations, unloads, purges, compares;
static int observed_force, observed_full, step;
static int cancellations, native_finalized, native_frame, fatals, install_commits;
static int prepares, commits, removes, fail_prepare, fail_commit, fail_remove;
static sh_map_transition_callbacks callbacks;

void backend_log(const char *message) { assert(message); }
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{ (void)base; (void)status; assert(!strcmp(name, "main_thread_id")); return (uintptr_t)&thread_id; }
static int native_compare(const char *a, const char *b)
{ compares++; return strcmp(a, b); }
static void native_purge(unsigned mask)
{
    assert(mask == 1); purges++;
    if (g_trans.phase == TRANS_UNLOADING) assert(step == 1);
}
static void native_unload(void *self, unsigned char force, unsigned char full)
{
    assert(self == common); unloads++; observed_force = force; observed_full = full;
    if (g_trans.phase == TRANS_UNLOADING) { assert(!step); step = 1; }
    if (mode == 6) RaiseException(0xe0475001, 0, 0, NULL);
    if (mode != 1) transition_purge_from(1, image + 0x300 + (mode == 7 ? 0x1aa : 0x1a9));
    if (g_trans.phase == TRANS_UNLOADING) step = 2;
}
static unsigned char native_allocation(void *self, void *out, const void *parms, void *extra)
{
    unsigned char full, force;
    assert(self == manager && parms == parameters && out == common + 0x1298 && extra == (void *)4);
    allocations++;
    pending_resource = NULL;
    full = has_resource && (native_compare(*(const char *const *)(parameters + 0x30),
        *(const char *const *)(manager + 0x2c70)) || force_full);
    force = (parameters[0x170f8] >> 5) & 1;
    if (mode != 15) transition_unload_from(common, force, full, image + 0x100 + 0xae);
    if (g_trans.phase == TRANS_LOADING && mode != 15) { assert(step == 3 && !g_trans.skip_unload && unloads == 1); step = 4; }
    if (mode == 5) RaiseException(0xe0475002, 0, 0, NULL);
    if (mode == 12) RaiseException(TRANS_CANCEL_EXCEPTION, 0, 0, NULL);
    return mode != 4;
}
static void native_finalize(void *self, const void *parms, void *extra)
{
    assert(self == common && parms == parameters && extra == (void *)4);
    native_frame++;
    __try {
        if (mode == 16) RaiseException(0xe0475016, 0, 0, NULL);
        common[0xf576] = 1;
        if (!transition_allocate_from(manager, common + 0x1298, parms, extra, image + 0x800 + 0x1ea)) {
            fatals++; RaiseException(0xe04750ff, 0, 0, NULL);
        }
        assert(!finishes); /* Successful allocation is not completed finalization. */
        if (mode == 13) RaiseException(0xe0475013, 0, 0, NULL);
        common[0xf576] = 0; native_finalized++;
    } __finally { native_frame--; }
}
static void native_cancel(void *self, unsigned char force)
{
    assert(self == common && !force && !native_frame && !finishes);
    assert(g_trans.phase == TRANS_FINISHING && !g_trans.skip_unload);
    cancellations++;
    if (mode == 14) RaiseException(0xe0475014, 0, 0, NULL);
}
static int pending(void *self, const void *parms)
{
    assert(self == manager && parms == parameters && g_trans.phase == TRANS_IDLE);
    checks_pending++; return pending_result;
}
static int activate(void)
{
    assert(sh_map_transition_at_boundary() && step == 2 && unloads == 1 && purges == 1);
    assert(!pending_resource); activations++; step = 3;
    if (mode == 3) RaiseException(0xe0475003, 0, 0, NULL);
    if (mode == 9) transition_allocate(manager, common + 0x1298, parameters, (void *)4);
    if (mode == 11) transition_finalize(common, parameters, (void *)4);
    return activation_result;
}
static void finished(int loaded)
{
    assert(!sh_map_transition_at_boundary() && g_trans.phase == TRANS_FINISHING && !native_frame);
    finishes++; finished_result = loaded;
    if (loaded) assert(native_finalized == 1 && !cancellations);
    if (mode == 8) RaiseException(0xe0475004, 0, 0, NULL);
}
static int commit_install(void)
{
    assert(!native_frame && native_finalized == 1 && g_trans.allocated && !finishes);
    install_commits++;
    if (mode == 18) RaiseException(0xe0475018, 0, 0, NULL);
    return mode != 17;
}
void *hook_prepare(void *target, void *detour, size_t stolen)
{
    static const size_t expected[] = {21, 20, 15, 17};
    void *originals[] = {native_allocation, native_unload, native_purge, native_finalize};
    int index = prepares++;
    assert(index < TRANS_HOOKS && target == g_trans_sites[index] && detour && stolen == expected[index]);
    return prepares == fail_prepare ? NULL : originals[index];
}
sh_patch_status hook_commit(void *trampoline)
{
    assert(trampoline);
    for (int i = 0; i < TRANS_HOOKS; i++) assert(g_trans_original[i]);
    commits++; return commits == fail_commit ? B2_PATCH_REFUSED_BADARG : B2_PATCH_OK;
}
int hook_unpatch(void *trampoline)
{ assert(trampoline); removes++; return !fail_remove; }
static void relative(unsigned char *where, const char *opcode, size_t length, const void *target)
{
    intptr_t delta = (const unsigned char *)target - (where + length + 4);
    int32_t displacement = (int32_t)delta;
    assert(delta == displacement); memcpy(where, opcode, length); memcpy(where + length, &displacement, 4);
}
static void make_binding(sig_result result[4])
{
    unsigned char *allocation = image + 0x100, *unload = image + 0x300, *purge = image + 0x600;
    unsigned char *finalize = image + 0x800, *cancel = image + 0xb00;
    memset(image, 0x90, sizeof(image));
    relative(allocation + 0x5b, "\x4c\x89\x3d", 3, &pending_resource);
    relative(allocation + 0x62, "\x44\x39\x3d", 3, &has_resource);
    relative(allocation + 0x84, "\x44\x39\x3d", 3, &force_full);
    relative(allocation + 0x7b, "\xe8", 1, native_compare);
    relative(allocation + 0xa2, "\x48\x8d\x0d", 3, common);
    relative(allocation + 0xa9, "\xe8", 1, unload);
    memcpy(allocation + 0x70, "\x48\x8b\x91\x70\x2c\x00\x00\x49\x8b\x48\x30", 11);
    memcpy(allocation + 0x95, "\x0f\xb6\x96\xf8\x70\x01\x00\xc0\xea\x05\x80\xe2\x01", 13);
    relative(unload + 0x1a4, "\xe8", 1, purge);
    relative(unload + 0x1b3, "\xe8", 1, purge);
    memcpy(purge, "\x40\x57\x48\x83\xec\x40\x48\xc7\x44\x24\x20\xfe\xff\xff\xff", 15);
    memcpy(finalize, "\x40\x53\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\xb8\x70\x74\x00\x00", 17);
    memcpy(finalize + 0x1b9, "\x48\x8b\x06\x33\xd2\x48\x8b\xce\xff\x50\x28", 11);
    memcpy(finalize + 0x1c9, "\xc6\x86\x76\xf5\x00\x00\x01", 7);
    memcpy(finalize + 0x1e0, "\x48\x8d\x96\x98\x12\x00\x00\xff\x50\x58\x84\xc0", 12);
    *(void ***)common = common_vtable; common_vtable[5] = cancel;
    memset(result, 0, 4 * sizeof(*result));
    result[0].name = "AllocateGameResources"; result[0].addr = (uintptr_t)allocation; result[0].rva = 0x100; result[0].status = SIG_OK;
    result[1].name = "UnloadGameResources"; result[1].addr = (uintptr_t)unload; result[1].rva = 0x300; result[1].status = SIG_OK;
    result[2].name = "FinalizeMapChange"; result[2].addr = (uintptr_t)finalize; result[2].rva = 0x800; result[2].status = SIG_OK;
    result[3].name = "CancelMapChange"; result[3].addr = (uintptr_t)cancel; result[3].rva = 0xb00; result[3].status = SIG_OK;
}
static void reset_case(void)
{
    checks_pending = activations = finishes = finished_result = allocations = unloads = purges = compares = 0;
    observed_force = observed_full = step = mode = cancellations = native_finalized = native_frame = fatals = 0;
    common[0xf576] = common[0xf577] = 0;
    install_commits = 0;
    pending_result = activation_result = 1; has_resource = 1; force_full = 0;
    pending_resource = (void *)123;
    thread_id = GetCurrentThreadId();
    *(const char **)(manager + 0x2c70) = "maps/old";
    *(const char **)(parameters + 0x30) = "maps/new";
    parameters[0x170f8] = 0;
    assert(g_trans.phase == TRANS_IDLE && !g_trans.parameters);
}
static DWORD WINAPI wrong_thread(void *unused)
{
    (void)unused;
    assert(transition_allocate(manager, common + 0x1298, parameters, (void *)4));
    assert(!sh_map_transition_at_boundary()); return 0;
}
int main(void)
{
    sig_result result[4];
    callbacks.pending = pending; callbacks.activate = activate; callbacks.commit = commit_install; callbacks.finished = finished;
    thread_id = GetCurrentThreadId(); make_binding(result);
    assert(sh_map_transition_bind(result, 4, image));
    assert(!sh_map_transition_bind(result, 1, image));
    result[0].rva++; assert(!sh_map_transition_bind(result, 4, image)); result[0].rva--;
    image[0x100 + 0x95] ^= 1; assert(!sh_map_transition_bind(result, 4, image)); image[0x100 + 0x95] ^= 1;
    image[0x300 + 0x1b3] ^= 1; assert(!sh_map_transition_bind(result, 4, image)); image[0x300 + 0x1b3] ^= 1;
    assert(!sh_map_transition_install(result, 4, image, NULL));
    fail_prepare = 2;
    assert(!sh_map_transition_install(result, 4, image, &callbacks) && !g_trans_ready && removes == 1);
    prepares = commits = removes = fail_prepare = 0; fail_commit = 2; fail_remove = 1;
    assert(!sh_map_transition_install(result, 4, image, &callbacks) && !g_trans_ready);
    reset_case(); pending_result = -1;
    transition_finalize(common, parameters, (void *)4);
    assert(!checks_pending && allocations == 1 && unloads == 1);
    assert(!sh_map_transition_install(result, 4, image, &callbacks));
    prepares = commits = removes = fail_commit = fail_remove = 0;
    assert(sh_map_transition_install(result, 4, image, &callbacks));
    assert(g_trans_ready && prepares == 4 && commits == 4 && removes == 4);
    g_trans_cancel = native_cancel;
    for (int select = -1; select <= 1; select++) {
        reset_case(); pending_result = select;
        transition_finalize(common, parameters, (void *)4);
        assert(allocations == (select >= 0) && unloads == (select >= 0));
        assert(activations == (select == 1) && finishes == (select != 0));
        assert(!sh_map_transition_at_boundary() && g_trans.phase == TRANS_IDLE && !fatals && !common[0xf576]);
    }
    for (int flags = 0; flags < 8; flags++) {
        reset_case(); has_resource = flags & 1; force_full = (flags & 2) != 0;
        parameters[0x170f8] = flags & 4 ? 0x20 : 0;
        *(const char **)(parameters + 0x30) = "maps/old";
        transition_finalize(common, parameters, (void *)4);
        assert(observed_full == (has_resource && force_full) && observed_force == ((flags & 4) != 0));
        assert(compares == (has_resource ? 2 : 0) && unloads == 1 && purges == 1 && finishes == 1);
    }
    for (int failure = 1; failure <= 18; failure++) {
        DWORD caught = 0;
        reset_case(); mode = failure;
        if (mode == 2 || mode == 14) activation_result = 0;
        __try { transition_finalize(common, parameters, (void *)4); }
        __except (EXCEPTION_EXECUTE_HANDLER) { caught = GetExceptionCode(); }
        assert(!!caught == (mode == 5 || mode == 6 || mode == 12 || mode == 13 || mode == 14 || mode == 16));
        assert(finishes == (mode != 16) && finished_result == (mode == 8 || mode == 10));
        assert(!fatals && !native_frame && g_trans.phase == TRANS_IDLE && !g_trans.parameters);
        if (!caught) assert(!common[0xf576] && !common[0xf577]);
        if (mode == 12) assert(caught == TRANS_CANCEL_EXCEPTION && !cancellations);
        if (mode == 13) assert(!cancellations && allocations == 1);
        assert(install_commits == (mode == 8 || mode == 10 || mode == 17 || mode == 18));
        if (mode >= 17) assert(cancellations == 1 && native_finalized == 1 && !finished_result);
    }
    reset_case();
    assert(transition_allocate(manager, common + 0x1298, parameters, (void *)4));
    assert(!checks_pending && !finishes && allocations == 1);
    reset_case();
    {
        HANDLE thread = CreateThread(NULL, 0, wrong_thread, NULL, 0, NULL);
        assert(thread && WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0); CloseHandle(thread);
        assert(!checks_pending && allocations == 1 && unloads == 1 && !finishes);
    }
    reset_case(); g_trans.parameters = parameters; g_trans.extra = (void *)4;
    assert(transition_allocate_from(manager, common + 0x1298, parameters, (void *)4, image + 0x800 + 0x1eb));
    assert(!checks_pending && allocations == 1 && !finishes);
    memset(&g_trans, 0, sizeof(g_trans));
    /* A neighboring return address or another owner cannot consume the bypass. */
    reset_case(); g_trans.phase = TRANS_LOADING; g_trans.skip_unload = 1;
    transition_unload_from(common, 0, 0, image + 0x100 + 0xaf);
    assert(unloads == 1 && g_trans.skip_unload);
    transition_unload_from(common, 0, 0, image + 0x100 + 0xae);
    assert(unloads == 1 && !g_trans.skip_unload);
    transition_unload_from(common, 0, 0, image + 0x100 + 0xae);
    assert(unloads == 2);
    memset(&g_trans, 0, sizeof(g_trans));
    puts("map_transition_test: PASS"); return 0;
}
