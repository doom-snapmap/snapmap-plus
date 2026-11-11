/* modcontent.c -- see modcontent.h. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "modcontent.h"
#include "modscene_decl.h"
#include "modhud.h"
#include "config_json.h"

static int mc_cause(char *cause, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (cause && cap) {
        va_start(ap, fmt);
        _vsnprintf_s(cause, cap, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    return 0;
}

/* The scene reader's first refusal, without its log prefix. */
typedef struct mc_scene { char why[400]; } mc_scene;

static void mc_scene_line(void *context, const char *line)
{
    mc_scene *s = (mc_scene *)context;
    /* The reader's first refusal is the cause; its lines name the entry. */
    if (!s->why[0]) strncpy_s(s->why, sizeof s->why, line, _TRUNCATE);
}

/* The HUD rule's weapon name, as modhud.c reads it. */
static int mc_hud_name(const char *s)
{
    const char *part = s;
    size_t n = strlen(s), i;
    if (!n || s[0] == '/' || s[n - 1] == '/') return 0;
    for (i = 0; i <= n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!c || c == '/') {
            size_t len = (size_t)(s + i - part);
            if (!len || (len == 1 && part[0] == '.') || (len == 2 && part[0] == '.' && part[1] == '.')) return 0;
            part = s + i + 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            return 0;
        }
    }
    return 1;
}

/* modhud.c sh_modhud_parse's grammar, without its rule table:
 * {"weapons": {"<weapon decl>": {"ammo_display": "weapon" | "engine"}}}. */
static int mc_hud(const char *json, size_t len, char *cause, size_t cap)
{
    sh_json_object root = {0}, weapons = {0};
    const char *raw;
    size_t i;
    int ok = 0;
    if (!sh_json_parse_object(json, len, 5, &root))
        return mc_cause(cause, cap, "mod.json \"hud\" must be an object like {\"weapons\": {...}}; correct it and press Refresh");
    if (!root.count) { sh_json_object_free(&root); return 1; }
    raw = sh_json_object_get(&root, "weapons");
    if (root.count != 1 || !raw || !sh_json_parse_object(raw, strlen(raw), 3, &weapons)) {
        mc_cause(cause, cap, "mod.json \"hud\" may hold only \"weapons\", an object of weapon names; correct it and press "
                 "Refresh");
        goto done;
    }
    for (i = 0; i < weapons.count; i++) {
        const sh_json_member *m = &weapons.members[i];
        sh_json_object settings = {0};
        char mode[24];
        size_t got = 0;
        const char *v;
        int valid;
        if (strlen(m->key) != m->key_length || !mc_hud_name(m->key)) {
            mc_cause(cause, cap, "mod.json \"hud\" names the weapon \"%s\", which is not a weapon decl name (lower-case "
                     "letters, digits, _, - and . in /-separated parts); correct it and press Refresh", m->key);
            goto done;
        }
        valid = sh_json_parse_object(m->value_json, strlen(m->value_json), 2, &settings) && settings.count == 1 &&
                (v = sh_json_object_get(&settings, "ammo_display")) != NULL &&
                sh_json_decode_string(v, strlen(v), mode, sizeof mode, &got) && strlen(mode) == got &&
                (!strcmp(mode, "weapon") || !strcmp(mode, "engine"));
        sh_json_object_free(&settings);
        if (!valid) {
            mc_cause(cause, cap, "mod.json \"hud\" gives the weapon %s something other than {\"ammo_display\": \"weapon\"} or "
                     "{\"ammo_display\": \"engine\"}; correct it and press Refresh", m->key);
            goto done;
        }
    }
    ok = 1;
done:
    sh_json_object_free(&weapons);
    sh_json_object_free(&root);
    return ok;
}

int sh_modcontent_check(const char *salt, const char *extra, size_t len, char *cause, size_t cap)
{
    sh_json_object root = {0};
    const char *hud;
    int ok = 1;
    if (cause && cap) cause[0] = 0;
    if (!extra || !len || !salt) return 1;
    {
        sh_scene_table t;
        mc_scene s;
        memset(&t, 0, sizeof t);
        s.why[0] = 0;
        if (sh_scene_add_bundle(&t, salt, extra, len, mc_scene_line, &s) < 0)
            ok = mc_cause(cause, cap, "mod.json \"scenes\" is refused: %s; correct it and press Refresh",
                          s.why[0] ? s.why : "it does not read");
        sh_scene_table_free(&t);
        if (!ok) return 0;
    }
    if (!sh_json_parse_object(extra, len, 24, &root)) return 1;   /* the builder read it; nothing more to check */
    hud = sh_json_object_get(&root, "hud");
    if (hud) ok = mc_hud(hud, strlen(hud), cause, cap);
    sh_json_object_free(&root);
    return ok;
}
