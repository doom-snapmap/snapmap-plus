/* webview_profile_test -- the companion window's WebView2 profile folder
 * (webview_profile.h) against a temporary %LOCALAPPDATA%: a fresh profile is
 * created under the cache folder, an old snapmap-plus\webview2 folder is
 * renamed into it with its content intact (the same files, not copies), a
 * second start finds it there, an old folder next to an existing new one is
 * left alone, and a failed rename keeps using the old folder. */
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "webview_profile.h"

static int g_failed, g_checks;
#define CHECK(expr) do { g_checks++; if (!(expr)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); g_failed++; } } while (0)

static void write_file(const std::wstring &path, const char *text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD put = 0;
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, text, (DWORD)std::strlen(text), &put, nullptr);
    CloseHandle(h);
}

static bool file_is(const std::wstring &path, const char *text)
{
    char buf[256] = {};
    DWORD got = 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ReadFile(h, buf, sizeof buf - 1, &got, nullptr);
    CloseHandle(h);
    return std::string(buf, got) == text;
}

static unsigned long long file_id(const std::wstring &path)
{
    BY_HANDLE_FILE_INFORMATION info;
    unsigned long long id = 0;
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (GetFileInformationByHandle(h, &info)) id = ((unsigned long long)info.nFileIndexHigh << 32) | info.nFileIndexLow;
    CloseHandle(h);
    return id;
}

static void remove_tree(const std::wstring &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName, path = dir + L"\\" + name;
            if (name == L"." || name == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(path);
            else { SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(path.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static std::wstring fresh_local(const wchar_t *tag)
{
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    std::wstring local = std::wstring(base) + L"smp_webview_profile_" + std::to_wstring(GetCurrentProcessId()) + L"_" + tag;
    remove_tree(local);
    CreateDirectoryW(local.c_str(), nullptr);
    return local;
}

int main()
{
    using namespace sh_webview_profile;
    int how = -1;
    DWORD error = 0;

    std::printf("-- a first start creates cache\\webview2\n");
    {
        std::wstring local = fresh_local(L"a");
        std::wstring got = choose(local, &how, &error);
        CHECK(got == local + L"\\snapmap-plus\\cache\\webview2");
        CHECK(how == created && is_dir(got));
        CHECK(!is_dir(local + L"\\snapmap-plus\\webview2"));
        got = choose(local, &how);
        CHECK(how == existing && got == local + L"\\snapmap-plus\\cache\\webview2");
        remove_tree(local);
    }

    std::printf("-- an old webview2 folder is renamed into the cache folder, content intact\n");
    {
        std::wstring local = fresh_local(L"b");
        std::wstring old = local + L"\\snapmap-plus\\webview2";
        CreateDirectoryW((local + L"\\snapmap-plus").c_str(), nullptr);
        CreateDirectoryW(old.c_str(), nullptr);
        CreateDirectoryW((old + L"\\EBWebView").c_str(), nullptr);
        write_file(old + L"\\EBWebView\\Local State", "profile bytes");
        unsigned long long before = file_id(old + L"\\EBWebView\\Local State");
        std::wstring got = choose(local, &how, &error);
        CHECK(how == moved && got == local + L"\\snapmap-plus\\cache\\webview2");
        CHECK(!is_dir(old));
        CHECK(file_is(got + L"\\EBWebView\\Local State", "profile bytes"));
        CHECK(before && file_id(got + L"\\EBWebView\\Local State") == before);   /* the same file: renamed, not copied */
        got = choose(local, &how);
        CHECK(how == existing);
        remove_tree(local);
    }

    std::printf("-- an old folder next to an existing new one is left alone\n");
    {
        std::wstring local = fresh_local(L"c");
        std::wstring old = local + L"\\snapmap-plus\\webview2";
        CreateDirectoryW((local + L"\\snapmap-plus").c_str(), nullptr);
        CreateDirectoryW((local + L"\\snapmap-plus\\cache").c_str(), nullptr);
        CreateDirectoryW((local + L"\\snapmap-plus\\cache\\webview2").c_str(), nullptr);
        CreateDirectoryW(old.c_str(), nullptr);
        write_file(old + L"\\keep.txt", "old");
        std::wstring got = choose(local, &how);
        CHECK(how == existing && got == local + L"\\snapmap-plus\\cache\\webview2");
        CHECK(file_is(old + L"\\keep.txt", "old"));
        remove_tree(local);
    }

    std::printf("-- a failed rename keeps the old folder for this start\n");
    {
        std::wstring local = fresh_local(L"d");
        std::wstring old = local + L"\\snapmap-plus\\webview2";
        CreateDirectoryW((local + L"\\snapmap-plus").c_str(), nullptr);
        CreateDirectoryW(old.c_str(), nullptr);
        write_file(old + L"\\held.txt", "held");
        /* A plain file named cache makes cache\webview2 impossible. */
        write_file(local + L"\\snapmap-plus\\cache", "not a folder");
        std::wstring got = choose(local, &how, &error);
        CHECK(how == kept_old && got == old && error != 0);
        CHECK(file_is(old + L"\\held.txt", "held"));
        remove_tree(local);
    }

    std::printf("webview_profile_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
