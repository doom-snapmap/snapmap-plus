#include "webview_profile.h"
#include <cassert>

int main()
{
    assert(sh_webview_profile::path(L"C:\\Users\\Alex\\AppData\\Local\\snapmap-plus") ==
           L"C:\\Users\\Alex\\AppData\\Local\\snapmap-plus\\cache\\webview2");
    assert(sh_webview_profile::path(L"D:\\private profile") == L"D:\\private profile\\cache\\webview2");
}
