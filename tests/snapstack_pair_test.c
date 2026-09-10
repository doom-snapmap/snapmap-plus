/* Invoke the production console handler with controlled interface results. */
#include <assert.h>
#include "../src/backend/snapstack.c"

static int pair_calls[6], rebuild_calls[6], reads[6];
static char message[200];
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
static const char *source(sh_iface *self, int id, char *out, int cap)
{
    (void)self;
    assert(id != 1 && id != 2 && pair_calls[id] == 1 && cap > 8);
    reads[id]++;
    strcpy_s(out, (size_t)cap, "source");
    return out;
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
}
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
    iface.vtbl = &table;
    sh_snapstack_push_ids_backend(0, ids, 6);
    h_bsincls(&iface, 4, args);
    assert(!strcmp(message, "bsincls: 2 completed, 1 refused, 3 partial (of 6 entities)"));
    assert(pair_calls[0] == 2 && pair_calls[5] == 2);
    assert(pair_calls[1] == 1 && pair_calls[2] == 1);
    assert(!reads[1] && !reads[2] && !rebuild_calls[1] && !rebuild_calls[2]);
    assert(rebuild_calls[0] == 1 && rebuild_calls[3] == 1 && rebuild_calls[4] == 1);

    table.apply_class_inherit = NULL;
    sh_snapstack_push_ids_backend(0, ids, 1);
    h_bsincls(&iface, 4, args);
    assert(!strcmp(message, "bsincls: 0 completed, 1 refused, 0 partial (of 1 entities)"));
    assert(pair_calls[0] == 2 && rebuild_calls[0] == 1);
    table.apply_class_inherit = pair;
    table.rebuild_set_declsource = NULL;
    sh_snapstack_push_ids_backend(0, ids, 1);
    h_bsincls(&iface, 4, args);
    assert(!strcmp(message, "bsincls: 0 completed, 1 refused, 0 partial (of 1 entities)"));
    assert(pair_calls[0] == 2 && rebuild_calls[0] == 1);
    puts("snapstack_pair_test OK");
    return 0;
}

/* Other console commands are outside this handler test. */
int sh_iface_lookup_cmd(sh_iface *self, const char *name, sh_cmd_handler *handler, void **ctx)
{ (void)self; (void)name; (void)handler; (void)ctx; assert(0); return 0; }
void sh_printf(const char *fmt, ...) { (void)fmt; assert(0); }
void backend_log(const char *text) { (void)text; assert(0); }
int sh_json_patch_set_leaf(const char *json, const char *path, const char *token, char *out, int cap)
{ (void)json; (void)path; (void)token; (void)out; (void)cap; assert(0); return 0; }
int sh_json_patch_upsert_reflist(const char *json, const char *path, const char *const *ids,
                                 int count, char *out, int cap)
{ (void)json; (void)path; (void)ids; (void)count; (void)out; (void)cap; assert(0); return 0; }
int sh_json_quote_string(const char *raw, char *out, int cap)
{ (void)raw; (void)out; (void)cap; assert(0); return 0; }
