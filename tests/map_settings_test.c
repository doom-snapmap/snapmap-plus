/* The open map's settings rows (map_settings.h): read and removed at load,
 * written back at save in the same bytes earlier builds wrote, held between. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/backend/map_settings.h"
#include "../src/backend/map_shards.h"

static int logs;
void backend_log(const char *s) { (void)s; logs++; }

/* A string variable as the engine and sh_shard_insert spell it. */
#define VAR(name, value)                                                                     \
    "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,"    \
    "\"~type\":\"|pointer\"},\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},"           \
    "\"initialValue\":\"" value "\",\"~type\":\"snapVarString_t\"}"
#define MAP(alloc, strings)                                                                  \
    "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0," alloc ",0,0,0,0,0],"          \
    "\"boolean\":[],\"string\":[" strings "],\"~type\":\"idSnapVariables\"},\"version\":111}"

static const char *const AUTHOR = VAR("String 1", "hello");

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

static char *take(const char *json, sh_map_settings *s, sh_map_settings_report *r)
{
    size_t n = 0;
    char *out = sh_map_settings_take(json, strlen(json), s, &n, r);
    if (out) assert(n == strlen(out) && valid(out));
    return out;
}

int main(void)
{
    sh_map_settings s, on, off;
    sh_map_settings_report r;
    char *a, *b;
    on_both(&on);
    sh_map_settings_default(&off);

    /* A map an earlier build saved with both on: the rows load as settings and
     * leave the text, allocCount[4] follows; the save puts back the same bytes. */
    {
        static const char saved[] = MAP("3", VAR("String 1", "hello") "," VAR("smp.render.v1", "4;0") ","
                                              VAR("smp.bounds.v1", "1;32671"));
        static const char bare[] = MAP("1", VAR("String 1", "hello"));
        a = take(saved, &s, &r);
        assert(a && !strcmp(a, bare) && !strstr(a, "smp."));
        assert(r.found == 2 && r.removed == 2 && !r.kept && !r.refused && !r.failed);
        assert(sh_map_render_unlocked(&s.render) && sh_map_bounds_enabled(&s.bounds) && s.bounds.limit == SH_BOUNDS_CEILING);
        b = put(a, &s, &r);
        assert(b && !strcmp(b, saved) && r.written == 2 && !r.removed);
        HeapFree(GetProcessHeap(), 0, b);
        /* Turning both on in a fresh copy writes the same rows in the same order. */
        b = put(bare, &on, &r);
        assert(b && !strcmp(b, saved));
        HeapFree(GetProcessHeap(), 0, b);
        /* Both off: nothing is written. */
        assert(!put(a, &off, &r) && !r.written && !r.failed);
        HeapFree(GetProcessHeap(), 0, a);
        /* One on: only its row. */
        sh_map_settings_default(&s);
        sh_map_render_from_toggle(1, &s.render);
        b = put(bare, &s, &r);
        assert(b && !strcmp(b, MAP("2", VAR("String 1", "hello") "," VAR("smp.render.v1", "4;0"))));
        HeapFree(GetProcessHeap(), 0, b);
    }

    /* A map with neither row stays byte-identical both ways. */
    {
        static const char plain[] = MAP("1", VAR("String 1", "hello"));
        static const char empty[] = MAP("0", "");
        s = on;
        assert(!take(plain, &s, &r) && !r.found && !r.failed);
        assert(!sh_map_render_unlocked(&s.render) && !sh_map_bounds_enabled(&s.bounds));
        assert(!put(plain, &off, &r) && !r.failed);
        /* An empty list gains rows without a leading comma. */
        b = put(empty, &on, &r);
        assert(b && !strcmp(b, MAP("2", VAR("smp.render.v1", "4;0") "," VAR("smp.bounds.v1", "1;32671"))));
        a = take(b, &s, &r);
        assert(a && !strcmp(a, empty) && r.removed == 2);
        HeapFree(GetProcessHeap(), 0, a);
        HeapFree(GetProcessHeap(), 0, b);
    }

    /* Legacy row versions: an older render row reads as on and saves as 4;0;
     * an older bounds value keeps its limit; native values read as off and
     * save as no row. */
    {
        static const char old[] = MAP("3", VAR("String 1", "hello") "," VAR("smp.bounds.v1", "1;20000") ","
                                            VAR("smp.render.v1", "1;60000;100;1500;6500;0.35;0.4;0.45"));
        a = take(old, &s, &r);
        assert(a && r.removed == 2 && sh_map_render_unlocked(&s.render) && s.bounds.limit == 20000.0f);
        b = put(a, &s, &r);
        assert(b && !strcmp(b, MAP("3", VAR("String 1", "hello") "," VAR("smp.render.v1", "4;0") ","
                                         VAR("smp.bounds.v1", "1;20000"))));
        HeapFree(GetProcessHeap(), 0, a);
        HeapFree(GetProcessHeap(), 0, b);
    }
    {
        static const char native[] = MAP("3", VAR("String 1", "hello") "," VAR("smp.render.v1", "4;8192") ","
                                               VAR("smp.bounds.v1", "1;0"));
        a = take(native, &s, &r);
        assert(a && !strcmp(a, MAP("1", VAR("String 1", "hello"))) && r.removed == 2);
        assert(!sh_map_render_unlocked(&s.render) && !sh_map_bounds_enabled(&s.bounds));
        assert(!put(a, &s, &r));
        HeapFree(GetProcessHeap(), 0, a);
        /* A disabled version-2 row is off too. */
        a = take(MAP("1", VAR("smp.render.v1", "2;0;200000;80;500;1500;.1;.2;.3")), &s, &r);
        assert(a && !sh_map_render_unlocked(&s.render));
        HeapFree(GetProcessHeap(), 0, a);
    }

    /* Malformed or duplicated rows read as off and are still removed. */
    {
        a = take(MAP("2", VAR("smp.render.v1", "4;0") "," VAR("smp.render.v1", "4;0")), &s, &r);
        assert(a && r.found == 2 && r.removed == 2 && r.refused == 2 && !sh_map_render_unlocked(&s.render));
        assert(!strcmp(a, MAP("0", "")));
        HeapFree(GetProcessHeap(), 0, a);
        a = take(MAP("1", VAR("smp.bounds.v1", "garbage")), &s, &r);
        assert(a && r.refused == 1 && !sh_map_bounds_enabled(&s.bounds));
        HeapFree(GetProcessHeap(), 0, a);
    }

    /* Stale rows at save (a strip that could not run, a prefab's rows): the
     * text ends up carrying exactly the current settings, once. */
    {
        static const char stale[] = MAP("2", VAR("String 1", "hello") "," VAR("smp.render.v1", "4;60000"));
        b = put(stale, &off, &r);
        assert(b && !strcmp(b, MAP("1", VAR("String 1", "hello"))) && r.removed == 1);
        HeapFree(GetProcessHeap(), 0, b);
        b = put(stale, &on, &r);
        assert(b && !strcmp(b, MAP("3", VAR("String 1", "hello") "," VAR("smp.render.v1", "4;0") ","
                                         VAR("smp.bounds.v1", "1;32671"))));
        HeapFree(GetProcessHeap(), 0, b);
        /* A row already right is taken out and written back in row order. */
        b = put(MAP("2", VAR("String 1", "hello") "," VAR("smp.render.v1", "4;0")), &on, &r);
        assert(b && !strcmp(b, MAP("3", VAR("String 1", "hello") "," VAR("smp.render.v1", "4;0") ","
                                         VAR("smp.bounds.v1", "1;32671"))));
        HeapFree(GetProcessHeap(), 0, b);
    }

    /* A row an author's variable follows keeps its index: it stays at load
     * (read all the same) and is rewritten in place at save. */
    {
        static const char mixed[] = MAP("3", VAR("String 1", "hello") "," VAR("smp.render.v1", "4;0") ","
                                              VAR("String 2", "bound by index"));
        a = take(mixed, &s, &r);
        assert(!a && r.found == 1 && r.kept == 1 && !r.removed && sh_map_render_unlocked(&s.render));
        b = put(mixed, &off, &r);
        assert(b && !strcmp(b, MAP("3", VAR("String 1", "hello") "," VAR("smp.render.v1", "4;8192") ","
                                         VAR("String 2", "bound by index"))));
        assert(r.kept == 1 && r.written == 1);
        HeapFree(GetProcessHeap(), 0, b);
        /* On: the kept row already says so; only bounds is appended, last. */
        b = put(mixed, &on, &r);
        assert(b && !strcmp(b, MAP("4", VAR("String 1", "hello") "," VAR("smp.render.v1", "4;0") ","
                                         VAR("String 2", "bound by index") "," VAR("smp.bounds.v1", "1;32671"))));
        HeapFree(GetProcessHeap(), 0, b);
    }

    /* Rows only Snapmap+ rows follow are removed; those rows stay. */
    {
        static const char carriers[] = MAP("4", VAR("String 1", "hello") "," VAR("smp.render.v1", "4;0") ","
                                                 VAR("smnav1.0.1.0123456789abcdef", "QUJD") ","
                                                 VAR("smp.bounds.v1", "1;32671"));
        a = take(carriers, &s, &r);
        assert(a && r.removed == 2 && !r.kept);
        assert(!strcmp(a, MAP("2", VAR("String 1", "hello") "," VAR("smnav1.0.1.0123456789abcdef", "QUJD"))));
        HeapFree(GetProcessHeap(), 0, a);
    }

    /* Pretty-printed text keeps its layout around the removed rows. */
    {
        static const char pretty[] =
            "{\n  \"variables\": {\n    \"allocCount\": [0, 0, 0, 0, 2],\n    \"string\": [\n"
            "      {\"info\": {\"name\": \"String 1\", \"~type\": \"snapVarInfo_t\"}, \"initialValue\": \"x\"},\n"
            "      {\"info\": {\"name\": \"smp.render.v1\", \"~type\": \"snapVarInfo_t\"}, \"initialValue\": \"4;0\"}\n"
            "    ]\n  }\n}\n";
        a = take(pretty, &s, &r);
        assert(a && r.removed == 1 && sh_map_render_unlocked(&s.render));
        assert(!strcmp(a,
            "{\n  \"variables\": {\n    \"allocCount\": [0, 0, 0, 0, 1],\n    \"string\": [\n"
            "      {\"info\": {\"name\": \"String 1\", \"~type\": \"snapVarInfo_t\"}, \"initialValue\": \"x\"}\n"
            "    ]\n  }\n}\n"));
        HeapFree(GetProcessHeap(), 0, a);
    }

    /* Text that does not read is left alone. */
    a = take("{\"variables\":{\"string\":[" VAR("smp.render.v1", "4;0"), &s, &r);
    assert(!a && r.failed && !sh_map_render_unlocked(&s.render));
    assert(!put("not a map smp.render.v1", &on, &r) && r.failed);

    /* The open map: a parse makes its settings current, a failed parse keeps
     * the current map's, a refused launch restores the editor map's, and a
     * closed map leaves both off. */
    {
        size_t n;
        sh_map_settings cur;
        sh_map_render render;
        sh_map_settings_reset();
        a = sh_map_settings_prepare_load(MAP("1", VAR("smp.render.v1", "4;0")), strlen(MAP("1", VAR("smp.render.v1", "4;0"))), &n);
        assert(a && n == strlen(a) && !strstr(a, "smp."));
        HeapFree(GetProcessHeap(), 0, a);
        sh_map_settings_current(&cur);
        assert(!sh_map_render_unlocked(&cur.render));     /* not until the parse succeeded */
        sh_map_settings_loaded(1);
        sh_map_settings_current(&cur);
        assert(sh_map_render_unlocked(&cur.render) && !sh_map_bounds_enabled(&cur.bounds));
        /* The editor's Apply. */
        sh_map_settings_set_bounds(&on.bounds);
        b = sh_map_settings_on_save(MAP("0", ""), strlen(MAP("0", "")), &n);
        assert(b && !strcmp(b, MAP("2", VAR("smp.render.v1", "4;0") "," VAR("smp.bounds.v1", "1;32671"))));
        HeapFree(GetProcessHeap(), 0, b);
        /* A failed parse of another map. */
        a = sh_map_settings_prepare_load(MAP("0", ""), strlen(MAP("0", "")), &n);
        assert(!a);
        sh_map_settings_loaded(0);
        sh_map_settings_current(&cur);
        assert(sh_map_render_unlocked(&cur.render) && sh_map_bounds_enabled(&cur.bounds));
        /* A vanilla map never inherits them. */
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
