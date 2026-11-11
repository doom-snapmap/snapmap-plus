/* modbundle_old.c -- see modbundle_old.h. Store tool only. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modbundle.h"
#include "store_text.h"
#include "modbundle_old.h"
#include "mp_json.h"

#define MBO_WALK_DEPTH  48

static const char *const MBO_RETIRED[] = { "id", "key", "shadows", "stock", "scope", "player", "extends", "replaces",
                                           "legacy", "composes", "schema", NULL };
static const char *const MBO_FIELDS[] = { "name", "version", "author", "homepage", "requirements", "format", NULL };

static int mbo_in(const char *const *list, const char *k)
{
    int i;
    for (i = 0; list[i]; i++) if (!strcmp(list[i], k)) return 1;
    return 0;
}

static char *mbo_dump(const mpj *v)
{
    return v ? mpj_dump(v, NULL) : NULL;
}

static int mbo_add(sh_mb_old *o, const char *path, size_t plen)
{
    sh_mb_old_file *grown = (sh_mb_old_file *)realloc(o->files, sizeof *grown * (o->file_count + 1));
    sh_mb_old_file *f;
    if (!grown) return 0;
    o->files = grown;
    f = &o->files[o->file_count];
    memset(f, 0, sizeof *f);
    f->path = (char *)malloc(plen + 1);
    if (!f->path) return 0;
    memcpy(f->path, path, plen);
    f->path[plen] = 0;
    o->file_count++;
    return 1;
}

static void mbo_block_list(sh_mb_old *o, const mpj *list, int pointer)
{
    int i;
    for (i = 0; list && list->type == MPJ_ARR && i < list->n; i++) {
        const mpj *e = list->items[i], *p, *sha, *size, *src, *in, *cl;
        sh_mb_old_file *f;
        if (!e || e->type != MPJ_OBJ) continue;
        p = mpj_get(e, "path");
        sha = mpj_get(e, "sha256");
        size = mpj_get(e, "size");
        src = mpj_get(e, "source");
        in = mpj_get(e, "inAssets");
        cl = mpj_get(e, "closure");
        if (!p || p->type != MPJ_STR) continue;
        if (!mbo_add(o, p->text, strlen(p->text))) return;
        f = &o->files[o->file_count - 1];
        if (sha && sha->type == MPJ_STR) st_cpy(f->sha256, sizeof f->sha256, sha->text);
        if (size && mpj_is_num(size)) f->size = (unsigned long long)mpj_int(size);
        f->pointer = pointer;
        if (pointer && src && src->type == MPJ_STR) f->source = _strdup(src->text);
        f->absent = pointer && in && in->type == MPJ_BOOL && !in->b;
        f->closure = pointer && cl && cl->type == MPJ_BOOL && cl->b;
    }
}

static void mbo_walk(sh_mb_old *o, const wchar_t *dir, const char *rel, int depth)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern;
    size_t dn = wcslen(dir);
    HANDLE h;
    if (depth > MBO_WALK_DEPTH) return;
    pattern = (wchar_t *)malloc(sizeof(wchar_t) * (dn + 3));
    if (!pattern) return;
    _snwprintf_s(pattern, dn + 3, _TRUNCATE, L"%s\\*", dir);
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char *name, child[1024];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        name = sh_mb_utf8(fd.cFileName);
        if (!name) continue;
        st_fmt(child, sizeof child, "%s/%s", rel, name);
        free(name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            size_t sn = dn + wcslen(fd.cFileName) + 2;
            wchar_t *sub = (wchar_t *)malloc(sizeof(wchar_t) * sn);
            if (!sub) continue;
            _snwprintf_s(sub, sn, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            mbo_walk(o, sub, child, depth + 1);
            free(sub);
        } else if (mbo_add(o, child, strlen(child))) {
            o->files[o->file_count - 1].size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static unsigned char *mbo_read(const char *folder, const char *leaf, size_t *len)
{
    char path[ST_PATH];
    st_fmt(path, sizeof path, "%s\\%s", folder, leaf);
    return sh_mb_read_file(path, (size_t)-1, len);
}

static void mbo_leaf_key(sh_mb_old *o, const char *folder)
{
    const char *leaf = folder + strlen(folder), *end;
    char name[256];
    while (leaf > folder && (leaf[-1] == '\\' || leaf[-1] == '/')) leaf--;
    end = leaf;
    while (leaf > folder && leaf[-1] != '\\' && leaf[-1] != '/') leaf--;
    st_fmt(name, sizeof name, "%.*s", (int)(end - leaf), leaf);
    if (sh_mb_id_normalize(name, strlen(name), o->key)) o->key_from = SH_MB_OLD_KEY_FROM_FOLDER;
    else o->key[0] = 0;
}

int sh_mb_old_load(const char *folder, sh_mb_old *o)
{
    unsigned char *text;
    size_t len = 0;
    mpj *root, *kept, *retired;
    const mpj *block, *v;
    int i;
    if (!o) return 0;
    memset(o, 0, sizeof *o);
    o->validated = 1;
    if (!folder) { strcpy_s(o->cause, sizeof o->cause, "no folder given"); return 0; }
    strcpy_s(o->descriptor, sizeof o->descriptor, "mod.json");
    text = mbo_read(folder, "mod.json", &len);
    if (!text) {
        strcpy_s(o->descriptor, sizeof o->descriptor, "package.json");
        text = mbo_read(folder, "package.json", &len);
    }
    if (!text) { strcpy_s(o->cause, sizeof o->cause, "the folder has no readable mod.json or package.json"); return 0; }
    root = mpj_parse((const char *)text, len);
    free(text);
    if (!root || root->type != MPJ_OBJ) {
        mpj_free(root);
        st_putf(o->cause, sizeof o->cause, "%s is not a JSON object", o->descriptor);
        return 0;
    }
    o->name = mbo_dump(mpj_get(root, "name"));
    o->version = mbo_dump(mpj_get(root, "version"));
    o->author = mbo_dump(mpj_get(root, "author"));
    o->homepage = mbo_dump(mpj_get(root, "homepage"));
    o->requirements = mbo_dump(mpj_get(root, "requirements"));
    kept = mpj_new(MPJ_OBJ);
    retired = mpj_new(MPJ_OBJ);
    for (i = 0; kept && retired && i < root->n; i++) {
        const char *k = root->keys[i];
        if (!strcmp(k, SH_MB_BLOCK_KEY) || mbo_in(MBO_FIELDS, k)) continue;
        if (mbo_in(MBO_RETIRED, k)) mpj_set(retired, k, mpj_clone(root->items[i]));
        else mpj_set(kept, k, mpj_clone(root->items[i]));
    }
    o->kept = kept ? mpj_dump(kept, NULL) : NULL;
    o->retired = retired ? mpj_dump(retired, NULL) : NULL;
    mpj_free(kept);
    mpj_free(retired);

    block = mpj_get(root, SH_MB_BLOCK_KEY);
    if (block && block->type == MPJ_OBJ) {
        v = mpj_get(block, "format");
        o->block_format = v && mpj_is_num(v) ? (int)mpj_int(v) : 1;
        v = mpj_get(block, "key");
        if (v && v->type == MPJ_STR && sh_mb_id_normalize(v->text, strlen(v->text), o->key))
            o->key_from = SH_MB_OLD_KEY_FROM_BLOCK;
        v = mpj_get(block, "bundle");
        if (v && v->type == MPJ_STR) st_cpy(o->bundle, sizeof o->bundle, v->text);
        v = mpj_get(block, "build");
        if (v && mpj_is_num(v)) o->build = (unsigned long long)mpj_int(v);
        v = mpj_get(block, "built");
        if (v && mpj_is_num(v)) o->built = mpj_int(v);
        v = mpj_get(block, "rules");
        if (v && mpj_is_num(v)) o->rules = (int)mpj_int(v);
        v = mpj_get(block, "validated");
        if (v && v->type == MPJ_BOOL) o->validated = v->b;
        v = mpj_get(block, "origin");
        if (v && v->type == MPJ_STR && strcmp(v->text, "by-hand"))
            st_cpy(o->origin, sizeof o->origin, v->text);
        o->block_legacy = mbo_dump(mpj_get(block, "legacy"));
        o->block_shadows = mbo_dump(mpj_get(block, "shadows"));
        mbo_block_list(o, mpj_get(block, "files"), 0);
        mbo_block_list(o, mpj_get(block, "stock"), 1);
    }
    /* The key of before: the author id when present (it won over the
     * block's), else the block's, else the folder's own name. */
    v = mpj_get(root, "id");
    if (v && v->type == MPJ_STR && sh_mb_id_normalize(v->text, strlen(v->text), o->key))
        o->key_from = SH_MB_OLD_KEY_FROM_ID;
    if (!o->key_from) mbo_leaf_key(o, folder);
    if (!block || block->type != MPJ_OBJ) {
        char assets[ST_PATH];
        wchar_t *w;
        st_fmt(assets, sizeof assets, "%s\\assets", folder);
        w = sh_mb_wpath(assets);
        if (w) { mbo_walk(o, w, "assets", 1); free(w); }
    }
    mpj_free(root);
    if (!o->kept || !o->retired) { strcpy_s(o->cause, sizeof o->cause, "out of memory"); return 0; }
    return 1;
}

void sh_mb_old_free(sh_mb_old *o)
{
    size_t i;
    if (!o) return;
    free(o->name); free(o->version); free(o->author); free(o->homepage); free(o->requirements);
    free(o->kept); free(o->retired); free(o->block_legacy); free(o->block_shadows);
    for (i = 0; i < o->file_count; i++) { free(o->files[i].path); free(o->files[i].source); }
    free(o->files);
    memset(o, 0, sizeof *o);
}

int sh_mb_old_is_old(const char *folder)
{
    int s = sh_mb_descriptor_state(folder);
    return s == SH_MB_DESC_EARLIER || s == SH_MB_DESC_RETIRED_KEY;
}
