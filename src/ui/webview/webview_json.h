/* Text conversion and field access for valid JSON supplied by WebView2.
 * Field readers preserve the host message format; they are not JSON validators. */
#pragma once
#include <string>
#include <vector>

namespace sh_webview_json {
// Return escaped string contents without surrounding JSON quotes.
std::wstring escape_wide(const char *utf8);
std::string escape_utf8(const std::string &value);
std::string to_utf8(const std::wstring &value);
bool get_string(const std::wstring &json, const wchar_t *key, std::wstring &out);
bool get_int(const std::wstring &json, const wchar_t *key, int *out);
void get_int_array(const std::wstring &json, const wchar_t *key, std::vector<int> &out);
// Strings of a JSON array value; a non-string element ends the read.
void get_string_array(const std::wstring &json, const wchar_t *key, std::vector<std::wstring> &out);
bool get_double(const std::wstring &json, const wchar_t *key, double *out);
}
