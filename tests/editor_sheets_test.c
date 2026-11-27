#include "../src/backend/editor_sheets.h"
#include "../src/backend/editor_panel_schema.h"
#include <assert.h>
#include <stdio.h>

static const char base[] =
    "{edit={category=3; propertySheets={num=2;"
    "item[0]={titleTag=\"#str_native\"; properties={num=2;"
    "item[0]={path=\"displayName\"; inspector=\"text\"; keep=14;}"
    "item[1]={path=\"flag\"; inspector=\"bool\"; extra={num=0;}}}}"
    "item[1]={properties={num=1; item[0]={path=\"colour\"; inspector=\"color\";}}}}}}";

static void exercise(void)
{
    char why[100], *written;
    size_t n;
    sh_dt *parent = sh_dt_parse(base, strlen(base), why, sizeof why);
    sh_dt *tree = sh_dt_parse("{edit={test=1;}}", strlen("{edit={test=1;}}"), why, sizeof why);
    sh_editor_sheet_source src[] = {
        {"aaaaaaaa","{\"shared/general\":{\"title\":\"details\",\"before\":\"native/displayName\","
                   "\"properties\":{\"flag\":{\"before\":\"newValue\"},\"newValue\":{\"inspector\":\"bool\"}}}}"},
        {"bbbbbbbb","{\"native/colour\":{\"properties\":{\"flag\":{\"description\":\"other_help\"}}},"
                    "\"shared/general\":{\"title\":\"loses\"}}"}
    };
    written = sh_editor_sheet_ids(parent);
    assert(written && !strcmp(written, "[{\"id\":\"native/displayName\",\"properties\":[\"displayName\",\"flag\"]},"
        "{\"id\":\"native/colour\",\"properties\":[\"colour\"]}]"));
    free(written);
    assert(parent && tree && sh_editor_sheets_inherit(tree, parent) == 1);
    sh_dt_free(parent);
    assert(sh_editor_sheets_apply(tree, src, 2) == 1);
    written = sh_dt_text(tree, &n);
    assert(written && !strstr(written, "loses") && strstr(written, "#str_aaaaaaaa_details"));
    assert(strstr(written, "#str_bbbbbbbb_other_help"));
    assert(strstr(written, "\"flag\"") < strstr(written, "\"newValue\""));
    assert(strstr(written, "\"newValue\"") < strstr(written, "\"displayName\""));
    assert(strstr(written, "\"displayName\"") < strstr(written, "\"colour\""));
    assert(strstr(written, "keep = 14") && strstr(written, "extra = {") && strstr(written, "test = 1"));
    assert(!strstr(written, "category = 3"));
    sh_dt_free(tree);
    tree = sh_dt_parse(written, n, why, sizeof why);
    assert(sh_dt_at(sh_dt_root(tree), "edit/propertySheets/item[0]/properties/item[0]/extra"));
    free(written);
    sh_dt_free(tree);
    assert(!sh_editor_sheets_validate("{\"a\":{\"nonsense\":true}}", why, sizeof why) && strstr(why, "nonsense"));
    assert(!sh_editor_sheets_validate("{\"a\":{\"before\":\"x\",\"after\":\"y\"}}", why, sizeof why));
}

static void exercise_lift(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *text, *written;
    sh_dt *tree;
    sh_dt_node *old;
    sh_editor_sheet_source src = {"aaaaaaaa", "{\"shared/general\":{\"before\":\"native/displayName\","
        "\"properties\":{\"noPush\":{},\"displayName\":{\"after\":\"noPush\"}}}}"};
    assert(f && !fseek(f, 0, SEEK_END));
    n = ftell(f);
    rewind(f);
    text = (char *)malloc((size_t)n + 1);
    assert(text && fread(text, 1, n, f) == (size_t)n);
    fclose(f);
    tree = sh_dt_parse(text, n, NULL, 0);
    old = sh_dt_child(sh_dt_at(sh_dt_root(tree), "edit/propertySheets"), "num");
    assert(old && old->val_n == 1 && *old->val == '7');
    assert(sh_editor_sheets_apply(tree, &src, 1) == 1);
    written = sh_dt_text(tree, NULL);
    assert(written);
    sh_dt_free(tree);
    tree = sh_dt_parse(written, strlen(written), NULL, 0);
    old = sh_dt_child(sh_dt_at(sh_dt_root(tree), "edit/propertySheets"), "num");
    assert(old && old->val_n == 1 && *old->val == '8');
    assert(sh_dt_at(sh_dt_root(tree), "edit/propertySheets/item[7]/properties"));
    assert(strstr(written, "\"noPush\"") < strstr(written, "\"displayName\""));
    printf("Lift: seven native sheets retained, two complete bindings moved into a new first sheet.\n");
    sh_dt_free(tree);
    free(written);
    free(text);
}

static int personal_value;
static char *prefs(void)
{
    return _strdup(personal_value ? "{\"aaaaaaaa/move\":true}" : "{}");
}

static void test_conditions(void)
{
    sh_editor_sheet_source s = {"aaaaaaaa","{\"general\":{\"when\":{\"move\":true},\"properties\":{\"flag\":{}}}}",
        "{\"move\":{\"title\":\"move\",\"scope\":\"editor\",\"default\":false}}"};
    char why[100];
    sh_dt *t = sh_dt_parse(base, strlen(base), NULL, 0);
    uint64_t inactive, active;
    assert(sh_editor_sheets_check_conditions(s.json, s.settings, why, sizeof why));
    assert(!sh_editor_sheets_check_conditions(s.json,"{}",why,sizeof why));
    assert(!sh_editor_sheets_check_conditions(s.json,"{\"move\":{\"scope\":\"map\"}}",why,sizeof why));
    sh_editor_sheets_set_preferences(prefs);
    inactive = sh_editor_sheets_preferences_key(&s, 1);
    assert(sh_editor_sheets_apply(t, &s, 1) == 0);
    personal_value = 1;
    active = sh_editor_sheets_preferences_key(&s, 1);
    assert(inactive != active && sh_editor_sheets_apply(t, &s, 1) == 1);
    personal_value = 0;
    assert(sh_editor_sheets_preferences_key(&s, 1) == inactive);
    sh_editor_sheets_set_preferences(NULL);
    sh_dt_free(t);
}

static void test_independent_move(void)
{
    sh_editor_sheet_source s[] = {
        {"aaaaaaaa","{\"native/displayName\":{\"properties\":{\"flag\":{\"title\":\"custom\"}}}}"},
        {"bbbbbbbb","{\"native/colour\":{\"properties\":{\"flag\":{\"after\":\"colour\"}}}}"}
    };
    sh_dt *t = sh_dt_parse(base, strlen(base), NULL, 0);
    char *written;
    assert(sh_editor_sheets_apply(t, s, 2) == 1);
    written = sh_dt_text(t, NULL);
    assert(written && strstr(written, "\"colour\"") < strstr(written, "\"flag\""));
    assert(strstr(written, "#str_aaaaaaaa_custom"));
    free(written);
    sh_dt_free(t);
}

static void test_inherited_item(void)
{
    const char sparse[] = "{edit={propertySheets={item[0]={properties={item[1]={descriptionTag=\"#str_replaced\";}}}}}}";
    sh_dt *parent = sh_dt_parse(base, strlen(base), NULL, 0);
    sh_dt *child = sh_dt_parse(sparse, strlen(sparse), NULL, 0);
    sh_editor_sheet_source src = {"aaaaaaaa","{\"moved\":{\"properties\":{\"flag\":{}}}}"};
    char *written;
    assert(sh_editor_sheets_inherit(child, parent) == 1);
    sh_dt_free(parent);
    assert(sh_editor_sheets_apply(child, &src, 1) == 1);
    written = sh_dt_text(child, NULL);
    assert(written && strstr(written, "\"colour\"") && strstr(written, "\"displayName\""));
    assert(strstr(written, "#str_replaced") && strstr(written, "extra = {") && strstr(written, "\"bool\""));
    free(written);
    sh_dt_free(child);
}

static void exercise_blocking(const char *path)
{
    char input[20000], *written;
    FILE *f = fopen(path, "rb");
    sh_dt *t;
    sh_dt_node *groups, *base_count;
    sh_editor_sheet_source s = {"aaaaaaaa",
        "{\"shared/collision\":{\"title\":\"collision\",\"before\":\"native/displayName\","
        "\"properties\":{\"blockPlayers\":{},\"blockShots\":{\"after\":\"blockPlayers\"},\"blockDemons\":{\"after\":\"blockShots\"}}}}"};
    size_t n;
    assert(f);
    n = fread(input, 1, sizeof input, f);
    assert(n < sizeof input);
    fclose(f);
    t = sh_dt_parse(input, n, NULL, 0);
    assert(t && sh_editor_sheets_apply(t, &s, 1) == 1);
    written = sh_dt_text(t, NULL);
    sh_dt_free(t);
    t = sh_dt_parse(written, strlen(written), NULL, 0);
    groups = sh_dt_at(sh_dt_root(t), "edit/propertySheets");
    assert(groups && sh_dt_at(groups, "item[0]/properties/item[2]") && sh_dt_at(groups, "item[1]/properties/item[5]"));
    base_count = sh_dt_child(sh_dt_at(groups, "item[1]/properties"), "num");
    assert(base_count && base_count->val_n == 1 && *base_count->val == '6');
    assert(strstr(written, "\"blockPlayers\"") < strstr(written, "\"blockShots\""));
    assert(strstr(written, "\"blockShots\"") < strstr(written, "\"blockDemons\""));
    assert(strstr(written, "\"blockDemons\"") < strstr(written, "\"displayName\""));
    printf("Blocking Volume: three collision bindings on a new first sheet, six original bindings on the second.\n");
    sh_dt_free(t);
    free(written);
}

int main(int argc, char **argv)
{
    exercise();
    test_conditions();
    test_inherited_item();
    test_independent_move();
    if (argc >= 2 && argv[1][0]) exercise_lift(argv[1]);
    if (argc >= 3 && argv[2][0]) exercise_blocking(argv[2]);
    puts("editor_sheets_test passed");
    return 0;
}
