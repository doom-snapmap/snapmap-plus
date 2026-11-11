/* Invoke the production console handlers with controlled interface results. */
#include <assert.h>
#include "../src/backend/snapstack.c"

/* The interface's owned text (snapmap_plus_iface.c, not linked here: this test defines the
 * registry calls itself). */
char *sh_iface_textf(const char *fmt, ...)
{
    va_list ap;
    int n;
    char *out;
    va_start(ap, fmt);
    n = _vscprintf(fmt, ap);
    va_end(ap);
    out = n < 0 ? NULL : (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)n + 1);
    if (out) { va_start(ap, fmt); vsnprintf(out, (size_t)n + 1, fmt, ap); va_end(ap); }
    return out;
}
void sh_iface_text_free(void *text) { if (text) HeapFree(GetProcessHeap(), 0, text); }

static int pair_calls[6], rebuild_calls[6], reads[6];
static int toasts, applies, apply_result = 1;
static unsigned long generation = 7;
static char message[300];
static int pair(sh_iface *self, int id, const char *cls, const char *inh)
{
    (void)self;
    assert(!strcmp(cls, "class") && !strcmp(inh, "inherit"));
    assert(id >= 0 && id < 6);
    int call = pair_calls[id]++;
    if (id == 1) return 0;
    if (id == 2) return -2;
    if (id == 3 && call == 1) return 0;
    if (id == 4 && call == 1) return -2;
    return 1;
}
static char *source(sh_iface *self, int id)
{
    (void)self;
    assert(id != 1 && id != 2 && pair_calls[id] == 1);
    reads[id]++;
    return sh_iface_textf("source");
}
static void rebuild(sh_iface *self, int id, const char *text)
{
    (void)self;
    assert(reads[id] == 1 && pair_calls[id] == 1 && !strcmp(text, "source"));
    rebuild_calls[id]++;
}
static void toast(sh_iface *self, const char *title, const char *text)
{
    (void)self;
    assert(!strcmp(title, "SnapStack"));
    strcpy_s(message, sizeof message, text);
    toasts++;
}
static int valid(sh_iface *self, int id) { (void)self; return id >= 0; }
static char *serialize(sh_iface *self, int id)
{ (void)self; return sh_iface_textf("{\"id\":%d}", id); }
static int apply_sync(sh_iface *self, const sh_apply_item *items, int n, const char *op)
{ (void)self; (void)items; (void)op; applies++; return apply_result < 0 ? apply_result : (apply_result ? n : 0); }

int main(void)
{
    sh_iface_vtbl table = {0};
    sh_iface iface = {0};
    int ids[] = {0, 1, 2, 3, 4, 5};
    const char *args[] = {"bsincls", "0", "inherit", "class"};
    table.apply_class_inherit = pair;
    table.get_declsource_copy = source;
    table.rebuild_set_declsource = rebuild;
    table.toast = toast;
    table.is_valid_id = valid;
    table.serialize_entity = serialize;
    table.apply_sync = apply_sync;
    iface.vtbl = &table;
    sh_snapstack_push_ids_backend(0, ids, 6);
    h_bsincls(&iface, 4, args);
    assert(strstr(message, "2 changed, 1 refused, 3 need the command again (of 6 entities)"));
    assert(pair_calls[0] == 2 && pair_calls[5] == 2);
    assert(pair_calls[1] == 1 && pair_calls[2] == 1);
    assert(!reads[1] && !reads[2] && !rebuild_calls[1] && !rebuild_calls[2]);
    assert(rebuild_calls[0] == 1 && rebuild_calls[3] == 1 && rebuild_calls[4] == 1);

    table.apply_class_inherit = NULL;
    sh_snapstack_push_ids_backend(0, ids, 1);
    h_bsincls(&iface, 4, args);
    assert(strstr(message, "0 changed, 1 refused (of 1 entities)"));
    assert(pair_calls[0] == 2 && rebuild_calls[0] == 1);
    table.apply_class_inherit = pair;
    table.rebuild_set_declsource = NULL;
    sh_snapstack_push_ids_backend(0, ids, 1);
    h_bsincls(&iface, 4, args);
    assert(strstr(message, "0 changed, 1 refused (of 1 entities)"));
    assert(pair_calls[0] == 2 && rebuild_calls[0] == 1);

    /* A different map empties every stack before anything reads it. */
    sh_snapstack_push_ids_backend(3, ids, 4);
    assert(g_stacks[3].count == 4);
    generation++;
    {
        const char *chk[] = {"chkstk", "3"};
        h_chkstk(&iface, 2, chk);
        assert(g_stacks[3].count == 0 && strstr(message, "stack 3: 0 id(s)"));
    }

    /* bse and accl need the targets and a receiver: one id is refused and stays on the stack. */
    {
        const char *bse[] = {"bse", "1", "targets"};
        const char *accl[] = {"accl", "1", "targets"};
        sh_snapstack_push_ids_backend(1, ids, 1);
        h_bse(&iface, 3, bse);
        assert(strstr(message, "at least two entities") && g_stacks[1].count == 1);
        h_accl(&iface, 3, accl);
        assert(strstr(message, "at least two entities") && g_stacks[1].count == 1);
        sh_snapstack_clear_stack_backend(1);
    }

    /* Over the bulk limit: refused with the limit named, the stack left as it was. */
    {
        static int many[SS_BULK_MAX + 1];
        const char *bss[] = {"bss", "2", "name", "x"};
        for (int i = 0; i <= SS_BULK_MAX; i++) many[i] = i;
        sh_snapstack_push_ids_backend(2, many, SS_BULK_MAX + 1);
        applies = 0;
        h_bss(&iface, 4, bss);
        assert(strstr(message, "at most 512") && g_stacks[2].count == SS_BULK_MAX + 1 && applies == 0);
        sh_snapstack_clear_stack_backend(2);
    }

    /* accl says "added" only when the editor applied it. */
    {
        const char *accl[] = {"accl", "4", "targets"};
        sh_snapstack_push_ids_backend(4, ids, 3);
        apply_result = 0;
        h_accl(&iface, 3, accl);
        assert(strstr(message, "did not accept") && !strstr(message, "added"));
        sh_snapstack_push_ids_backend(4, ids, 3);
        apply_result = 1;
        h_accl(&iface, 3, accl);
        assert(strstr(message, "added 2 reference(s)"));
    }

    /* pg names a missing group instead of pushing nothing. */
    {
        const char *pg[] = {"pg", "0", "nosuch"};
        h_pg(&iface, 3, pg);
        assert(strstr(message, "no group named 'nosuch'"));
    }
    puts("snapstack_pair_test OK");
    return 0;
}

/* Other console commands are outside this handler test. */
int sh_iface_lookup_cmd(sh_iface *self, const char *name, sh_cmd_handler *handler, void **ctx)
{ (void)self; (void)name; (void)handler; (void)ctx; assert(0); return 0; }
void sh_printf(const char *fmt, ...) { (void)fmt; }
void backend_log(const char *text) { (void)text; }
unsigned long sh_rawmap_load_generation(void) { return generation; }
int sh_json_patch_set_leaf(const char *json, const char *path, const char *token, char *out, int cap)
{ (void)path; (void)token; strcpy_s(out, (size_t)cap, json); return 1; }
int sh_json_patch_upsert_reflist(const char *json, const char *path, const char *const *ids,
                                 int count, char *out, int cap)
{ (void)path; (void)ids; (void)count; strcpy_s(out, (size_t)cap, json); return 1; }
int sh_json_quote_string(const char *raw, char *out, int cap)
{ _snprintf_s(out, (size_t)cap, _TRUNCATE, "\"%s\"", raw); return 1; }
