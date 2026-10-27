// The frontend takes the game identity only from the backend's startup argv.
#include <cassert>
#include <cstdio>
#include <string>
#include "../src/ui/webview/game_identity.h"

int main()
{
    static const char kGame[] = "DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)";
    char a0[] = "snapmap-plus";
    char a1[] = "--game=DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)";
    char *both[] = { a0, a1, nullptr };
    assert(sh_game_identity::from_argv(2, both) == kGame);

    // The backend passes only argv[0] when it did not attach to a supported build.
    char *bare[] = { a0, nullptr };
    assert(sh_game_identity::from_argv(1, bare).empty());
    assert(sh_game_identity::from_argv(0, nullptr).empty());

    char empty[] = "--game=";
    char *blank[] = { a0, empty, nullptr };
    assert(sh_game_identity::from_argv(2, blank).empty());

    // Control characters would break the one-line report metadata.
    char newline[] = "--game=DOOM 2016\n- Contact: spoof";
    char *bad[] = { a0, newline, nullptr };
    assert(sh_game_identity::from_argv(2, bad).empty());

    std::string longer = "--game=" + std::string(200, 'x');
    char *huge[] = { a0, &longer[0], nullptr };
    assert(sh_game_identity::from_argv(2, huge).empty());

    // Unrelated arguments are ignored.
    char other[] = "--gamex=1";
    char *noise[] = { a0, other, a1, nullptr };
    assert(sh_game_identity::from_argv(3, noise) == kGame);

    std::puts("game_identity_arg_test: PASS");
    return 0;
}
