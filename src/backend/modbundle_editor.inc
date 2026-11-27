/* Author files under editor/ are assembled before normal mod name resolution. */
static mbj *mb_editor_object(mbj *parent, const char *key)
{
    mbj *obj = mbj_get(parent, key);
    char *copy;
    if (obj) return obj->t == MBJ_OBJ ? obj : NULL;
    obj = (mbj *)calloc(1, sizeof *obj);
    copy = mb_strdup(key);
    if (!obj || !copy || !mbj_push(parent, obj, copy, strlen(key))) {
        free(copy);
        free(obj);
        return NULL;
    }
    obj->t = MBJ_OBJ;
    return obj;
}

static int mb_editor_join(mbj *target, mbj *source, const char *file, char **cause)
{
    for (size_t i = 0; i < source->count; i++) {
        const char *key = source->keys[i];
        if (!*key || strlen(key) != source->klen[i])
            return mb_cause(cause, "%s contains an invalid empty or NUL key", file);
        if (mbj_get(target, key)) return mb_cause(cause, "%s defines %s more than once", file, key);
        if (!mbj_push(target, source->items[i], source->keys[i], source->klen[i]))
            return mb_cause(cause, "out of memory");
        source->items[i] = NULL;
        source->keys[i] = NULL;
    }
    return 1;
}

static int mb_editor_locale(const char *code)
{
    size_t first = strspn(code, "abcdefghijklmnopqrstuvwxyz");
    const char *p = code + first;
    if (first < 2 || first > 3) return 0;
    while (*p) {
        if (*p++ != '-') return 0;
        size_t n = strcspn(p, "-");
        int region = (n == 2 && strspn(p, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") == n) ||
                     (n == 3 && strspn(p, "0123456789") == n);
        int script = n == 4 && p[0] >= 'A' && p[0] <= 'Z' && strspn(p+1, "abcdefghijklmnopqrstuvwxyz") == 3;
        if (!region && !script) return 0;
        p += n;
    }
    return 1;
}

static int mb_editor_file(mbj *root, const wchar_t *dir, const wchar_t *leaf, int catalog, char **cause)
{
    wchar_t *path = mb_wjoin(dir, leaf);
    char *file = sh_mb_utf8(leaf);
    size_t len = 0;
    char *text = path ? (char *)mb_read_wfile(path, MB_ANY_SIZE, &len) : NULL, *reason = NULL;
    mbj *doc = text ? mbj_parse(text, len, &reason) : NULL, *target;
    int ok = 0;
    if (!file || !doc || doc->t != MBJ_OBJ) {
        mb_cause(cause, "editor/%s%s must contain a JSON object (%s)", catalog ? "strings/" : "",
                 file ? file : "?", reason ? reason : "missing or invalid file");
        goto done;
    }
    if (catalog) {
        char *locale = mb_strndup(file, strlen(file) - 5);
        if (!locale) goto done;
        if (!mb_editor_locale(locale)) {
            mb_cause(cause, "editor/strings/%s needs a locale filename such as en.json, es-419.json or zh-Hant.json", file);
            free(locale); goto done;
        }
        target = mb_editor_object(root, "strings");
        if (target) target = mb_editor_object(target, locale);
        free(locale);
        if (!target) { mb_cause(cause, "%s conflicts with mod.json strings", file); goto done; }
        for (size_t i = 0; i < doc->count; i++)
            if (doc->items[i]->t != MBJ_STR || strlen(doc->items[i]->s) != doc->items[i]->n) {
                mb_cause(cause, "editor/strings/%s: %s needs a text value", file, doc->keys[i]);
                goto done;
            }
        ok = mb_editor_join(target, doc, file, cause);
    } else {
        mbj *editor = mb_editor_object(root, "editor");
        if (!editor) goto done;
        for (size_t i = 0; i < doc->count; i++) {
            const char *key = doc->keys[i];
            mbj *entries = doc->items[i];
            if (strcmp(key, "tabs") && strcmp(key, "tiles") && strcmp(key, "sheets") && strcmp(key, "settings")) {
                mb_cause(cause, "editor/%s: unknown section %s", file, key); goto done;
            }
            if (entries->t != MBJ_OBJ || !(target = mb_editor_object(editor, key))) {
                mb_cause(cause, "editor/%s: %s needs an object of IDs", file, key); goto done;
            }
            for (size_t j = 0; j < entries->count; j++)
                if (entries->items[j]->t != MBJ_OBJ) {
                    mb_cause(cause, "editor/%s: %s.%s must be an object", file, key, entries->keys[j]); goto done;
                }
            if (!strcmp(key, "tabs")) {
                for (size_t j = 0; j < entries->count; j++) {
                    const mbj *tab = entries->items[j], *title = mbj_get(tab, "title");
                    const char *id = entries->keys[j], *value = title && title->t == MBJ_STR ? title->s : NULL;
                    if (!sh_editor_id_valid(!strncmp(id, "#str_", 5) ? id + 5 : id) || tab->count != 1 ||
                        !value || strlen(value) != title->n || !sh_editor_id_valid(!strncmp(value, "#str_", 5) ? value + 5 : value)) {
                        mb_cause(cause, "editor/%s: tab %s needs a title string key", file, id); goto done;
                    }
                }
            }
            if (!strcmp(key, "sheets") || !strcmp(key, "settings")) {
                int sheets = !strcmp(key, "sheets");
                for (size_t j = 0; j < (sheets ? entries->count : 1); j++) {
                    mb_buf raw = {0};
                    char why[256] = {0};
                    int valid = mbj_canon(sheets ? entries->items[j] : entries, &raw, NULL) && !raw.oom &&
                        (sheets ? sh_editor_sheets_validate(raw.p, why, sizeof why) :
                                  sh_editor_settings_validate(raw.p, why, sizeof why));
                    free(raw.p);
                    if (!valid) { mb_cause(cause, "editor/%s: %s", file, *why ? why : "invalid panel"); goto done; }
                }
            }
            if (!mb_editor_join(target, entries, file, cause)) goto done;
        }
        ok = 1;
    }
done:
    if (!ok && cause && !*cause) mb_cause(cause, "out of memory");
    mbj_free(doc);
    free(reason); free(file); free(path); free(text);
    return ok;
}

static int mb_editor_scan(mbj *root, const wchar_t *dir, int catalog, char **cause)
{
    wchar_t *pattern = mb_wjoin(dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = pattern ? FindFirstFileW(pattern, &fd) : INVALID_HANDLE_VALUE;
    int ok = 1;
    DWORD error;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 1 : mb_cause(cause, "cannot list editor files");
    do {
        const wchar_t *leaf = fd.cFileName, *ext;
        if (!wcscmp(leaf, L".") || !wcscmp(leaf, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ok = mb_cause(cause, "editor files must be real files and folders"); break; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!catalog && !_wcsicmp(leaf, L"strings")) {
                wchar_t *sub = mb_wjoin(dir, leaf);
                ok = sub && mb_editor_scan(root, sub, 1, cause);
                free(sub);
            }
            continue;
        }
        ext = wcsrchr(leaf, L'.');
        if (ext && ext != leaf && !_wcsicmp(ext, L".json"))
            ok = mb_editor_file(root, dir, leaf, catalog, cause);
    } while (ok && FindNextFileW(h, &fd));
    error = GetLastError();
    FindClose(h);
    if (ok && error != ERROR_NO_MORE_FILES) return mb_cause(cause, "could not finish reading editor files");
    return ok;
}

static int mb_editor_conditions(const mbj *root, char **cause)
{
    const mbj *editor = mbj_get(root, "editor"), *targets = mbj_get(editor, "sheets"), *settings = mbj_get(editor, "settings");
    mb_buf controls = {0};
    int ok = 1;
    if (!targets) return 1;
    if (settings && (!mbj_canon(settings, &controls, cause) || controls.oom)) ok = 0;
    for (size_t i = 0; ok && i < targets->count; i++) {
        mb_buf pages = {0};
        char why[256] = {0};
        ok = mbj_canon(targets->items[i], &pages, cause) && !pages.oom &&
             sh_editor_sheets_check_conditions(pages.p, controls.p ? controls.p : "{}", why, sizeof why);
        free(pages.p);
        if (!ok) mb_cause(cause, "editor sheets for %s: %s", targets->keys[i], *why ? why : "invalid settings");
    }
    free(controls.p);
    return ok;
}

/* NULL with no cause means the untouched mod.json is already the effective source. */
static char *mb_editor_source(const wchar_t *folder, const char *source, size_t len, size_t *size, char **cause)
{
    wchar_t *dir = mb_wjoin(folder, L"editor");
    DWORD attr, err;
    mbj *root;
    mb_buf json = {0};
    if (!dir) { mb_cause(cause, "out of memory"); return NULL; }
    attr = GetFileAttributesW(dir);
    err = GetLastError();
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) mb_cause(cause, "cannot read the editor folder");
        free(dir);
        return NULL;
    }
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        mb_cause(cause, "editor must be a real folder"); free(dir); return NULL;
    }
    root = mbj_parse(source, len, cause);
    if (root && root->t == MBJ_OBJ) {
        const mbj *inline_strings = mbj_get(root, "strings");
        if (inline_strings && inline_strings->t == MBJ_OBJ && inline_strings->count)
            mb_cause(cause, "move mod.json strings into editor/strings/<locale>.json before using the editor folder");
        else if (mbj_get(root, "editor")) mb_cause(cause, "move mod.json editor definitions into editor/*.json");
        else if (mb_editor_scan(root, dir, 0, cause) && mb_editor_conditions(root, cause) && mbj_canon(root, &json, cause) && !json.oom)
            *size = json.n;
        else { free(json.p); json.p = NULL; }
    }
    free(dir);
    mbj_free(root);
    return json.p;
}

char *sh_mb_read_editor_source(const char *folder, size_t *len)
{
    wchar_t *dir = folder ? sh_mb_wpath(folder) : NULL, *path = dir ? mb_wjoin(dir, L"mod.json") : NULL;
    size_t n = 0;
    char *source = path ? (char *)mb_read_wfile(path, MB_ANY_SIZE, &n) : NULL, *reason = NULL;
    char *effective = source ? mb_editor_source(dir, source, n, &n, &reason) : NULL;
    if (effective || reason) { free(source); source = effective; }
    free(reason); free(path); free(dir);
    if (len) *len = source ? n : 0;
    return source;
}
