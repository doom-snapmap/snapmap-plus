/* An owned tile produces the same native row as a file in decls/snapeditorentitydef/. */
static const char *mb_tile_string(const mbj *v, const char *key)
{
    const mbj *item = mbj_get(v, key);
    return item && item->t == MBJ_STR && item->n == strlen(item->s) ? item->s : NULL;
}

static int mb_tile_set(sh_dt *tree, sh_dt_node *parent, const char *key, const char *value, int string_id, int *changed)
{
    char *quoted;
    int ok;
    if (!value) return 1;
    quoted = sh_textf("\"%s%s\"", string_id && strncmp(value, "#str_", 5) ? "#str_" : "", value);
    sh_dt_node *old = sh_dt_child(parent, key);
    if (old && old->val && quoted && old->val_n == strlen(quoted) && !memcmp(old->val, quoted, old->val_n)) {
        free(quoted); return 1;
    }
    *changed = 1;
    ok = quoted && sh_dt_assign_scalar(tree, parent, key, quoted);
    free(quoted);
    return ok;
}

/* Check every component so a template cannot leave the author folder through a junction. */
static char *mb_tile_template(const wchar_t *root, const char *rel, size_t *size)
{
    wchar_t *path, *p;
    DWORD attr;
    char *text = NULL;
    if (strncmp(rel, "editor/templates/", 17) || !mb_ends(rel, ".decl") || !sh_mb_check_request(rel, NULL)) return NULL;
    path = mb_file_wpath(root, rel);
    if (!path) return NULL;
    for (p = path + wcslen(root) + 1; ; p++) {
        wchar_t c = *p;
        if (c && c != L'\\') continue;
        *p = 0;
        attr = GetFileAttributesW(path);
        *p = c;
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) break;
        if (!c) {
            if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) text = (char *)mb_read_wfile(path, MB_ANY_SIZE, size);
            break;
        }
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) break;
    }
    free(path);
    return text;
}

static int mb_tile_fields(const mbj *tile, const char *id, char **cause)
{
    const int owned = mbj_get(tile, "entity") != NULL;
    if (!sh_editor_id_valid(id)) return mb_cause(cause, "invalid editor tile ID %s", id);
    for (size_t i = 0; i < tile->count; i++) {
        const char *key = tile->keys[i], *v = mb_tile_string(tile, key);
        int token = !strcmp(key, "title") || !strcmp(key, "description") || !strcmp(key, "tab");
        if (!strcmp(key, "order")) {
            const mbj *order = tile->items[i];
            if (order->t != MBJ_NUM || !mb_plain_integer(order->s, order->n) ||
                _strtoi64(order->s, NULL, 10) < INT32_MIN || _strtoi64(order->s, NULL, 10) > INT32_MAX)
                return mb_cause(cause, "editor tile %s needs a signed integer order", id);
            continue;
        }
        if (!token && strcmp(key, "icon") && (!owned ||
            (strcmp(key, "entity") && strcmp(key, "base") && strcmp(key, "template"))))
            return mb_cause(cause, "editor tile %s: unexpected field %s", id, key);
        if (!v || !*v || (token ? !sh_editor_id_valid(!strncmp(v, "#str_", 5) ? v + 5 : v) :
               !sh_mb_check_request(v, NULL)))
            return mb_cause(cause, "editor tile %s needs a valid %s", id, key);
    }
    return 1;
}

static int mb_tile_build(mb_walk *w, const char *id, const mbj *tile, char **cause)
{
    static const char empty[] = "{\n edit = { metaCategoryNameTag = \"#str_snapentity_metacategory_toybox\"; categoryNameTag = \"#str_sh_category_1\"; }\n}\n";
    const char *templ = mb_tile_string(tile, "template");
    const char *initial = mbj_get(tile, "base") ? "{ edit = { } }" : empty;
    char *native = NULL, *output = NULL, *logical = NULL;
    size_t size = strlen(initial), out_len = 0;
    char parse[SH_DT_CAUSE_CAP] = {0};
    wchar_t *dest = NULL, *plain = NULL;
    sh_dt *tree = NULL;
    sh_dt_node *edit;
    int ok = 0, changed = 0;
    if (!mb_tile_fields(tile, id, cause)) return 0;
    if (!mbj_get(tile, "entity")) return 1;
    if (templ && !(native = mb_tile_template(w->root, templ, &size))) {
        mb_cause(cause, "%s: cannot read %s as an editor/templates/ declaration", id, templ); goto done;
    }
    tree = sh_dt_parse(native ? native : initial, size, parse, sizeof parse);
    edit = tree ? sh_dt_at(sh_dt_root(tree), "edit") : NULL;
    if (!edit || !edit->block || !mb_tile_set(tree, sh_dt_root(tree), "inherit", mb_tile_string(tile, "base"), 0, &changed) ||
        !mb_tile_set(tree, edit, "entityDef", mb_tile_string(tile, "entity"), 0, &changed) ||
        !mb_tile_set(tree, edit, "displayNameTag", mb_tile_string(tile, "title"), 1, &changed) ||
        !mb_tile_set(tree, edit, "displayDescriptionTag", mb_tile_string(tile, "description"), 1, &changed) ||
        !mb_tile_set(tree, edit, "previewImage", mb_tile_string(tile, "icon"), 0, &changed)) {
        mb_cause(cause, "%s: invalid native editor template (%s)", id, *parse ? parse : "expected unique edit fields");
        goto done;
    }
    if (native && !changed) { output = native; native = NULL; out_len = size; }
    else output = sh_dt_text(tree, &out_len);
    logical = sh_textf("decls/snapeditorentitydef/%s.decl", id);
    if (!logical || !output) goto done;
    mb_lower(logical);
    plain = mb_file_wpath(w->root, logical);
    if (!plain || GetFileAttributesW(plain) != INVALID_FILE_ATTRIBUTES) {
        mb_cause(cause, "%s and editor tile %s both define the same row", logical, id); goto done;
    }
    dest = mb_compiled_path(w->root, logical, 1);
    size_t previous_len = 0;
    unsigned char *previous = dest ? mb_read_wfile(dest, out_len, &previous_len) : NULL;
    int matches = previous && previous_len == out_len && !memcmp(previous, output, out_len);
    free(previous);
    if (!matches && (!dest || !mb_write_wfile(dest, output, out_len))) {
        mb_cause(cause, "could not write compiled editor tile %s", id); goto done;
    }
    mb_walk_file(w, logical, out_len);
    ok = 1;
done:
    sh_dt_free(tree); free(plain); free(dest); free(native); free(output); free(logical);
    if (!ok && cause && !*cause) mb_cause(cause, "out of memory");
    return ok;
}

static void mb_editor_build(mb_walk *w, const char *source, size_t size)
{
    mbj *doc = mbj_parse(source, size, NULL), *editor = mbj_get(doc, "editor"), *tiles = mbj_get(editor, "tiles");
    char *cause = NULL;
    for (size_t i = 0; tiles && i < tiles->count; i++)
        if (!mb_tile_build(w, tiles->keys[i], tiles->items[i], &cause)) {
            mb_error(w->b, "%s", cause ? cause : "out of memory"); break;
        }
    mbj_free(doc); free(cause);
}


static int mb_tile_hash_cmp(const void *left, const void *right)
{
    return strcmp((const char *)left, (const char *)right);
}

static void mb_editor_prune(const wchar_t *root, const sh_mb_bundle *b)
{
    wchar_t *manifest = mb_manifest_path(root, 0), *pattern;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char (*active)[65] = NULL;
    size_t n = 0;
    if (!manifest) return;
    *wcsrchr(manifest, L'\\') = 0;
    pattern = mb_wjoin(manifest, L"tile-*.decl");
    h = pattern ? FindFirstFileW(pattern, &fd) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) { free(manifest); return; }
    for (size_t i = 0; i < b->file_count; i++) {
        const char *rel = b->files[i].path;
        if (rel && !b->files[i].absent && !strncmp(rel, "decls/snapeditorentitydef/", 26)) {
            wchar_t *plain = mb_file_wpath(root, rel);
            if (!plain) goto done;
            if (GetFileAttributesW(plain) == INVALID_FILE_ATTRIBUTES) {
                if (!active) active = calloc(b->file_count, sizeof *active);
                if (!active) { free(plain); goto done; }
                sh_mb_sha256_hex(rel, strlen(rel), active[n++]);
            }
            free(plain);
        }
    }
    if (n > 1) qsort(active, n, sizeof *active, mb_tile_hash_cmp);
    do {
        char digest[65];
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (wcslen(fd.cFileName) != 74 || wcsspn(fd.cFileName + 5, L"0123456789abcdef") != 64) continue;
        for (size_t i = 0; i < 64; i++) digest[i] = (char)fd.cFileName[5+i];
        digest[64] = 0;
        if (!n || !bsearch(digest, active, n, sizeof *active, mb_tile_hash_cmp)) {
            wchar_t *file = mb_wjoin(manifest, fd.cFileName);
            if (file) DeleteFileW(file);
            free(file);
        }
    } while (FindNextFileW(h, &fd));
done:
    FindClose(h);
    free(manifest); free(active);
}
