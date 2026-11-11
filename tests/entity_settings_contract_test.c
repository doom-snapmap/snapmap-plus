#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n",                            \
                __FILE__, __LINE__, #expr);                                     \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

static char *read_all(const char *path)
{
    FILE *file = NULL;
    long length;
    char *bytes;
    if (fopen_s(&file, path, "rb") != 0 || !file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    bytes = (char *)malloc((size_t)length + 1u);
    if (!bytes) { fclose(file); return NULL; }
    if (length && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    bytes[length] = 0;
    return bytes;
}

static unsigned int count_text(const char *text, const char *needle)
{
    unsigned int count = 0;
    size_t length = strlen(needle);
    const char *at = text;
    while ((at = strstr(at, needle)) != NULL) {
        count++;
        at += length;
    }
    return count;
}

static const char *matching_brace(const char *open)
{
    const char *at;
    unsigned int depth = 0;
    char quote = 0;
    int escaped = 0;
    int line_comment = 0;
    int block_comment = 0;
    if (!open || *open != '{') return NULL;
    for (at = open; *at; at++) {
        char c = *at;
        char next = at[1];
        if (line_comment) {
            if (c == '\r' || c == '\n') line_comment = 0;
            continue;
        }
        if (block_comment) {
            if (c == '*' && next == '/') {
                block_comment = 0;
                at++;
            }
            continue;
        }
        if (quote) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '/' && next == '/') {
            line_comment = 1;
            at++;
        } else if (c == '/' && next == '*') {
            block_comment = 1;
            at++;
        } else if (c == '\'' || c == '"' || c == '`') {
            quote = c;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            if (--depth == 0) return at;
        }
    }
    return NULL;
}

static const char *find_in_body(
    const char *body,
    const char *end,
    const char *needle)
{
    const char *found;
    if (!body || !end) return NULL;
    found = strstr(body, needle);
    return found && found < end ? found : NULL;
}

int main(int argc, char **argv)
{
    char *html;
    const char *apply;
    const char *open;
    const char *end;
    const char *select_predicate;
    const char *sync_checked;
    const char *select_checked;
    const char *follow_if;
    const char *follow_open;
    const char *follow_end;
    const char *else_branch;
    const char *else_open;
    const char *else_end;
    const char *follow_assign;
    const char *follow_post;
    const char *off_post;
    const char *select_assign;
    const char *guarded_push;
    if (argc != 2) {
        fprintf(stderr,
                "usage: entity_settings_contract_test <mockup.html>\n");
        return 2;
    }
    html = read_all(argv[1]);
    CHECK(html != NULL);
    if (!html) return 1;

    CHECK(strstr(html,
          "var ENTITY_SHOW_HIDDEN_KEY = 'entities.show_hidden';") != NULL);
    CHECK(strstr(html,
          "var ENTITY_SELECTION_MODE_KEY = 'entities.selection_mode';") != NULL);
    CHECK(strstr(html, "function receiveEntitySetting(d)") != NULL);
    CHECK(strstr(html,
          "typeof parsedShowHidden === 'boolean'") != NULL);
    CHECK(strstr(html,
          "entityModeValid(parsedSelectionMode)") != NULL);
    CHECK(strstr(html,
          "entitySettingsPending.showHidden === null") != NULL);
    CHECK(strstr(html,
          "entitySettingsPending.selectionMode === null") != NULL);
    CHECK(strstr(html,
          "applyEntitySelectionMode(entitySettingsPending.selectionMode, false)") != NULL);
    CHECK(strstr(html, "configGet(ENTITY_SHOW_HIDDEN_KEY)") != NULL);
    CHECK(strstr(html, "configGet(ENTITY_SELECTION_MODE_KEY)") != NULL);
    CHECK(strstr(html, "'follow' : 'off'") != NULL);
    CHECK(strstr(html, "'select_in_3d' : 'off'") != NULL);
    CHECK(strstr(html,
          "persistEntitySetting(ENTITY_SHOW_HIDDEN_KEY, this.checked)") != NULL);
    CHECK(count_text(
          html,
          "persistEntitySetting(ENTITY_SELECTION_MODE_KEY, mode)") == 2);
    CHECK(count_text(html, "localStorage") == 0);
    CHECK(strstr(html, "id=\"deselectBtn\"") == NULL);
    CHECK(strstr(html, "function isSelectionWhitespaceTarget(target)") != NULL);
    CHECK(strstr(html,
          "if (target.closest('.right-pane')) return false;") != NULL);
    CHECK(strstr(html, "[role=\"separator\"]") != NULL);
    CHECK(strstr(html,
          "function clearEntitySelection()") != NULL);
    CHECK(strstr(html,
          "function clearWorkspaceSelectionsFromWhitespace(target)") != NULL);
    CHECK(strstr(html,
          "panel.id === 'panel-prefabs' && selectedPrefabName") != NULL);
    CHECK(strstr(html,
          "panel.id === 'panel-timelines' && (selectedTlEid !== -1 || tlModel)") != NULL);
    CHECK(strstr(html,
          "document.querySelector('.app').addEventListener('click'") != NULL);
    CHECK(strstr(html,
          "clearWorkspaceSelectionsFromWhitespace(e.target);") != NULL);
    CHECK(count_text(html, "post({cmd:'deselect'});") == 1);
    CHECK(strstr(html,
          "if (editor.classList.contains('focus-mode')) setDeclFocus(false);") != NULL);

    apply = strstr(
        html,
        "function applyEntitySelectionMode(mode, pushSelection)");
    open = apply ? strchr(apply, '{') : NULL;
    end = matching_brace(open);
    CHECK(apply != NULL);
    CHECK(end != NULL);
    select_predicate = find_in_body(
        apply, end, "var select = mode === 'select_in_3d';");
    sync_checked = find_in_body(
        apply, end,
        "document.getElementById('syncEditor').checked = follow;");
    select_checked = find_in_body(
        apply, end,
        "document.getElementById('selectInEditor').checked = select;");
    follow_if = find_in_body(apply, end, "if (follow) {");
    follow_open = follow_if ? strchr(follow_if, '{') : NULL;
    follow_end = matching_brace(follow_open);
    else_branch = follow_end && follow_end < end
        ? strstr(follow_end, "} else {") : NULL;
    if (else_branch && else_branch >= end) else_branch = NULL;
    else_open = else_branch ? strchr(else_branch, '{') : NULL;
    else_end = matching_brace(else_open);
    follow_assign = find_in_body(
        follow_open, follow_end, "selectMode = false;");
    follow_post = find_in_body(
        follow_open, follow_end, "post({cmd:'setSync', on:1});");
    off_post = find_in_body(
        else_open, else_end, "post({cmd:'setSync', on:0});");
    select_assign = find_in_body(
        else_open, else_end, "selectMode = select;");
    guarded_push = find_in_body(
        else_open, else_end,
        "if (select && pushSelection) pushSelectionToEditor();");
    CHECK(select_predicate != NULL);
    CHECK(sync_checked != NULL);
    CHECK(select_checked != NULL);
    CHECK(follow_if != NULL);
    CHECK(follow_end != NULL);
    CHECK(else_branch != NULL);
    CHECK(else_end != NULL);
    CHECK(follow_assign != NULL);
    CHECK(follow_post != NULL);
    CHECK(off_post != NULL);
    CHECK(select_assign != NULL);
    CHECK(guarded_push != NULL);
    if (select_predicate && sync_checked)
        CHECK(select_predicate < sync_checked);
    if (sync_checked && select_checked)
        CHECK(sync_checked < select_checked);
    if (select_checked && follow_if)
        CHECK(select_checked < follow_if);
    if (follow_assign && follow_post)
        CHECK(follow_assign < follow_post);
    if (off_post && select_assign) CHECK(off_post < select_assign);
    if (select_assign && guarded_push)
        CHECK(select_assign < guarded_push);

    free(html);
    if (g_failed) {
        fprintf(stderr,
                "entity_settings_contract_test: %d failure(s)\n",
                g_failed);
        return 1;
    }
    puts("entity_settings_contract_test: OK");
    return 0;
}
