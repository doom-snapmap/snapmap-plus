/* modauthor.c -- the mod folder an author writes, and the engine names it stands for (modauthor.h). Pure. */
#include "modauthor.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../common/text.h"

/* A folder of the mod and the engine root its files are served under (the salt goes after the root). A row with an
 * extension takes only files of that extension; the first row of a folder that matches wins. */
typedef struct ma_row { const char *folder; const char *ext; const char *root; } ma_row;

static const ma_row MA_ROWS[] = {
    { "materials",               ".decl",      "generated/decls/material/" },
    { "effects",                 ".decl",      "generated/decls/fx/" },
    { "environments",            ".decl",      "generated/decls/env/" },
    { "particles",               ".decl",      "generated/decls/particle/" },
    { "entities",                ".decl",      "generated/decls/entitydef/" },
    { "sounds",                  ".decl",      "generated/decls/sound/" },
    { "images",                  ".bimage",    "generated/image/" },
    { "atlases/decals",          ".bimage",    "generated/decalatlas/" },
    { "atlases/lights",          ".bimage",    "generated/lightatlas/" },
    { "atlases/transparency",    ".bimage",    "generated/transsortatlas/" },
    { "meshes",                  ".decl",      "generated/decls/md6def/" },
    { "meshes",                  ".bmd6model", "generated/basemodel/md6/" },
    { "meshes",                  ".bmd6skl",   "generated/skeleton/md6/" },
    { "meshes",                  ".bmodel",    "cooked/model/" },
    { "meshes",                  ".dmodel",    "generated/discreteanimation/" },
    { "meshes",                  ".pmodel",    "generated/" },
    /* an md6 mesh's collision models, found by the mesh's path at generated/model/md6/<mesh path>/collision/ */
    { "collision",               ".bmodel",    "generated/model/md6/" },
    { "collision",               ".bcm",       "generated/cm/md6/" },
    { "animations",              ".bmd6anim",  "cooked/anim/md6/" },
    { "movies",                  ".bswf",      "generated/swf/" },
    { "virtual-textures",        ".mega2",     "virtualtextures/" },
    { "virtual-textures",        ".pages",     "virtualtextures/" },
    { "movies",                  ".bimage",    "generated/swf/" },
    /* a render program: its decl, its cooked stages, its permutations and their SPIR-V, each where the engine opens it */
    { "programs/spirv/permutations", ".vspv",  "generated/spirv/generated/renderprogs/permutations/" },
    { "programs/spirv/permutations", ".fspv",  "generated/spirv/generated/renderprogs/permutations/" },
    { "programs/spirv/permutations", ".cspv",  "generated/spirv/generated/renderprogs/permutations/" },
    { "programs/spirv",          ".vspv",      "generated/spirv/" },
    { "programs/spirv",          ".fspv",      "generated/spirv/" },
    { "programs/spirv",          ".cspv",      "generated/spirv/" },
    { "programs/permutations",   ".decl",      "generated/renderprogs/permutations/" },
    { "programs/permutations",   ".bin",       "generated/renderprogs/permutations/" },
    { "programs",                ".decl",      "generated/decls/renderprog/" },
    { "programs",                ".bin",       "generated/renderprogs/" },
};
#define MA_NROWS (sizeof MA_ROWS / sizeof MA_ROWS[0])

/* The decl folders that have a folder of their own; decls/<one of these>/ is refused with where it goes. */
static const struct { const char *type; const char *folder; } MA_NAMED[] = {
    { "material", "materials" }, { "fx", "effects" }, { "env", "environments" }, { "particle", "particles" }, { "entitydef", "entities" },
    { "sound", "sounds" }, { "md6def", "meshes" }, { "renderprog", "programs" },
};
#define MA_NNAMED (sizeof MA_NAMED / sizeof MA_NAMED[0])

static const char MA_FOLDERS_SENTENCE[] =
    "materials, images, meshes, collision, animations, effects, environments, particles, entities, sounds, movies, "
    "scripts, map-modules, virtual-textures, atlases, programs, decls and native";

static int ma_starts(const char *s, const char *p) { return _strnicmp(s, p, strlen(p)) == 0; }

static int ma_ends(const char *s, size_t n, const char *e)
{
    size_t k = strlen(e);
    return n >= k && _strnicmp(s + n - k, e, k) == 0;
}

static char *ma_lower(const char *s)
{
    char *o = sh_text(s ? s : ""), *p;
    if (!o) return NULL;
    for (p = o; *p; ++p) {
        if (*p == '\\') *p = '/';
        else if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    }
    return o;
}

static int ma_fail(char **cause, const char *fmt, const char *a, const char *b)
{
    if (cause) { free(*cause); *cause = sh_textf(fmt, a, b ? b : ""); }
    return 0;
}

int sh_ma_folder(const char *name, size_t n)
{
    static const char *const top[] = { "materials", "images", "meshes", "collision", "animations", "effects", "environments", "particles",
                                       "entities", "sounds", "movies", "scripts", "map-modules", "virtual-textures",
                                       "atlases", "programs", "decls", "native", NULL };
    size_t i;
    for (i = 0; top[i]; ++i)
        if (strlen(top[i]) == n && _strnicmp(top[i], name, n) == 0) return 1;
    return 0;
}

/* The folders, each with one example. */
#define MA_KIND_LIST                                                                                                    \
    "Put each file in the folder for its kind: entities/ for entities (entities/imp.decl), materials/ for materials "  \
    "(materials/imp_skin.decl), images/ for images (images/imp_skin.bimage), meshes/ for models "                      \
    "(meshes/imp.md6.decl), collision/ for a model's collision models at its mesh's path "                             \
    "(collision/imp/collision/imp_chest.bmodel), animations/ for animation clips (animations/imp_walk.bmd6anim), "     \
    "effects/ for effects "                                                                                            \
    "(effects/imp_sparks.decl), particles/ for particle systems (particles/imp_smoke.decl), sounds/ for sounds and "   \
    "their banks (sounds/imp.bnk), scripts/ for Lua scripts (scripts/imp.lua), movies/ for Flash movies "              \
    "(movies/imp_hud.bswf), programs/ for render programs (programs/imp_glow.decl), map-modules/ for SnapMap modules " \
    "(map-modules/imp_arena.decl), virtual-textures/ for a texture board (virtual-textures/imp.pages), environments/ " \
    "for environments (environments/imp_fog.decl), atlases/decals/ for decal atlases a converter writes "              \
    "(atlases/decals/imp.bimage), and decls/ with the kind's name for any other decl (decls/damage/imp_claw.decl)."

const char *sh_ma_kind_list(void) { return MA_KIND_LIST; }

char *sh_ma_outside_cause(const char *path, int dooms)
{
    const char *p = path ? path : "";
    if (dooms)
        return sh_textf("This mod was not built: %s is DOOM's own file, outside native/. If you changed it, move it to "
                        "native/%s; if you did not, delete it: the build finds DOOM's files the mod uses by itself.", p, p);
    return sh_textf("This mod was not built: %s is in none of the mod's folders, and DOOM has no file at that path. "
                    MA_KIND_LIST " A file that changes one of DOOM's own files goes in native/ at DOOM's own path for it "
                    "(for example native/generated/decls/entitydef/ai/conductor.decl); DOOM's files you use without "
                    "changing them are found by the build and never need copying.", p);
}

char *sh_ma_kind_path(const char *request)
{
    size_t i, n;
    const char *r = request ? request : "", *ext = strrchr(r, '.');
    if (ext && strchr(ext, '/')) ext = NULL;
    n = strlen(r);
    if (ma_starts(r, "sound/soundbanks/pc/") && ma_ends(r, n, ".bnk") && !strchr(r + 20, '/'))
        return sh_textf("sounds/%s", r + 20);
    if (ma_starts(r, "maps/modules/") && r[13]) return sh_textf("map-modules/%s", r + 13);
    if (ma_starts(r, "generated/decls/")) {
        const char *type = r + 16, *ts = strchr(type, '/');
        size_t tl;
        if (!ts || ts == type || !ts[1] || !ma_ends(r, n, ".decl")) return NULL;
        tl = (size_t)(ts - type);
        for (i = 0; i < MA_NNAMED; ++i)
            if (strlen(MA_NAMED[i].type) == tl && !_strnicmp(type, MA_NAMED[i].type, tl))
                return sh_textf("%s/%s", MA_NAMED[i].folder, ts + 1);
        return sh_textf("decls/%.*s/%s", (int)tl, type, ts + 1);
    }
    for (i = 0; i < MA_NROWS; ++i) {
        size_t rl = strlen(MA_ROWS[i].root);
        if (_strnicmp(r, MA_ROWS[i].root, rl) || !r[rl] || !ext || _stricmp(ext, MA_ROWS[i].ext)) continue;
        return sh_textf("%s/%s", MA_ROWS[i].folder, r + rl);
    }
    return NULL;
}

/* `root` + "mods/<salt>/" + rest with "<salt>_" before its last part: a sound's engine name (a decl name or a request
 * path; rest is rn bytes, its directories kept). */
static char *ma_sound_name(const char *root, const char *salt, const char *rest, size_t rn)
{
    const char *leaf = rest, *q;
    for (q = rest; q < rest + rn; ++q) if (*q == '/') leaf = q + 1;
    return sh_textf("%smods/%s/%.*s%s_%.*s", root, salt, (int)(leaf - rest), rest, salt, (int)(rn - (size_t)(leaf - rest)),
                    leaf);
}

int sh_ma_request(const char *salt, const char *path, char **request, char **cause)
{
    char *p = NULL, *slash, *rest;
    const char *ext;
    size_t i, n;
    if (request) *request = NULL;
    if (cause) *cause = NULL;
    if (!salt || !path || !request) return 0;
    if (!(p = ma_lower(path))) return ma_fail(cause, "out of memory", NULL, NULL);
    slash = strchr(p, '/');
    if (!slash) { free(p); return 0; }                       /* beside mod.json: not part of the mod */
    n = strlen(p);
    ext = strrchr(p, '.');
    if (ext && strchr(ext, '/')) ext = NULL;
    if (ma_starts(p, SH_MA_NATIVE)) {
        rest = p + 7;
        if (!*rest) { free(p); return ma_fail(cause, "%s: name DOOM's file under native/, at DOOM's own path", path, NULL); }
        /* a mod's namespace under native/ is refused; a name of the product's own (snapmapplus/..., a closure pointer
         * such as an SDK effect's emitter snapmapplus/effect/mods/<salt>/x) is not a mod's */
        const char *ns = strstr(rest, "/mods/"), *prod = strstr(rest, "/snapmapplus/");
        if (ma_starts(rest, "mods/") || (ns && !(prod && prod < ns))) {
            free(p);
            return ma_fail(cause, "%s: native/ holds DOOM's own files the mod changes, at DOOM's own paths; put the mod's own "
                           "files in its folders (%s)", path, MA_FOLDERS_SENTENCE);
        }
        *request = sh_text(rest);
        free(p);
        return *request ? 1 : ma_fail(cause, "out of memory", NULL, NULL);
    }
    if (ma_starts(p, "scripts/")) {
        if (!ma_ends(p, n, ".lua")) {
            free(p);
            return ma_fail(cause, "%s: scripts/ holds Lua files (.lua); move it to the folder for what it is", path, NULL);
        }
        *request = sh_textf("mods/%s/%s", salt, p);
        free(p);
        return *request ? 1 : ma_fail(cause, "out of memory", NULL, NULL);
    }
    if (ma_starts(p, "sounds/") && ma_ends(p, n, ".bnk")) {
        rest = p + 7;
        if (strchr(rest, '/')) {
            free(p);
            return ma_fail(cause, "%s: a sound bank goes in sounds/ itself, not in a folder below it; move it there",
                           path, NULL);
        }
        *request = sh_textf("sound/soundbanks/pc/%s_%s", salt, rest);
        free(p);
        return *request ? 1 : ma_fail(cause, "out of memory", NULL, NULL);
    }
    if (ma_starts(p, "map-modules/")) {
        /* a SnapMap module: its decl and the files the engine opens beside it, a palette's model under the palette,
         * its navigation under generated/ */
        rest = p + 12;
        if (!*rest) { free(p); return ma_fail(cause, "%s: name the module's file", path, NULL); }
        if (ma_starts(rest, "palettes/")) {
            char *ps = strchr(rest + 9, '/');
            if (ps && ps[1]) {
                *request = sh_textf("maps/modules/palettes/%.*s/mods/%s/%s", (int)(ps - (rest + 9)), rest + 9, salt, ps + 1);
                free(p);
                return *request ? 1 : ma_fail(cause, "out of memory", NULL, NULL);
            }
        }
        if (ext && !_strnicmp(ext, ".baas_", 6)) *request = sh_textf("generated/maps/modules/mods/%s/%s", salt, rest);
        else *request = sh_textf("maps/modules/mods/%s/%s", salt, rest);
        free(p);
        return *request ? 1 : ma_fail(cause, "out of memory", NULL, NULL);
    }
    if (ma_starts(p, "decls/")) {        char *type = p + 6, *tslash = strchr(type, '/');
        if (!tslash || tslash == type || !tslash[1]) {
            free(p);
            return ma_fail(cause, "%s: decls/ holds one folder per kind of decl, named as the game names it (for "
                           "example decls/damage/); move it into one", path, NULL);
        }
        *tslash = 0;
        for (i = 0; i < MA_NNAMED; ++i)
            if (!strcmp(type, MA_NAMED[i].type)) {
                char *why = sh_textf("%%s: a %s decl goes in %s/; move it there", MA_NAMED[i].type, MA_NAMED[i].folder);
                ma_fail(cause, why ? why : "%s: move it to its own folder", path, NULL);
                free(why);
                free(p);
                return 0;
            }
        if (!ma_ends(tslash + 1, strlen(tslash + 1), ".decl")) {
            free(p);
            return ma_fail(cause, "%s: decls/ holds decl files (.decl); move it to the folder for what it is", path, NULL);
        }
        *request = sh_textf("generated/decls/%s/mods/%s/%s", type, salt, tslash + 1);
        free(p);
        return *request ? 1 : ma_fail(cause, "out of memory", NULL, NULL);
    }
    if (ma_starts(p, "sounds/") && ma_ends(p, n, ".decl") && n > 12) {
        /* a sound decl's name is its Wwise event (play_ + its last part), which every mod shares: the leaf
         * carries the namespace (sounds/a/play_x.decl -> generated/decls/sound/mods/<salt>/a/<salt>_play_x.decl) */
        *request = ma_sound_name("generated/decls/sound/", salt, p + 7, n - 7);
        free(p);
        return *request ? 1 : ma_fail(cause, "out of memory", NULL, NULL);
    }
    for (i = 0; i < MA_NROWS; ++i) {
        size_t fl = strlen(MA_ROWS[i].folder);
        if (strncmp(p, MA_ROWS[i].folder, fl) || p[fl] != '/') continue;
        if (!p[fl + 1]) break;
        if (!ext || _stricmp(ext, MA_ROWS[i].ext)) continue;
        *request = sh_textf("%smods/%s/%s", MA_ROWS[i].root, salt, p + fl + 1);
        free(p);
        return *request ? 1 : ma_fail(cause, "out of memory", NULL, NULL);
    }
    *slash = 0;
    if (sh_ma_folder(p, strlen(p))) {
        char kinds[200] = "";
        for (i = 0; i < MA_NROWS; ++i) {
            size_t fl = strlen(MA_ROWS[i].folder);
            if (strncmp(path, MA_ROWS[i].folder, fl) && _strnicmp(path, MA_ROWS[i].folder, fl)) continue;
            if (kinds[0]) strcat_s(kinds, sizeof kinds, ", ");
            strcat_s(kinds, sizeof kinds, MA_ROWS[i].ext);
        }
        if (!strcmp(p, "atlases"))
            ma_fail(cause, "%s: atlases/ holds decals/, lights/ and transparency/; move it into one", path, NULL);
        else if (kinds[0]) ma_fail(cause, "%s: this folder holds %s files; move it to the folder for what it is", path, kinds);
        else ma_fail(cause, "%s: move it to the folder for what it is", path, NULL);
        free(p);
        return 0;
    }
    ma_fail(cause, "%s: '%s' is not one of a mod's folders; a mod's files go in materials, images, meshes, collision, "
            "animations, effects, environments, particles, entities, sounds, movies, scripts, map-modules, "
            "virtual-textures, atlases, programs, decls or native", path, p);
    free(p);
    return 0;
}

char *sh_ma_path(const char *salt, const char *request)
{
    char own[64];
    size_t i, ol;
    const char *r = request ? request : "", *ext = strrchr(r, '.');
    if (ext && strchr(ext, '/')) ext = NULL;
    if (!salt) return sh_textf(SH_MA_NATIVE "%s", r);
    _snprintf_s(own, sizeof own, _TRUNCATE, "mods/%s/", salt);
    ol = strlen(own);
    if (ma_starts(r, own) && ma_starts(r + ol, "scripts/")) return sh_text(r + ol);
    if (ma_starts(r, "sound/soundbanks/pc/") && !_strnicmp(r + 20, salt, strlen(salt)) && r[20 + strlen(salt)] == '_' &&
        !strchr(r + 20, '/'))
        return sh_textf("sounds/%s", r + 21 + strlen(salt));
    if (ma_starts(r, "generated/maps/modules/") && ma_starts(r + 23, own)) return sh_textf("map-modules/%s", r + 23 + ol);
    if (ma_starts(r, "maps/modules/") && ma_starts(r + 13, own)) return sh_textf("map-modules/%s", r + 13 + ol);
    if (ma_starts(r, "maps/modules/palettes/")) {
        const char *ps = strchr(r + 22, '/');
        if (ps && ma_starts(ps + 1, own)) return sh_textf("map-modules/palettes/%.*s/%s", (int)(ps - (r + 22)), r + 22, ps + 1 + ol);
    }
    if (ma_starts(r, "generated/decls/")) {        const char *type = r + 16, *ts = strchr(type, '/');
        if (ts && ma_starts(ts + 1, own)) {
            size_t tl = (size_t)(ts - type);
            if (tl == 5 && !_strnicmp(type, "sound", 5)) {
                /* the leaf carries the namespace (sh_ma_request); one without it is no author path's */
                const char *rest = ts + 1 + ol, *leaf = strrchr(rest, '/');
                size_t sl = strlen(salt);
                leaf = leaf ? leaf + 1 : rest;
                if (!_strnicmp(leaf, salt, sl) && leaf[sl] == '_' && leaf[sl + 1])
                    return sh_textf("sounds/%.*s%s", (int)(leaf - rest), rest, leaf + sl + 1);
                return sh_textf(SH_MA_NATIVE "%s", r);
            }
            for (i = 0; i < MA_NNAMED; ++i)
                if (strlen(MA_NAMED[i].type) == tl && !_strnicmp(type, MA_NAMED[i].type, tl))
                    return sh_textf("%s/%s", MA_NAMED[i].folder, ts + 1 + ol);
            if (_strnicmp(type, "md6def", tl))
                return sh_textf("decls/%.*s/%s", (int)tl, type, ts + 1 + ol);
        }
    }
    for (i = 0; i < MA_NROWS; ++i) {
        size_t rl = strlen(MA_ROWS[i].root);
        if (_strnicmp(r, MA_ROWS[i].root, rl) || !ma_starts(r + rl, own)) continue;
        if (!ext || _stricmp(ext, MA_ROWS[i].ext)) continue;
        return sh_textf("%s/%s", MA_ROWS[i].folder, r + rl + ol);
    }
    return sh_textf(SH_MA_NATIVE "%s", r);
}

/* A name in a text: letters, digits and _ . / $ - + @ ~ (the characters the game's names and paths hold; a name
 * with a space or a parenthesis is not one a text refers to). */
static int ma_name_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '/' || c == '$' || c == '-' || c == '+' || c == '@' || c == '~';
}

/* The name prefixes: a folder and the engine's spelling before the salt ("%s" is the salt). */
static const struct { const char *folder; const char *engine; } MA_NAMES[] = {
    { "materials/", "mods/%s/" }, { "effects/", "mods/%s/" }, { "environments/", "mods/%s/" }, { "particles/", "mods/%s/" },
    { "entities/", "mods/%s/" }, { "images/", "mods/%s/" },   /* sounds/ is decided apart (ma_sound_name) */
    { "animations/", "md6/mods/%s/" }, { "collision/", "md6/mods/%s/" }, { "movies/", "swf/mods/%s/" },
    { "scripts/", "mods/%s/scripts/" },
    { "map-modules/", "maps/modules/mods/%s/" }, { "virtual-textures/", "mods/%s/" }, { "programs/", "mods/%s/" },
};
#define MA_NNAMES (sizeof MA_NAMES / sizeof MA_NAMES[0])

char *sh_ma_engine_name(const char *salt, const char *name, size_t n)
{
    size_t i;
    if (!salt || !name || !n) return NULL;
    if (n > 7 && !_strnicmp(name, "native:", 7)) return sh_textn(name + 7, n - 7);
    if (n > 7 && !_strnicmp(name, "meshes/", 7)) {
        const char *rest = name + 7;
        size_t rn = n - 7;
        if (ma_ends(rest, rn, ".md6mesh") || ma_ends(rest, rn, ".md6skl"))
            return sh_textf("md6/mods/%s/%.*s", salt, (int)rn, rest);
        return sh_textf("mods/%s/%.*s", salt, (int)rn, rest);
    }
    if (n > 7 && !_strnicmp(name, "sounds/", 7)) return ma_sound_name("", salt, name + 7, n - 7);
    if (n > 6 && !_strnicmp(name, "decls/", 6)) {
        const char *type = name + 6, *ts = memchr(type, '/', n - 6);
        if (!ts || ts == type || (size_t)(ts + 1 - name) >= n) return NULL;
        return sh_textf("mods/%s/%.*s", salt, (int)(n - (size_t)(ts + 1 - name)), ts + 1);
    }
    for (i = 0; i < MA_NNAMES; ++i) {
        size_t fl = strlen(MA_NAMES[i].folder);
        char *pre, *out;
        if (n <= fl || _strnicmp(name, MA_NAMES[i].folder, fl)) continue;
        pre = sh_textf(MA_NAMES[i].engine, salt);
        out = pre ? sh_textf("%s%.*s", pre, (int)(n - fl), name + fl) : NULL;
        free(pre);
        return out;
    }
    return NULL;
}

/* A growing output buffer. */
typedef struct ma_buf { char *p; size_t n, cap; int oom; } ma_buf;

static void ma_put(ma_buf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *g;
        while (cap < b->n + n + 1) cap *= 2;
        g = (char *)realloc(b->p, cap);
        if (!g) { b->oom = 1; return; }
        b->p = g;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static int ma_key_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* 1 when the name at t[i] is the quoted argument of native("...") (the Graphics SDK's other spelling of a DOOM name,
 * sdkparse.h): it names DOOM's, as native:<name> does, and is left as written. */
static int ma_in_native_call(const char *t, size_t i)
{
    size_t k = i;
    if (k == 0 || t[k - 1] != '"') return 0;
    k--;
    while (k > 0 && (t[k - 1] == ' ' || t[k - 1] == '\t')) k--;
    if (k == 0 || t[k - 1] != '(') return 0;
    k--;
    return k >= 6 && !_strnicmp(t + k - 6, "native", 6) && (k == 6 || !ma_name_char((unsigned char)t[k - 7]));
}

/* Rewrites t[0..len) into b; returns 1 when anything changed. */
static int ma_rewrite(const char *salt, const char *t, size_t len, sh_ma_key_fn own_key, void *ctx, ma_buf *b)
{
    size_t i = 0, copied = 0;
    int changed = 0;
    while (i < len) {
        unsigned char c = (unsigned char)t[i];
        int start = i == 0 || !ma_name_char((unsigned char)t[i - 1]);
        if (start && c == '#' && i + 5 < len && !strncmp(t + i, "#str_", 5) && own_key) {
            size_t k = i + 5;
            while (k < len && ma_key_char((unsigned char)t[k])) k++;
            if (k > i + 5 && own_key(ctx, t + i + 5, k - i - 5)) {
                ma_put(b, t + copied, i - copied);
                ma_put(b, "#str_", 5);
                ma_put(b, salt, strlen(salt));
                ma_put(b, "_", 1);
                ma_put(b, t + i + 5, k - i - 5);
                copied = i = k;
                changed = 1;
                continue;
            }
            i = k;
            continue;
        }
        if (start && ma_name_char(c) && c != '/' && c != '.' && ma_in_native_call(t, i)) {
            while (i < len && ma_name_char((unsigned char)t[i])) i++;
            continue;
        }
        if (start && ma_name_char(c) && c != '/' && c != '.') {
            size_t k = i;
            char *eng;
            /* native:<name> is one run: the colon joins the two */
            if (len - i > 7 && !_strnicmp(t + i, "native:", 7) && ma_name_char((unsigned char)t[i + 7])) k = i + 7;
            while (k < len && ma_name_char((unsigned char)t[k])) k++;
            if (k > i && (eng = sh_ma_engine_name(salt, t + i, k - i)) != NULL) {
                ma_put(b, t + copied, i - copied);
                ma_put(b, eng, strlen(eng));
                free(eng);
                copied = i = k;
                changed = 1;
                continue;
            }
            i = k > i ? k : i + 1;
            continue;
        }
        i++;
    }
    if (changed) ma_put(b, t + copied, len - copied);
    return changed;
}

char *sh_ma_text(const char *salt, const char *text, size_t len, sh_ma_key_fn own_key, void *ctx, size_t *out_len)
{
    ma_buf b = { NULL, 0, 0, 0 };
    if (out_len) *out_len = 0;
    if (!salt || !text) return NULL;
    if (!ma_rewrite(salt, text, len, own_key, ctx, &b)) { free(b.p); return NULL; }
    if (b.oom) { free(b.p); if (out_len) *out_len = (size_t)-1; return NULL; }
    if (out_len) *out_len = b.n;
    return b.p;
}

/* A walk over JSON text: every string token, whether it is an object key, and whether it is a key of a language
 * object of the top-level "strings" object (depth 3: the root, strings, a language). */
typedef void (*ma_json_fn)(void *ctx, const char *s, size_t n, int is_key, int is_strings_key);

static void ma_json_walk(const char *t, size_t len, ma_json_fn fn, void *ctx)
{
    /* per depth: 1 object / 0 array; whether a key is expected next; the top-level key the depth-2 value is under */
    enum { MA_DEPTH = 64 };
    char is_obj[MA_DEPTH], want_key[MA_DEPTH];
    const char *top_key = NULL;
    size_t top_n = 0, i = 0;
    int depth = 0, in_strings = 0, in_block = 0;
    while (i < len) {
        char c = t[i];
        if (c == '{' || c == '[') {
            if (depth == 1 && c == '{') in_strings = top_key && top_n == 7 && !strncmp(top_key, "strings", 7);
            /* the builder's own part names files by their paths in the folder: never a name to rewrite */
            if (depth == 1 && c == '{') in_block = top_key && top_n == 12 && !strncmp(top_key, "snapmap-plus", 12);
            if (depth < MA_DEPTH) { is_obj[depth] = c == '{'; want_key[depth] = c == '{'; }
            depth++;
            i++;
            continue;
        }
        if (c == '}' || c == ']') {
            if (depth > 0) depth--;
            if (depth == 1) in_strings = in_block = 0;
            i++;
            continue;
        }
        if (c == ',') {
            if (depth > 0 && depth <= MA_DEPTH && is_obj[depth - 1]) want_key[depth - 1] = 1;
            i++;
            continue;
        }
        if (c == ':') {
            if (depth > 0 && depth <= MA_DEPTH) want_key[depth - 1] = 0;
            i++;
            continue;
        }
        if (c == '"') {
            size_t s = ++i;
            int key;
            while (i < len && t[i] != '"') i += t[i] == '\\' && i + 1 < len ? 2 : 1;
            key = depth > 0 && depth <= MA_DEPTH && is_obj[depth - 1] && want_key[depth - 1];
            if (key && depth == 1) { top_key = t + s; top_n = i - s; }
            if (!in_block) fn(ctx, t + s, i - s, key, key && depth == 3 && in_strings);
            if (i < len) i++;
            continue;
        }
        i++;
    }
}

typedef struct ma_json_ctx {
    const char *salt;
    const char *text;
    sh_ma_game_key_fn is_game_key;
    ma_buf *b;
    size_t copied;
    int changed;
    char **keys;
    size_t nkeys, capkeys;
    int oom;
} ma_json_ctx;

static int ma_game_key(sh_ma_game_key_fn fn, const char *s, size_t n)
{
    return fn ? fn(s, n) : 0;
}

static void ma_json_rewrite_cb(void *vctx, const char *s, size_t n, int is_key, int is_strings_key)
{
    ma_json_ctx *c = (ma_json_ctx *)vctx;
    size_t at = (size_t)(s - c->text);
    (void)is_key;
    if (is_strings_key) {
        if (ma_game_key(c->is_game_key, s, n)) return;
        ma_put(c->b, c->text + c->copied, at - c->copied);
        ma_put(c->b, c->salt, strlen(c->salt));
        ma_put(c->b, "_", 1);
        c->copied = at;                                   /* the key itself is copied with the next stretch */
        c->changed = 1;
        return;
    }
    {
        ma_buf one = { NULL, 0, 0, 0 };
        sh_ma_keys own;
        own.keys = c->keys;
        own.count = c->nkeys;
        if (ma_rewrite(c->salt, s, n, sh_ma_keys_has, &own, &one) && !one.oom) {
            ma_put(c->b, c->text + c->copied, at - c->copied);
            ma_put(c->b, one.p, one.n);
            c->copied = at + n;
            c->changed = 1;
        }
        if (one.oom) c->b->oom = 1;
        free(one.p);
    }
}

char *sh_ma_json(const char *salt, const char *text, size_t len, sh_ma_game_key_fn is_game_key, size_t *out_len)
{
    ma_buf b = { NULL, 0, 0, 0 };
    ma_json_ctx c;
    if (out_len) *out_len = 0;
    if (!salt || !text) return NULL;
    memset(&c, 0, sizeof c);
    c.salt = salt;
    c.text = text;
    c.is_game_key = is_game_key;
    c.b = &b;
    if (!sh_ma_string_keys(text, len, is_game_key, &c.keys, &c.nkeys)) { if (out_len) *out_len = (size_t)-1; return NULL; }
    ma_json_walk(text, len, ma_json_rewrite_cb, &c);
    sh_ma_keys_free(c.keys, c.nkeys);
    if (!c.changed) { free(b.p); return NULL; }
    ma_put(&b, text + c.copied, len - c.copied);
    if (b.oom) { free(b.p); if (out_len) *out_len = (size_t)-1; return NULL; }
    if (out_len) *out_len = b.n;
    return b.p;
}

/* The inverse of the strings-key half of sh_ma_json: a key of a `strings` language object written <salt>_<key> back
 * to <key>, for mod.json written from a build known only by its manifest (modbundle.c mb_author_synth). */
static void ma_unkey_cb(void *vctx, const char *s, size_t n, int is_key, int is_strings_key)
{
    ma_json_ctx *c = (ma_json_ctx *)vctx;
    size_t at = (size_t)(s - c->text), sl = strlen(c->salt);
    (void)is_key;
    if (!is_strings_key || n <= sl + 1 || strncmp(s, c->salt, sl) || s[sl] != '_') return;
    ma_put(c->b, c->text + c->copied, at - c->copied);
    c->copied = at + sl + 1;
    c->changed = 1;
}

char *sh_ma_json_author_keys(const char *salt, const char *text, size_t len, size_t *out_len)
{
    ma_buf b = { NULL, 0, 0, 0 };
    ma_json_ctx c;
    if (out_len) *out_len = 0;
    if (!salt || !text) return NULL;
    memset(&c, 0, sizeof c);
    c.salt = salt;
    c.text = text;
    c.b = &b;
    ma_json_walk(text, len, ma_unkey_cb, &c);
    if (!c.changed) { free(b.p); return NULL; }
    ma_put(&b, text + c.copied, len - c.copied);
    if (b.oom) { free(b.p); if (out_len) *out_len = (size_t)-1; return NULL; }
    if (out_len) *out_len = b.n;
    return b.p;
}

static void ma_keys_cb(void *vctx, const char *s, size_t n, int is_key, int is_strings_key)
{
    ma_json_ctx *c = (ma_json_ctx *)vctx;
    (void)is_key;
    if (!is_strings_key || c->oom || ma_game_key(c->is_game_key, s, n)) return;
    if (c->nkeys == c->capkeys) {
        size_t cap = c->capkeys ? c->capkeys * 2 : 16;
        char **g = (char **)realloc(c->keys, cap * sizeof *g);
        if (!g) { c->oom = 1; return; }
        c->keys = g;
        c->capkeys = cap;
    }
    if (!(c->keys[c->nkeys] = sh_textn(s, n))) { c->oom = 1; return; }
    c->nkeys++;
}

static int ma_str_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int sh_ma_string_keys(const char *text, size_t len, sh_ma_game_key_fn is_game_key, char ***keys, size_t *count)
{
    ma_json_ctx c;
    size_t i, w = 0;
    *keys = NULL;
    *count = 0;
    if (!text) return 1;
    memset(&c, 0, sizeof c);
    c.text = text;
    c.is_game_key = is_game_key;
    ma_json_walk(text, len, ma_keys_cb, &c);
    if (c.oom) { sh_ma_keys_free(c.keys, c.nkeys); return 0; }
    if (c.nkeys) qsort(c.keys, c.nkeys, sizeof *c.keys, ma_str_cmp);
    for (i = 0; i < c.nkeys; ++i) {
        if (w && !strcmp(c.keys[w - 1], c.keys[i])) { free(c.keys[i]); continue; }
        c.keys[w++] = c.keys[i];
    }
    *keys = c.keys;
    *count = w;
    return 1;
}

void sh_ma_keys_free(char **keys, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) free(keys[i]);
    free(keys);
}

int sh_ma_keys_has(void *ctx, const char *key, size_t n)
{
    const sh_ma_keys *k = (const sh_ma_keys *)ctx;
    size_t lo = 0, hi;
    if (!k || !k->count) return 0;
    hi = k->count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strncmp(k->keys[mid], key, n);
        if (c == 0 && k->keys[mid][n] != 0) c = 1;
        if (c == 0) return 1;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}
