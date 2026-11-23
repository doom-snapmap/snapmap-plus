/* plain_reason.h -- one table that turns a Windows error code, a mod store
 * status or a refused file-name character into a plain reason with the next
 * step, so no message carries a bare number ("error 5", "did not answer",
 * "character 0xc3").
 *
 *   sh_reason(SH_REASON_WIN32, GetLastError(), path, bytes_needed)
 *       subject: the file or folder the call used (its drive names a full disk);
 *       bytes: what the write needed (0 when unknown).
 *   sh_reason(SH_REASON_STORE, modnet code, next_step, 0)
 *       code: a MODNET_E_ value (modnet.h); subject: the caller's next step
 *       ("it is retried at the next launch"), NULL for the table's own.
 *   sh_reason(SH_REASON_NAME, code point, path, 0)
 *       code: the character the game cannot read in a file name; subject: the file.
 *
 * Header-only: every module that reports a failure includes it without a
 * link change. Returns the sentence, malloc'd and sized to the subject it
 * names (free it), or NULL when memory runs out. */
#ifndef SNAPMAP_PLUS_PLAIN_REASON_H
#define SNAPMAP_PLUS_PLAIN_REASON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/text.h"

#define SH_REASON_WIN32 1
#define SH_REASON_STORE 2
#define SH_REASON_NAME  3

typedef struct sh_reason_row {
    int kind;
    long code;
    const char *text;       /* one %s: the subject (WIN32), or the next step (STORE) */
    const char *next;       /* STORE: the next step when the caller names none */
} sh_reason_row;

/* The MODNET_E_ values (modnet.h; plain_reason_test checks they agree). */
static const sh_reason_row SH_REASON_TABLE[] = {
    { SH_REASON_WIN32,   5, "Windows refused to write %s; close the program holding it or run the game once as the same user", NULL },
    { SH_REASON_WIN32,  19, "Windows refused to write %s: the drive is write-protected", NULL },
    { SH_REASON_WIN32,  32, "%s is open in another program; close that program and try again", NULL },
    { SH_REASON_WIN32,  33, "%s is open in another program; close that program and try again", NULL },
    { SH_REASON_WIN32, 1224, "%s is open in another program; close that program and try again", NULL },
    { SH_REASON_WIN32,   2, "%s is missing (moved or deleted while the game ran); try again", NULL },
    { SH_REASON_WIN32,   3, "%s is missing (moved or deleted while the game ran); try again", NULL },
    { SH_REASON_WIN32,  80, "%s already exists; remove or rename it and try again", NULL },
    { SH_REASON_WIN32, 183, "%s already exists; remove or rename it and try again", NULL },
    { SH_REASON_WIN32, 123, "Windows does not accept the name %s; rename it", NULL },
    { SH_REASON_WIN32, 206, "the path %s is too long for Windows; give the folder a shorter name", NULL },
    { SH_REASON_WIN32, 145, "%s still holds files another program is using; close that program and try again", NULL },
    { SH_REASON_WIN32,  21, "the drive holding %s is not ready; reconnect it and try again", NULL },
    /* 112 and 39 (disk full) are worded by sh_reason itself: the drive and the bytes. */
    { SH_REASON_STORE, -15, "the mod store is busy", "try again in a few minutes" },
    { SH_REASON_STORE, -17, "the mod store is busy", "try again in a few minutes" },
    { SH_REASON_STORE,  -4, "the mod store could not be reached", "check the internet connection and try again" },
    { SH_REASON_STORE,  -5, "the mod store took too long to answer", "check the internet connection and try again" },
    { SH_REASON_STORE,  -1, "a file of the map's mod content is not on this computer", "check the map's mods in the Mods tab, then try again" },
    { SH_REASON_STORE,  -3, "Snapmap+'s mod store address setting is not usable", "reinstall Snapmap+ to restore it" },
    { SH_REASON_STORE,  -7, "the mod store's address answered, but not as the mod store", "reinstall Snapmap+ to restore the address" },
    { SH_REASON_STORE,  -8, "the mod store's address answered, but not as the mod store", "reinstall Snapmap+ to restore the address" },
    { SH_REASON_STORE,  -9, "a download arrived damaged", "try again" },
    { SH_REASON_STORE,  -6, "the request was cancelled", "try again" },
    { SH_REASON_STORE,  -2, "the game ran out of memory for the request", "restart the game and try again" },
    { SH_REASON_STORE, -11, "the mod store no longer holds it", "its author may publish it again" },
    { SH_REASON_STORE, -16, "it was removed from the mod store", "nothing can restore it here" },
    { SH_REASON_STORE, -13, "the mod store lacked part of the upload", "try again" },
    { SH_REASON_STORE, -14, "the mod store refused it", "use Send feedback in the Snapmap+ window" },
    { SH_REASON_STORE, -12, "the mod store did not accept your publishing key for this map", "restore the key that published it from its backup (Settings menu)" },
    { SH_REASON_STORE, -18, "the content needs a different version of DOOM's own files", "use the version of DOOM it was made for" },
    { SH_REASON_STORE, -19, "this version of Snapmap+ is too old for the mod store", "run the Snapmap+ installer to update" },
    { SH_REASON_STORE, -20, "Snapmap+ could not use your publishing key", "restore it from your backup in the Settings menu" },
    { SH_REASON_STORE, -10, "it is over the mod store's size limit", "make it smaller" },
};

static __inline char *sh_reason(int kind, long code, const char *subject, unsigned long long bytes)
{
    const char *what = subject && subject[0] ? subject : (kind == SH_REASON_STORE ? NULL : "the file");
    size_t i;
    if (kind == SH_REASON_WIN32 && (code == 112 || code == 39)) {
        char drive[4] = "";
        char need[48];
        if (what[0] && what[1] == ':') { drive[0] = what[0]; drive[1] = ':'; drive[2] = 0; }
        if (bytes >= 1024ull * 1024ull)
            _snprintf_s(need, sizeof need, _TRUNCATE, "at least %llu MB", (bytes + 1024ull * 1024ull - 1) / (1024ull * 1024ull));
        else if (bytes) _snprintf_s(need, sizeof need, _TRUNCATE, "at least %llu KB", (bytes + 1023) / 1024);
        else strcpy_s(need, sizeof need, "some space");
        return sh_textf("the drive %s is full (writing %s); free %s on it and try again",
                        drive[0] ? drive : "holding it", what, need);
    }
    if (kind == SH_REASON_NAME) {
        char ch[8] = "";
        unsigned long cp = (unsigned long)code;
        if (cp >= 0x20 && cp < 0x7f) { ch[0] = (char)cp; ch[1] = 0; }
        else if (cp >= 0x80 && cp < 0x800) { ch[0] = (char)(0xc0 | (cp >> 6)); ch[1] = (char)(0x80 | (cp & 0x3f)); ch[2] = 0; }
        else if (cp >= 0x800 && cp < 0x10000) {
            ch[0] = (char)(0xe0 | (cp >> 12)); ch[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); ch[2] = (char)(0x80 | (cp & 0x3f)); ch[3] = 0;
        } else if (cp >= 0x10000 && cp < 0x110000) {
            ch[0] = (char)(0xf0 | (cp >> 18)); ch[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
            ch[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); ch[3] = (char)(0x80 | (cp & 0x3f)); ch[4] = 0;
        }
        /* The allowed set is modbundle.c mb_path_char's. */
        if (ch[0] && cp >= 0x80)
            return sh_textf("%s has the character '%s' (U+%04lX) in its name; the game reads only ASCII file names "
                            "(letters, digits, spaces and . _ - $ + @ ~ ( )), so rename it", what, ch, cp);
        if (ch[0])
            return sh_textf("%s has the character '%s' (U+%04lX) in its name, which the game does not accept; file "
                            "names use letters, digits, spaces and . _ - $ + @ ~ ( ), so rename it", what, ch, cp);
        return sh_textf("%s has a control character (U+%04lX) in its name; file names use letters, digits, spaces "
                        "and . _ - $ + @ ~ ( ), so rename it", what, cp);
    }
    for (i = 0; i < sizeof SH_REASON_TABLE / sizeof SH_REASON_TABLE[0]; i++) {
        const sh_reason_row *r = &SH_REASON_TABLE[i];
        if (r->kind != kind || r->code != code) continue;
        if (kind == SH_REASON_STORE) return sh_textf("%s; %s", r->text, what ? what : r->next);
        return sh_textf(r->text, what);
    }
    if (kind == SH_REASON_STORE)
        return sh_textf("the mod store did not answer; %s", what ? what : "try again in a few minutes");
    return sh_textf("Windows could not use %s; close programs that may hold it and try again", what);
}

/* sh_reason(SH_REASON_WIN32, ...) in a per-thread buffer that the next call
 * on the thread replaces: one call per message. Never NULL. */
static __inline const char *sh_reason_win32(unsigned long err, const char *subject, unsigned long long bytes)
{
    static __declspec(thread) char *held;
    char *text = sh_reason(SH_REASON_WIN32, (long)err, subject, bytes);
    if (!text) return "Windows could not use the file, and memory ran out describing why; close other programs";
    free(held);
    held = text;
    return held;
}

#endif
