/* The game identity the backend hands the frontend at startup.
 *
 * The backend produces the one game-identity string (sh_host_game_identity in
 * src/backend/host_image.c) and passes it in the startup argument block as
 * "--game=<identity>". The frontend never recomputes it: an absent or empty
 * argument means the backend did not attach to a supported executable, and
 * reports then say "not attached". The backend's release version arrives the
 * same way, as "--version=<tag>", so the window can check that both of its files
 * come from one release. Header-only so the parser is testable. */
#ifndef SH_UI_GAME_IDENTITY_H
#define SH_UI_GAME_IDENTITY_H

#include <string>
#include <cstring>

namespace sh_game_identity {

static const char kArgPrefix[] = "--game=";
static const char kVersionPrefix[] = "--version=";

/* The value of the first "<prefix><value>" argument, or "" when none is present
 * or it is empty. Values with control characters are refused (they would break
 * the one-line report metadata). */
inline std::string arg_value(int argc, char **argv, const char *prefix)
{
    if (argc <= 0 || !argv) return std::string();
    size_t n = std::strlen(prefix);
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!a || std::strncmp(a, prefix, n) != 0) continue;
        std::string value(a + n);
        if (value.size() > 160) return std::string();
        for (size_t k = 0; k < value.size(); k++)
            if ((unsigned char)value[k] < 0x20 || value[k] == 0x7f) return std::string();
        return value;
    }
    return std::string();
}

/* The game identity, or "" when the backend did not attach to a supported build. */
inline std::string from_argv(int argc, char **argv) { return arg_value(argc, argv, kArgPrefix); }

/* The backend's release version ("v1.0.0", "dev"), or "" when it passed none. */
inline std::string backend_version(int argc, char **argv) { return arg_value(argc, argv, kVersionPrefix); }

}  // namespace sh_game_identity

#endif
