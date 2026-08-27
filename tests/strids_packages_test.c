/* strids_packages_test -- a package ships its own #str_ strings.
 *
 * Before this layer existed the injector read exactly one document, the user's global
 * strings\strids.json, so a package that added an entity had no way to name it: the only route was
 * hand-editing a file every other package also had to share. These cases pin the layer that fixed it,
 * and the precedence that keeps the old behaviour intact.
 *
 * The four engine entry points cannot be reimplemented here -- they intern into an engine-private
 * string pool and grow an engine-owned idList -- so the test supplies doubles and inspects what the
 * injector asked them to append.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "../src/backend/strids.h"
#include "../src/backend/overrides.h"
#include "../src/backend/resource_bridge.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_failed++; } } while (0)

/* Only the injector is under test, so the rest of its collaborators are stubbed. backend_log is
 * CAPTURED rather than discarded: a cross-package conflict is supposed to name both packages, and a
 * diagnostic nobody can read is not a diagnostic. */
static char g_log[8192];

void backend_log(const char *message)
{
    if (!message) return;
    strncat_s(g_log, sizeof g_log, message, _TRUNCATE);
    strncat_s(g_log, sizeof g_log, "\n", _TRUNCATE);
}

void *install_inline_hook(void *target, void *detour, size_t stolen)
{
    (void)target; (void)detour; (void)stolen;
    return NULL;   /* the test binds the engine doubles directly; no detour is installed */
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

/* ------------------------------------------------------------------ engine doubles */

#define CAP 1024
static char g_keys[CAP][256];
static char g_vals[CAP][512];
static int  g_pairs;
static int  g_appended;

/* The injector interns the key, then the value, then appends. Recording them in ctor order is what
 * lets the test read back the (key, value) it actually asked the engine for. */
static void fake_idstr_ctor(void *out_handle, const char *s)
{
    *(void **)out_handle = (void *)s;
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
    (void)table_desc; (void)record32;
    g_appended++;
    if (g_pairs < CAP) g_pairs++;     /* one (key,value) pair completed */
    return 1;
}

static void reset_doubles(void)
{
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
    for (int i = 0; i < g_pairs; i++)
        if (_stricmp(g_keys[i], want) == 0) return g_vals[i];
    return NULL;
}

static int count_key(const char *id)
{
    char want[256];
    int n = 0;
    _snprintf_s(want, sizeof want, _TRUNCATE, "#str_%s", id);
    for (int i = 0; i < g_pairs; i++)
        if (_stricmp(g_keys[i], want) == 0) n++;
    return n;
}

/* ------------------------------------------------------------------ fixture helpers */

/* Delete a fixture tree, files first. The test names its tree after the process id, and pids are
 * recycled -- without this, a later run inherits an earlier run's packages and the injector sees
 * a conflict the test never set up. */
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
    sh_strids_test_inject((void *)0x1, (void *)fake_insert, (void *)fake_hash, (void *)fake_idstr_ctor);
}

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
    /* Own EVERY layer. An unset source path falls back to %LOCALAPPDATA%\snapmap-plus\strings\
     * strids.json -- the developer's real document -- which would make this test's result depend on
     * whose machine it runs on. Point it inside the fixture at a file that does not exist yet. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\user_strids.json", root);
    sh_strids_set_source(path);

    /* A package's own strings are injected, so an entity it adds can be named without the user
     * editing anything. This is the whole point of the layer. */
    CHECK(install_strings(overrides, "cyberdemon", "cyberdemon.json",
                          "{ \"ai_cyberdemon_name\" : \"Cyberdemon\","
                          "  \"cyber_desc\" : \"A towering cybernetic demon.\" }"));
    run_inject();
    CHECK(value_for("ai_cyberdemon_name") != NULL);
    CHECK(value_for("ai_cyberdemon_name") && strcmp(value_for("ai_cyberdemon_name"), "Cyberdemon") == 0);
    CHECK(value_for("cyber_desc") != NULL);

    /* The row is attributed to the package that supplied it, which is what lets a conflict name names. */
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

    /* Identical text in two packages COMPOSES: a shared prerequisite vendored into both is being
     * self-contained, not wrong. One row, no complaint. */
    CHECK(install_strings(overrides, "zz-second", "shared.json",
                          "{ \"shared_key\" : \"Shared Text\" }"));
    CHECK(install_strings(overrides, "cyberdemon", "shared.json",
                          "{ \"shared_key\" : \"Shared Text\" }"));
    run_inject();
    CHECK(count_key("shared_key") == 1);

    /* A real disagreement keeps the FIRST definition and never appends twice -- a duplicate key would
     * corrupt the engine's sorted-by-hash dictionary, so "refuse" here means "do not append". */
    CHECK(install_strings(overrides, "zz-second", "shared.json",
                          "{ \"shared_key\" : \"A DIFFERENT VALUE\" }"));
    run_inject();
    CHECK(count_key("shared_key") == 1);
    CHECK(value_for("shared_key") && strcmp(value_for("shared_key"), "Shared Text") == 0);
    /* And it says so, naming both sides: silently picking a winner by enumeration order is exactly
     * what the composition rule exists to prevent. */
    CHECK(strstr(g_log, "REFUSED") != NULL);
    CHECK(strstr(g_log, "shared_key") != NULL);
    CHECK(strstr(g_log, "zz-second") != NULL);
    CHECK(strstr(g_log, "cyberdemon") != NULL);

    /* The user's own document still OUTRANKS a package: their explicit override is the top layer, and
     * that precedence is what the pre-package behaviour depended on. */
    CHECK(write_text(path, "{ \"ai_cyberdemon_name\" : \"MY OWN NAME\" }"));
    run_inject();
    CHECK(count_key("ai_cyberdemon_name") == 1);
    CHECK(value_for("ai_cyberdemon_name") &&
          strcmp(value_for("ai_cyberdemon_name"), "MY OWN NAME") == 0);
    sh_strids_set_source(NULL);

    /* No packages installed at all is not an error: the injector still runs and the baked defaults
     * still cover the shipped pack. */
    sh_overrides_set_root(NULL);

    remove_tree(root);                 /* and do not leave one behind for the next run either */

    if (g_failed) {
        fprintf(stderr, "strids_packages_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    printf("strids_packages_test: ok\n");
    return 0;
}
