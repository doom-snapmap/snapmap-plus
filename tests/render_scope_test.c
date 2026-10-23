/* Renderer ownership, nested refreshes and partial native-call failures. */
#define WIN32_LEAN_AND_MEAN
#include <assert.h>
#include <stdio.h>
#include "../src/backend/render_scope.h"

static unsigned char object[64], replacement[64];
static void *table[40], *renderer;
static DWORD main_thread;
static int mode, synchronizations, increments, decrements, updates;
enum { OK, SYNC_FAULT, INC_BEFORE, INC_AFTER, INC_NOOP, INC_EXTRA,
    DEC_BEFORE, DEC_AFTER, DEC_NOOP, UPDATE_FAULT };
#define COUNT (*(int *)(object + 0x14))

static void fail(void) { RaiseException(0xe0530051, 0, 0, NULL); }
static void synchronize(void *self, unsigned char a, unsigned char b, unsigned char c)
{
    assert(self == object && !a && !b && c == 1);
    synchronizations++;
    if (mode == SYNC_FAULT) fail();
}
static void adjust(void *self, int delta)
{
    assert(self == object && (delta == 1 || delta == -1));
    if (delta == 1) {
        increments++;
        if (mode == INC_BEFORE) fail();
        if (mode == INC_NOOP) return;
        COUNT += mode == INC_EXTRA ? 2 : 1;
        if (mode == INC_AFTER) fail();
    } else {
        decrements++;
        if (mode == DEC_BEFORE) fail();
        if (mode == DEC_NOOP) return;
        COUNT--;
        if (mode == DEC_AFTER) fail();
    }
}
static void update(void *self)
{
    assert(self == object && COUNT > 0);
    updates++;
    if (mode == UPDATE_FAULT) fail();
}
static void reset(void)
{
    memset(object, 0, sizeof(object)); memset(table, 0, sizeof(table));
    *(void ***)object = table;
    table[0x80/8] = synchronize; table[0x110/8] = adjust; table[0x108/8] = update;
    renderer = object; main_thread = GetCurrentThreadId();
    mode = synchronizations = increments = decrements = updates = 0;
}
static DWORD WINAPI other_thread(void *context)
{
    sh_render_scope empty = {0};
    assert(!sh_render_scope_leave(context));
    assert(!sh_render_scope_update(context));
    assert(!sh_render_scope_enter(&renderer, &main_thread, &empty) && !empty.active);
    return 0;
}
static void ownership_cases(void)
{
    sh_render_scope outer = {0}, inner = {0};
    HANDLE worker;
    reset(); COUNT = 3;
    assert(sh_render_scope_enter(&renderer, &main_thread, &outer) && COUNT == 4);
    assert(!sh_render_scope_enter(&renderer, &main_thread, &outer) && COUNT == 4);
    assert(sh_render_scope_update(&outer) && updates == 1);
    assert(sh_render_scope_enter(&renderer, &main_thread, &inner) && COUNT == 5);
    assert(!sh_render_scope_leave(&outer) && !decrements && COUNT == 5);
    assert(!sh_render_scope_update(&outer) && updates == 1);
    assert(sh_render_scope_update(&inner) && updates == 2);
    assert(sh_render_scope_leave(&inner) && COUNT == 4);
    worker = CreateThread(NULL, 0, other_thread, &outer, 0, NULL);
    assert(worker && WaitForSingleObject(worker, 3000) == WAIT_OBJECT_0);
    CloseHandle(worker);
    assert(COUNT == 4 && decrements == 1 && updates == 2);
    renderer = replacement;
    assert(!sh_render_scope_leave(&outer) && !sh_render_scope_update(&outer));
    renderer = object; main_thread++;
    assert(!sh_render_scope_leave(&outer)); main_thread--;
    assert(sh_render_scope_leave(&outer) && COUNT == 3 && decrements == 2);
    assert(sh_render_scope_leave(&outer) && decrements == 2);
    assert(!sh_render_scope_update(&outer));
}
static void acquisition_cases(void)
{
    for (int failure = SYNC_FAULT; failure <= INC_EXTRA; failure++) {
        sh_render_scope scope = {0};
        reset(); mode = failure;
        assert(!sh_render_scope_enter(&renderer, &main_thread, &scope));
        assert(!updates);
        if (failure == INC_EXTRA) {
            assert(scope.active && COUNT == 2 && !decrements);
            assert(!sh_render_scope_leave(&scope) && COUNT == 2);
            /* Only after the interfering owner returns its hold can ours leave. */
            COUNT--; mode = OK;
            assert(sh_render_scope_leave(&scope) && COUNT == 0 && decrements == 1);
        } else {
            assert(!scope.active && !COUNT);
            assert(decrements == (failure == INC_AFTER));
        }
    }
    {
        sh_render_scope scope = {0};
        reset(); COUNT = -1;
        assert(!sh_render_scope_enter(&renderer, &main_thread, &scope));
        COUNT = INT_MAX;
        assert(!sh_render_scope_enter(&renderer, &main_thread, &scope));
        COUNT = 0; table[0x108/8] = NULL;
        assert(!sh_render_scope_enter(&renderer, &main_thread, &scope));
        assert(!scope.active && !synchronizations && !increments);
        assert(!sh_render_scope_enter(NULL, &main_thread, &scope));
        assert(!sh_render_scope_enter(&renderer, NULL, &scope));
        assert(!sh_render_scope_enter(&renderer, &main_thread, NULL));
        assert(sh_render_scope_leave(NULL) && !sh_render_scope_update(NULL));
        renderer = (void *)1;
        assert(!sh_render_scope_enter(&renderer, &main_thread, &scope) && !scope.active);
    }
}
static void restoration_cases(void)
{
    for (int failure = DEC_BEFORE; failure <= DEC_NOOP; failure++) {
        sh_render_scope scope = {0};
        reset(); assert(sh_render_scope_enter(&renderer, &main_thread, &scope));
        mode = failure;
        assert(!sh_render_scope_leave(&scope) && decrements == 1);
        if (failure == DEC_AFTER) {
            assert(!scope.active && COUNT == 0);
            assert(sh_render_scope_leave(&scope) && decrements == 1);
        } else {
            assert(scope.active && COUNT == 1);
            mode = OK;
            assert(sh_render_scope_leave(&scope) && COUNT == 0 && decrements == 2);
        }
    }
    {
        sh_render_scope scope = {0};
        reset(); assert(sh_render_scope_enter(&renderer, &main_thread, &scope));
        mode = UPDATE_FAULT;
        assert(!sh_render_scope_update(&scope) && scope.active && COUNT == 1);
        assert(sh_render_scope_leave(&scope) && COUNT == 0);
    }
}
int main(void)
{
    ownership_cases(); acquisition_cases(); restoration_cases();
    puts("render_scope_test OK"); return 0;
}
