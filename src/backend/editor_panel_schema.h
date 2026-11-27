#ifndef SH_EDITOR_PANEL_SCHEMA_H
#define SH_EDITOR_PANEL_SCHEMA_H
#include "config_json.h"
#include "editor_ids.h"
#include "../common/text.h"

static inline int ep_object(const char *raw, sh_json_object *o)
{
    return raw && sh_json_parse_object(raw, strlen(raw), 32, o);
}

static inline int ep_identifier(const char *s)
{
    return s && *s && s[strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./:-[]#@")] == 0;
}

static inline char *ep_string(const char *raw)
{
    size_t n;
    char *s;
    if (!raw || *raw != '"') return NULL;
    n = strlen(raw);
    s = (char *)malloc(n);
    if (s && (!sh_json_decode_string(raw, n, s, n, &n) || strlen(s) != n)) { free(s); s = NULL; }
    return s;
}

static inline char *ep_tag(const char *salt, const char *key)
{
    size_t n = strlen(salt) + strlen(key) + 7;
    char *tag = (char *)malloc(n);
    if (tag && !sh_editor_string_tag(salt, key, tag, n)) { free(tag); return NULL; }
    return tag;
}

static inline int ep_problem(char *cause, size_t cap, const char *id, const char *member)
{
    if (cause && cap) snprintf(cause, cap, "Editor entry '%s' has an invalid '%s'.", id, member);
    return 0;
}

enum ep_kind { EP_SHEET, EP_PROPERTY, EP_SETTING };

static inline int ep_validate(const char *raw, enum ep_kind kind, char *cause, size_t cap)
{
    sh_json_object map = {0}, row = {0};
    size_t i, j;
    int ok = 0;
    if (!ep_object(raw, &map)) return ep_problem(cause, cap, "panel", "object");
    for (i = 0; i < map.count; ++i) {
        const char *id = map.members[i].key;
        if (strlen(id) != map.members[i].key_length || !ep_identifier(id) ||
            (kind == EP_SETTING && strpbrk(id, "/:#[]")) || !ep_object(map.members[i].value_json, &row)) {
            ep_problem(cause, cap, id, "id or object");
            goto done;
        }
        for (j = 0; j < row.count; ++j) {
            const char *key = row.members[j].key, *value = row.members[j].value_json;
            char *s;
            if (strlen(key) != row.members[j].key_length) { ep_problem(cause, cap, id, "field"); goto done; }
            if (kind == EP_SHEET && !strcmp(key, "properties")) {
                if (!ep_validate(value, EP_PROPERTY, cause, cap)) goto done;
                continue;
            }
            if (kind == EP_SHEET && !strcmp(key, "when")) {
                sh_json_object condition = {0};
                int valid = ep_object(value, &condition) && condition.count == 1 &&
                    ep_identifier(condition.members[0].key) && !strpbrk(condition.members[0].key, "/:#[]") &&
                    strlen(condition.members[0].key) == condition.members[0].key_length &&
                    (!strcmp(condition.members[0].value_json, "true") || !strcmp(condition.members[0].value_json, "false"));
                sh_json_object_free(&condition);
                if (!valid) { ep_problem(cause, cap, id, "when"); goto done; }
                continue;
            }
            if (kind == EP_SETTING && !strcmp(key, "default") && (!strcmp(value, "true") || !strcmp(value, "false"))) continue;
            s = ep_string(value);
            if (!ep_identifier(s) ||
                ((!strcmp(key, "title") || !strcmp(key, "description")) &&
                 !sh_editor_id_valid(!strncmp(s, "#str_", 5) ? s + 5 : s)) ||
                !( !strcmp(key, "title") || !strcmp(key, "description") ||
                  (kind != EP_SETTING && (!strcmp(key, "before") || !strcmp(key, "after"))) ||
                  (kind == EP_PROPERTY && !strcmp(key, "inspector")) ||
                  (kind == EP_SETTING && !strcmp(key, "scope") && (!strcmp(s, "map") || !strcmp(s, "editor"))) )) {
                free(s);
                ep_problem(cause, cap, id, key);
                goto done;
            }
            free(s);
        }
        if (sh_json_object_get(&row, "before") && sh_json_object_get(&row, "after")) {
            ep_problem(cause, cap, id, "before and after"); goto done;
        }
        if (kind == EP_SETTING && (!sh_json_object_get(&row, "scope") || !sh_json_object_get(&row, "title"))) {
            ep_problem(cause, cap, id, "scope or title"); goto done;
        }
        sh_json_object_free(&row);
    }
    ok = 1;
done:
    sh_json_object_free(&row);
    sh_json_object_free(&map);
    return ok;
}

static inline int sh_editor_sheets_validate(const char *sheets, char *cause, size_t cap)
{
    return ep_validate(sheets, EP_SHEET, cause, cap);
}
static inline int sh_editor_sheets_check_conditions(const char *sheets, const char *settings, char *cause, size_t cap)
{
    sh_json_object pages = {0}, defs = {0}, page = {0}, cond = {0}, decl = {0};
    size_t i;
    int ok = 0;
    if (!ep_object(sheets, &pages)) goto done;
    if (settings && !ep_object(settings, &defs)) goto done;
    for (i = 0; i < pages.count; ++i) {
        const char *raw, *scope;
        if (!ep_object(pages.members[i].value_json, &page)) goto done;
        raw = sh_json_object_get(&page, "when");
        if (raw) {
            if (!ep_object(raw, &cond) || cond.count != 1 ||
                !ep_object(sh_json_object_get(&defs, cond.members[0].key), &decl) ||
                !(scope = sh_json_object_get(&decl, "scope")) || strcmp(scope, "\"editor\"")) {
                if (cause && cap) snprintf(cause, cap, "Sheet '%s' needs an editor setting named '%s'.",
                    pages.members[i].key, cond.count ? cond.members[0].key : "");
                goto done;
            }
            sh_json_object_free(&cond);
            sh_json_object_free(&decl);
        }
        sh_json_object_free(&page);
    }
    ok = 1;
done:
    sh_json_object_free(&page);
    sh_json_object_free(&cond);
    sh_json_object_free(&decl);
    sh_json_object_free(&defs);
    sh_json_object_free(&pages);
    return ok;
}

static inline int sh_editor_settings_validate(const char *settings, char *cause, size_t cap)
{
    return ep_validate(settings, EP_SETTING, cause, cap);
}
#endif
