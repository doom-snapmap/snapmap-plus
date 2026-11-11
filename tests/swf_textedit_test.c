/* swf_textedit_test.c -- paste into a field replaces the selection, keeps to the field's limit
 * without splitting a character, and says when the limit cut it short. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The real character-boundary cut, without the real clipboard: the test supplies the text. */
#define sh_clipboard_get os_clipboard_get
#define sh_clipboard_set os_clipboard_set
#include "../src/backend/clipboard.c"
#undef sh_clipboard_get
#undef sh_clipboard_set
int sh_clipboard_get(char *out, int cap);
int sh_clipboard_set(const char *text);

#include "../src/backend/swf_textedit.c"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

/* ---- the engine and host surface the module links against */
const sig_entry BACKEND_ENGINE_SIGNATURES[] = { { NULL } };
sig_status sig_resolve_one(const uint8_t *m, const sig_entry *s, sig_result *o) { (void)m; (void)s; (void)o; return SIG_NOT_FOUND; }
void *sh_prepare_detour_sig(const sig_result *r, void *d, size_t n) { (void)r; (void)d; (void)n; return NULL; }
sh_patch_status sh_commit_detour(void *t) { (void)t; return B2_PATCH_OK; }
int sh_uninstall_detour(void *t) { (void)t; return 1; }
int sh_detour_is_installed(void *t) { (void)t; return 0; }
void backend_log(const char *msg) { (void)msg; }

static const char *g_clip;
int sh_clipboard_get(char *out, int cap)
{
    strncpy_s(out, (size_t)cap, g_clip, _TRUNCATE);
    return 1;
}
int sh_clipboard_set(const char *text) { (void)text; return 1; }

static char toast_text[128];
static void fake_toast(sh_iface *self, const char *title, const char *text)
{
    (void)self; (void)title;
    strcpy_s(toast_text, sizeof toast_text, text);
}
static sh_iface_vtbl g_vtbl;
static sh_iface g_iface_obj;
sh_iface *sh_ui_get_iface(void) { return &g_iface_obj; }

/* idStr::operator=: copy length and bytes into the field's own buffer. */
static char field_buf[256];
static void fake_assign(void *dst, const void *src)
{
    int len = *(const int *)((const uint8_t *)src + IDSTR_LEN_OFF);
    const char *data = *(const char *const *)((const uint8_t *)src + IDSTR_DATA_OFF);
    memcpy(field_buf, data, (size_t)len);
    field_buf[len] = '\0';
    *(int *)((uint8_t *)dst + IDSTR_LEN_OFF) = len;
    *(char **)((uint8_t *)dst + IDSTR_DATA_OFF) = field_buf;
}

static uint8_t ti[0x300];
static void field(const char *text, int sel_a, int sel_b, int maxchars)
{
    memset(ti, 0, sizeof ti);
    strcpy_s(field_buf, sizeof field_buf, text);
    *(int *)(ti + TI_TEXT_OFF + IDSTR_LEN_OFF) = (int)strlen(text);
    *(char **)(ti + TI_TEXT_OFF + IDSTR_DATA_OFF) = field_buf;
    *(int *)(ti + TI_SELSTART_OFF) = sel_a;
    *(int *)(ti + TI_SELEND_OFF) = sel_b;
    *(int *)(ti + TI_MAXCHARS_OFF) = maxchars;
}

int main(void)
{
    g_vtbl.toast = fake_toast;
    g_iface_obj.vtbl = &g_vtbl;
    g_idstr_assign = fake_assign;

    /* A paste that fits replaces the selection and leaves the caret after it. */
    field("hello world", 6, 11, 0);
    g_clip = "there";
    CHECK(paste_at_selection(ti) == 1);
    CHECK(strcmp(field_buf, "hello there") == 0);
    CHECK(*(int *)(ti + TI_SELEND_OFF) == 11 && g_cut_limit == 0);

    /* Over the limit: kept to whole characters, and the player is told. "ab" + e-acute (2 bytes)
     * into a 5-byte field holding "xy": three bytes fit, which would split the e-acute. */
    field("xy", 2, 2, 5);
    g_clip = "ab\xC3\xA9";
    toast_text[0] = '\0';
    CHECK(paste_at_selection(ti) == 1);
    CHECK(strcmp(field_buf, "xyab") == 0);
    CHECK(g_cut_limit == 5);
    tell_cut();
    CHECK(strcmp(toast_text, "Pasted text was cut to the field's 5-character limit.") == 0);
    CHECK(g_cut_limit == 0);

    /* A full field takes nothing and still says why. */
    field("abcde", 5, 5, 5);
    g_clip = "z";
    CHECK(paste_at_selection(ti) == 0);
    CHECK(strcmp(field_buf, "abcde") == 0 && g_cut_limit == 5);
    tell_cut();

    /* A single-line field folds line breaks into one space. */
    field("", 0, 0, 0);
    g_clip = "one\r\ntwo\n";
    CHECK(paste_at_selection(ti) == 1);
    CHECK(strcmp(field_buf, "one two") == 0);

    if (failures) { fprintf(stderr, "%d swf text edit check(s) failed\n", failures); return 1; }
    puts("swf text edit tests passed");
    return 0;
}
