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

static const char *find_text_eol_agnostic(
    const char *text,
    const char *needle)
{
    const char *start;
    const char *at;
    const char *want;
    if (!*needle) return text;
    for (start = text; *start; start++) {
        at = start;
        want = needle;
        while (*want) {
            if (*want == '\n') {
                if (*at == '\r') at++;
                if (*at != '\n') break;
            } else if (*at != *want) {
                break;
            }
            at++;
            want++;
        }
        if (!*want) return start;
    }
    return NULL;
}

static void check_eol_matcher(void)
{
    static const char expected[] = "alpha\nbeta";
    CHECK(find_text_eol_agnostic("alpha\nbeta", expected) != NULL);
    CHECK(find_text_eol_agnostic("alpha\r\nbeta", expected) != NULL);
    CHECK(find_text_eol_agnostic("alpha\rbeta", expected) == NULL);
}

int main(int argc, char **argv)
{
    char *html;
    const char *preview_read;
    const char *preview_write;
    const char *read_storage;
    const char *write_storage;
    if (argc != 2) {
        fprintf(stderr, "usage: theme_contract_test <mockup.html>\n");
        return 2;
    }
    html = read_all(argv[1]);
    CHECK(html != NULL);
    if (!html) return 1;
    check_eol_matcher();

    CHECK(strstr(html, "<html lang=\"en\">") != NULL);
    CHECK(strstr(html, "function applyTheme(dark, persist)") != NULL);
    CHECK(strstr(html, "function configGet(key)") != NULL);
    CHECK(strstr(html, "function configSet(key, value)") != NULL);
    CHECK(strstr(html, "valueJson:JSON.stringify(value)") != NULL);
    CHECK(strstr(html, "applyTheme(false, true)") != NULL);
    CHECK(strstr(html, "applyTheme(true, true)") != NULL);
    CHECK(strstr(html, "else configSet('theme', value);") != NULL);
    CHECK(strstr(html, "function setTheme(") == NULL);
    CHECK(strstr(html, "class=\"menu-label\" data-menu=\"view\"") != NULL);
    CHECK(strstr(html, "id=\"menu-view\" role=\"menu\" hidden") != NULL);
    CHECK(strstr(html, "id=\"menuLight\" role=\"menuitemradio\"") != NULL);
    CHECK(strstr(html, "id=\"menuDark\" role=\"menuitemradio\"") != NULL);
    CHECK(strstr(html, "function initMenus()") != NULL);
    CHECK(strstr(html, "document.getElementById('menuLight').setAttribute('aria-checked'") != NULL);
    CHECK(strstr(html, "document.getElementById('menuDark').setAttribute('aria-checked'") != NULL);
    CHECK(strstr(html, "theme-toggle") == NULL);
    CHECK(strstr(html, "segLight") == NULL);
    CHECK(strstr(html, "segDark") == NULL);
    CHECK(strstr(html,
          ".icon-button { display: inline-grid; place-items: center;") != NULL);
    CHECK(strstr(html,
          ".btn:active:not(:disabled) { border-color: var(--accent); }") != NULL);
    CHECK(strstr(html, "transform: translateY(1px)") == NULL);
    CHECK(strstr(html,
          ".btn.primary:hover:not(:disabled) { filter: brightness(1.16); }") != NULL);
    CHECK(strstr(html, "id=\"icon-circle-help\"") != NULL);
    CHECK(strstr(html,
          "class=\"icon-button\" id=\"reportBtn\"") != NULL);
    CHECK(strstr(html, ">?</button>") == NULL);
    CHECK(strstr(html,
          "class=\"tl-tab-del icon-button\"") != NULL);
    CHECK(strstr(html,
          "class=\"tl-inner-tab tl-add icon-button\"") != NULL);
    CHECK(strstr(html, "class=\"ev-del icon-button\"") != NULL);
    CHECK(strstr(html,
          "class=\"pf-folder-del icon-button\"") != NULL);
    CHECK(strstr(html, "<span class=\"tl-tab-del\"") == NULL);
    CHECK(strstr(html, "<div class=\"ev-del\"") == NULL);
    CHECK(strstr(html, "<span class=\"pf-folder-del\"") == NULL);
    CHECK(strstr(html,
          "revertBtn.classList.toggle('icon-button', on);") != NULL);
    CHECK(strstr(html,
          "saveBtn.classList.toggle('icon-button', on);") != NULL);
    CHECK(strstr(html, "id=\"camLockChk\"") != NULL);
    CHECK(strstr(html, "class=\"camera\"") == NULL);
    CHECK(strstr(html, ".camera {") == NULL);
    CHECK(strstr(html, "<input type=\"text\" id=\"camX\"") != NULL);
    CHECK(strstr(html, "<input type=\"text\" id=\"camY\"") != NULL);
    CHECK(strstr(html, "<input type=\"text\" id=\"camZ\"") != NULL);
    CHECK(strstr(html,
          "<div class=\"status-group status-main\">") != NULL);
    CHECK(strstr(html, "id=\"connText\"") == NULL);
    CHECK(strstr(html, "(Connected)") == NULL);
    CHECK(strstr(html, "aria-label=\"Connecting\"") != NULL);
    CHECK(strstr(html,
          "bridgeDot.setAttribute('aria-label', 'Connected');") != NULL);
    CHECK(strstr(html,
          "<div class=\"status-group status-camera\"") != NULL);
    CHECK(strstr(html,
          "<div class=\"status-group status-right\">") != NULL);
    CHECK(strstr(html, "aria-label=\"Camera origin controls\"") != NULL);
    CHECK(strstr(html, "aria-label=\"Camera X coordinate\"") != NULL);
    CHECK(strstr(html, "aria-label=\"Camera Y coordinate\"") != NULL);
    CHECK(strstr(html, "aria-label=\"Camera Z coordinate\"") != NULL);
    CHECK(strstr(html, "> Lock position</label>") != NULL);
    CHECK(count_text(html, "id=\"camX\"") == 1);
    CHECK(count_text(html, "id=\"camY\"") == 1);
    CHECK(count_text(html, "id=\"camZ\"") == 1);
    CHECK(strstr(html, "<span id=\"stamp\">Updated: --</span>") != NULL);
    CHECK(strstr(html,
          ".status-camera { gap: 8px; margin-left: 22px; padding-left: 22px; border-left: 1px solid var(--border); }") != NULL);
    CHECK(strstr(html,
          "width: 100%; height: 100%; overflow: hidden;") != NULL);
    CHECK(strstr(html,
          ".app { --split-primary: 40%; display: flex; flex-direction: column; width: 100%; height: 100%; min-width: 0; min-height: 0; overflow: hidden;") != NULL);
    CHECK(strstr(html,
          ".content { position: relative; flex: 1 1 0; display: flex; min-width: 0; min-height: 0; overflow: hidden;") != NULL);
    CHECK(strstr(html, ".editor-col.focus-mode { height: auto; }") != NULL);
    CHECK(strstr(html, "position: absolute; inset: 14px; z-index: 1800;") == NULL);
    CHECK(strstr(html,
          "<div class=\"modal-overlay\" id=\"declFocusModal\" style=\"display:none;\" aria-hidden=\"true\">") != NULL);
    CHECK(strstr(html,
          ".modal.decl-focus-modal { width: min(1040px, 94vw); height: min(720px, 88vh);") != NULL);
    CHECK(strstr(html,
          "document.getElementById('declFocusHost').appendChild(col);") != NULL);
    CHECK(strstr(html,
          "declFocusAnchor.parentNode.insertBefore(col, declFocusAnchor);") != NULL);
    CHECK(strstr(html,
          "if (e.target === declFocusOverlay) setDeclFocus(false);") != NULL);
    CHECK(strstr(html,
          ".prob { display: flex; gap: 7px; padding: 1px 0; border-radius: 3px; cursor: pointer; }") != NULL);
    CHECK(strstr(html, ".prob:hover") == NULL);
    CHECK(strstr(html,
          ".statusbar { display: grid; grid-template-columns: minmax(0, 1fr) auto;") != NULL);
    CHECK(strstr(html,
          "grid-template-columns: repeat(3, minmax(0, 1fr)) auto;") != NULL);
    CHECK(strstr(html,
          ".panel-head { display: flex; align-items: center; gap: 8px; height: 40px; min-height: 40px;") != NULL);
    CHECK(strstr(html,
          ".split-workspace { --pane-inline: 10px; display: flex;") != NULL);
    CHECK(strstr(html,
          ".primary-split > .left-pane { width: var(--split-primary); flex: 0 0 var(--split-primary);") != NULL);
    CHECK(strstr(html,
          ".split-workspace > .panel > .panel-body.list { padding-inline: var(--pane-inline); }") != NULL);
    CHECK(strstr(html,
          ".split-workspace > .panel > .panel-toolbar { position: relative; border-bottom-color: transparent; }") != NULL);
    CHECK(strstr(html,
          "content: \"\"; position: absolute; left: var(--pane-inline); right: var(--pane-inline); bottom: -1px;") != NULL);
    CHECK(strstr(html,
          ".split-workspace > .panel > .panel-body.list { overflow-x: hidden; }") != NULL);
    CHECK(strstr(html,
          "min-width: 0; max-width: 100%; overflow: hidden; text-overflow: ellipsis;") != NULL);
    CHECK(strstr(html,
          ".pane-splitter { position: relative; z-index: 7; width: 1px; flex: 0 0 1px;") != NULL);
    CHECK(strstr(html,
          ".pane-splitter::before { content: \"\"; position: absolute; top: 0; bottom: 0; left: -6px; right: -6px; }") != NULL);
    CHECK(strstr(html,
          ".pane-splitter:hover, .pane-splitter:focus-visible, .pane-splitter.dragging { background: var(--accent); box-shadow: -1px 0 var(--accent), 1px 0 var(--accent); }") != NULL);
    CHECK(count_text(html,
          "class=\"split-workspace primary-split\" data-split-workspace") == 3);
    CHECK(count_text(html, "data-split-variable=\"--split-primary\"") == 3);
    CHECK(count_text(html, "data-split-shared=\"app\"") == 3);
    CHECK(count_text(html, "class=\"pane-splitter\"") == 3);
    CHECK(strstr(html, "data-split-variable=\"--split-rail\"") == NULL);
    CHECK(strstr(html, "data-split-variable=\"--split-inspector\"") == NULL);
    CHECK(strstr(html, "<div class=\"ab-grid\" id=\"abTabHost\"></div>") != NULL);
    CHECK(strstr(html, "<div class=\"ab-grid\" id=\"abModalHost\"></div>") != NULL);
    CHECK(strstr(html, "function initSplitWorkspaces()") != NULL);
    CHECK(strstr(html,
          "document.querySelectorAll('.pane-splitter').forEach(function(splitter)") != NULL);
    CHECK(strstr(html, "splitter.setPointerCapture(e.pointerId);") != NULL);
    CHECK(strstr(html, "e.key === 'ArrowLeft'") != NULL);
    CHECK(strstr(html, "e.key === 'ArrowRight'") != NULL);
    CHECK(strstr(html, "else if (e.key === 'Home')") != NULL);
    CHECK(strstr(html, "else if (e.key === 'End')") != NULL);
    CHECK(strstr(html, "function splitStyleOwner(splitter)") != NULL);
    CHECK(strstr(html, "owner.style.setProperty(variable") != NULL);
    CHECK(strstr(html, "owner.style.removeProperty(variable);") != NULL);
    CHECK(strstr(html, "id=\"newFolderBtn\" title=\"New Folder\" aria-label=\"New Folder\"") != NULL);
    CHECK(strstr(html, "<use href=\"#icon-folder-plus\"></use>") != NULL);
    CHECK(strstr(html, "id=\"prefabCreateBtn\" title=\"Create from Selection\"") != NULL);
    CHECK(strstr(html,
          "class=\"btn primary icon icon-button\" id=\"createTimelineBtn\" disabled title=\"Create New Timeline") != NULL);
    CHECK(strstr(html, "function setCamField(id, v)") != NULL);
    CHECK(strstr(html,
          "setCamField('camX', d.x); setCamField('camY', d.y); setCamField('camZ', d.z);") != NULL);
    CHECK(strstr(html, "function camVal(") != NULL);
    CHECK(strstr(html, "post({cmd:'camLock'") != NULL);
    CHECK(strstr(html, "post({cmd:'camSet'") != NULL);
    CHECK(strstr(html,
          "o.cmd === 'camLock' || o.cmd === 'camSet'") != NULL);

    CHECK(strstr(html, "d.kind === 'configValue'") != NULL);
    CHECK(strstr(html, "d.kind === 'configSetResult'") != NULL);
    CHECK(strstr(html, "d.kind === 'configStatus'") != NULL);
    CHECK(strstr(html, "SH_CONFIG_STATUS") == NULL);

    preview_read = find_text_eol_agnostic(
        html,
        "function previewThemeRead() {\n"
        "    if (!PREVIEW) return null;\n"
        "    try { return localStorage.getItem('sh_theme'); } catch (e) { return null; }\n"
        "  }");
    preview_write = find_text_eol_agnostic(
        html,
        "function previewThemeWrite(value) {\n"
        "    if (!PREVIEW) return;\n"
        "    try { localStorage.setItem('sh_theme', value); }\n"
        "    catch (e) { toast(\"Theme changed for this session, but couldn't be saved.\", 'warn'); }\n"
        "  }");
    read_storage = strstr(html, "localStorage.getItem('sh_theme')");
    write_storage = strstr(html, "localStorage.setItem('sh_theme', value)");
    CHECK(preview_read != NULL);
    CHECK(preview_write != NULL);
    CHECK(read_storage != NULL);
    CHECK(write_storage != NULL);
    if (preview_read && preview_write && read_storage && write_storage) {
        CHECK(preview_read < read_storage);
        CHECK(read_storage < preview_write);
        CHECK(preview_write < write_storage);
    }
    CHECK(count_text(html, "localStorage") == 2);

    free(html);
    if (g_failed) {
        fprintf(stderr, "theme_contract_test: %d failure(s)\n", g_failed);
        return 1;
    }
    puts("theme_contract_test: OK");
    return 0;
}
