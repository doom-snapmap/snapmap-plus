// publishing_key_dispatch_test.cpp -- every publishing key request of the Settings page, end to end without the game
// (R-LB1, R-LV9 F4): the page (publishing_key.js, run in node by publishing_key_page_driver.js) posts its requests;
// each is read the way the window's host reads it (mods_message.h: the route, the action's code, the backup's and
// restore's file from the picker, the answer's envelope, the code on_message, the worker and ui_mods_finish run);
// the worker's part goes to the real Mods slot (modui.c sh_modui_slot RUN and FETCH) with the key bound to modclaim
// as the product binds it; and the answer goes back to the page. Checked: status answers the page, Not now saves the
// offer as done in the mods folder's record, a backup writes its file, a restore reads it back, a closed picker
// leaves the dialog usable, and a message the host cannot route fails here rather than vanishing.
//
//   publishing_key_dispatch_test.exe <node.exe> <publishing_key_page_driver.js>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "mods_message.h"

extern "C" {
int pk_backend_setup(void);
const char *pk_backend_backup_path(void);
int pk_backend_run(int action, const char *arg, char **doc);
int pk_backend_saved_offer(char *out, size_t cap);
void pk_backend_teardown(void);
}

static int failures;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

// The page, as a child process talking over pipes.
struct Page {
    HANDLE in_w = nullptr, out_r = nullptr;
    PROCESS_INFORMATION pi = {};
    std::string pending;
    bool start(const char *node, const char *driver)
    {
        SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
        HANDLE in_r, out_w;
        if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) return false;
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si = {};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_r;
        si.hStdOutput = out_w;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        std::string cmd = std::string("\"") + node + "\" \"" + driver + "\"";
        std::vector<char> line(cmd.begin(), cmd.end());
        line.push_back(0);
        BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
        CloseHandle(in_r);
        CloseHandle(out_w);
        return ok != 0;
    }
    bool read_line(std::string &line)
    {
        for (;;) {
            size_t nl = pending.find('\n');
            if (nl != std::string::npos) {
                line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(out_r, buf, sizeof buf, &got, nullptr) || !got) return false;
            pending.append(buf, got);
        }
    }
    // The lines up to END: the page's posts, its toasts and its state.
    std::vector<std::string> until_end()
    {
        std::vector<std::string> lines;
        std::string l;
        while (read_line(l) && l != "END") lines.push_back(l);
        return lines;
    }
    std::vector<std::string> send(const std::string &command)
    {
        std::string s = command + "\n";
        DWORD put = 0;
        WriteFile(in_w, s.data(), (DWORD)s.size(), &put, nullptr);
        return until_end();
    }
    void stop()
    {
        send_quit();
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(in_w);
        CloseHandle(out_r);
    }
    void send_quit() { DWORD put = 0; WriteFile(in_w, "QUIT\n", 5, &put, nullptr); }
};

static std::wstring wide(const std::string &s) { return sh_mods_message::wide(s); }
static std::string narrow(const std::wstring &w) { return sh_webview_json::to_utf8(w); }

// What the picker answers this time: a path, or the picker closed.
static bool g_pick_ok = true;
static std::vector<std::string> g_routed;     // the backend arguments the host ran ("backup\t<path>\t..." as sent)
static int g_dropped;                         // messages the host could not route

// The host's part for every line the page printed: route the posts, run them as the worker runs them, answer the page.
// Returns the toasts and states seen.
static std::vector<std::string> host(Page &page, std::vector<std::string> lines)
{
    std::vector<std::string> seen;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &l = lines[i];
        if (l.compare(0, 5, "POST ")) { seen.push_back(l); continue; }
        std::wstring json = wide(l.substr(5)), action, warg;
        if (!sh_mods_message::parse_action(json, action, warg)) {
            std::printf("  page -> host: a message with no \"cmd\" route: the host drops it\n");   // never its text: a passphrase
            g_dropped++;
            continue;
        }
        int code = sh_mods_message::action_code(action);
        CHECK(code == SH_MODS_ACT_KEY);
        std::string arg = narrow(warg);
        int run_rc = 0;
        std::string doc;
        if (sh_mods_message::key_file_request(arg, [](bool save, std::wstring &picked) {
                (void)save;
                if (!g_pick_ok) return false;
                picked = wide(pk_backend_backup_path());
                return true;
            }) == -3) {
            run_rc = -3;
        } else {
            char *d = nullptr;
            run_rc = pk_backend_run(code, arg.c_str(), &d) ? 1 : 0;
            if (d) { doc = d; std::free(d); }
            g_routed.push_back(arg.substr(0, arg.find('\t')));
        }
        std::string answer = narrow(sh_mods_message::finish_message(code, run_rc, false, doc));
        std::printf("  page -> host: %s -> %s\n", arg.substr(0, arg.find('\t')).c_str(),
                    run_rc == 1 ? "answered" : run_rc == -3 ? "picker closed" : "refused");
        std::vector<std::string> more = page.send("MSG " + answer);
        lines.insert(lines.end(), more.begin(), more.end());
    }
    return seen;
}

static std::string state(Page &page)
{
    std::vector<std::string> l = page.send("STATE");
    for (const std::string &s : l) if (!s.compare(0, 6, "STATE ")) return s.substr(6);
    return "";
}

static bool has(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

int main(int argc, char **argv)
{
    Page page;
    char offer[32];
    if (argc < 3 || !argv[1][0]) { std::printf("publishing_key_dispatch_test: no node or driver given; skipped\n"); return 0; }
    CHECK(pk_backend_setup());
    CHECK(pk_backend_saved_offer(offer, sizeof offer) && !std::strcmp(offer, "pending"));
    CHECK(page.start(argv[1], argv[2]));

    std::printf("-- the key's state, asked at start and when Settings opens, reaches the page\n");
    host(page, page.until_end());
    CHECK(has(state(page), "\"key\":{\"exists\":true"));
    g_routed.clear();
    host(page, page.send("SETTINGS"));
    CHECK(g_routed.size() == 1 && g_routed[0] == "status");

    std::printf("-- Not now saves the offer as answered\n");
    host(page, page.send("MSG {\"kind\":\"mods\",\"data\":{\"publishingKey\":{\"exists\":true,\"id\":\"x\",\"offer\":true}}}"));
    CHECK(has(state(page), "\"open\":true,\"page\":\"offer\""));
    host(page, page.send("NOTNOW"));
    {
        std::string st = state(page);
        CHECK(has(st, "\"open\":false") && has(st, "\"laterPending\":false") && has(st, "\"offer\":false"));
    }
    CHECK(pk_backend_saved_offer(offer, sizeof offer) && !std::strcmp(offer, "done"));
    std::printf("  the saved offer: %s\n", offer);

    std::printf("-- a backup asks the picker, writes the file and closes the dialog with its sentence\n");
    DeleteFileA(pk_backend_backup_path());
    {
        std::vector<std::string> seen = host(page, page.send("BACKUP correct horse battery"));
        std::string st = state(page), toasts;
        for (const std::string &s : seen) toasts += s;
        CHECK(GetFileAttributesA(pk_backend_backup_path()) != INVALID_FILE_ATTRIBUTES);
        CHECK(has(st, "\"open\":false") && has(st, "\"busy\":false"));
        CHECK(has(toasts, "is saved in publishing-key.snapmap-plus-key"));
    }

    std::printf("-- a closed picker leaves the dialog open and usable\n");
    g_pick_ok = false;
    host(page, page.send("BACKUP correct horse battery"));
    CHECK(has(state(page), "\"open\":true,\"page\":\"backup\",\"busy\":false"));
    g_pick_ok = true;
    host(page, page.send("NOTNOW"));

    std::printf("-- a restore reads the backup back\n");
    {
        std::vector<std::string> seen = host(page, page.send("RESTORE correct horse battery"));
        std::string st = state(page), toasts;
        for (const std::string &s : seen) toasts += s;
        CHECK(has(st, "\"open\":false") && has(st, "\"busy\":false"));
        CHECK(has(toasts, "now publishes with the key from this backup"));
    }

    std::printf("-- a wrong passphrase is said in the dialog\n");
    {
        host(page, page.send("RESTORE not the passphrase"));
        std::string st = state(page);
        CHECK(has(st, "\"open\":true,\"page\":\"restore\",\"busy\":false") && !has(st, "\"error\":\"\""));
    }

    CHECK(g_dropped == 0);   // every message the page posted was one the host routes
    page.stop();
    pk_backend_teardown();
    std::printf(failures ? "publishing_key_dispatch_test: %d failure(s)\n" : "publishing_key_dispatch_test: all checks passed\n",
                failures);
    return failures ? 1 : 0;
}
