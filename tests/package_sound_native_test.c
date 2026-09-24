#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/package_sound_native.c"

static int failures, original_calls, lookup_calls, prepare_calls, unpatch_calls;
static int fail_prepare, fail_unpatch, saw_published_original;
static sh_patch_status commit_status;
static uint32_t published_event;
static void (*captured_detour)(void *);
static unsigned char entry[] = {
    0x48,0x8b,0xc4,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0x48,0xfe,0xff,0xff
};
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

void backend_log(const char *message) { (void)message; }
int sh_package_runtime_sound_event(const char *name, uint32_t *event)
{
    lookup_calls++; *event = 0;
    if (name && !strcmp(name, "smp/sound_a") && published_event) {
        *event = published_event; return 1;
    }
    return 0;
}
static void original(void *sound)
{
    original_calls++;
    if (!sound) RaiseException(0xe1234567, 0, 0, NULL);
    ((unsigned char *)sound)[0xa8] = 0x4f;
    *(uint32_t *)((unsigned char *)sound + 0xac) = 900;
}
void *hook_prepare(void *target, void *detour, size_t stolen)
{
    prepare_calls++; CHECK(target == entry && stolen == sizeof(entry));
    captured_detour = (void (*)(void *))detour;
    return fail_prepare ? NULL : (void *)original;
}
sh_patch_status hook_commit(void *trampoline)
{
    CHECK(trampoline == (void *)original);
    saw_published_original = g_psn_original == original;
    return commit_status;
}
int hook_unpatch(void *trampoline)
{
    CHECK(trampoline == (void *)original); unpatch_calls++;
    return !fail_unpatch;
}
static void reset(void)
{
    g_psn_original = NULL; g_psn_trampoline = NULL; g_psn_ready = 0;
    original_calls = lookup_calls = prepare_calls = unpatch_calls = 0;
    fail_prepare = fail_unpatch = saw_published_original = 0;
    commit_status = B2_PATCH_OK; published_event = 0; captured_detour = NULL;
}
int main(void)
{
    sig_result result = {"SoundEventInitialize", SIG_OK, (uintptr_t)entry, 0};
    unsigned char sound[0xb0]; memset(sound, 0x6c, sizeof(sound));
    *(const char **)(sound + 8) = "smp/sound_a";
    reset(); CHECK(!sh_package_sound_native_ready());
    CHECK(!sh_package_sound_native_install(NULL, 0) && !prepare_calls);
    result.status = SIG_OK_HOOKED;
    CHECK(!sh_package_sound_native_install(&result, 1) && !prepare_calls);
    result.status = SIG_OK; entry[0] = 0x90;
    CHECK(!sh_package_sound_native_install(&result, 1) && !prepare_calls);
    entry[0] = 0x48; fail_prepare = 1;
    CHECK(!sh_package_sound_native_install(&result, 1) && !sh_package_sound_native_ready());
    fail_prepare = 0; commit_status = B2_PATCH_FAIL_ROLLBACK; fail_unpatch = 1;
    CHECK(!sh_package_sound_native_install(&result, 1));
    CHECK(saw_published_original && g_psn_trampoline && !sh_package_sound_native_ready());
    int prepared = prepare_calls;
    CHECK(!sh_package_sound_native_install(&result, 1) && prepare_calls == prepared);
    fail_unpatch = 0; commit_status = B2_PATCH_OK;
    CHECK(sh_package_sound_native_install(&result, 1) && sh_package_sound_native_ready());
    CHECK(saw_published_original && sh_package_sound_native_install(&result, 1));

    /* Parse, event-table construction, and preview all re-enter the same
     * initializer. Repeated calls must not overwrite a retained binding. */
    published_event = 41;
    for (int i = 0; i < 3; i++) {
        captured_detour(sound);
        CHECK(*(uint32_t *)(sound + 0xac) == 41 && sound[0xa8] == 0x4f && sound[0xa9] == 0x6c);
    }
    CHECK(original_calls == 3 && lookup_calls == 3);
    published_event = 52; captured_detour(sound);
    CHECK(*(uint32_t *)(sound + 0xac) == 52);
    published_event = 41; captured_detour(sound);
    CHECK(*(uint32_t *)(sound + 0xac) == 41);
    published_event = 0; captured_detour(sound);
    CHECK(*(uint32_t *)(sound + 0xac) == 900);
    *(const char **)(sound + 8) = "play_native"; published_event = 41;
    captured_detour(sound); CHECK(*(uint32_t *)(sound + 0xac) == 900);
    int lookups = lookup_calls, caught = 0;
    __try { captured_detour(NULL); }
    __except (GetExceptionCode() == 0xe1234567 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 1; }
    CHECK(caught && lookup_calls == lookups);
    printf("package_sound_native_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
