/* String-only path helpers for sh_dumpmap. The native writer accepts paths
 * relative to the game base directory and forces the extension. These helpers
 * choose the directory and numbered filename; the command handler owns
 * collision checks and filesystem operations.
 */
#ifndef SNAPMAP_PLUS_DUMPMAP_PATH_H
#define SNAPMAP_PLUS_DUMPMAP_PATH_H

#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* Default subdirectory for a bare dump name, relative to base. */
#define DUMPMAP_SUBDIR  "mapdumps"

/* Highest _N suffix the handler tries before giving up on a name. */
#define DUMPMAP_MAX_SEQ 999

/* Append src to dst (bounded, always NUL-terminated). Local so this header stays dependency-free. */
static void dp_cat(char *dst, size_t cap, const char *src)
{
    size_t o = strlen(dst);
    while (*src && o + 1 < cap) dst[o++] = *src++;
    dst[o] = '\0';
}

/* Validate a game-relative name. Return 1 when usable, otherwise 0 with a
 * printable reason in why.
 */
static int dumpmap_validate(const char *arg, const char **why)
{
    *why = NULL;
    if (arg == NULL || arg[0] == '\0') {
        *why = "empty name";
        return 0;
    }
    if (strchr(arg, ':') != NULL || arg[0] == '\\' || arg[0] == '/') {
        *why = "that is a Windows path -- sh_dumpmap takes a game-relative name";
        return 0;
    }
    if (strstr(arg, "..") != NULL) {
        *why = "'..' is not allowed -- dumps stay under the game's base directory";
        return 0;
    }
    return 1;
}

/* The typed name minus any file extension, prefixed with DUMPMAP_SUBDIR when it is a bare name (a
 * name that already contains a separator picks its own folder under base\ and is left where it is).
 * Only an extension on the FILE is stripped: a '.' inside a directory component is left alone, so
 * "my.dir/pass1" keeps its folder. Returns 0 if nothing usable is left. */
static int dumpmap_stem(const char *arg, char *out, size_t outcap)
{
    if (outcap == 0) return 0;
    out[0] = '\0';
    if (arg == NULL) return 0;

    if (strchr(arg, '/') == NULL && strchr(arg, '\\') == NULL) {
        dp_cat(out, outcap, DUMPMAP_SUBDIR);
        dp_cat(out, outcap, "/");
    }
    dp_cat(out, outcap, arg);

    char *dot = strrchr(out, '.');
    if (dot != NULL && dot > strrchr(out, '/') && dot > strrchr(out, '\\'))
        *dot = '\0';

    /* a stem that is nothing but a directory ("sub/") has no filename to write */
    const char *tail = out;
    const char *s;
    if ((s = strrchr(out, '/')) != NULL && s + 1 > tail) tail = s + 1;
    if ((s = strrchr(out, '\\')) != NULL && s + 1 > tail) tail = s + 1;
    return tail[0] != '\0';
}

/* The seq'th candidate game-relative path for a stem: 1 -> "<stem>.map", 2 -> "<stem>_2.map", ... */
static void dumpmap_candidate(const char *stem, int seq, char *out, size_t outcap)
{
    if (outcap == 0) return;
    out[0] = '\0';
    dp_cat(out, outcap, stem);
    if (seq > 1) {
        char suffix[16];
        _snprintf_s(suffix, sizeof suffix, _TRUNCATE, "_%d", seq);
        dp_cat(out, outcap, suffix);
    }
    dp_cat(out, outcap, ".map");
}

/* Join the game's base directory with a game-relative path into an OS path, normalizing every
 * separator to '\' so the result is printable as-is. */
static void dumpmap_ospath(const char *base, const char *rel, char *out, size_t outcap)
{
    if (outcap == 0) return;
    out[0] = '\0';
    dp_cat(out, outcap, base);
    dp_cat(out, outcap, "\\");
    dp_cat(out, outcap, rel);
    for (char *p = out; *p; p++)
        if (*p == '/') *p = '\\';
}

#endif /* SNAPMAP_PLUS_DUMPMAP_PATH_H */
