#include "webview_profile.h"
#include <cassert>

int main()
{
    assert(sh_webview_profile::path(L"C:\\Users\\Alex\\AppData\\Local") ==
           L"C:\\Users\\Alex\\AppData\\Local\\snapmap-plus\\cache\\webview2");
}
