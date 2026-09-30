/* Toybox label publication: display-only source qualifiers on package label
 * aliases, decided per selected language against every palette entry. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "package_library.h"
#include "package_runtime.h"
#include "strids.h"
#include "hook.h"
#include "package_fixture.h"
#include "../src/backend/virtual_material_runtime.c"

void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; assert(0); return NULL; }
sh_patch_status hook_commit(void *trampoline) { (void)trampoline; assert(0); return B2_PATCH_OK; }
int hook_is_installed(void *trampoline) { return trampoline != NULL; }
int hook_unpatch(void *trampoline) { (void)trampoline; return 1; }
static char g_log[4096];
void backend_log(const char *message) { strncat_s(g_log, sizeof(g_log), message, _TRUNCATE); }
int sh_user_overrides_enabled_for_launch(void) { return 1; }
static const sh_package_compilation *published;
const sh_package_compilation *sh_package_runtime_acquire(void) { return published; }
void sh_package_runtime_release(void) {}

#define ROWS 4096
static unsigned char dictionary_rows[ROWS][32];
static struct { void *rows; unsigned count, capacity; } dictionary = {dictionary_rows, 0, ROWS};
static char *pool[ROWS * 4]; static size_t pool_count;
static void number(void *p, size_t offset, int value) { memcpy((char *)p + offset, &value, 4); }
static uint32_t native_hash(const char *key)
{
    uint32_t hash = 2166136261u;
    for (; *key; key++) {
        unsigned char c = (unsigned char)*key;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        hash = (hash ^ c) * 16777619u;
    }
    return hash;
}
static void intern(void *out, const char *value)
{
    assert(pool_count < ROWS * 4); pool[pool_count] = _strdup(value); assert(pool[pool_count]);
    memcpy(out, pool + pool_count++, 8);
}
static int insert(void *table, void *row)
{
    assert(table == &dictionary && dictionary.count < ROWS);
    memcpy(dictionary_rows[dictionary.count++], row, 32); return (int)dictionary.count - 1;
}
static int row_order(const void *a, const void *b)
{
    uint32_t left, right; memcpy(&left, a, 4); memcpy(&right, b, 4);
    return left < right ? -1 : left > right;
}
static void sort_rows(void *context, void *rows, unsigned count, unsigned radix)
{ assert(context == &dictionary && radix == 0x20); qsort(rows, count, 32, row_order); }
static const char *value_of(const char *key)
{
    const char *result = NULL;
    for (unsigned i = 0; i < dictionary.count; i++) {
        const char *name = *(const char **)(dictionary_rows[i] + 8);
        if (name && !strcmp(name, key)) { assert(!result); result = *(const char **)(dictionary_rows[i] + 16); }
    }
    return result;
}

/* The native table under the selected language. */
static const char *language = "en";
static const char *locale_order[1];
static int locales(void *context, const char *const **order, size_t *count, char *error, size_t capacity)
{
    (void)context; (void)error; (void)capacity;
    locale_order[0] = language; *order = locale_order; *count = 1; return 1;
}
static const char *native_text(const char *key)
{
    int fr = !strcmp(language, "fr");
    if (!strcmp(key, "#str_speaker")) return fr ? "Haut-parleur" : "Speaker";
    if (!strcmp(key, "#str_other")) return fr ? "Autre" : "Speaker"; /* another key, same English text */
    if (!strcmp(key, "#str_cat_audio")) return fr ? "Audio" : "Audio";
    if (!strcmp(key, "#str_cat_light")) return fr ? "Lumiere" : "Light";
    if (!strcmp(key, "#str_cat_fx")) return fr ? "Effets" : "FX";
    if (!strcmp(key, "#str_meta_toybox")) return fr ? "Jouets" : "Toybox";
    if (!strcmp(key, "#str_bell")) return fr ? "Cloche" : "Bell";
    if (!strcmp(key, "#str_same") || !strcmp(key, "#str_lamp_unused")) return "";
    return NULL;
}
static int native_original(void *context, const char *key, unsigned char **body, size_t *length,
    char *error, size_t capacity)
{
    (void)context; (void)error; (void)capacity;
    const char *text = native_text(key);
    if (!text) { fprintf(stderr, "unexpected native lookup %s\n", key); assert(0); }
    *length = strlen(text); *body = malloc(*length + 1); assert(*body); memcpy(*body, text, *length); return 1;
}

/* The toybox roster (the runtime's served presentation): every stock row with
 * its own keys, and every row an alias is selected by, naming that alias. */
static const sh_strids_palette_entry stock_rows[] = {
    {"audio/2d_speaker", "#str_speaker", "#str_cat_audio", "#str_meta_toybox"},
    {"light/lamp_literal", "Lamp", "#str_cat_light", "#str_meta_toybox"},
    {"fx/bell", "#str_bell", "#str_cat_fx", "#str_meta_toybox"},
    {"fx/tam_literal", "Tam", "#str_cat_fx", "#str_meta_toybox"},
};
#define STOCK_ROWS (sizeof(stock_rows) / sizeof(*stock_rows))
static sh_strids_palette_entry roster[64];
static char roster_rows[64][96];
static size_t roster_count;
static int roster_state = 1;
static int palette_entries(void *context, const sh_strids_palette_entry **entries, size_t *count)
{
    (void)context;
    if (roster_state != 1) return roster_state;
    *entries = roster; *count = roster_count; return 1;
}

typedef struct alias_spec {
    const char *tag, *native, *locales, *row, *category, *sources;
    int listed; /* 0 no visible row selects it; 1 its row; 2 its row and an inheriting child */
} alias_spec;
static const alias_spec aliases[] = {
    /* Two package variants inheriting the stock label: both qualified. */
    {"p7", "#str_speaker", "{}", "smp/p7", "#str_cat_audio", "{\"0\":{\"name\":\"Perf probe2 7\",\"id\":\"acceptance.perf.probe2-7\",\"component\":\"\",\"row\":\"perfprobe2/p7/speaker_offset\"}}", 1},
    {"p8", "#str_speaker", "{}", "smp/p8", "#str_cat_audio", "{\"0\":{\"name\":\"Perf probe2 8\",\"id\":\"acceptance.perf.probe2-8\",\"component\":\"\",\"row\":\"perfprobe2/p8/speaker_offset\"}}", 1},
    /* Another key rendering the same English text collides too. */
    {"q", "#str_other", "{}", "smp/q", "#str_cat_audio", "{\"0\":{\"name\":\"Other speakers\",\"id\":\"local.other\",\"component\":\"\",\"row\":\"other/speaker\"}}", 1},
    /* Only French is authored: English falls back to the native text. */
    {"f", "#str_speaker", "{\"fr\":\"Haut-parleur F\"}", "smp/f", "#str_cat_audio", "{\"0\":{\"name\":\"French\",\"id\":\"local.french\",\"component\":\"\",\"row\":\"french/speaker\"}}", 1},
    /* An exact copy two packages supply is one entry naming both. */
    {"x", "#str_speaker", "{}", "smp/x", "#str_cat_audio", "{\"0\":{\"name\":\"Beta\",\"id\":\"local.beta\",\"component\":\"\",\"row\":\"copy/speaker\"},\"1\":{\"name\":\"Alpha\",\"id\":\"local.alpha\",\"component\":\"\",\"row\":\"copy/speaker\"}}", 1},
    /* A literal stock label is a peer. */
    {"l", "#str_lamp_unused", "{\"en\":\"Lamp\",\"fr\":\"Lampe\"}", "smp/l", "#str_cat_light", "{\"0\":{\"name\":\"Lamps\",\"id\":\"local.lamps\",\"component\":\"\",\"row\":\"lamps/lamp\"}}", 1},
    /* A literal base label is its own fallback in every language. */
    {"b", "Bulb", "{}", "smp/b", "#str_cat_light", "{\"0\":{\"name\":\"Bulbs\",\"id\":\"local.bulbs\",\"component\":\"\",\"row\":\"bulbs/bulb\"}}", 1},
    /* One key, different text in two packages (separate private keys): no collision. */
    {"red", "#str_same", "{\"en\":\"Red\",\"fr\":\"Red\"}", "smp/red", "#str_cat_fx", "{\"0\":{\"name\":\"Red\",\"id\":\"local.red\",\"component\":\"\",\"row\":\"fx/red\"}}", 1},
    {"green", "#str_same", "{\"en\":\"Green\",\"fr\":\"Green\"}", "smp/green", "#str_cat_fx", "{\"0\":{\"name\":\"Green\",\"id\":\"local.green\",\"component\":\"\",\"row\":\"fx/green\"}}", 1},
    /* The same label in another category is no collision. */
    {"c", "#str_speaker", "{}", "smp/c", "#str_cat_fx", "{\"0\":{\"name\":\"Elsewhere\",\"id\":\"local.elsewhere\",\"component\":\"\",\"row\":\"fx/speaker\"}}", 1},
    /* Equal package names need their ids. */
    {"e1", "#str_same", "{\"en\":\"Echo\",\"fr\":\"Echo\"}", "smp/e1", "#str_cat_fx", "{\"0\":{\"name\":\"Speakers\",\"id\":\"acme.speakers\",\"component\":\"\",\"row\":\"echo\"}}", 1},
    {"e2", "#str_same", "{\"en\":\"Echo\",\"fr\":\"Echo\"}", "smp/e2", "#str_cat_fx", "{\"0\":{\"name\":\"Speakers\",\"id\":\"other.speakers\",\"component\":\"\",\"row\":\"echo\"}}", 1},
    /* One package and component: its authored row names. */
    {"r1", "#str_same", "{\"en\":\"Rumble\",\"fr\":\"Rumble\"}", "smp/r1", "#str_cat_fx", "{\"0\":{\"name\":\"Pack\",\"id\":\"pack.id\",\"component\":\"sub\",\"row\":\"pack/r1\"}}", 1},
    {"r2", "#str_same", "{\"en\":\"Rumble\",\"fr\":\"Rumble\"}", "smp/r2", "#str_cat_fx", "{\"0\":{\"name\":\"Pack\",\"id\":\"pack.id\",\"component\":\"sub\",\"row\":\"pack/r2\"}}", 1},
    /* Nothing tells them apart: an ordinal by published row name. */
    {"o2", "#str_same", "{\"en\":\"Organ\",\"fr\":\"Organ\"}", "smp/o2", "#str_cat_fx", "{\"0\":{\"name\":\"Twin\",\"id\":\"twin.id\",\"component\":\"\",\"row\":\"organ\"}}", 1},
    {"o1", "#str_same", "{\"en\":\"Organ\",\"fr\":\"Organ\"}", "smp/o1", "#str_cat_fx", "{\"0\":{\"name\":\"Twin\",\"id\":\"twin.id\",\"component\":\"\",\"row\":\"organ\"}}", 1},
    /* Two mappings for one published row (the installed library's and the
     * map's, which the overlay serves): one tile, which alone keeps its base
     * label; the unselected mapping is no peer. */
    {"dupA", "#str_same", "{\"en\":\"Chime\",\"fr\":\"Chime\"}", "smp/dup", "#str_cat_fx", "{\"0\":{\"name\":\"Local chimes\",\"id\":\"local.chimes\",\"component\":\"\",\"row\":\"chime\"}}", 0},
    {"dupB", "#str_same", "{\"en\":\"Chime\",\"fr\":\"Chime\"}", "smp/dup", "#str_cat_fx", "{\"0\":{\"name\":\"Map chimes\",\"id\":\"map.chimes\",\"component\":\"\",\"row\":\"chime\"}}", 1},
    /* The same with a real stock peer: only the selected mapping is qualified,
     * by its own sources. */
    {"bellA", "#str_bell", "{}", "smp/bell", "#str_cat_fx", "{\"0\":{\"name\":\"Local bells\",\"id\":\"local.bells\",\"component\":\"\",\"row\":\"bell\"}}", 0},
    {"bellB", "#str_bell", "{}", "smp/bell", "#str_cat_fx", "{\"0\":{\"name\":\"Map bells\",\"id\":\"map.bells\",\"component\":\"\",\"row\":\"bell\"}}", 1},
    /* A mapping whose row no longer serves is no peer of a live one. */
    {"ghost", "#str_same", "{\"en\":\"Harp\",\"fr\":\"Harp\"}", "smp/ghost", "#str_cat_fx", "{\"0\":{\"name\":\"Ghost\",\"id\":\"local.ghost\",\"component\":\"\",\"row\":\"harp\"}}", 0},
    {"harp", "#str_same", "{\"en\":\"Harp\",\"fr\":\"Harp\"}", "smp/harp", "#str_cat_fx", "{\"0\":{\"name\":\"Harps\",\"id\":\"local.harps\",\"component\":\"\",\"row\":\"harp\"}}", 1},
    /* Rows showing one alias (a child inherits it) are one key: no collision
     * alone, one qualifier beside a real peer. */
    {"gong", "#str_same", "{\"en\":\"Gong\",\"fr\":\"Gong\"}", "smp/gong", "#str_cat_fx", "{\"0\":{\"name\":\"Gongs\",\"id\":\"local.gongs\",\"component\":\"\",\"row\":\"gong\"}}", 2},
    {"tam", "#str_same", "{\"en\":\"Tam\",\"fr\":\"Tam\"}", "smp/tam", "#str_cat_fx", "{\"0\":{\"name\":\"Tams\",\"id\":\"local.tams\",\"component\":\"\",\"row\":\"tam\"}}", 2},
};
#define ALIASES (sizeof(aliases) / sizeof(*aliases))
static char alias_keys[ALIASES][96];

static void roster_add(const char *row, const char *label, const char *category, const char *meta)
{
    assert(roster_count < sizeof(roster) / sizeof(*roster));
    size_t at = roster_count++;
    strcpy_s(roster_rows[at], sizeof(roster_rows[at]), row);
    roster[at] = (sh_strids_palette_entry){roster_rows[at], label, category, meta};
}
static void build(sh_package_compilation *compiled, int reverse)
{
    char mapping[2048], child[96];
    memset(compiled, 0, sizeof(*compiled));
    roster_count = 0;
    for (size_t n = 0; n < ALIASES; n++) {
        size_t i = reverse ? ALIASES - 1 - n : n;
        const alias_spec *a = aliases + i;
        snprintf(alias_keys[i], sizeof(alias_keys[i]), "#str_smp_loc_%064zx", (size_t)(0x1000 + i));
        snprintf(mapping, sizeof(mapping), "{\"native\":\"%s\",\"locales\":%s,\"label\":{\"row\":\"%s\","
            "\"category\":\"%s\",\"meta\":\"#str_meta_toybox\",\"sources\":%s}}",
            a->native, a->locales, a->row, a->category, a->sources);
        char error[256] = "";
        int ok = sh_package_policy_localization(&compiled->policy, alias_keys[i], mapping, NULL, error, sizeof(error));
        if (!ok) fprintf(stderr, "alias %s: %s\n", a->tag, error);
        assert(ok);
        if (a->listed) roster_add(a->row, alias_keys[i], a->category, "#str_meta_toybox");
        if (a->listed == 2) {
            snprintf(child, sizeof(child), "%s_child", a->row);
            roster_add(child, alias_keys[i], a->category, "#str_meta_toybox");
        }
        if (n == ALIASES / 2)
            for (size_t s = 0; s < STOCK_ROWS; s++) {
                const sh_strids_palette_entry *stock = stock_rows + (reverse ? STOCK_ROWS - 1 - s : s);
                roster_add(stock->row, stock->label, stock->category, stock->meta);
            }
    }
}
static const char *text_of(const char *tag)
{
    for (size_t i = 0; i < ALIASES; i++) if (!strcmp(aliases[i].tag, tag)) return value_of(alias_keys[i]);
    assert(0); return NULL;
}
static int same(const char *tag, const char *expected)
{
    const char *actual = text_of(tag);
    if (!actual || strcmp(actual, expected)) {
        fprintf(stderr, "%s [%s]: '%s' expected '%s'\n", tag, language, actual ? actual : "<missing>", expected);
        return 0;
    }
    return 1;
}

int main(void)
{
    char temporary[MAX_PATH], path[1024];
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-labels-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    snprintf(path, sizeof(path), "%s/missing-user.json", root); assert(sh_strids_set_source(path));
    number(dictionary_rows[0], 0, (int)native_hash("#str_stock"));
    intern(dictionary_rows[0] + 8, "#str_stock"); intern(dictionary_rows[0] + 16, "Stock"); dictionary.count = 1;
    assert(sh_strids_test_inject(&dictionary, insert, native_hash, intern) > 0);
    sh_strids_test_set_sort(sort_rows);
    sh_strids_localization localization = {NULL, locales, native_original};
    sh_strids_set_localization(&localization);
    sh_strids_palette palette = {NULL, palette_entries};
    sh_strids_set_palette(&palette);

    /* The policy accepts label aliases (with or without authored locales) and
     * refuses a label without its row or sources. */
    {
        sh_package_policy policy = {0}; char error[256];
        assert(!sh_package_policy_localization(&policy, "#str_smp_loc_bad",
            "{\"native\":\"#str_speaker\",\"locales\":{},\"label\":{\"category\":\"x\"}}", NULL, error, sizeof(error)));
        assert(!sh_package_policy_localization(&policy, "#str_smp_loc_bad",
            "{\"native\":\"#str_speaker\",\"locales\":{}}", NULL, error, sizeof(error)));
        sh_package_policy_free(&policy);
    }

    const char *english[][2] = {
        {"p7", "Speaker (Perf probe2 7)"}, {"p8", "Speaker (Perf probe2 8)"}, {"q", "Speaker (Other speakers)"},
        {"f", "Speaker (French)"}, {"x", "Speaker (Alpha + Beta)"}, {"l", "Lamp (Lamps)"}, {"b", "Bulb"},
        {"red", "Red"}, {"green", "Green"}, {"c", "Speaker"},
        {"e1", "Echo (Speakers [acme.speakers])"}, {"e2", "Echo (Speakers [other.speakers])"},
        {"r1", "Rumble (Pack [pack.id], sub, pack/r1)"}, {"r2", "Rumble (Pack [pack.id], sub, pack/r2)"},
        {"o1", "Organ (Twin [twin.id], organ #1)"}, {"o2", "Organ (Twin [twin.id], organ #2)"},
        {"dupA", "Chime"}, {"dupB", "Chime"}, {"bellA", "Bell"}, {"bellB", "Bell (Map bells)"},
        {"ghost", "Harp"}, {"harp", "Harp"}, {"gong", "Gong"}, {"tam", "Tam (Tams)"},
    };
    const char *french[][2] = {
        /* In French "#str_other" reads "Autre" and F has its own text: they leave the group. */
        {"p7", "Haut-parleur (Perf probe2 7)"}, {"p8", "Haut-parleur (Perf probe2 8)"}, {"q", "Autre"},
        {"f", "Haut-parleur F"}, {"x", "Haut-parleur (Alpha + Beta)"}, {"l", "Lampe"}, {"b", "Bulb"},
        {"red", "Red"}, {"green", "Green"}, {"c", "Haut-parleur"},
        {"e1", "Echo (Speakers [acme.speakers])"}, {"o2", "Organ (Twin [twin.id], organ #2)"},
        {"dupA", "Chime"}, {"dupB", "Chime"}, {"bellA", "Cloche"}, {"bellB", "Cloche (Map bells)"},
        {"ghost", "Harp"}, {"harp", "Harp"}, {"gong", "Gong"}, {"tam", "Tam (Tams)"},
    };
    for (int order = 0; order < 2; order++) {
        sh_package_compilation compiled;
        build(&compiled, order);
        published = &compiled;
        language = "en"; assert(sh_strids_rearm());
        int ok = 1;
        for (size_t i = 0; i < sizeof(english) / sizeof(*english); i++) ok &= same(english[i][0], english[i][1]);
        assert(ok);
        assert(!strcmp(value_of("#str_stock"), "Stock"));
        language = "fr"; assert(sh_strids_rearm());
        for (size_t i = 0; i < sizeof(french) / sizeof(*french); i++) ok &= same(french[i][0], french[i][1]);
        assert(ok);
        /* No shipped roster yet: every alias shows its base label. */
        language = "en"; roster_state = 0; assert(sh_strids_rearm());
        ok &= same("p7", "Speaker") && same("x", "Speaker") && same("o1", "Organ") && same("bellB", "Bell");
        assert(ok);
        /* A failing roster refuses the refresh and leaves the published text. */
        roster_state = -1; g_log[0] = 0; assert(!sh_strids_rearm()); assert(strstr(g_log, "toybox labels"));
        roster_state = 1; assert(sh_strids_rearm()); assert(same("p7", "Speaker (Perf probe2 7)"));
        published = NULL; assert(sh_strids_rearm());
        for (size_t i = 0; i < ALIASES; i++) assert(!value_of(alias_keys[i]));
        sh_package_policy_free(&compiled.policy);
    }
    sh_strids_set_palette(NULL); sh_strids_set_localization(NULL);
    /* Release injector-owned copies before freeing the synthetic native pool. */
    sh_strids_test_inject(&dictionary, insert, native_hash, intern);
    while (pool_count) free(pool[--pool_count]);
    cleanup(); assert(!failures);
    puts("toybox label qualifiers: locale grouping, fallback, peers, distinct and ordered qualifiers, "
        "selected aliases of the served roster passed");
    return 0;
}
