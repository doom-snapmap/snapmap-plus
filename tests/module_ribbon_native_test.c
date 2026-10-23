/* Publication changes must reach an already-open picker without freeing rows. */
#define SH_MODULE_RIBBON_TESTING
#include <assert.h>
#include "../src/backend/module_ribbon_native.c"

static unsigned char editor_bytes[MR_GUI + 16], mode_bytes[0x80];
static unsigned char map_bytes[0x760], gui_bytes[MR_RIBBON + 16];
static unsigned char heap_bytes[0xc8], stock[MR_RECORD * 2];
static unsigned char variant_a[MR_RECORD], variant_b[MR_RECORD], instance[MR_RECORD];
static unsigned char *grown;
static const char *published_names[4];
static size_t published_count;
static long served_serial = 1;
static int queries, ticks, pushes, pops, rebuilds, currents, scrolls, items;
static int allow_growth = 1, ignore_push, throw_rebuild, sequence;

void backend_log(const char *message) { assert(message); }
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{ (void)base; (void)name; (void)status; return 0; }
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; return NULL; }
sh_patch_status hook_commit(void *trampoline) { (void)trampoline; return B2_PATCH_OK; }
int hook_is_installed(void *trampoline) { (void)trampoline; return 0; }
int hook_unpatch(void *trampoline) { (void)trampoline; return 1; }

void *sh_grid_native_variant_record(const char *name)
{
    if (!strcmp(name, "a")) return variant_a;
    if (!strcmp(name, "b")) return variant_b;
    return NULL;
}
int sh_grid_native_variant_wrapper(const void *wrapper)
{ return wrapper == (void *)1 || wrapper == (void *)2; }
long sh_package_runtime_served_serial(void) { return served_serial; }
int sh_package_runtime_module_variants(char ***out, size_t *count)
{
    size_t i;
    queries++;
    *count = published_count;
    *out = published_count ? calloc(published_count, sizeof(char *)) : NULL;
    assert(!published_count || *out);
    for (i = 0; i < published_count; i++) {
        (*out)[i] = _strdup(published_names[i]);
        assert((*out)[i]);
    }
    return 1;
}
void sh_package_runtime_module_variants_free(char **names, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) free(names[i]);
    free(names);
}
static void *heap_get(void) { return heap_bytes; }
static void heap_push(void *self, int id)
{
    int *depth = (int *)((unsigned char *)self + 0xc4);
    pushes++;
    if (ignore_push) return;
    *(int *)((unsigned char *)self + 0x44 + *depth * 4) = id;
    (*depth)++;
}
static void heap_pop(void *self)
{ pops++; (*(int *)((unsigned char *)self + 0xc4))--; }
static void original_tick(void *mode, void *input, void *editor)
{ (void)mode; (void)input; (void)editor; ticks++; }
static void rebuild(void *mode, void *editor, int sort)
{
    assert(mode == mode_bytes && editor == editor_bytes && sort == 3);
    assert(*(int *)(heap_bytes + 0xc4) == 1);
    assert(sequence == 0); sequence = 1; rebuilds++;
    if (throw_rebuild) RaiseException(0xe0420101, 0, 0, NULL);
}
static void set_current(void *palette, void *record)
{
    assert(palette == editor_bytes + MR_PALETTE && record == instance);
    assert(sequence == 1); sequence = 2; currents++;
}
static void scroll(void *holder)
{ assert(holder == (void *)3 && sequence == 2); sequence = 3; scrolls++; }
static void refresh_items(void *holder)
{ assert(holder == (void *)3 && sequence == 3); sequence = 4; items++; }
static unsigned char resize(void *storage, int size)
{
    mr_list *list = storage;
    assert(list->memory == 2 && !list->records);
    if (!allow_growth) return 0;
    assert(!grown);
    grown = calloc((size_t)size, MR_RECORD); assert(grown);
    list->records = grown; list->capacity = size;
    return 1;
}
static mr_list *rows(void) { return (mr_list *)(editor_bytes + MR_PALETTE + 0x10); }
static void tick(void)
{
    sequence = 0;
    sh_module_ribbon_test_tick(mode_bytes, NULL, editor_bytes);
    assert(*(int *)(heap_bytes + 0xc4) == 0);
}
static void publish(const char *a, const char *b, const char *c)
{
    published_count = 0;
    if (a) published_names[published_count++] = a;
    if (b) published_names[published_count++] = b;
    if (c) published_names[published_count++] = c;
    served_serial++;
}
int main(void)
{
    sh_process_heap_api heap = {heap_get, heap_push, heap_pop};
    unsigned char stock_before[sizeof(stock)];
    mr_list *list = rows();
    *(void **)stock = (void *)10; *(void **)(stock + MR_RECORD) = (void *)11;
    stock[32] = 0x71; stock[MR_RECORD + 32] = 0x72;
    memcpy(stock_before, stock, sizeof(stock));
    *(void **)variant_a = (void *)1; variant_a[32] = 0x41;
    *(void **)variant_b = (void *)2; variant_b[32] = 0x42;
    list->records = stock; list->count = list->capacity = 2; list->memory = 2;
    *(void **)(editor_bytes + MR_MAP) = map_bytes;
    *(void **)(map_bytes + 0x750) = instance; *(int *)(map_bytes + 0x758) = 1;
    *(void **)(editor_bytes + MR_GUI) = gui_bytes;
    *(void **)(gui_bytes + MR_RIBBON) = (void *)3;
    *(int *)(mode_bytes + 0x60) = 3;
    sh_module_ribbon_test_bind(original_tick, rebuild, set_current, scroll,
        refresh_items, resize, editor_bytes, &heap);

    /* Duplicate names produce one variant row; old readers retain their array. */
    publish("a", "a", "missing"); tick();
    assert(queries == 1 && ticks == 1 && rebuilds == 1 && sequence == 4);
    assert(list->count == 3 && list->records == grown && g_rows_shown == 1 && g_rows_refused == 1);
    assert(sh_module_ribbon_test_retained() == 1 && g_retained[0] == stock);
    assert(!memcmp(stock, stock_before, sizeof(stock)));
    assert(!memcmp(list->records, stock_before, sizeof(stock)));
    assert(!memcmp(list->records + 2 * MR_RECORD, variant_a, MR_RECORD));
    tick(); assert(queries == 1 && rebuilds == 1 && ticks == 2);

    /* A changed served library replaces an open picker's rows on its next tick. */
    publish("b", NULL, NULL); tick();
    assert(queries == 2 && rebuilds == 2 && list->count == 3);
    assert(!memcmp(list->records + 2 * MR_RECORD, variant_b, MR_RECORD));
    publish(NULL, NULL, NULL); tick();
    assert(queries == 3 && rebuilds == 3 && list->count == 2);
    assert(!memcmp(list->records, stock_before, sizeof(stock)));
    assert(currents == 3 && scrolls == 3 && items == 3 && pushes == pops);

    /* Unavailable capacity refuses only additional variants, keeping stock rows. */
    list->capacity = 2; allow_growth = 0; publish("a", NULL, NULL); tick();
    assert(list->count == 2 && g_rows_shown == 0 && g_rows_refused == 1);
    assert(!memcmp(list->records, stock_before, sizeof(stock)));

    /* A silent wrong-thread heap push postpones refresh without an unsafe pop. */
    ignore_push = 1; publish("b", NULL, NULL); tick();
    assert(rebuilds == 4 && pushes == pops + 1 && g_shown != g_published);
    ignore_push = 0; tick();
    assert(rebuilds == 5 && g_shown == g_published);

    /* Native failure still unwinds the heap and disables only future refreshes. */
    throw_rebuild = 1; publish("a", NULL, NULL); tick();
    assert(g_faulted && rebuilds == 6 && pushes == pops + 1);
    tick(); assert(rebuilds == 6 && ticks == 9);
    assert(!memcmp(stock, stock_before, sizeof(stock)));
    sh_package_runtime_module_variants_free(g_names, g_name_count);
    free(grown);
    puts("module_ribbon_native_test: PASS");
    return 0;
}
