/* Tests map-package shard reconstruction, safe extraction and the load gate
 * against generated reference fixtures (map_package_fixtures.h). Registration
 * and consent are stubbed; this suite does not verify live engine installation. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map_package.h"
#include "map_package_share.h"
#include "map_shards.h"
#include "map_provenance.h"
#include "package_archive.h"
#include "package_runtime.h"
#include "engine_dialog.h"
#include "map_package_fixtures.h"
#include "map_package_historical_fixtures.h"
#include "r137_map_fixtures.h"
#include "resource_catalog.h"
#include "config_json.h"
#include "package_sources.h"
#include <bcrypt.h>

/* Runtime registration and re-arm are stubbed; the live engine path is outside this suite. */
static int g_registration_ready, g_rearm_requests;
static const sh_package_compilation *g_compiled;
static const sh_package_compilation *g_effective_map;
static int g_compile_ready = 1;
const sh_package_compilation *sh_package_runtime_acquire(void) { return g_effective_map ? g_effective_map : g_compiled; }
const sh_package_compilation *sh_package_runtime_library_acquire(void) { return g_compiled; }
void sh_package_runtime_release(void) {}
sh_package_bundle *sh_package_runtime_installed_bundle(const unsigned char fingerprint[32]) { (void)fingerprint; return NULL; }
int sh_package_runtime_ready(void) { return g_compile_ready; }
int sh_package_runtime_admission_ready(void) { return g_compile_ready && g_registration_ready; }
/* sh_user_overrides off for this launch: nothing is served. */
static int g_serving_disabled;
int sh_package_runtime_serving_disabled(void) { return g_serving_disabled; }
int sh_decl_server_registration_succeeded(void)
{
    return g_registration_ready;
}

void sh_decl_server_request_rearm(void)
{
    g_rearm_requests++;
}

static int g_source_notices;
void sh_package_runtime_note_sources_changed(void) { g_source_notices++; }
/* A gate installation (no continuing map load) asks its refresh to fold the
 * published packages in as an installation does. */
static int g_install_notices;
void sh_package_runtime_note_install_published(void) { g_install_notices++; }

static int g_failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

static void complete_install(void)
{
    char error[256] = "";
    int notices = g_source_notices;
    CHECK(sh_mpkg_activation_commit(error, sizeof(error)));
    /* The committed files become library sources the compiled provider has not
     * scanned; the runtime has to be told before the map overlay changes. */
    CHECK(g_source_notices == notices + 1);
}

/* Retain diagnostics so valid peer installation cannot regress to replacement. */
static char g_log[4096];
void backend_log(const char *message)
{
    size_t used = strlen(g_log), length = message ? strlen(message) : 0;
    if (!length || used + length + 2 >= sizeof(g_log)) return;
    memcpy(g_log + used, message, length); g_log[used + length] = '\n'; g_log[used + length + 1] = 0;
}

static void join(char *out, size_t size, const char *a, const char *b)
{
    _snprintf_s(out, size, _TRUNCATE, "%s\\%s", a, b);
}

static int make_dir(const char *path)
{
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int touch(const char *path, const char *body)
{
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wrote = 0;
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (body && body[0]) WriteFile(file, body, (DWORD)strlen(body), &wrote, NULL);
    CloseHandle(file);
    return 1;
}

static void remove_tree(const char *path)
{
    char pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", path);
    search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(found.cFileName, ".") == 0 ||
                strcmp(found.cFileName, "..") == 0) continue;
            join(child, sizeof(child), path, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else DeleteFileA(child);
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    RemoveDirectoryA(path);
}

static int file_exists(const char *path)
{
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* The sweep's owner lock: a share-none .owner file in the directory, which a
 * second opener cannot share while this process holds it. */
static int owner_locked(const char *directory)
{
    char path[MAX_PATH];
    HANDLE probe;
    join(path, sizeof(path), directory, ".owner");
    probe = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (probe != INVALID_HANDLE_VALUE) { CloseHandle(probe); return 0; }
    return GetLastError() == ERROR_SHARING_VIOLATION;
}

static int dir_exists(const char *path)
{
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* Read a whole file; returns bytes read into caller's buffer, or -1. */
static long read_file(const char *path, unsigned char *buf, size_t cap)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD rd = 0;
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (!ReadFile(h, buf, (DWORD)cap, &rd, NULL)) { CloseHandle(h); return -1; }
    CloseHandle(h);
    return (long)rd;
}

/* Verify an unpacked tree matches the fixture package byte for byte. */
static void check_unpacked_members(const char *dest)
{
    size_t i;
    for (i = 0; i < sizeof(fix_members) / sizeof(fix_members[0]); i++) {
        char rel[MAX_PATH], path[MAX_PATH];
        unsigned char buf[4096];
        long got;
        size_t j;
        strcpy_s(rel, sizeof rel, fix_members[i].path);
        for (j = 0; rel[j]; j++) if (rel[j] == '/') rel[j] = '\\';
        join(path, sizeof path, dest, rel);
        got = read_file(path, buf, sizeof buf);
        CHECK(got == (long)fix_members[i].len);
        if (got == (long)fix_members[i].len && got > 0)
            CHECK(memcmp(buf, fix_members[i].body, (size_t)got) == 0);
    }
}

/* Check balance and orphan commas outside strings; this is not a full JSON parser. */
static int json_well_formed(const char *json, size_t len)
{
    size_t i;
    int depth = 0, in_string = 0;
    char prev = 0;
    for (i = 0; i < len; i++) {
        char c = json[i];
        if (in_string) {
            if (c == '\\') { i++; continue; }
            if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') { in_string = 1; prev = c; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '{' || c == '[') depth++;
        if (c == '}' || c == ']') {
            if (prev == ',') return 0;          /* trailing comma before a close */
            depth--;
            if (depth < 0) return 0;
        }
        if (c == ',' && (prev == ',' || prev == '[' || prev == '{')) return 0;
        prev = c;
    }
    return depth == 0 && !in_string;
}

/* Strip `json` and assert the shape of the result. Frees the stripped buffer. */
static void check_strip(const char *json, size_t len, int expect_stripped)
{
    size_t out_len = 0;
    char *out = sh_mpkg_strip(json, len, &out_len);
    sh_mpkg_decl after[16];

    if (!expect_stripped) {
        CHECK(out == NULL);
        if (out) HeapFree(GetProcessHeap(), 0, out);
        return;
    }

    CHECK(out != NULL);
    if (!out) return;
    CHECK(out_len > 0);
    CHECK(out_len < len);                      /* something actually came out */
    CHECK(strlen(out) == out_len);             /* NUL-terminated at the reported length */
    CHECK(json_well_formed(out, out_len));
    CHECK(sh_mpkg_scan(out, out_len, after, 16) == 0);   /* no shards left */
    HeapFree(GetProcessHeap(), 0, out);
}

/* Build the map members required by embedding; extra supplies existing
 * string variables to test allocCount updates. */
#define EMPTY_MAP \
    "{\"variables\":{\"allocCount\":[0,0,0,0,0,0,0,0,0,0],\"string\":[]},\"name\":\"m\"}"
#define ONE_VAR_MAP \
    "{\"variables\":{\"allocCount\":[0,0,0,0,1,0,0,0,0,0],\"string\":[" \
    "{\"info\":{\"name\":\"score\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"\"," \
    "\"~type\":\"snapVarString_t\"}]},\"name\":\"m\"}"

/* Read `variables.allocCount[4]` back out of a map, or -1 if it cannot be read. Deliberately a
 * separate, dumber parser than the one under test: a shared bug would pass both. */
static int alloc_count_string(const char *json)
{
    const char *a = strstr(json, "\"allocCount\"");
    int i;
    if (!a) return -1;
    a = strchr(a, '[');
    if (!a) return -1;
    a++;
    for (i = 0; i < 4; i++) {
        a = strchr(a, ',');
        if (!a) return -1;
        a++;
    }
    return atoi(a);
}

/* Embed `payload` into `json`, then read it back the way a delivered map is read. */
static void check_round_trip(const char *json, const unsigned char *payload, size_t payload_len,
                             int expect_total_vars)
{
    char err[SH_MPKG_ERR_CAP];
    size_t out_len = 0, back_len = 0;
    char *out = sh_mpkg_embed(json, strlen(json), "demons-testpkg",
                              payload, payload_len, &out_len, err, sizeof err);
    sh_mpkg_decl decls[16];
    unsigned char *back;
    char digest[SH_MPKG_DIGEST_CHARS + 1];

    CHECK(out != NULL);
    if (!out) { fprintf(stderr, "  embed said: %s\n", err); return; }
    CHECK(strlen(out) == out_len);
    CHECK(json_well_formed(out, out_len));

    /* the consumer's own view of what we just wrote */
    CHECK(sh_mpkg_scan(out, out_len, decls, 16) == 1);
    CHECK(strcmp(decls[0].id, "demons-testpkg") == 0);
    CHECK(decls[0].consistent == 1 && decls[0].complete == 1);
    CHECK(decls[0].present == decls[0].total);

    sh_mpkg_digest16(payload, payload_len, digest);
    CHECK(strcmp(decls[0].digest, digest) == 0);

    back = sh_mpkg_extract(out, out_len, "demons-testpkg", &back_len, err, sizeof err);
    CHECK(back != NULL);
    if (back) {
        CHECK(back_len == payload_len);
        if (back_len == payload_len) CHECK(memcmp(back, payload, payload_len) == 0);
        HeapFree(GetProcessHeap(), 0, back);
    } else {
        fprintf(stderr, "  extract said: %s\n", err);
    }

    /* the engine reads this slot, not the list length */
    CHECK(alloc_count_string(out) == expect_total_vars);

    /* embed and strip are inverses */
    {
        size_t stripped_len = 0;
        char *stripped = sh_mpkg_strip(out, out_len, &stripped_len);
        sh_mpkg_decl none[16];
        CHECK(stripped != NULL);
        if (stripped) {
            CHECK(sh_mpkg_scan(stripped, stripped_len, none, 16) == 0);
            CHECK(json_well_formed(stripped, stripped_len));
            HeapFree(GetProcessHeap(), 0, stripped);
        }
    }
    HeapFree(GetProcessHeap(), 0, out);
}


/* Controllable native modal boundary; filesystem staging and consent polling
 * remain the production paths. engine_dialog_test covers the native layout. */
static int g_dialog_idle, g_dialog_asks, g_dialog_answer, g_dialog_raise_fails;
static char g_dialog_text[256];
int sh_engine_dialog_ready(void) { return 0; }
int sh_engine_dialog_can_ask(void) { return g_dialog_idle; }
int sh_engine_dialog_ask(unsigned gdm_id, unsigned button_set, const char *text)
{
    CHECK(gdm_id == 0x29 && text);
    CHECK(button_set == 1 || (button_set == 6 && text && (strstr(text, "Install") || strstr(text, "Open the map without it?"))));
    CHECK(g_dialog_idle);
    strcpy_s(g_dialog_text, sizeof(g_dialog_text), text);
    g_dialog_asks++;
    return g_dialog_raise_fails ? 0 : 1;
}
int sh_engine_dialog_poll(int ticket) { CHECK(ticket == 1); return g_dialog_answer; }
void sh_engine_dialog_release(int ticket) { (void)ticket; }

static void test_player_notices(void)
{
    char diagnostic[768], save_notice[256];
    sh_mpkg_test_reset();
    memset(diagnostic, 'x', sizeof(diagnostic) - 1);
    memcpy(diagnostic, "private/path: parser offset 812; ", 32);
    diagnostic[sizeof(diagnostic) - 1] = 0;
    g_log[0] = 0; g_dialog_asks = 0; g_dialog_idle = 0;
    g_dialog_answer = SH_ENGINE_DIALOG_PENDING; g_dialog_raise_fails = 0;
    sh_mpkg_report_error(SH_MPKG_PROBLEM_SAVE, diagnostic);
    CHECK(strstr(g_log, diagnostic) != NULL);
    sh_mpkg_consent_poll(); CHECK(g_dialog_asks == 0);
    g_dialog_idle = 1;
    sh_mpkg_consent_poll(); CHECK(g_dialog_asks == 1);
    CHECK(strlen(g_dialog_text) < 128 && strstr(g_dialog_text, "saved"));
    CHECK(!strstr(g_dialog_text, "private/path") && !strstr(g_dialog_text, "812"));
    strcpy_s(save_notice, sizeof(save_notice), g_dialog_text);
    sh_mpkg_consent_poll(); CHECK(g_dialog_asks == 1);
    g_dialog_answer = SH_ENGINE_DIALOG_ACCEPTED; sh_mpkg_consent_poll();
    sh_mpkg_report_error(SH_MPKG_PROBLEM_OPEN, diagnostic);
    sh_mpkg_consent_poll(); CHECK(g_dialog_asks == 2);
    CHECK(strcmp(g_dialog_text, save_notice) && strstr(g_dialog_text, "opened"));
    CHECK(!strstr(g_dialog_text, "parser") && strlen(g_dialog_text) < 128);
    sh_mpkg_consent_poll();
    sh_mpkg_report_error(SH_MPKG_PROBLEM_OPEN, "");
    sh_mpkg_consent_poll(); CHECK(g_dialog_asks == 2);
    sh_mpkg_test_reset(); g_dialog_idle = 0;
}

static unsigned char batch_fingerprints[129][32];
static int batch_publish_calls, batch_fail, batch_interrupt;
static char batch_staging[MAX_PATH], batch_destination[MAX_PATH];
static void check_batch_package(const char *folder, size_t index);

static int batch_publish(const char *staging, const char *destination)
{
    char path[MAX_PATH];
    batch_publish_calls++;
    strcpy_s(batch_staging, sizeof(batch_staging), staging);
    strcpy_s(batch_destination, sizeof(batch_destination), destination);
    /* One commit sees both complete trees, not the first package of a chain. */
    join(path, sizeof(path), staging, "batch-00"); check_batch_package(path, 0);
    join(path, sizeof(path), staging, "batch-01"); check_batch_package(path, 1);
    CHECK(!dir_exists(destination));
    if (batch_fail) { SetLastError(ERROR_DISK_FULL); return 0; }
    if (batch_interrupt == 1) TerminateProcess(GetCurrentProcess(), 81);
    if (!MoveFileExA(staging, destination, MOVEFILE_WRITE_THROUGH)) return 0;
    if (batch_interrupt == 2) TerminateProcess(GetCurrentProcess(), 82);
    return 1;
}

static void batch_interrupt_prepare(const char *package)
{
    (void)package;
    TerminateProcess(GetCurrentProcess(), 83);
}

static void batch_edit_prepare(const char *package)
{
    char path[MAX_PATH];
    join(path, sizeof(path), package, "assets\\shared.bimage"); CHECK(touch(path, "changed while preparing"));
}

static int installed_package(const char *root, const char *id, char out[MAX_PATH])
{
    char error[512];
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error));
    int found = 0;
    CHECK(sources); out[0] = 0;
    if (!sources) return 0;
    for (size_t i = 0; i < sources->component_count; i++) {
        const sh_package_component *c = &sources->components[i];
        if (!c->relative[0] && !strcmp(c->descriptor.id, id)) {
            CHECK(!found); strcpy_s(out, MAX_PATH, c->root); found++;
        }
    }
    sh_package_sources_free(sources); return found == 1;
}

static size_t installed_count(const char *root)
{
    char error[512];
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error));
    size_t count = sources ? sources->package_count : SIZE_MAX;
    CHECK(sources); sh_package_sources_free(sources); return count;
}

/* Generate distinct, valid archives through the product packer. Both packages
 * intentionally contain the same opaque resource, plus an empty author folder. */
static char *make_batch_map(const char *author, size_t count, size_t *length)
{
    char *map = (char *)HeapAlloc(GetProcessHeap(), 0, strlen(EMPTY_MAP) + 1);
    size_t i;
    CHECK(map); if (!map) return NULL;
    strcpy_s(map, strlen(EMPTY_MAP) + 1, EMPTY_MAP); *length = strlen(map);
    CHECK(make_dir(author));
    for (i = 0; i < count; i++) {
        char id[32], folder[MAX_PATH], path[MAX_PATH], descriptor[128], error[512];
        sh_package_sources *sources;
        unsigned char *archive;
        size_t archive_length, next_length;
        char *next;
        snprintf(id, sizeof(id), "batch-%02zu", i);
        join(folder, sizeof(folder), author, id); CHECK(make_dir(folder));
        join(path, sizeof(path), folder, "package.json");
        snprintf(descriptor, sizeof(descriptor), "{\"id\":\"%s\",\"name\":\"Batch package %zu\"}", id, i);
        CHECK(touch(path, descriptor));
        join(path, sizeof(path), folder, "assets"); CHECK(make_dir(path));
        join(path, sizeof(path), folder, "assets\\shared.bimage"); CHECK(touch(path, "unchanged shared resource"));
        join(path, sizeof(path), folder, "empty"); CHECK(make_dir(path));
        sources = sh_package_sources_scan_directory(folder, error, sizeof(error));
        CHECK(sources); if (!sources) goto failed;
        memcpy(batch_fingerprints[i], sources->fingerprints[0], 32);
        archive = sh_package_archive_pack(sources, 0, &archive_length, error, sizeof(error));
        sh_package_sources_free(sources);
        CHECK(archive); if (!archive) goto failed;
        next = sh_mpkg_embed(map, *length, id, archive, archive_length, &next_length, error, sizeof(error));
        free(archive); CHECK(next); if (!next) goto failed;
        HeapFree(GetProcessHeap(), 0, map); map = next; *length = next_length;
    }
    return map;
failed:
    HeapFree(GetProcessHeap(), 0, map); return NULL;
}

static void check_batch_package(const char *folder, size_t index)
{
    char error[512];
    sh_package_sources *sources = sh_package_sources_scan_directory(folder, error, sizeof(error));
    CHECK(sources);
    if (sources) CHECK(!memcmp(sources->fingerprints[0], batch_fingerprints[index], 32));
    sh_package_sources_free(sources);
}

/* The child dies inside the real installer, so no unwinding, rollback or
 * in-memory bookkeeping can help the parent observe the expected disk state. */
static int interrupt_install_child(const char *client, const char *author, int when)
{
    size_t length;
    char *map = make_batch_map(author, 2, &length);
    if (!map) return 90;
    sh_mpkg_boot_capture(client);
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
    batch_interrupt = when;
    if (when == 3) sh_mpkg_test_set_prepare(batch_interrupt_prepare);
    else sh_mpkg_test_set_publish(batch_publish);
    sh_mpkg_gate(map, length);
    if (when == 4) TerminateProcess(GetCurrentProcess(), 84);
    if (when == 5) {
        complete_install();
        TerminateProcess(GetCurrentProcess(), 85);
    }
    return 91; /* The requested interruption point must have terminated us. */
}

static void run_interrupted_install(const char *client, const char *author, int when)
{
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {0};
    char exe[MAX_PATH], command[MAX_PATH * 4];
    DWORD code = 0;
    CHECK(GetModuleFileNameA(NULL, exe, sizeof(exe)));
    snprintf(command, sizeof(command), "\"%s\" --interrupt-install \"%s\" \"%s\" %d", exe, client, author, when);
    CHECK(CreateProcessA(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process));
    if (!process.hProcess) return;
    CHECK(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
    CHECK(GetExitCodeProcess(process.hProcess, &code));
    CHECK(code == (DWORD)(80 + when));
    printf("install interruption boundary %d: child terminated with code %lu\n", when, code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}

static void test_batch_install(const char *root)
{
    char author[MAX_PATH], client[MAX_PATH], overrides[MAX_PATH], first[MAX_PATH], second[MAX_PATH], marker[MAX_PATH];
    char *map;
    size_t length;
    join(author, sizeof(author), root, "batch-author");
    join(client, sizeof(client), root, "batch-client"); CHECK(make_dir(client));
    join(overrides, sizeof(overrides), client, "overrides"); CHECK(make_dir(overrides));
    map = make_batch_map(author, 2, &length); if (!map) return;

    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
    sh_mpkg_test_set_publish(batch_publish);
    g_registration_ready = g_rearm_requests = batch_publish_calls = g_install_notices = 0;
    CHECK(!sh_mpkg_gate(map, length));
    CHECK(batch_publish_calls == 1 && sh_mpkg_test_session_installed_count() == 2 && g_rearm_requests == 1 &&
        g_install_notices == 1);
    CHECK(installed_package(client, "batch-00", first)); CHECK(installed_package(client, "batch-01", second));
    check_batch_package(first, 0); check_batch_package(second, 1);
    CHECK(!sh_mpkg_gate(map, length));
    g_registration_ready = 1; CHECK(!sh_mpkg_gate(map, length));
    complete_install(); CHECK(sh_mpkg_gate(map, length));
    remove_tree(overrides); CHECK(make_dir(overrides));

    /* All fingerprints are checked after preparing the entire set. A changed
     * staged member must never reach the sole publication operation. */
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
    sh_mpkg_test_set_prepare(batch_edit_prepare); sh_mpkg_test_set_publish(batch_publish);
    g_rearm_requests = batch_publish_calls = g_install_notices = 0;
    CHECK(!sh_mpkg_gate(map, length));
    CHECK(installed_count(client) == 0 && !batch_publish_calls && !g_rearm_requests && !g_install_notices);

    /* A failed commit installs none, discards temporary files and allows retry. */
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT); sh_mpkg_test_set_publish(batch_publish);
    g_rearm_requests = batch_publish_calls = g_install_notices = 0; batch_fail = 1;
    CHECK(!sh_mpkg_gate(map, length));
    CHECK(batch_publish_calls == 1 && !g_rearm_requests && !g_install_notices && !sh_mpkg_test_session_installed_count());
    CHECK(installed_count(client) == 0 && !dir_exists(batch_staging) && !dir_exists(batch_destination));
    batch_fail = 0;
    CHECK(!sh_mpkg_gate(map, length));
    CHECK(installed_count(client) == 2 && g_rearm_requests == 1 && g_install_notices == 1);
    g_log[0] = 0;
    sh_mpkg_report_error(SH_MPKG_PROBLEM_INSTALL, "specific activation failure");
    CHECK(sh_mpkg_activation_cancel());
    CHECK(installed_count(client) == 0 && !sh_mpkg_test_session_installed_count());
    g_dialog_idle = 1; g_dialog_asks = 0; g_dialog_answer = SH_ENGINE_DIALOG_PENDING;
    sh_mpkg_consent_poll();
    CHECK(g_dialog_asks == 1 && strstr(g_dialog_text, "couldn't be installed"));
    CHECK(!strstr(g_dialog_text, "specific activation failure") && strstr(g_log, "specific activation failure"));
    g_dialog_answer = SH_ENGINE_DIALOG_ACCEPTED; sh_mpkg_consent_poll();
    g_dialog_idle = 0;
    CHECK(!sh_mpkg_gate(map, length));
    CHECK(installed_count(client) == 2 && g_rearm_requests == 2);
    {
        /* A final commit failure keeps the entire group pending. No package
         * can pass the gate until the caller recovers and cancels the set. */
        char pending[MAX_PATH], error[256]; HANDLE blocked;
        int notices = g_source_notices;
        strcpy_s(pending, sizeof(pending), batch_staging);
        strcpy_s(strrchr(pending, '\\') + 1, sizeof(pending) - (strrchr(pending, '\\') + 1 - pending), "pending");
        blocked = CreateFileA(pending, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(blocked != INVALID_HANDLE_VALUE);
        CHECK(!sh_mpkg_activation_commit(error, sizeof(error)));
        CHECK(!sh_mpkg_gate(map, length));
        CHECK(!sh_mpkg_activation_cancel() && installed_count(client) == 2);
        if (blocked != INVALID_HANDLE_VALUE) CloseHandle(blocked);
        CHECK(!sh_mpkg_activation_commit(error, sizeof(error))); /* Failed cancellation cannot become success. */
        /* Nothing was committed, so the library has no new sources to rescan. */
        CHECK(g_source_notices == notices);
        CHECK(sh_mpkg_activation_cancel() && installed_count(client) == 0);
        CHECK(!sh_mpkg_gate(map, length)); complete_install();
        CHECK(installed_count(client) == 2 && sh_mpkg_gate(map, length));
    }
    remove_tree(overrides); CHECK(make_dir(overrides));

    /* Abrupt termination before or after publication must cancel the whole
     * uncommitted install on boot. Only completed activation survives. */
    for (int when = 1; when <= 5; when++) {
        run_interrupted_install(client, author, when);
        CHECK(installed_count(client) == (when == 2 || when >= 4 ? 2 : 0));
        sh_mpkg_test_reset();
        if (when == 2) {
            /* A locked cancellation record blocks capture. Once released,
             * the same process must cancel before capturing source identities. */
            char staging[MAX_PATH], pattern[MAX_PATH], working[MAX_PATH], pending[MAX_PATH];
            WIN32_FIND_DATAA found; HANDLE search, blocked = INVALID_HANDLE_VALUE;
            join(staging, sizeof(staging), client, "package-staging");
            join(pattern, sizeof(pattern), staging, "map-*");
            search = FindFirstFileA(pattern, &found); CHECK(search != INVALID_HANDLE_VALUE);
            if (search != INVALID_HANDLE_VALUE) {
                join(working, sizeof(working), staging, found.cFileName);
                join(pending, sizeof(pending), working, "pending"); FindClose(search);
                blocked = CreateFileA(pending, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                CHECK(blocked != INVALID_HANDLE_VALUE);
            }
            sh_mpkg_boot_capture(client);
            CHECK(!sh_mpkg_startup_ready() && installed_count(client) == 2);
            CHECK(!sh_mpkg_gate(map, length));
            if (blocked != INVALID_HANDLE_VALUE) CloseHandle(blocked);
        }
        sh_mpkg_boot_capture(client);
        CHECK(sh_mpkg_startup_ready());
        if (when == 5) CHECK(sh_mpkg_gate(map, length));
        else {
            char staging[MAX_PATH], pattern[MAX_PATH]; WIN32_FIND_DATAA found; HANDLE search; size_t abandoned = 0;
            join(staging, sizeof(staging), client, "package-staging"); join(pattern, sizeof(pattern), staging, "map-*");
            search = FindFirstFileA(pattern, &found);
            if (search != INVALID_HANDLE_VALUE) { do { abandoned++; } while (FindNextFileA(search, &found)); FindClose(search); }
            CHECK(abandoned == 0 && installed_count(client) == 0);
            sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT); g_rearm_requests = 0;
            CHECK(!sh_mpkg_gate(map, length)); CHECK(g_rearm_requests == 1 && installed_count(client) == 2);
            complete_install();
            search = FindFirstFileA(pattern, &found); CHECK(search == INVALID_HANDLE_VALUE);
            if (search != INVALID_HANDLE_VALUE) FindClose(search);
        }
        CHECK(installed_package(client, "batch-00", first)); CHECK(installed_package(client, "batch-01", second));
        check_batch_package(first, 0); check_batch_package(second, 1);
        remove_tree(overrides); CHECK(make_dir(overrides));
    }

    /* Declining applies to the whole set for this load, not the game session. */
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_DECLINE); g_rearm_requests = 0;
    CHECK(!sh_mpkg_gate(map, length));
    CHECK(installed_count(client) == 0 && !g_rearm_requests);
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
    CHECK(!sh_mpkg_gate(map, length)); CHECK(installed_count(client) == 2 && g_rearm_requests == 1);
    complete_install();
    remove_tree(overrides); CHECK(make_dir(overrides));

    /* Existing author content is preserved and the delivery still installs: a
     * different-content variant of an authored identity is published beside it,
     * and the compiler resolves the two as variants of one package. An identical
     * tree at another label is reused instead of installed. */
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
    g_dialog_idle = 1; g_dialog_asks = g_dialog_raise_fails = 0; g_dialog_answer = SH_ENGINE_DIALOG_PENDING;
    CHECK(!sh_mpkg_gate(map, length)); sh_mpkg_consent_poll(); CHECK(g_dialog_asks == 1);
    {
        char renamed[MAX_PATH], error[512]; unsigned char *payload; size_t payload_length; unsigned files;
        join(renamed, sizeof(renamed), overrides, "author-chosen-label");
        payload = sh_mpkg_extract(map, length, "batch-01", &payload_length, error, sizeof(error)); CHECK(payload);
        if (payload) {
            CHECK(sh_mpkg_unpack(payload, payload_length, renamed, &files, error, sizeof(error)));
            HeapFree(GetProcessHeap(), 0, payload);
        }
        join(marker, sizeof(marker), renamed, "assets\\shared.bimage"); CHECK(touch(marker, "author change"));
        g_rearm_requests = 0; g_log[0] = 0;
        g_dialog_answer = SH_ENGINE_DIALOG_ACCEPTED; sh_mpkg_consent_poll();
        /* Both packages install; the author's edited copy is untouched and is
         * now one of two variants of that identity. */
        CHECK(g_rearm_requests == 1 && sh_mpkg_test_session_installed_count() == 2);
        CHECK(installed_count(client) == 3);
        { unsigned char bytes[32]; CHECK(read_file(marker, bytes, sizeof(bytes)) == 13 && !memcmp(bytes, "author change", 13)); }
        CHECK(sh_mpkg_activation_cancel()); CHECK(installed_count(client) == 1);
        { unsigned char bytes[32]; CHECK(read_file(marker, bytes, sizeof(bytes)) == 13 && !memcmp(bytes, "author change", 13)); }
        g_rearm_requests = 0;
        CHECK(touch(marker, "unchanged shared resource"));
        sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
        CHECK(!sh_mpkg_gate(map, length));
        CHECK(sh_mpkg_test_session_installed_count() == 2 && g_rearm_requests == 1);
        CHECK(installed_package(client, "batch-00", first)); check_batch_package(first, 0); check_batch_package(renamed, 1);
        CHECK(installed_package(client, "batch-01", second) && !strcmp(second, renamed));
        CHECK(sh_mpkg_activation_cancel());
        CHECK(installed_count(client) == 1); check_batch_package(renamed, 1);
        CHECK(!sh_mpkg_gate(map, length)); complete_install();
        CHECK(installed_count(client) == 2 && sh_mpkg_gate(map, length));
    }
    remove_tree(overrides); CHECK(make_dir(overrides));
    HeapFree(GetProcessHeap(), 0, map);

    map = make_batch_map(author, 129, &length);
    if (map) {
        sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
        g_dialog_idle = 1; g_dialog_asks = g_rearm_requests = 0; g_dialog_answer = SH_ENGINE_DIALOG_PENDING;
        CHECK(!sh_mpkg_gate(map, length)); sh_mpkg_consent_poll();
        CHECK(g_dialog_asks == 1 && !sh_mpkg_test_session_installed_count()); CHECK(strstr(g_dialog_text, "more"));
        g_dialog_answer = SH_ENGINE_DIALOG_ACCEPTED; sh_mpkg_consent_poll();
        CHECK(sh_mpkg_test_session_installed_count() == 129 && g_rearm_requests == 1 && installed_count(client) == 129);
        for (size_t i = 0; i < 129; i++) {
            char id[32], folder[MAX_PATH]; snprintf(id, sizeof(id), "batch-%02zu", i);
            CHECK(installed_package(client, id, folder)); check_batch_package(folder, i);
        }
        g_registration_ready = 1; complete_install(); CHECK(sh_mpkg_gate(map, length));
        /* Every high-index identity survives a fresh boot capture too. */
        sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
        g_rearm_requests = g_dialog_asks = 0;
        CHECK(sh_mpkg_gate(map, length)); CHECK(!g_rearm_requests && !g_dialog_asks);
        {
            sh_mpkg_decl *all = calloc(129, sizeof(*all));
            CHECK(all && sh_mpkg_scan(map, length, all, 129) == 129);
            CHECK(sh_mpkg_scan(map, length, all, 64) == SIZE_MAX);
            free(all);
        }
        HeapFree(GetProcessHeap(), 0, map);
    }
    sh_mpkg_test_reset(); g_registration_ready = g_rearm_requests = 0;
    g_dialog_idle = g_dialog_asks = 0; g_dialog_answer = SH_ENGINE_DIALOG_PENDING;
    remove_tree(client); remove_tree(author);
}

/* Build a one-package map whose content is chosen by the caller, so two maps can
 * deliver the same package identity with genuinely different bytes. */
static char *make_variant_map(const char *author, const char *id, const char *body,
    unsigned char fingerprint[32], size_t *length)
{
    char folder[MAX_PATH], path[MAX_PATH], descriptor[160], error[512];
    sh_package_sources *sources;
    unsigned char *archive = NULL;
    size_t archive_length = 0;
    char *map = NULL;
    CHECK(make_dir(author));
    join(folder, sizeof(folder), author, id); remove_tree(folder); CHECK(make_dir(folder));
    join(path, sizeof(path), folder, "package.json");
    snprintf(descriptor, sizeof(descriptor), "{\"id\":\"%s\",\"name\":\"Variant package\"}", id);
    CHECK(touch(path, descriptor));
    join(path, sizeof(path), folder, "assets"); CHECK(make_dir(path));
    join(path, sizeof(path), folder, "assets\\variant.bimage"); CHECK(touch(path, body));
    sources = sh_package_sources_scan_directory(folder, error, sizeof(error));
    CHECK(sources); if (!sources) return NULL;
    memcpy(fingerprint, sources->fingerprints[0], 32);
    archive = sh_package_archive_pack(sources, 0, &archive_length, error, sizeof(error));
    sh_package_sources_free(sources);
    CHECK(archive); if (!archive) return NULL;
    map = sh_mpkg_embed(EMPTY_MAP, strlen(EMPTY_MAP), id, archive, archive_length, length, error, sizeof(error));
    free(archive); CHECK(map); return map;
}

/* Count the installed packages carrying this identity whose tree matches the
 * fingerprint. Variants of one identity may coexist, so this does not assume a
 * unique match. */
static int variant_matches(const char *root, const char *id, const unsigned char fingerprint[32])
{
    char error[512];
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error));
    int matches = 0;
    CHECK(sources);
    if (!sources) return 0;
    for (size_t i = 0; i < sources->component_count; i++) {
        const sh_package_component *c = &sources->components[i];
        if (c->relative[0] || strcmp(c->descriptor.id, id)) continue;
        if (!memcmp(sources->fingerprints[c->owner], fingerprint, 32)) matches++;
    }
    sh_package_sources_free(sources); return matches;
}

/* IDs do not replace installed authored trees. Same-ID deliveries coexist;
 * canceling a later transaction removes only its own newly published tree. */
static void test_delivered_variants(const char *root)
{
    char author[MAX_PATH], client[MAX_PATH], overrides[MAX_PATH], authored[MAX_PATH], path[MAX_PATH];
    unsigned char first_print[32], second_print[32], third_print[32];
    char *first = NULL, *second = NULL, *third = NULL;
    size_t first_length = 0, second_length = 0, third_length = 0;
    join(author, sizeof(author), root, "variants-author");
    join(client, sizeof(client), root, "variants-client"); CHECK(make_dir(client));
    join(overrides, sizeof(overrides), client, "overrides"); CHECK(make_dir(overrides));
    first = make_variant_map(author, "demons-variant", "variant one bytes", first_print, &first_length);
    second = make_variant_map(author, "demons-variant", "variant two bytes, longer", second_print, &second_length);
    third = make_variant_map(author, "demons-variant", "variant three", third_print, &third_length);
    if (!first || !second || !third) goto done;
    CHECK(memcmp(first_print, second_print, 32) && memcmp(second_print, third_print, 32));

    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
    g_registration_ready = 1; g_rearm_requests = 0;
    CHECK(!sh_mpkg_gate(first, first_length));
    complete_install();
    CHECK(installed_count(client) == 1 && variant_matches(client, "demons-variant", first_print));
    /* The same bytes again need no installation at all. */
    CHECK(sh_mpkg_gate(first, first_length));
    CHECK(installed_count(client) == 1);

    g_log[0] = 0;
    CHECK(!sh_mpkg_gate(second, second_length));
    CHECK(!strstr(g_log, "superseding"));
    CHECK(installed_count(client) == 2 && variant_matches(client, "demons-variant", second_print));
    CHECK(variant_matches(client, "demons-variant", first_print) == 1);
    complete_install();
    CHECK(installed_count(client) == 2 && variant_matches(client, "demons-variant", second_print));
    CHECK(variant_matches(client, "demons-variant", first_print) == 1);
    CHECK(sh_mpkg_gate(second, second_length));

    /* A canceled addition leaves both previous deliveries untouched. */
    CHECK(!sh_mpkg_gate(third, third_length));
    CHECK(installed_count(client) == 3 && variant_matches(client, "demons-variant", third_print));
    CHECK(sh_mpkg_activation_cancel());
    CHECK(installed_count(client) == 2 && variant_matches(client, "demons-variant", second_print));
    CHECK(variant_matches(client, "demons-variant", first_print) == 1);
    CHECK(variant_matches(client, "demons-variant", third_print) == 0);
    /* Both earlier deliveries remain usable after a fresh process inventory. */
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
    g_registration_ready = 1;
    CHECK(sh_mpkg_gate(first, first_length));
    CHECK(sh_mpkg_gate(second, second_length));

    /* Upgrade recovery still restores a tree retired by an older release's
     * pending supersession journal. New installations never create this form. */
    {
        char error[512], prior[MAX_PATH] = "", stage[MAX_PATH], batch[MAX_PATH];
        char area[MAX_PATH], slot[MAX_PATH], content[MAX_PATH], origin[MAX_PATH];
        const char marker[] = "Snapmap+ pending package installation\n";
        sh_package_sources *inventory = sh_package_sources_scan(client, error, sizeof(error));
        CHECK(inventory);
        if (inventory) {
            for (size_t owner = 0; owner < inventory->package_count; owner++)
                if (!memcmp(inventory->fingerprints[owner], first_print, 32))
                    strcpy_s(prior, sizeof(prior), inventory->packages[owner].root);
            sh_package_sources_free(inventory);
        }
        CHECK(prior[0] && !strncmp(prior, overrides, strlen(overrides)));
        if (prior[0]) {
            join(stage, sizeof(stage), client, "package-staging"); CHECK(make_dir(stage));
            join(batch, sizeof(batch), stage, "map-0000000000000001"); CHECK(make_dir(batch));
            join(area, sizeof(area), batch, "superseded"); CHECK(make_dir(area));
            join(slot, sizeof(slot), area, "0"); CHECK(make_dir(slot));
            join(origin, sizeof(origin), slot, "origin");
            CHECK(touch(origin, prior + strlen(overrides) + 1));
            join(content, sizeof(content), slot, "content");
            join(path, sizeof(path), batch, "pending");
            FILE *file = NULL;
            CHECK(!fopen_s(&file, path, "wb") && file);
            if (file) { CHECK(fwrite(marker, 1, sizeof(marker), file) == sizeof(marker)); fclose(file); }
            CHECK(MoveFileExA(prior, content, MOVEFILE_WRITE_THROUGH));
            CHECK(!dir_exists(prior));
            sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
            CHECK(dir_exists(prior) && !dir_exists(batch));
            CHECK(installed_count(client) == 2);
            CHECK(variant_matches(client, "demons-variant", first_print) == 1);
            CHECK(variant_matches(client, "demons-variant", second_print) == 1);
        }
    }

    /* An authored package of the same identity is the user's own work: it is
     * neither moved nor replaced, and it does not block the delivery. The map
     * bundle installs beside it and both trees keep their exact bytes. */
    remove_tree(overrides); CHECK(make_dir(overrides));
    join(authored, sizeof(authored), overrides, "my-variant"); CHECK(make_dir(authored));
    join(path, sizeof(path), authored, "package.json");
    CHECK(touch(path, "{\"id\":\"demons-variant\",\"name\":\"Authored variant\"}"));
    join(path, sizeof(path), authored, "assets"); CHECK(make_dir(path));
    join(path, sizeof(path), authored, "assets\\variant.bimage"); CHECK(touch(path, "authored bytes"));
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
    g_registration_ready = 1; g_log[0] = 0;
    CHECK(!sh_mpkg_gate(second, second_length));
    CHECK(!strstr(g_log, "superseding"));
    CHECK(installed_count(client) == 2 && dir_exists(authored) && file_exists(path));
    CHECK(sh_mpkg_test_session_installed_count() == 1);
    {   /* The author's bytes are exactly what they were. */
        unsigned char bytes[32];
        CHECK(read_file(path, bytes, sizeof(bytes)) == 14 && !memcmp(bytes, "authored bytes", 14));
    }
    complete_install();
    CHECK(installed_count(client) == 2 && variant_matches(client, "demons-variant", second_print) == 1);
done:
    sh_mpkg_test_reset();
    if (first) HeapFree(GetProcessHeap(), 0, first);
    if (second) HeapFree(GetProcessHeap(), 0, second);
    if (third) HeapFree(GetProcessHeap(), 0, third);
    g_registration_ready = 0; g_log[0] = 0;
    remove_tree(client); remove_tree(author);
}

static void test_refreshed_author_sources(const char *root)
{
    char client[MAX_PATH], overrides[MAX_PATH], package[MAX_PATH], descriptor[MAX_PATH], error[512];
    sh_package_sources *sources = NULL;
    sh_package_compilation compiled = {0};
    unsigned char *archive = NULL;
    char *map = NULL;
    size_t archive_length = 0, map_length = 0;
    unsigned files;
    join(client, sizeof(client), root, "refreshed-client"); CHECK(make_dir(client));
    join(overrides, sizeof(overrides), client, "overrides"); CHECK(make_dir(overrides));
    join(package, sizeof(package), overrides, "authored");
    CHECK(sh_mpkg_unpack(fix_payload, sizeof(fix_payload), package, &files, error, sizeof(error)));
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
    join(descriptor, sizeof(descriptor), package, "package.json");
    CHECK(touch(descriptor, "{\"id\":\"demons-testpkg\",\"name\":\"Author revised the installed package\"}"));
    sources = sh_package_sources_scan(client, error, sizeof(error));
    CHECK(sources != NULL);
    if (!sources) goto done;
    archive = sh_package_archive_pack(sources, 0, &archive_length, error, sizeof(error));
    CHECK(archive != NULL);
    if (!archive) goto done;
    map = sh_mpkg_embed(EMPTY_MAP, strlen(EMPTY_MAP), FIX_PKG_ID, archive, archive_length,
                        &map_length, error, sizeof(error));
    CHECK(map != NULL);
    if (!map) goto done;
    compiled.sources = sources; g_compiled = &compiled;
    g_dialog_idle = 1; g_dialog_asks = 0; g_dialog_answer = SH_ENGINE_DIALOG_PENDING;
    g_registration_ready = g_compile_ready = 1;
    CHECK(sh_mpkg_gate(map, map_length));
    sh_mpkg_consent_poll(); CHECK(!g_dialog_asks);
    CHECK(!sh_mpkg_test_session_installed_count());
    /* A published source snapshot alone is not native admission. */
    g_registration_ready = 0;
    CHECK(!sh_mpkg_gate(map, map_length));
    CHECK(strstr(sh_mpkg_test_last_refusal(), "registration is incomplete or failed") != NULL);
    sh_mpkg_consent_poll(); CHECK(!g_dialog_asks);
    g_registration_ready = 1; g_compile_ready = 0;
    CHECK(!sh_mpkg_gate(map, map_length));
    sh_mpkg_consent_poll(); CHECK(!g_dialog_asks);
    g_compile_ready = 1; CHECK(sh_mpkg_gate(map, map_length));
    /* Files changed after compilation cannot satisfy the saved map. */
    CHECK(touch(descriptor, "{\"id\":\"demons-testpkg\",\"name\":\"Changed again without refresh\"}"));
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_DECLINE);
    CHECK(!sh_mpkg_gate(map, map_length));
done:
    g_compiled = NULL; g_compile_ready = 1; g_registration_ready = 0;
    sh_mpkg_test_reset();
    if (map) HeapFree(GetProcessHeap(), 0, map);
    free(archive); sh_package_sources_free(sources); remove_tree(client);
}

static void test_consent_retirement(const char *root, const char *overrides)
{
    char package[MAX_PATH];
    join(package, sizeof package, overrides, FIX_PKG_ID);
    sh_mpkg_test_reset();
    sh_mpkg_boot_capture(root);
    g_dialog_idle = g_dialog_asks = g_dialog_raise_fails = 0;
    g_dialog_answer = SH_ENGINE_DIALOG_PENDING;
    CHECK(!sh_mpkg_gate(fix_map_happy, fix_map_happy_len));
    {
        int previous = g_registration_ready;
        g_registration_ready = 1;
        CHECK(!sh_mpkg_activation_ready()); /* A pending dialog also blocks the no-install route. */
        g_registration_ready = previous;
    }
    for (int i = 0; i < 20; i++) sh_mpkg_consent_poll();
    CHECK(!g_dialog_asks && !dir_exists(package));

    /* Waiting for native retirement retains the staged package and asks once. */
    g_dialog_idle = 1;
    sh_mpkg_consent_poll();
    CHECK(g_dialog_asks == 1 && !dir_exists(package));
    for (int i = 0; i < 3; i++) sh_mpkg_consent_poll();
    CHECK(g_dialog_asks == 1 && !dir_exists(package));
    g_dialog_answer = SH_ENGINE_DIALOG_ACCEPTED;
    sh_mpkg_consent_poll();
    CHECK(installed_package(root, FIX_PKG_ID, package) && sh_mpkg_test_session_installed_count() == 1);
    remove_tree(package);

    /* A permanently occupied native surface declines within the wait budget. */
    sh_mpkg_test_reset();
    sh_mpkg_boot_capture(root);
    g_dialog_idle = g_dialog_asks = 0;
    CHECK(!sh_mpkg_gate(fix_map_happy, fix_map_happy_len));
    for (int i = 0; i < 610; i++) sh_mpkg_consent_poll();
    g_dialog_idle = 1;
    sh_mpkg_consent_poll();
    CHECK(!g_dialog_asks && !dir_exists(package));
    CHECK(!sh_mpkg_test_session_installed_count());

    /* A native raise failure never converts a request into consent. */
    sh_mpkg_test_reset();
    sh_mpkg_boot_capture(root);
    g_dialog_raise_fails = 1;
    CHECK(!sh_mpkg_gate(fix_map_happy, fix_map_happy_len));
    sh_mpkg_consent_poll();
    CHECK(g_dialog_asks == 1 && !dir_exists(package));
    CHECK(!sh_mpkg_test_session_installed_count());
    g_dialog_raise_fails = g_dialog_idle = 0;

    /* No retires consent. Idle ticks cannot ask again; a new load can. */
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(root);
    g_dialog_idle = 1; g_dialog_asks = g_rearm_requests = 0;
    g_dialog_answer = SH_ENGINE_DIALOG_PENDING;
    CHECK(!sh_mpkg_gate(fix_map_happy, fix_map_happy_len)); sh_mpkg_consent_poll();
    CHECK(g_dialog_asks == 1);
    g_dialog_answer = SH_ENGINE_DIALOG_DECLINED; sh_mpkg_consent_poll();
    for (int i = 0; i < 10; i++) sh_mpkg_consent_poll();
    CHECK(g_dialog_asks == 1 && !g_rearm_requests && !sh_mpkg_test_session_installed_count());
    CHECK(!sh_mpkg_gate(fix_map_happy, fix_map_happy_len));
    g_dialog_answer = SH_ENGINE_DIALOG_PENDING; sh_mpkg_consent_poll();
    CHECK(g_dialog_asks == 2);
    g_dialog_answer = SH_ENGINE_DIALOG_ACCEPTED; sh_mpkg_consent_poll();
    CHECK(g_rearm_requests == 1 && sh_mpkg_test_session_installed_count() == 1);
    CHECK(installed_package(root, FIX_PKG_ID, package));
    complete_install(); remove_tree(package);
    g_dialog_idle = 0;
}

/* With user packages off for this launch (sh_user_overrides 0) a map that
 * declares packages is refused as such, whether its package is missing or
 * already installed: no installation is offered, staged or published, and the
 * player is told the reason, not a failed compile. Readiness reports the same
 * reason. A map declaring nothing passes. */
static void test_serving_disabled_gate(const char *root, const char *overrides)
{
    static const char vanilla[] = "{\"variables\":{\"string\":[]}}";
    char package[MAX_PATH];
    join(package, sizeof package, overrides, FIX_PKG_ID);
    for (int installed = 0; installed <= 1; installed++) {
        if (installed) {
            /* Installed by an earlier launch with packages on. */
            sh_mpkg_test_reset(); sh_mpkg_boot_capture(root);
            g_dialog_idle = 1; g_dialog_asks = g_rearm_requests = 0;
            g_dialog_answer = SH_ENGINE_DIALOG_PENDING;
            CHECK(!sh_mpkg_gate(fix_map_happy, fix_map_happy_len)); sh_mpkg_consent_poll();
            g_dialog_answer = SH_ENGINE_DIALOG_ACCEPTED; sh_mpkg_consent_poll();
            CHECK(g_rearm_requests == 1 && installed_package(root, FIX_PKG_ID, package));
            complete_install();
        }
        sh_mpkg_test_reset(); sh_mpkg_boot_capture(root);
        g_serving_disabled = 1; g_dialog_idle = 1; g_dialog_asks = g_rearm_requests = 0;
        g_dialog_answer = SH_ENGINE_DIALOG_PENDING; g_dialog_text[0] = 0;
        CHECK(!sh_mpkg_gate(fix_map_happy, fix_map_happy_len));
        CHECK(strstr(sh_mpkg_test_last_refusal(), "serving is disabled") != NULL);
        for (int i = 0; i < 5; i++) sh_mpkg_consent_poll();
        CHECK(g_dialog_asks == 1 && strstr(g_dialog_text, "mods") && strstr(g_dialog_text, "disabled") &&
            !strstr(g_dialog_text, "sh_user_overrides") && !strstr(g_dialog_text, "Install"));
        CHECK(!g_rearm_requests && !sh_mpkg_test_session_installed_count());
        CHECK(!!dir_exists(package) == installed);
        CHECK(!sh_mpkg_activation_ready() && strstr(sh_mpkg_test_last_refusal(), "serving is disabled"));
        CHECK(sh_mpkg_gate(vanilla, sizeof(vanilla) - 1));
        g_serving_disabled = 0;
    }
    remove_tree(package); g_dialog_idle = 0;
}

static void test_private_map_context(const char *root)
{
    char error[512], folder[MAX_PATH], package[MAX_PATH], id[SH_PACKAGE_ID_CAP];
    unsigned char expected[32];
    sh_mpkg_context *a = NULL, *b = NULL;
    sh_package_sources *sources;
    char *combined; size_t combined_length;
    CHECK(sh_package_archive_identity(fix_payload, fix_payload_len, id, expected, error, sizeof(error)));
    a = sh_mpkg_context_open(root, fix_map_happy, fix_map_happy_len, error, sizeof(error));
    CHECK(a && sh_mpkg_context_count(a) == 1 && sh_mpkg_context_root(a));
    if (!a) { fprintf(stderr, "%s\n", error); return; }
    strcpy_s(folder, sizeof(folder), sh_mpkg_context_root(a));
    CHECK(strstr(folder, "\\package-cache\\maps\\map-") != NULL);
    sources = sh_package_sources_scan(folder, error, sizeof(error));
    CHECK(sources && sources->package_count == 1 && !memcmp(sources->fingerprints[0], expected, 32));
    if (sources) {
        sh_package_compilation private_view = {0};
        private_view.sources = sources;
        sh_mpkg_test_reset(); sh_mpkg_boot_capture(root);
        sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_DECLINE);
        g_effective_map = &private_view; g_registration_ready = 1;
        CHECK(!sh_mpkg_gate(fix_map_happy, fix_map_happy_len));
        CHECK(strstr(sh_mpkg_test_last_refusal(), "uninstalled package"));
        CHECK(!sh_mpkg_test_session_installed_count());
        g_effective_map = NULL; sh_mpkg_test_reset();
    }
    sh_package_sources_free(sources);
    sources = sh_package_sources_scan(root, error, sizeof(error));
    CHECK(sources && sources->package_count == 0); /* Private bytes never satisfy installation. */
    sh_package_sources_free(sources);
    b = sh_mpkg_context_open(root, fix_map_happy_pretty, fix_map_happy_pretty_len, error, sizeof(error));
    CHECK(b && strcmp(sh_mpkg_context_root(a), sh_mpkg_context_root(b)));
    CHECK(sh_mpkg_context_close(&a) && !a && !dir_exists(folder));
    CHECK(b && dir_exists(sh_mpkg_context_root(b))); /* A and B own separate complete trees. */
    CHECK(sh_mpkg_context_close(&b) && !b);
    CHECK(sh_mpkg_context_close(&b));
    a = sh_mpkg_context_open(NULL, "{}", 2, error, sizeof(error));
    CHECK(a && !sh_mpkg_context_count(a) && !sh_mpkg_context_root(a));
    CHECK(sh_mpkg_context_close(&a));
    CHECK(!sh_mpkg_context_open("", fix_map_happy, fix_map_happy_len, error, sizeof(error)));
    {
        const char native[] = "{\"message\":\"old \x97 and UTF-8 \xc3\xa9\","
            "\"variables\":{\"string\":[],\"allocCount\":[0,0,0,0,0]}}";
        a = sh_mpkg_context_open(NULL, native, sizeof(native) - 1, error, sizeof(error));
        CHECK(a && !sh_mpkg_context_count(a) && !sh_mpkg_context_root(a));
        CHECK(sh_mpkg_context_close(&a));
        combined = sh_mpkg_embed(native, sizeof(native) - 1, FIX_PKG_ID,
            fix_payload, fix_payload_len, &combined_length, error, sizeof(error));
        CHECK(combined && strstr(combined, "old \x97 and UTF-8 \xc3\xa9"));
        if (combined) {
            a = sh_mpkg_context_open(root, combined, combined_length, error, sizeof(error));
            CHECK(a && sh_mpkg_context_count(a) == 1);
            CHECK(sh_mpkg_context_close(&a));
            HeapFree(GetProcessHeap(), 0, combined);
        }
    }
    CHECK(!sh_mpkg_context_open(root, "{", 1, error, sizeof(error)));
    CHECK(strstr(error, "byte 1") && strstr(error, "invalid JSON syntax"));
    CHECK(!sh_mpkg_context_open(root, fix_map_wrongdigest, fix_map_wrongdigest_len, error, sizeof(error)));
    CHECK(!sh_mpkg_context_open(root, fix_map_incomplete, fix_map_incomplete_len, error, sizeof(error)));
    /* A valid prefix followed by a mismatched descriptor must publish nothing. */
    combined = sh_mpkg_embed(fix_map_happy, fix_map_happy_len, "other",
        fix_payload, fix_payload_len, &combined_length, error, sizeof(error));
    CHECK(combined);
    if (combined) {
        CHECK(!sh_mpkg_context_open(root, combined, combined_length, error, sizeof(error)));
        CHECK(strstr(error, "delivery identity"));
        HeapFree(GetProcessHeap(), 0, combined);
    }
    /* All duplicate assets and empty author folders survive intact delivery. */
    join(package, sizeof(package), root, "context-author");
    combined = make_batch_map(package, 2, &combined_length);
    CHECK(combined);
    if (combined) {
        a = sh_mpkg_context_open(root, combined, combined_length, error, sizeof(error));
        CHECK(a && sh_mpkg_context_count(a) == 2);
        if (a) {
            /* Private owner folders are context-local ordinals; the context
             * records the exact carrier each one holds. */
            char overrides_dir[MAX_PATH];
            CHECK(!strcmp(sh_mpkg_context_bundles(a, NULL)[0].source_name, "0") &&
                !strcmp(sh_mpkg_context_carrier(a, 0), "batch-00"));
            CHECK(!strcmp(sh_mpkg_context_bundles(a, NULL)[1].source_name, "1") &&
                !strcmp(sh_mpkg_context_carrier(a, 1), "batch-01") && !sh_mpkg_context_carrier(a, 2));
            join(overrides_dir, sizeof(overrides_dir), sh_mpkg_context_root(a), "overrides");
            join(folder, sizeof(folder), overrides_dir, "0"); check_batch_package(folder, 0);
            join(folder, sizeof(folder), overrides_dir, "1"); check_batch_package(folder, 1);
            /* The open context owns its directory: the sweep's lock file is
             * there and cannot be taken by another process. */
            CHECK(owner_locked(sh_mpkg_context_root(a)));
            /* A held source prevents deletion, and close retains retry ownership. */
            join(package, sizeof(package), folder, "package.json");
            {
                char retired_root[MAX_PATH];
                HANDLE held = CreateFileA(package, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                CHECK(held != INVALID_HANDLE_VALUE);
                strcpy_s(retired_root, sizeof(retired_root), sh_mpkg_context_root(a));
                CHECK(!sh_mpkg_context_close(&a) && a);
                /* The directory stays owned while its deletion waits, so the
                 * startup sweep of another process leaves it alone. */
                CHECK(owner_locked(retired_root));
                /* After provider retirement, a locked cache no longer holds
                 * the map context. A different map can prepare immediately. */
                sh_mpkg_context_retire(&a); CHECK(!a && dir_exists(folder));
                CHECK(owner_locked(retired_root));
                b = sh_mpkg_context_open(root, fix_map_happy, fix_map_happy_len, error, sizeof(error));
                CHECK(b && sh_mpkg_context_count(b) == 1);
                CHECK(owner_locked(sh_mpkg_context_root(b)));
                strcpy_s(package, sizeof(package), sh_mpkg_context_root(b));
                CHECK(sh_mpkg_context_close(&b));
                /* Closing releases the lock with the directory. */
                CHECK(!dir_exists(package) && !owner_locked(package));
                if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
            }
            sh_mpkg_context_collect(); CHECK(!dir_exists(folder));
        }
        HeapFree(GetProcessHeap(), 0, combined);
    }
    join(package, sizeof(package), root, "context-author"); remove_tree(package);
    join(package, sizeof(package), root, "package-cache\\maps");
    {
        WIN32_FIND_DATAA found; HANDLE search;
        join(folder, sizeof(folder), package, "map-*");
        search = FindFirstFileA(folder, &found);
        CHECK(search == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND);
        if (search != INVALID_HANDLE_VALUE) FindClose(search);
    }
}

static int map_install_calls, map_install_outcome;
static void map_install_completed(void *context, int outcome)
{ CHECK(context == &map_install_calls); map_install_calls++; map_install_outcome = outcome; }

static void test_same_id_deliveries(const char *root)
{
    char author[MAX_PATH], client[MAX_PATH], folder[MAX_PATH], path[MAX_PATH], error[512];
    char key[SH_PACKAGE_ID_CAP], forged[SH_PACKAGE_ID_CAP];
    char *map = HeapAlloc(GetProcessHeap(), 0, strlen(EMPTY_MAP) + 1), *next;
    size_t length = strlen(EMPTY_MAP), next_length, archive_length;
    unsigned char *archive, fingerprint[32];
    sh_mpkg_context *context = NULL;
    sh_package_sources *sources = NULL;
    sh_package_compilation candidate = {0};
    sh_package_owners selected = {0};
    CHECK(map); if (!map) return; memcpy(map, EMPTY_MAP, length + 1);
    join(author, sizeof(author), root, "same-id-author"); CHECK(make_dir(author));
    join(client, sizeof(client), root, "same-id-client"); CHECK(make_dir(client));
    join(path, sizeof(path), client, "overrides"); CHECK(make_dir(path));
    for (size_t i = 0; i < 2; i++) {
        snprintf(folder, sizeof(folder), "%s\\part%zu", author, i); CHECK(make_dir(folder));
        join(path, sizeof(path), folder, "package.json");
        CHECK(touch(path, "{\"id\":\"same.identity\",\"name\":\"Same package family\"}"));
        join(path, sizeof(path), folder, "assets"); CHECK(make_dir(path));
        snprintf(path, sizeof(path), "%s\\assets\\part%zu.bimage", folder, i);
        CHECK(touch(path, i ? "second complete asset" : "first complete asset"));
        sources = sh_package_sources_scan_directory(folder, error, sizeof(error)); CHECK(sources);
        if (!sources) goto done;
        sh_package_archive_delivery_id(sources->fingerprints[0], key);
        archive = sh_package_archive_pack(sources, 0, &archive_length, error, sizeof(error));
        sh_package_sources_free(sources); sources = NULL; CHECK(archive); if (!archive) goto done;
        next = sh_mpkg_embed(map, length, key, archive, archive_length, &next_length, error, sizeof(error));
        CHECK(next);
        if (!i) {
            char *bad; size_t bad_length;
            strcpy_s(forged, sizeof(forged), key); forged[9] = forged[9] == 'a' ? 'b' : 'a';
            bad = sh_mpkg_embed(EMPTY_MAP, strlen(EMPTY_MAP), forged, archive, archive_length, &bad_length, error, sizeof(error));
            CHECK(bad && !sh_mpkg_context_open(client, bad, bad_length, error, sizeof(error)));
            CHECK(strstr(error, "delivery identity")); if (bad) HeapFree(GetProcessHeap(), 0, bad);
        }
        free(archive); if (!next) goto done;
        HeapFree(GetProcessHeap(), 0, map); map = next; length = next_length;
    }
    {
        sh_mpkg_decl declarations[2];
        CHECK(sh_mpkg_scan(map, length, declarations, 2) == 2);
        CHECK(strcmp(declarations[0].id, declarations[1].id));
    }
    context = sh_mpkg_context_open(client, map, length, error, sizeof(error));
    CHECK(context && sh_mpkg_context_count(context) == 2); if (!context) goto done;
    sources = sh_package_sources_scan(sh_mpkg_context_root(context), error, sizeof(error));
    CHECK(sources && sources->package_count == 2); if (!sources) goto done;
    candidate.sources = sources;
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT); map_install_calls = 0;
    CHECK(sh_package_owners_add(&selected, 0) && sh_package_owners_add(&selected, 1));
    CHECK(sh_mpkg_request_map_install(map, length, &candidate, &selected,
        map_install_completed, &map_install_calls, error, sizeof(error)));
    CHECK(map_install_outcome == 1 && installed_count(client) == 2);
    CHECK(sh_mpkg_activation_cancel() && !installed_count(client));
    sh_package_owners_free(&selected); CHECK(sh_package_owners_add(&selected, 0));
    CHECK(sh_mpkg_request_map_install(map, length, &candidate, &selected,
        map_install_completed, &map_install_calls, error, sizeof(error)));
    CHECK(map_install_outcome == 1); complete_install(); CHECK(installed_count(client) == 1);
    memcpy(fingerprint, sources->fingerprints[0], 32);
    CHECK(variant_matches(client, "same.identity", fingerprint) == 1);
    sh_package_owners_free(&selected); CHECK(sh_package_owners_add(&selected, 1));
    CHECK(sh_mpkg_request_map_install(map, length, &candidate, &selected,
        map_install_completed, &map_install_calls, error, sizeof(error)));
    CHECK(map_install_outcome == 1 && installed_count(client) == 2);
    CHECK(variant_matches(client, "same.identity", fingerprint) == 1);
    CHECK(sh_mpkg_activation_cancel() && installed_count(client) == 1);
    CHECK(sh_mpkg_request_map_install(map, length, &candidate, &selected,
        map_install_completed, &map_install_calls, error, sizeof(error)));
    CHECK(map_install_outcome == 1); complete_install(); CHECK(installed_count(client) == 2);
    CHECK(variant_matches(client, "same.identity", sources->fingerprints[0]) == 1);
    CHECK(variant_matches(client, "same.identity", sources->fingerprints[1]) == 1);
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client); g_registration_ready = 1;
    CHECK(sh_mpkg_gate(map, length)); /* Both complete variants survive a fresh inventory. */
done:
    sh_package_owners_free(&selected); sh_package_sources_free(sources);
    CHECK(sh_mpkg_context_close(&context)); HeapFree(GetProcessHeap(), 0, map);
    sh_mpkg_test_reset(); remove_tree(author); remove_tree(client);
}

static void test_resource_owner_install(const char *root)
{
    char author[MAX_PATH], client[MAX_PATH], overrides[MAX_PATH], package[MAX_PATH], error[512];
    char *map;
    size_t length;
    sh_mpkg_context *context;
    sh_package_sources *sources;
    sh_package_compilation candidate = {0};
    sh_package_owners selected = {0};
    join(author, sizeof(author), root, "owner-author");
    join(client, sizeof(client), root, "owner-client"); CHECK(make_dir(client));
    join(overrides, sizeof(overrides), client, "overrides"); CHECK(make_dir(overrides));
    map = make_batch_map(author, 2, &length); CHECK(map);
    if (!map) return;
    context = sh_mpkg_context_open(client, map, length, error, sizeof(error)); CHECK(context);
    sources = context ? sh_package_sources_scan(sh_mpkg_context_root(context), error, sizeof(error)) : NULL;
    CHECK(sources && sources->package_count == 2);
    if (!sources) goto done;
    candidate.sources = sources; CHECK(sh_package_owners_add(&selected, 1));
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
    g_registration_ready = g_compile_ready = 1; g_rearm_requests = map_install_calls = 0;
    CHECK(sh_package_owners_add(&selected, 9));
    CHECK(!sh_mpkg_request_map_install(map, length, &candidate, &selected,
        map_install_completed, &map_install_calls, error, sizeof(error)));
    CHECK(!map_install_calls && !installed_count(client));
    sh_package_owners_free(&selected); CHECK(sh_package_owners_add(&selected, 1));
    sources->fingerprints[1][0] ^= 1;
    CHECK(!sh_mpkg_request_map_install(map, length, &candidate, &selected,
        map_install_completed, &map_install_calls, error, sizeof(error)));
    CHECK(!map_install_calls && !installed_count(client)); sources->fingerprints[1][0] ^= 1;
    /* Cancel pending consent without calling the installer or a global rearm. */
    g_dialog_idle = 1; g_dialog_answer = SH_ENGINE_DIALOG_PENDING;
    CHECK(sh_mpkg_request_map_install(map, length, &candidate, &selected,
        map_install_completed, &map_install_calls, error, sizeof(error)));
    sh_mpkg_consent_poll(); CHECK(strstr(g_dialog_text, "batch-01") && !strstr(g_dialog_text, "batch-00"));
    sh_mpkg_cancel_map_consent(&map_install_calls);
    CHECK(map_install_calls == 1 && !map_install_outcome && !g_rearm_requests && !installed_count(client));
    sh_mpkg_consent_poll(); CHECK(map_install_calls == 1);
    /* The same explicit load may ask again. Only the missing owner installs,
     * but it installs completely, including its duplicate resource and notes. */
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
    CHECK(sh_mpkg_request_map_install(map, length, &candidate, &selected,
        map_install_completed, &map_install_calls, error, sizeof(error)));
    CHECK(map_install_calls == 2 && map_install_outcome == 1 && !g_rearm_requests);
    CHECK(installed_count(client) == 1 && installed_package(client, "batch-01", package));
    check_batch_package(package, 1); CHECK(!sh_mpkg_activation_ready());
    CHECK(sh_mpkg_activation_cancel() && !installed_count(client));
    /* Preparation failure cancels the entire offer and completes once. */
    sh_mpkg_test_set_prepare(batch_edit_prepare);
    CHECK(sh_mpkg_request_map_install(map, length, &candidate, &selected,
        map_install_completed, &map_install_calls, error, sizeof(error)));
    CHECK(map_install_calls == 3 && map_install_outcome == -1 && !g_rearm_requests && !installed_count(client));
    sh_mpkg_test_set_prepare(NULL);
    CHECK(sh_mpkg_request_map_install(map, length, &candidate, &selected,
        map_install_completed, &map_install_calls, error, sizeof(error)));
    CHECK(map_install_calls == 4 && map_install_outcome == 1); complete_install();
    CHECK(sh_mpkg_activation_ready() && !g_rearm_requests && installed_count(client) == 1);
    CHECK(installed_package(client, "batch-01", package));
    check_batch_package(package, 1);
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
    CHECK(installed_count(client) == 1); check_batch_package(package, 1);
done:
    sh_package_owners_free(&selected); sh_package_sources_free(sources);
    CHECK(sh_mpkg_context_close(&context)); HeapFree(GetProcessHeap(), 0, map);
    sh_mpkg_test_reset(); remove_tree(author); remove_tree(client);
    g_dialog_idle = 0;
}

static int inspect_private_map_context(const char *data_root, const char *map_path)
{
    FILE *input = NULL;
    __int64 size;
    char *json = NULL, error[512];
    sh_mpkg_context *context = NULL;
    sh_package_sources *sources = NULL;
    size_t i, files = 0, directories = 0;
    int result = 1;
    if (fopen_s(&input, map_path, "rb") || _fseeki64(input, 0, SEEK_END) ||
        (size = _ftelli64(input)) <= 0 || (uint64_t)size >= SIZE_MAX ||
        _fseeki64(input, 0, SEEK_SET) || !(json = malloc((size_t)size + 1))) goto done;
    if (fread(json, 1, (size_t)size, input) != (size_t)size) goto done;
    json[size] = 0;
    context = sh_mpkg_context_open(data_root, json, (size_t)size, error, sizeof(error));
    if (!context) { fprintf(stderr, "%s\n", error); goto done; }
    if (sh_mpkg_context_count(context)) {
        sources = sh_package_sources_scan(sh_mpkg_context_root(context), error, sizeof(error));
        if (!sources) { fprintf(stderr, "%s\n", error); goto done; }
        for (i = 0; i < sources->file_count; i++) {
            if (sources->files[i].directory) directories++; else files++;
        }
        for (i = 0; i < sources->package_count; i++) {
            size_t j;
            printf("package %zu ", i);
            for (j = 0; j < 32; j++) printf("%02x", sources->fingerprints[i][j]);
            printf("\n");
        }
    }
    printf("private map context: %zu packages, %zu files, %zu authored directories\n",
        sh_mpkg_context_count(context), files, directories);
    result = 0;
done:
    sh_package_sources_free(sources);
    if (!sh_mpkg_context_close(&context)) { fprintf(stderr, "private map context cleanup failed\n"); result = 1; }
    if (input) fclose(input);
    free(json); return result;
}

static void test_legacy_map_context(const char *root)
{
    const char marker[] = "{\"schema\":\"snapmap-plus.override-package.v1\",\"name\":\"Old boss\"}";
    const char decl[] = "{ health = 10; }";
    sh_package_archive_file members[] = {
        {"package.json", (unsigned char *)marker, sizeof(marker) - 1, 0},
        {"decls/entitydef/boss.decl", (unsigned char *)decl, sizeof(decl) - 1, 0}
    };
    sh_package_archive_files files = {members, 2};
    unsigned char *zip, *original;
    char *map, error[512], path[MAX_PATH];
    size_t length, map_length, original_length;
    sh_mpkg_context *context;
    zip = sh_package_archive_write(&files, &length, error, sizeof(error)); CHECK(zip != NULL);
    if (!zip) return;
    map = sh_mpkg_embed(EMPTY_MAP, strlen(EMPTY_MAP), "old-boss", zip, length, &map_length, error, sizeof(error)); CHECK(map != NULL);
    if (!map) { free(zip); return; }
    context = sh_mpkg_context_open(root, map, map_length, error, sizeof(error));
    if (!context) fprintf(stderr, "legacy private context: %s\n", error);
    CHECK(context != NULL);
    if (context) {
        CHECK(sh_mpkg_context_count(context) == 1);
        CHECK(!strcmp(sh_mpkg_context_carrier(context, 0), "old-boss"));
        snprintf(path, sizeof(path), "%s/overrides/%s/assets/generated/decls/entitydef/boss.decl", sh_mpkg_context_root(context),
            sh_mpkg_context_bundles(context, NULL)[0].source_name);
        CHECK(file_exists(path));
        CHECK(sh_mpkg_context_close(&context));
    }
    /* Reconstructing the private view does not rewrite the original map. */
    original = sh_mpkg_extract(map, map_length, "old-boss", &original_length, error, sizeof(error));
    CHECK(original && original_length == length && !memcmp(original, zip, length));
    if (original) HeapFree(GetProcessHeap(), 0, original);
    {
        char *name = strstr(map, "smpkg.old-boss.");
        sh_mpkg_decl declaration;
        CHECK(name != NULL);
        if (name) {
            memcpy(name + 6, "-boss__", 7);
            CHECK(sh_mpkg_scan(map, map_length, &declaration, 1) == 1);
            CHECK(!strncmp(declaration.id, "legacy.", 7));
            context = sh_mpkg_context_open(root, map, map_length, error, sizeof(error));
            CHECK(context && sh_mpkg_context_count(context) == 1);
            CHECK(sh_mpkg_context_close(&context));
        }
    }
    HeapFree(GetProcessHeap(), 0, map); free(zip);
}

static void test_legacy_delivery_roundtrip(const char *root)
{
    const char marker[] = "{\"schema\":\"snapmap-plus.override-package.v1\",\"name\":\"Old boss\"}";
    const char *decls[] = {"{ health = 10; }", "{ health = 20; }"};
    sh_package_bundle_binding originals[2] = {0};
    sh_mpkg_context *context = NULL;
    char key[2][SH_MPKG_ID_CAP] = {{0}}, error[512], seed[SH_PACKAGE_ID_CAP];
    char *combined = HeapAlloc(GetProcessHeap(), 0, strlen(EMPTY_MAP) + 1), *next = NULL;
    size_t combined_length = strlen(EMPTY_MAP);
    CHECK(combined); if (!combined) return;
    strcpy_s(combined, combined_length + 1, EMPTY_MAP);
    for (size_t v = 0; v < 2; v++) {
        sh_package_archive_file members[] = {
            {"package.json", (unsigned char *)marker, sizeof(marker) - 1, 0},
            {"decls/entitydef/boss.decl", (unsigned char *)decls[v], strlen(decls[v]), 0}
        };
        sh_package_archive_files files = {members, 2};
        size_t length, map_length, count;
        unsigned char *zip = sh_package_archive_write(&files, &length, error, sizeof(error));
        char *map = zip ? sh_mpkg_embed(EMPTY_MAP, strlen(EMPTY_MAP), "old-boss", zip, length,
            &map_length, error, sizeof(error)) : NULL;
        CHECK(zip && map);
        if (!map) { free(zip); goto done; }
        context = sh_mpkg_context_open(root, map, map_length, error, sizeof(error)); CHECK(context);
        if (context) {
            const sh_package_bundle_binding *bindings = sh_mpkg_context_bundles(context, &count);
            CHECK(count == 1 && bindings && bindings[0].original);
            if (count == 1 && bindings && bindings[0].original) {
                originals[v] = bindings[0]; originals[v].original = sh_package_bundle_retain(bindings[0].original);
                CHECK(memcmp(originals[v].compiler_tree, sh_package_bundle_fingerprint(originals[v].original), 32));
                sh_package_bundle_delivery_key(originals[v].original, key[v]);
                CHECK(sh_package_bundle_parse_key(key[v], NULL, seed) && !strcmp(seed, "old-boss"));
                next = sh_mpkg_embed(combined, combined_length, key[v], zip, length,
                    &combined_length, error, sizeof(error)); CHECK(next);
                if (next) { HeapFree(GetProcessHeap(), 0, combined); combined = next; next = NULL; }
            }
        }
        CHECK(sh_mpkg_context_close(&context));
        HeapFree(GetProcessHeap(), 0, map); free(zip);
    }
    if (!originals[0].original || !originals[1].original) goto done;
    CHECK(strcmp(key[0], key[1]));
    context = sh_mpkg_context_open(root, combined, combined_length, error, sizeof(error));
    if (!context) fprintf(stderr, "retained legacy roundtrip: %s\n", error);
    CHECK(context && sh_mpkg_context_count(context) == 2);
    if (context) {
        size_t count;
        const sh_package_bundle_binding *bindings = sh_mpkg_context_bundles(context, &count);
        CHECK(count == 2);
        for (size_t v = 0; v < 2; v++) {
            size_t b;
            for (b = 0; b < count; b++) if (!memcmp(sh_package_bundle_fingerprint(bindings[b].original),
                sh_package_bundle_fingerprint(originals[v].original), 32)) break;
            CHECK(b < count);
            if (b < count) {
                char stable[SH_MPKG_ID_CAP];
                unsigned char *a, *z; size_t a_length, z_length;
                CHECK(!memcmp(bindings[b].compiler_tree, originals[v].compiler_tree, 32));
                sh_package_bundle_delivery_key(bindings[b].original, stable); CHECK(!strcmp(stable, key[v]));
                a = sh_package_bundle_read(bindings[b].original, &a_length, error, sizeof(error));
                z = sh_package_bundle_read(originals[v].original, &z_length, error, sizeof(error));
                CHECK(a && z && a_length == z_length && !memcmp(a, z, a_length)); free(a); free(z);
            }
        }
    }
    CHECK(sh_mpkg_context_close(&context));
    /* The maximum existing canonical ID fits the extended transport grammar;
     * its private extraction directory remains bounded. */
    {
        size_t length, map_length, count;
        unsigned char *zip = sh_package_bundle_read(originals[0].original, &length, error, sizeof(error));
        char full_seed[SH_PACKAGE_ID_CAP], extended[SH_MPKG_ID_CAP];
        memset(full_seed, 'a', sizeof(full_seed) - 1); full_seed[sizeof(full_seed) - 1] = 0;
        sh_package_bundle *bundle = zip ? sh_package_bundle_from_legacy_archive(zip, length, full_seed,
            root, error, sizeof(error)) : NULL;
        sh_package_bundle_delivery_key(bundle, extended);
        char *map = bundle ? sh_mpkg_embed(EMPTY_MAP, strlen(EMPTY_MAP), extended, zip, length,
            &map_length, error, sizeof(error)) : NULL;
        CHECK(map);
        context = map ? sh_mpkg_context_open(root, map, map_length, error, sizeof(error)) : NULL;
        CHECK(context && sh_mpkg_context_count(context) == 1);
        if (context) {
            const sh_package_bundle_binding *bindings = sh_mpkg_context_bundles(context, &count);
            char roundtrip[SH_MPKG_ID_CAP];
            CHECK(count == 1 && !strcmp(bindings[0].source_name, "0") && !strcmp(sh_mpkg_context_carrier(context, 0), extended));
            sh_package_bundle_delivery_key(bindings[0].original, roundtrip); CHECK(!strcmp(roundtrip, extended));
        }
        CHECK(sh_mpkg_context_close(&context)); sh_package_bundle_release(bundle);
        free(zip); if (map) HeapFree(GetProcessHeap(), 0, map);
    }
    /* A forged original-tree segment cannot smuggle a seed into conversion. */
    {
        size_t length, map_length;
        unsigned char *zip = sh_package_bundle_read(originals[0].original, &length, error, sizeof(error));
        key[0][16] = key[0][16] == 'a' ? 'b' : 'a';
        char *map = zip ? sh_mpkg_embed(EMPTY_MAP, strlen(EMPTY_MAP), key[0], zip, length,
            &map_length, error, sizeof(error)) : NULL;
        CHECK(map && !sh_mpkg_context_open(root, map, map_length, error, sizeof(error)));
        CHECK(strstr(error, "complete delivery identity"));
        free(zip); if (map) HeapFree(GetProcessHeap(), 0, map);
    }
done:
    CHECK(sh_mpkg_context_close(&context));
    for (size_t v = 0; v < 2; v++) sh_package_bundle_release(originals[v].original);
    HeapFree(GetProcessHeap(), 0, combined);
}

/* ==================================================================== */
/* historical writers: beta 6-15 carriers through today's reader        */
/* ==================================================================== */

/* Installed-catalog stand-in for the legacy manifest: the campaign model it
 * imports is supplied and the stock-identical material already ships. */
static int g_hist_missing;
static int hist_catalog(void *context, const char *type, const char *name, const char *path,
    unsigned char **body, size_t *length, char *error, size_t capacity)
{
    static const char text[] = "campaign body read from the installed catalog";
    (void)context; (void)path;
    if (!type) return 0;
    if (!strcmp(type, "material") && !strcmp(name, "stock_shared")) return 2;
    if (g_hist_missing || strcmp(type, "model") || strcmp(name, "old_boss_body")) {
        snprintf(error, capacity, "campaign %s '%s' is not in the installed catalogs", type, name);
        return 0;
    }
    if (!(*body = malloc(sizeof(text) - 1))) return 0;
    memcpy(*body, text, sizeof(text) - 1); *length = sizeof(text) - 1;
    return 1;
}

/* The complete-tree key a re-save uses, spelled out independently of package_bundle.c. */
static void hist_key(const unsigned char *payload, size_t length, const char *seed,
    char out[SH_PACKAGE_BUNDLE_KEY_CAP])
{
    unsigned char tree[32];
    char error[256];
    size_t at = 16;
    out[0] = 0;
    if (!sh_package_archive_tree_identity(payload, length, tree, error, sizeof(error))) return;
    memcpy(out, "delivery.legacy.", 16);
    for (size_t i = 0; i < 32; i++, at += 2) snprintf(out + at, 3, "%02x", tree[i]);
    out[at++] = '.';
    for (size_t i = 0; seed[i]; i++, at += 2) snprintf(out + at, 3, "%02x", (unsigned char)seed[i]);
    out[at] = 0;
}

static size_t hist_private_maps(const char *root)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    size_t count = 0;
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\package-cache\\maps\\map-*", root);
    search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return 0;
    do count++; while (FindNextFileA(search, &found));
    FindClose(search); return count;
}

/* The save route: the engine's carrier-free document plus every retained
 * original under its own complete-tree key, embedded as one set as rawmap.c
 * does (sh_mpkg_pack_used reads the same bundle bytes). Returns a
 * process-heap map. */
static char *hist_resave(const char *native, size_t length, const sh_mpkg_context *context, size_t *out_length)
{
    size_t count = 0, made = 0;
    const sh_package_bundle_binding *b = sh_mpkg_context_bundles(context, &count);
    char (*keys)[SH_PACKAGE_BUNDLE_KEY_CAP] = count ? calloc(count, sizeof(*keys)) : NULL;
    sh_mpkg_delivery *items = count ? calloc(count, sizeof(*items)) : NULL;
    char *out = NULL, error[SH_MPKG_ERR_CAP];
    *out_length = 0;
    if (!count) {
        out = HeapAlloc(GetProcessHeap(), 0, length + 1);
        if (out) { memcpy(out, native, length); out[length] = 0; *out_length = length; }
        return out;
    }
    if (!keys || !items) goto done;
    for (; made < count; made++) {
        unsigned char *payload = sh_package_bundle_read(b[made].original, &items[made].length, error, sizeof(error));
        if (!payload) { fprintf(stderr, "historical re-save: %s\n", error); goto done; }
        sh_package_bundle_delivery_key(b[made].original, keys[made]);
        items[made].id = keys[made]; items[made].archive = payload;
    }
    out = sh_mpkg_embed_set(native, length, items, count, out_length, NULL, error, sizeof(error));
    if (!out) fprintf(stderr, "historical re-save: %s\n", error);
done:
    for (size_t i = 0; items && i < made; i++) free((void *)items[i].archive);
    free(items); free(keys); return out;
}

/* Private trees can exceed MAX_PATH below a complete-tree key; read them the
 * way production writes them. Returns bytes read or -1. */
static long hist_read(const char *path, unsigned char *buffer, size_t capacity)
{
    wchar_t *wide = sh_package_source_wide_path(path);
    HANDLE file = wide ? CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    DWORD got = 0;
    free(wide);
    if (file == INVALID_HANDLE_VALUE) return -1;
    if (!ReadFile(file, buffer, (DWORD)capacity, &got, NULL)) got = (DWORD)-1;
    CloseHandle(file);
    return (long)got;
}

/* The derived private view: modern layout and the carrier's identity, authored
 * bytes unchanged, the campaign import materialized and the stock-identical
 * one omitted. Only the retained original keeps the old descriptor. */
static void hist_check_converted(const sh_mpkg_context *context, size_t index, const char *seed)
{
    size_t count = 0;
    const sh_package_bundle_binding *b = sh_mpkg_context_bundles(context, &count);
    char base[1024], path[1024], quoted[SH_PACKAGE_ID_CAP + 2];
    static unsigned char buffer[65536];
    long got;
    sh_json_object descriptor = {0};
    CHECK(index < count); if (index >= count) return;
    _snprintf_s(base, sizeof(base), _TRUNCATE, "%s\\overrides\\%s", sh_mpkg_context_root(context), b[index].source_name);
    join(path, sizeof(path), base, "assets\\generated\\decls\\entitydef\\ai\\conductor\\coop\\snapmap_default.decl");
    got = hist_read(path, buffer, sizeof(buffer));
    CHECK(got == (long)hist_boss_conductor_len && !memcmp(buffer, hist_boss_conductor, hist_boss_conductor_len));
    join(path, sizeof(path), base, "assets\\generated\\model\\old_boss_body.bmodel");
    CHECK(hist_read(path, buffer, sizeof(buffer)) > 0);
    join(path, sizeof(path), base, "assets\\generated\\decls\\material\\stock_shared.decl");
    CHECK(hist_read(path, buffer, sizeof(buffer)) < 0);
    join(path, sizeof(path), base, "package.json");
    got = hist_read(path, buffer, sizeof(buffer) - 1);
    CHECK(got > 0);
    if (got <= 0) return;
    buffer[got] = 0;
    /* The authored string value keeps its UTF-8 bytes in the normalized policy. */
    CHECK(strstr((const char *)buffer, "Old Boss \xe2\x80\x94 authored") != NULL);
    CHECK(sh_json_parse_object((const char *)buffer, (size_t)got, 8, &descriptor));
    {
        const char *id = sh_json_object_get(&descriptor, "id"), *name = sh_json_object_get(&descriptor, "name");
        _snprintf_s(quoted, sizeof(quoted), _TRUNCATE, "\"%s\"", seed);
        CHECK(id && !strcmp(id, quoted));
        CHECK(name && !strcmp(name, "\"old-boss\""));
        CHECK(sh_json_object_get(&descriptor, "description") != NULL);
        CHECK(!sh_json_object_get(&descriptor, "contents") && !sh_json_object_get(&descriptor, "schema"));
    }
    sh_json_object_free(&descriptor);
}

/* One historical map: exact carrier, migration, re-save under complete-tree
 * keys, reopen and a byte-identical second save. seeds/payloads follow the
 * map's carrier order. */
static void hist_round(const char *root, const char *map, size_t map_length, const char *base, size_t base_length,
    const char *const *ids, const char *const *seeds, const unsigned char *const *payloads, const size_t *lengths,
    size_t count)
{
    char error[SH_MPKG_ERR_CAP], keys[4][SH_PACKAGE_BUNDLE_KEY_CAP];
    unsigned char trees[4][32];
    sh_mpkg_decl decls[4];
    sh_mpkg_context *context = NULL, *reopened = NULL;
    char *stripped = NULL, *saved = NULL, *saved_native = NULL, *again = NULL;
    size_t stripped_length = 0, saved_length = 0, saved_native_length = 0, again_length = 0, n, i;
    CHECK(count <= 4); if (count > 4) return;
    /* The historical writers' carriers read back exactly. */
    CHECK(sh_mpkg_scan(map, map_length, decls, 4) == count);
    for (i = 0; i < count; i++) {
        unsigned char *payload;
        size_t length = 0;
        char digest[SH_MPKG_DIGEST_CHARS + 1];
        sh_mpkg_digest16(payloads[i], lengths[i], digest);
        CHECK(!strcmp(decls[i].id, ids[i]) && !strcmp(decls[i].digest, digest) && decls[i].complete);
        payload = sh_mpkg_extract(map, map_length, ids[i], &length, error, sizeof(error));
        CHECK(payload && length == lengths[i] && !memcmp(payload, payloads[i], length));
        if (payload) HeapFree(GetProcessHeap(), 0, payload);
        hist_key(payloads[i], lengths[i], seeds[i], keys[i]);
        CHECK(keys[i][0]);
    }
    /* Only delivery variables leave: native byte strings, the user's string
     * variable and its count come back as the original document. */
    stripped = sh_mpkg_strip(map, map_length, &stripped_length);
    CHECK(stripped && stripped_length == base_length && !memcmp(stripped, base, base_length));
    if (!stripped) return;
    context = sh_mpkg_context_open(root, map, map_length, error, sizeof(error));
    if (!context) fprintf(stderr, "historical writer context: %s\n", error);
    CHECK(context && sh_mpkg_context_count(context) == count);
    if (!context || sh_mpkg_context_count(context) != count) goto done;
    {
        const sh_package_bundle_binding *b = sh_mpkg_context_bundles(context, &n);
        for (i = 0; i < count; i++) {
            char key[SH_PACKAGE_BUNDLE_KEY_CAP];
            size_t length = 0;
            unsigned char *original = sh_package_bundle_read(b[i].original, &length, error, sizeof(error));
            CHECK(original && length == lengths[i] && !memcmp(original, payloads[i], length));
            free(original);
            sh_package_bundle_delivery_key(b[i].original, key);
            CHECK(!strcmp(key, keys[i]));
            /* The compiler sees the derived tree; the original stays the delivery. */
            CHECK(memcmp(b[i].compiler_tree, sh_package_bundle_fingerprint(b[i].original), 32));
            memcpy(trees[i], b[i].compiler_tree, 32);
            if (payloads[i] == hist_boss_payload) hist_check_converted(context, i, seeds[i]);
        }
    }
    saved = hist_resave(stripped, stripped_length, context, &saved_length);
    CHECK(saved != NULL); if (!saved) goto done;
    CHECK(sh_mpkg_scan(saved, saved_length, decls, 4) == count);
    for (i = 0; i < count; i++) {
        unsigned char *payload;
        size_t length = 0;
        CHECK(!strcmp(decls[i].id, keys[i]) && decls[i].complete);
        payload = sh_mpkg_extract(saved, saved_length, keys[i], &length, error, sizeof(error));
        CHECK(payload && length == lengths[i] && !memcmp(payload, payloads[i], length));
        if (payload) HeapFree(GetProcessHeap(), 0, payload);
    }
    saved_native = sh_mpkg_strip(saved, saved_length, &saved_native_length);
    CHECK(saved_native && saved_native_length == base_length && !memcmp(saved_native, base, base_length));
    if (!saved_native) goto done;
    /* Reopen recognizes the complete-tree keys and their migration seeds. */
    reopened = sh_mpkg_context_open(root, saved, saved_length, error, sizeof(error));
    if (!reopened) fprintf(stderr, "historical reopen: %s\n", error);
    CHECK(reopened && sh_mpkg_context_count(reopened) == count);
    if (!reopened || sh_mpkg_context_count(reopened) != count) goto done;
    {
        const sh_package_bundle_binding *b = sh_mpkg_context_bundles(reopened, &n);
        for (i = 0; i < count; i++) {
            char key[SH_PACKAGE_BUNDLE_KEY_CAP];
            size_t length = 0;
            unsigned char *original = sh_package_bundle_read(b[i].original, &length, error, sizeof(error));
            CHECK(original && length == lengths[i] && !memcmp(original, payloads[i], length));
            free(original);
            sh_package_bundle_delivery_key(b[i].original, key);
            CHECK(!strcmp(key, keys[i]) && !memcmp(b[i].compiler_tree, trees[i], 32));
            if (payloads[i] == hist_boss_payload) hist_check_converted(reopened, i, seeds[i]);
        }
    }
    again = hist_resave(saved_native, saved_native_length, reopened, &again_length);
    CHECK(again && again_length == saved_length && !memcmp(again, saved, saved_length));
done:
    CHECK(sh_mpkg_context_close(&context)); CHECK(sh_mpkg_context_close(&reopened));
    if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    if (saved_native) HeapFree(GetProcessHeap(), 0, saved_native);
    if (again) HeapFree(GetProcessHeap(), 0, again);
}

/* The deepest data directory under root whose private map inventory directory
 * (package-cache\maps\map-<32 hex>\overrides) can be created, measured apart
 * from map_package.c with the same ANSI directory calls and a placeholder. */
static size_t hist_inventory_cap(const char *root)
{
    size_t base = strlen(root), cap = 0;
    for (size_t depth = base + 2; depth < MAX_PATH - 20; depth++) {
        static const char *const levels[] = {"", "\\package-cache", "\\package-cache\\maps",
            "\\package-cache\\maps\\map-00000000000000000000000000000000",
            "\\package-cache\\maps\\map-00000000000000000000000000000000\\overrides"};
        char deep[MAX_PATH], padding[MAX_PATH], path[MAX_PATH];
        int made = 1;
        memset(padding, 'c', depth - base - 1); padding[depth - base - 1] = 0;
        join(deep, sizeof(deep), root, padding);
        for (size_t l = 0; made && l < sizeof(levels) / sizeof(*levels); l++)
            made = snprintf(path, sizeof(path), "%s%s", deep, levels[l]) < (int)sizeof(path) && make_dir(path);
        remove_tree(deep);
        if (!made) break;
        cap = depth;
    }
    return cap;
}

/* A copy of the single-package map with one change, for refusal controls. */
static char *hist_variant(const char *find, const char *replace_with, size_t *length)
{
    char *copy = HeapAlloc(GetProcessHeap(), 0, hist_map_one_len + 1), *at;
    *length = hist_map_one_len;
    if (!copy) return NULL;
    memcpy(copy, hist_map_one, hist_map_one_len + 1);
    at = strstr(copy, find);
    CHECK(at && strlen(find) == strlen(replace_with));
    if (at) memcpy(at, replace_with, strlen(replace_with));
    return copy;
}

/* W3: the beta14/15 writers keyed an already-modern package by its root
 * descriptor id, which the reader matches against the root descriptor (the
 * nested component's precedes it in the archive). The archive stays its own
 * compiler tree, and a re-save keys the same bytes by their complete tree,
 * exactly as the beta16 writer did for the same package. */
static void hist_w3_round(const char *root)
{
    char error[SH_MPKG_ERR_CAP], key[SH_PACKAGE_BUNDLE_KEY_CAP], digest[SH_MPKG_DIGEST_CHARS + 1];
    unsigned char tree[32], *payload;
    sh_mpkg_decl decls[2];
    sh_mpkg_context *context = NULL, *reopened = NULL;
    char *stripped = NULL, *saved = NULL, *saved_native = NULL, *again = NULL;
    size_t stripped_length = 0, saved_length = 0, saved_native_length = 0, again_length = 0, length = 0;
    sh_mpkg_digest16(hist_w3_payload, hist_w3_payload_len, digest);
    CHECK(sh_mpkg_scan(hist_map_w3, hist_map_w3_len, decls, 2) == 1);
    CHECK(!strcmp(decls[0].id, HIST_W3_ID) && !strcmp(decls[0].digest, digest) && decls[0].complete);
    payload = sh_mpkg_extract(hist_map_w3, hist_map_w3_len, HIST_W3_ID, &length, error, sizeof(error));
    CHECK(payload && length == hist_w3_payload_len && !memcmp(payload, hist_w3_payload, length));
    if (payload) HeapFree(GetProcessHeap(), 0, payload);
    /* The complete-tree key, spelled out independently of package_bundle.c. */
    CHECK(sh_package_archive_tree_identity(hist_w3_payload, hist_w3_payload_len, tree, error, sizeof(error)));
    memcpy(key, "delivery.", 9);
    for (size_t i = 0; i < 32; i++) snprintf(key + 9 + i * 2, 3, "%02x", tree[i]);
    /* Only the carrier leaves: the native document is the writers' UTF-8 base. */
    stripped = sh_mpkg_strip(hist_map_w3, hist_map_w3_len, &stripped_length);
    CHECK(stripped && stripped_length == hist_base_w3_len && !memcmp(stripped, hist_base_w3, hist_base_w3_len));
    if (!stripped) goto done;
    for (size_t pass = 0; pass < 2; pass++) {
        sh_mpkg_context **slot = pass ? &reopened : &context;
        size_t count = 0, bytes = 0;
        const sh_package_bundle_binding *b;
        char held[SH_PACKAGE_BUNDLE_KEY_CAP];
        unsigned char *original;
        *slot = sh_mpkg_context_open(root, pass ? saved : hist_map_w3, pass ? saved_length : hist_map_w3_len,
            error, sizeof(error));
        if (!*slot) fprintf(stderr, "W3 %s: %s\n", pass ? "reopen" : "context", error);
        CHECK(*slot && sh_mpkg_context_count(*slot) == 1);
        if (!*slot || sh_mpkg_context_count(*slot) != 1) goto done;
        b = sh_mpkg_context_bundles(*slot, &count);
        CHECK(!strcmp(b[0].source_name, "0") && !strcmp(sh_mpkg_context_carrier(*slot, 0), pass ? key : HIST_W3_ID));
        original = sh_package_bundle_read(b[0].original, &bytes, error, sizeof(error));
        CHECK(original && bytes == hist_w3_payload_len && !memcmp(original, hist_w3_payload, bytes));
        free(original);
        sh_package_bundle_delivery_key(b[0].original, held);
        CHECK(!strcmp(held, key));
        /* Already modern: no migration; the compiler sees the archive's own tree. */
        CHECK(!memcmp(b[0].compiler_tree, tree, 32) && !memcmp(sh_package_bundle_fingerprint(b[0].original), tree, 32));
        if (!pass) {
            /* The modern re-save is the beta16 writer's map for the same package, byte for byte. */
            saved = hist_resave(stripped, stripped_length, context, &saved_length);
            CHECK(saved && saved_length == hist_map_w4_reference_len && !memcmp(saved, hist_map_w4_reference, saved_length));
            if (!saved) goto done;
        }
    }
    saved_native = sh_mpkg_strip(saved, saved_length, &saved_native_length);
    CHECK(saved_native && saved_native_length == hist_base_w3_len && !memcmp(saved_native, hist_base_w3, hist_base_w3_len));
    again = saved_native ? hist_resave(saved_native, saved_native_length, reopened, &again_length) : NULL;
    CHECK(again && again_length == saved_length && !memcmp(again, saved, saved_length));
done:
    CHECK(sh_mpkg_context_close(&context)); CHECK(sh_mpkg_context_close(&reopened));
    if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    if (saved_native) HeapFree(GetProcessHeap(), 0, saved_native);
    if (again) HeapFree(GetProcessHeap(), 0, again);
}

static void test_historical_writer_maps(const char *root)
{
    const char *one_ids[] = {HIST_BOSS_ID}, *two_ids[] = {HIST_BOSS_ID, HIST_RUNE_ID};
    const unsigned char *one_payloads[] = {hist_boss_payload}, *two_payloads[] = {hist_boss_payload, hist_rune_payload};
    const size_t one_lengths[] = {hist_boss_payload_len}, two_lengths[] = {hist_boss_payload_len, hist_rune_payload_len};
    char odd[SH_MPKG_ID_CAP], digest[SH_MPKG_DIGEST_CHARS + 1], error[SH_MPKG_ERR_CAP], *map;
    const char *odd_ids[1] = {odd};
    size_t maps = hist_private_maps(root), length;
    sh_mpkg_context *usable = NULL, *context;

    sh_mpkg_set_legacy_reader(hist_catalog, NULL); g_hist_missing = 0;
    hist_round(root, hist_map_one, hist_map_one_len, hist_base_compact, hist_base_compact_len,
        one_ids, one_ids, one_payloads, one_lengths, 1);
    hist_round(root, hist_map_two, hist_map_two_len, hist_base_compact, hist_base_compact_len,
        two_ids, two_ids, two_payloads, two_lengths, 2);
    hist_round(root, hist_map_pretty, hist_map_pretty_len, hist_base_pretty, hist_base_pretty_len,
        one_ids, one_ids, one_payloads, one_lengths, 1);
    /* Beta 13 folder ids allowed leading/trailing '-' and '_'; the carrier
     * gets one stable modern identity without manual selection. */
    sh_mpkg_digest16((const unsigned char *)HIST_ODD_ID, strlen(HIST_ODD_ID), digest);
    snprintf(odd, sizeof(odd), "legacy.%s", digest);
    hist_round(root, hist_map_odd, hist_map_odd_len, hist_base_compact, hist_base_compact_len,
        odd_ids, odd_ids, one_payloads, one_lengths, 1);

    /* Mixed historical and current deliveries keep their own originals. */
    {
        static const char marker[] = "{\"id\":\"modern-pack\",\"name\":\"Modern pack\"}";
        static const char decl[] = "{ health = 1; }";
        sh_package_archive_file members[] = {
            {"package.json", (unsigned char *)marker, sizeof(marker) - 1, 0},
            {"assets/generated/decls/entitydef/modern.decl", (unsigned char *)decl, sizeof(decl) - 1, 0}
        };
        sh_package_archive_files files = {members, 2};
        unsigned char tree[32], *modern = sh_package_archive_write(&files, &length, error, sizeof(error));
        char modern_key[SH_PACKAGE_ID_CAP], *mixed = NULL;
        size_t mixed_length = 0;
        CHECK(modern && sh_package_archive_tree_identity(modern, length, tree, error, sizeof(error)));
        if (modern) {
            sh_package_archive_delivery_id(tree, modern_key);
            mixed = sh_mpkg_embed(hist_map_one, hist_map_one_len, modern_key, modern, length,
                &mixed_length, error, sizeof(error));
            CHECK(mixed != NULL);
        }
        context = mixed ? sh_mpkg_context_open(root, mixed, mixed_length, error, sizeof(error)) : NULL;
        if (mixed && !context) fprintf(stderr, "historical mixed context: %s\n", error);
        CHECK(context && sh_mpkg_context_count(context) == 2);
        if (context && sh_mpkg_context_count(context) == 2) {
            size_t count, i, found = 0;
            const sh_package_bundle_binding *b = sh_mpkg_context_bundles(context, &count);
            char expected[SH_PACKAGE_BUNDLE_KEY_CAP];
            hist_key(hist_boss_payload, hist_boss_payload_len, HIST_BOSS_ID, expected);
            for (i = 0; i < count; i++) {
                char key[SH_PACKAGE_BUNDLE_KEY_CAP];
                size_t bytes = 0;
                unsigned char *original = sh_package_bundle_read(b[i].original, &bytes, error, sizeof(error));
                sh_package_bundle_delivery_key(b[i].original, key);
                if (!strcmp(key, expected)) {
                    CHECK(original && bytes == hist_boss_payload_len && !memcmp(original, hist_boss_payload, bytes)); found |= 1;
                } else if (!strcmp(key, modern_key)) {
                    CHECK(original && bytes == length && !memcmp(original, modern, bytes)); found |= 2;
                    CHECK(!memcmp(b[i].compiler_tree, sh_package_bundle_fingerprint(b[i].original), 32));
                }
                free(original);
            }
            CHECK(found == 3);
        }
        CHECK(sh_mpkg_context_close(&context));
        free(modern); if (mixed) HeapFree(GetProcessHeap(), 0, mixed);
    }

    /* Refusals keep their cause and never leave a partial private source; an
     * earlier usable context is untouched and recovery needs no repair. */
    usable = sh_mpkg_context_open(root, hist_map_one, hist_map_one_len, error, sizeof(error));
    CHECK(usable != NULL);
    {
        char find[64], damaged[64];
        const char *shard = strstr(hist_map_one, "smpkg." HIST_BOSS_ID ".1.");
        const char *value = shard ? strstr(shard, "\"initialValue\":\"") : NULL;
        CHECK(value != NULL);
        if (value) {
            value += 16;
            memcpy(find, value, 24); find[24] = 0; strcpy_s(damaged, sizeof(damaged), find);
            damaged[10] = damaged[10] == 'A' ? 'B' : 'A';
            map = hist_variant(find, damaged, &length);
            CHECK(map && !sh_mpkg_context_open(root, map, length, error, sizeof(error)));
            CHECK(strstr(error, "failed its digest") != NULL);
            if (map) HeapFree(GetProcessHeap(), 0, map);
            damaged[10] = '*';
            map = hist_variant(find, damaged, &length);
            CHECK(map && !sh_mpkg_context_open(root, map, length, error, sizeof(error)));
            CHECK(strstr(error, "unreadable shard") != NULL && strstr(error, HIST_BOSS_ID) != NULL);
            if (map) HeapFree(GetProcessHeap(), 0, map);
        }
    }
    map = hist_variant("smpkg." HIST_BOSS_ID ".1.", "smpkg.old-bosz.1.", &length);
    CHECK(map && !sh_mpkg_context_open(root, map, length, error, sizeof(error)));
    CHECK(strstr(error, "incomplete") != NULL);
    if (map) HeapFree(GetProcessHeap(), 0, map);
    /* Environment: no catalog, or a campaign resource it lacks. The carrier is intact. */
    sh_mpkg_set_legacy_reader(NULL, NULL);
    CHECK(!sh_mpkg_context_open(root, hist_map_one, hist_map_one_len, error, sizeof(error)));
    CHECK(strstr(error, "requires the installed resource catalog") != NULL);
    sh_mpkg_set_legacy_reader(hist_catalog, NULL); g_hist_missing = 1;
    CHECK(!sh_mpkg_context_open(root, hist_map_one, hist_map_one_len, error, sizeof(error)));
    CHECK(strstr(error, "is not in the installed catalogs") != NULL);
    g_hist_missing = 0;
    CHECK(hist_private_maps(root) == maps + 1);
    if (usable) {
        size_t count, bytes = 0;
        const sh_package_bundle_binding *b = sh_mpkg_context_bundles(usable, &count);
        unsigned char *original = sh_package_bundle_read(b[0].original, &bytes, error, sizeof(error));
        CHECK(original && bytes == hist_boss_payload_len && !memcmp(original, hist_boss_payload, bytes));
        CHECK(dir_exists(sh_mpkg_context_root(usable)));
        free(original);
    }
    CHECK(sh_mpkg_context_close(&usable));
    context = sh_mpkg_context_open(root, hist_map_one, hist_map_one_len, error, sizeof(error));
    CHECK(context && sh_mpkg_context_count(context) == 1);
    CHECK(sh_mpkg_context_close(&context));
    CHECK(hist_private_maps(root) == maps);

    /* Private owner folders are context-local ordinals, so a carrier's name
     * needs no more room than the private inventory directory itself: the
     * historical map, its modern re-save and carriers with 1- and 60-character
     * ids all open at the deepest data directory where that directory can be
     * created, the first two also install and cancel there, and all are refused
     * one level deeper. Every first open the carrier-id naming supported stays
     * supported, and a re-save reopens wherever its original opened. */
    {
        char deep[MAX_PATH], padding[MAX_PATH], key[SH_PACKAGE_BUNDLE_KEY_CAP], long_id[61];
        char *native = NULL, *saved = NULL, *shortest = NULL, *longest = NULL;
        size_t native_length = 0, saved_length = 0, shortest_length = 0, longest_length = 0, count;
        size_t base = strlen(root), cap = hist_inventory_cap(root);
        memset(long_id, 'l', 60); long_id[60] = 0;
        context = sh_mpkg_context_open(root, hist_map_one, hist_map_one_len, error, sizeof(error));
        native = sh_mpkg_strip(hist_map_one, hist_map_one_len, &native_length);
        saved = context && native ? hist_resave(native, native_length, context, &saved_length) : NULL;
        CHECK(saved != NULL);
        CHECK(sh_mpkg_context_close(&context));
        shortest = sh_mpkg_embed(hist_base_compact, hist_base_compact_len, "a", hist_boss_payload, hist_boss_payload_len,
            &shortest_length, error, sizeof(error));
        longest = sh_mpkg_embed(hist_base_compact, hist_base_compact_len, long_id, hist_boss_payload, hist_boss_payload_len,
            &longest_length, error, sizeof(error));
        CHECK(shortest && longest);
        hist_key(hist_boss_payload, hist_boss_payload_len, HIST_BOSS_ID, key);
        CHECK(cap > base + 2 && cap + 1 < MAX_PATH - 20);
        printf("historical private owner boundary: every map opens at data root depth %zu, the deepest where the private "
            "inventory directory can be created; all are refused at %zu\n", cap, cap + 1);
        for (size_t step = 0; saved && shortest && longest && cap > base + 2 && step < 2; step++) {
            const char *maps[4] = {hist_map_one, saved, shortest, longest};
            const size_t lengths[4] = {hist_map_one_len, saved_length, shortest_length, longest_length};
            const char *carriers[4] = {HIST_BOSS_ID, key, "a", long_id};
            memset(padding, 'd', cap + step - base - 1); padding[cap + step - base - 1] = 0;
            join(deep, sizeof(deep), root, padding);
            CHECK(make_dir(deep));
            for (size_t which = 0; which < 4; which++) {
                context = sh_mpkg_context_open(deep, maps[which], lengths[which], error, sizeof(error));
                if (step) {
                    CHECK(!context && hist_private_maps(deep) == 0);
                    CHECK(sh_mpkg_context_close(&context));
                    continue;
                }
                if (!context) fprintf(stderr, "deepest historical open %zu: %s\n", which, error);
                CHECK(context && sh_mpkg_context_count(context) == 1);
                if (context) {
                    const sh_package_bundle_binding *b = sh_mpkg_context_bundles(context, &count);
                    char reopened[SH_PACKAGE_BUNDLE_KEY_CAP];
                    size_t bytes = 0;
                    unsigned char *original = sh_package_bundle_read(b[0].original, &bytes, error, sizeof(error));
                    CHECK(!strcmp(b[0].source_name, "0") && !strcmp(sh_mpkg_context_carrier(context, 0), carriers[which]));
                    sh_package_bundle_delivery_key(b[0].original, reopened);
                    CHECK(which > 1 || !strcmp(reopened, key));
                    CHECK(original && bytes == hist_boss_payload_len && !memcmp(original, hist_boss_payload, bytes));
                    free(original);
                }
                if (context && which < 2) {
                    /* Missing-content installation finds the owner, installs it
                     * completely and cancels without leaving the package behind. */
                    char overrides_dir[MAX_PATH], package[MAX_PATH];
                    sh_package_sources *sources = sh_package_sources_scan(sh_mpkg_context_root(context), error, sizeof(error));
                    sh_package_compilation candidate = {0};
                    sh_package_owners selected = {0};
                    join(overrides_dir, sizeof(overrides_dir), deep, "overrides");
                    CHECK(make_dir(overrides_dir) && sources && sources->package_count == 1);
                    if (sources) {
                        candidate.sources = sources; CHECK(sh_package_owners_add(&selected, 0));
                        sh_mpkg_test_reset(); sh_mpkg_boot_capture(deep);
                        g_registration_ready = g_compile_ready = 1; map_install_calls = 0;
                        sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
                        CHECK(sh_mpkg_request_map_install(maps[which], lengths[which], &candidate, &selected,
                            map_install_completed, &map_install_calls, error, sizeof(error)));
                        CHECK(map_install_calls == 1 && map_install_outcome == 1);
                        CHECK(installed_count(deep) == 1 && installed_package(deep, HIST_BOSS_ID, package));
                        CHECK(sh_mpkg_activation_cancel() && !installed_count(deep));
                        sh_mpkg_test_reset();
                    }
                    sh_package_owners_free(&selected); sh_package_sources_free(sources);
                }
                CHECK(sh_mpkg_context_close(&context));
            }
            remove_tree(deep);
        }
        if (native) HeapFree(GetProcessHeap(), 0, native);
        if (saved) HeapFree(GetProcessHeap(), 0, saved);
        if (shortest) HeapFree(GetProcessHeap(), 0, shortest);
        if (longest) HeapFree(GetProcessHeap(), 0, longest);
    }

    /* Authored historical ids that spell other carriers' ordinals ("1" before
     * "0"), two carriers sharing one migration seed (an original and another
     * archive's re-save key), and twelve carriers with two-digit ordinals: each
     * owner installs its own package. Installation resolves an owner folder
     * through its context's carrier record, never folder spelling or an id. */
    {
        static const char *const rune_member = "assets\\generated\\decls\\actormodifier\\actormodifier\\demon\\old_rune.decl";
        static const char *const boss_member = "assets\\generated\\decls\\entitydef\\ai\\conductor\\coop\\snapmap_default.decl";
        char client[MAX_PATH], overrides_dir[MAX_PATH], seeded[SH_PACKAGE_BUNDLE_KEY_CAP];
        char *maps[3] = {NULL, NULL, NULL};
        size_t lengths[3] = {0, 0, 0};
        hist_key(hist_boss_payload, hist_boss_payload_len, HIST_BOSS_ID, seeded);
        join(client, sizeof(client), root, "lookalike-client"); join(overrides_dir, sizeof(overrides_dir), client, "overrides");
        CHECK(make_dir(client) && make_dir(overrides_dir));
        for (size_t m = 0; m < 3; m++) {
            const char *ids[12];
            const unsigned char *payloads[12];
            size_t sizes[12], n = m == 2 ? 12 : 2;
            char numbers[12][4];
            for (size_t k = 0; k < n; k++) {
                snprintf(numbers[k], sizeof(numbers[k]), "%zu", n - 1 - k);
                ids[k] = m == 1 ? (k ? seeded : HIST_BOSS_ID) : numbers[k];
                payloads[k] = m != 2 && k ? hist_boss_payload : hist_rune_payload;
                sizes[k] = m != 2 && k ? hist_boss_payload_len : hist_rune_payload_len;
            }
            maps[m] = HeapAlloc(GetProcessHeap(), 0, hist_base_compact_len + 1);
            if (!maps[m]) continue;
            memcpy(maps[m], hist_base_compact, hist_base_compact_len + 1); lengths[m] = hist_base_compact_len;
            for (size_t k = 0; maps[m] && k < n; k++) {
                size_t next_length = 0;
                char *next = sh_mpkg_embed(maps[m], lengths[m], ids[k], payloads[k], sizes[k], &next_length, error, sizeof(error));
                CHECK(next != NULL); HeapFree(GetProcessHeap(), 0, maps[m]); maps[m] = next; lengths[m] = next_length;
            }
            context = maps[m] ? sh_mpkg_context_open(client, maps[m], lengths[m], error, sizeof(error)) : NULL;
            if (maps[m] && !context) fprintf(stderr, "lookalike context %zu: %s\n", m, error);
            CHECK(context && sh_mpkg_context_count(context) == n);
            if (!context) continue;
            {
                sh_package_sources *sources = sh_package_sources_scan(sh_mpkg_context_root(context), error, sizeof(error));
                size_t held = 0;
                const sh_package_bundle_binding *b = sh_mpkg_context_bundles(context, &held);
                CHECK(sources && sources->package_count == n);
                for (size_t k = 0; k < held; k++) {
                    char ordinal[8];
                    snprintf(ordinal, sizeof(ordinal), "%zu", k);
                    CHECK(!strcmp(b[k].source_name, ordinal) && !strcmp(sh_mpkg_context_carrier(context, k), ids[k]));
                }
                for (size_t owner = 0; sources && owner < sources->package_count; owner++) {
                    const char *leaf = strrchr(sources->packages[owner].root, '\\');
                    size_t k;
                    char package[MAX_PATH], path[MAX_PATH];
                    unsigned char probe[16];
                    sh_package_compilation candidate = {0};
                    sh_package_owners selected = {0};
                    leaf = leaf ? leaf + 1 : sources->packages[owner].root;
                    for (k = 0; k < held && strcmp(b[k].source_name, leaf); k++) {}
                    CHECK(k < held);
                    if (k >= held || (m == 2 && strcmp(leaf, "0") && strcmp(leaf, "1") && strcmp(leaf, "10"))) continue;
                    candidate.sources = sources; CHECK(sh_package_owners_add(&selected, owner));
                    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
                    g_registration_ready = g_compile_ready = 1; map_install_calls = 0;
                    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
                    CHECK(sh_mpkg_request_map_install(maps[m], lengths[m], &candidate, &selected,
                        map_install_completed, &map_install_calls, error, sizeof(error)));
                    CHECK(map_install_calls == 1 && map_install_outcome == 1);
                    CHECK(installed_count(client) == 1 && installed_package(client, m == 1 ? HIST_BOSS_ID : ids[k], package));
                    join(path, sizeof(path), package, payloads[k] == hist_rune_payload ? rune_member : boss_member);
                    CHECK(package[0] && hist_read(path, probe, sizeof(probe)) > 0);
                    CHECK(sh_mpkg_activation_cancel() && !installed_count(client));
                    sh_package_owners_free(&selected); sh_mpkg_test_reset();
                }
                sh_package_sources_free(sources);
            }
            CHECK(sh_mpkg_context_close(&context));
        }
        for (size_t m = 0; m < 3; m++) if (maps[m]) HeapFree(GetProcessHeap(), 0, maps[m]);
        remove_tree(client);
    }
    sh_mpkg_set_legacy_reader(NULL, NULL);
    /* The W3 carrier needs no legacy catalog. */
    hist_w3_round(root);
}

/* The same route over an authentic retained map, with the installed game's
 * catalogs opened read-only for legacy manifests. Prints one JSON receipt. */
static void hist_sha256(const void *bytes, size_t length, char out[65])
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    unsigned char digest[32] = {0};
    out[0] = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return;
    if (BCryptHash(algorithm, NULL, 0, (PUCHAR)bytes, (ULONG)length, digest, sizeof(digest)) >= 0)
        for (size_t i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
    BCryptCloseAlgorithmProvider(algorithm, 0);
}

static int probe_historical_map(const char *data_root, const char *map_path, const char *doom_base)
{
    FILE *input = NULL;
    __int64 size;
    char *json = NULL, error[SH_MPKG_ERR_CAP] = "", hash[65];
    sh_resource_catalog *catalog = NULL;
    sh_mpkg_context *context = NULL, *reopened = NULL;
    sh_mpkg_decl *decls = NULL;
    char *stripped = NULL, *saved = NULL, *saved_native = NULL, *again = NULL;
    size_t count = 0, i, n, stripped_length = 0, saved_length = 0, saved_native_length = 0, again_length = 0;
    int failures = 0;
    if (fopen_s(&input, map_path, "rb") || !input || _fseeki64(input, 0, SEEK_END) ||
        (size = _ftelli64(input)) <= 0 || (uint64_t)size >= SIZE_MAX ||
        _fseeki64(input, 0, SEEK_SET) || !(json = malloc((size_t)size + 1)) ||
        fread(json, 1, (size_t)size, input) != (size_t)size) { fprintf(stderr, "cannot read %s\n", map_path); goto done; }
    json[size] = 0;
    if (!(catalog = sh_resource_catalog_open(doom_base, error, sizeof(error)))) { fprintf(stderr, "catalog: %s\n", error); goto done; }
    sh_mpkg_set_legacy_reader(sh_resource_catalog_legacy_read, catalog);
    hist_sha256(json, (size_t)size, hash);
    printf("{\"map\":{\"bytes\":%lld,\"sha256\":\"%s\"},\"carriers\":[", size, hash);
    if (!(decls = calloc(64, sizeof(*decls))) || (count = sh_mpkg_scan(json, (size_t)size, decls, 64)) == SIZE_MAX) goto done;
    for (i = 0; i < count; i++) {
        size_t length = 0;
        unsigned char *payload = sh_mpkg_extract(json, (size_t)size, decls[i].id, &length, error, sizeof(error));
        hist_sha256(payload ? payload : (unsigned char *)"", length, hash);
        printf("%s{\"id\":\"%s\",\"digest\":\"%s\",\"total\":%u,\"complete\":%d,\"payload_bytes\":%zu,\"payload_sha256\":\"%s\"}",
            i ? "," : "", decls[i].id, decls[i].digest, decls[i].total, decls[i].complete, length, payload ? hash : "");
        if (!payload) failures++; else HeapFree(GetProcessHeap(), 0, payload);
    }
    stripped = sh_mpkg_strip(json, (size_t)size, &stripped_length);
    if (!stripped) { stripped_length = (size_t)size; stripped = HeapAlloc(GetProcessHeap(), 0, stripped_length + 1);
        if (stripped) memcpy(stripped, json, stripped_length + 1); }
    if (!stripped) goto done;
    hist_sha256(stripped, stripped_length, hash);
    printf("],\"native\":{\"bytes\":%zu,\"sha256\":\"%s\"},\"context\":", stripped_length, hash);
    context = sh_mpkg_context_open(data_root, json, (size_t)size, error, sizeof(error));
    if (!context) { printf("{\"error\":\""); for (const char *c = error; *c; c++) printf(*c == '"' || *c == '\\' ? "\\%c" : "%c", *c); printf("\"}}\n"); failures++; goto done; }
    {
        const sh_package_bundle_binding *b = sh_mpkg_context_bundles(context, &n);
        printf("[");
        for (i = 0; i < n; i++) {
            char key[SH_PACKAGE_BUNDLE_KEY_CAP], tree[65];
            size_t length = 0;
            unsigned char *original = sh_package_bundle_read(b[i].original, &length, error, sizeof(error));
            hist_sha256(original ? original : (unsigned char *)"", length, hash);
            for (size_t j = 0; j < 32; j++) snprintf(tree + j * 2, 3, "%02x", b[i].compiler_tree[j]);
            sh_package_bundle_delivery_key(b[i].original, key);
            printf("%s{\"source\":\"%s\",\"key\":\"%s\",\"original_bytes\":%zu,\"original_sha256\":\"%s\",\"compiler_tree\":\"%s\"}",
                i ? "," : "", b[i].source_name, key, length, hash, tree);
            if (!original) failures++;
            free(original);
        }
        printf("]");
    }
    saved = hist_resave(stripped, stripped_length, context, &saved_length);
    if (!saved) { failures++; goto finish; }
    hist_sha256(saved, saved_length, hash);
    saved_native = sh_mpkg_strip(saved, saved_length, &saved_native_length);
    if (!saved_native && !n) { saved_native_length = saved_length; saved_native = HeapAlloc(GetProcessHeap(), 0, saved_length + 1);
        if (saved_native) memcpy(saved_native, saved, saved_length + 1); }
    printf(",\"resave\":{\"bytes\":%zu,\"sha256\":\"%s\",\"native_unchanged\":%d", saved_length, hash,
        saved_native && saved_native_length == stripped_length && !memcmp(saved_native, stripped, stripped_length));
    if (!saved_native || saved_native_length != stripped_length || memcmp(saved_native, stripped, stripped_length)) failures++;
    reopened = saved_native ? sh_mpkg_context_open(data_root, saved, saved_length, error, sizeof(error)) : NULL;
    if (!reopened) {
        failures++; printf(",\"reopen\":false,\"reopen_error\":\"");
        for (const char *c = error; *c; c++) printf(*c == '"' || *c == '\\' ? "\\%c" : "%c", *c);
        printf("\"}"); goto finish;
    }
    {
        size_t m = 0;
        const sh_package_bundle_binding *a = sh_mpkg_context_bundles(context, &n), *b = sh_mpkg_context_bundles(reopened, &m);
        int same = m == n;
        for (i = 0; same && i < n; i++) {
            char left[SH_PACKAGE_BUNDLE_KEY_CAP], right[SH_PACKAGE_BUNDLE_KEY_CAP];
            size_t ll = 0, rl = 0;
            unsigned char *lo = sh_package_bundle_read(a[i].original, &ll, error, sizeof(error));
            unsigned char *ro = sh_package_bundle_read(b[i].original, &rl, error, sizeof(error));
            sh_package_bundle_delivery_key(a[i].original, left); sh_package_bundle_delivery_key(b[i].original, right);
            same = lo && ro && ll == rl && !memcmp(lo, ro, ll) && !strcmp(left, right) &&
                !memcmp(a[i].compiler_tree, b[i].compiler_tree, 32);
            free(lo); free(ro);
        }
        again = hist_resave(saved_native, saved_native_length, reopened, &again_length);
        printf(",\"reopen_same\":%d,\"second_save_identical\":%d}", same,
            again && again_length == saved_length && !memcmp(again, saved, saved_length));
        if (!same || !again || again_length != saved_length || memcmp(again, saved, saved_length)) failures++;
    }
finish:
    printf(",\"failures\":%d}\n", failures);
done:
    sh_mpkg_set_legacy_reader(NULL, NULL);
    if (!sh_mpkg_context_close(&context) || !sh_mpkg_context_close(&reopened)) failures++;
    sh_resource_catalog_close(catalog);
    if (input) fclose(input);
    if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    if (saved_native) HeapFree(GetProcessHeap(), 0, saved_native);
    if (again) HeapFree(GetProcessHeap(), 0, again);
    free(decls); free(json);
    return json && catalog && !failures ? 0 : 1;
}

/* ------------------------------------------------------------------------ */
/* Exact shared carriers (map_package_share.h)                               */
/* ------------------------------------------------------------------------ */

#define SHARE_MEMBERS 24
#define SHARE_MEMBER_BYTES 3000

/* Incompressible member bytes: the same seed always gives the same file. */
static void share_fill(unsigned char *out, size_t length, unsigned seed)
{
    uint32_t x = seed * 2654435761u + 0x9e3779b9u;
    for (size_t i = 0; i < length; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; out[i] = (unsigned char)(x >> 24); }
}

/* A package of nested binary members assets/nested/deep/fileNN.bin, member i
 * filled from seeds[i]; equal seeds at equal names give equal records. */
static unsigned char *share_archive(const char *id, const unsigned *seeds, size_t *length)
{
    static char names[SHARE_MEMBERS][48];
    char descriptor[160], error[512];
    unsigned char *bodies = malloc(SHARE_MEMBERS * SHARE_MEMBER_BYTES), *out = NULL;
    sh_package_archive_file files[SHARE_MEMBERS + 1];
    *length = 0;
    CHECK(bodies); if (!bodies) return NULL;
    snprintf(descriptor, sizeof(descriptor), "{\"id\":\"%s\",\"name\":\"Shared fixture\"}", id);
    files[0] = (sh_package_archive_file){"package.json", (unsigned char *)descriptor, strlen(descriptor), 0};
    for (size_t i = 0; i < SHARE_MEMBERS; i++) {
        snprintf(names[i], sizeof(names[i]), "assets/nested/deep/file%02u.bin", (unsigned)i);
        share_fill(bodies + i * SHARE_MEMBER_BYTES, SHARE_MEMBER_BYTES, seeds[i]);
        files[i + 1] = (sh_package_archive_file){names[i], bodies + i * SHARE_MEMBER_BYTES, SHARE_MEMBER_BYTES, 0};
    }
    {
        sh_package_archive_files archive = {files, SHARE_MEMBERS + 1};
        out = sh_package_archive_write(&archive, length, error, sizeof(error));
    }
    free(bodies); CHECK(out); return out;
}

/* The complete-tree key a modern carrier uses. */
static void share_key(const unsigned char *archive, size_t length, char out[SH_PACKAGE_BUNDLE_KEY_CAP])
{
    unsigned char tree[32];
    char error[256];
    out[0] = 0;
    CHECK(sh_package_archive_tree_identity(archive, length, tree, error, sizeof(error)));
    sh_package_archive_delivery_id(tree, out);
}

/* The representation before sharing: one carrier per delivery, in order. */
static char *share_classic(const char *base, const sh_mpkg_delivery *items, size_t count, size_t *length)
{
    char *current = NULL, error[SH_MPKG_ERR_CAP];
    size_t current_length = strlen(base);
    for (size_t i = 0; i < count; i++) {
        size_t next_length = 0;
        char *next = sh_mpkg_embed(current ? current : base, current_length, items[i].id, items[i].archive,
            items[i].length, &next_length, error, sizeof(error));
        CHECK(next);
        if (current) HeapFree(GetProcessHeap(), 0, current);
        if (!next) return NULL;
        current = next; current_length = next_length;
    }
    *length = current_length; return current;
}

static char *share_set(const char *base, const sh_mpkg_delivery *items, size_t count, size_t *length)
{
    char error[SH_MPKG_ERR_CAP];
    char *out = sh_mpkg_embed_set(base, strlen(base), items, count, length, NULL, error, sizeof(error));
    if (!out) fprintf(stderr, "shared embed: %s\n", error);
    CHECK(out); return out;
}

/* Every delivery reads back as its exact complete original archive. */
static void share_expect_exact(const char *map, size_t length, const sh_mpkg_delivery *items, size_t count)
{
    char error[SH_MPKG_ERR_CAP];
    for (size_t i = 0; i < count; i++) {
        size_t got = 0;
        unsigned char *archive = sh_mpkg_extract(map, length, items[i].id, &got, error, sizeof(error));
        if (!archive) fprintf(stderr, "shared extract %s: %s\n", items[i].id, error);
        CHECK(archive && got == items[i].length && !memcmp(archive, items[i].archive, got));
        if (archive) HeapFree(GetProcessHeap(), 0, archive);
    }
}

/* The shared and classic encodings of one delivery set open the same private
 * context: carriers, owner folders, retained originals and compiler trees. */
static void share_same_context(const char *root, const char *shared, size_t shared_length,
    const char *classic, size_t classic_length, size_t count)
{
    char error[SH_MPKG_ERR_CAP];
    sh_mpkg_context *a = sh_mpkg_context_open(root, shared, shared_length, error, sizeof(error)), *b;
    if (!a) fprintf(stderr, "shared context: %s\n", error);
    b = sh_mpkg_context_open(root, classic, classic_length, error, sizeof(error));
    CHECK(a && b);
    if (a && b) {
        size_t na = 0, nb = 0;
        const sh_package_bundle_binding *ba = sh_mpkg_context_bundles(a, &na), *bb = sh_mpkg_context_bundles(b, &nb);
        CHECK(na == count && nb == count && sh_mpkg_context_count(a) == count);
        for (size_t i = 0; i < na && i < nb; i++) {
            size_t la = 0, lb = 0;
            unsigned char *xa = sh_package_bundle_read(ba[i].original, &la, error, sizeof(error));
            unsigned char *xb = sh_package_bundle_read(bb[i].original, &lb, error, sizeof(error));
            char ka[SH_PACKAGE_BUNDLE_KEY_CAP], kb[SH_PACKAGE_BUNDLE_KEY_CAP];
            sh_package_bundle_delivery_key(ba[i].original, ka); sh_package_bundle_delivery_key(bb[i].original, kb);
            CHECK(!strcmp(sh_mpkg_context_carrier(a, i), sh_mpkg_context_carrier(b, i)));
            CHECK(!strcmp(ba[i].source_name, bb[i].source_name) && !memcmp(ba[i].compiler_tree, bb[i].compiler_tree, 32));
            CHECK(!memcmp(sh_package_bundle_fingerprint(ba[i].original), sh_package_bundle_fingerprint(bb[i].original), 32));
            CHECK(!strcmp(ka, kb) && xa && xb && la == lb && !memcmp(xa, xb, la));
            free(xa); free(xb);
        }
    }
    if (a) CHECK(sh_mpkg_context_close(&a));
    if (b) CHECK(sh_mpkg_context_close(&b));
}

/* Base archives a direct decode may read, by key. */
typedef struct share_bases {
    const char *keys[3];
    const unsigned char *bytes[3];
    size_t lengths[3], count;
    int calls;
} share_bases;
static unsigned char *share_resolve(void *context, const char *key, size_t *length, char *error, size_t capacity)
{
    share_bases *b = (share_bases *)context;
    b->calls++;
    for (size_t i = 0; i < b->count; i++) if (!strcmp(b->keys[i], key)) {
        unsigned char *out = HeapAlloc(GetProcessHeap(), 0, b->lengths[i]);
        if (!out) return NULL;
        memcpy(out, b->bytes[i], b->lengths[i]); *length = b->lengths[i]; return out;
    }
    snprintf(error, capacity, "not present"); return NULL;
}

/* A delta decode that must refuse, with nothing returned and the reason. */
static void share_refuses(const unsigned char *delta, size_t length, const char *self, share_bases *bases,
    const char *reason)
{
    char error[SH_MPKG_ERR_CAP] = "";
    size_t got = 7;
    unsigned char *out = sh_mpkg_delta_decode(delta, length, self, share_resolve, bases, SH_MPKG_MAX_PAYLOAD,
        &got, error, sizeof(error));
    if (out || got || (reason && !strstr(error, reason)))
        fprintf(stderr, "delta refusal expected (%s), got '%s'\n", reason ? reason : "any", error);
    CHECK(!out && !got && error[0] && (!reason || strstr(error, reason)));
    if (out) HeapFree(GetProcessHeap(), 0, out);
}

static void share_mutate(const unsigned char *delta, size_t length, size_t at, unsigned char value,
    const char *self, share_bases *bases, const char *reason)
{
    unsigned char *copy = malloc(length);
    CHECK(copy); if (!copy) return;
    memcpy(copy, delta, length); copy[at] = value;
    share_refuses(copy, length, self, bases, reason);
    free(copy);
}

static void share_put64(unsigned char *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i)); }

/* Map-level refusals: the delivery is refused and no private context opens. */
static void share_map_refuses(const char *root, const char *map, size_t length, const char *id, const char *reason)
{
    char error[SH_MPKG_ERR_CAP] = "";
    size_t got = 0, before = hist_private_maps(root);
    unsigned char *archive = sh_mpkg_extract(map, length, id, &got, error, sizeof(error));
    sh_mpkg_context *context;
    if (archive || (reason && !strstr(error, reason))) fprintf(stderr, "map refusal expected (%s), got '%s'\n", reason, error);
    CHECK(!archive && (!reason || strstr(error, reason)));
    if (archive) HeapFree(GetProcessHeap(), 0, archive);
    context = sh_mpkg_context_open(root, map, length, error, sizeof(error));
    CHECK(!context && error[0]);
    if (context) CHECK(sh_mpkg_context_close(&context));
    sh_mpkg_context_collect();
    CHECK(hist_private_maps(root) == before);
}

static void test_shared_carriers(const char *root)
{
    unsigned seeds_a[SHARE_MEMBERS], seeds_b[SHARE_MEMBERS], seeds_c[SHARE_MEMBERS], seeds_d[SHARE_MEMBERS],
        seeds_e[SHARE_MEMBERS];
    unsigned char *a, *b, *c, *d, *e;
    size_t la, lb, lc, ld, le, length = 0, classic_length = 0, again_length = 0;
    char ka[SH_PACKAGE_BUNDLE_KEY_CAP], kb[SH_PACKAGE_BUNDLE_KEY_CAP], kc[SH_PACKAGE_BUNDLE_KEY_CAP],
        kd[SH_PACKAGE_BUNDLE_KEY_CAP], ke[SH_PACKAGE_BUNDLE_KEY_CAP], error[SH_MPKG_ERR_CAP];
    char *map = NULL, *classic = NULL, *again = NULL;
    sh_mpkg_decl decls[8];
    for (unsigned i = 0; i < SHARE_MEMBERS; i++) {
        seeds_a[i] = 100 + i; seeds_b[i] = 100 + i; seeds_c[i] = 500 + i; seeds_d[i] = 700 + i;
        seeds_e[i] = i < SHARE_MEMBERS / 2 ? 100 + i : 500 + i;
    }
    seeds_b[5] = 999;          /* same name, different bytes */
    seeds_d[0] = seeds_a[0];   /* one equal record: too little to save a shard */
    a = share_archive("shared-fixture", seeds_a, &la); b = share_archive("shared-fixture", seeds_b, &lb);
    c = share_archive("other-fixture", seeds_c, &lc); d = share_archive("third-fixture", seeds_d, &ld);
    e = share_archive("mixed-fixture", seeds_e, &le);
    if (!a || !b || !c || !d || !e) goto done;
    share_key(a, la, ka); share_key(b, lb, kb); share_key(c, lc, kc); share_key(d, ld, kd); share_key(e, le, ke);
    CHECK(strcmp(ka, kb) != 0);

    /* Two variants of one package: the later one stores only what differs,
     * each reads back as its own exact archive, and saving is deterministic. */
    {
        sh_mpkg_delivery pair[2] = {{ka, a, la}, {kb, b, lb}};
        map = share_set(ONE_VAR_MAP, pair, 2, &length);
        classic = share_classic(ONE_VAR_MAP, pair, 2, &classic_length);
        if (!map || !classic) goto done;
        CHECK(length + SH_MPKG_SHARD_CHARS < classic_length);
        {
            /* The engine reads allocCount[4]: it counts every string variable, shared carriers included. */
            int vars = 0;
            for (const char *at = map; (at = strstr(at, "\"~type\":\"snapVarString_t\"")) != NULL; at++) vars++;
            CHECK(json_well_formed(map, length) && alloc_count_string(map) == vars && vars > 2);
        }
        CHECK(sh_mpkg_scan(map, length, decls, 8) == 2 && !strcmp(decls[0].id, ka) && !strcmp(decls[1].id, kb) &&
            decls[0].complete && decls[1].complete);
        CHECK(strstr(map, "\"score\"") != NULL);
        share_expect_exact(map, length, pair, 2);
        again = share_set(ONE_VAR_MAP, pair, 2, &again_length);
        CHECK(again && again_length == length && !memcmp(again, map, length));
        if (again) { HeapFree(GetProcessHeap(), 0, again); again = NULL; }
        share_same_context(root, map, length, classic, classic_length, 2);
        /* Saving from the reopened private context gives the same map again. */
        {
            char out[SH_MPKG_ERR_CAP];
            sh_mpkg_context *context = sh_mpkg_context_open(root, map, length, out, sizeof(out));
            size_t stripped_length = 0, resaved_length = 0;
            char *stripped = sh_mpkg_strip(map, length, &stripped_length), *resaved = NULL;
            CHECK(context && stripped && sh_mpkg_scan(stripped, stripped_length, decls, 8) == 0);
            if (context && stripped) resaved = hist_resave(stripped, stripped_length, context, &resaved_length);
            CHECK(resaved && resaved_length == length && !memcmp(resaved, map, length));
            if (resaved) HeapFree(GetProcessHeap(), 0, resaved);
            if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
            if (context) CHECK(sh_mpkg_context_close(&context));
        }
        /* Stripping removes every delivery, the shared one included. */
        check_strip(map, length, 1);
        HeapFree(GetProcessHeap(), 0, map); HeapFree(GetProcessHeap(), 0, classic); map = classic = NULL;
    }

    /* Nothing shared, or too little to save a shard: the classic carriers, byte for byte. */
    {
        sh_mpkg_delivery unrelated[2] = {{ka, a, la}, {kc, c, lc}}, little[2] = {{ka, a, la}, {kd, d, ld}};
        for (int round = 0; round < 2; round++) {
            const sh_mpkg_delivery *items = round ? little : unrelated;
            map = share_set(EMPTY_MAP, items, 2, &length);
            classic = share_classic(EMPTY_MAP, items, 2, &classic_length);
            CHECK(map && classic && length == classic_length && !memcmp(map, classic, length));
            if (map) share_expect_exact(map, length, items, 2);
            if (map) HeapFree(GetProcessHeap(), 0, map);
            if (classic) HeapFree(GetProcessHeap(), 0, classic);
            map = classic = NULL;
        }
    }

    /* A delivery sharing with two earlier ones reads both; a delta never reads a delta. */
    {
        sh_mpkg_delivery three[4] = {{ka, a, la}, {kc, c, lc}, {ke, e, le}, {kb, b, lb}};
        sh_mpkg_share_item plan[4] = {{ka, a, la}, {kc, c, lc}, {ke, e, le}, {kb, b, lb}};
        map = share_set(EMPTY_MAP, three, 4, &length);
        classic = share_classic(EMPTY_MAP, three, 4, &classic_length);
        CHECK(map && classic && length + 2 * SH_MPKG_SHARD_CHARS < classic_length);
        if (map) share_expect_exact(map, length, three, 4);
        CHECK(sh_mpkg_share_plan(plan, 4, SH_MPKG_SHARD_CHARS, error, sizeof(error)));
        CHECK(!plan[0].delta && !plan[1].delta && plan[2].delta && plan[3].delta);
        /* e reads a and c: two bases, in first-use order. */
        CHECK(plan[2].delta && plan[2].delta[52] == 2 && plan[2].delta[53] == 0);
        sh_mpkg_share_clear(plan, 4);
        if (map && classic) share_same_context(root, map, length, classic, classic_length, 4);
        if (map) HeapFree(GetProcessHeap(), 0, map);
        if (classic) HeapFree(GetProcessHeap(), 0, classic);
        map = classic = NULL;
    }

    /* Only classic carriers are bases: a delivery whose bytes also sit in an
     * earlier delta reads them from a classic carrier or carries them itself. */
    {
        unsigned seeds_x[SHARE_MEMBERS];
        size_t lx = 0;
        unsigned char *x;
        char kx[SH_PACKAGE_BUNDLE_KEY_CAP];
        memcpy(seeds_x, seeds_b, sizeof(seeds_x)); seeds_x[9] = 4242;
        x = share_archive("shared-fixture", seeds_x, &lx);
        if (x) {
            sh_mpkg_share_item plan[3] = {{ka, a, la}, {kb, b, lb}, {NULL, x, lx}};
            share_key(x, lx, kx); plan[2].key = kx;
            CHECK(sh_mpkg_share_plan(plan, 3, SH_MPKG_SHARD_CHARS, error, sizeof(error)));
            CHECK(!plan[0].delta && plan[1].delta && plan[2].delta);
            CHECK(plan[2].delta && plan[2].delta[52] == 1 && !memcmp(plan[2].delta + 58, ka, strlen(ka)));
            if (plan[2].delta) {
                share_bases bases = {{ka, kb}, {a, b}, {la, lb}, 2, 0};
                size_t got = 0;
                unsigned char *out = sh_mpkg_delta_decode(plan[2].delta, plan[2].delta_length, kx, share_resolve, &bases,
                    SH_MPKG_MAX_PAYLOAD, &got, error, sizeof(error));
                CHECK(out && got == lx && !memcmp(out, x, lx));
                if (out) HeapFree(GetProcessHeap(), 0, out);
            }
            sh_mpkg_share_clear(plan, 3); free(x);
        }
    }

    /* Equal digests never stand for equal bytes: with every segment digest
     * colliding, planning compares bytes and gives exactly the same maps. */
    {
        sh_mpkg_delivery three[4] = {{ka, a, la}, {kc, c, lc}, {ke, e, le}, {kb, b, lb}};
        size_t colliding_length = 0;
        char *colliding;
        map = share_set(EMPTY_MAP, three, 4, &length);
        sh_mpkg_share_test_colliding_digests(1);
        colliding = share_set(EMPTY_MAP, three, 4, &colliding_length);
        sh_mpkg_share_test_colliding_digests(0);
        CHECK(map && colliding && colliding_length == length && !memcmp(colliding, map, length));
        if (colliding) { share_expect_exact(colliding, colliding_length, three, 4); HeapFree(GetProcessHeap(), 0, colliding); }
        if (map) HeapFree(GetProcessHeap(), 0, map);
        map = NULL;
    }

    /* Bytes that are not a readable archive, or whose central directory is out
     * of order, are one segment: an identical copy is still shared, nothing else
     * is, and every delivery rebuilds exactly. */
    {
        unsigned char *raw = malloc(50000), *same = malloc(50000), *altered = malloc(50000);
        unsigned char *a_odd = malloc(la), *b_odd = malloc(lb);
        CHECK(raw && same && altered && a_odd && b_odd);
        if (raw && same && altered && a_odd && b_odd) {
            share_fill(raw, 50000, 31337); memcpy(same, raw, 50000); memcpy(altered, raw, 50000); altered[100] ^= 1;
            {
                sh_mpkg_share_item plan[3] = {{"delivery.raw", raw, 50000}, {"delivery.same", same, 50000},
                                              {"delivery.altered", altered, 50000}};
                share_bases bases = {{"delivery.raw"}, {raw}, {50000}, 1, 0};
                size_t got = 0;
                unsigned char *out;
                CHECK(sh_mpkg_share_plan(plan, 3, SH_MPKG_SHARD_CHARS, error, sizeof(error)));
                CHECK(!plan[0].delta && plan[1].delta && !plan[2].delta);
                out = plan[1].delta ? sh_mpkg_delta_decode(plan[1].delta, plan[1].delta_length, "delivery.same",
                    share_resolve, &bases, SH_MPKG_MAX_PAYLOAD, &got, error, sizeof(error)) : NULL;
                CHECK(out && got == 50000 && !memcmp(out, same, 50000));
                if (out) HeapFree(GetProcessHeap(), 0, out);
                sh_mpkg_share_clear(plan, 3);
            }
            {
                /* Swap the first two records' central directory offsets in both variants. */
                unsigned char *odd[2] = {a_odd, b_odd};
                const unsigned char *src[2] = {a, b};
                size_t lens[2] = {la, lb};
                int swapped = 1;
                for (int v = 0; v < 2; v++) {
                    const unsigned char *eocd = src[v] + lens[v] - 22;
                    size_t cd = (size_t)(eocd[16] | eocd[17] << 8 | eocd[18] << 16 | (size_t)eocd[19] << 24), second;
                    unsigned char first_offset[4];
                    memcpy(odd[v], src[v], lens[v]);
                    second = cd + 46 + (odd[v][cd + 28] | odd[v][cd + 29] << 8) + (odd[v][cd + 30] | odd[v][cd + 31] << 8) +
                        (odd[v][cd + 32] | odd[v][cd + 33] << 8);
                    if (second + 46 > lens[v]) { swapped = 0; continue; }
                    memcpy(first_offset, odd[v] + cd + 42, 4);
                    memcpy(odd[v] + cd + 42, odd[v] + second + 42, 4); memcpy(odd[v] + second + 42, first_offset, 4);
                }
                CHECK(swapped);
                {
                    sh_mpkg_share_item plan[2] = {{"delivery.odd-a", a_odd, la}, {"delivery.odd-b", b_odd, lb}};
                    CHECK(sh_mpkg_share_plan(plan, 2, SH_MPKG_SHARD_CHARS, error, sizeof(error)) && !plan[0].delta && !plan[1].delta);
                    sh_mpkg_share_clear(plan, 2);
                }
            }
        }
        free(raw); free(same); free(altered); free(a_odd); free(b_odd);
    }

    /* Field-by-field refusals of a valid delta (b of a), before and after its base is read. */
    {
        sh_mpkg_share_item plan[2] = {{ka, a, la}, {kb, b, lb}};
        share_bases bases = {{ka}, {a}, {la}, 1, 0};
        const unsigned char *delta;
        size_t n, key = strlen(ka), ops, first;
        CHECK(sh_mpkg_share_plan(plan, 2, SH_MPKG_SHARD_CHARS, error, sizeof(error)) && !plan[0].delta && plan[1].delta);
        delta = plan[1].delta; n = plan[1].delta_length;
        if (delta) {
            unsigned char *copy = malloc(n + 1), *out;
            size_t got = 0;
            ops = 98 + key; first = ops + 4;
            out = sh_mpkg_delta_decode(delta, n, kb, share_resolve, &bases, SH_MPKG_MAX_PAYLOAD, &got, error, sizeof(error));
            CHECK(out && got == lb && !memcmp(out, b, lb) && bases.calls == 1);
            if (out) HeapFree(GetProcessHeap(), 0, out);
            /* Every truncation refuses before any base is read. */
            bases.calls = 0;
            for (size_t cut = 0; cut < n; cut++) share_refuses(delta, cut, kb, &bases, NULL);
            CHECK(bases.calls == 0);
            share_mutate(delta, n, 8, 2, kb, &bases, "newer than this Snapmap+");
            share_mutate(delta, n, 0, 'X', kb, &bases, NULL);
            share_mutate(delta, n, 12, 0, kb, &bases, NULL);
            share_mutate(delta, n, 52, 0, kb, &bases, "invalid base count");
            share_mutate(delta, n, 52, 65, kb, &bases, "invalid base count");
            share_mutate(delta, n, 52, 2, kb, &bases, NULL);
            share_mutate(delta, n, 56, 0, kb, &bases, NULL);
            share_mutate(delta, n, 58, 'D', kb, &bases, "invalid base");
            share_mutate(delta, n, ops, 0, kb, &bases, "invalid operation count");
            share_mutate(delta, n, ops + 3, 0x7f, kb, &bases, "invalid operation count");
            share_mutate(delta, n, first, 3, kb, &bases, "unknown operation");
            CHECK(bases.calls == 0);
            if (copy) {
                /* A base named as itself, an empty or overlong read, and a short or long total. */
                memcpy(copy, delta, n);
                share_refuses(copy, n, ka, &bases, "names itself");
                memcpy(copy, delta, n); share_put64(copy + 58 + key, 0);
                share_refuses(copy, n, kb, &bases, "invalid base length");
                memcpy(copy, delta, n); share_put64(copy + 12, lb - 1);
                share_refuses(copy, n, kb, &bases, NULL);
                memcpy(copy, delta, n); share_put64(copy + 12, lb + 1);
                share_refuses(copy, n, kb, &bases, "does not cover");
                memcpy(copy, delta, n); copy[n] = 0;
                share_refuses(copy, n + 1, kb, &bases, NULL);
                if (copy[first] == 1) {
                    memcpy(copy, delta, n); share_put64(copy + first + 13, 0);
                    share_refuses(copy, n, kb, &bases, NULL);
                    memcpy(copy, delta, n); share_put64(copy + first + 5, la);
                    share_refuses(copy, n, kb, &bases, "outside its base");
                    memcpy(copy, delta, n); share_put64(copy + first + 13, (uint64_t)la + 1);
                    share_refuses(copy, n, kb, &bases, "outside its base");
                }
                CHECK(bases.calls == 0);
                /* After validation: a base that differs, a missing one, a shared one, a wrong result. */
                memcpy(copy, delta, n); copy[66 + key] ^= 1;
                share_refuses(copy, n, kb, &bases, "not its recorded archive");
                memcpy(copy, delta, n); copy[20] ^= 1;
                share_refuses(copy, n, kb, &bases, "does not rebuild");
                {
                    size_t literal = 0;
                    for (size_t at = n; at-- > first;) if (delta[at] != 0) { literal = at; break; }
                    memcpy(copy, delta, n); copy[literal] ^= 0x5a;
                    share_refuses(copy, n, kb, &bases, NULL);
                }
                {
                    share_bases missing = {{"delivery.missing"}, {a}, {la}, 1, 0};
                    share_refuses(delta, n, kb, &missing, "unavailable");
                    share_bases shared = {{ka}, {delta}, {n}, 1, 0};
                    share_refuses(delta, n, kb, &shared, NULL);
                }
                free(copy);
            }
        }
        /* An unused or repeated base is refused before any base is read. */
        if (delta) {
            size_t entry = 2 + key + 40, grown = n + entry;
            unsigned char *two = malloc(grown);
            CHECK(two);
            if (two) {
                memcpy(two, delta, 56 + entry); memcpy(two + 56 + entry, delta + 56, entry);
                memcpy(two + 56 + 2 * entry, delta + 56 + entry, n - 56 - entry);
                two[52] = 2; bases.calls = 0;
                share_refuses(two, grown, kb, &bases, "repeats a base");
                memcpy(two + 56 + entry + 2, kc, key);
                share_refuses(two, grown, kb, &bases, "unused base");
                CHECK(bases.calls == 0);
                free(two);
            }
        }
        /* Map level: a missing, incomplete or shared base refuses the delivery and opens nothing. */
        if (delta) {
            size_t only_length = 0, with_length = 0, chain_length = 0;
            char *only = sh_mpkg_embed(EMPTY_MAP, strlen(EMPTY_MAP), kb, delta, n, &only_length, error, sizeof(error));
            char *with = only ? sh_mpkg_embed(only, only_length, ka, a, la, &with_length, error, sizeof(error)) : NULL;
            CHECK(only && with);
            if (only) share_map_refuses(root, only, only_length, kb, "unavailable");
            if (with) {
                char *cut = _strdup(with), *header;
                char name[SH_MPKG_HEADER_CAP];
                share_expect_exact(with, with_length, (sh_mpkg_delivery[]){{kb, b, lb}}, 1);
                snprintf(name, sizeof(name), "\"smpkg.%s.1.", ka);
                header = cut ? strstr(cut, name) : NULL;
                CHECK(header != NULL);
                if (header) { header[8] = 'X'; share_map_refuses(root, cut, with_length, kb, "unavailable"); }
                free(cut);
                /* A delta whose base is itself a delta: a third variant read against b. */
                {
                    unsigned seeds_f[SHARE_MEMBERS];
                    size_t lf = 0;
                    unsigned char *third;
                    char kf[SH_PACKAGE_BUNDLE_KEY_CAP];
                    memcpy(seeds_f, seeds_a, sizeof(seeds_f)); seeds_f[7] = 777;
                    third = share_archive("shared-fixture", seeds_f, &lf);
                    if (third) {
                        sh_mpkg_share_item chained[2] = {{kb, b, lb}, {kf, third, lf}};
                        share_key(third, lf, kf);
                        CHECK(sh_mpkg_share_plan(chained, 2, SH_MPKG_SHARD_CHARS, error, sizeof(error)) && chained[1].delta);
                        if (chained[1].delta) {
                            char *chain = sh_mpkg_embed(with, with_length, kf, chained[1].delta, chained[1].delta_length,
                                &chain_length, error, sizeof(error));
                            CHECK(chain != NULL);
                            if (chain) { share_map_refuses(root, chain, chain_length, kf, "itself shared"); HeapFree(GetProcessHeap(), 0, chain); }
                        }
                        sh_mpkg_share_clear(chained, 2);
                        free(third);
                    }
                }
            }
            if (only) HeapFree(GetProcessHeap(), 0, only);
            if (with) HeapFree(GetProcessHeap(), 0, with);
        }
        sh_mpkg_share_clear(plan, 2);
    }

    /* Exhausted memory: a decode returns nothing or the exact archive, a save
     * falls back to whole archives; neither ever returns part of one. */
    {
        sh_mpkg_delivery pair[2] = {{ka, a, la}, {kb, b, lb}};
        long used_decode, used_plan;
        map = share_set(EMPTY_MAP, pair, 2, &length);
        classic = share_classic(EMPTY_MAP, pair, 2, &classic_length);
        if (map && classic) {
            size_t got = 0;
            unsigned char *archive;
            sh_mpkg_share_test_refuse_allocation(100000);
            archive = sh_mpkg_extract(map, length, kb, &got, error, sizeof(error));
            used_decode = 100000 - sh_mpkg_share_test_allocations_left();
            if (archive) HeapFree(GetProcessHeap(), 0, archive);
            sh_mpkg_share_test_refuse_allocation(100000);
            again = share_set(EMPTY_MAP, pair, 2, &again_length);
            used_plan = 100000 - sh_mpkg_share_test_allocations_left();
            sh_mpkg_share_test_refuse_allocation(0);
            if (again) { HeapFree(GetProcessHeap(), 0, again); again = NULL; }
            CHECK(used_decode >= 2 && used_plan >= 4);
            for (long nth = 1; nth <= used_decode; nth++) {
                sh_mpkg_share_test_refuse_allocation(nth); got = 7;
                archive = sh_mpkg_extract(map, length, kb, &got, error, sizeof(error));
                CHECK(!archive && !got && error[0]);
                if (archive) HeapFree(GetProcessHeap(), 0, archive);
            }
            for (long nth = 1; nth <= used_plan; nth++) {
                sh_mpkg_share_test_refuse_allocation(nth);
                again = sh_mpkg_embed_set(EMPTY_MAP, strlen(EMPTY_MAP), pair, 2, &again_length, NULL, error, sizeof(error));
                CHECK(again && again_length == classic_length && !memcmp(again, classic, classic_length));
                if (again) { HeapFree(GetProcessHeap(), 0, again); again = NULL; }
            }
            sh_mpkg_share_test_refuse_allocation(0);
            share_expect_exact(map, length, pair, 2);
        }
        if (map) HeapFree(GetProcessHeap(), 0, map);
        if (classic) HeapFree(GetProcessHeap(), 0, classic);
        map = classic = NULL;
    }
done:
    sh_mpkg_share_test_refuse_allocation(0);
    free(a); free(b); free(c); free(d); free(e);
    if (map) HeapFree(GetProcessHeap(), 0, map);
    if (classic) HeapFree(GetProcessHeap(), 0, classic);
}

/* Two beta13 folder carriers holding the same authentic archive under two
 * migration seeds: storage is shared, each keeps its own recipe and identity,
 * a historical key and a new delta mix in one map, and re-save and reopen
 * agree with the classic encoding. */
static void test_shared_historical_recipes(const char *root)
{
    char copy_key[SH_PACKAGE_BUNDLE_KEY_CAP], boss_key[SH_PACKAGE_BUNDLE_KEY_CAP], error[SH_MPKG_ERR_CAP];
    sh_mpkg_delivery mixed[2] = {{HIST_BOSS_ID, hist_boss_payload, hist_boss_payload_len},
                                 {"boss-copy", hist_boss_payload, hist_boss_payload_len}};
    size_t length = 0, classic_length = 0;
    char *map, *classic;
    sh_mpkg_set_legacy_reader(hist_catalog, NULL); g_hist_missing = 0;
    hist_key(hist_boss_payload, hist_boss_payload_len, HIST_BOSS_ID, boss_key);
    hist_key(hist_boss_payload, hist_boss_payload_len, "boss-copy", copy_key);
    CHECK(strcmp(boss_key, copy_key) != 0);
    map = share_set(hist_base_compact, mixed, 2, &length);
    classic = share_classic(hist_base_compact, mixed, 2, &classic_length);
    CHECK(map && classic && length + SH_MPKG_SHARD_CHARS <= classic_length);
    if (map && classic) {
        sh_mpkg_context *context;
        share_expect_exact(map, length, mixed, 2);
        share_same_context(root, map, length, classic, classic_length, 2);
        context = sh_mpkg_context_open(root, map, length, error, sizeof(error));
        CHECK(context != NULL);
        if (context) {
            size_t count = 0, resaved_length = 0, second_length = 0;
            const sh_package_bundle_binding *bundles = sh_mpkg_context_bundles(context, &count);
            char *resaved, *second = NULL;
            char keys[2][SH_PACKAGE_BUNDLE_KEY_CAP];
            CHECK(count == 2);
            sh_package_bundle_delivery_key(bundles[0].original, keys[0]);
            sh_package_bundle_delivery_key(bundles[1].original, keys[1]);
            CHECK(!strcmp(keys[0], boss_key) && !strcmp(keys[1], copy_key));
            hist_check_converted(context, 0, HIST_BOSS_ID);
            hist_check_converted(context, 1, "boss-copy");
            /* Re-saved under both complete-tree keys, still shared; reopen and save again. */
            resaved = hist_resave(hist_base_compact, hist_base_compact_len, context, &resaved_length);
            CHECK(sh_mpkg_context_close(&context));
            CHECK(resaved && resaved_length + SH_MPKG_SHARD_CHARS <= classic_length);
            if (resaved) {
                sh_mpkg_delivery keyed[2] = {{boss_key, hist_boss_payload, hist_boss_payload_len},
                                             {copy_key, hist_boss_payload, hist_boss_payload_len}};
                share_expect_exact(resaved, resaved_length, keyed, 2);
                context = sh_mpkg_context_open(root, resaved, resaved_length, error, sizeof(error));
                CHECK(context != NULL);
                if (context) {
                    hist_check_converted(context, 0, HIST_BOSS_ID);
                    hist_check_converted(context, 1, "boss-copy");
                    second = hist_resave(hist_base_compact, hist_base_compact_len, context, &second_length);
                    CHECK(second && second_length == resaved_length && !memcmp(second, resaved, resaved_length));
                    CHECK(sh_mpkg_context_close(&context));
                }
                if (second) HeapFree(GetProcessHeap(), 0, second);
                HeapFree(GetProcessHeap(), 0, resaved);
            }
        }
    }
    if (map) HeapFree(GetProcessHeap(), 0, map);
    if (classic) HeapFree(GetProcessHeap(), 0, classic);
    sh_mpkg_set_legacy_reader(NULL, NULL);
}

/* ---- the reference carrier (L2) ------------------------------------------ */

static int reference_completions, reference_outcome;
static void reference_completed(void *context, int outcome)
{ CHECK(context == &reference_completions); reference_completions++; reference_outcome = outcome; }

/* An archive with a root descriptor and one member of `bytes` incompressible
 * bytes; the descriptor may carry author and homepage. */
static unsigned char *make_described_archive(const char *descriptor, size_t bytes, size_t *length)
{
    sh_package_archive_file members[2];
    sh_package_archive_files files = {members, 2};
    unsigned char *blob = malloc(bytes ? bytes : 1), *archive;
    char error[256];
    unsigned state = 0x9e3779b9u;
    if (!blob) return NULL;
    for (size_t i = 0; i < bytes; i++) { state = state * 1664525u + 1013904223u; blob[i] = (unsigned char)(state >> 24); }
    members[0].name = "package.json"; members[0].body = (unsigned char *)descriptor;
    members[0].length = strlen(descriptor); members[0].directory = 0;
    members[1].name = "assets/generated/blob.bin"; members[1].body = blob; members[1].length = bytes; members[1].directory = 0;
    archive = sh_package_archive_write(&files, length, error, sizeof(error));
    free(blob);
    return archive;
}

static void test_reference_carrier(void)
{
    static const char described[] = "{\"id\":\"local.described\",\"name\":\"Described \\\"pack\\\"\","
        "\"author\":\"Some One\",\"homepage\":\"https://example.test/described\"}";
    static const char plain[] = "{\"id\":\"local.plain\",\"name\":\"Plain\",\"author\":7}";
    unsigned char *small, *big, tree[32];
    size_t small_len = 0, big_len = 0, out_len = 0, stripped_len = 0, n;
    char err[SH_MPKG_ERR_CAP], key[SH_PACKAGE_BUNDLE_KEY_CAP], error[256], *out, *stripped, *again;
    sh_mpkg_reference reference, scanned[4];
    int referenced[2];
    sh_mpkg_decl decls[4];

    sh_mpkg_test_reset();
    small = make_described_archive(described, 300, &small_len); CHECK(small);
    if (!small) return;
    CHECK(sh_package_archive_tree_identity(small, small_len, tree, err, sizeof err));
    sh_package_archive_delivery_id(tree, key);

    /* ---- describe: descriptor fields, packed size and digest -------------- */
    CHECK(!sh_mpkg_reference_describe(key, NULL, 0, &reference, err, sizeof err));
    CHECK(sh_mpkg_reference_describe(key, small, small_len, &reference, err, sizeof err));
    CHECK(!strcmp(reference.key, key) && !strcmp(reference.id, "local.described"));
    CHECK(!strcmp(reference.name, "Described \"pack\"") && !strcmp(reference.author, "Some One"));
    CHECK(!strcmp(reference.homepage, "https://example.test/described"));
    CHECK(reference.packed == small_len && strlen(reference.sha256) == 64);
    CHECK(reference.tree_known && !memcmp(reference.tree, tree, 32));

    /* ---- embed, scan, strip: one shard, the body verified by its digest --- */
    out = sh_mpkg_reference_embed(EMPTY_MAP, strlen(EMPTY_MAP), &reference, &out_len, err, sizeof err);
    CHECK(out && out_len == strlen(out) && json_well_formed(out, out_len));
    if (!out) { fprintf(stderr, "  reference embed said: %s\n", err); free(small); return; }
    CHECK(strstr(out, "smpref.delivery.") && !strstr(out, "smpkg.") && alloc_count_string(out) == 1);
    CHECK(sh_mpkg_scan(out, out_len, decls, 4) == 0);   /* a reference is not a delivery */
    n = sh_mpkg_reference_scan(out, out_len, scanned, 4);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(!strcmp(scanned[0].key, key) && !strcmp(scanned[0].id, "local.described"));
        CHECK(!strcmp(scanned[0].name, "Described \"pack\"") && !strcmp(scanned[0].author, "Some One"));
        CHECK(!strcmp(scanned[0].homepage, "https://example.test/described") && scanned[0].packed == small_len);
        CHECK(!strcmp(scanned[0].sha256, reference.sha256) && scanned[0].tree_known && !memcmp(scanned[0].tree, tree, 32));
        CHECK(strlen(scanned[0].digest) == SH_MPKG_DIGEST_CHARS);
    }
    CHECK(sh_mpkg_reference_scan(out, out_len, NULL, 0) == 1);
    CHECK(sh_mpkg_reference_scan(out, out_len, scanned, 0) == SIZE_MAX);   /* no partial set */
    /* A second write of the same key replaces the first. */
    again = sh_mpkg_reference_embed(out, out_len, &reference, &n, err, sizeof err);
    CHECK(again && sh_mpkg_reference_scan(again, n, scanned, 4) == 1 && alloc_count_string(again) == 1);
    if (again) HeapFree(GetProcessHeap(), 0, again);
    /* A reference replaces a delivery of its key, and a delivery replaces the reference. */
    again = sh_mpkg_embed(out, out_len, key, small, small_len, &n, err, sizeof err);
    CHECK(again && sh_mpkg_scan(again, n, decls, 4) == 1 && sh_mpkg_reference_scan(again, n, scanned, 4) == 1);
    if (again) {
        char *back = sh_mpkg_reference_embed(again, n, &reference, &stripped_len, err, sizeof err);
        CHECK(back && sh_mpkg_scan(back, stripped_len, decls, 4) == 0 &&
            sh_mpkg_reference_scan(back, stripped_len, scanned, 4) == 1 && alloc_count_string(back) == 1);
        if (back) HeapFree(GetProcessHeap(), 0, back);
        HeapFree(GetProcessHeap(), 0, again);
    }
    stripped = sh_mpkg_reference_strip(out, out_len, &stripped_len);
    CHECK(stripped && stripped_len == strlen(EMPTY_MAP) && !memcmp(stripped, EMPTY_MAP, stripped_len));
    if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
    CHECK(!sh_mpkg_reference_strip(EMPTY_MAP, strlen(EMPTY_MAP), &stripped_len));
    /* A damaged body is a malformed map, never a guess. */
    {
        char *corrupt = _strdup(out), *digest = corrupt ? strstr(corrupt, "smpref.delivery.") : NULL;
        if (digest) {
            digest += strlen("smpref.") + strlen(key) + 1;
            digest[0] = digest[0] == '0' ? '1' : '0';
            CHECK(sh_mpkg_reference_scan(corrupt, strlen(corrupt), scanned, 4) == SIZE_MAX);
        }
        free(corrupt);
    }
    /* Prose that merely contains the magic is not a reference. */
    {
        static const char prose[] = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
            "\"name\":\"smpref.delivery.notes.0123456789abcdef\"}";
        CHECK(sh_mpkg_reference_scan(prose, sizeof prose - 1, scanned, 4) == 0);
        CHECK(sh_mpkg_gate(prose, sizeof prose - 1) == 1);
    }

    /* ---- the gate: installed or opened-anyway passes, otherwise one notice -- */
    g_dialog_idle = 1; g_dialog_asks = 0; g_dialog_answer = SH_ENGINE_DIALOG_PENDING; g_log[0] = 0;
    CHECK(sh_mpkg_gate(out, out_len) == 0);
    CHECK(strstr(sh_mpkg_test_last_refusal(), "references a package that is not installed"));
    sh_mpkg_consent_poll(); CHECK(g_dialog_asks == 1);
    CHECK(strstr(g_dialog_text, "Open the map without it?") && strstr(g_dialog_text, "Described \"pack\" by Some One"));
    CHECK(strstr(g_dialog_text, "Get it at https://example.test/described") && strlen(g_dialog_text) < 256);
    CHECK(!strstr(g_dialog_text, "delivery."));
    /* While it is up, the same map is not asked twice. */
    CHECK(sh_mpkg_request_reference_consent(out, out_len, NULL, NULL, error, sizeof error) < 0);
    CHECK(strstr(error, "already waiting"));
    g_dialog_answer = SH_ENGINE_DIALOG_DECLINED; sh_mpkg_consent_poll();
    CHECK(strstr(g_log, "was not opened"));
    CHECK(sh_mpkg_gate(out, out_len) == 0);   /* a later load asks again */
    sh_mpkg_consent_poll(); CHECK(g_dialog_asks == 2);
    g_dialog_answer = SH_ENGINE_DIALOG_ACCEPTED; sh_mpkg_consent_poll();
    CHECK(strstr(g_log, "opens without package"));
    CHECK(sh_mpkg_gate(out, out_len) == 1);   /* opened anyway: this session passes silently */
    sh_mpkg_consent_poll(); CHECK(g_dialog_asks == 2);
    CHECK(!sh_mpkg_test_session_installed_count());   /* nothing was installed */
    /* With a completion the answer is reported once, exactly as consent is. */
    sh_mpkg_test_reset(); g_dialog_idle = 1; g_dialog_asks = 0; g_dialog_answer = SH_ENGINE_DIALOG_PENDING;
    reference_completions = 0;
    CHECK(sh_mpkg_request_reference_consent(out, out_len, reference_completed, &reference_completions,
        error, sizeof error) == 0);
    CHECK(!reference_completions);
    sh_mpkg_cancel_map_consent(&reference_completions);   /* the caller withdrew */
    CHECK(reference_completions == 1 && reference_outcome == 0);
    sh_mpkg_consent_poll(); CHECK(g_dialog_asks == 0);
    CHECK(sh_mpkg_request_reference_consent(out, out_len, reference_completed, &reference_completions,
        error, sizeof error) == 0);
    sh_mpkg_consent_poll(); CHECK(g_dialog_asks == 1);
    g_dialog_answer = SH_ENGINE_DIALOG_ACCEPTED; sh_mpkg_consent_poll();
    CHECK(reference_completions == 2 && reference_outcome == 1);
    CHECK(sh_mpkg_request_reference_consent(out, out_len, reference_completed, &reference_completions,
        error, sizeof error) == 1);
    /* Synchronous test decisions. */
    sh_mpkg_test_reset(); sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_DECLINE); reference_completions = 0;
    CHECK(sh_mpkg_request_reference_consent(out, out_len, reference_completed, &reference_completions,
        error, sizeof error) == 0);
    CHECK(reference_completions == 1 && reference_outcome == 0);
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
    CHECK(sh_mpkg_request_reference_consent(out, out_len, reference_completed, &reference_completions,
        error, sizeof error) == 0);
    CHECK(reference_completions == 2 && reference_outcome == 1 && sh_mpkg_gate(out, out_len) == 1);
    sh_mpkg_test_reset(); g_dialog_idle = 0;
    HeapFree(GetProcessHeap(), 0, out);

    /* ---- the budget: a delivery over 8 MiB packed travels as a reference -- */
    big = make_described_archive(plain, SH_MPKG_DELIVERY_BUDGET_BYTES + 1, &big_len); CHECK(big);
    if (big) {
        sh_mpkg_delivery items[2];
        char big_key[SH_PACKAGE_BUNDLE_KEY_CAP];
        size_t failed_item = SIZE_MAX;
        CHECK(sh_package_archive_tree_identity(big, big_len, tree, err, sizeof err));
        sh_package_archive_delivery_id(tree, big_key);
        CHECK(sh_mpkg_shards_for(big_len) > SH_MPKG_DELIVERY_BUDGET_SHARDS / 2);
        items[0].id = key; items[0].archive = small; items[0].length = small_len;
        items[1].id = big_key; items[1].archive = big; items[1].length = big_len;
        out = sh_mpkg_embed_within(EMPTY_MAP, strlen(EMPTY_MAP), items, 2, 0, referenced, &out_len, &failed_item,
                                   err, sizeof err);
        CHECK(out && !referenced[0] && referenced[1]);
        if (out) {
            unsigned char *back;
            CHECK(sh_mpkg_scan(out, out_len, decls, 4) == 1 && !strcmp(decls[0].id, key));
            CHECK(sh_mpkg_reference_scan(out, out_len, scanned, 4) == 1 && !strcmp(scanned[0].key, big_key));
            CHECK(!strcmp(scanned[0].id, "local.plain") && !scanned[0].author[0] && scanned[0].packed == big_len);
            back = sh_mpkg_extract(out, out_len, key, &n, err, sizeof err);
            CHECK(back && n == small_len && !memcmp(back, small, n));
            if (back) HeapFree(GetProcessHeap(), 0, back);
            HeapFree(GetProcessHeap(), 0, out);
        }
        /* The text bound: the largest fitting delivery is referenced first. */
        items[1].id = key; items[1].archive = small; items[1].length = small_len;
        {
            unsigned char *medium = make_described_archive(plain, 3000, &n);
            CHECK(medium);
            if (medium) {
                char medium_key[SH_PACKAGE_BUNDLE_KEY_CAP];
                CHECK(sh_package_archive_tree_identity(medium, n, tree, err, sizeof err));
                sh_package_archive_delivery_id(tree, medium_key);
                items[0].id = medium_key; items[0].archive = medium; items[0].length = n;
                out = sh_mpkg_embed_within(EMPTY_MAP, strlen(EMPTY_MAP), items, 2, 0, referenced, &out_len,
                                           &failed_item, err, sizeof err);
                CHECK(out && !referenced[0] && !referenced[1] && sh_mpkg_scan(out, out_len, decls, 4) == 2);
                if (out) HeapFree(GetProcessHeap(), 0, out);
                out = sh_mpkg_embed_within(EMPTY_MAP, strlen(EMPTY_MAP), items, 2, strlen(EMPTY_MAP) + 2000,
                                           referenced, &out_len, &failed_item, err, sizeof err);
                CHECK(out && referenced[0] && !referenced[1] && out_len <= strlen(EMPTY_MAP) + 2000);
                if (out) {
                    CHECK(sh_mpkg_scan(out, out_len, decls, 4) == 1 && !strcmp(decls[0].id, key));
                    CHECK(sh_mpkg_reference_scan(out, out_len, scanned, 4) == 1 && !strcmp(scanned[0].key, medium_key));
                    HeapFree(GetProcessHeap(), 0, out);
                }
                /* Nothing fits: every delivery is a reference and the text stays small. */
                out = sh_mpkg_embed_within(EMPTY_MAP, strlen(EMPTY_MAP), items, 2, strlen(EMPTY_MAP) + 100,
                                           referenced, &out_len, &failed_item, err, sizeof err);
                CHECK(out && referenced[0] && referenced[1] && sh_mpkg_scan(out, out_len, decls, 4) == 0);
                CHECK(out && sh_mpkg_reference_scan(out, out_len, scanned, 4) == 2);
                if (out) HeapFree(GetProcessHeap(), 0, out);
                free(medium);
            }
        }
        free(big);
    }
    free(small);
}

/* ---- references at open: the installed library supplies a referenced tree --
 * A reference whose complete tree an installed package has is laid out in the
 * map's context from that package (matched by tree digest, never by id), so a
 * record that composed it compiles; a reference nobody has stays a reference,
 * and the load degrades the record for a player who lacks it. */
/* A stock answer for the degrade: the game ships every declaration asked about. */
static int stock_all(void *context, const char *family, const char *name)
{ (void)context; (void)family; (void)name; return 1; }

static void test_reference_context(const char *root)
{
    char error[512], err[SH_MPKG_ERR_CAP], key[SH_PACKAGE_BUNDLE_KEY_CAP], folder[MAX_PATH], path[MAX_PATH];
    char *map = NULL, *text, *degraded;
    size_t map_len = 0, n, written = 0, degraded_len = 0;
    unsigned char *archive = NULL, tree[32];
    sh_package_sources *sources;
    sh_mpkg_reference reference;
    sh_mpkg_context *context;
    sh_package_provenance uses = {0}, back = {0};
    unsigned char trees[2][32] = {{0}};
    sh_package_provenance_site first = {SH_PACKAGE_USE_OBJECT, "entity/0", "entityDef"};
    sh_package_provenance_site second = {SH_PACKAGE_USE_OBJECT, "entity/1", "entityDef"};
    size_t contexts[2];
    int present = 0;
    sh_mpkg_test_reset();
    /* The installed package whose tree the map will reference. */
    join(folder, sizeof folder, root, "overrides"); CHECK(make_dir(folder));
    join(folder, sizeof folder, folder, "referenced"); CHECK(make_dir(folder));
    join(path, sizeof path, folder, "package.json");
    CHECK(touch(path, "{\"id\":\"local.referenced\",\"name\":\"Referenced\",\"author\":\"Lib Author\"}"));
    join(path, sizeof path, folder, "assets"); CHECK(make_dir(path));
    join(path, sizeof path, path, "generated"); CHECK(make_dir(path));
    join(path, sizeof path, path, "decls"); CHECK(make_dir(path));
    join(path, sizeof path, path, "entitydef"); CHECK(make_dir(path));
    join(path, sizeof path, path, "referenced.decl"); CHECK(touch(path, "{ edit = { health = 9; } }"));
    sources = sh_package_sources_scan_directory(folder, error, sizeof error); CHECK(sources);
    if (!sources) return;
    archive = sh_package_archive_pack(sources, 0, &n, error, sizeof error); CHECK(archive);
    memcpy(tree, sources->fingerprints[0], 32); sh_package_sources_free(sources);
    if (!archive) return;
    sh_package_archive_delivery_id(tree, key);
    CHECK(sh_mpkg_reference_describe(key, archive, n, &reference, err, sizeof err) && !strcmp(reference.author, "Lib Author"));
    /* The map: one carried delivery, one reference, and a record whose first
     * use composes both trees while the second names the carried one. */
    map = sh_mpkg_embed(EMPTY_MAP, strlen(EMPTY_MAP), FIX_PKG_ID, fix_payload, fix_payload_len, &map_len, err, sizeof err);
    CHECK(map);
    if (!map) { free(archive); return; }
    text = sh_mpkg_reference_embed(map, map_len, &reference, &n, err, sizeof err);
    CHECK(text); HeapFree(GetProcessHeap(), 0, map); map = text; map_len = n;
    CHECK(sh_package_archive_tree_identity(fix_payload, fix_payload_len, trees[1], err, sizeof err));
    memcpy(trees[0], tree, 32);
    contexts[0] = sh_package_provenance_context_add(&uses, SH_PACKAGE_CONTEXT_COMPOSED, trees, 2, NULL);
    contexts[1] = sh_package_provenance_context_add(&uses, SH_PACKAGE_CONTEXT_ISOLATED, trees + 1, 1, "");
    CHECK(sh_package_provenance_bind(&uses, "entitydef", "referenced", "smp/referenced-private", contexts, 1));
    CHECK(sh_package_provenance_bind(&uses, "entitydef", "testpkg/brain", "testpkg/brain", contexts + 1, 1));
    CHECK(sh_package_provenance_use_set(&uses, &first, 0, contexts, 1));
    CHECK(sh_package_provenance_use_set(&uses, &second, 1, contexts + 1, 1));
    {
        char *with_record = sh_map_provenance_write(map, map_len, &uses, &written, error, sizeof error);
        CHECK(with_record && strstr(with_record, "smpbind."));
        if (with_record) { HeapFree(GetProcessHeap(), 0, map); map = with_record; map_len = written; }
    }
    /* Nothing installed: the context holds the carried delivery only, the
     * reference is missing, and the load degrades the record. */
    context = sh_mpkg_context_open(root, map, map_len, error, sizeof error);
    CHECK(context && sh_mpkg_context_count(context) == 1);
    CHECK(sh_mpkg_context_close(&context));
    CHECK(sh_mpkg_reference_missing(map, map_len, trees, 2) == 1 && !memcmp(trees[0], tree, 32));
    degraded = HeapAlloc(GetProcessHeap(), 0, map_len + 1); memcpy(degraded, map, map_len + 1); degraded_len = map_len;
    CHECK(sh_mpkg_degrade_absent_references(&degraded, &degraded_len, stock_all, NULL) && degraded);
    CHECK(sh_map_provenance_read(degraded, degraded_len, &back, &present, error, sizeof error) && present);
    CHECK(back.use_count == 1 && !strcmp(back.uses[0].owner, "entity/1") && back.context_count == 1 &&
        back.contexts[0].mode == SH_PACKAGE_CONTEXT_ISOLATED);
    sh_package_provenance_free(&back);
    HeapFree(GetProcessHeap(), 0, degraded);
    /* Installed (the boot snapshot knows the tree): the context lays the
     * referenced tree out beside the carried delivery, from the library. */
    sh_mpkg_boot_capture(root);
    CHECK(sh_mpkg_reference_missing(map, map_len, trees, 2) == 0);
    context = sh_mpkg_context_open(root, map, map_len, error, sizeof error);
    if (!context) fprintf(stderr, "reference context: %s\n", error);
    CHECK(context && sh_mpkg_context_count(context) == 2);
    if (context && sh_mpkg_context_count(context) == 2) {
        const sh_package_bundle_binding *bundles = sh_mpkg_context_bundles(context, &n);
        char unpacked[MAX_PATH];
        CHECK(n == 2 && !memcmp(bundles[1].compiler_tree, tree, 32) &&
            !memcmp(sh_package_bundle_fingerprint(bundles[1].original), tree, 32) && !strcmp(bundles[1].source_name, "1"));
        CHECK(!strcmp(sh_mpkg_context_carrier(context, 1), key) && !strcmp(sh_mpkg_context_carrier(context, 0), FIX_PKG_ID));
        join(unpacked, sizeof unpacked, sh_mpkg_context_root(context), "overrides\\1\\package.json");
        CHECK(file_exists(unpacked));
        /* A degrade with nothing missing leaves the text alone. */
        degraded = HeapAlloc(GetProcessHeap(), 0, map_len + 1); memcpy(degraded, map, map_len + 1); degraded_len = map_len;
        {
            char *before = degraded;
            CHECK(sh_mpkg_degrade_absent_references(&degraded, &degraded_len, stock_all, NULL) && degraded == before && degraded_len == map_len);
        }
        HeapFree(GetProcessHeap(), 0, degraded);
    }
    if (context) CHECK(sh_mpkg_context_close(&context));
    /* The installed tree is matched by digest: a package of the same id with
     * other bytes supplies nothing. */
    join(path, sizeof path, folder, "assets\\generated\\decls\\entitydef\\referenced.decl");
    CHECK(touch(path, "{ edit = { health = 10; } }"));
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(root);
    CHECK(sh_mpkg_reference_missing(map, map_len, trees, 2) == 1);
    context = sh_mpkg_context_open(root, map, map_len, error, sizeof error);
    CHECK(context && sh_mpkg_context_count(context) == 1);
    if (context) CHECK(sh_mpkg_context_close(&context));
    sh_package_provenance_free(&uses);
    HeapFree(GetProcessHeap(), 0, map); free(archive);
    remove_tree(folder);
    sh_mpkg_test_reset();
}

/* ---- opening without a referenced package: the dependents leave --------------
 * A document in the engine's shape whose record names a package the player
 * lacks: the entity whose use names an identity only that package adds leaves
 * with its instance attribution, logic wires, reference rows and door slot; an
 * entity using a stock identity the package merely shadows stays under the
 * stock name; the carried package's use is renumbered. A use of such an
 * identity outside an entity, a table whose meaning is not modelled naming the
 * entity, or a stock question nobody can answer refuses the degrade instead. */
typedef struct stock_fixture { int calls; } stock_fixture;
static int stock_spider(void *context, const char *family, const char *name)
{
    stock_fixture *fixture = context;
    fixture->calls++;
    if (strcmp(family, "entitydef")) return -1;
    if (!strcmp(name, "ai/conductor/coop/snapmap_default")) return 1;
    if (!strcmp(name, "snapmaps/placeablesnapaiencounter/spider_mastermind")) return 0;
    return -1;
}
static int stock_unknown(void *context, const char *family, const char *name)
{ (void)context; (void)family; (void)name; return -1; }

#define DEGRADE_MAP_HEAD \
    "{\"variables\":{\"allocCount\":[0,0,0,0,0,0,0,0,0,0],\"string\":[]}," \
    "\"entities\":[{\"entityDef\":{\"inherit\":\"smp/list\",\"targetType\":\"idDeclEntityDef\"},\"uniqueId\":1}," \
    "{\"entityDef\":{\"inherit\":\"smp/spider\",\"targetType\":\"idDeclEntityDef\"},\"uniqueId\":2}," \
    "{\"entityDef\":{\"inherit\":\"testpkg/brain\",\"targetType\":\"idDeclEntityDef\"},\"uniqueId\":3}]," \
    "\"instanceEntities\":{\"keyValues\":[0,3],\"values\":[1,2,3],\"~type\":\"idIndexMultimap\"}," \
    "\"doorsAndCaps\":{\"portalDoors\":[1,2],\"portalFrames\":[-1,-1]}," \
    "\"references\":{\"entityEntRefs\":{\"keyValues\":[0,0,1,2,3],\"values\":[2,-1,1]}," \
    "\"entityVarRefs\":{\"keyValues\":[0,0,1,2,3],\"values\":[16777215,16777215,16777215]}},"
#define DEGRADE_MAP DEGRADE_MAP_HEAD \
    "\"targets\":{\"connections\":[-2,2,3,-4,1],\"entitySources\":{\"keyValues\":[],\"values\":[]}," \
    "\"entityTargets\":{\"keyValues\":[],\"values\":[]}},\"name\":\"m\"}"
#define DEGRADE_MAP_TARGETED DEGRADE_MAP_HEAD \
    "\"targets\":{\"connections\":[-2,2,3,-4,1],\"entitySources\":{\"keyValues\":[],\"values\":[]}," \
    "\"entityTargets\":{\"keyValues\":[0,1],\"values\":[2]}},\"name\":\"m\"}"

/* The map text: the carried fixture delivery, the reference, the record. */
static char *degrade_map_text(const char *base, const sh_mpkg_reference *reference, const sh_package_provenance *uses, size_t *length)
{
    char err[SH_MPKG_ERR_CAP], error[512], *map, *next;
    size_t n = 0, written = 0;
    map = sh_mpkg_embed(base, strlen(base), FIX_PKG_ID, fix_payload, fix_payload_len, &n, err, sizeof err);
    CHECK(map); if (!map) return NULL;
    next = sh_mpkg_reference_embed(map, n, reference, &n, err, sizeof err);
    CHECK(next); HeapFree(GetProcessHeap(), 0, map); if (!next) return NULL; map = next;
    next = sh_map_provenance_write(map, n, uses, &written, error, sizeof error);
    CHECK(next && strstr(next, "smpbind.")); HeapFree(GetProcessHeap(), 0, map); if (!next) return NULL;
    *length = written;
    return next;
}

static void test_degrade_leaves_out_dependents(const char *root)
{
    char error[512], err[SH_MPKG_ERR_CAP], key[SH_PACKAGE_BUNDLE_KEY_CAP], folder[MAX_PATH], path[MAX_PATH];
    char *map = NULL, *degraded = NULL, *before;
    size_t map_len = 0, degraded_len = 0, n, contexts[2];
    unsigned char *archive = NULL, trees[2][32];
    sh_package_sources *sources;
    sh_mpkg_reference reference;
    sh_package_provenance uses = {0}, world_uses = {0}, back = {0};
    sh_package_provenance_site sites[3] = {
        {SH_PACKAGE_USE_OBJECT, "entity/0", "/k:entityDef/k:inherit"},
        {SH_PACKAGE_USE_OBJECT, "entity/1", "/k:entityDef/k:inherit"},
        {SH_PACKAGE_USE_OBJECT, "entity/2", "/k:entityDef/k:inherit"}};
    sh_package_provenance_site world = {SH_PACKAGE_USE_WORLD, "world", "/k:name"};
    stock_fixture stock = {0};
    int present = 0;
    sh_mpkg_test_reset();
    /* The package the map references and the player lacks. */
    join(folder, sizeof folder, root, "overrides"); CHECK(make_dir(folder));
    join(folder, sizeof folder, folder, "boss-demons"); CHECK(make_dir(folder));
    join(path, sizeof path, folder, "package.json");
    CHECK(touch(path, "{\"id\":\"local.boss-demons\",\"name\":\"Boss Demons\",\"author\":\"Boss Author\"}"));
    join(path, sizeof path, folder, "assets"); CHECK(make_dir(path));
    join(path, sizeof path, path, "generated"); CHECK(make_dir(path));
    join(path, sizeof path, path, "decls"); CHECK(make_dir(path));
    join(path, sizeof path, path, "entitydef"); CHECK(make_dir(path));
    join(path, sizeof path, path, "snapmaps"); CHECK(make_dir(path));
    join(path, sizeof path, path, "placeablesnapaiencounter"); CHECK(make_dir(path));
    join(path, sizeof path, path, "spider_mastermind.decl"); CHECK(touch(path, "{ edit = { health = 9000; } }"));
    sources = sh_package_sources_scan_directory(folder, error, sizeof error); CHECK(sources);
    if (!sources) return;
    archive = sh_package_archive_pack(sources, 0, &n, error, sizeof error); CHECK(archive);
    memcpy(trees[0], sources->fingerprints[0], 32); sh_package_sources_free(sources);
    remove_tree(folder);
    if (!archive) return;
    sh_package_archive_delivery_id(trees[0], key);
    CHECK(sh_mpkg_reference_describe(key, archive, n, &reference, err, sizeof err));
    free(archive); archive = NULL;
    CHECK(sh_package_archive_tree_identity(fix_payload, fix_payload_len, trees[1], err, sizeof err));
    /* The record: a shadowed stock list and an added encounter from the
     * referenced tree, a carried entity from the fixture delivery. */
    contexts[0] = sh_package_provenance_context_add(&uses, SH_PACKAGE_CONTEXT_ISOLATED, trees, 1, "");
    contexts[1] = sh_package_provenance_context_add(&uses, SH_PACKAGE_CONTEXT_ISOLATED, trees + 1, 1, "");
    CHECK(sh_package_provenance_bind(&uses, "entitydef", "ai/conductor/coop/snapmap_default", "smp/list", contexts, 1));
    CHECK(sh_package_provenance_bind(&uses, "entitydef", "snapmaps/placeablesnapaiencounter/spider_mastermind", "smp/spider", contexts, 1));
    CHECK(sh_package_provenance_bind(&uses, "entitydef", "testpkg/brain", "testpkg/brain", contexts + 1, 1));
    CHECK(sh_package_provenance_use_set(&uses, &sites[0], 0, contexts, 1));
    CHECK(sh_package_provenance_use_set(&uses, &sites[1], 1, contexts, 1));
    CHECK(sh_package_provenance_use_set(&uses, &sites[2], 2, contexts + 1, 1));
    map = degrade_map_text(DEGRADE_MAP, &reference, &uses, &map_len);
    if (!map) goto done;
    CHECK(sh_mpkg_reference_missing(map, map_len, NULL, 0) == 1);
    degraded = HeapAlloc(GetProcessHeap(), 0, map_len + 1); memcpy(degraded, map, map_len + 1); degraded_len = map_len;
    g_log[0] = 0;
    CHECK(sh_mpkg_degrade_absent_references(&degraded, &degraded_len, stock_spider, &stock) && degraded);
    CHECK(stock.calls == 2);
    CHECK(strstr(degraded, "\"entities\":[{\"entityDef\":{\"inherit\":\"ai/conductor/coop/snapmap_default\",\"targetType\":\"idDeclEntityDef\"},\"uniqueId\":1},"
        "{\"entityDef\":{\"inherit\":\"testpkg/brain\",\"targetType\":\"idDeclEntityDef\"},\"uniqueId\":3}]"));
    CHECK(!strstr(degraded, "smp/"));
    CHECK(strstr(degraded, "\"instanceEntities\":{\"keyValues\":[0,2],\"values\":[1,3],\"~type\":\"idIndexMultimap\"}"));
    CHECK(strstr(degraded, "\"portalDoors\":[1,-1],\"portalFrames\":[-1,-1]"));
    CHECK(strstr(degraded, "\"entityEntRefs\":{\"keyValues\":[0,0,1,1,2],\"values\":[-1,1]}"));
    CHECK(strstr(degraded, "\"entityVarRefs\":{\"keyValues\":[0,0,1,1,2],\"values\":[16777215,16777215]}"));
    CHECK(strstr(degraded, "\"connections\":[-2,3,-4,1],\"entitySources\""));
    CHECK(strstr(degraded, "smpkg.") && strstr(degraded, "smpref.") && strstr(degraded, "\"name\":\"m\"}"));
    CHECK(strstr(g_log, "MPKG: left out entity 1 (entitydef snapmaps/placeablesnapaiencounter/spider_mastermind), which only "
        "'Boss Demons' (local.boss-demons) supplies"));
    CHECK(strstr(g_log, "2 use(s) left the binding record, 2 private name(s) went back to their authored names, "
        "1 entity and 1 logic wire(s) were left out"));
    CHECK(sh_map_provenance_read(degraded, degraded_len, &back, &present, error, sizeof error) && present);
    CHECK(back.use_count == 1 && back.binding_count == 1 && !strcmp(back.uses[0].owner, "entity/1") &&
        !strcmp(back.bindings[back.uses[0].binding].name, "testpkg/brain"));
    sh_package_provenance_free(&back);
    HeapFree(GetProcessHeap(), 0, degraded); degraded = NULL;
    /* The stock question nobody can answer refuses, and the text stays. */
    degraded = HeapAlloc(GetProcessHeap(), 0, map_len + 1); memcpy(degraded, map, map_len + 1); degraded_len = map_len;
    before = degraded; g_log[0] = 0;
    CHECK(!sh_mpkg_degrade_absent_references(&degraded, &degraded_len, stock_unknown, NULL));
    CHECK(degraded == before && degraded_len == map_len && strstr(g_log, "cannot tell whether the game ships"));
    HeapFree(GetProcessHeap(), 0, degraded); degraded = NULL;
    HeapFree(GetProcessHeap(), 0, map); map = NULL;
    /* A table whose meaning is not modelled names the entity: refused. */
    map = degrade_map_text(DEGRADE_MAP_TARGETED, &reference, &uses, &map_len);
    if (map) {
        degraded = HeapAlloc(GetProcessHeap(), 0, map_len + 1); memcpy(degraded, map, map_len + 1); degraded_len = map_len;
        before = degraded; g_log[0] = 0;
        CHECK(!sh_mpkg_degrade_absent_references(&degraded, &degraded_len, stock_spider, &stock));
        CHECK(degraded == before && strstr(g_log, "entityTargets names entity 2"));
        HeapFree(GetProcessHeap(), 0, degraded); degraded = NULL;
        HeapFree(GetProcessHeap(), 0, map); map = NULL;
    }
    /* The added identity used by the map itself, not by one entity: refused. */
    contexts[0] = sh_package_provenance_context_add(&world_uses, SH_PACKAGE_CONTEXT_ISOLATED, trees, 1, "");
    CHECK(sh_package_provenance_bind(&world_uses, "entitydef", "snapmaps/placeablesnapaiencounter/spider_mastermind", "smp/spider", contexts, 1));
    CHECK(sh_package_provenance_use_set(&world_uses, &world, 0, contexts, 1));
    map = degrade_map_text(DEGRADE_MAP, &reference, &world_uses, &map_len);
    if (map) {
        degraded = HeapAlloc(GetProcessHeap(), 0, map_len + 1); memcpy(degraded, map, map_len + 1); degraded_len = map_len;
        before = degraded; g_log[0] = 0;
        CHECK(!sh_mpkg_degrade_absent_references(&degraded, &degraded_len, stock_spider, &stock));
        CHECK(degraded == before && strstr(g_log, "is used by the map itself (world /k:name) and cannot be left out"));
        HeapFree(GetProcessHeap(), 0, degraded); degraded = NULL;
    }
done:
    if (degraded) HeapFree(GetProcessHeap(), 0, degraded);
    if (map) HeapFree(GetProcessHeap(), 0, map);
    sh_package_provenance_free(&uses); sh_package_provenance_free(&world_uses);
    g_log[0] = 0;
    sh_mpkg_test_reset();
}

/* ---- the three levels on the retained R137 maps ----------------------------
 * Each fixture's engine text is the map with its authored carrier stripped.
 * r137-native2's row converts to native on save, so the saved map is that
 * engine text and nothing else (L0). r137-mixed2 keeps its five authored files
 * package-served, so its save embeds the complete archive (L1), or refers to
 * it when the text budget is too small (L2). The conversion decision itself is
 * the runtime's and is live-verified (R137); this covers the carrier writer's
 * side of each level on the real bytes. */
static void test_r137_levels(const char *root)
{
    size_t native_len = 0, mixed_len = 0, out_len = 0, n;
    char err[SH_MPKG_ERR_CAP], key[SH_PACKAGE_BUNDLE_KEY_CAP];
    char *native = sh_mpkg_strip(r137_native2_map, r137_native2_map_len, &native_len);
    char *mixed = sh_mpkg_strip(r137_mixed2_map, r137_mixed2_map_len, &mixed_len);
    unsigned char tree[32], *payload;
    sh_mpkg_decl decls[4];
    sh_mpkg_reference scanned[2];
    sh_mpkg_delivery item;
    sh_mpkg_context *context;
    int referenced = 0;
    sh_mpkg_test_reset();
    CHECK(sh_mpkg_scan(r137_native2_map, r137_native2_map_len, decls, 4) == 1 &&
        !strcmp(decls[0].id, R137_NATIVE2_PKG_ID));
    CHECK(sh_mpkg_scan(r137_mixed2_map, r137_mixed2_map_len, decls, 4) == 1 &&
        !strcmp(decls[0].id, R137_MIXED2_PKG_ID));
    CHECK(native && mixed);
    if (!native || !mixed) goto done;
    /* L0: the engine text names no package-added identity. It carries no
     * delivery, no reference and no binding record, passes the gate without a
     * boot snapshot, and opens an empty private context: a true pass-through. */
    CHECK(sh_mpkg_scan(native, native_len, decls, 4) == 0 && sh_mpkg_reference_scan(native, native_len, NULL, 0) == 0);
    CHECK(!sh_shard_find(native, native_len, "smpkg.", 6) && !sh_shard_find(native, native_len, "smpbind.", 8) &&
        !sh_shard_find(native, native_len, "smpref.", 7));
    CHECK(sh_mpkg_gate(native, native_len) == 1 && !sh_mpkg_test_last_refusal()[0]);
    CHECK(!sh_mpkg_strip(native, native_len, &n) && !sh_mpkg_reference_strip(native, native_len, &n));
    context = sh_mpkg_context_open(root, native, native_len, err, sizeof err);
    CHECK(context && !sh_mpkg_context_count(context) && !sh_mpkg_context_root(context));
    CHECK(sh_mpkg_context_close(&context));
    /* L1: the complete five-file archive travels under its complete-tree key. */
    payload = sh_mpkg_extract(r137_mixed2_map, r137_mixed2_map_len, R137_MIXED2_PKG_ID, &n, err, sizeof err);
    CHECK(payload && n == r137_mixed2_payload_len && !memcmp(payload, r137_mixed2_payload, n));
    if (payload) HeapFree(GetProcessHeap(), 0, payload);
    CHECK(sh_package_archive_tree_identity(r137_mixed2_payload, r137_mixed2_payload_len, tree, err, sizeof err));
    sh_package_archive_delivery_id(tree, key);
    item.id = key; item.archive = r137_mixed2_payload; item.length = r137_mixed2_payload_len;
    {
        char *saved = sh_mpkg_embed_within(mixed, mixed_len, &item, 1, 10485760u, &referenced,
                                           &out_len, &n, err, sizeof err);
        CHECK(saved && !referenced);
        if (saved) {
            sh_package_archive_files files = {0};
            CHECK(sh_mpkg_scan(saved, out_len, decls, 4) == 1 && !strcmp(decls[0].id, key) && decls[0].complete);
            CHECK(sh_mpkg_reference_scan(saved, out_len, NULL, 0) == 0);
            payload = sh_mpkg_extract(saved, out_len, key, &n, err, sizeof err);
            CHECK(payload && n == r137_mixed2_payload_len && !memcmp(payload, r137_mixed2_payload, n));
            if (payload && sh_package_archive_read(payload, n, &files, err, sizeof err)) {
                size_t regular = 0, matched = 0;
                for (size_t i = 0; i < files.count; i++) {
                    if (files.items[i].directory) continue;
                    regular++;
                    for (size_t f = 0; f < R137_MIXED2_FILE_COUNT; f++)
                        if (!strcmp(files.items[i].name, r137_mixed2_files[f])) matched++;
                }
                CHECK(regular == R137_MIXED2_FILE_COUNT && matched == R137_MIXED2_FILE_COUNT);
                sh_package_archive_files_free(&files);
            } else CHECK(0);
            if (payload) HeapFree(GetProcessHeap(), 0, payload);
            /* The recipient's private context holds the complete tree. */
            context = sh_mpkg_context_open(root, saved, out_len, err, sizeof err);
            CHECK(context && sh_mpkg_context_count(context) == 1);
            if (context) {
                const sh_package_bundle_binding *bundles = sh_mpkg_context_bundles(context, &n);
                CHECK(n == 1 && !memcmp(bundles[0].compiler_tree, tree, 32) &&
                    !memcmp(sh_package_bundle_fingerprint(bundles[0].original), tree, 32));
                CHECK(!strcmp(sh_mpkg_context_carrier(context, 0), key));
                CHECK(sh_mpkg_context_close(&context));
            }
            /* Stripping returns the engine text byte for byte. */
            {
                size_t back_len = 0;
                char *back = sh_mpkg_strip(saved, out_len, &back_len);
                CHECK(back && back_len == mixed_len && !memcmp(back, mixed, mixed_len));
                if (back) HeapFree(GetProcessHeap(), 0, back);
            }
            HeapFree(GetProcessHeap(), 0, saved);
        }
    }
    /* L2: the same delivery over the text budget becomes one reference. */
    {
        char *saved = sh_mpkg_embed_within(mixed, mixed_len, &item, 1, mixed_len + 600, &referenced,
                                           &out_len, &n, err, sizeof err);
        CHECK(saved && referenced);
        if (saved) {
            size_t back_len = 0;
            char *back;
            CHECK(sh_mpkg_scan(saved, out_len, decls, 4) == 0);
            CHECK(sh_mpkg_reference_scan(saved, out_len, scanned, 2) == 1);
            CHECK(!strcmp(scanned[0].key, key) && !strcmp(scanned[0].id, R137_MIXED2_PKG_ID));
            CHECK(!strcmp(scanned[0].name, "R137 mixed-rows") && scanned[0].packed == r137_mixed2_payload_len);
            CHECK(!strcmp(scanned[0].sha256, R137_MIXED2_PAYLOAD_SHA256) && !memcmp(scanned[0].tree, tree, 32));
            CHECK(out_len <= mixed_len + 600);
            /* The smpkg gate does not refuse it; the reference asks. */
            sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_DECLINE);
            CHECK(sh_mpkg_gate(saved, out_len) == 0 && strstr(sh_mpkg_test_last_refusal(), "references a package"));
            sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
            CHECK(sh_mpkg_gate(saved, out_len) == 0);
            CHECK(sh_mpkg_gate(saved, out_len) == 1);
            /* Carrier-free for the engine: the context is empty and the strip
             * is the engine text. */
            context = sh_mpkg_context_open(root, saved, out_len, err, sizeof err);
            CHECK(context && !sh_mpkg_context_count(context));
            CHECK(sh_mpkg_context_close(&context));
            back = sh_mpkg_reference_strip(saved, out_len, &back_len);
            CHECK(back && back_len == mixed_len && !memcmp(back, mixed, mixed_len));
            if (back) HeapFree(GetProcessHeap(), 0, back);
            HeapFree(GetProcessHeap(), 0, saved);
        }
    }
done:
    if (native) HeapFree(GetProcessHeap(), 0, native);
    if (mixed) HeapFree(GetProcessHeap(), 0, mixed);
    sh_mpkg_test_reset();
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--map-context"))
        return inspect_private_map_context(argv[2], argv[3]);
    if (argc == 5 && !strcmp(argv[1], "--historical-probe"))
        return probe_historical_map(argv[2], argv[3], argv[4]);
    if (argc == 5 && !strcmp(argv[1], "--interrupt-install"))
        return interrupt_install_child(argv[2], argv[3], atoi(argv[4]));
    char temp[MAX_PATH], root[MAX_PATH], overrides[MAX_PATH], sub[MAX_PATH];
    DWORD pid = GetCurrentProcessId();
    char err[SH_MPKG_ERR_CAP];
    sh_mpkg_decl decls[16];
    size_t n;

    GetTempPathA(sizeof(temp), temp);
    _snprintf_s(root, sizeof(root), _TRUNCATE, "%ssh_mpkg_test_%lu",
                temp, (unsigned long)pid);
    remove_tree(root);
    CHECK(make_dir(root));
    test_player_notices();
    test_reference_carrier();
    test_r137_levels(root);
    test_reference_context(root);
    test_degrade_leaves_out_dependents(root);
    test_legacy_map_context(root);
    test_legacy_delivery_roundtrip(root);
    test_historical_writer_maps(root);
    test_shared_carriers(root);
    test_shared_historical_recipes(root);
    join(overrides, sizeof overrides, root, "overrides");
    CHECK(make_dir(overrides));
    test_private_map_context(root);
    test_resource_owner_install(root);
    test_same_id_deliveries(root);
    test_batch_install(root);
    test_delivered_variants(root);
    test_refreshed_author_sources(root);
    test_consent_retirement(root, overrides);
    test_serving_disabled_gate(root, overrides);

    /* ---- scan: the no-shard fast path ---------------------------------- */
    n = sh_mpkg_scan("{\"variables\":{\"string\":[]}}", 27, decls, 16);
    CHECK(n == 0);
    /* header-shaped PROSE (in a value, and in an entity name) is not a shard */
    n = sh_mpkg_scan(fix_map_prose, fix_map_prose_len, decls, 16);
    CHECK(n == 0);

    /* ---- scan: the happy map, compact and pretty ----------------------- */
    n = sh_mpkg_scan(fix_map_happy, fix_map_happy_len, decls, 16);
    CHECK(n == 1);
    CHECK(strcmp(decls[0].id, FIX_PKG_ID) == 0);
    {
        char *mixed = _strdup(fix_map_happy), *cursor = mixed;
        unsigned char *payload;
        size_t length, j;
        sh_mpkg_context *context;
        CHECK(mixed);
        if (mixed) {
            while ((cursor = strstr(cursor, FIX_PKG_ID)) != NULL) {
                for (j = 0; j < strlen(FIX_PKG_ID); j++)
                    if (cursor[j] >= 'a' && cursor[j] <= 'z') cursor[j] -= 'a' - 'A';
                cursor += strlen(FIX_PKG_ID);
            }
            CHECK(sh_mpkg_scan(mixed, strlen(mixed), decls, 16) == 1);
            CHECK(!strcmp(decls[0].id, FIX_PKG_ID));
            payload = sh_mpkg_extract(mixed, strlen(mixed), FIX_PKG_ID, &length, err, sizeof(err));
            CHECK(payload && length == fix_payload_len && !memcmp(payload, fix_payload, length));
            if (payload) HeapFree(GetProcessHeap(), 0, payload);
            context = sh_mpkg_context_open(root, mixed, strlen(mixed), err, sizeof(err));
            CHECK(context && sh_mpkg_context_count(context) == 1);
            CHECK(sh_mpkg_context_close(&context));
            free(mixed);
        }
    }
    CHECK(strcmp(decls[0].digest, FIX_DIGEST) == 0);
    CHECK(decls[0].total == FIX_SHARDS);
    CHECK(decls[0].present == FIX_SHARDS);
    CHECK(decls[0].consistent == 1 && decls[0].complete == 1);

    n = sh_mpkg_scan(fix_map_happy_pretty, fix_map_happy_pretty_len, decls,
                     16);
    CHECK(n == 1);
    CHECK(decls[0].complete == 1);

    /* ---- scan: the broken variants ------------------------------------- */
    n = sh_mpkg_scan(fix_map_incomplete, fix_map_incomplete_len, decls,
                     16);
    CHECK(n == 1 && decls[0].present == FIX_SHARDS - 1 && decls[0].complete == 0);
    n = sh_mpkg_scan(fix_map_duplicate, fix_map_duplicate_len, decls,
                     16);
    CHECK(n == 1 && decls[0].consistent == 0 && decls[0].complete == 0);
    n = sh_mpkg_scan(fix_map_inconsistent, fix_map_inconsistent_len, decls,
                     16);
    CHECK(n == 1 && decls[0].consistent == 0);

    /* ---- extract: happy ------------------------------------------------- */
    {
        size_t out_len = 0;
        unsigned char *payload = sh_mpkg_extract(fix_map_happy, fix_map_happy_len,
                                                 FIX_PKG_ID, &out_len, err, sizeof err);
        CHECK(payload != NULL);
        CHECK(out_len == fix_payload_len);
        if (payload && out_len == fix_payload_len)
            CHECK(memcmp(payload, fix_payload, out_len) == 0);
        if (payload) HeapFree(GetProcessHeap(), 0, payload);

        payload = sh_mpkg_extract(fix_map_happy_pretty, fix_map_happy_pretty_len,
                                  FIX_PKG_ID, &out_len, err, sizeof err);
        CHECK(payload != NULL && out_len == fix_payload_len);
        if (payload && out_len == fix_payload_len)
            CHECK(memcmp(payload, fix_payload, out_len) == 0);
        if (payload) HeapFree(GetProcessHeap(), 0, payload);
    }

    /* ---- extract: every reference-error case --------------------------- */
    {
        size_t out_len = 0;
        CHECK(sh_mpkg_extract(fix_map_incomplete, fix_map_incomplete_len, FIX_PKG_ID,
                              &out_len, err, sizeof err) == NULL);
        CHECK(strstr(err, "incomplete") != NULL);
        CHECK(sh_mpkg_extract(fix_map_duplicate, fix_map_duplicate_len, FIX_PKG_ID,
                              &out_len, err, sizeof err) == NULL);
        CHECK(strstr(err, "duplicate") != NULL);
        CHECK(sh_mpkg_extract(fix_map_inconsistent, fix_map_inconsistent_len, FIX_PKG_ID,
                              &out_len, err, sizeof err) == NULL);
        CHECK(strstr(err, "inconsistent") != NULL);
        CHECK(sh_mpkg_extract(fix_map_wrongdigest, fix_map_wrongdigest_len, FIX_PKG_ID,
                              &out_len, err, sizeof err) == NULL);
        CHECK(strstr(err, "failed its digest") != NULL);
        CHECK(sh_mpkg_extract(fix_map_corrupt, fix_map_corrupt_len, FIX_PKG_ID,
                              &out_len, err, sizeof err) == NULL);
        CHECK(strstr(err, "failed its digest") != NULL);
        CHECK(sh_mpkg_extract(fix_map_happy, fix_map_happy_len, "nope",
                              &out_len, err, sizeof err) == NULL);
        CHECK(strstr(err, "not present") != NULL);
    }

    /* ---- unpack: happy --------------------------------------------------- */
    {
        unsigned files = 0;
        join(sub, sizeof sub, root, "unpack");
        CHECK(sh_mpkg_unpack(fix_payload, fix_payload_len, sub, &files, err, sizeof err) == 1);
        CHECK(files == FIX_FILE_COUNT);
        check_unpacked_members(sub);
    }

    /* ---- unpack: unsafe member paths refuse BEFORE anything is written -- */
    {
        unsigned files = 0;
        char escaped[MAX_PATH];
        join(sub, sizeof sub, root, "trav");
        CHECK(sh_mpkg_unpack(fix_payload_traversal, fix_payload_traversal_len, sub,
                             &files, err, sizeof err) == 0);
        CHECK(strstr(err, "unsafe member path") != NULL);
        CHECK(!dir_exists(sub));                       /* nothing was created */
        join(escaped, sizeof escaped, root, "evil.txt");
        CHECK(!file_exists(escaped));                  /* and nothing escaped */

        join(sub, sizeof sub, root, "abs");
        CHECK(sh_mpkg_unpack(fix_payload_absolute, fix_payload_absolute_len, sub,
                             &files, err, sizeof err) == 0);
        CHECK(strstr(err, "unsafe member path") != NULL);
        CHECK(!dir_exists(sub));
    }

    /* ---- gate: refuses when the boot snapshot is missing ----------------- */
    sh_mpkg_test_reset();
    CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 0);
    CHECK(strstr(sh_mpkg_test_last_refusal(), "snapshot") != NULL);
    CHECK(!sh_mpkg_activation_ready());

    /* ---- gate: no-shard maps pass without a snapshot too ----------------- */
    CHECK(sh_mpkg_gate("{\"variables\":{\"string\":[]}}", 27) == 1);
    CHECK(sh_mpkg_gate(fix_map_prose, fix_map_prose_len) == 1);

    /* ---- gate: missing package + user DECLINES --------------------------- */
    sh_mpkg_test_reset();
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_DECLINE);
    sh_mpkg_boot_capture(root);
    CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 0);
    CHECK(sh_mpkg_test_session_installed_count() == 0);
    join(sub, sizeof sub, overrides, FIX_PKG_ID);
    CHECK(!dir_exists(sub));                           /* declined = not installed */
    CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 0);   /* another load is declined again */

    /* Gate: acceptance installs the package, but registration is still pending. */
    sh_mpkg_test_reset();
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);
    sh_mpkg_boot_capture(root);
    CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 0);   /* THIS load still refused */
    CHECK(sh_mpkg_test_session_installed_count() == 1);
    CHECK(installed_package(root, FIX_PKG_ID, sub));
    CHECK(dir_exists(sub));
    check_unpacked_members(sub);
    {
        char marker[MAX_PATH];
        join(marker, sizeof marker, sub, "package.json");
        CHECK(file_exists(marker));
        join(marker, sizeof marker, sub, "smpkg.digest");
        CHECK(!file_exists(marker));
    }
    /* The registration stub still reports pending, so another load must be
     * refused with registration wording rather than a restart requirement. */
    CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 0);
    CHECK(strstr(sh_mpkg_test_last_refusal(), "registration is incomplete or failed") != NULL);
    CHECK(strstr(sh_mpkg_test_last_refusal(), "restart") == NULL);
    g_registration_ready = 1;
    CHECK(!sh_mpkg_activation_ready()); /* Installation has not committed. */
    complete_install();
    CHECK(sh_mpkg_activation_ready());
    CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 1);
    {
        char *repacked;
        size_t repacked_len;
        unsigned char original[32], alternate[32];
        CHECK(sh_package_archive_identity(fix_payload, fix_payload_len, NULL, original, err, sizeof(err)));
        CHECK(sh_package_archive_identity(fix_payload_repacked, fix_payload_repacked_len, NULL, alternate, err, sizeof(err)));
        CHECK(!memcmp(original, alternate, 32));
        repacked = sh_mpkg_embed(fix_map_happy, fix_map_happy_len, FIX_PKG_ID,
                                 fix_payload_repacked, fix_payload_repacked_len, &repacked_len, err, sizeof(err));
        CHECK(repacked);
        if (repacked) {
            /* A different ZIP encoder and omitted directory rows describe the
             * same installed source, both during the session and after boot. */
            CHECK(sh_mpkg_gate(repacked, repacked_len) == 1);
            sh_mpkg_test_reset(); sh_mpkg_boot_capture(root);
            CHECK(sh_mpkg_gate(repacked, repacked_len) == 1);
            CHECK(sh_mpkg_gate(fix_map_corrupt, fix_map_corrupt_len) == 0);
            HeapFree(GetProcessHeap(), 0, repacked);
        }
    }
    {
        /* One successful session package must not admit a second missing one. */
        size_t combined_length = 0;
        char error[SH_MPKG_ERR_CAP];
        char *combined = sh_mpkg_embed(fix_map_happy, fix_map_happy_len, "another",
                                      fix_payload, sizeof(fix_payload), &combined_length,
                                      error, sizeof(error));
        CHECK(combined != NULL);
        if (combined) {
            CHECK(sh_mpkg_gate(combined, combined_length) == 0);
            HeapFree(GetProcessHeap(), 0, combined);
        }
    }
    g_registration_ready = 0;

    /* ---- gate: once the package is in the captured set, the map passes ---- */
    sh_mpkg_test_reset();
    sh_mpkg_boot_capture(root);   /* capture the installed package */
    CHECK(!sh_mpkg_activation_ready());
    CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 0);
    g_registration_ready = 1; g_compile_ready = 0;
    CHECK(!sh_mpkg_activation_ready());
    CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 0);
    g_compile_ready = 1;
    CHECK(sh_mpkg_activation_ready());
    CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 1);
    /* control in the same state: a different-version map still refuses */
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_DECLINE);
    CHECK(sh_mpkg_gate(fix_map_wrongdigest, fix_map_wrongdigest_len) == 0);

    /* Folder labels do not affect exact authored identity; source edits do. */
    {
        char root2[MAX_PATH], ov2[MAX_PATH], pkg2[MAX_PATH], marker2[MAX_PATH];
        _snprintf_s(root2, sizeof root2, _TRUNCATE, "%ssh_mpkg_test2_%lu",
                    temp, (unsigned long)pid);
        remove_tree(root2);
        CHECK(make_dir(root2));
        join(ov2, sizeof ov2, root2, "overrides");
        CHECK(make_dir(ov2));
        join(pkg2, sizeof pkg2, ov2, "testpkg");   /* map declares demons-testpkg */
        { unsigned files;
          CHECK(sh_mpkg_unpack(fix_payload_repacked, fix_payload_repacked_len, pkg2, &files, err, sizeof(err))); }
        sh_mpkg_test_reset();
        sh_mpkg_boot_capture(root2);
        CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 1);
        join(marker2, sizeof marker2, pkg2, "package.json");
        CHECK(touch(marker2, "{\"id\":\"demons-testpkg\",\"name\":\"Changed package\"}"));
        sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_DECLINE);
        CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 0);
        remove_tree(root2);
    }

    /* ---- gate: a hostile payload refuses the load AND never installs ----- */
    {
        char root3[MAX_PATH], ov3[MAX_PATH], evil3[MAX_PATH];
        _snprintf_s(root3, sizeof root3, _TRUNCATE, "%ssh_mpkg_test3_%lu",
                    temp, (unsigned long)pid);
        remove_tree(root3);
        CHECK(make_dir(root3));
        join(ov3, sizeof ov3, root3, "overrides");
        CHECK(make_dir(ov3));
        sh_mpkg_test_reset();
        sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT);   /* even consenting... */
        sh_mpkg_boot_capture(root3);
        CHECK(sh_mpkg_gate(fix_map_evil, fix_map_evil_len) == 0);   /* refused, no crash */
        CHECK(sh_mpkg_test_session_installed_count() == 0);         /* ...installs nothing */
        join(evil3, sizeof evil3, ov3, "evil-pkg");
        CHECK(!dir_exists(evil3));
        remove_tree(root3);
    }

    /* Strip embedded shards before handing the map to the engine: an 8 KiB
     * shard exceeds the playtest's 4 KiB message budget. */
    check_strip(fix_map_happy, fix_map_happy_len, 1);
    check_strip(fix_map_happy_pretty, fix_map_happy_pretty_len, 1);

    /* A broken payload strips too: the shards are unusable either way, and leaving them in is
     * exactly what makes the map unplayable. */
    check_strip(fix_map_incomplete, fix_map_incomplete_len, 1);
    check_strip(fix_map_duplicate, fix_map_duplicate_len, 1);
    check_strip(fix_map_wrongdigest, fix_map_wrongdigest_len, 1);

    /* No payload, no rewrite -- NULL means "use the original buffer", and an ordinary map must
     * never be copied, let alone edited, by this feature. */
    check_strip("{\"variables\":{\"string\":[]}}", 27, 0);
    check_strip(fix_map_prose, fix_map_prose_len, 0);   /* prose is an author's words, not ours */

    /* Read embedded output through the production scanner and extractor. */
    {
        unsigned char small[777], big[SH_MPKG_SHARD_CHARS * 2];   /* one shard, then several */
        size_t i;
        for (i = 0; i < sizeof small; i++) small[i] = (unsigned char)(i * 7 + 3);
        for (i = 0; i < sizeof big; i++) big[i] = (unsigned char)((i * 31) ^ (i >> 5));

        check_round_trip(EMPTY_MAP, small, sizeof small, 1);
        check_round_trip(ONE_VAR_MAP, small, sizeof small, 2);   /* the map's own var is kept */

        /* A multi-shard payload: every shard must carry the same total and digest, or the
         * consumer refuses the set as inconsistent. */
        {
            char err[SH_MPKG_ERR_CAP];
            size_t out_len = 0;
            char *out = sh_mpkg_embed(EMPTY_MAP, strlen(EMPTY_MAP), "demons-testpkg",
                                      big, sizeof big, &out_len, err, sizeof err);
            sh_mpkg_decl decls[16];
            CHECK(out != NULL);
            if (out) {
                CHECK(sh_mpkg_scan(out, out_len, decls, 16) == 1);
                CHECK(decls[0].total > 1);              /* it really did split */
                CHECK(decls[0].complete == 1 && decls[0].consistent == 1);
                CHECK(alloc_count_string(out) == (int)decls[0].total);
                HeapFree(GetProcessHeap(), 0, out);
            }
        }

        /* Re-embedding REPLACES. A map saved ten times must not carry ten packages. */
        {
            char err[SH_MPKG_ERR_CAP];
            size_t once_len = 0, twice_len = 0;
            char *once = sh_mpkg_embed(EMPTY_MAP, strlen(EMPTY_MAP), "demons-testpkg",
                                       small, sizeof small, &once_len, err, sizeof err);
            CHECK(once != NULL);
            if (once) {
                char *twice = sh_mpkg_embed(once, once_len, "demons-testpkg",
                                            small, sizeof small, &twice_len, err, sizeof err);
                CHECK(twice != NULL);
                if (twice) {
                    CHECK(twice_len == once_len);       /* byte-for-byte the same map */
                    CHECK(memcmp(twice, once, once_len) == 0);
                    HeapFree(GetProcessHeap(), 0, twice);
                }
                HeapFree(GetProcessHeap(), 0, once);
            }
        }

        /* Re-embedding one package must preserve the other package's shards. */
        {
            char err[SH_MPKG_ERR_CAP];
            size_t one_len = 0, two_len = 0;
            char *one = sh_mpkg_embed(EMPTY_MAP, strlen(EMPTY_MAP), "pkg-alpha",
                                      small, sizeof small, &one_len, err, sizeof err);
            CHECK(one != NULL);
            if (one) {
                char *two = sh_mpkg_embed(one, one_len, "pkg-beta",
                                          big, sizeof big, &two_len, err, sizeof err);
                CHECK(two != NULL);
                if (two) {
                    sh_mpkg_decl d[16];
                    size_t n = sh_mpkg_scan(two, two_len, d, 16);
                    size_t k, seen_alpha = 0, seen_beta = 0, total_shards = 0;
                    CHECK(n == 2);
                    for (k = 0; k < n; k++) {
                        CHECK(d[k].complete == 1 && d[k].consistent == 1);
                        total_shards += d[k].total;
                        if (strcmp(d[k].id, "pkg-alpha") == 0) seen_alpha = 1;
                        if (strcmp(d[k].id, "pkg-beta") == 0) seen_beta = 1;
                    }
                    CHECK(seen_alpha == 1);
                    CHECK(seen_beta == 1);
                    CHECK(alloc_count_string(two) == (int)total_shards);

                    /* and each extracts back to its OWN payload */
                    {
                        size_t ra = 0, rb = 0;
                        unsigned char *a = sh_mpkg_extract(two, two_len, "pkg-alpha", &ra,
                                                           err, sizeof err);
                        unsigned char *b = sh_mpkg_extract(two, two_len, "pkg-beta", &rb,
                                                           err, sizeof err);
                        CHECK(a != NULL && ra == sizeof small);
                        if (a && ra == sizeof small) CHECK(memcmp(a, small, ra) == 0);
                        CHECK(b != NULL && rb == sizeof big);
                        if (b && rb == sizeof big) CHECK(memcmp(b, big, rb) == 0);
                        if (a) HeapFree(GetProcessHeap(), 0, a);
                        if (b) HeapFree(GetProcessHeap(), 0, b);
                    }
                    HeapFree(GetProcessHeap(), 0, two);
                }
                HeapFree(GetProcessHeap(), 0, one);
            }
        }

        /* A map with no variables block cannot carry a payload, and must say so rather than
         * produce something the consumer will choke on. */
        {
            char err[SH_MPKG_ERR_CAP];
            size_t n = 0;
            char *bad = sh_mpkg_embed("{\"name\":\"m\"}", 12, "demons-testpkg",
                                      small, sizeof small, &n, err, sizeof err);
            CHECK(bad == NULL);
            CHECK(err[0] != '\0');
            if (bad) HeapFree(GetProcessHeap(), 0, bad);
        }
    }

    {
        char package[MAX_PATH], marker[MAX_PATH];
        for (int i = 0; i < 65; i++) {
            _snprintf_s(package, sizeof(package), _TRUNCATE,
                        "%s\\overflow-%02d", overrides, i);
            CHECK(make_dir(package));
            join(marker, sizeof(marker), package, "package.json");
            CHECK(touch(marker, "{}"));
        }
        sh_mpkg_test_reset();
        sh_mpkg_boot_capture(root);
        /* Malformed local packages are excluded by name; they do not block
         * startup identities for healthy packages or map admission. */
        CHECK(sh_mpkg_startup_ready());
        g_registration_ready = 0;
        CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 0);
        g_registration_ready = 1;
        CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 1);
        for (int i = 0; i < 65; i++) {
            _snprintf_s(package, sizeof(package), _TRUNCATE,
                        "%s\\overflow-%02d", overrides, i);
            remove_tree(package);
        }
        /* A source that cannot be read is a storage failure, not a package
         * defect: capture fails rather than publishing a partial inventory. */
        {
            char locked_file[MAX_PATH];
            HANDLE lock;
            _snprintf_s(package, sizeof(package), _TRUNCATE, "%s\\locked", overrides);
            CHECK(make_dir(package));
            join(marker, sizeof(marker), package, "package.json");
            CHECK(touch(marker, "{\"id\":\"locked\",\"name\":\"Locked\"}"));
            join(locked_file, sizeof(locked_file), package, "notes.txt");
            CHECK(touch(locked_file, "held open without sharing"));
            lock = CreateFileA(locked_file, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            CHECK(lock != INVALID_HANDLE_VALUE);
            sh_mpkg_test_reset();
            sh_mpkg_boot_capture(root);
            CHECK(!sh_mpkg_startup_ready());
            CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 0);
            CHECK(strstr(sh_mpkg_test_last_refusal(), "boot package snapshot is missing") != NULL);
            CloseHandle(lock);
            /* A failed capture can retry once the source is readable again.
             * Admission still requires native activation of the recovered sources. */
            sh_mpkg_boot_capture(root);
            CHECK(sh_mpkg_startup_ready());
            g_registration_ready = 0;
            CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 0);
            g_registration_ready = 1;
            CHECK(sh_mpkg_gate(fix_map_happy, fix_map_happy_len) == 1);
            remove_tree(package);
        }
    }
    remove_tree(root);

    if (g_failed) {
        fprintf(stderr, "%d check(s) FAILED\n", g_failed);
        return 1;
    }
    printf("map_package_test: all checks passed\n");
    return 0;
}
