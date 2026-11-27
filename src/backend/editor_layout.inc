/* Installed editor documents are indexed at refresh. Engine file opens use the selected body's pointer. */
#define MS_TILE_PREFIX "generated/decls/snapeditorentitydef/"

enum { ML_TAB, ML_TITLE, ML_DESCRIPTION, ML_ICON, ML_ORDER, ML_FIELDS };
static const char *const ml_field[ML_FIELDS] = { "tab", "title", "description", "icon", "order" };
static const char *const ml_native[ML_FIELDS] = {
    "categoryNameTag", "displayNameTag", "displayDescriptionTag", "previewImage", "sortPriority"
};

typedef struct ms_layout_part {
    const char *request, *salt, *field[ML_FIELDS], *sheets, *settings;
} ms_layout_part;

/* A scope can switch a parent's build while the child continues to serve the same file. */
typedef struct ml_dependency { int key, file; const ms_body *body; } ml_dependency;
typedef struct ml_ancestors { ml_dependency items[48]; int count; } ml_ancestors;

typedef struct ms_layout_memo {
    int file, scoped;
    uint64_t preferences;
    ml_dependency *deps; int ndeps, capacity;
    const ms_body *base;
    ms_body *result;
    struct ms_layout_memo *next;
} ms_layout_memo;

typedef struct ms_layout_row {
    const char *request, *field[ML_FIELDS];
    sh_editor_sheet_source *sheets;
    size_t nsheets;
    uint64_t preferences;
    ms_layout_memo *memo;
    ms_body *variants;
    int provider, anchor, source;
    ms_body *source_body;
} ms_layout_row;

static int ml_compare(const void *left, const void *right)
{
    const ms_layout_part *a = left, *b = right;
    int c = strcmp(a->request, b->request);
    return c ? c : strcmp(a->salt, b->salt);
}

static char *ml_text(ms_index *x, const sh_json_object *obj, const char *key)
{
    const char *s = sh_json_object_get(obj, key);
    return s ? ms_jstr(x, s, s + strlen(s), MS_ANY_SIZE) : NULL;
}

static const char *ml_token(ms_index *x, const char *salt, int field, const sh_json_object *entry)
{
    if (field == ML_ORDER) {
        const char *number = sh_json_object_get(entry, "order");
        if (!number || !*number) return NULL;
        char *end;
        errno = 0;
        long n = strtol(number, &end, 10);
        if (errno || *end || n < INT_MIN || n > INT_MAX) return NULL;
        return ms_strdup(x, number);
    }
    char *id = ml_text(x, entry, ml_field[field]), *mapped = NULL;
    if (!id) return NULL;
    size_t cap = strlen(salt) + strlen(id) + 96;
    char *tag = ms_alloc(x, cap);
    if (!tag) return NULL;
    if (field == ML_TAB) {
        if (!sh_editor_tab_tag(salt, id, tag, cap)) return NULL;
    } else if (field == ML_ICON) {
        mapped = sh_ma_engine_name(salt, id, strlen(id));
        if (!sh_editor_id_valid(mapped ? mapped : id)) { free(mapped); return NULL; }
        strcpy_s(tag, cap, mapped ? mapped : id);
        free(mapped);
    } else if (!sh_editor_string_tag(salt, id, tag, cap)) return NULL;
    size_t len = strlen(tag);
    char *token = ms_alloc(x, len + 3);
    if (token) { token[0] = '"'; memcpy(token + 1, tag, len); token[len + 1] = '"'; token[len + 2] = 0; }
    return token;
}

static int ml_collect(ms_index *x, const ms_bundle *b, ms_layout_part **parts, int *count, int *cap)
{
    sh_json_object root = {0}, editor = {0}, rows = {0}, entry = {0};
    int ok = 1;
    if (!b->extra || !sh_json_parse_object(b->extra, strlen(b->extra), 32, &root)) goto done;
    const char *doc = sh_json_object_get(&root, "editor");
    if (!doc || !sh_json_parse_object(doc, strlen(doc), 32, &editor)) goto done;
    const char *settings = sh_json_object_get(&editor, "settings");
    if (settings) settings = ms_strdup(x, settings);
    for (int sheets = 0; sheets < 2; sheets++) {
        const char *map = sh_json_object_get(&editor, sheets ? "sheets" : "tiles");
        if (!map || !sh_json_parse_object(map, strlen(map), 32, &rows)) continue;
        for (size_t i = 0; i < rows.count; i++) {
            const sh_json_member *m = &rows.members[i];
            if (!sh_editor_id_valid(m->key) || !sh_json_parse_object(m->value_json, strlen(m->value_json), 32, &entry)) continue;
            /* New tiles already carry their owner's intrinsic data, so any installed patch can override it. */
            if (sheets || !sh_json_object_get(&entry, "entity")) {
                if (!ms_grow((void **)parts, cap, *count + 1, sizeof **parts)) { ok = 0; goto done; }
                ms_layout_part *p = &(*parts)[(*count)++];
                memset(p, 0, sizeof *p);
                char *r = sh_textf(MS_TILE_PREFIX "%s.decl", m->key);
                p->request = r ? ms_strdup(x, r) : NULL;
                free(r);
                if (!p->request) { ok = 0; goto done; }
                ms_lower((char *)p->request);
                p->salt = b->id;
                p->settings = settings;
                if (sheets) p->sheets = ms_strdup(x, m->value_json);
                else for (int j = 0; j < ML_FIELDS; j++) p->field[j] = ml_token(x, b->id, j, &entry);
            }
            sh_json_object_free(&entry);
        }
        sh_json_object_free(&rows);
    }
done:
    sh_json_object_free(&entry);
    sh_json_object_free(&rows);
    sh_json_object_free(&editor);
    sh_json_object_free(&root);
    return ok;
}

typedef struct ml_stock_entry {
    struct ml_stock_entry *next;
    const char *request, *text;
    size_t len;
} ml_stock_entry;

/* The stock base of a row, including tiles supplied by the product itself. */
static const char *ml_stock(ms_index *x, const char *request, size_t *len)
{
    const ov_baked_decl_t *b = ms_baked_tile(request);
    if (b) { *len = b->len; return b->text; }
    if (!strncmp(request, MS_TILE_PREFIX "mods/", sizeof MS_TILE_PREFIX + 3)) return NULL;
    size_t bucket = (size_t)ms_hash_folded(request, NULL) % 127;
    for (ml_stock_entry *e = x->layout_stock[bucket]; e; e = e->next)
        if (!strcmp(request, e->request)) { *len = e->len; return e->text; }
    ml_stock_entry *entry = ms_alloc(x, sizeof *entry);
    if (!entry || !(entry->request = ms_strdup(x, request))) return NULL;
    entry->next = x->layout_stock[bucket];
    x->layout_stock[bucket] = entry;
    entry->len = 0;
    entry->text = NULL;
    char sha[SH_MB_HASH_HEX + 1];
    unsigned long long size;
    unsigned char *bytes = NULL;
    if (sh_mb_stock_identify(request, sha, &size) == SH_MB_STOCK_OK &&
        sh_mb_stock_read(request, sha, &bytes, len, request, NULL) == SH_MB_STOCK_OK) {
        entry->text = ms_strndup(x, (char *)bytes, *len);
        entry->len = *len;
        HeapFree(GetProcessHeap(), 0, bytes);
    }
    return entry->text;
}

static int ml_build(ms_index *x)
{
    ms_layout_part *parts = NULL;
    int n = 0, cap = 0, ok = 0;
    for (int i = 0; i < x->nbundles; i++) {
        const ms_bundle *b = &x->bundles[i];
        if (b->admitted && !b->place && !ml_collect(x, b, &parts, &n, &cap)) goto done;
    }
    if (!n) { ok = 1; goto done; }
    qsort(parts, n, sizeof *parts, ml_compare);
    x->layouts = ms_alloc(x, (size_t)n * sizeof *x->layouts);
    if (!x->layouts) goto done;
    for (int first = 0; first < n;) {
        int last = first + 1;
        while (last < n && !strcmp(parts[last].request, parts[first].request)) last++;
        ms_layout_row *row = &x->layouts[x->nlayouts++];
        memset(row, 0, sizeof *row);
        row->provider = row->anchor = row->source = -1;
        row->request = parts[first].request;
        row->sheets = ms_alloc(x, (size_t)(last-first) * sizeof *row->sheets);
        if (!row->sheets) goto done;
        const char *owner[ML_FIELDS] = {0};
        for (int i = first; i < last; i++) {
            const ms_layout_part *p = &parts[i];
            if (p->sheets) {
                row->sheets[row->nsheets].settings = p->settings;
                row->sheets[row->nsheets].salt = p->salt;
                row->sheets[row->nsheets++].json = p->sheets;
            }
            for (int k = 0; k < ML_FIELDS; k++) {
                if (!p->field[k]) continue;
                if (!row->field[k]) { row->field[k] = p->field[k]; owner[k] = p->salt; }
                else if (strcmp(row->field[k], p->field[k]))
                    ms_log("editor: %s %s uses %s's value; %s supplied a different value", row->request, ml_field[k], owner[k], p->salt);
            }
        }
        if (row->nsheets) row->preferences = sh_editor_sheets_preferences_key(row->sheets, row->nsheets);
        first = last;
    }
    /* Attach each known provider directly to its row. Names absent from the collection never become new tiles. */
    for (int i = 0; i < x->nfiles; i++) {
        ms_file *f = &x->files[i];
        if (!f->request || strncmp(f->request, MS_TILE_PREFIX, sizeof MS_TILE_PREFIX - 1)) continue;
        int lo = 0, hi = x->nlayouts;
        while (lo < hi) {
            int mid = lo + (hi-lo)/2;
            if (strcmp(x->layouts[mid].request, f->request) < 0) lo = mid + 1; else hi = mid;
        }
        if (lo < x->nlayouts && !strcmp(x->layouts[lo].request, f->request)) {
            f->layout = &x->layouts[lo];
            if (f->served || f->registry_keyed) f->layout->provider = i;
        }
    }
    for (int i = 0; i < x->nlayouts; i++) {
        ms_layout_row *r = &x->layouts[i];
        size_t len = 0;
        const char *stock = ml_stock(x, r->request, &len);
        if (stock || r->provider >= 0) x->installed_layouts++;
        if (!stock) continue;
        if (!ms_grow((void **)&x->files, &x->cfiles, x->nfiles + 1, sizeof(ms_file))) goto done;
        ms_file *f = &x->files[x->nfiles];
        memset(f, 0, sizeof *f);
        f->bundle = x->reg_bundle;
        f->layout = r;
        r->anchor = x->nfiles++;
        f->layout_anchor = 1;
        f->registry_keyed = r->provider < 0;
        f->request = r->request;
        f->reg = f->request + 16;
        const char *name = f->request + sizeof MS_TILE_PREFIX - 1;
        f->pname = ms_strndup(x, name, strlen(name) - 5);
        if (!f->pname) goto done;
        f->ptype = "snapEditorEntityDef";
        f->rank = ms_rank(f->ptype);
        f->is_decl = f->is_sedef = f->is_shadow = 1;
        f->body = (const unsigned char *)stock;
        f->body_len = len;
        f->alt_of = f->scope_alt = f->scope_last = -1;
    }
    ok = 1;
done:
    free(parts);
    return ok;
}

static ml_dependency ml_source(const ms_index *x, int key)
{
    const ms_layout_row *row = x->files[key].layout;
    ml_dependency dep = {key};
    dep.file = row ? row->source : ms_scoped(x, key);
    dep.body = row ? row->source_body : dep.file >= 0 ? x->files[dep.file].scope_body : NULL;
    return dep;
}

static sh_dt *ml_native_tree(ms_index *x, const char *name, ml_ancestors *ancestors)
{
    if (!sh_editor_id_valid(name)) return NULL;
    char *request = sh_textf(MS_TILE_PREFIX "%s.decl", name);
    if (!request) return NULL;
    ms_lower(request);
    int file = x->nkeys ? ms_find(x->slots, x->mask, request) : -1;
    size_t len = 0;
    unsigned char *raw = NULL;
    const char *text = NULL;
    if (file >= 0) {
        ml_dependency dep = ml_source(x, file);
        if (ancestors && ancestors->count < 48) ancestors->items[ancestors->count++] = dep;
        if (dep.file >= 0) {
            const ms_file *f = &x->files[dep.file];
            if (dep.body) { text = (const char *)dep.body->p; len = dep.body->len; }
            else if (!g_scope_on) { text = (const char *)f->body; len = f->body_len; }
            if (!text && !f->registry) { raw = ms_file_raw(f, &len); text = (const char *)raw; }
        }
    }
    if (!text) text = ml_stock(x, request, &len);
    free(request);
    sh_dt *tree = text ? sh_dt_parse(text, len, NULL, 0) : NULL;
    if (raw) HeapFree(GetProcessHeap(), 0, raw);
    return tree;
}

static int ml_inherit(ms_index *x, sh_dt *tree, int depth, ml_ancestors *ancestors)
{
    sh_dt_node *node = sh_dt_child(sh_dt_root(tree), "inherit");
    if (!node) return 1;
    if (depth >= 48 || !node->val || node->val_n < 3 || node->val[0] != '"') return 0;
    char *name = sh_textn(node->val + 1, node->val_n - 2);
    sh_dt *parent = name ? ml_native_tree(x, name, ancestors) : NULL;
    free(name);
    int ok = parent && ml_inherit(x, parent, depth + 1, ancestors) && sh_editor_sheets_inherit(tree, parent) >= 0;
    sh_dt_free(parent);
    return ok;
}

char *sh_modserve_editor_row_native(const char *row, size_t *len)
{
    char *result = NULL;
    if (len) *len = 0;
    AcquireSRWLockExclusive(&g_build_lock);
    ms_index *x = g_index;
    sh_dt *tree = x ? ml_native_tree(x, row, NULL) : NULL;
    if (tree && ml_inherit(x, tree, 0, NULL)) result = sh_dt_text(tree, len);
    sh_dt_free(tree);
    ReleaseSRWLockExclusive(&g_build_lock);
    return result;
}

static int ml_assign(sh_dt *tree, sh_dt_node *edit, const char *field, const char *value)
{
    sh_dt_node *old = sh_dt_child(edit, field);
    size_t len = strlen(value);
    if (old && old->val && old->val_n == len && !memcmp(old->val, value, len)) return 0;
    return sh_dt_assign_scalar(tree, edit, field, value) ? 1 : -1;
}

static int ml_same_parents(ms_index *x, const ms_layout_memo *m)
{
    for (int i = 0; i < m->ndeps; i++) {
        ml_dependency now = ml_source(x, m->deps[i].key);
        if (now.file != m->deps[i].file || now.body != m->deps[i].body) return 0;
    }
    return 1;
}

static ms_body *ml_project(ms_index *x, ms_layout_row *r, int file, ms_body *base, int scoped)
{
    if (file < 0) return NULL;
    ms_layout_memo **at = &r->memo;
    int count = 0;
    while (*at) {
        ms_layout_memo *m = *at;
        if (m->file == file && m->base == base && m->scoped == scoped && m->preferences == r->preferences &&
            ml_same_parents(x, m)) return m->result;
        count++;
        if (!m->next) break;
        at = &m->next;
    }
    ml_ancestors ancestors = {0};
    const ms_file *f = &x->files[file];
    size_t len = 0;
    unsigned char *raw = NULL;
    const char *bytes = NULL;
    if (base) { bytes = (const char *)base->p; len = base->len; }
    else if (!scoped || f->layout_anchor) { bytes = (const char *)f->body; len = f->body_len; }
    if (!bytes) { raw = ms_file_raw(f, &len); bytes = (const char *)raw; }
    sh_dt *tree = bytes ? sh_dt_parse(bytes, len, NULL, 0) : NULL;
    if (raw) HeapFree(GetProcessHeap(), 0, raw);
    ms_body *result = NULL;
    int changed = 0;
    if (tree) {
        sh_dt_node *edit = sh_dt_ensure_block(tree, sh_dt_root(tree), "edit");
        for (int k = 0; edit && changed >= 0 && k < ML_FIELDS; k++)
            if (r->field[k]) changed |= ml_assign(tree, edit, ml_native[k], r->field[k]);
        if (edit && changed >= 0 && r->field[ML_TAB])
            changed |= ml_assign(tree, edit, "metaCategoryNameTag", "\"#str_snapentity_metacategory_toybox\"");
        if (changed >= 0 && r->nsheets && ml_inherit(x, tree, 0, &ancestors))
            changed |= sh_editor_sheets_apply(tree, r->sheets, r->nsheets);
        if (changed > 0) {
            char *out = sh_dt_text(tree, &len);
            if (out) {
                for (result = r->variants; result; result = result->next)
                    if (result->len == len && !memcmp(result->p, out, len)) break;
                if (!result && (result = ms_reg_body_new(x, out, len)) != NULL) {
                    result->next = r->variants;
                    r->variants = result;
                }
                free(out);
            }
        }
        sh_dt_free(tree);
    }
    /* Engine consumers may still hold a body's address. Only the small lookup records are recycled. */
    ms_layout_memo *memo;
    if (count >= 8) { memo = *at; *at = NULL; }
    else {
        memo = ms_alloc(x, sizeof *memo);
        if (!memo) return result;
        memset(memo, 0, sizeof *memo);
    }
    if (ancestors.count > memo->capacity) {
        memo->deps = ms_alloc(x, (size_t)ancestors.count * sizeof *memo->deps);
        if (!memo->deps) return result;
        memo->capacity = ancestors.count;
    }
    if (ancestors.count) memcpy(memo->deps, ancestors.items, ancestors.count * sizeof *memo->deps);
    memo->ndeps = ancestors.count;
    memo->file = file;
    memo->preferences = r->preferences;
    memo->scoped = scoped;
    memo->base = base;
    memo->result = result;
    memo->next = r->memo;
    r->memo = memo;
    return result;
}

/* Select inheritance from the native rows of this scope before projecting any local sheet or tile edits. */
static int ml_select_row(ms_index *x, ms_layout_row *r, int scoped, int play, int mark)
{
    int key = r->provider >= 0 ? r->provider : r->anchor;
    if (key < 0) return 0;
    ms_file *f = &x->files[key];
    int best = r->source;
    ms_body *body = r->source_body;
    if (!play) {
        int source = best >= 0 ? best : r->anchor;
        ms_body *projected = ml_project(x, r, source, body, scoped);
        if (projected) { best = source; body = projected; }
    }
    int was = f->scope_alt;
    ms_body *was_body = was >= 0 ? x->files[was].scope_body : NULL;
    if (best >= 0)
        InterlockedExchangePointer((PVOID volatile *)&x->files[best].scope_body, body);
    InterlockedExchange(&f->scope_alt, best);
    if (was >= 0 && was != best)
        InterlockedExchangePointer((PVOID volatile *)&x->files[was].scope_body, NULL);
    if (best >= 0) InterlockedExchange(&f->scope_last, best);
    if (mark && !ms_pick_same(x, was, was_body, best, body)) {
        InterlockedExchange(&f->refresh_gen, 0);
        InterlockedExchange(&f->refresh, 1);
        return 1;
    }
    return 0;
}
