/* Tests #str_ injection from the user's strids.json and the baked defaults, with
 * user-over-default precedence, rearm, retirement and native-table refusal.
 * Engine string-pool and list operations are doubled to inspect appended
 * key/value pairs. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#include "../src/backend/strids.h"
#include "../src/backend/hook.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_failed++; } } while (0)

/* Stub collaborators and capture logs. */
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

/* Remove old PID-named fixtures so a reused process ID cannot inherit them. */
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

static void run_inject(void)
{
    reset_doubles();
    g_log[0] = '\0';
    sh_strids_test_inject(&g_dictionary, (void *)fake_insert, (void *)fake_hash, (void *)fake_idstr_ctor);
}

static int rearm_strings(void)
{
    return sh_strids_rearm();
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

/* A key the user document stops supplying is retired: a replaced vanilla row
 * gets its original record back and an introduced row disappears, while engine
 * pool text is never freed. */
static void test_dictionary_retirement(const char *user_path)
{
    unsigned char stock[32] = {0}, collision[32] = {0};
    unsigned int baseline;
    const char *retired_text;
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
    CHECK(write_text(user_path, "{\"stock_label\":\"Modded\",\"temporary_label\":\"Temporary\"}"));
    CHECK(rearm_strings());
    CHECK(g_dictionary.count == baseline + 1 && count_key("stock_label") == 1);
    CHECK(value_for("stock_label") && !strcmp(value_for("stock_label"), "Modded"));
    CHECK(dictionary_row("unrelated_collision") && !memcmp(dictionary_row("unrelated_collision"), collision, 32));
    retired_text = value_for("temporary_label"); CHECK(retired_text);
    CHECK(write_text(user_path, "{\"stock_label\":\"Local\"}"));
    CHECK(rearm_strings());
    CHECK(!count_key("temporary_label") && count_key("stock_label") == 1);
    CHECK(value_for("stock_label") && !strcmp(value_for("stock_label"), "Local"));
    CHECK(retired_text && !strcmp(retired_text, "Temporary")); /* Engine pool text was not freed. */
    CHECK(write_text(user_path, "{}"));
    CHECK(rearm_strings());
    CHECK(g_dictionary.count == baseline);
    CHECK(dictionary_row("stock_label") && !memcmp(dictionary_row("stock_label"), stock, 32));
    for (int cycle = 0; cycle < 4; cycle++) {
        CHECK(write_text(user_path, "{\"stock_label\":\"Again\",\"temporary_label\":\"Again\"}"));
        CHECK(rearm_strings() && count_key("stock_label") == 1 && count_key("temporary_label") == 1);
        CHECK(write_text(user_path, "{}"));
        CHECK(rearm_strings() && g_dictionary.count == baseline && !count_key("temporary_label"));
        CHECK(dictionary_row("stock_label") && !memcmp(dictionary_row("stock_label"), stock, 32));
    }
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
    CHECK(sh_strids_test_inject(&g_dictionary, fake_insert, fake_hash, fake_idstr_ctor) > 0);
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


int main(void)
{
    char temp[MAX_PATH], root[MAX_PATH], path[MAX_PATH];
    DWORD pid = GetCurrentProcessId();

    GetTempPathA(sizeof temp, temp);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%ssnapmap-plus-strids-%lu", temp, (unsigned long)pid);
    remove_tree(root);
    CHECK(make_dir(root));
    /* Point the user document inside the fixture so the real one is never read. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\user_strids.json", root);
    sh_strids_set_source(path);

    /* Without a user document the baked defaults still apply. */
    run_inject();
    CHECK(g_appended > 0);
    CHECK(strstr(g_log, "no user strids.json") != NULL);
    {
        const char *id = NULL, *owner = NULL;
        CHECK(sh_strids_test_row(0, &id, &owner) && owner && !strcmp(owner, "<user>"));
    }

    /* The user document supplies new keys and outranks a baked default. */
    CHECK(write_text(path, "{ \"ai_cyberdemon_name\" : \"Cyberdemon\","
                           "  \"sh_bv_navigation_desc\" : \"MY OWN TEXT\" }"));
    run_inject();
    CHECK(value_for("ai_cyberdemon_name") && !strcmp(value_for("ai_cyberdemon_name"), "Cyberdemon"));
    CHECK(count_key("sh_bv_navigation_desc") == 1);
    CHECK(value_for("sh_bv_navigation_desc") && !strcmp(value_for("sh_bv_navigation_desc"), "MY OWN TEXT"));

    /* JSON escapes are decoded before native interning. */
    CHECK(write_text(path, "\xef\xbb\xbf"
                     "{\"ai_cyberdemon_name\":\"MY OWN NAME\","
                     "\"escaped\\u005fkey\":\"Pilot\\u0027s caf\\u00e9 \\ud834\\udd1e\","
                     "\"controls\":\"line\\r\\n\\t\\b\\f\\/\\\\\\\"\"}"));
    run_inject();
    CHECK(value_for("escaped_key") &&
          !strcmp(value_for("escaped_key"), "Pilot's caf\xc3\xa9 \xf0\x9d\x84\x9e"));
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

    /* Rearm appends new keys and updates owned keys after native sorting moved
     * their rows. Repeated refresh never duplicates. */
    CHECK(write_text(path, "{\"ai_cyberdemon_name\":\"MY OWN NAME\"}"));
    run_inject();
    sh_strids_test_set_sort((void *)fake_sort);
    CHECK(write_text(path, "{\"ai_cyberdemon_name\":\"MY OWN NAME\",\"runtime_key\":\"First value\"}"));
    CHECK(rearm_strings() == 1);
    CHECK(count_key("runtime_key") == 1 && !strcmp(value_for("runtime_key"), "First value"));
    CHECK(write_text(path, "{\"ai_cyberdemon_name\":\"MY OWN NAME\",\"runtime_key\":\"Updated value\"}"));
    CHECK(rearm_strings() == 1);
    CHECK(count_key("runtime_key") == 1 && !strcmp(value_for("runtime_key"), "Updated value"));
    CHECK(rearm_strings() == 1);
    CHECK(count_key("runtime_key") == 1);
    CHECK(g_sorts == 3);
    for (unsigned int i = 1; i < g_dictionary.count; i++)
        CHECK(*(unsigned int *)g_records[i-1] <= *(unsigned int *)g_records[i]);
    g_sort_fault = 1;
    CHECK(rearm_strings() == 0);
    g_sort_fault = 0;
    CHECK(rearm_strings() == 1);
    CHECK(write_text(path, "{\"runtime_key\":\"Updated value\",\"append_refusal\":\"try again\"}"));
    g_insert_refused = 1;
    CHECK(rearm_strings() == 0);
    CHECK(count_key("append_refusal") == 0);
    g_insert_refused = 0;
    CHECK(rearm_strings() == 1);
    CHECK(count_key("append_refusal") == 1);
    CHECK(write_text(path, "{}"));
    CHECK(rearm_strings() == 1);
    test_dictionary_retirement(path);
    test_sort_hook_recovery(path);
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

    remove_tree(root);
    reset_doubles();

    if (g_failed) {
        fprintf(stderr, "strids_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    printf("strids_test: ok\n");
    return 0;
}
