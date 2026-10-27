/* The game identity the backend hands the frontend at startup.
 *
 * The backend produces the one game-identity string (sh_host_game_identity in
 * src/backend/host_image.c) and passes it in the startup argument block as
 * "--game=<identity>". The frontend never recomputes it: an absent or empty
 * argument means the backend did not attach to a supported executable, and
 * reports then say "not attached". Header-only so the parser is testable. */
#ifndef SH_UI_GAME_IDENTITY_H
#define SH_UI_GAME_IDENTITY_H

#include <string>
#include <cstring>

namespace sh_game_identity {

static const char kArgPrefix[] = "--game=";

/* The identity from an argv vector, or "" when no non-empty --game= argument is
 * present. Values with control characters are refused (they would break the
 * one-line report metadata). */
inline std::string from_argv(int argc, char **argv)
{
    if (argc <= 0 || !argv) return std::string();
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!a || std::strncmp(a, kArgPrefix, sizeof kArgPrefix - 1) != 0) continue;
        std::string value(a + sizeof kArgPrefix - 1);
        if (value.size() > 160) return std::string();
        for (size_t k = 0; k < value.size(); k++)
            if ((unsigned char)value[k] < 0x20 || value[k] == 0x7f) return std::string();
        return value;
    }
    return std::string();
}

}  // namespace sh_game_identity

#endif
