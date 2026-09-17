/* Tests package #str_ injection and user/package/default precedence. Engine
 * string-pool and list operations are doubled to inspect appended key/value pairs. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#include "../src/backend/strids.h"
#include "package_runtime.h"
#include "../src/backend/overrides.h"
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


/* Stub baked navigation; only its lookup priority matters here. */
#include "../src/backend/nav_bake.h"

/* With no marked-volume bake, lookup must fall through to the file shadow. */
void sh_nav_bake_source_update_begin(void) {}
void sh_nav_bake_source_update_end(int committed) { (void)committed; }
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

#define CAP 4096
static char g_keys[CAP][2048];
static char g_vals[CAP][512];
static int  g_pairs;
static int  g_appended;
static unsigned char g_static_records[CAP][32];
static unsigned char (*g_records)[32] = g_static_records;
static struct { void *rows; unsigned int count, capacity; } g_dictionary = {g_static_records, 0, CAP};
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
    CHECK(g_dictionary.count < g_dictionary.capacity);
    if (g_dictionary.count >= g_dictionary.capacity) return -1;
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
    char want[2048];
    _snprintf_s(want, sizeof want, _TRUNCATE, "#str_%s", id);
    for (unsigned int i = 0; i < g_dictionary.count; i++)
        if (_stricmp(*(char **)(g_records[i] + 8), want) == 0)
            return *(char **)(g_records[i] + 16);
    return NULL;
}

static int count_key(const char *id)
{
    char want[2048];
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

/* Update inline strings without discarding this package's other string keys. */
static int install_strings(const char *overrides, const char *name, const char *file, const char *body)
{
    char dir[MAX_PATH], path[MAX_PATH], prior[8192], descriptor[16384];
    FILE *input = NULL;
    sh_json_object package = {0}, locales = {0}, strings = {0}, added = {0};
    const char *raw;
    char *encoded = NULL;
    size_t length;
    int ok = 0;
    (void)file;
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/%s", overrides, name);
    if (!make_dir(dir)) return 0;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/package.json", dir);
    if (!fopen_s(&input, path, "rb") && input) {
        length = fread(prior, 1, sizeof(prior) - 1, input); fclose(input); prior[length] = 0;
        if (!sh_json_parse_object(prior, length, 8, &package)) goto done;
        raw = sh_json_object_get(&package, "strings");
        if (raw && !sh_json_parse_object(raw, strlen(raw), 4, &locales)) goto done;
        raw = sh_json_object_get(&locales, "en");
        if (raw && !sh_json_parse_object(raw, strlen(raw), 2, &strings)) goto done;
    }
    if (!sh_json_parse_object(body, strlen(body), 2, &added)) goto done;
    for (size_t i = 0; i < added.count; i++)
        if (!sh_json_object_set(&strings, added.members[i].key, added.members[i].value_json, 2)) goto done;
    encoded = sh_json_serialize_object(&strings, 0, &length);
    if (!encoded || snprintf(descriptor, sizeof descriptor,
        "{\"id\":\"%s\",\"name\":\"%s\",\"strings\":{\"en\":%s}}", name, name, encoded) >= sizeof descriptor) goto done;
    ok = write_text(path, descriptor);
done:
    free(encoded); sh_json_object_free(&added); sh_json_object_free(&strings);
    sh_json_object_free(&locales); sh_json_object_free(&package); return ok;
}

static void run_inject(void)
{
    reset_doubles();
    g_log[0] = '\0';
    sh_overrides_rescan_packages();
    sh_strids_test_inject(&g_dictionary, (void *)fake_insert, (void *)fake_hash, (void *)fake_idstr_ctor);
}

/* Production declaration rearm refreshes the shared compiler first. */
static int rearm_strings(void)
{
    if (sh_overrides_rescan_packages() == SH_OVERRIDES_RESCAN_FAILED) return 0;
    return sh_strids_rearm();
}

/* Package size and extractor-shaped identifiers are not injector quotas.
 * Exercise the real compiler, then native refresh after sorting moved rows. */
static void test_large_strings(const char *overrides, const char *user_path)
{
    char directory[MAX_PATH], descriptor[MAX_PATH], id[1101];
    FILE *file = NULL;
    unsigned int before;
    memset(id, 'a', sizeof(id) - 1); id[sizeof(id) - 1] = 0;
    _snprintf_s(directory, sizeof directory, _TRUNCATE, "%s/large-strings", overrides);
    CHECK(make_dir(directory));
    _snprintf_s(descriptor, sizeof descriptor, _TRUNCATE, "%s/package.json", directory);
    CHECK(!fopen_s(&file, descriptor, "wb") && file);
    if (!file) return;
    fputs("{\"id\":\"large-strings\",\"name\":\"Large strings\",\"strings\":{\"en\":{", file);
    for (int i = 0; i < 1200; i++)
        fprintf(file, "\"large_%04d\":\"Value %d\",", i, i);
    fprintf(file, "\"%s\":\"Long ID\"}}}", id);
    CHECK(fclose(file) == 0);
    g_log[0] = 0;
    { int ok = rearm_strings(); if (!ok) fprintf(stderr, "large string refresh: %s\n", g_log); CHECK(ok == 1); }
    for (int i = 0; i < 1200; i++) {
        char key[32], value[32];
        snprintf(key, sizeof key, "large_%04d", i);
        snprintf(value, sizeof value, "Value %d", i);
        CHECK(count_key(key) == 1);
        CHECK(value_for(key) && !strcmp(value_for(key), value));
    }
    CHECK(count_key(id) == 1 && value_for(id) && !strcmp(value_for(id), "Long ID"));
    /* Both long keys share more than the former 255-byte bound. */
    CHECK(!fopen_s(&file, user_path, "wb") && file);
    if (!file) return;
    fprintf(file, "{\"%s\":\"User override\",", id);
    id[sizeof(id) - 2] = 'b';
    fprintf(file, "\"%s\":\"Distinct long ID\",\"large_1199\":\"Updated\",", id);
    /* Whitespace is legal JSON: the local file reader must not ignore a
     * complete document merely because it exceeds its former 16 MiB cap. */
    {
        char spaces[4096]; memset(spaces, ' ', sizeof spaces);
        for (int i = 0; i < 4097; i++) CHECK(fwrite(spaces, 1, sizeof spaces, file) == sizeof spaces);
    }
    fputs("\"large_document\":\"Read completely\"}", file);
    CHECK(fclose(file) == 0);
    before = g_dictionary.count;
    CHECK(count_key("hook_recovery_key") == 1);
    CHECK(rearm_strings() == 1);
    CHECK(g_dictionary.count == before + 1); /* Two new keys; the removed local key retires. */
    CHECK(count_key("hook_recovery_key") == 0);
    CHECK(value_for("large_document") && !strcmp(value_for("large_document"), "Read completely"));
    CHECK(value_for("large_1199") && !strcmp(value_for("large_1199"), "Updated"));
    CHECK(count_key(id) == 1 && value_for(id) && !strcmp(value_for(id), "Distinct long ID"));
    id[sizeof(id) - 2] = 'a';
    CHECK(count_key(id) == 1 && value_for(id) && !strcmp(value_for(id), "User override"));
    before = g_dictionary.count;
    CHECK(rearm_strings() == 1 && g_dictionary.count == before);
    for (unsigned int i = 1; i < g_dictionary.count; i++)
        CHECK(*(unsigned int *)g_records[i - 1] <= *(unsigned int *)g_records[i]);
    CHECK(write_text(user_path, "{}"));
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

static unsigned char *dictionary_row(const char *id)
{
    char key[256];
    snprintf(key, sizeof(key), "#str_%s", id);
    for (unsigned int i = 0; i < g_dictionary.count; i++)
        if (*(char **)(g_records[i] + 8) && !_stricmp(*(char **)(g_records[i] + 8), key))
            return g_records[i];
    return NULL;
}

static void retirement_policy(const char *descriptor, const char *rows)
{
    char json[2048];
    snprintf(json, sizeof(json), "{\"id\":\"retirement\",\"name\":\"Retirement\",\"strings\":{\"en\":%s}}", rows);
    CHECK(write_text(descriptor, json));
}

typedef struct string_activation_case { int fail, fault, calls, recoveries; } string_activation_case;
static int string_activation(void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    CHECK(changes);
    string_activation_case *test = context;
    if (restoring) test->recoveries++; else test->calls++;
    CHECK(!sh_package_runtime_ready());
    if (!sh_strids_rearm()) return 0;
    CHECK(count_key("stock_label") == 1);
    CHECK(value_for("stock_label") && !strcmp(value_for("stock_label"), restoring ? "Baseline" : "Candidate"));
    CHECK(value_for("rollback_keep") && !strcmp(value_for("rollback_keep"), restoring ? "Before" : "After"));
    CHECK(count_key("rollback_added") == !restoring);
    CHECK(count_key("rollback_removed") == restoring);
    if (!restoring && test->fault) RaiseException(0xe04d5354, 0, 0, NULL);
    if (!restoring && test->fail) { snprintf(error, capacity, "later activation step failed"); return 0; }
    return 1;
}

static void test_dictionary_retirement(const char *overrides, const char *user_path)
{
    unsigned char stock[32] = {0}, collision[32] = {0};
    char descriptor[MAX_PATH];
    unsigned int baseline;
    const char *retired_text;
    string_activation_case activation = {0};
    *(unsigned int *)stock = fake_hash("#str_stock_label");
    *(unsigned int *)(stock + 4) = 0x12345678; /* Preserve unrelated native record bytes too. */
    fake_idstr_ctor(stock + 8, "#str_stock_label");
    fake_idstr_ctor(stock + 16, "Vanilla");
    *(unsigned int *)(stock + 24) = *(unsigned int *)(stock + 28) = 7;
    fake_insert(&g_dictionary, stock);
    memcpy(collision, stock, sizeof(collision));
    fake_idstr_ctor(collision + 8, "#str_unrelated_collision");
    fake_idstr_ctor(collision + 16, "Unrelated");
    *(unsigned int *)(collision + 24) = *(unsigned int *)(collision + 28) = 9;
    fake_insert(&g_dictionary, collision);
    baseline = g_dictionary.count;
    CHECK(install_strings(overrides, "retirement", "inline", "{}"));
    snprintf(descriptor, sizeof(descriptor), "%s/retirement/package.json", overrides);
    retirement_policy(descriptor, "{\"stock_label\":\"Modded\",\"temporary_label\":\"Temporary\"}");
    CHECK(rearm_strings());
    CHECK(g_dictionary.count == baseline + 1 && count_key("stock_label") == 1);
    CHECK(value_for("stock_label") && !strcmp(value_for("stock_label"), "Modded"));
    CHECK(dictionary_row("unrelated_collision") && !memcmp(dictionary_row("unrelated_collision"), collision, 32));
    retired_text = value_for("temporary_label"); CHECK(retired_text);
    CHECK(write_text(user_path, "{\"stock_label\":\"Local\",\"ai_cyberdemon_name\":\"MY OWN NAME\"}"));
    retirement_policy(descriptor, "{}");
    CHECK(rearm_strings());
    CHECK(!count_key("temporary_label") && count_key("stock_label") == 1);
    CHECK(value_for("stock_label") && !strcmp(value_for("stock_label"), "Local"));
    CHECK(retired_text && !strcmp(retired_text, "Temporary")); /* Engine pool text was not freed. */
    CHECK(write_text(user_path, "{\"ai_cyberdemon_name\":\"MY OWN NAME\"}"));
    CHECK(rearm_strings());
    CHECK(g_dictionary.count == baseline);
    CHECK(dictionary_row("stock_label") && !memcmp(dictionary_row("stock_label"), stock, 32));
    for (int cycle = 0; cycle < 4; cycle++) {
        retirement_policy(descriptor, "{\"stock_label\":\"Again\",\"temporary_label\":\"Again\"}");
        CHECK(rearm_strings() && count_key("stock_label") == 1 && count_key("temporary_label") == 1);
        retirement_policy(descriptor, "{}");
        CHECK(rearm_strings() && g_dictionary.count == baseline && !count_key("temporary_label"));
        CHECK(dictionary_row("stock_label") && !memcmp(dictionary_row("stock_label"), stock, 32));
    }
    retirement_policy(descriptor, "{\"stock_label\":\"Baseline\",\"rollback_keep\":\"Before\",\"rollback_removed\":\"Old\"}");
    CHECK(rearm_strings());
    retirement_policy(descriptor, "{\"stock_label\":\"Candidate\",\"rollback_keep\":\"After\",\"rollback_added\":\"New\"}");
    for (int fault = 0; fault < 2; fault++) {
        activation = (string_activation_case){1, fault, 0, 0};
        CHECK(sh_overrides_rescan_packages_activated(string_activation, &activation) == SH_OVERRIDES_RESCAN_FAILED);
        CHECK(activation.calls == 1 && activation.recoveries == 1 && !sh_package_runtime_ready());
        CHECK(g_dictionary.count == baseline + 2);
        CHECK(!count_key("rollback_added") && count_key("rollback_removed") == 1);
    }
    activation = (string_activation_case){0};
    CHECK(sh_overrides_rescan_packages_activated(string_activation, &activation) != SH_OVERRIDES_RESCAN_FAILED);
    CHECK(activation.calls == 1 && !activation.recoveries && sh_package_runtime_ready());
    retirement_policy(descriptor, "{}");
    CHECK(rearm_strings() && g_dictionary.count == baseline);
    CHECK(!count_key("rollback_added") && !count_key("rollback_removed") && !count_key("rollback_keep"));
    CHECK(dictionary_row("stock_label") && !memcmp(dictionary_row("stock_label"), stock, 32));
    CHECK(dictionary_row("unrelated_collision") && !memcmp(dictionary_row("unrelated_collision"), collision, 32));
    for (unsigned int i = 1; i < g_dictionary.count; i++)
        CHECK(*(unsigned int *)g_records[i - 1] <= *(unsigned int *)g_records[i]);
}

static void test_native_table_extent(void)
{
    const unsigned int original = 2000001u, capacity = original + CAP;
    unsigned char (*large)[32] = calloc(capacity, 32);
    CHECK(large != NULL);
    if (!large) return;
    reset_doubles();
    g_records = large; g_dictionary.rows = large;
    g_dictionary.count = original; g_dictionary.capacity = capacity;
    /* The empty records stand in for unrelated native rows. Native helpers
     * receive a real allocation covering the complete descriptor capacity. */
    CHECK(sh_strids_test_inject(&g_dictionary, fake_insert, fake_hash, fake_idstr_ctor) > 1200);
    CHECK(g_dictionary.count == original + (unsigned int)g_appended);
    fake_sort(&g_dictionary, large, g_dictionary.count, 0x20);
    for (unsigned int i = 1; i < g_dictionary.count; i++)
        CHECK(*(unsigned int *)large[i - 1] <= *(unsigned int *)large[i]);
    /* Malformed and exhausted signed native counts still refuse safely,
     * before attempting any native append or walking outside the buffer. */
    {
        static const unsigned int extents[][2] = {
            {5u, 4u}, {0x80000000u, 0x80000000u}, {0x7fffffffu, 0x7fffffffu}
        };
        for (size_t i = 0; i < sizeof extents / sizeof extents[0]; i++) {
            int appended = g_appended;
            g_dictionary.count = extents[i][0]; g_dictionary.capacity = extents[i][1];
            CHECK(sh_strids_test_inject(&g_dictionary, fake_insert, fake_hash, fake_idstr_ctor) == 0);
            CHECK(g_appended == appended);
        }
    }
    g_records = g_static_records; g_dictionary.rows = g_static_records;
    g_dictionary.count = 0; g_dictionary.capacity = CAP;
    free(large);
}

static void invoke_sort_hook(void)
{
    int before = g_sorts;
    CHECK(g_hook_detour != NULL);
    g_hook_detour(&g_dictionary, g_records, g_dictionary.count, 0x20);
    CHECK(g_sorts == before + 1);
}

static int initial_string_activation(void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    CHECK(changes);
    int *calls = context;
    (*calls)++;
    CHECK(sh_strids_rearm());
    CHECK(count_key("boot_package_label") == !restoring);
    CHECK(value_for("sh_bv_navigation_desc") &&
        (!strcmp(value_for("sh_bv_navigation_desc"), "Package navigation")) == !restoring);
    if (!restoring) { snprintf(error, capacity, "first activation failed after strings"); return 0; }
    return 1;
}

static void test_initial_binding(const char *root, const char *overrides)
{
    static unsigned char table_lea[64];
    intptr_t distance = (unsigned char *)&g_dictionary - (table_lea + 7);
    int32_t displacement = (int32_t)distance;
    CHECK((intptr_t)displacement == distance);
    memcpy(table_lea, "\x48\x8d\x0d", 3);
    memcpy(table_lea + 3, &displacement, 4);
    reset_doubles();
    g_hook_mode = 1; g_hook_commit_result = B2_PATCH_OK;
    CHECK(install_strings(overrides, "boot-strings", "inline",
        "{\"boot_package_label\":\"Package ready\",\"sh_bv_navigation_desc\":\"Package navigation\"}"));
    CHECK(!sh_package_runtime_ready());
    CHECK(sh_strids_install(fake_sort, 1, table_lea, fake_insert, fake_hash, fake_idstr_ctor));
    CHECK(!sh_package_runtime_ready() && g_dictionary.count > 0);
    CHECK(count_key("sh_bv_navigation_desc") == 1 && count_key("boot_package_label") == 0);
    CHECK(value_for("sh_bv_navigation_desc") && strcmp(value_for("sh_bv_navigation_desc"), "Package navigation"));
    invoke_sort_hook();
    CHECK(count_key("sh_bv_navigation_desc") == 1 && count_key("boot_package_label") == 0);
    CHECK(sh_strids_rearm()); /* Local/baked refresh does not imply package readiness. */
    CHECK(!sh_package_runtime_ready() && count_key("boot_package_label") == 0);
    {
        const sh_package_compilation *current;
        int calls = 0;
        char error[2048];
        CHECK(!sh_package_runtime_refresh_activated(root, (sh_package_activation_guard){0}, initial_string_activation, &calls));
        CHECK(calls == 2 && !sh_package_runtime_ready());
        current = sh_package_runtime_acquire(); CHECK(!current); sh_package_runtime_release();
        CHECK(!count_key("boot_package_label"));
        sh_package_runtime_error(error, sizeof(error)); CHECK(!strstr(error, "recovery failed"));
    }
    CHECK(sh_package_runtime_refresh(root));
    CHECK(sh_strids_rearm());
    CHECK(count_key("boot_package_label") == 1 && count_key("sh_bv_navigation_desc") == 1);
    CHECK(value_for("sh_bv_navigation_desc") && !strcmp(value_for("sh_bv_navigation_desc"), "Package navigation"));
    CHECK(hook_unpatch(g_hook_owned));
    sh_strids_test_set_sort(NULL);
    g_hook_mode = g_hook_prepares = g_hook_commits = g_hook_unpatches = 0;
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
    CHECK(rearm_strings() == 0);

    g_hook_prepare_fail = 0;
    g_hook_commit_result = B2_PATCH_FAIL_SEH;
    CHECK(sh_strids_install(fake_sort, 1, table_lea, fake_insert,
                           fake_hash, fake_idstr_ctor) == 0);
    CHECK(g_hook_prepares == 2 && g_hook_commits == 1 && g_hook_unpatches == 1);
    CHECK(g_hook_owned == NULL && rearm_strings() == 0);

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
    CHECK(rearm_strings() == 0);
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
    CHECK(rearm_strings() == 1);
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
    sh_package_runtime_test_empty_catalog();
    sh_overrides_set_root(root);
    /* Set every source path inside the fixture to avoid reading the user's
     * real strings document through the default-path fallback. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\user_strids.json", root);
    sh_strids_set_source(path);
    test_initial_binding(root, overrides);

    /* A package must supply strings without edits to the global user document. */
    CHECK(install_strings(overrides, "cyberdemon", "cyberdemon.json",
                          "{ \"ai_cyberdemon_name\" : \"Cyberdemon\","
                          "  \"cyber_desc\" : \"A towering cybernetic demon.\" }"));
    run_inject();
    CHECK(value_for("ai_cyberdemon_name") != NULL);
    CHECK(value_for("ai_cyberdemon_name") && strcmp(value_for("ai_cyberdemon_name"), "Cyberdemon") == 0);
    CHECK(value_for("cyber_desc") != NULL);

    /* Compiled package rows remain distinguishable from user overrides. */
    {
        int i, found = 0;
        const char *id = NULL, *owner = NULL;
        for (i = 0; sh_strids_test_row(i, &id, &owner); i++)
            if (id && _stricmp(id, "ai_cyberdemon_name") == 0) {
                found = 1;
                CHECK(owner && strcmp(owner, "<packages>") == 0);
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

    /* Conflicting packages are excluded together; unrelated package strings
     * and product defaults still compile and inject. */
    CHECK(install_strings(overrides, "zz-second", "shared.json",
                          "{ \"shared_key\" : \"A DIFFERENT VALUE\" }"));
    run_inject();
    CHECK(count_key("shared_key") == 0);
    CHECK(sh_package_runtime_ready());
    CHECK(strstr(g_log, "shared_key") != NULL);
    CHECK(strstr(g_log, "zz-second") != NULL);
    CHECK(strstr(g_log, "cyberdemon") != NULL);
    CHECK(install_strings(overrides, "zz-second", "shared.json",
                          "{ \"shared_key\" : \"Shared Text\" }"));
    run_inject();
    CHECK(count_key("shared_key") == 1);

    /* The explicit user document takes precedence over package strings. */
    CHECK(write_text(path, "{ \"ai_cyberdemon_name\" : \"MY OWN NAME\" }"));
    run_inject();
    CHECK(count_key("ai_cyberdemon_name") == 1);
    CHECK(value_for("ai_cyberdemon_name") &&
          strcmp(value_for("ai_cyberdemon_name"), "MY OWN NAME") == 0);
    /* Both string sources decode JSON escapes before native interning. */
    CHECK(install_strings(overrides, "cyberdemon", "unicode.json",
                          "{\"package_escaped\":\"Pilot\\u0027s caf\\u00e9 \\ud834\\udd1e\"}"));
    CHECK(write_text(path, "\xef\xbb\xbf"
                     "{\"ai_cyberdemon_name\":\"MY OWN NAME\","
                     "\"escaped\\u005fkey\":\"Pilot\\u0027s caf\\u00e9 \\ud834\\udd1e\","
                     "\"controls\":\"line\\r\\n\\t\\b\\f\\/\\\\\\\"\"}"));
    run_inject();
    CHECK(value_for("escaped_key") &&
          !strcmp(value_for("escaped_key"), "Pilot's caf\xc3\xa9 \xf0\x9d\x84\x9e"));
    CHECK(value_for("package_escaped") &&
          !strcmp(value_for("package_escaped"), "Pilot's caf\xc3\xa9 \xf0\x9d\x84\x9e"));
    CHECK(value_for("controls") && !strcmp(value_for("controls"), "line\r\n\t\b\f/\\\""));
    {
        static const char *invalid[] = {
            "{\"partial_user_key\":\"never\",\"bad\":\"\\ud800\"}",
            "{\"partial_user_key\":\"never\",\"bad\":\"\\u0000\"}",
            "{\"partial_user_key\":\"never\",\"bad\":\"\\q\"}",
            "{\"partial_user_key\":\"never\",\"bad\":{\"nested\":\"not a row\"}}",
            "{\"partial_user_key\":\"never\",\"partial_user_key\":\"duplicate\"}"
        };
        size_t i;
        for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            CHECK(write_text(path, invalid[i]));
            run_inject();
            CHECK(count_key("partial_user_key") == 0);
            CHECK(strstr(g_log, "strids REFUSED") != NULL);
        }
    }
    CHECK(write_text(path, "{\"ai_cyberdemon_name\":\"MY OWN NAME\"}"));
    run_inject();
    /* Rearm appends newly installed strings and updates owned keys after
     * native sorting has moved their rows. Repeated refresh never duplicates. */
    sh_strids_test_set_sort((void *)fake_sort);
    CHECK(install_strings(overrides, "new-runtime", "runtime.json",
                          "{ \"runtime_key\" : \"First value\" }"));
    CHECK(rearm_strings() == 1);
    CHECK(count_key("runtime_key") == 1);
    CHECK(strcmp(value_for("runtime_key"), "First value") == 0);
    CHECK(install_strings(overrides, "new-runtime", "runtime.json",
                          "{ \"runtime_key\" : \"Updated value\" }"));
    CHECK(rearm_strings() == 1);
    CHECK(count_key("runtime_key") == 1);
    CHECK(strcmp(value_for("runtime_key"), "Updated value") == 0);
    CHECK(rearm_strings() == 1);
    CHECK(count_key("runtime_key") == 1 && count_key("shared_key") == 1);
    CHECK(g_sorts == 3);
    for (unsigned int i = 1; i < g_dictionary.count; i++)
        CHECK(*(unsigned int *)g_records[i-1] <= *(unsigned int *)g_records[i]);
    CHECK(strcmp(value_for("ai_cyberdemon_name"), "MY OWN NAME") == 0);
    g_sort_fault = 1;
    CHECK(rearm_strings() == 0);
    g_sort_fault = 0;
    CHECK(rearm_strings() == 1);
    CHECK(install_strings(overrides, "new-runtime", "refused.json",
                          "{ \"append_refusal\" : \"try again\" }"));
    g_insert_refused = 1;
    CHECK(rearm_strings() == 0);
    CHECK(count_key("append_refusal") == 0);
    g_insert_refused = 0;
    CHECK(rearm_strings() == 1);
    CHECK(count_key("append_refusal") == 1);
    test_dictionary_retirement(overrides, path);
    test_sort_hook_recovery(path);
    /* Reject every conflicting pair across a high-index inventory, while the
     * remaining strings still refresh successfully. */
    for (int i = 0; i < 65; i++) {
        char package[32];
        _snprintf_s(package, sizeof(package), _TRUNCATE, "overflow-%02d", i);
        CHECK(install_strings(overrides, package, "value.json",
                              "{ \"partial_key\" : \"must not appear\" }"));
    }
    CHECK(install_strings(overrides, "overflow-64", "value.json",
                          "{ \"partial_key\" : \"conflicting value\" }"));
    CHECK(rearm_strings() == 1);
    CHECK(count_key("partial_key") == 0);
    CHECK(install_strings(overrides, "overflow-64", "value.json",
                          "{ \"partial_key\" : \"must not appear\" }"));
    CHECK(rearm_strings() == 1 && count_key("partial_key") == 1);
    test_large_strings(overrides, path);
    /* A malformed suffix after NUL is still part of the input document. */
    {
        static const char document[] = "{\"nul_prefix_key\":\"never\"}\0trailing invalid bytes";
        FILE *file = NULL;
        CHECK(!fopen_s(&file, path, "wb") && file);
        if (file) {
            CHECK(fwrite(document, 1, sizeof(document) - 1, file) == sizeof(document) - 1);
            CHECK(fclose(file) == 0);
        }
        CHECK(!rearm_strings() && !count_key("nul_prefix_key"));
        CHECK(write_text(path, "{}"));
        HANDLE locked = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        CHECK(locked != INVALID_HANDLE_VALUE);
        if (locked != INVALID_HANDLE_VALUE) {
            CHECK(!rearm_strings());
            CloseHandle(locked);
        }
        CHECK(rearm_strings());
    }
    test_native_table_extent();
    g_hook_installed = 0; /* simulate a removal that still owns its record */
    CHECK(sh_strids_install(NULL, 0, NULL, NULL, NULL, NULL) == 0);
    CHECK(g_hook_owned == NULL && rearm_strings() == 0);
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

/* AAS validation is exercised by grid_room_nav_test, not these lookup tests. */
int sh_navmesh_validate_aas(const unsigned char *p,size_t n,char *e,size_t cap)
{(void)p;(void)n;if(e&&cap)e[0]=0;return 0;}
