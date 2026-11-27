#include "editor_sheets.h"
#include <limits.h>
#include "editor_panel_schema.h"

typedef struct entry {
    char *id, *qualified, *anchor;
    sh_dt_node *node;
    int parent, original, assigned, after, keep_empty;
    unsigned edits;
} entry;

typedef struct entries { entry *items; size_t count, capacity; } entries;
typedef struct layout {
    sh_dt *tree;
    entries sheets, fields;
    sh_json_object prefs;
    int changed, read_prefs;
} layout;

static char *(*read_preferences)(void);
void sh_editor_sheets_set_preferences(char *(*read)(void)) { read_preferences = read; }

static int add(entries *list, char *id, sh_dt_node *node, int parent)
{
    entry *e;
    if (!id || !node) { free(id); return -1; }
    if (list->count == list->capacity) {
        size_t cap = list->capacity ? list->capacity * 2 : 16;
        entry *next = (entry *)realloc(list->items, cap * sizeof *next);
        if (!next) { free(id); return -1; }
        list->items = next;
        list->capacity = cap;
    }
    e = &list->items[list->count];
    memset(e, 0, sizeof *e);
    e->id = id;
    e->node = node;
    e->parent = e->original = parent;
    return (int)list->count++;
}

static void clear(entries *list)
{
    size_t i;
    for (i = 0; i < list->count; ++i) {
        free(list->items[i].id);
        free(list->items[i].qualified);
        free(list->items[i].anchor);
    }
    free(list->items);
}

static char *native_string(sh_dt_node *node, const char *key)
{
    sh_dt_node *s = sh_dt_child(node, key);
    if (!s || s->block || !s->val_n) return NULL;
    if (s->val_n >= 2 && s->val[0] == '"' && s->val[s->val_n - 1] == '"')
        return sh_textf("%.*s", (int)(s->val_n - 2), s->val + 1);
    return sh_textf("%.*s", (int)s->val_n, s->val);
}

static char *unique_id(const entries *list, size_t count, char *stem, int qualified)
{
    char *id = stem;
    size_t i, ordinal = 1;
    if (!id) return NULL;
    for (i = 0; i < count; ++i) {
        const char *used = qualified ? list->items[i].qualified : list->items[i].id;
        if (!used || strcmp(used, id)) continue;
        if (id != stem) free(id);
        id = sh_textf("%s@%zu", stem, ++ordinal);
        if (!id) break;
    }
    if (id != stem) free(stem);
    return id;
}

static int read_base(layout *l, sh_dt_node *pages)
{
    int i, j;
    if (pages && (!pages->list || pages->num_delta)) return 0;
    for (i = 0; pages && i < pages->nkids; ++i) {
        sh_dt_node *page = pages->kids[i], *props;
        size_t first = l->fields.count, k;
        int index = (int)l->sheets.count;
        char *id;
        if (!page->block) continue;
        props = sh_dt_child(page, "properties");
        if (props && (!props->list || props->num_delta)) return 0;
        for (j = 0; props && j < props->nkids; ++j) {
            sh_dt_node *prop = props->kids[j];
            char *path;
            if (!prop->block) continue;
            path = native_string(prop, "path");
            if (!path) path = sh_textf("@%zu", l->fields.count - first);
            if (add(&l->fields, path, prop, index) < 0) return -1;
        }
        id = first < l->fields.count ? sh_textf("native/%s", l->fields.items[first].id) : sh_textf("native/@%d", index);
        id = unique_id(&l->sheets, l->sheets.count, id, 0);
        if (add(&l->sheets, id, page, -1) < 0) return -1;
        l->sheets.items[index].keep_empty = first == l->fields.count;
        for (k = first; k < l->fields.count; ++k) {
            entry *field = &l->fields.items[k];
            field->qualified = unique_id(&l->fields, k, sh_textf("%s::%s", id, field->id), 1);
            if (!field->qualified) return -1;
        }
    }
    return 1;
}

static char *page_id(const char *salt, const char *id)
{
    if (!strncmp(id, "native/", 7) || !strncmp(id, "shared/", 7) || !strncmp(id, "mods/", 5))
        return _strdup(id);
    return sh_textf("mods/%s/%s", salt, id);
}

/* Prefer the original page when a binding occurs in several native sheets. */
static int locate(const entries *list, const char *id, int preferred)
{
    int result = -1, closest = -1, n = 0, m = 0;
    size_t i;
    for (i = 0; i < list->count; ++i) {
        const entry *e = &list->items[i];
        if (e->qualified && !strcmp(e->qualified, id)) return (int)i;
        if (strcmp(e->id, id)) continue;
        result = (int)i;
        ++n;
        if (e->original == preferred) { closest = (int)i; ++m; }
    }
    return m == 1 ? closest : n == 1 ? result : n ? -2 : -1;
}

char *sh_editor_sheet_ids(sh_dt *tree)
{
    layout l = {0};
    sh_text_buf out = {0};
    size_t i, j = 0;
    sh_dt_node *base = sh_dt_at(sh_dt_root(tree), "edit/propertySheets");
    if (read_base(&l, base) != 1) goto done;
    sh_text_append_str(&out, "[");
    for (i = 0; i < l.sheets.count; ++i) {
        const char *id = l.sheets.items[i].id;
        size_t start = j;
        sh_text_append_str(&out, i ? ",{\"id\":" : "{\"id\":");
        sh_json_append_quoted(&out, id, strlen(id));
        sh_text_append_str(&out, ",\"properties\":[");
        for (; j < l.fields.count && l.fields.items[j].parent == (int)i; ++j) {
            const entry *f = &l.fields.items[j];
            const char *property = locate(&l.fields, f->id, -1) == (int)j ? f->id : f->qualified;
            if (j != start) sh_text_append_str(&out, ",");
            sh_json_append_quoted(&out, property, strlen(property));
        }
        sh_text_append_str(&out, "]}");
    }
    sh_text_append_str(&out, "]");
done:
    clear(&l.sheets);
    clear(&l.fields);
    if (out.oom) { free(out.p); return NULL; }
    return out.p;
}

static int quoted(sh_dt *tree, sh_dt_node *n, const char *member, const char *value)
{
    char *token = sh_textf("\"%s\"", value);
    int ok = token && sh_dt_assign_scalar(tree, n, member, token);
    free(token);
    return ok;
}

static int configure(layout *l, entry *e, const sh_json_object *obj, const char *salt, int page)
{
    static const char *const keys[] = {"title", "description", "inspector"};
    size_t i;
    for (i = 0; i < (page ? 2u : 3u); ++i) {
        const char *raw = sh_json_object_get(obj, keys[i]);
        const char *native = i == 0 ? (page ? "titleTag" : "displayNameTag") : i == 1 ? "descriptionTag" : "inspector";
        char *key, *value;
        int ok;
        if (!raw || (e->edits & (1u << i))) continue;
        key = ep_string(raw);
        if (!key) return 0;
        value = i == 2 ? _strdup(key) : ep_tag(salt, key);
        ok = value && quoted(l->tree, e->node, native, value);
        free(value);
        free(key);
        if (!ok) return 0;
        e->edits |= 1u << i;
        l->changed = 1;
    }
    if (!e->anchor) {
        const char *raw = sh_json_object_get(obj, "before");
        char *ref;
        if (!raw) { raw = sh_json_object_get(obj, "after"); e->after = raw != NULL; }
        if (!raw) return 1;
        ref = ep_string(raw);
        if (!ref) return 0;
        e->anchor = page ? page_id(salt, ref) : _strdup(ref);
        free(ref);
        if (!e->anchor) return 0;
        l->changed = 1;
    }
    return 1;
}

static int apply_field(layout *l, const char *salt, int page, const char *path, const char *raw)
{
    sh_json_object obj = {0};
    int index = locate(&l->fields, path, page), ok = 0;
    entry *field;
    if (!ep_object(raw, &obj)) goto done;
    if (index < 0) {
        sh_dt_node *node;
        if (index == -2 || strstr(path, "::") || !sh_json_object_get(&obj, "inspector")) { ok = 1; goto done; }
        node = sh_dt_member(l->tree, "item[0]", 1);
        if (!node || !quoted(l->tree, node, "path", path)) goto done;
        index = add(&l->fields, _strdup(path), node, page);
        if (index < 0) goto done;
        l->fields.items[index].assigned = 1;
        l->changed = 1;
    }
    field = &l->fields.items[index];
    if (!field->assigned && field->original != page) {
        l->changed |= field->parent != page;
        field->parent = page;
        field->assigned = 1;
    }
    ok = configure(l, field, &obj, salt, 0);
done:
    sh_json_object_free(&obj);
    return ok;
}

static int matches(layout *l, const sh_editor_sheet_source *src, const char *condition)
{
    sh_json_object cond = {0}, defs = {0}, setting = {0};
    const char *value;
    char *key = NULL;
    int ok = -1;
    if (!condition) return 1;
    if (!ep_object(condition, &cond) || cond.count != 1 || !ep_object(src->settings, &defs) ||
        !ep_object(sh_json_object_get(&defs, cond.members[0].key), &setting)) goto done;
    if (!l->read_prefs) {
        char *raw = read_preferences ? read_preferences() : NULL;
        if (raw && !ep_object(raw, &l->prefs)) { free(raw); goto done; }
        free(raw);
        l->read_prefs = 1;
    }
    key = sh_textf("%s/%s", src->salt, cond.members[0].key);
    if (!key) goto done;
    value = sh_json_object_get(&l->prefs, key);
    if (!value) value = sh_json_object_get(&setting, "default");
    ok = !strcmp(value ? value : "false", cond.members[0].value_json);
done:
    free(key);
    sh_json_object_free(&cond);
    sh_json_object_free(&defs);
    sh_json_object_free(&setting);
    return ok;
}

uint64_t sh_editor_sheets_preferences_key(const sh_editor_sheet_source *sources, size_t count)
{
    layout l = {0};
    uint64_t key = 0;
    size_t i, j;
    for (i = 0; i < count; ++i) {
        sh_json_object map = {0};
        if (!ep_object(sources[i].json, &map)) continue;
        for (j = 0; j < map.count; ++j) {
            sh_json_object page = {0};
            const char *condition;
            if (ep_object(map.members[j].value_json, &page) && (condition = sh_json_object_get(&page, "when"))) {
                int state = matches(&l, &sources[i], condition);
                key = (key ^ (uint64_t)(state + 2)) * UINT64_C(1099511628211);
            }
            sh_json_object_free(&page);
        }
        sh_json_object_free(&map);
    }
    sh_json_object_free(&l.prefs);
    return key;
}

static int apply_page(layout *l, const sh_editor_sheet_source *src, const sh_json_member *m)
{
    sh_json_object page = {0}, props = {0};
    const char *salt = src->salt;
    char *id = page_id(salt, m->key);
    const char *raw;
    size_t i;
    int index, ok = 0;
    if (!id || !ep_object(m->value_json, &page)) goto done;
    index = matches(l, src, sh_json_object_get(&page, "when"));
    if (index < 1) { ok = index == 0; goto done; }
    index = locate(&l->sheets, id, -1);
    if (index < 0) {
        if (index == -2 || !strncmp(id, "native/", 7)) { ok = 1; goto done; }
        index = add(&l->sheets, _strdup(id), sh_dt_member(l->tree, "item[0]", 1), -1);
        if (index < 0) goto done;
        l->changed = 1;
    }
    if (!configure(l, &l->sheets.items[index], &page, salt, 1)) goto done;
    raw = sh_json_object_get(&page, "properties");
    if (raw && !ep_object(raw, &props)) goto done;
    for (i = 0; i < props.count; ++i)
        if (!apply_field(l, salt, index, props.members[i].key, props.members[i].value_json)) goto done;
    ok = 1;
done:
    free(id);
    sh_json_object_free(&page);
    sh_json_object_free(&props);
    return ok;
}

/* Attach each move to its anchor. Drop links that would make a cycle. */
typedef struct order { int anchor, state, next, before, after, last_before, last_after; } order;
typedef struct frame { int at, child, stage; } frame;

static int emit(const entries *es, int group, sh_dt *tree, sh_dt_node *out)
{
    size_t i, n = es->count, depth = 0;
    order *ord = (order *)calloc(n ? n : 1, sizeof *ord);
    frame *stack = (frame *)calloc(n ? n : 1, sizeof *stack);
    int ok = 0;
    if (!ord || !stack) goto done;
    for (i = 0; i < n; ++i) {
        int a = -1;
        const entry *e = &es->items[i];
        if (e->parent == group && e->anchor) a = locate(es, e->anchor, group);
        ord[i].anchor = a >= 0 && es->items[a].parent == group && a != (int)i ? a : -1;
        ord[i].next = ord[i].before = ord[i].after = ord[i].last_before = ord[i].last_after = -1;
    }
    for (i = 0; i < n; ++i) {
        int at = (int)i;
        while (at >= 0 && !ord[at].state) { ord[at].state = 1; at = ord[at].anchor; }
        if (at >= 0 && ord[at].state == 1) ord[at].anchor = -1;
        at = (int)i;
        while (at >= 0 && ord[at].state == 1) { ord[at].state = 2; at = ord[at].anchor; }
    }
    for (i = 0; i < n; ++i) {
        int a = ord[i].anchor, *head, *tail;
        if (a < 0) continue;
        head = es->items[i].after ? &ord[a].after : &ord[a].before;
        tail = es->items[i].after ? &ord[a].last_after : &ord[a].last_before;
        if (*tail < 0) *head = (int)i;
        else ord[*tail].next = (int)i;
        *tail = (int)i;
    }
    out->nkids = 0;
    out->list = 1;
    out->num_delta = 0;
    for (i = 0; i < n; ++i) {
        if (es->items[i].parent != group || ord[i].anchor >= 0) continue;
        stack[depth++] = (frame){(int)i, ord[i].before, 0};
        while (depth) {
            frame *f = &stack[depth - 1];
            if (f->child >= 0) {
                int child = f->child;
                f->child = ord[child].next;
                stack[depth++] = (frame){child, ord[child].before, 0};
            } else if (f->stage == 0) {
                if (!sh_dt_append(tree, out, es->items[f->at].node)) goto done;
                f->child = ord[f->at].after;
                f->stage = 1;
            } else --depth;
        }
    }
    ok = 1;
done:
    free(ord);
    free(stack);
    return ok;
}

/* A derived native decl can override just one inspector in an inherited item[N]. */
static int list_length(sh_dt_node *list, int fallback)
{
    sh_dt_node *n = sh_dt_child(list, "num");
    int value = 0;
    size_t i;
    if (!n) return fallback;
    if (n->block || !n->val_n) return -1;
    for (i = 0; i < n->val_n; ++i) {
        if (n->val[i] < '0' || n->val[i] > '9' || value > (INT_MAX - 9) / 10) return -1;
        value = 10 * value + n->val[i] - '0';
    }
    return value;
}

static int materialize(sh_dt *t, sh_dt_node *out, sh_dt_node *local, sh_dt_node *parent, int page_list)
{
    int count = list_length(local, list_length(parent, 0)), i;
    if (count < 0 || count > (local ? local->nkids : 0) + (parent ? parent->nkids : 0)) return 0;
    if (!out) return 0;
    {
        char n[32];
        snprintf(n, sizeof n, "%d", count);
        if (!sh_dt_assign_scalar(t, out, "num", n)) return 0;
    }
    out->list = 1;
    for (i = 0; i < count; ++i) {
        char key[32];
        sh_dt_node *base, *own, *dst;
        int pass, j;
        snprintf(key, sizeof key, "item[%d]", i);
        base = sh_dt_child(parent, key);
        own = sh_dt_child(local, key);
        dst = sh_dt_member(t, key, 1);
        if (!dst || !sh_dt_append(t, out, dst)) return 0;
        for (pass = 0; pass < 2; ++pass) {
            sh_dt_node *src = pass ? own : base;
            for (j = 0; src && j < src->nkids; ++j) {
                sh_dt_node *field = src->kids[j];
                char *name = sh_textf("%.*s", (int)field->key_n, field->key);
                int skip;
                if (!name) return 0;
                skip = (!pass && sh_dt_child(own, name)) || (page_list && !_stricmp(name, "properties"));
                free(name);
                if (!skip && !sh_dt_copy_child(t, dst, field)) return 0;
            }
        }
        if (page_list) {
            sh_dt_node *bp = sh_dt_child(base, "properties"), *op = sh_dt_child(own, "properties");
            if ((bp || op) && !materialize(t, sh_dt_ensure_block(t, dst, "properties"), op, bp, 0)) return 0;
        }
    }
    return 1;
}

int sh_editor_sheets_inherit(sh_dt *tree, const sh_dt *parent)
{
    sh_dt_node *src = sh_dt_at(sh_dt_root((sh_dt *)parent), "edit/propertySheets"), *edit, *own, *merged;
    int i;
    if (!src) return 0;
    edit = sh_dt_ensure_block(tree, sh_dt_root(tree), "edit");
    if (!edit) return -1;
    own = sh_dt_child(edit, "propertySheets");
    if (!own) return sh_dt_copy_child(tree, edit, src) ? 1 : -1;
    merged = sh_dt_member(tree, "propertySheets", 1);
    if (!merged || !materialize(tree, merged, own, src, 1)) return -1;
    for (i = 0; i < edit->nkids; ++i) if (edit->kids[i] == own) {
        edit->kids[i] = merged;
        return 1;
    }
    return -1;
}

int sh_editor_sheets_apply(sh_dt *tree, const sh_editor_sheet_source *sources, size_t count)
{
    layout l = {0};
    sh_dt_node *edit = sh_dt_child(sh_dt_root(tree), "edit"), *pages = sh_dt_child(edit, "propertySheets");
    sh_json_object map = {0};
    int result;
    size_t i, j;
    if (!count) return 0;
    l.tree = tree;
    result = read_base(&l, pages);
    if (result <= 0) goto done;
    result = -1;
    for (i = 0; i < count; ++i) {
        if (!sh_editor_sheets_validate(sources[i].json, NULL, 0) || !ep_object(sources[i].json, &map)) goto done;
        for (j = 0; j < map.count; ++j)
            if (!apply_page(&l, &sources[i], &map.members[j])) goto done;
        sh_json_object_free(&map);
    }
    if (!l.changed) { result = 0; goto done; }
    if (!edit) edit = sh_dt_ensure_block(tree, sh_dt_root(tree), "edit");
    if (!pages) pages = sh_dt_ensure_block(tree, edit, "propertySheets");
    if (!pages) goto done;
    for (i = 0; i < l.sheets.count; ++i) {
        sh_dt_node *props = sh_dt_ensure_block(tree, l.sheets.items[i].node, "properties");
        if (!props || !emit(&l.fields, (int)i, tree, props)) goto done;
        if (!props->nkids && !l.sheets.items[i].keep_empty) l.sheets.items[i].parent = -2;
    }
    if (!emit(&l.sheets, -1, tree, pages)) goto done;
    result = 1;
done:
    sh_json_object_free(&map);
    sh_json_object_free(&l.prefs);
    clear(&l.sheets);
    clear(&l.fields);
    return result;
}
