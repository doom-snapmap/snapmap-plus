/* dumpmap_path.h -- pure path arithmetic for sh_dumpmap's output location.
 *
 * The map writer takes a GAME-RELATIVE path rooted at <game dir>\base\ and does two unhelpful things:
 * it silently clobbers an earlier dump of the same name, and it forces the extension, so the name on
 * disk is not necessarily the one that was typed. The command handler therefore resolves the whole
 * destination itself -- default subdirectory, collision-free filename -- before handing the writer a
 * path. This header is the string half of that: no file system, no OS calls, no engine calls, so the
 * exact same code is unit-tested off-game (tests/dumpmap_path_test.c) and compiled into the backend.
 * The handler keeps the parts that touch the disk (the collision probe, the mkdir, the size read).
 */
#ifndef SNAPMAP_PLUS_DUMPMAP_PATH_H
#define SNAPMAP_PLUS_DUMPMAP_PATH_H

#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* Where a bare name lands, under the game's base directory. Its own folder, so a dump can never
 * collide with one of the engine's own resource namespaces. */
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

/* Reject a name that cannot serve as a game-relative path. 0 + *why set to a one-line printable
 * reason, or 1 if the name is usable. The Windows-path case is the one users actually hit: the
 * command reads like it wants a file path, and it does not. */
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
