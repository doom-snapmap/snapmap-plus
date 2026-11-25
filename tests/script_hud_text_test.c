/* Exercise the shipped HUD adapter with a stock message and a separate dynamic display entry. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/backend/script/script_hud_text.h"
#include "../src/backend/typeinfo.h"

static unsigned char player[32], manager[32], screen[32], widget[32];
static unsigned char movie[512], root[256], source[256], copy[256], entry[96], field[256];
static int clones, visible, have_field = 1;
static char clip_name[48], message[64];

int sh_typeinfo_class_field(const char *type, uint32_t index, sh_ti_field *out)
{
    static const char *types[] = { "idPlayer", "localView_t", "idPlayerHud_Render", "idMenuManager_Hud_Coop",
                                   "idMenuScreen_Hud_Coop", "idSWFTextInstance" };
    static const char *fields[] = { "localView", "hudRender", "coopHudMgr", "hudCoopScreen", "centerMessage", "color" };
    static const unsigned offsets[] = { 8, 0, 8, 8, 8, 0xc8 };
    if (index) return 0;
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++) {
        if (!strcmp(type, types[i])) {
            out->name = fields[i]; out->offset = offsets[i];
            return 1;
        }
    }
    return 0;
}

static void make_string(void *s, const char *name) { *(const char **)s = name; }
static void drop(void *s) { (void)s; }
static unsigned char resolve(void *w) { return w == widget; }
static void *find_entry(void *p, int depth) { return p == root && depth == 0x4200 && clones ? entry : NULL; }
static void *duplicate(void *s, int n, void *str)
{
    assert(s == source && n == 0x200 && clones == 0);
    strcpy_s(clip_name, sizeof clip_name, *(const char **)str);
    ++clones;
    memcpy(copy, source, sizeof copy);
    *(int *)(copy + 0x38) = 0x4200;
    *(void **)(entry + 0x48) = copy;
    return copy;
}
static void *lookup(void *p, void *value, const char *key, const char *next, void *a, void *b, void *c, void *d)
{
    (void)a; (void)b; (void)c; (void)d;
    *(void **)value = NULL;
    if (p == root && clones && !strcmp(key, clip_name) && !next) *(void **)value = copy;
    if (p == copy && have_field && !strcmp(key, "message") && next && !strcmp(next, "txtField"))
        *(void **)value = field;
    return value;
}
static void *instance(void *value) { return *(void **)value; }
static void set_text(void *p, const char *txt) { assert(p == field); strcpy_s(message, sizeof message, txt); }
static void set_visible(void *p, char show, char propagate) { assert(p == copy && !propagate); visible = show; }
static int label(void *p, const char *name) { assert(p == copy && !strcmp(name, "rollOn")); return 3; }
static void play(void *p, int frame) { assert(p == copy && frame == 3); }

int main(void)
{
#define BIND(name, fn) { name, SIG_OK, (uintptr_t)fn, 0 }
    const sig_result symbols[] = {
        BIND("IdStrCtor", make_string), BIND("IdStrDtor", drop), BIND("SwfValueDestroy", drop),
        BIND("SwfWidgetResolve", resolve), BIND("SwfDisplayEntry", find_entry), BIND("SwfDuplicateSprite", duplicate),
        BIND("SwfNestedValue", lookup), BIND("SwfSpriteInstance", instance), BIND("SwfTextInstance", instance),
        BIND("SwfSetText", set_text), BIND("SwfSpriteSetVisible", set_visible), BIND("SwfFrameLabel", label),
        BIND("SwfPlayFrame", play)
    };
    const char *name = "smp_a_text_2";
    const float colour[] = {0.25f, 0.5f, 0.75f, 1};
    unsigned char original[sizeof source];
    sh_script_hud_bind(symbols, sizeof symbols / sizeof symbols[0]);
    assert(!sh_script_hud_text(player, name, 2, "early", colour));
    *(void **)(player + 16) = manager;
    *(void **)(manager + 8) = screen;
    *(void **)(screen + 8) = widget;
    *(void **)(widget + 8) = source;
    *(void **)(source + 0x30) = root;
    *(void **)(source + 0x20) = movie;
    *(float *)(movie + 0x1bc) = 1920;
    *(float *)(movie + 0x1c0) = 1080;
    memcpy(original, source, sizeof source);
    assert(sh_script_hud_text(player, name, 2, "Bottom", colour));
    assert(visible && clones == 1 && !strcmp(message, "Bottom"));
    assert(*(float *)(entry + 0x18) == 960 && *(float *)(entry + 0x1c) == 918);
    assert(field[0xc8] == 64 && field[0xc9] == 128 && field[0xca] == 191 && field[0xcb] == 255);
    assert(*(float *)(entry + 0x2c) == 1 && !memcmp(original, source, sizeof source));
    assert(sh_script_hud_text(player, name, 2, "Replaced", colour) && !strcmp(message, "Replaced"));
    assert(sh_script_hud_text(player, name, 2, "", NULL) && !visible);
    have_field = 0;
    assert(!sh_script_hud_text(player, name, 2, "missing field", colour) && !visible && clones == 1);
    puts("script_hud_text_test: ok");
    return 0;
}
