#include <assert.h>
#include "../src/backend/decl_visibility.c"

static int native_result, native_calls, published, published_calls, throw_error;
static const char *expected_path;
void backend_log(const char *message) { (void)message; }
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{ (void)base; (void)name; (void)status; return 0; }
sig_status sig_resolve_one(const uint8_t *base, const sig_entry *entry, sig_result *out)
{ (void)base; (void)entry; (void)out; return SIG_NOT_FOUND; }
int sh_overrides_internal_decl_published(const char *key)
{
    published_calls++;
    assert(!strncmp(key, SH_OVERRIDES_INTERNAL_DECL_PREFIX, sizeof(SH_OVERRIDES_INTERNAL_DECL_PREFIX) - 1));
    assert(!strcmp(key + sizeof(SH_OVERRIDES_INTERNAL_DECL_PREFIX) - 1,
                   expected_path + sizeof(DV_PATH_PREFIX) - 1));
    return published;
}
static unsigned char native_probe(void *self, const char *path, void *out1,
    void *out2, void *out3, void *out4, unsigned char quiet)
{
    assert(self == (void *)17 && path == expected_path && quiet == 1);
    assert(out1 && out2 && out3 && out4);
    *(uint64_t *)out1 = UINT64_MAX;
    *(uint64_t *)out2 = UINT64_MAX;
    *(uint64_t *)out3 = UINT64_MAX;
    *(unsigned char *)out4 = 1;
    native_calls++;
    if (throw_error) RaiseException(0xe0001234, 0, 0, NULL);
    return (unsigned char)native_result;
}
int main(void)
{
    char *long_path = (char *)malloc(12000);
    assert(long_path);
    expected_path = "generated/decls/entitydef/example.decl";
    assert(sh_decl_visibility_source_exists(expected_path) == -1);
    g_orig_probe = native_probe; g_manager = (void *)17;
    native_result = 1;
    assert(sh_decl_visibility_source_exists(expected_path) == 1 && !published_calls);
    native_result = 0;
    assert(sh_decl_visibility_source_exists(expected_path) == 0 && published_calls == 1);
    published = 1;
    assert(sh_decl_visibility_source_exists(expected_path) == 1 && published_calls == 2);
    throw_error = 1;
    assert(sh_decl_visibility_source_exists(expected_path) == -1 && published_calls == 2);
    throw_error = 0;
    expected_path = "generated/images/example.bimage";
    assert(sh_decl_visibility_source_exists(expected_path) == 0 && published_calls == 2);
    assert(sh_decl_visibility_source_exists(NULL) == -1);
    assert(sh_decl_visibility_source_exists("") == -1);
    strcpy(long_path, "generated/decls/entitydef/");
    memset(long_path + strlen(long_path), 'x', 11900 - strlen(long_path));
    strcpy(long_path + 11900, ".decl"); expected_path = long_path;
    assert(sh_decl_visibility_source_exists(expected_path) == 1);
    assert(native_calls == 6 && published_calls == 3);
    free(long_path); puts("decl_visibility_test: PASS"); return 0;
}
