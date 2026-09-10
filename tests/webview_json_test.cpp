/* Exercise the native message codec without a window, WebView or engine state. */
#include <cstdio>
#include <string>
#include <vector>
#include "webview_json.h"

static int failed;
#define CHECK(test) do { if (!(test)) { std::printf("FAIL line %d: %s\n", __LINE__, #test); failed++; } } while (0)

int main()
{
    using namespace sh_webview_json;
    CHECK(escape_utf8("quote\" slash\\\n\r\t\b\f") == "quote\\\" slash\\\\\\n\\r\\t\\u0008\\u000c");
    CHECK(escape_wide("quote\" slash\\\n\r\t\b\f") == L"quote\\\" slash\\\\\\n\\r\\t\\u0008\\u000c");
    CHECK(escape_utf8("").empty() && escape_wide("").empty() && to_utf8(L"").empty());
    const std::wstring wide = L"caf\x00e9 \xD83D\xDE00";
    const std::string utf8 = "caf\xc3\xa9 \xf0\x9f\x98\x80";
    CHECK(to_utf8(wide) == utf8);
    CHECK(escape_wide(utf8.c_str()) == wide);
    std::wstring value;
    const std::wstring json = L"{\"cmd\":\"save\",\"name\":\"line\\n\\u00e9\\\"\\\\\",\"eid\":-17,\"ids\":[1,-2,300],\"offset\":-1.25e2}";
    CHECK(get_string(json, L"cmd", value) && value == L"save");
    CHECK(get_string(json, L"name", value) && value == L"line\n\x00e9\"\\");
    CHECK(!get_string(json, L"missing", value));
    int id = 0;
    CHECK(get_int(json, L"eid", &id) && id == -17);
    CHECK(!get_int(json, L"cmd", &id));
    std::vector<int> ids;
    get_int_array(json, L"ids", ids);
    CHECK(ids == std::vector<int>({1,-2,300}));
    get_int_array(L"{\"ids\":[]}", L"ids", ids);
    CHECK(ids.empty());
    double offset = 0;
    CHECK(get_double(json, L"offset", &offset) && offset == -125.0);
    CHECK(!get_double(json, L"missing", &offset));
    const std::wstring payload = L"{\"value\":\"" + escape_wide(utf8.c_str()) + L"\"}";
    CHECK(get_string(payload, L"value", value) && to_utf8(value) == utf8);
    std::printf("webview_json_test: %d failures\n", failed);
    return failed != 0;
}
