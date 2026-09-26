/* key_inspector.c -- SnapMap's bit-flag property inspector over package keys.
 *
 * Model<idSnapBitFlagInspector> (Vulkan build 0x1405c25c0 / apply
 * 0x1405cf600, OpenGL 0x1405c1cb0 / 0x1405cecf0; the same code apart from
 * relocations) builds one checkbox per values[].enumValue that names an enum
 * constant, reads the property as constant names and writes the single-bit
 * constants of the edited value. Compiler relocation spells a package's
 * choices over its private keys as one decimal number, which the stock build
 * skips; it also stores each widget at the choice's list index while appending
 * slots compactly, so one skipped choice misaligns every later widget and
 * leaves a slot without its own.
 *
 * For spawnAIType_t inspectors with a decimal choice or a numeric value this
 * builds instead: a decimal choice is its number, a numeric value reads as the
 * engine reads it (the first token through atoi), each widget belongs to its
 * own slot, and a choice is checked when it shares a named bit or a key with
 * the value. Apply keeps what no choice controls, sets the chosen named bits
 * and keys, and writes one decimal when the result holds keys (a set of several
 * keys takes the serving generation's group code) or constant names otherwise.
 * Private bits stay plain bits, as their readers treat them, while the key
 * contract is absent, in a composite (all, all unique) and where the serving
 * generation keeps them historically. A composite value (the stock AI filter's
 * default, all unique) matches by intersection, package keys included, so it
 * checks every choice it covers; an edit spells it out as its named bits, as
 * the stock writer does, and an apply that changes no checkbox keeps it. Every
 * other inspector, and every spawn-type inspector without either spelling,
 * stays native. */
#include "key_inspector.h"
#include "backend_log.h"
#include "hook.h"
#include "package_runtime.h"
#include "spawn_keys.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Model data (the object + 8), as both routines address it. */
#define KI_OBJECT 0x00
#define KI_DECL 0x08
#define KI_PATH 0x10
#define KI_ENUM 0x18
#define KI_SLOTS 0x20
#define KI_NUM 0x28
#define KI_CAP 0x2c
#define KI_CURRENT 0x38
#define KI_MASK 0x3c
/* Inspector declaration: values[] of {idStr enumValue, int label, int description}. */
#define KI_VALUES 0x98
#define KI_COUNT 0xa0
#define KI_VALUE_STRIDE 0x38
#define KI_VALUE_NAME 0x10
#define KI_VALUE_LABEL 0x30
#define KI_VALUE_DESCRIPTION 0x34
/* Slots {widget, value}; a checkbox widget's state byte. */
#define KI_SLOT_STRIDE 0x10
#define KI_SLOT_VALUE 0x08
#define KI_CHECKED 0xd0
/* idStr: 0x30 bytes, text pointer at +0x10. */
#define KI_STR_SIZE 0x40
#define KI_STR_TEXT 0x10
#define KI_TYPES_METHOD 0x240

typedef void (*ki_build_fn)(uint8_t *model, void *parent, void *defaults);
typedef void (*ki_apply_fn)(uint8_t *model);
static struct {
    void (*str_ctor)(void *);
    void (*str_dtor)(void *);
    char (*read)(void *, void *, void *, char);
    char (*write)(void *, void *, const char *, void *);
    void **types_owner;
    void *(*types)(void);
    uint32_t (*parse)(void *, void *, const char *, uint32_t);
    void *(*lookup)(void *, void *, const char *, char);
    void (*text)(void *, void *, uint32_t, void *);
    void (*resize)(void *, int);
    void (*grow)(void *);
    const char *(*label)(const int *);
    void *(*create)(void *, const char *, const int *, const int *, char, char);
    int (*keys_active)(void);
    void (*decode)(uint32_t, sh_package_key_set *, uint32_t *);
    int (*code)(const sh_package_key_set *, uint32_t *);
} g;
static ki_build_fn g_build;
static ki_apply_fn g_apply;
static void *g_build_hook, *g_apply_hook;
static volatile LONG g_active;

static void *ki_types(void)
{
    if (g.types) return g.types();
    void *owner = *g.types_owner;
    return ((void *(*)(void *))(*(void ***)owner)[KI_TYPES_METHOD / 8])(owner);
}
static int ki_decimal(const char *text) { return text && ((*text >= '0' && *text <= '9') || *text == '-'); }

/* A spawnAIType_t inspector that offers a decimal choice (or 2 when none). */
static int ki_spawn_choices(const uint8_t *m)
{
    int decimal = 0;
    __try {
        const char *name = *(const char **)*(void **)(m + KI_ENUM);
        const uint8_t *decl = *(const uint8_t **)(m + KI_DECL);
        int count = *(const int *)(decl + KI_COUNT);
        const uint8_t *values = *(const uint8_t **)(decl + KI_VALUES);
        if (!name || strcmp(name, SH_PACKAGE_KEY_TYPE)) return 0;
        for (int i = 0; i < count && !decimal; i++)
            decimal = ki_decimal(*(const char **)(values + (size_t)i * KI_VALUE_STRIDE + KI_VALUE_NAME));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return decimal ? 1 : 2;
}

/* A value's plain bits and the keys its private bits name. Private bits stay
 * plain while the contract is absent, in a composite (all, all unique) and
 * when the serving generation keeps them historically; readers treat them so. */
typedef struct ki_parts { uint32_t bits; sh_package_key_set keys; } ki_parts;
static int ki_keyed_value(uint32_t value)
{ return (value & SH_PACKAGE_KEY_PRIVATE) && !(value & SH_PACKAGE_KEY_COMPOSITE); }
static void ki_split(uint32_t value, ki_parts *out)
{
    uint32_t historical = 0;
    memset(out, 0, sizeof(*out));
    out->bits = value;
    if (!ki_keyed_value(value) || !g.keys_active()) return;
    g.decode(value, &out->keys, &historical);
    if (!(value & SH_PACKAGE_KEY_PRIVATE & ~historical)) { memset(&out->keys, 0, sizeof(out->keys)); return; }
    out->bits = value & ~SH_PACKAGE_KEY_PRIVATE;
}
static int ki_selected(uint32_t value, uint32_t choice)
{
    ki_parts held, offered;
    if (value & SH_PACKAGE_KEY_COMPOSITE) return (value & choice) != 0;
    ki_split(value, &held); ki_split(choice, &offered);
    return (held.bits & offered.bits) != 0 || sh_package_key_set_meets(&held.keys, &offered.keys);
}

static uint8_t *ki_append(uint8_t *m)
{
    if (*(int *)(m + KI_NUM) == *(int *)(m + KI_CAP)) g.grow(m + KI_SLOTS);
    int num = *(int *)(m + KI_NUM), cap = *(int *)(m + KI_CAP);
    uint8_t *slots = *(uint8_t **)(m + KI_SLOTS);
    if (num < cap) { *(int *)(m + KI_NUM) = num + 1; return slots + (size_t)num * KI_SLOT_STRIDE; }
    return slots + (size_t)(cap - 1) * KI_SLOT_STRIDE;
}

static void ki_build(uint8_t *m, void *parent, void *defaults)
{
    unsigned char str[KI_STR_SIZE];
    int kind = ki_spawn_choices(m);
    if (!kind) { g_build(m, parent, defaults); return; }
    memset(str, 0, sizeof(str));
    g.str_ctor(str);
    g.read(**(void ***)(m + KI_OBJECT), *(void **)(m + KI_PATH), str, 1);
    const char *text = *(const char **)(str + KI_STR_TEXT);
    if (kind == 2 && !ki_decimal(text)) { g.str_dtor(str); g_build(m, parent, defaults); return; }
    void *types = ki_types(), *enum_type = *(void **)(m + KI_ENUM);
    uint32_t current = ki_decimal(text) ? (uint32_t)atoi(text) : g.parse(types, enum_type, text, 0);
    uint8_t *decl = *(uint8_t **)(m + KI_DECL);
    int count = *(int *)(decl + KI_COUNT);
    *(uint32_t *)(m + KI_CURRENT) = current;
    if (count > *(int *)(m + KI_CAP)) g.resize(m + KI_SLOTS, count);
    for (int i = 0; i < count; i++) {
        uint8_t *v = *(uint8_t **)(decl + KI_VALUES) + (size_t)i * KI_VALUE_STRIDE;
        const char *name = *(const char **)(v + KI_VALUE_NAME);
        const uint8_t *label_from = *(int *)(v + KI_VALUE_LABEL) ? v : (const uint8_t *)defaults;
        const uint8_t *description_from = *(int *)(v + KI_VALUE_DESCRIPTION) ? v : (const uint8_t *)defaults;
        uint32_t value;
        int ids[3];
        if (ki_decimal(name)) value = (uint32_t)atoi(name);
        else {
            void *constant = g.lookup(types, enum_type, name, 0);
            if (!constant) continue;
            value = *(uint32_t *)((uint8_t *)constant + 8);
        }
        uint8_t *slot = ki_append(m);
        *(uint32_t *)(slot + KI_SLOT_VALUE) = value; *(uint32_t *)(m + KI_MASK) |= value;
        ids[0] = ids[2] = *(const int *)(label_from + KI_VALUE_LABEL);
        ids[1] = *(const int *)(description_from + KI_VALUE_DESCRIPTION);
        *(void **)slot = g.create(parent, g.label(ids), ids + 2, ids + 1, 1, (char)ki_selected(current, value));
    }
    g.str_dtor(str);
}

/* A spawn-type inspector whose value or choices hold keys (a stock-only
 * value, composites included, keeps the native writer). */
static int ki_keyed(const uint8_t *m)
{
    int keyed = 0;
    __try {
        const char *name = *(const char **)*(void **)(m + KI_ENUM);
        if (!name || strcmp(name, SH_PACKAGE_KEY_TYPE)) return 0;
        keyed = ki_keyed_value(*(const uint32_t *)(m + KI_CURRENT));
        const uint8_t *slots = *(const uint8_t **)(m + KI_SLOTS);
        for (int i = 0; i < *(const int *)(m + KI_NUM) && !keyed; i++)
            keyed = ki_keyed_value(*(const uint32_t *)(slots + (size_t)i * KI_SLOT_STRIDE + KI_SLOT_VALUE));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return keyed;
}

/* The bits the stock writer spells for a value: its single-bit constant
 * names. It spells a value equal to a composite by that composite's name, so
 * it reads the value without the unnamed bits (VK FUN_141a1e290). */
static uint32_t ki_named(void *types, void *enum_type, uint32_t value)
{
    unsigned char str[KI_STR_SIZE];
    uint32_t named;
    memset(str, 0, sizeof(str));
    g.str_ctor(str);
    g.text(types, enum_type, value & ~SH_PACKAGE_KEY_UNNAMED, str);
    named = g.parse(types, enum_type, *(const char **)(str + KI_STR_TEXT), 0);
    g.str_dtor(str);
    return named;
}

static void ki_apply(uint8_t *m)
{
    if (!ki_keyed(m)) { g_apply(m); return; }
    uint32_t current = *(uint32_t *)(m + KI_CURRENT), mask = 0, chosen = 0, bits, code = 0;
    sh_package_key_set controlled, picked, keys;
    ki_parts held, one;
    uint8_t *slots = *(uint8_t **)(m + KI_SLOTS);
    int num = *(int *)(m + KI_NUM), changed = 0;
    void *types = ki_types();
    for (int i = 0; i < num && !changed; i++) {
        uint8_t *slot = slots + (size_t)i * KI_SLOT_STRIDE;
        changed = (*(*(uint8_t **)slot + KI_CHECKED) != 0) != ki_selected(current, *(uint32_t *)(slot + KI_SLOT_VALUE));
    }
    if (!changed) return;
    ki_split(current, &held);
    if (current & SH_PACKAGE_KEY_COMPOSITE) {
        held.bits = ki_named(types, *(void **)(m + KI_ENUM), current);
        memset(&held.keys, 0, sizeof(held.keys));
    }
    memset(&controlled, 0, sizeof(controlled)); memset(&picked, 0, sizeof(picked));
    for (int i = 0; i < num; i++) {
        uint8_t *slot = slots + (size_t)i * KI_SLOT_STRIDE;
        int checked = *(*(uint8_t **)slot + KI_CHECKED) != 0;
        ki_split(*(uint32_t *)(slot + KI_SLOT_VALUE), &one);
        mask |= one.bits; if (checked) chosen |= one.bits;
        for (size_t w = 0; w < sizeof(one.keys.words) / sizeof(one.keys.words[0]); w++) {
            controlled.words[w] |= one.keys.words[w];
            if (checked) picked.words[w] |= one.keys.words[w];
        }
    }
    bits = (held.bits & ~mask) | chosen;
    for (size_t w = 0; w < sizeof(keys.words) / sizeof(keys.words[0]); w++)
        keys.words[w] = (held.keys.words[w] & ~controlled.words[w]) | picked.words[w];
    if (bits == held.bits && sh_package_key_set_equal(&keys, &held.keys)) return;
    if (sh_package_key_set_count(&keys)) {
        /* Plain private bits and keys cannot share one value. */
        int status = bits & SH_PACKAGE_KEY_PRIVATE ? 0 : g.code(&keys, &code);
        if (status != 1) {
            backend_log(status < 0 ? "KEY-INSPECTOR: the serving key codes could not be updated; the value is unchanged" :
                "KEY-INSPECTOR: no key code holds this choice set; the value is unchanged");
            return;
        }
    }
    uint32_t value = bits | code;
    if (value == current) return;
    unsigned char str[KI_STR_SIZE];
    char number[16];
    const char *text = number;
    memset(str, 0, sizeof(str));
    g.str_ctor(str);
    if (value & SH_PACKAGE_KEY_UNNAMED) snprintf(number, sizeof(number), "%d", (int)value);
    else { g.text(types, *(void **)(m + KI_ENUM), value, str); text = *(const char **)(str + KI_STR_TEXT); }
    g.write(**(void ***)(m + KI_OBJECT), *(void **)(m + KI_PATH), text, types);
    g.str_dtor(str);
}

/* Build and apply as verified on both supported builds; ?? marks relocated
 * call targets and globals. */
static const char ki_build_code[] =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 90 00 00 00 48 C7 44 24 50 "
    "FE FF FF FF 48 89 9C 24 E0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 "
    "84 24 88 00 00 00 4C 89 44 24 40 48 89 54 24 48 4C 8B E1 48 8D 4C 24 58 "
    "E8 ?? ?? ?? ?? 90 49 8B 04 24 41 B1 01 4C 8D 44 24 58 49 8B 54 24 10 48 "
    "8B 08 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 40 02 00 00 4C "
    "8B E8 45 33 C9 4C 8B 44 24 68 49 8B 54 24 18 48 8B C8 E8 ?? ?? ?? ?? 41 "
    "89 44 24 38 49 8B 4C 24 08 48 63 99 A0 00 00 00 41 3B 5C 24 2C 7E 0C 8B "
    "D3 49 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B EB 85 DB 0F 8E 11 01 00 00 45 33 "
    "FF 45 8B F7 0F 1F 40 00 0F 1F 84 00 00 00 00 00 49 8B 44 24 08 48 8B 98 "
    "98 00 00 00 49 03 DE 45 33 C9 4C 8B 43 10 49 8B 54 24 18 49 8B CD E8 ?? "
    "?? ?? ?? 48 8B F8 48 85 C0 0F 84 BE 00 00 00 41 8B 4C 24 2C 41 39 4C 24 "
    "28 75 0A 49 8D 4C 24 20 E8 ?? ?? ?? ?? 49 63 44 24 28 41 8B 4C 24 2C 3B "
    "C1 7D 15 48 8B D0 48 C1 E2 04 49 03 54 24 20 FF C0 41 89 44 24 28 EB 0F "
    "8D 41 FF 48 63 D0 48 C1 E2 04 49 03 54 24 20 8B 47 08 89 42 08 41 09 44 "
    "24 3C 48 8B 44 24 40 83 7B 34 00 48 0F 45 C3 8B 48 34 48 8B 44 24 40 83 "
    "7B 30 00 48 0F 45 C3 8B 40 30 89 44 24 30 45 8B 44 24 38 44 85 42 08 0F "
    "95 C3 89 4C 24 34 89 44 24 38 49 8B 7C 24 20 48 8D 4C 24 30 E8 ?? ?? ?? "
    "?? 48 8B D0 88 5C 24 28 C6 44 24 20 01 4C 8D 4C 24 34 4C 8D 44 24 38 48 "
    "8B 4C 24 48 E8 ?? ?? ?? ?? 4A 89 04 3F 49 83 C6 38 49 83 C7 10 48 83 ED "
    "01 0F 85 01 FF FF FF 48 8D 4C 24 58 E8 ?? ?? ?? ?? 48 8B 8C 24 88 00 00 "
    "00 48 33 CC E8 ?? ?? ?? ?? 48 8B 9C 24 E0 00 00 00 48 81 C4 90 00 00 00 "
    "41 5F 41 5E 41 5D 41 5C 5F 5E 5D C3";
static const char ki_apply_code[] =
    "40 57 48 83 EC 60 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 78 48 89 B4 24 "
    "80 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 58 48 8B F1 44 8B "
    "41 38 8B 79 3C F7 D7 41 23 F8 83 79 28 00 7E 29 48 8B 41 20 8B 51 28 66 "
    "0F 1F 84 00 00 00 00 00 48 8B 08 80 B9 D0 00 00 00 00 74 03 0B 78 08 48 "
    "83 C0 10 48 83 EA 01 75 E7 41 3B F8 74 54 48 8B 0D ?? ?? ?? ?? 48 8B 01 "
    "FF 90 40 02 00 00 48 8B D8 48 8D 4C 24 28 E8 ?? ?? ?? ?? 90 4C 8D 4C 24 "
    "28 44 8B C7 48 8B 56 18 48 8B CB E8 ?? ?? ?? ?? 48 8B 0E 4C 8B CB 4C 8B "
    "44 24 38 48 8B 56 10 48 8B 09 E8 ?? ?? ?? ?? 90 48 8D 4C 24 28 E8 ?? ?? "
    "?? ?? 48 8B 4C 24 58 48 33 CC E8 ?? ?? ?? ?? 48 8B 5C 24 78 48 8B B4 24 "
    "80 00 00 00 48 83 C4 60 5F C3";

static int ki_hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int ki_matches(const uint8_t *code, const char *pattern)
{
    size_t at = 0;
    __try {
        for (const char *p = pattern; *p; p += 3, at++) {
            if (p[0] == '?') continue;
            if (code[at] != (uint8_t)(ki_hex(p[0]) << 4 | ki_hex(p[1]))) return 0;
            if (!p[2]) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
static void *ki_call(const uint8_t *site) { int32_t rel; memcpy(&rel, site + 1, 4); return (void *)(site + 5 + rel); }
static void *ki_rip(const uint8_t *site, size_t length) { int32_t rel; memcpy(&rel, site + length - 4, 4); return (void *)(site + length + rel); }
static uint8_t *ki_clean(const sig_result *r, size_t n, const char *name, const uint8_t *base)
{
    for (size_t i = 0; i < n; i++)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK && r[i].addr &&
            r[i].addr == (uintptr_t)base + r[i].rva) return (uint8_t *)r[i].addr;
    return NULL;
}

static void ki_runtime_decode(uint32_t value, sh_package_key_set *keys, uint32_t *historical)
{ sh_package_runtime_key_decode(value, keys, historical); }

int sh_key_inspector_install(const sig_result *r, size_t n, const uint8_t *base)
{
    uint8_t *build = ki_clean(r, n, "BitFlagInspectorBuild", base), *apply = ki_clean(r, n, "BitFlagInspectorApply", base);
    if (InterlockedCompareExchange(&g_active, 0, 0)) return 1;
    if (!build || !apply) {
        backend_log("KEY-INSPECTOR: bit-flag inspector routines not found; package key choices stay native");
        return 0;
    }
    if (!ki_matches(build, ki_build_code) || !ki_matches(apply, ki_apply_code) ||
        ki_call(build + 0x48) != ki_call(apply + 0x86) || ki_call(build + 0x1d4) != ki_call(apply + 0xbd) ||
        ki_rip(build + 0x67, 7) != ki_rip(apply + 0x6e, 7)) {
        backend_log("KEY-INSPECTOR: bit-flag inspector routines differ from the verified builds; package key choices stay native");
        return 0;
    }
    g.str_ctor = (void (*)(void *))ki_call(build + 0x48);
    g.read = (char (*)(void *, void *, void *, char))ki_call(build + 0x62);
    g.types_owner = (void **)ki_rip(build + 0x67, 7);
    g.parse = (uint32_t (*)(void *, void *, const char *, uint32_t))ki_call(build + 0x8a);
    g.resize = (void (*)(void *, int))ki_call(build + 0xae);
    g.lookup = (void *(*)(void *, void *, const char *, char))ki_call(build + 0xee);
    g.grow = (void (*)(void *))ki_call(build + 0x110);
    g.label = (const char *(*)(const int *))ki_call(build + 0x194);
    g.create = (void *(*)(void *, const char *, const int *, const int *, char, char))ki_call(build + 0x1b4);
    g.str_dtor = (void (*)(void *))ki_call(build + 0x1d4);
    g.text = (void (*)(void *, void *, uint32_t, void *))ki_call(apply + 0x9b);
    g.write = (char (*)(void *, void *, const char *, void *))ki_call(apply + 0xb2);
    g.types = NULL;
    g.keys_active = sh_spawn_keys_active; g.decode = ki_runtime_decode; g.code = sh_package_runtime_key_code;
    /* Whole position-independent prologues. */
    g_build_hook = hook_prepare(build, (void *)ki_build, 19);
    g_apply_hook = hook_prepare(apply, (void *)ki_apply, 15);
    g_build = (ki_build_fn)g_build_hook; g_apply = (ki_apply_fn)g_apply_hook;
    if (g_build_hook && g_apply_hook && hook_commit(g_build_hook) == B2_PATCH_OK &&
        hook_commit(g_apply_hook) == B2_PATCH_OK) {
        InterlockedExchange(&g_active, 1);
        backend_log("KEY-INSPECTOR: bit-flag inspector key choices installed (build, apply)");
        return 1;
    }
    if (g_apply_hook && hook_unpatch(g_apply_hook)) g_apply_hook = NULL;
    if (g_build_hook && hook_unpatch(g_build_hook)) g_build_hook = NULL;
    backend_log("KEY-INSPECTOR: bit-flag inspector replacements could not be installed; package key choices stay native");
    return 0;
}

int sh_key_inspector_active(void) { return InterlockedCompareExchange(&g_active, 0, 0) != 0; }

#ifdef SH_KEY_INSPECTOR_TESTING
void sh_key_inspector_test_bind(const sh_key_inspector_native *n)
{
    g.str_ctor = n->str_ctor; g.str_dtor = n->str_dtor; g.read = n->read; g.write = n->write;
    g.types_owner = NULL; g.types = n->types; g.parse = n->parse; g.lookup = n->lookup; g.text = n->text;
    g.resize = n->resize; g.grow = n->grow; g.label = n->label; g.create = n->create;
    g.keys_active = n->keys_active; g.decode = n->decode; g.code = n->code;
    g_build = n->native_build; g_apply = n->native_apply;
}
void sh_key_inspector_test_build(uint8_t *model, void *parent, void *defaults) { ki_build(model, parent, defaults); }
void sh_key_inspector_test_apply(uint8_t *model) { ki_apply(model); }
int sh_key_inspector_test_verify(const uint8_t *build, const uint8_t *apply)
{ return (!build || ki_matches(build, ki_build_code)) && (!apply || ki_matches(apply, ki_apply_code)); }
const char *sh_key_inspector_test_code(int apply) { return apply ? ki_apply_code : ki_build_code; }
#endif
