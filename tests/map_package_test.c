/* Tests map-package shard reconstruction, safe extraction and the load gate
 * against generated reference fixtures (map_package_fixtures.h). Registration
 * and consent are stubbed; this suite does not verify live engine installation. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map_package.h"
#include "package_archive.h"
#include "package_runtime.h"
#include "engine_dialog.h"
#include "map_package_fixtures.h"

/* Runtime registration and re-arm are stubbed; the live engine path is outside this suite. */
static int g_registration_ready, g_rearm_requests;
static const sh_package_compilation *g_compiled;
static const sh_package_compilation *g_effective_map;
static int g_compile_ready = 1;
const sh_package_compilation *sh_package_runtime_acquire(void) { return g_effective_map ? g_effective_map : g_compiled; }
const sh_package_compilation *sh_package_runtime_library_acquire(void) { return g_compiled; }
void sh_package_runtime_release(void) {}
int sh_package_runtime_ready(void) { return g_compile_ready; }
int sh_package_runtime_admission_ready(void) { return g_compile_ready && g_registration_ready; }
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

/* Most assertions are on returned state; supersession also has to be readable
 * in the log, because that is where the user learns a package was replaced. */
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
    CHECK(button_set == 1 || (button_set == 6 && text && strstr(text, "Install")));
    CHECK(g_dialog_idle);
    strcpy_s(g_dialog_text, sizeof(g_dialog_text), text);
    g_dialog_asks++;
    return g_dialog_raise_fails ? 0 : 1;
}
int sh_engine_dialog_poll(int ticket) { CHECK(ticket == 1); return g_dialog_answer; }
void sh_engine_dialog_release(int ticket) { (void)ticket; }

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
    g_registration_ready = g_rearm_requests = batch_publish_calls = 0;
    CHECK(!sh_mpkg_gate(map, length));
    CHECK(batch_publish_calls == 1 && sh_mpkg_test_session_installed_count() == 2 && g_rearm_requests == 1);
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
    g_rearm_requests = batch_publish_calls = 0;
    CHECK(!sh_mpkg_gate(map, length));
    CHECK(installed_count(client) == 0 && !batch_publish_calls && !g_rearm_requests);

    /* A failed commit installs none, discards temporary files and allows retry. */
    sh_mpkg_test_reset(); sh_mpkg_boot_capture(client);
    sh_mpkg_test_set_consent_mode(SH_MPKG_CONSENT_ACCEPT); sh_mpkg_test_set_publish(batch_publish);
    g_rearm_requests = batch_publish_calls = 0; batch_fail = 1;
    CHECK(!sh_mpkg_gate(map, length));
    CHECK(batch_publish_calls == 1 && !g_rearm_requests && !sh_mpkg_test_session_installed_count());
    CHECK(installed_count(client) == 0 && !dir_exists(batch_staging) && !dir_exists(batch_destination));
    batch_fail = 0;
    CHECK(!sh_mpkg_gate(map, length));
    CHECK(installed_count(client) == 2 && g_rearm_requests == 1);
    sh_mpkg_report_error("specific activation failure");
    CHECK(sh_mpkg_activation_cancel());
    CHECK(installed_count(client) == 0 && !sh_mpkg_test_session_installed_count());
    g_dialog_idle = 1; g_dialog_asks = 0; g_dialog_answer = SH_ENGINE_DIALOG_PENDING;
    sh_mpkg_consent_poll();
    CHECK(g_dialog_asks == 1 && !strcmp(g_dialog_text, "specific activation failure"));
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

/* One package identity can only supply one set of bytes: the library would
 * otherwise compile the same package twice with contradictory content. A second
 * delivery of the same identity supersedes the delivered copy inside the same
 * transaction; the user's own authored package is never moved. */
static void test_supersession(const char *root)
{
    char author[MAX_PATH], client[MAX_PATH], overrides[MAX_PATH], authored[MAX_PATH], path[MAX_PATH];
    unsigned char first_print[32], second_print[32], third_print[32];
    char *first = NULL, *second = NULL, *third = NULL;
    size_t first_length = 0, second_length = 0, third_length = 0;
    join(author, sizeof(author), root, "supersede-author");
    join(client, sizeof(client), root, "supersede-client"); CHECK(make_dir(client));
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
    CHECK(strstr(g_log, "superseding the previously delivered package"));
    /* Exactly one variant is visible even before the commit. */
    CHECK(installed_count(client) == 1 && variant_matches(client, "demons-variant", second_print));
    complete_install();
    CHECK(installed_count(client) == 1 && variant_matches(client, "demons-variant", second_print));
    CHECK(sh_mpkg_gate(second, second_length));

    /* A canceled supersession puts the previous delivery back untouched. */
    CHECK(!sh_mpkg_gate(third, third_length));
    CHECK(installed_count(client) == 1 && variant_matches(client, "demons-variant", third_print));
    CHECK(sh_mpkg_activation_cancel());
    CHECK(installed_count(client) == 1 && variant_matches(client, "demons-variant", second_print));
    /* And the restored delivery is still a complete, usable installation. */
    CHECK(sh_mpkg_gate(second, second_length));

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
    CHECK(installed_count(client) == 2 && variant_matches(client, "demons-variant", second_print) >= 0);
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
            join(folder, sizeof(folder), sh_mpkg_context_root(a), "overrides\\batch-00");
            check_batch_package(folder, 0);
            join(folder, sizeof(folder), sh_mpkg_context_root(a), "overrides\\batch-01");
            check_batch_package(folder, 1);
            /* A held source prevents deletion, and close retains retry ownership. */
            join(package, sizeof(package), folder, "package.json");
            {
                HANDLE held = CreateFileA(package, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                CHECK(held != INVALID_HANDLE_VALUE);
                CHECK(!sh_mpkg_context_close(&a) && a);
                /* After provider retirement, a locked cache no longer holds
                 * the map context. A different map can prepare immediately. */
                sh_mpkg_context_retire(&a); CHECK(!a && dir_exists(folder));
                b = sh_mpkg_context_open(root, fix_map_happy, fix_map_happy_len, error, sizeof(error));
                CHECK(b && sh_mpkg_context_count(b) == 1);
                CHECK(sh_mpkg_context_close(&b));
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
        snprintf(path, sizeof(path), "%s/overrides/old-boss/assets/generated/decls/entitydef/boss.decl", sh_mpkg_context_root(context));
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

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--map-context"))
        return inspect_private_map_context(argv[2], argv[3]);
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
    test_legacy_map_context(root);
    join(overrides, sizeof overrides, root, "overrides");
    CHECK(make_dir(overrides));
    test_private_map_context(root);
    test_resource_owner_install(root);
    test_batch_install(root);
    test_supersession(root);
    test_refreshed_author_sources(root);
    test_consent_retirement(root, overrides);

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
