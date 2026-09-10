/* JSON field access and text encoding for the WebView message boundary. */
#include <windows.h>
#include <cwchar>
#include "webview_json.h"

namespace sh_webview_json {

/* String and JSON helpers. */
std::wstring escape_wide(const char *utf8)
{
    int wl = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w;
    if (wl > 0) { w.resize(wl - 1); if (wl > 1) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], wl); }
    std::wstring o; o.reserve(w.size() + 8);
    for (wchar_t c : w) {
        switch (c) {
        case L'\\': o += L"\\\\"; break; case L'"': o += L"\\\""; break;
        case L'\n': o += L"\\n"; break;  case L'\r': o += L"\\r"; break; case L'\t': o += L"\\t"; break;
        default: if (c < 0x20) { wchar_t b[8]; _snwprintf_s(b, _countof(b), _TRUNCATE, L"\\u%04x", (unsigned)c); o += b; } else o += c;
        }
    }
    return o;
}
/* narrow (UTF-8) JSON string-body escaper -- the crash payload is composed host-side as UTF-8. */
std::string escape_utf8(const std::string &s)
{
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '\\': o += "\\\\"; break; case '"': o += "\\\""; break;
        case '\n': o += "\\n"; break;  case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; _snprintf_s(b, _countof(b), _TRUNCATE, "\\u%04x", (unsigned)c); o += b; }
            else o += (char)c;
        }
    }
    return o;
}
std::string to_utf8(const std::wstring &w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s; s.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
bool get_string(const std::wstring &j, const wchar_t *key, std::wstring &out)
{
    std::wstring needle = L"\""; needle += key; needle += L"\"";
    size_t p = j.find(needle);
    if (p == std::wstring::npos) return false;
    p += needle.size();
    while (p < j.size() && j[p] != L':') p++;
    if (p >= j.size()) return false; p++;
    while (p < j.size() && (j[p] == L' ' || j[p] == L'\t')) p++;
    if (p >= j.size() || j[p] != L'"') return false; p++;
    out.clear();
    while (p < j.size()) {
        wchar_t c = j[p++];
        if (c == L'"') break;
        if (c == L'\\' && p < j.size()) {
            wchar_t e = j[p++];
            switch (e) {
            case L'"': out += L'"'; break; case L'\\': out += L'\\'; break; case L'/': out += L'/'; break;
            case L'n': out += L'\n'; break; case L'r': out += L'\r'; break; case L't': out += L'\t'; break;
            case L'b': out += L'\b'; break; case L'f': out += L'\f'; break;
            case L'u': if (p + 4 <= j.size()) { wchar_t h[5] = {j[p],j[p+1],j[p+2],j[p+3],0}; out += (wchar_t)wcstoul(h, nullptr, 16); p += 4; } break;
            default: out += e; break;
            }
        } else out += c;
    }
    return true;
}
bool get_int(const std::wstring &j, const wchar_t *key, int *out)
{
    std::wstring needle = L"\""; needle += key; needle += L"\"";
    size_t p = j.find(needle);
    if (p == std::wstring::npos) return false;
    p += needle.size();
    while (p < j.size() && j[p] != L':') p++;
    if (p >= j.size()) return false; p++;
    while (p < j.size() && (j[p] == L' ' || j[p] == L'\t')) p++;
    bool neg = false; if (p < j.size() && j[p] == L'-') { neg = true; p++; }
    if (p >= j.size() || j[p] < L'0' || j[p] > L'9') return false;
    long v = 0; while (p < j.size() && j[p] >= L'0' && j[p] <= L'9') { v = v * 10 + (j[p] - L'0'); p++; }
    *out = neg ? -(int)v : (int)v;
    return true;
}
void get_int_array(const std::wstring &j, const wchar_t *key, std::vector<int> &out)
{
    out.clear();
    std::wstring needle = L"\""; needle += key; needle += L"\"";
    size_t p = j.find(needle);
    if (p == std::wstring::npos) return;
    p += needle.size();
    while (p < j.size() && j[p] != L'[') p++;
    if (p >= j.size()) return; p++;
    while (p < j.size() && j[p] != L']') {
        while (p < j.size() && (j[p] == L' ' || j[p] == L',' || j[p] == L'\t')) p++;
        if (p >= j.size() || j[p] == L']') break;
        bool neg = false; if (j[p] == L'-') { neg = true; p++; }
        if (p >= j.size() || j[p] < L'0' || j[p] > L'9') break;
        long v = 0; while (p < j.size() && j[p] >= L'0' && j[p] <= L'9') { v = v * 10 + (j[p] - L'0'); p++; }
        out.push_back(neg ? -(int)v : (int)v);
    }
}

bool get_double(const std::wstring &j, const wchar_t *key, double *out)
{
    std::wstring needle = L"\""; needle += key; needle += L"\"";
    size_t p = j.find(needle);
    if (p == std::wstring::npos) return false;
    p += needle.size();
    while (p < j.size() && j[p] != L':') p++;
    if (p >= j.size()) return false; p++;
    while (p < j.size() && (j[p] == L' ' || j[p] == L'\t')) p++;
    wchar_t *end = nullptr;
    double v = wcstod(j.c_str() + p, &end);
    if (end == j.c_str() + p) return false;
    *out = v;
    return true;
}

}
