/* A private copy of the SnapMap HUD's message clip for each mod and screen position. */
#include "script_hud_text.h"
#include "../typeinfo.h"
#include <windows.h>
#include <string.h>

typedef union { uint64_t align; unsigned char bytes[48]; } hud_string;
typedef union { uint64_t align; unsigned char bytes[16]; } hud_value;

typedef void (*string_fn)(void *, const char *);
typedef void (*destroy_fn)(void *);
typedef unsigned char (*resolve_fn)(void *);
typedef void *(*entry_fn)(void *, int);
typedef void *(*clone_fn)(void *, int, void *);
typedef void *(*lookup_fn)(void *, void *, const char *, const char *, void *, void *, void *, void *);
typedef void *(*instance_fn)(void *);
typedef void (*visible_fn)(void *, char, char);
typedef int (*label_fn)(void *, const char *);
typedef void (*play_fn)(void *, int);

static string_fn make_string, set_text;
static destroy_fn drop_string, drop_value;
static resolve_fn resolve_widget;
static entry_fn display_entry;
static clone_fn duplicate;
static lookup_fn lookup;
static instance_fn as_sprite, as_text;
static visible_fn set_visible;
static label_fn frame_label;
static play_fn play_frame;
static int ready;

void sh_script_hud_bind(const sig_result *results, size_t count)
{
#define BIND(var, type, key) var = (type)sig_pointer(results, count, key)
    BIND(make_string, string_fn, "IdStrCtor");
    BIND(drop_string, destroy_fn, "IdStrDtor");
    BIND(drop_value, destroy_fn, "SwfValueDestroy");
    BIND(resolve_widget, resolve_fn, "SwfWidgetResolve");
    BIND(display_entry, entry_fn, "SwfDisplayEntry");
    BIND(duplicate, clone_fn, "SwfDuplicateSprite");
    BIND(lookup, lookup_fn, "SwfNestedValue");
    BIND(as_sprite, instance_fn, "SwfSpriteInstance");
    BIND(as_text, instance_fn, "SwfTextInstance");
    BIND(set_text, string_fn, "SwfSetText");
    BIND(set_visible, visible_fn, "SwfSpriteSetVisible");
    BIND(frame_label, label_fn, "SwfFrameLabel");
    BIND(play_frame, play_fn, "SwfPlayFrame");
#undef BIND
    ready = make_string && drop_string && drop_value && resolve_widget && display_entry && duplicate &&
            lookup && as_sprite && as_text && set_text && set_visible && frame_label && play_frame;
}

static int field_offset(const char *type, const char *name, unsigned *offset)
{
    sh_ti_field field;
    for (unsigned i = 0; sh_typeinfo_class_field(type, i, &field); ++i) {
        if (!strcmp(field.name, name)) { *offset = field.offset; return 1; }
    }
    return 0;
}

typedef struct {
    unsigned view, render, manager, screen, widget, colour;
} hud_layout;

static int read_layout(hud_layout *l)
{
    return field_offset("idPlayer", "localView", &l->view) &&
           field_offset("localView_t", "hudRender", &l->render) &&
           field_offset("idPlayerHud_Render", "coopHudMgr", &l->manager) &&
           field_offset("idMenuManager_Hud_Coop", "hudCoopScreen", &l->screen) &&
           field_offset("idMenuScreen_Hud_Coop", "centerMessage", &l->widget) &&
           field_offset("idSWFTextInstance", "color", &l->colour);
}

static unsigned char *child(void *parent, const char *name, const char *nested, instance_fn instance)
{
    hud_value value;
    void *result = instance(lookup(parent, &value, name, nested, NULL, NULL, NULL, NULL));
    drop_value(&value);
    return (unsigned char *)result;
}

static unsigned char *copy_clip(void *parent, void *source, const char *name)
{
    hud_string id;
    void *copy;
    int depth = 0x4200;
    while (depth <= 0x7fff && display_entry(parent, depth)) ++depth;
    if (depth > 0x7fff) return NULL;
    make_string(&id, name);
    copy = duplicate(source, depth - 0x4000, &id);
    drop_string(&id);
    return (unsigned char *)copy;
}

static int place_message(unsigned char *source, const char *name, int slot, const char *text,
                         const float rgba[4], unsigned colour)
{
    unsigned char *parent = *(unsigned char **)(source + 0x30), *copy, *entry, *field, *movie;
    float width, height, x, y;
    int depth;
    if (!parent) return 0;
    copy = child(parent, name, NULL, as_sprite);
    if (!copy) {
        if (!text[0]) return 1;
        copy = copy_clip(parent, source, name);
        if (!copy) return 0;
    }
    depth = *(int *)(copy + 0x38);
    entry = (unsigned char *)display_entry(parent, depth);
    if (depth < 0x4200 || *(void **)(copy + 0x30) != parent || !entry ||
        *(void **)(entry + 0x48) != copy || *(void **)(source + 0x28) != *(void **)(copy + 0x28)) return 0;
    if (!text[0]) { set_visible(copy, 0, 0); return 1; }
    movie = *(unsigned char **)(copy + 0x20);
    if (!movie) return 0;
    width = *(float *)(movie + 0x1bc);
    height = *(float *)(movie + 0x1c0);
    if (!(width > 0 && height > 0)) return 0;
    x = width * (slot == 0 ? 0.25f : slot == 1 ? 0.75f : 0.5f);
    y = height * (slot == 2 ? 0.85f : 0.1f);
    /* The stock message may be fully faded at the instant the clip is copied. */
    for (int i = 0; i < 4; i++) {
        ((float *)(entry + 0x20))[i] = 1;
        ((float *)(entry + 0x30))[i] = 0;
    }
    *(float *)(entry + 0x18) = *(float *)(copy + 0x74) = x;
    *(float *)(entry + 0x1c) = *(float *)(copy + 0x78) = y;
    play_frame(copy, frame_label(copy, "rollOn"));
    field = child(copy, "message", "txtField", as_text);
    if (!field) { set_visible(copy, 0, 0); return 0; }
    set_text(field, text);
    for (int i = 0; i < 4; i++) field[colour + i] = (unsigned char)(rgba[i] * 255.0f + 0.5f);
    set_visible(copy, 1, 0);
    return 1;
}

int sh_script_hud_text(void *player, const char *name, int slot, const char *text, const float rgba[4])
{
    static hud_layout layout;
    static int have_layout;
    unsigned char *manager, *screen, *widget, *source;
    if (!ready || !player || !name || !text || slot < 0 || slot > 2) return 0;
    if (!have_layout && !(have_layout = read_layout(&layout))) return 0;
    __try {
        manager = *(unsigned char **)((unsigned char *)player + layout.view + layout.render + layout.manager);
        screen = manager ? *(unsigned char **)(manager + layout.screen) : NULL;
        widget = screen ? *(unsigned char **)(screen + layout.widget) : NULL;
        source = widget && resolve_widget(widget) ? *(unsigned char **)(widget + 8) : NULL;
        return source && place_message(source, name, slot, text, rgba, layout.colour);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 0;
    }
}
