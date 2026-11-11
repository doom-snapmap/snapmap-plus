/* The open map's settings (map_settings.h): read from the map's member at
 * load, written into the member at save, held between. The variables earlier
 * builds wrote are the store tool's (map_old_test). */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/backend/map_settings.h"
#include "../src/backend/map_shards.h"

static int logs;
void backend_log(const char *s) { (void)s; logs++; }

/* A string variable as the engine spells it. */
#define VAR(name, value)                                                                     \
    "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,"    \
    "\"~type\":\"|pointer\"},\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},"           \
    "\"initialValue\":\"" value "\",\"~type\":\"snapVarString_t\"}"
#define MAPM(alloc, strings, member)                                                         \
    "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0," alloc ",0,0,0,0,0],"          \
    "\"boolean\":[],\"string\":[" strings "],\"~type\":\"idSnapVariables\"},\"version\":111" member "}"
#define MAP(alloc, strings) MAPM(alloc, strings, "")
/* The member as a save writes it (map_carrier.h). */
#define SET(body) ",\"snapmap-plus\":{\"format\":1,\"settings\":{" body "}}"
#define BOTH SET("\"render\":\"4;0\",\"bounds\":\"1;32671\"")

static void on_both(sh_map_settings *s)
{
    sh_map_settings_default(s);
    sh_map_render_from_toggle(1, &s->render);
    sh_map_bounds_from_toggle(1, &s->bounds);
}

static int valid(const char *json)
{
    sh_shard_doc d;
    int ok = sh_shard_doc_build(json, strlen(json), &d);
    sh_shard_doc_free(&d);
    return ok && !strstr(json, ",,") && !strstr(json, ",]") && !strstr(json, "[,");
}

static char *put(const char *json, const sh_map_settings *s, sh_map_settings_report *r)
{
    size_t n = 0;
    char *out = sh_map_settings_put(json, strlen(json), s, &n, r);
    if (out) assert(n == strlen(out) && valid(out));
    return out;
}

/* take() never changes the text: the member goes with the parse detour's last step. */
static void take(const char *json, sh_map_settings *s, sh_map_settings_report *r)
{
    size_t n = 7;
    char *out = sh_map_settings_take(json, strlen(json), s, &n, r);
    assert(!out && n == 0);
}

int main(void)
{
    sh_map_settings s, on, off;
    sh_map_settings_report r;
    char *b;
    on_both(&on);
    sh_map_settings_default(&off);

    /* Both on: the save writes the member and leaves the author's variables
     * alone; the member reads back as the same settings. */
    {
        static const char bare[] = MAP("1", VAR("String 1", "hello"));
        static const char member[] = MAPM("1", VAR("String 1", "hello"), BOTH);
        b = put(bare, &on, &r);
        assert(b && !strcmp(b, member) && r.written == 2 && !r.failed);
        HeapFree(GetProcessHeap(), 0, b);
        take(member, &s, &r);
        assert(r.member && !r.refused && !r.failed);
        assert(sh_map_render_unlocked(&s.render) && sh_map_bounds_enabled(&s.bounds) && s.bounds.limit == SH_BOUNDS_CEILING);
        /* Putting what the member already says changes nothing. */
        assert(!put(member, &s, &r) && !r.failed);
        /* Both off: nothing is written, and an existing member part goes. */
        assert(!put(bare, &off, &r) && !r.written && !r.failed);
        b = put(member, &off, &r);
        assert(b && !strcmp(b, bare));
        HeapFree(GetProcessHeap(), 0, b);
        /* One on: only its key. */
        sh_map_settings_default(&s);
        sh_map_render_from_toggle(1, &s.render);
        b = put(bare, &s, &r);
        assert(b && !strcmp(b, MAPM("1", VAR("String 1", "hello"), SET("\"render\":\"4;0\""))));
        HeapFree(GetProcessHeap(), 0, b);
    }

    /* The member's values: one that does not read is off; a member without a
     * settings part, or a malformed one, is both off. */
    {
        take(MAPM("0", "", SET("\"render\":\"garbage\",\"bounds\":\"1;20000\"")), &s, &r);
        assert(r.member && r.refused == 1 && !sh_map_render_unlocked(&s.render) && s.bounds.limit == 20000.0f);
        take(MAPM("0", "", ",\"snapmap-plus\":{\"format\":1,\"mods\":[]}"), &s, &r);
        assert(!r.member && !sh_map_render_unlocked(&s.render) && !sh_map_bounds_enabled(&s.bounds));
        take(MAPM("0", "", ",\"snapmap-plus\":{\"format\":1,\"settings\":[]}"), &s, &r);
        assert(!r.member && !sh_map_render_unlocked(&s.render));
        /* A variable with an earlier build's name is the author's text here:
         * nothing reads it (the game refuses such a map before this runs). */
        s = on;
        take(MAP("1", VAR("smp.render.v1", "4;0")), &s, &r);
        assert(!r.member && !sh_map_render_unlocked(&s.render) && !sh_map_bounds_enabled(&s.bounds));
    }

    /* A map with neither stays byte-identical both ways. */
    {
        static const char plain[] = MAP("1", VAR("String 1", "hello"));
        static const char empty[] = MAP("0", "");
        s = on;
        take(plain, &s, &r);
        assert(!r.failed && !r.member && !sh_map_render_unlocked(&s.render) && !sh_map_bounds_enabled(&s.bounds));
        assert(!put(plain, &off, &r) && !r.failed);
        b = put(empty, &on, &r);
        assert(b && !strcmp(b, MAPM("0", "", BOTH)));
        HeapFree(GetProcessHeap(), 0, b);
        /* A root with no member at all gains one without a leading comma. */
        b = put("{}", &on, &r);
        assert(b && !strcmp(b, "{\"snapmap-plus\":{\"format\":1,\"settings\":{\"render\":\"4;0\",\"bounds\":\"1;32671\"}}}"));
        HeapFree(GetProcessHeap(), 0, b);
    }

    /* Pretty-printed text: the member is read and written in it. */
    {
        static const char pretty[] =
            "{\n  \"variables\": {\n    \"allocCount\": [0, 0, 0, 0, 1],\n    \"string\": [\n"
            "      {\"info\": {\"name\": \"String 1\", \"~type\": \"snapVarInfo_t\"}, \"initialValue\": \"x\"}\n"
            "    ]\n  }\n}\n";
        sh_map_settings_default(&s);
        sh_map_render_from_toggle(1, &s.render);
        b = put(pretty, &s, &r);
        assert(b && !strcmp(b,
            "{\n  \"variables\": {\n    \"allocCount\": [0, 0, 0, 0, 1],\n    \"string\": [\n"
            "      {\"info\": {\"name\": \"String 1\", \"~type\": \"snapVarInfo_t\"}, \"initialValue\": \"x\"}\n"
            "    ]\n  },\"snapmap-plus\":{\"format\":1,\"settings\":{\"render\":\"4;0\"}}\n}\n"));
        take(b, &s, &r);
        assert(r.member && sh_map_render_unlocked(&s.render));
        HeapFree(GetProcessHeap(), 0, b);
    }

    /* Text that does not read is left alone. */
    assert(!put("not a map", &on, &r) && r.failed);

    /* The open map: a parse makes its settings current, a failed parse keeps
     * the current map's, a refused launch restores the editor map's, and a
     * closed map leaves both off. */
    {
        size_t n;
        sh_map_settings cur;
        sh_map_render render;
        static const char rendered[] = MAPM("0", "", SET("\"render\":\"4;0\""));
        sh_map_settings_reset();
        assert(!sh_map_settings_prepare_load(rendered, strlen(rendered), &n));
        sh_map_settings_current(&cur);
        assert(!sh_map_render_unlocked(&cur.render));     /* not until the parse succeeded */
        sh_map_settings_loaded(1);
        sh_map_settings_current(&cur);
        assert(sh_map_render_unlocked(&cur.render) && !sh_map_bounds_enabled(&cur.bounds));
        /* The editor's Apply. */
        sh_map_settings_set_bounds(&on.bounds);
        b = sh_map_settings_on_save(MAP("0", ""), strlen(MAP("0", "")), &n);
        assert(b && !strcmp(b, MAPM("0", "", BOTH)) && n == strlen(b));
        HeapFree(GetProcessHeap(), 0, b);
        /* A member-carrying map loads the same way; its member stays for the
         * carrier strip that runs last. */
        assert(!sh_map_settings_prepare_load(MAPM("0", "", BOTH), strlen(MAPM("0", "", BOTH)), &n));
        sh_map_settings_loaded(1);
        sh_map_settings_current(&cur);
        assert(sh_map_render_unlocked(&cur.render) && sh_map_bounds_enabled(&cur.bounds));
        /* A failed parse of another map. */
        assert(!sh_map_settings_prepare_load(MAP("0", ""), strlen(MAP("0", "")), &n));
        sh_map_settings_loaded(0);
        sh_map_settings_current(&cur);
        assert(sh_map_render_unlocked(&cur.render) && sh_map_bounds_enabled(&cur.bounds));
        /* A vanilla map never inherits them, and saves as the engine wrote it. */
        (void)sh_map_settings_prepare_load(MAP("0", ""), strlen(MAP("0", "")), &n);
        sh_map_settings_loaded(1);
        sh_map_settings_current(&cur);
        assert(!sh_map_render_unlocked(&cur.render) && !sh_map_bounds_enabled(&cur.bounds));
        assert(!sh_map_settings_on_save(MAP("0", ""), strlen(MAP("0", "")), &n));
        /* That launch was refused: the editor map's come back, once. */
        assert(sh_map_settings_restore_previous());
        sh_map_settings_current(&cur);
        assert(sh_map_render_unlocked(&cur.render) && sh_map_bounds_enabled(&cur.bounds));
        assert(!sh_map_settings_restore_previous());
        /* Invalid values are refused. */
        render.unlocked = 5;
        sh_map_settings_set_render(&render);
        sh_map_settings_current(&cur);
        assert(cur.render.unlocked == 1);
        sh_map_settings_reset();
        sh_map_settings_current(&cur);
        assert(!sh_map_render_unlocked(&cur.render) && !sh_map_bounds_enabled(&cur.bounds));
        assert(!sh_map_settings_restore_previous());
    }
    puts("map_settings_test: passed");
    return 0;
}
