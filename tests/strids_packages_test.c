/* Tests package #str_ injection and user/package/default precedence. Engine
 * string-pool and list operations are doubled to inspect appended key/value pairs. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#include "../src/backend/strids.h"
#include "../src/backend/overrides.h"
#include "../src/backend/resource_bridge.h"
#include "../src/backend/hook.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_failed++; } } while (0)

/* Stub collaborators and capture logs so conflict tests can check both package names. */
static char g_log[8192];

void backend_log(const char *message)
{
    if (!message) return;
    strncat_s(g_log, sizeof g_log, message, _TRUNCATE);
    strncat_s(g_log, sizeof g_log, "\n", _TRUNCATE);
}

static int g_hook_mode, g_hook_prepare_fail, g_hook_unpatch_fail;
static int g_hook_installed, g_hook_invoke_on_commit;
static int g_hook_prepares, g_hook_commits, g_hook_unpatches;
static void *g_hook_owned;
static void (*g_hook_detour)(void *, void *, unsigned int, unsigned int);
static sh_patch_status g_hook_commit_result;
static void invoke_sort_hook(void);

void *hook_prepare(void *target, void *detour, size_t stolen)
{
    CHECK(g_hook_mode && target != NULL && detour != NULL && stolen == 16);
    CHECK(g_hook_owned == NULL);
    g_hook_prepares++;
    if (g_hook_prepare_fail) return NULL;
    g_hook_owned = target;
    g_hook_detour = (void (*)(void *, void *, unsigned int, unsigned int))detour;
    return target;
}
sh_patch_status hook_commit(void *tramp)
{
    CHECK(tramp == g_hook_owned && tramp != NULL);
    g_hook_commits++;
    CHECK(sh_strids_rearm() == 0);
    if (g_hook_invoke_on_commit) invoke_sort_hook();
    g_hook_installed = g_hook_commit_result == B2_PATCH_OK;
    return g_hook_commit_result;
}
int hook_is_installed(void *tramp)
{
    if (!g_hook_mode) return tramp != NULL; /* directly bound helper tests */
    return tramp != NULL && tramp == g_hook_owned && g_hook_installed;
}
int hook_unpatch(void *tramp)
{
    CHECK(tramp == g_hook_owned && tramp != NULL);
    g_hook_unpatches++;
    g_hook_installed = 0;
    if (g_hook_unpatch_fail) return 0;
    g_hook_owned = NULL;
    g_hook_detour = NULL;
    return 1;
}

int sh_user_overrides_enabled_for_launch(void) { return 1; }

int sh_resource_bridge_capture(const char *data_root) { (void)data_root; return 1; }

void sh_resource_bridge_set_provider_ready(int ready) { (void)ready; }

int sh_resource_bridge_open(const char *name, unsigned char **out,
                            size_t *out_length, const char **out_source)
{
    (void)name;
    if (out) *out = NULL;
    if (out_length) *out_length = 0;
    if (out_source) *out_source = NULL;
    return SH_RESOURCE_BRIDGE_MISS;
}

/* Stub baked navigation; only its lookup priority matters here. */
#include "../src/backend/nav_bake.h"

/* With no marked-volume bake, lookup must fall through to the file shadow. */
int sh_nav_bake_open(const char *name, sh_nav_bake_reader read_shipped,
                     unsigned char **out_bytes, size_t *out_len)
{
    (void)name; (void)read_shipped;
    if (out_bytes) *out_bytes = NULL;
    if (out_len) *out_len = 0;
    return 0;
}

int sh_navmesh_open(const char *name, unsigned char **out_bytes, size_t *out_len)
{
    (void)name;
    if (out_bytes) *out_bytes = NULL;
    if (out_len) *out_len = 0;
    return 0;
}

/* ------------------------------------------------------------------ engine doubles */

#define CAP 1024
static char g_keys[CAP][256];
static char g_vals[CAP][512];
static int  g_pairs;
static int  g_appended;
static unsigned char g_records[CAP][32];
static struct { void *rows; unsigned int count, capacity; } g_dictionary = {g_records, 0, CAP};
static char *g_pool[CAP * 4];
static int g_pool_count;
static int g_sorts;
static int g_sort_fault;
static int g_insert_refused;

/* Record constructor order to reconstruct each appended key/value pair. */
static void fake_idstr_ctor(void *out_handle, const char *s)
{
    char *copy = _strdup(s ? s : "");
    CHECK(copy != NULL && g_pool_count < CAP * 4);
    if (!copy || g_pool_count >= CAP * 4) return;
    g_pool[g_pool_count++] = copy;
    *(void **)out_handle = copy;
    if (g_pairs >= CAP) return;
    if (g_keys[g_pairs][0] == '\0' && s && s[0] == '#')
        strncpy_s(g_keys[g_pairs], sizeof g_keys[0], s, _TRUNCATE);
    else
        strncpy_s(g_vals[g_pairs], sizeof g_vals[0], s ? s : "", _TRUNCATE);
}

static unsigned int fake_hash(const char *s)
{
    unsigned int h = 2166136261u;
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static int fake_insert(void *table_desc, void *record32)
{
    (void)table_desc;
    if (g_insert_refused) return -1;
    CHECK(g_dictionary.count < CAP);
    if (g_dictionary.count >= CAP) return -1;
    memcpy(g_records[g_dictionary.count++], record32, 32);
    g_appended++;
    if (g_pairs < CAP) g_pairs++;     /* one (key,value) pair completed */
    return 1;
}

static void reset_doubles(void)
{
    int i;
    for (i = 0; i < g_pool_count; i++) free(g_pool[i]);
    g_pool_count = 0;
    g_dictionary.count = 0;
    g_sorts = 0;
    memset(g_keys, 0, sizeof g_keys);
    memset(g_vals, 0, sizeof g_vals);
    g_pairs = 0;
    g_appended = 0;
}

/* Find the value the injector appended for "#str_<id>", or NULL if it never appended that key. */
static const char *value_for(const char *id)
{
    char want[256];
    _snprintf_s(want, sizeof want, _TRUNCATE, "#str_%s", id);
    for (unsigned int i = 0; i < g_dictionary.count; i++)
        if (_stricmp(*(char **)(g_records[i] + 8), want) == 0)
            return *(char **)(g_records[i] + 16);
    return NULL;
}

static int count_key(const char *id)
{
    char want[256];
    int n = 0;
    _snprintf_s(want, sizeof want, _TRUNCATE, "#str_%s", id);
    for (unsigned int i = 0; i < g_dictionary.count; i++)
        if (_stricmp(*(char **)(g_records[i] + 8), want) == 0) n++;
    return n;
}

/* ------------------------------------------------------------------ fixture helpers */

/* Remove old PID-named fixtures so a reused process ID cannot inherit packages. */
static void remove_tree(const char *path)
{
    char pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", path);
    search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(found.cFileName, ".") == 0 ||
                strcmp(found.cFileName, "..") == 0) continue;
            _snprintf_s(child, sizeof child, _TRUNCATE, "%s\\%s", path, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else DeleteFileA(child);
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    RemoveDirectoryA(path);
}


static int make_dir(const char *p) { return CreateDirectoryA(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS; }

static int write_text(const char *path, const char *body)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wrote = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    WriteFile(h, body, (DWORD)strlen(body), &wrote, NULL);
    CloseHandle(h);
    return wrote == (DWORD)strlen(body);
}

/* Create <overrides>\<name> as a real package (marker) with a strings\<file>.json document. */
static int install_strings(const char *overrides, const char *name, const char *file, const char *body)
{
    char dir[MAX_PATH], path[MAX_PATH];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", overrides, name);
    if (!make_dir(dir)) return 0;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\package.json", dir);
    if (!write_text(path, "{}")) return 0;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\strings", dir);
    if (!make_dir(path)) return 0;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\strings\\%s", dir, file);
    return write_text(path, body);
}

static void run_inject(void)
{
    reset_doubles();
    g_log[0] = '\0';
    sh_strids_test_inject(&g_dictionary, (void *)fake_insert, (void *)fake_hash, (void *)fake_idstr_ctor);
}

static int compare_record(const void *a, const void *b)
{
    unsigned int x = *(const unsigned int *)a, y = *(const unsigned int *)b;
    return x < y ? -1 : x > y;
}

static void fake_sort(void *ctx, void *rows, unsigned int count, unsigned int radix)
{
    CHECK(ctx == &g_dictionary && rows == g_records && radix == 0x20);
    qsort(rows, count, 32, compare_record);
    g_sorts++;
    if (g_sort_fault) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
}

static void invoke_sort_hook(void)
{
    int before = g_sorts;
    CHECK(g_hook_detour != NULL);
    g_hook_detour(&g_dictionary, g_records, g_dictionary.count, 0x20);
    CHECK(g_sorts == before + 1);
}

static void test_sort_hook_recovery(const char *user_path)
{
    static unsigned char table_lea[64];
    intptr_t distance = (unsigned char *)&g_dictionary - (table_lea + 7);
    int32_t displacement = (int32_t)distance;
    int prepares;
    unsigned int rows;
    CHECK((intptr_t)displacement == distance);
    memcpy(table_lea, "\x48\x8d\x0d", 3);
    memcpy(table_lea + 3, &displacement, 4);
    sh_strids_test_set_sort(NULL); /* the previous tests used no owned hook */
    g_hook_mode = 1;

    g_hook_prepare_fail = 1;
    CHECK(sh_strids_install(fake_sort, 1, table_lea, fake_insert,
                           fake_hash, fake_idstr_ctor) == 0);
    CHECK(g_hook_prepares == 1 && g_hook_commits == 0 && g_hook_owned == NULL);
    CHECK(sh_strids_rearm() == 0);

    g_hook_prepare_fail = 0;
    g_hook_commit_result = B2_PATCH_FAIL_SEH;
    CHECK(sh_strids_install(fake_sort, 1, table_lea, fake_insert,
                           fake_hash, fake_idstr_ctor) == 0);
    CHECK(g_hook_prepares == 2 && g_hook_commits == 1 && g_hook_unpatches == 1);
    CHECK(g_hook_owned == NULL && sh_strids_rearm() == 0);

    g_hook_commit_result = B2_PATCH_FAIL_ROLLBACK;
    g_hook_unpatch_fail = 1;
    g_hook_invoke_on_commit = 1;
    CHECK(sh_strids_install(fake_sort, 1, table_lea, fake_insert,
                           fake_hash, fake_idstr_ctor) == 0);
    CHECK(g_hook_prepares == 3 && g_hook_commits == 2 && g_hook_unpatches == 2);
    CHECK(g_hook_owned != NULL && !hook_is_installed(g_hook_owned));
    CHECK(write_text(user_path, "{ \"hook_recovery_key\" : \"Recovery\" }"));
    rows = g_dictionary.count;
    invoke_sort_hook();
    CHECK(g_dictionary.count == rows && count_key("hook_recovery_key") == 0);
    CHECK(sh_strids_rearm() == 0);
    prepares = g_hook_prepares;
    CHECK(sh_strids_install(fake_sort, 1, table_lea, fake_insert,
                           fake_hash, fake_idstr_ctor) == 0);
    CHECK(g_hook_prepares == prepares && g_hook_owned != NULL);

    g_hook_unpatch_fail = 0;
    g_hook_commit_result = B2_PATCH_OK;
    CHECK(sh_strids_install(fake_sort, 1, table_lea, fake_insert,
                           fake_hash, fake_idstr_ctor) == 1);
    CHECK(g_hook_prepares == prepares + 1 && hook_is_installed(g_hook_owned));
    CHECK(count_key("hook_recovery_key") == 1);
    CHECK(sh_strids_rearm() == 1);
    CHECK(count_key("hook_recovery_key") == 1);
    prepares = g_hook_prepares;
    CHECK(sh_strids_install(fake_sort, 1, table_lea, fake_insert,
                           fake_hash, fake_idstr_ctor) == 1);
    CHECK(g_hook_prepares == prepares);
}


/* The overlap reporter, stubbed -- see the note in overrides_internal_test.c. */
int sh_pkg_conflicts_report(const char *data_root) { (void)data_root; return 0; }

int main(void)
{
    char temp[MAX_PATH], root[MAX_PATH], overrides[MAX_PATH], path[MAX_PATH];
    DWORD pid = GetCurrentProcessId();

    GetTempPathA(sizeof temp, temp);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%ssnapmap-plus-strids-pkg-%lu", temp, (unsigned long)pid);
    remove_tree(root);                 /* a recycled pid must not inherit an earlier run's packages */
    CHECK(make_dir(root));
    _snprintf_s(overrides, sizeof overrides, _TRUNCATE, "%s\\overrides", root);
    CHECK(make_dir(overrides));
    sh_overrides_set_root(root);
    /* Set every source path inside the fixture to avoid reading the user's
     * real strings document through the default-path fallback. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\user_strids.json", root);
    sh_strids_set_source(path);

    /* A package must supply strings without edits to the global user document. */
    CHECK(install_strings(overrides, "cyberdemon", "cyberdemon.json",
                          "{ \"ai_cyberdemon_name\" : \"Cyberdemon\","
                          "  \"cyber_desc\" : \"A towering cybernetic demon.\" }"));
    run_inject();
    CHECK(value_for("ai_cyberdemon_name") != NULL);
    CHECK(value_for("ai_cyberdemon_name") && strcmp(value_for("ai_cyberdemon_name"), "Cyberdemon") == 0);
    CHECK(value_for("cyber_desc") != NULL);

    /* Retain source-package attribution for conflict reports. */
    {
        int i, found = 0;
        const char *id = NULL, *owner = NULL;
        for (i = 0; sh_strids_test_row(i, &id, &owner); i++)
            if (id && _stricmp(id, "ai_cyberdemon_name") == 0) {
                found = 1;
                CHECK(owner && strcmp(owner, "cyberdemon") == 0);
            }
        CHECK(found);
    }

    /* Identical shared strings compose into one row without a conflict. */
    CHECK(install_strings(overrides, "zz-second", "shared.json",
                          "{ \"shared_key\" : \"Shared Text\" }"));
    CHECK(install_strings(overrides, "cyberdemon", "shared.json",
                          "{ \"shared_key\" : \"Shared Text\" }"));
    run_inject();
    CHECK(count_key("shared_key") == 1);

    /* On disagreement, retain the first value and never append duplicate keys
     * to the engine's hash-sorted dictionary. */
    CHECK(install_strings(overrides, "zz-second", "shared.json",
                          "{ \"shared_key\" : \"A DIFFERENT VALUE\" }"));
    run_inject();
    CHECK(count_key("shared_key") == 1);
    CHECK(value_for("shared_key") && strcmp(value_for("shared_key"), "Shared Text") == 0);
    /* A conflict must identify both packages. */
    CHECK(strstr(g_log, "REFUSED") != NULL);
    CHECK(strstr(g_log, "shared_key") != NULL);
    CHECK(strstr(g_log, "zz-second") != NULL);
    CHECK(strstr(g_log, "cyberdemon") != NULL);

    /* The explicit user document takes precedence over package strings. */
    CHECK(write_text(path, "{ \"ai_cyberdemon_name\" : \"MY OWN NAME\" }"));
    run_inject();
    CHECK(count_key("ai_cyberdemon_name") == 1);
    CHECK(value_for("ai_cyberdemon_name") &&
          strcmp(value_for("ai_cyberdemon_name"), "MY OWN NAME") == 0);
    /* Rearm appends newly installed strings and updates owned keys after
     * native sorting has moved their rows. Repeated refresh never duplicates. */
    sh_strids_test_set_sort((void *)fake_sort);
    CHECK(install_strings(overrides, "new-runtime", "runtime.json",
                          "{ \"runtime_key\" : \"First value\" }"));
    CHECK(sh_strids_rearm() == 1);
    CHECK(count_key("runtime_key") == 1);
    CHECK(strcmp(value_for("runtime_key"), "First value") == 0);
    CHECK(install_strings(overrides, "new-runtime", "runtime.json",
                          "{ \"runtime_key\" : \"Updated value\" }"));
    CHECK(sh_strids_rearm() == 1);
    CHECK(count_key("runtime_key") == 1);
    CHECK(strcmp(value_for("runtime_key"), "Updated value") == 0);
    CHECK(sh_strids_rearm() == 1);
    CHECK(count_key("runtime_key") == 1 && count_key("shared_key") == 1);
    CHECK(g_sorts == 3);
    for (unsigned int i = 1; i < g_dictionary.count; i++)
        CHECK(*(unsigned int *)g_records[i-1] <= *(unsigned int *)g_records[i]);
    CHECK(strcmp(value_for("ai_cyberdemon_name"), "MY OWN NAME") == 0);
    g_sort_fault = 1;
    CHECK(sh_strids_rearm() == 0);
    g_sort_fault = 0;
    CHECK(sh_strids_rearm() == 1);
    CHECK(install_strings(overrides, "new-runtime", "refused.json",
                          "{ \"append_refusal\" : \"try again\" }"));
    g_insert_refused = 1;
    CHECK(sh_strids_rearm() == 0);
    CHECK(count_key("append_refusal") == 0);
    g_insert_refused = 0;
    CHECK(sh_strids_rearm() == 1);
    CHECK(count_key("append_refusal") == 1);
    test_sort_hook_recovery(path);
    /* Overflow is a failed inventory, never a partial new string set. */
    for (int i = 0; i < 65; i++) {
        char package[32];
        _snprintf_s(package, sizeof(package), _TRUNCATE, "overflow-%02d", i);
        CHECK(install_strings(overrides, package, "value.json",
                              "{ \"partial_key\" : \"must not appear\" }"));
    }
    CHECK(sh_strids_rearm() == 0);
    CHECK(count_key("partial_key") == 0);
    g_hook_installed = 0; /* simulate a removal that still owns its record */
    CHECK(sh_strids_install(NULL, 0, NULL, NULL, NULL, NULL) == 0);
    CHECK(g_hook_owned == NULL && sh_strids_rearm() == 0);
    sh_strids_set_source(NULL);

    /* Without packages, injection must still supply baked defaults. */
    sh_overrides_set_root(NULL);

    remove_tree(root);                 /* and do not leave one behind for the next run either */
    reset_doubles();

    if (g_failed) {
        fprintf(stderr, "strids_packages_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    printf("strids_packages_test: ok\n");
    return 0;
}
