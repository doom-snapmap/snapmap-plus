#pragma once
#include <string>

namespace sh_webview_profile {

inline std::wstring path(const std::wstring &local)
{
    return local + L"\\snapmap-plus\\cache\\webview2";
}

}
