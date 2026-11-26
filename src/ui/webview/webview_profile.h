#pragma once
#include <string>

namespace sh_webview_profile {

inline std::wstring path(const std::wstring &profile)
{
    return profile + L"\\cache\\webview2";
}

}
