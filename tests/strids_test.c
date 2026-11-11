/* Tests #str_ injection from the admitted bundles' strings and the baked
 * defaults, with bundle-over-default precedence, cross-bundle conflicts,
 * rearm, retirement and native-table refusal. The user document of earlier
 * releases (strings\strids.json) is not read: the store tool moves it into a
 * mod, which user_doc stands in for. Engine string-pool and list operations and the serving index's
 * bundle accessor are doubled to inspect appended key/value pairs. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#include "../src/backend/strids.h"
#include "../src/backend/process_heap_scope.h"
#include "../src/backend/hook.h"
#include "../src/backend/modserve.h"

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

/* ------------------------------------------------------------------ serving index double */

/* The fixture lists bundles in the accessor's order (installed before
 * map-delivered, then id and version); like the real accessor, the double
 * visits only admitted bundles that carry an extra object. */
typedef struct fake_bundle {
    const char *id, *version;
    int place, admitted;
    const char *extra;
    int edits_off;      /* its edits do not apply now (another map, not declared) */
    int names_off;      /* its own names do not serve now (inert: the map declares another build) */
} fake_bundle;
static fake_bundle g_bundles[640];
static int g_nbundles, g_bundle_visits, g_bundle_refuse;

/* The double names each bundle's build by its id. */
int sh_modserve_bundle_edits_live(const char *bundle_hash)
{
    int i;
    for (i = 0; i < g_nbundles; i++)
        if (bundle_hash && !strcmp(g_bundles[i].id, bundle_hash)) return !g_bundles[i].edits_off;
    return 0;
}

static int g_scope_on;
int sh_modserve_scope_enabled(void) { return g_scope_on; }
int sh_modserve_bundle_scope(const char *bundle_hash, sh_modserve_scope_info *out)
{
    int i;
    memset(out, 0, sizeof *out);
    for (i = 0; i < g_nbundles; i++)
        if (bundle_hash && !strcmp(g_bundles[i].id, bundle_hash)) { out->names_live = !g_bundles[i].names_off; return 1; }
    return 0;
}

int sh_modserve_each_bundle_extra(sh_modserve_extra_fn fn, void *context)
{
    int i, n = 0;
    g_bundle_visits++;
    if (g_bundle_refuse) return -1;
    for (i = 0; i < g_nbundles; i++) {
        if (!g_bundles[i].admitted || !g_bundles[i].extra) continue;
        fn(context, g_bundles[i].id, g_bundles[i].id, g_bundles[i].place, g_bundles[i].extra);
        n++;
    }
    return n;
}

static void set_bundles(const fake_bundle *list, int n)
{
    int i;
    g_nbundles = n;
    for (i = 0; i < n; i++) g_bundles[i] = list[i];
}

/* One installed mod whose strings.en is `pairs`, where earlier releases read
 * the user's strids.json (the store tool moves that document into a mod). */
static char g_user_extra[4096];
static int user_doc(const char *pairs)
{
    fake_bundle user = { "user.mod", "1.0.0", 0, 1, g_user_extra };
    _snprintf_s(g_user_extra, sizeof g_user_extra, _TRUNCATE, "{\"strings\":{\"en\":%s}}", pairs);
    set_bundles(&user, 1);
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

/* The engine's heap selection (process_heap_scope.h): a stack of heap ids at
 * +0x44 and its depth at +0xC4. The map's heap (7) is selected unless a
 * scope pushed the process heap (0); g_push_ignored plays a push from the
 * wrong thread, which the engine ignores. */
static unsigned char g_heap_self[0x100];
static int g_push_ignored;
static int g_insert_heap = -2;   /* the heap selected at the last append */
static int heap_depth(void) { return *(int *)(g_heap_self + 0xC4); }
static int heap_top(void) { return heap_depth() ? *(int *)(g_heap_self + 0x44 + (heap_depth() - 1) * 4) : 7; }
static void *fake_heap_get(void) { return g_heap_self; }
static void fake_heap_push(void *self, int heap)
{
    int d = *(int *)((unsigned char *)self + 0xC4);
    if (g_push_ignored) return;
    *(int *)((unsigned char *)self + 0x44 + d * 4) = heap;
    *(int *)((unsigned char *)self + 0xC4) = d + 1;
}
static void fake_heap_pop(void *self) { *(int *)((unsigned char *)self + 0xC4) -= 1; }
static const sh_process_heap_api g_heap_api = { fake_heap_get, fake_heap_push, fake_heap_pop };

static int fake_insert(void *table_desc, void *record32)
{
    (void)table_desc;
    g_insert_heap = heap_top();
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

/* A key the mod stops supplying is retired: a replaced vanilla row
 * gets its original record back and an introduced row disappears, while engine
 * pool text is never freed. */
static void test_dictionary_retirement(void)
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
    CHECK(user_doc("{\"stock_label\":\"Modded\",\"temporary_label\":\"Temporary\"}"));
    CHECK(rearm_strings());
    CHECK(g_dictionary.count == baseline + 1 && count_key("stock_label") == 1);
    CHECK(value_for("stock_label") && !strcmp(value_for("stock_label"), "Modded"));
    CHECK(dictionary_row("unrelated_collision") && !memcmp(dictionary_row("unrelated_collision"), collision, 32));
    retired_text = value_for("temporary_label"); CHECK(retired_text);
    CHECK(user_doc("{\"stock_label\":\"Local\"}"));
    CHECK(rearm_strings());
    CHECK(!count_key("temporary_label") && count_key("stock_label") == 1);
    CHECK(value_for("stock_label") && !strcmp(value_for("stock_label"), "Local"));
    CHECK(retired_text && !strcmp(retired_text, "Temporary")); /* Engine pool text was not freed. */
    CHECK(user_doc("{}"));
    CHECK(rearm_strings());
    CHECK(g_dictionary.count == baseline);
    CHECK(dictionary_row("stock_label") && !memcmp(dictionary_row("stock_label"), stock, 32));
    for (int cycle = 0; cycle < 4; cycle++) {
        CHECK(user_doc("{\"stock_label\":\"Again\",\"temporary_label\":\"Again\"}"));
        CHECK(rearm_strings() && count_key("stock_label") == 1 && count_key("temporary_label") == 1);
        CHECK(user_doc("{}"));
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

static int log_count(const char *needle)
{
    int n = 0;
    const char *at = g_log;
    while ((at = strstr(at, needle)) != NULL) { n++; at += strlen(needle); }
    return n;
}

static const char *row_owner(const char *id)
{
    const char *row_id = NULL, *owner = NULL;
    int i;
    for (i = 0; sh_strids_test_row(i, &row_id, &owner); i++)
        if (row_id && !_stricmp(row_id, id)) return owner;
    return NULL;
}

/* The bundle layer sits over the baked defaults;
 * two bundles disagreeing log once and the first in the accessor's order
 * stands; a rearm after an index rebuild adds the new bundles' keys and
 * retires the keys of one that left. */
static void test_bundle_layer(void)
{
    static const fake_bundle first[] = {
        { "alpha.mod", "1.0.0", 0, 1,
          "{\"hud\":{\"x\":1},\"strings\":{\"en\":{\"shared_user_key\":\"Alpha text\","
          "\"alpha_name\":\"Alpha\",\"clash_key\":\"From alpha\",\"same_key\":\"Same\","
          "\"sh_bv_navigation_desc\":\"Alpha nav\"},\"fr\":{\"fr_only\":\"Francais\"}}}" },
        { "beta.mod", "1.0.0", 0, 1,
          "{\"strings\":{\"en\":{\"CLASH_KEY\":\"From beta\",\"same_key\":\"Same\",\"beta_name\":\"Beta\"}}}" },
        { "gamma.off", "1.0.0", 0, 0, "{\"strings\":{\"en\":{\"gamma_name\":\"Gamma\"}}}" },
        { "no.strings", "1.0.0", 0, 1, "{\"hud\":{}}" },
        { "old.map", "0123456789abcdef", 1, 1,
          "{\"legacy\":{},\"strings\":{\"en\":{\"clash_key\":\"From legacy\",\"legacy_name\":\"Legacy\"}}}" },
    };
    static const fake_bundle second[] = {
        { "alpha.mod", "1.0.0", 0, 1,
          "{\"strings\":{\"en\":{\"alpha_name\":\"Alpha\",\"clash_key\":\"From alpha\"}}}" },
        { "delta.mod", "2.0.0", 0, 1, "{\"strings\":{\"en\":{\"delta_name\":\"Delta\"}}}" },
        { "eps.bad", "1.0.0", 0, 1, "{\"strings\":{\"en\":{\"eps_name\":\"never\",\"bad\":{\"nested\":1}}}}" },
        { "old.map", "0123456789abcdef", 1, 1,
          "{\"strings\":{\"en\":{\"clash_key\":\"From legacy\",\"legacy_name\":\"Legacy\"}}}" },
    };
    const char *baked_nav;
    unsigned int before;

    CHECK(user_doc("{}"));
    set_bundles(NULL, 0);
    CHECK(rearm_strings() == 1);
    baked_nav = value_for("sh_bv_navigation_desc");
    CHECK(baked_nav != NULL && strcmp(baked_nav, "Alpha nav") != 0);
    before = g_dictionary.count;

    set_bundles(first, (int)(sizeof first / sizeof first[0]));
    g_log[0] = '\0';
    CHECK(rearm_strings() == 1);
    /* Bundle over baked. */
    CHECK(value_for("shared_user_key") && !strcmp(value_for("shared_user_key"), "Alpha text"));
    CHECK(value_for("sh_bv_navigation_desc") && !strcmp(value_for("sh_bv_navigation_desc"), "Alpha nav"));
    CHECK(count_key("sh_bv_navigation_desc") == 1);
    CHECK(value_for("alpha_name") && !strcmp(value_for("alpha_name"), "Alpha"));
    CHECK(value_for("beta_name") && !strcmp(value_for("beta_name"), "Beta"));
    CHECK(value_for("legacy_name") && !strcmp(value_for("legacy_name"), "Legacy"));
    CHECK(row_owner("alpha_name") && !strcmp(row_owner("alpha_name"), "alpha.mod"));
    CHECK(row_owner("legacy_name") && !strcmp(row_owner("legacy_name"), "old.map"));
    CHECK(row_owner("shared_user_key") && !strcmp(row_owner("shared_user_key"), "alpha.mod"));
    /* First writer wins across bundles; one line per disagreeing bundle. */
    CHECK(count_key("clash_key") == 1 && !strcmp(value_for("clash_key"), "From alpha"));
    CHECK(log_count("STRINGS: REFUSED '#str_CLASH_KEY' from bundle beta.mod -- bundle alpha.mod "
                    "already defines it with different text; the first definition stands") == 1);
    CHECK(log_count("STRINGS: REFUSED '#str_clash_key' from bundle old.map -- bundle "
                    "alpha.mod already defines it") == 1);
    CHECK(log_count("REFUSED '#str_") == 2);   /* equal text and baked precedence are not conflicts */
    CHECK(count_key("same_key") == 1);
    /* Only en; only admitted bundles. */
    CHECK(count_key("fr_only") == 0 && count_key("gamma_name") == 0);
    CHECK(strstr(g_log, "STRINGS: runtime refresh complete: ") != NULL);
    CHECK(strstr(g_log, " #str_ entries (bundles 7, baked ") != NULL);
    CHECK(strstr(g_log, "supplied") == NULL);   /* quiet without the debug flag */
    CHECK(g_dictionary.count == before + 6);    /* six new bundle keys */

    /* The per-bundle line, once per bundle per pass, only with the flag. */
    SetEnvironmentVariableA("SNAPMAP_PLUS_STRIDS_DEBUG", "1");
    g_log[0] = '\0';
    CHECK(rearm_strings() == 1);
    CHECK(log_count("STRINGS: bundle alpha.mod supplied 5 string(s)") == 1);
    CHECK(log_count("STRINGS: bundle beta.mod supplied 1 string(s)") == 1);
    CHECK(log_count("STRINGS: bundle old.map supplied 1 string(s)") == 1);
    CHECK(log_count("supplied") == 3);
    SetEnvironmentVariableA("SNAPMAP_PLUS_STRIDS_DEBUG", NULL);

    /* An index rebuild: beta left, delta arrived, eps carries a malformed
     * strings object and is refused alone. No duplicate rows appear. */
    set_bundles(second, (int)(sizeof second / sizeof second[0]));
    g_log[0] = '\0';
    CHECK(rearm_strings() == 1);
    CHECK(count_key("beta_name") == 0 && count_key("same_key") == 0);
    CHECK(value_for("delta_name") && !strcmp(value_for("delta_name"), "Delta"));
    CHECK(count_key("eps_name") == 0);
    CHECK(strstr(g_log, "STRINGS: REFUSED the strings of bundle eps.bad") != NULL);
    CHECK(count_key("clash_key") == 1 && !strcmp(value_for("clash_key"), "From alpha"));
    CHECK(log_count("REFUSED '#str_") == 1);
    CHECK(value_for("sh_bv_navigation_desc") && !strcmp(value_for("sh_bv_navigation_desc"), baked_nav));
    CHECK(count_key("alpha_name") == 1 && count_key("legacy_name") == 1);
    for (unsigned int i = 1; i < g_dictionary.count; i++)
        CHECK(*(unsigned int *)g_records[i - 1] <= *(unsigned int *)g_records[i]);

    /* Before the first injection a refresh request waits for it. */
    sh_strids_request_refresh();
    CHECK(sh_strids_refresh_pending() == -1);

    /* An unreadable bundle layer fails the pass and retires nothing. */
    g_bundle_refuse = 1;
    CHECK(rearm_strings() == 0);
    CHECK(count_key("delta_name") == 1);
    g_bundle_refuse = 0;

    /* Every bundle gone: their keys retire, the baked default returns. */
    set_bundles(NULL, 0);
    CHECK(user_doc("{}"));
    CHECK(rearm_strings() == 1);
    CHECK(count_key("alpha_name") == 0 && count_key("delta_name") == 0 && count_key("legacy_name") == 0);
    CHECK(count_key("clash_key") == 0 && count_key("shared_user_key") == 0);
    CHECK(value_for("sh_bv_navigation_desc") && !strcmp(value_for("sh_bv_navigation_desc"), baked_nav));
    CHECK(g_dictionary.count == before);
}

/* Keys under each mod's salt (DECISIONS 5: <salt>_<name>, no '/'): three
 * packs each carry <salt>_ci_tab with the same text. Identical text under
 * different keys is two rows and no conflict; one key with two texts is
 * still one. */
/* DECISIONS 9: a string edit (a key the game or Snapmap+ defines) applies
 * where its mod's edits apply; the mod's own keys always do. */
static void test_string_edits(void)
{
    static fake_bundle packs[] = {
        { "edmod000", "", 0, 1, "{\"strings\":{\"en\":{\"sh_bv_navigation_desc\":\"EDITED\",\"edmod000_own\":\"Own\"}}}", 1 },
    };
    set_bundles(packs, 1);
    CHECK(rearm_strings() == 1);
    CHECK(value_for("edmod000_own") && !strcmp(value_for("edmod000_own"), "Own"));
    CHECK(value_for("sh_bv_navigation_desc") && strcmp(value_for("sh_bv_navigation_desc"), "EDITED") != 0);
    CHECK(sh_strids_scope_check() == 0);
    g_bundles[0].edits_off = 0;   /* a map that declares it opens */
    CHECK(sh_strids_scope_check() == 1);
    CHECK(rearm_strings() == 1);
    CHECK(value_for("sh_bv_navigation_desc") && !strcmp(value_for("sh_bv_navigation_desc"), "EDITED"));
    CHECK(sh_strids_scope_check() == 0);
    set_bundles(NULL, 0);
    CHECK(rearm_strings() == 1);
}

/* The scope check covers every bundle with strings, however many: with 600
 * mods, a change to the last one's scope still asks for a refresh. */
static void test_scope_check_many_bundles(void)
{
    static fake_bundle many[600];
    static char ids[600][16], extras[600][96];
    int i;
    for (i = 0; i < 600; i++) {
        _snprintf_s(ids[i], sizeof ids[i], _TRUNCATE, "mny%05d", i);
        _snprintf_s(extras[i], sizeof extras[i], _TRUNCATE,
                    "{\"strings\":{\"en\":{\"sh_bv_navigation_desc\":\"E%d\",\"%s_own\":\"Own\"}}}", i, ids[i]);
        many[i].id = ids[i];
        many[i].version = "";
        many[i].place = 0;
        many[i].admitted = 1;
        many[i].extra = extras[i];
        many[i].edits_off = 1;
        many[i].names_off = 0;
    }
    set_bundles(many, 600);
    CHECK(rearm_strings() == 1);
    CHECK(value_for("mny00599_own") && !strcmp(value_for("mny00599_own"), "Own"));
    CHECK(sh_strids_scope_check() == 0);
    g_bundles[599].edits_off = 0;   /* a map that declares the 600th mod opens */
    CHECK(sh_strids_scope_check() == 1);
    CHECK(rearm_strings() == 1);
    CHECK(sh_strids_scope_check() == 0);
    set_bundles(NULL, 0);
    CHECK(rearm_strings() == 1);
}

/* R6: a mod's own strings come from the build that serves the open map. The
 * root's build of salt mmmmmmmm and a downloaded map's build of it both
 * carry mmmmmmmm_title; the map declares its own build, so the root's is
 * inert and the map's text shows; with the map closed the root's returns. */
static void test_own_strings_follow_the_build(void)
{
    static fake_bundle builds[] = {
        { "mmmmmmmm", "", 0, 1, "{\"strings\":{\"en\":{\"mmmmmmmm_title\":\"Root text\"}}}", 0, 1 },
        { "mmmmmmmm-place", "", 1, 1, "{\"strings\":{\"en\":{\"mmmmmmmm_title\":\"Map text\"}}}", 0, 0 },
    };
    CHECK(user_doc("{}"));
    set_bundles(builds, 2);
    g_scope_on = 1;
    g_log[0] = '\0';
    CHECK(rearm_strings() == 1);
    CHECK(count_key("mmmmmmmm_title") == 1 && !strcmp(value_for("mmmmmmmm_title"), "Map text"));
    CHECK(strstr(g_log, "REFUSED") == NULL);   /* the inert build does not compete */
    CHECK(sh_strids_scope_check() == 0);
    g_bundles[0].names_off = 0;   /* the map closes: the root's build serves its names, the place's does not */
    g_bundles[1].names_off = 1;
    CHECK(sh_strids_scope_check() == 1);
    CHECK(rearm_strings() == 1);
    CHECK(count_key("mmmmmmmm_title") == 1 && !strcmp(value_for("mmmmmmmm_title"), "Root text"));
    g_scope_on = 0;
    set_bundles(NULL, 0);
    CHECK(rearm_strings() == 1);
    CHECK(count_key("mmmmmmmm_title") == 0);
}

/* A map without mods: nothing is scoped, so the map load's check asks for no
 * refresh, and when the game rebuilds its dictionary only the product's own
 * rows are added again. */
static void test_plain_map(void)
{
    CHECK(user_doc("{}"));
    set_bundles(NULL, 0);
    CHECK(rearm_strings() == 1);
    CHECK(sh_strids_scope_check() == 0);
    CHECK(sh_strids_refresh_pending() == -1);
    g_dictionary.count = 0;
    g_log[0] = '\0';
    CHECK(rearm_strings() == 1);
    CHECK(strstr(g_log, "STRINGS: runtime refresh complete") != NULL);
    CHECK(strstr(g_log, "the game rebuilt its string table") != NULL);
    CHECK(value_for("sh_bv_navigation_desc") != NULL);
    CHECK(sh_strids_scope_check() == 0 && sh_strids_refresh_pending() == -1);
}

/* The engine rebuilds its own dictionary (a language reload): every row this
 * injector added is gone and the game's rows are new records. A Refresh that
 * changes a mod's strings while a map is open must leave a valid table: each
 * key once, the new text, sorted. */
static void test_rebuilt_table(void)
{
    static fake_bundle mod[] = {
        { "rrrrrrrr", "", 0, 1, "{\"strings\":{\"en\":{\"rrrrrrrr_name\":\"Before\",\"rrrrrrrr_other\":\"Other\"}}}" },
    };
    unsigned char game[32] = {0};
    unsigned int i, j;
    CHECK(user_doc("{}"));
    set_bundles(mod, 1);
    CHECK(rearm_strings() == 1 && count_key("rrrrrrrr_name") == 1);
    /* The rebuild: the table holds only the game's rows, as freshly read. */
    g_dictionary.count = 0;
    *(unsigned int *)game = fake_hash("#str_game_row");
    fake_idstr_ctor(game + 8, "#str_game_row");
    fake_idstr_ctor(game + 16, "Game");
    *(unsigned int *)(game + 24) = *(unsigned int *)(game + 28) = 4;
    fake_insert(&g_dictionary, game);
    /* Refresh with a changed string. */
    g_bundles[0].extra = "{\"strings\":{\"en\":{\"rrrrrrrr_name\":\"After\",\"rrrrrrrr_other\":\"Other\"}}}";
    g_log[0] = '\0';
    CHECK(rearm_strings() == 1);
    CHECK(strstr(g_log, "STRINGS: runtime refresh complete") != NULL);
    CHECK(strstr(g_log, "the game rebuilt its string table") != NULL);
    CHECK(count_key("rrrrrrrr_name") == 1 && !strcmp(value_for("rrrrrrrr_name"), "After"));
    CHECK(count_key("rrrrrrrr_other") == 1 && count_key("game_row") == 1);
    CHECK(value_for("sh_bv_navigation_desc") != NULL);   /* the baked layer is back too */
    for (i = 1; i < g_dictionary.count; i++) CHECK(*(unsigned int *)g_records[i - 1] <= *(unsigned int *)g_records[i]);
    for (i = 0; i < g_dictionary.count; i++)
        for (j = i + 1; j < g_dictionary.count; j++)
            CHECK(_stricmp(*(char **)(g_records[i] + 8), *(char **)(g_records[j] + 8)) != 0);
    set_bundles(NULL, 0);
    CHECK(rearm_strings() == 1 && count_key("rrrrrrrr_name") == 0 && count_key("game_row") == 1);
}

/* A refresh that cannot complete leaves the previous table whole: here one
 * wanted key matches two game rows (an ambiguous adoption), while another
 * key the injector owns changes text. Nothing of the pass may land. */
static void test_failed_refresh_changes_nothing(void)
{
    static fake_bundle mod[] = {
        { "ffffffff", "", 0, 1, "{\"strings\":{\"en\":{\"ffffffff_name\":\"Before\"}}}" },
    };
    static unsigned char snapshot[CAP][32];
    unsigned char twin[32] = {0};
    unsigned int count;
    CHECK(user_doc("{}"));
    set_bundles(mod, 1);
    CHECK(rearm_strings() == 1 && !strcmp(value_for("ffffffff_name"), "Before"));
    *(unsigned int *)twin = fake_hash("#str_twin_key");
    fake_idstr_ctor(twin + 8, "#str_twin_key");
    fake_idstr_ctor(twin + 16, "One");
    fake_insert(&g_dictionary, twin);
    fake_idstr_ctor(twin + 8, "#str_TWIN_KEY");
    fake_idstr_ctor(twin + 16, "Two");
    fake_insert(&g_dictionary, twin);
    qsort(g_records, g_dictionary.count, 32, compare_record);
    count = g_dictionary.count;
    memcpy(snapshot, g_records, (size_t)count * 32u);
    g_bundles[0].extra = "{\"strings\":{\"en\":{\"ffffffff_name\":\"After\",\"twin_key\":\"Mine\"}}}";
    g_log[0] = '\0';
    CHECK(rearm_strings() == 0);
    CHECK(strstr(g_log, "STRINGS: runtime refresh FAILED") != NULL);
    CHECK(g_dictionary.count == count && !memcmp(snapshot, g_records, (size_t)count * 32u));
    CHECK(!strcmp(value_for("ffffffff_name"), "Before"));
    /* The game's twin rows go; the next refresh completes. */
    g_dictionary.count = count - 2;
    {
        unsigned int r, w = 0;
        for (r = 0; r < count; r++) {
            const char *k = *(char **)(snapshot[r] + 8);
            if (k && !_stricmp(k, "#str_twin_key")) continue;
            memcpy(g_records[w++], snapshot[r], 32);
        }
        g_dictionary.count = w;
    }
    CHECK(rearm_strings() == 1 && !strcmp(value_for("ffffffff_name"), "After") && !strcmp(value_for("twin_key"), "Mine"));
    set_bundles(NULL, 0);
    g_bundles[0].extra = "{\"strings\":{\"en\":{\"ffffffff_name\":\"Before\"}}}";
    CHECK(rearm_strings() == 1 && count_key("ffffffff_name") == 0 && count_key("twin_key") == 0);
}

static void test_namespaced_keys(void)
{
    static const fake_bundle packs[] = {
        { "ciaaaaaa", "0.1.0", 0, 1,
          "{\"strings\":{\"en\":{\"ciaaaaaa_ci_tab\":\"Campaign Interactables\"}}}" },
        { "cibbbbbb", "0.1.0", 0, 1,
          "{\"strings\":{\"en\":{\"cibbbbbb_ci_tab\":\"Campaign Interactables\"}}}" },
        { "cicccccc", "0.1.0", 0, 1,
          "{\"strings\":{\"en\":{\"cicccccc_ci_tab\":\"Campaign Interactables\"}}}" },
    };
    CHECK(user_doc("{}"));
    set_bundles(packs, (int)(sizeof packs / sizeof packs[0]));
    g_log[0] = '\0';
    CHECK(rearm_strings() == 1);
    CHECK(count_key("ciaaaaaa_ci_tab") == 1 && count_key("cibbbbbb_ci_tab") == 1 &&
          count_key("cicccccc_ci_tab") == 1);
    CHECK(value_for("cibbbbbb_ci_tab") &&
          !strcmp(value_for("cibbbbbb_ci_tab"), "Campaign Interactables"));
    CHECK(strstr(g_log, "REFUSED") == NULL);
    set_bundles(NULL, 0);
    CHECK(rearm_strings() == 1);
    CHECK(count_key("cibbbbbb_ci_tab") == 0);
}


static void invoke_sort_hook(void)
{
    int before = g_sorts;
    CHECK(g_hook_detour != NULL);
    g_hook_detour(&g_dictionary, g_records, g_dictionary.count, 0x20);
    CHECK(g_sorts == before + 1);
}

static void test_sort_hook_recovery(void)
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
    CHECK(user_doc("{ \"hook_recovery_key\" : \"Recovery\" }"));
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
    {
        /* An index published before the first injection: that pass reads it. */
        static const fake_bundle boot[] = {
            { "boot.mod", "1.0.0", 0, 1,
              "{\"strings\":{\"en\":{\"boot_name\":\"Boot\",\"hook_recovery_key\":\"Recovery\"}}}" },
        };
        set_bundles(boot, 1);
    }
    g_log[0] = '\0';
    CHECK(sh_strids_install(fake_sort, 1, table_lea, fake_insert,
                           fake_hash, fake_idstr_ctor) == 1);
    CHECK(g_hook_prepares == prepares + 1 && hook_is_installed(g_hook_owned));
    CHECK(count_key("hook_recovery_key") == 1);
    CHECK(count_key("boot_name") == 1 && !strcmp(value_for("boot_name"), "Boot"));
    CHECK(strstr(g_log, "STRINGS: injected ") != NULL && strstr(g_log, "(bundles 2, baked ") != NULL);
    CHECK(rearm_strings() == 1);
    CHECK(count_key("hook_recovery_key") == 1);
    /* The engine rebuilds its table (a language load as the editor or a map
     * starts) and sorts it: a mod's names are back before that sort orders
     * it, so nothing parsed after the rebuild misses them. */
    g_dictionary.count = 0;
    g_log[0] = '\0';
    invoke_sort_hook();
    CHECK(count_key("hook_recovery_key") == 1 && count_key("boot_name") == 1);
    CHECK(strstr(g_log, "the game rebuilt its string table") != NULL);
    for (unsigned int i = 1; i < g_dictionary.count; i++)
        CHECK(*(unsigned int *)g_records[i-1] <= *(unsigned int *)g_records[i]);
    /* where the process heap cannot be selected, the next main-thread pass re-adds them */
    g_dictionary.count = 0;
    g_push_ignored = 1;
    invoke_sort_hook();
    CHECK(count_key("boot_name") == 0);
    g_push_ignored = 0;
    CHECK(sh_strids_refresh_pending() == 1 && count_key("boot_name") == 1);
    /* After it, a published index asks for one refresh, which rearms once. */
    {
        static const fake_bundle later[] = {
            { "later.mod", "1.0.0", 0, 1, "{\"strings\":{\"en\":{\"later_name\":\"Later\"}}}" },
        };
        int visits;
        set_bundles(later, 1);
        CHECK(sh_strids_refresh_pending() == -1);   /* nothing requested */
        sh_strids_request_refresh();
        visits = g_bundle_visits;
        CHECK(sh_strids_refresh_pending() == 1);
        CHECK(g_bundle_visits == visits + 1);
        CHECK(count_key("later_name") == 1 && count_key("boot_name") == 0);
        CHECK(sh_strids_refresh_pending() == -1);
        CHECK(g_bundle_visits == visits + 1);
        set_bundles(NULL, 0);
        sh_strids_request_refresh();
        CHECK(sh_strids_refresh_pending() == 1 && count_key("later_name") == 0);
    }
    prepares = g_hook_prepares;
    CHECK(sh_strids_install(fake_sort, 1, table_lea, fake_insert,
                           fake_hash, fake_idstr_ctor) == 1);
    CHECK(g_hook_prepares == prepares);
}


/* A runtime pass grows the game's table in the process heap, never in the
 * map's heap the game frees with the map, and puts the map's heap back; a
 * thread where the process heap cannot be selected runs no pass and keeps
 * the refresh for the next one. */
static void test_runtime_pass_heap(void)
{
    CHECK(user_doc("{\"heap_key\":\"in the process heap\"}"));
    g_insert_heap = -2;
    CHECK(rearm_strings() == 1);
    CHECK(count_key("heap_key") == 1 && g_insert_heap == 0);
    CHECK(heap_depth() == 0 && heap_top() == 7);
    CHECK(user_doc("{\"heap_key\":\"in the process heap\",\"later_key\":\"next pass\"}"));
    g_push_ignored = 1;
    g_insert_heap = -2;
    g_log[0] = 0;
    CHECK(rearm_strings() == 0);
    CHECK(count_key("later_key") == 0 && g_insert_heap == -2);
    CHECK(strstr(g_log, "runtime refresh waits") != NULL);
    g_push_ignored = 0;
    CHECK(rearm_strings() == 1);   /* the next pass on the main thread */
    CHECK(count_key("later_key") == 1 && g_insert_heap == 0);
    sh_strids_set_process_heap(NULL);
    CHECK(rearm_strings() == 0);
    sh_strids_set_process_heap(&g_heap_api);
}

int main(void)
{
    char temp[MAX_PATH], root[MAX_PATH];
    DWORD pid = GetCurrentProcessId();

    GetTempPathA(sizeof temp, temp);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%ssnapmap-plus-strids-%lu", temp, (unsigned long)pid);
    remove_tree(root);
    CHECK(make_dir(root));
    /* Without a mod the baked defaults still apply. */
    run_inject();
    CHECK(g_appended > 0);
    CHECK(strstr(g_log, "user") == NULL);
    {
        const char *id = NULL, *owner = NULL;
        CHECK(sh_strids_test_row(0, &id, &owner) && owner && !strcmp(owner, "<baked>"));
    }

    /* A mod supplies new keys and outranks a baked default. */
    CHECK(user_doc("{ \"ai_cyberdemon_name\" : \"Cyberdemon\","
                           "  \"sh_bv_navigation_desc\" : \"MY OWN TEXT\" }"));
    run_inject();
    CHECK(value_for("ai_cyberdemon_name") && !strcmp(value_for("ai_cyberdemon_name"), "Cyberdemon"));
    CHECK(count_key("sh_bv_navigation_desc") == 1);
    CHECK(value_for("sh_bv_navigation_desc") && !strcmp(value_for("sh_bv_navigation_desc"), "MY OWN TEXT"));

    /* JSON escapes are decoded before native interning. */
    CHECK(user_doc("{\"ai_cyberdemon_name\":\"MY OWN NAME\","
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
            CHECK(user_doc(invalid[i]));
            run_inject();
            CHECK(count_key("partial_user_key") == 0);
        }
    }

    /* Rearm appends new keys and updates owned keys after native sorting moved
     * their rows. Repeated refresh never duplicates. */
    CHECK(user_doc("{\"ai_cyberdemon_name\":\"MY OWN NAME\"}"));
    run_inject();
    sh_strids_test_set_sort((void *)fake_sort);
    sh_strids_set_process_heap(&g_heap_api);
    CHECK(user_doc("{\"ai_cyberdemon_name\":\"MY OWN NAME\",\"runtime_key\":\"First value\"}"));
    CHECK(rearm_strings() == 1);
    CHECK(count_key("runtime_key") == 1 && !strcmp(value_for("runtime_key"), "First value"));
    CHECK(user_doc("{\"ai_cyberdemon_name\":\"MY OWN NAME\",\"runtime_key\":\"Updated value\"}"));
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
    CHECK(user_doc("{\"runtime_key\":\"Updated value\",\"append_refusal\":\"try again\"}"));
    g_insert_refused = 1;
    CHECK(rearm_strings() == 0);
    CHECK(count_key("append_refusal") == 0);
    g_insert_refused = 0;
    CHECK(rearm_strings() == 1);
    CHECK(count_key("append_refusal") == 1);
    test_runtime_pass_heap();
    CHECK(user_doc("{}"));
    CHECK(rearm_strings() == 1);
    test_bundle_layer();
    test_namespaced_keys();
    test_string_edits();
    test_scope_check_many_bundles();
    test_own_strings_follow_the_build();
    test_plain_map();
    test_rebuilt_table();
    test_failed_refresh_changes_nothing();
    test_dictionary_retirement();
    test_sort_hook_recovery();
    test_native_table_extent();
    g_hook_installed = 0; /* simulate a removal that still owns its record */
    CHECK(sh_strids_install(NULL, 0, NULL, NULL, NULL, NULL) == 0);
    CHECK(g_hook_owned == NULL && rearm_strings() == 0);

    remove_tree(root);
    reset_doubles();

    if (g_failed) {
        fprintf(stderr, "strids_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    printf("strids_test: ok\n");
    return 0;
}
