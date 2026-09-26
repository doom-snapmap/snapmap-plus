/* The bit-flag inspector key adapter on synthetic native state: stock
 * inspectors stay native, relocated decimal choices get their own slot and
 * widget, key sets select choices, and apply writes one decimal (a group code
 * for several keys) or constant names. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "key_inspector.h"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

void backend_log(const char *message) { (void)message; }
int sh_decl_native_schema_enum_constants(sh_decl_native_schema *schema, const char *enum_name,
    int (*visit)(void *context, const char *name, uint32_t value), void *context)
{ (void)schema; (void)enum_name; (void)visit; (void)context; return 0; }
/* The production bindings; the tests bind their own through the seam. */
int sh_spawn_keys_active(void) { return 0; }
void sh_package_runtime_key_decode(uint32_t value, sh_package_key_set *keys, uint32_t *historical)
{ (void)value; memset(keys, 0, sizeof(*keys)); if (historical) *historical = 0; }
int sh_package_runtime_key_code(const sh_package_key_set *keys, uint32_t *code) { (void)keys; *code = 0; return -1; }

/* spawnAIType_t as the fixture enum: IMP bit 2, T3 bit 3, ALL every bit. */
typedef struct constant { const char *name; uint32_t value; uint32_t pad; } constant;
static constant g_constants[] = {{"IMP", 4, 0}, {"T3", 8, 0}, {"ALL", 0xffffffffu, 0}, {NULL, 0, 0}};
static struct { const char *name; uint64_t flags; constant *members; } g_spawn = {"spawnAIType_t", 0x80000, g_constants},
    g_other = {"other_t", 0x80000, g_constants};

static char g_property[64], g_written[64], g_names[64];
static int g_writes, g_native_builds, g_native_applies, g_keys = 1, g_code_status = 1, g_dtors, g_ctors, g_code_calls;
static uint32_t g_historical;
static sh_package_key_groups g_groups;

typedef struct widget { const char *label; int label_id, description_id; uint8_t pad[0xd0 - 16]; uint8_t checked; } widget;
static widget g_widgets[32];
static int g_widget_count;

static void str_ctor(void *str) { g_ctors++; *(const char **)((uint8_t *)str + 0x10) = ""; }
static void str_dtor(void *str) { (void)str; g_dtors++; }
static char read_property(void *object, void *path, void *str, char clear)
{ (void)object; (void)path; (void)clear; *(const char **)((uint8_t *)str + 0x10) = g_property; return 1; }
static char write_property(void *object, void *path, const char *text, void *types)
{ (void)object; (void)path; (void)types; strcpy_s(g_written, sizeof(g_written), text); g_writes++; return 1; }
static void *types(void) { return (void *)0x1234; }
static void *lookup(void *t, void *enum_type, const char *name, char create)
{
    constant *c = ((constant **)enum_type)[2];
    (void)t; (void)create;
    for (; c->name; c++) if (!strcmp(c->name, name)) return c;
    return NULL;
}
/* Names only: other tokens, numbers included, set nothing. */
static uint32_t parse(void *t, void *enum_type, const char *text, uint32_t fallback)
{
    char copy[64], *next = NULL;
    uint32_t value = fallback;
    strcpy_s(copy, sizeof(copy), text);
    for (char *token = strtok_s(copy, " ", &next); token; token = strtok_s(NULL, " ", &next)) {
        constant *c = lookup(t, enum_type, token, 0);
        if (c) value |= c->value;
    }
    return value;
}
/* As the stock writer spells a bit-flag value: a composite's own name for a
 * value equal to it, else the single-bit names it overlaps. */
static void text(void *t, void *enum_type, uint32_t value, void *str)
{
    (void)t; g_names[0] = 0;
    for (constant *c = ((constant **)enum_type)[2]; c->name; c++)
        if (c->value & (c->value - 1) && c->value == value) {
            strcpy_s(g_names, sizeof(g_names), c->name);
            *(const char **)((uint8_t *)str + 0x10) = g_names; return;
        }
    for (constant *c = ((constant **)enum_type)[2]; c->name; c++)
        if (c->value && !(c->value & (c->value - 1)) && (c->value & value)) {
            if (g_names[0]) strcat_s(g_names, sizeof(g_names), " ");
            strcat_s(g_names, sizeof(g_names), c->name);
        }
    *(const char **)((uint8_t *)str + 0x10) = g_names;
}
static void resize(void *list, int count)
{
    uint8_t **slots = (uint8_t **)list;
    *slots = realloc(*slots, (size_t)count * 16); *(int *)((uint8_t *)list + 0xc) = count;
}
static void grow(void *list) { resize(list, *(int *)((uint8_t *)list + 0xc) + 4); }
static const char *label(const int *id) { static char labels[8][16]; snprintf(labels[*id & 7], 16, "label-%d", *id); return labels[*id & 7]; }
static void *create(void *parent, const char *text_label, const int *label_id, const int *description_id,
    char enabled, char checked)
{
    widget *w = &g_widgets[g_widget_count++];
    (void)parent; (void)enabled;
    w->label = text_label; w->label_id = *label_id; w->description_id = *description_id; w->checked = (uint8_t)checked;
    return w;
}
static void native_build(uint8_t *model, void *parent, void *defaults) { (void)model; (void)parent; (void)defaults; g_native_builds++; }
static void native_apply(uint8_t *model) { (void)model; g_native_applies++; }
static int keys_active(void) { return g_keys; }
static void decode(uint32_t value, sh_package_key_set *keys, uint32_t *historical)
{ sh_package_key_decode(&g_groups, value, keys); if (historical) *historical = g_historical; }
static int code(const sh_package_key_set *keys, uint32_t *out)
{
    g_code_calls++;
    if (g_code_status != 1) { *out = 0; return g_code_status; }
    *out = sh_package_key_groups_code(&g_groups, NULL, NULL, keys); return *out ? 1 : 0;
}

/* One model: data as build/apply address it, a declaration of choices. */
typedef struct fixture {
    uint8_t model[0x48], decl[0xa8], values[8 * 0x38], defaults[0x38];
    void *object_holder, *object;
    const char *choices[8];
    int count;
} fixture;
static void setup(fixture *f, void *enum_type, const char *const *choices, int count, const char *property)
{
    memset(f, 0, sizeof(*f));
    f->object_holder = &f->object;
    *(void **)(f->model + 0x00) = &f->object_holder;
    *(void **)(f->model + 0x08) = f->decl;
    *(void **)(f->model + 0x18) = enum_type;
    *(void **)(f->decl + 0x98) = f->values; *(int *)(f->decl + 0xa0) = count;
    for (int i = 0; i < count; i++) {
        f->choices[i] = choices[i];
        *(const char **)(f->values + i * 0x38 + 0x10) = choices[i];
        *(int *)(f->values + i * 0x38 + 0x30) = i + 1;
        *(int *)(f->values + i * 0x38 + 0x34) = 0;   /* description from the defaults */
    }
    *(int *)(f->defaults + 0x34) = 7;
    strcpy_s(g_property, sizeof(g_property), property);
    g_widget_count = 0; g_writes = 0; g_written[0] = 0; g_native_builds = g_native_applies = 0;
    f->count = count;
}
static uint8_t *slot(fixture *f, int i) { return *(uint8_t **)(f->model + 0x20) + i * 16; }
static int slots(fixture *f) { return *(int *)(f->model + 0x28); }
static widget *slot_widget(fixture *f, int i) { return *(widget **)slot(f, i); }
static uint32_t slot_value(fixture *f, int i) { return *(uint32_t *)(slot(f, i) + 8); }
static void release(fixture *f) { free(*(void **)(f->model + 0x20)); }

static void bind_native(void)
{
    sh_key_inspector_native n = {str_ctor, str_dtor, read_property, write_property, types, parse, lookup, text,
        resize, grow, label, create, native_build, native_apply, keys_active, decode, code};
    sh_key_inspector_test_bind(&n);
}

static void test_native_paths(void)
{
    fixture f;
    const char *stock[] = {"IMP", "T3"}, *decimal[] = {"IMP", "524288"};
    setup(&f, &g_spawn, stock, 2, "IMP");
    sh_key_inspector_test_build(f.model, NULL, f.defaults);
    CHECK(g_native_builds == 1 && !g_widget_count && g_ctors == g_dtors);
    setup(&f, &g_other, decimal, 2, "524288");
    sh_key_inspector_test_build(f.model, NULL, f.defaults);
    CHECK(g_native_builds == 1 && !g_widget_count);
    /* A stock value and stock choices, composites included, keep the native writer. */
    setup(&f, &g_spawn, stock, 2, "ALL");
    *(uint32_t *)(f.model + 0x38) = 0xffffffffu;
    sh_key_inspector_test_apply(f.model);
    CHECK(g_native_applies == 1 && !g_writes);
}

/* A decimal choice before a named one: each widget belongs to its slot. */
static void test_decimal_choices(void)
{
    fixture f;
    const char *choices[] = {"524288", "IMP", "T3"};
    setup(&f, &g_spawn, choices, 3, "524288 ignored");
    sh_key_inspector_test_build(f.model, NULL, f.defaults);
    CHECK(!g_native_builds && slots(&f) == 3 && g_widget_count == 3 && g_ctors == g_dtors);
    CHECK(*(uint32_t *)(f.model + 0x38) == 524288u && *(uint32_t *)(f.model + 0x3c) == (524288u | 12u));
    for (int i = 0; i < 3; i++) {
        CHECK(slot_widget(&f, i) == &g_widgets[i] && g_widgets[i].label_id == i + 1 && g_widgets[i].description_id == 7);
        CHECK(!strcmp(g_widgets[i].label, i == 0 ? "label-1" : i == 1 ? "label-2" : "label-3"));
    }
    CHECK(slot_value(&f, 0) == 524288u && slot_value(&f, 1) == 4u && slot_value(&f, 2) == 8u);
    CHECK(g_widgets[0].checked && !g_widgets[1].checked && !g_widgets[2].checked);
    release(&f);
    /* A decimal choice over a value spelled in names is still offered. */
    const char *named_value[] = {"IMP", "524288"};
    setup(&f, &g_spawn, named_value, 2, "IMP");
    sh_key_inspector_test_build(f.model, NULL, f.defaults);
    CHECK(!g_native_builds && slots(&f) == 2 && slot_value(&f, 1) == 524288u);
    CHECK(g_widgets[0].checked && !g_widgets[1].checked);
    release(&f);
    /* A choice the enum does not name ahead of others: every later widget
     * still belongs to its own slot, unlike the stock build's list index. */
    const char *skipped[] = {"NOPE", "IMP", "524288"};
    setup(&f, &g_spawn, skipped, 3, "524292");
    sh_key_inspector_test_build(f.model, NULL, f.defaults);
    CHECK(slots(&f) == 2 && slot_value(&f, 0) == 4u && slot_value(&f, 1) == 524288u);
    CHECK(slot_widget(&f, 0) == &g_widgets[0] && g_widgets[0].label_id == 2 &&
        slot_widget(&f, 1) == &g_widgets[1] && g_widgets[1].label_id == 3);
    release(&f);
    /* Named choices over a numeric value read it as the engine does. */
    const char *stock[] = {"IMP", "T3", "NOPE"};
    setup(&f, &g_spawn, stock, 3, "12");
    sh_key_inspector_test_build(f.model, NULL, f.defaults);
    CHECK(!g_native_builds && slots(&f) == 2 && g_widgets[0].checked && g_widgets[1].checked);
    release(&f);
}

/* Keys select by set: a group code checks each member choice, and apply
 * writes the code the serving generation gives the chosen set. */
static void test_key_sets(void)
{
    fixture f;
    uint32_t a = sh_package_key_private(20), b = sh_package_key_private(21), c = sh_package_key_private(22), group;
    sh_package_key_set pair = {0};
    char av[16], bv[16], cv[16], value[32];
    memset(&g_groups, 0, sizeof(g_groups));
    sh_package_key_set_add(&pair, a); sh_package_key_set_add(&pair, b);
    group = sh_package_key_groups_code(&g_groups, NULL, NULL, &pair);
    CHECK(group && group != a && group != b && (group & SH_PACKAGE_KEY_MARKER));
    snprintf(av, sizeof(av), "%d", (int)a); snprintf(bv, sizeof(bv), "%d", (int)b); snprintf(cv, sizeof(cv), "%d", (int)c);
    const char *choices[] = {av, bv, cv, "IMP"};
    snprintf(value, sizeof(value), "%d", (int)(group | 4u));
    setup(&f, &g_spawn, choices, 4, value);
    sh_key_inspector_test_build(f.model, NULL, f.defaults);
    CHECK(g_widgets[0].checked && g_widgets[1].checked && !g_widgets[2].checked && g_widgets[3].checked);
    /* Numerically the group shares bits with c; as keys it does not hold it. */
    CHECK((group & c & SH_PACKAGE_KEY_PRIVATE) != 0);
    /* Unchanged: nothing is encoded or written. */
    g_code_calls = 0;
    sh_key_inspector_test_apply(f.model);
    CHECK(!g_writes && !g_native_applies && !g_code_calls);
    /* Drop b: the remaining key is its own code. */
    g_widgets[1].checked = 0;
    sh_key_inspector_test_apply(f.model);
    snprintf(value, sizeof(value), "%d", (int)(a | 4u));
    CHECK(g_writes == 1 && !strcmp(g_written, value));
    /* a and c: a new group code, defined in the serving table. */
    g_widgets[2].checked = 1;
    sh_key_inspector_test_apply(f.model);
    {
        sh_package_key_set expected = {0}, held;
        uint32_t written = (uint32_t)atoi(g_written);
        sh_package_key_set_add(&expected, a); sh_package_key_set_add(&expected, c);
        sh_package_key_decode(&g_groups, written, &held);
        CHECK(g_writes == 2 && (written & 4u) && written != (a | c | 4u) && sh_package_key_set_equal(&held, &expected));
    }
    /* No code left for a set: the value stays. */
    g_code_status = 0; g_widgets[1].checked = 1;
    sh_key_inspector_test_apply(f.model);
    CHECK(g_writes == 2);
    g_code_status = -1; sh_key_inspector_test_apply(f.model); CHECK(g_writes == 2);
    g_code_status = 1;
    /* Only stock choices left: constant names. */
    g_widgets[0].checked = g_widgets[1].checked = g_widgets[2].checked = 0; g_widgets[3].checked = 1;
    sh_key_inspector_test_apply(f.model);
    CHECK(g_writes == 3 && !strcmp(g_written, "IMP"));
    release(&f);
}

/* A checkbox whose bits another checked choice still covers changes
 * nothing: no code is asked for and nothing is written. */
static void test_overlap(void)
{
    fixture f;
    uint32_t a = sh_package_key_private(20);
    char av[16], value[16];
    memset(&g_groups, 0, sizeof(g_groups));
    snprintf(av, sizeof(av), "%d", (int)a); snprintf(value, sizeof(value), "%d", (int)(a | 12u));
    const char *choices[] = {av, "IMP", "12"};
    setup(&f, &g_spawn, choices, 3, value);
    sh_key_inspector_test_build(f.model, NULL, f.defaults);
    CHECK(g_widgets[0].checked && g_widgets[1].checked && g_widgets[2].checked);
    g_widgets[1].checked = 0; g_code_calls = 0;
    sh_key_inspector_test_apply(f.model);
    CHECK(!g_writes && !g_code_calls);
    release(&f);
}

/* Plain private bits: without the contract, and when the generation keeps
 * them historically, choices and values combine bitwise. */
static void test_plain_bits(void)
{
    fixture f;
    const char *choices[] = {"524288", "1048576", "IMP"};
    for (int round = 0; round < 2; round++) {
        g_keys = round == 0 ? 0 : 1; g_historical = round == 0 ? 0 : 524288u | 1048576u;
        memset(&g_groups, 0, sizeof(g_groups));
        setup(&f, &g_spawn, choices, 3, "524292");
        sh_key_inspector_test_build(f.model, NULL, f.defaults);
        CHECK(g_widgets[0].checked && !g_widgets[1].checked && g_widgets[2].checked);
        g_widgets[1].checked = 1;
        sh_key_inspector_test_apply(f.model);
        CHECK(g_writes == 1 && !strcmp(g_written, "1572868"));
        g_widgets[0].checked = g_widgets[1].checked = 0;
        sh_key_inspector_test_apply(f.model);
        CHECK(g_writes == 2 && !strcmp(g_written, "IMP"));
        release(&f);
    }
    /* Historical bits and keys cannot share one value. */
    g_keys = 1; g_historical = 524288u;
    uint32_t key = sh_package_key_private(20);
    char kv[16]; snprintf(kv, sizeof(kv), "%d", (int)key);
    const char *mixed[] = {"524288", kv};
    setup(&f, &g_spawn, mixed, 2, "524288");
    sh_key_inspector_test_build(f.model, NULL, f.defaults);
    g_widgets[1].checked = 1;
    sh_key_inspector_test_apply(f.model);
    CHECK(!g_writes);
    release(&f);
    g_historical = 0;
}

/* A composite value (the stock filter default) intersects package keys too,
 * as the filter does: every choice it covers is checked. Applying without a
 * change keeps it; an edit spells it out as its named bits beside the keys. */
static void test_composite(void)
{
    fixture f;
    uint32_t a = sh_package_key_private(20), b = sh_package_key_private(21);
    char av[16], bv[16], value[16];
    memset(&g_groups, 0, sizeof(g_groups));
    snprintf(av, sizeof(av), "%d", (int)a); snprintf(bv, sizeof(bv), "%d", (int)b);
    const char *choices[] = {"IMP", "T3", av, bv};
    setup(&f, &g_spawn, choices, 4, "ALL");
    sh_key_inspector_test_build(f.model, NULL, f.defaults);
    CHECK(*(uint32_t *)(f.model + 0x38) == 0xffffffffu);
    CHECK(g_widgets[0].checked && g_widgets[1].checked && g_widgets[2].checked && g_widgets[3].checked);
    g_code_calls = 0;
    sh_key_inspector_test_apply(f.model);
    CHECK(!g_writes && !g_native_applies && !g_code_calls);
    /* Drop T3: IMP and both keys, one group code and no composite bits. */
    g_widgets[1].checked = 0;
    sh_key_inspector_test_apply(f.model);
    {
        sh_package_key_set expected = {0}, held;
        uint32_t written = (uint32_t)atoi(g_written);
        sh_package_key_set_add(&expected, a); sh_package_key_set_add(&expected, b);
        sh_package_key_decode(&g_groups, written, &held);
        CHECK(g_writes == 1 && sh_package_key_set_equal(&held, &expected) &&
            written == (sh_package_key_groups_code(&g_groups, NULL, NULL, &expected) | 4u));
    }
    /* Only b: its own code beside IMP. */
    g_widgets[2].checked = 0;
    sh_key_inspector_test_apply(f.model);
    snprintf(value, sizeof(value), "%d", (int)(b | 4u));
    CHECK(g_writes == 2 && !strcmp(g_written, value));
    /* No key left: the stock writer's names. */
    g_widgets[3].checked = 0;
    sh_key_inspector_test_apply(f.model);
    CHECK(g_writes == 3 && !strcmp(g_written, "IMP"));
    release(&f);
}

/* Bytes from a pattern: relocations take arbitrary values. */
static size_t materialize(const char *pattern, uint8_t *code, uint8_t relocation)
{
    size_t n = 0;
    for (const char *p = pattern; *p; p += 3) {
        code[n++] = p[0] == '?' ? relocation : (uint8_t)strtoul(p, NULL, 16);
        if (!p[2]) break;
    }
    return n;
}
static void test_verify(void)
{
    uint8_t build[600] = {0}, apply[300] = {0};
    size_t build_length = materialize(sh_key_inspector_test_code(0), build, 0x5a);
    size_t apply_length = materialize(sh_key_inspector_test_code(1), apply, 0xa5);
    CHECK(build_length == 516 && apply_length == 226);
    CHECK(sh_key_inspector_test_verify(build, apply));
    materialize(sh_key_inspector_test_code(0), build, 0x00);
    CHECK(sh_key_inspector_test_verify(build, NULL));
    /* The slot index the stock build stores widgets by, and its checked byte. */
    build[0x1ba] ^= 1; CHECK(!sh_key_inspector_test_verify(build, NULL)); build[0x1ba] ^= 1;
    apply[0x55] ^= 1; CHECK(!sh_key_inspector_test_verify(NULL, apply)); apply[0x55] ^= 1;
    build[build_length - 1] = 0xcc; CHECK(!sh_key_inspector_test_verify(build, NULL));
    CHECK(sh_key_inspector_test_verify(NULL, NULL));
}

int main(void)
{
    bind_native();
    test_native_paths();
    test_decimal_choices();
    test_key_sets();
    test_overlap();
    test_plain_bits();
    test_composite();
    test_verify();
    if (failures) { fprintf(stderr, "key inspector tests: %d failure(s)\n", failures); return 1; }
    puts("key inspector tests passed");
    return 0;
}
