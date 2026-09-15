#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/decl_entity_class.h"

typedef struct fixture { int view; size_t reads; } fixture;
static sh_decl_source text(const char *value)
{ return (sh_decl_source){value, strlen(value)}; }
static int derives(void *context, const char *child, const char *parent)
{
    const char *classes[] = {"Base", "Child", "Grandchild", "Other"};
    size_t a, b;
    (void)context;
    for (a = 0; a < 4 && strcmp(child, classes[a]); a++) {}
    for (b = 0; b < 4 && strcmp(parent, classes[b]); b++) {}
    if (a == 4 || b == 4) return -1;
    return a == b || (a < 3 && b < 3 && a > b);
}
static int read_parent(void *context, const char *name, sh_decl_source *out)
{
    fixture *f = (fixture *)context;
    const char *value = NULL;
    char temporary[128];
    f->reads++;
    if (!strcmp(name, "base")) value = f->view ? "{ class = \"Child\"; }" : "{ class = \"Base\"; }";
    else if (!strcmp(name, "child")) value = "{ inherit = \"base\"; class = \"Child\"; }";
    else if (!strcmp(name, "empty")) value = "{ edit = {} }";
    else if (!strcmp(name, "bad")) value = "{ inherit = \"base\"; class = \"Other\"; }";
    else if (!strcmp(name, "cycle/a")) value = "{ inherit = \"/CYCLE/B\"; class = \"Child\"; }";
    else if (!strcmp(name, "cycle/b")) value = "{ inherit = \"cycle/a\"; }";
    else if (!strcmp(name, "unreadable")) return -1;
    else if (!strcmp(name, "state/base")) value =
        "{ class = \"Base\"; editorVars = { preview = \"editor-only\"; } edit = { target = \"old\"; nested = { keep = \"kept\"; change = 1; } list = { num = 3; item[0] = \"a\"; item[1] = \"b\"; item[2] = \"c\"; } } }";
    else if (!strcmp(name, "state/shrunk")) value =
        "{ inherit = \"state/base\"; edit = { list = { num = 1; } } }";
    else if (!strncmp(name, "deep/", 5)) {
        unsigned index;
        if (sscanf_s(name + 5, "%u", &index) != 1 || index > 4096) return -1;
        if (!index) value = "{ class = \"Base\"; }";
        else { snprintf(temporary, sizeof(temporary), "{ inherit = \"deep/%u\"; }", index - 1); value = temporary; }
    }
    if (!value) return 0;
    out->text = _strdup(value); out->length = strlen(value); assert(out->text); return 1;
}
static void expect(fixture *f, const char *definition, const char *class_name, const char *diagnostic)
{
    sh_decl_entity_class_source source = {f, read_parent, derives};
    char error[512], *result = sh_decl_entity_class(text(definition), &source, error, sizeof(error));
    if (class_name) { assert(result && !strcmp(result, class_name)); assert(!*error); }
    else {
        if (result || !*error || !strstr(error, diagnostic))
            fprintf(stderr, "definition: %s\nresult: %s\nerror: %s\nexpected: %s\n", definition,
                result ? result : "(none)", error, diagnostic);
        assert(!result && *error && strstr(error, diagnostic));
    }
    free(result);
}
static void test_expanded_state(void)
{
    fixture f = {0};
    sh_decl_entity_class_source source = {&f, read_parent, derives};
    char error[512];
    const char *raw = "{ inherit = \"state/base\"; edit = { target = \"new\"; nested = ! { change = 2; } list = { num = 2; item[1] = \"replacement\"; } } }";
    sh_decl_node *input = sh_decl_tree_parse(text(raw), NULL, 0);
    sh_decl_node *expanded = sh_decl_entity_expanded_state(input, &source, error, sizeof(error));
    const sh_decl_node *edit, *nested, *list;
    assert(expanded && !*error && f.reads == 1);
    assert(!sh_decl_tree_member(expanded, "inherit") && !sh_decl_tree_member(expanded, "editorVars"));
    edit = sh_decl_tree_member(expanded, "edit");
    assert(!strcmp(sh_decl_tree_member(edit, "target")->value, "\"new\""));
    nested = sh_decl_tree_member(edit, "nested");
    assert(nested->reset && sh_decl_tree_member(nested, "keep"));
    assert(!strcmp(sh_decl_tree_member(nested, "change")->value, "2"));
    list = sh_decl_tree_member(edit, "list");
    assert(!strcmp(sh_decl_tree_member(list, "item[0]")->value, "\"a\""));
    assert(!strcmp(sh_decl_tree_member(list, "item[1]")->value, "\"replacement\""));
    assert(!sh_decl_tree_member(list, "item[2]"));
    assert(sh_decl_tree_member(input, "inherit"));
    sh_decl_tree_free(expanded); sh_decl_tree_free(input);

    input = sh_decl_tree_parse(text("{ inherit = \"state/shrunk\"; edit = { list = { num = 3; item[2] = \"new\"; } } }"), NULL, 0);
    expanded = sh_decl_entity_expanded_state(input, &source, error, sizeof(error));
    assert(expanded);
    list = sh_decl_tree_member(sh_decl_tree_member(expanded, "edit"), "list");
    assert(sh_decl_tree_member(list, "item[0]") && !sh_decl_tree_member(list, "item[1]"));
    assert(!strcmp(sh_decl_tree_member(list, "item[2]")->value, "\"new\""));
    sh_decl_tree_free(expanded); sh_decl_tree_free(input);
    {
        const char *bad[] = {
            "{ inherit = \"cycle/a\"; }", "{ inherit = \"unreadable\"; }",
            "{ inherit = \"missing\"; }", "{ inherit = \"state/base\"; expandInheritance = false; }",
            "{ inherit = \"state/base\"; edit = { nested = 1; } }",
            "{ inherit = \"state/base\"; edit = { list = { num = -1; } } }",
            "{ inherit = \"state/base\"; edit = { Target = \"different-case\"; } }"
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            input = sh_decl_tree_parse(text(bad[i]), NULL, 0); assert(input);
            expanded = sh_decl_entity_expanded_state(input, &source, error, sizeof(error));
            assert(!expanded && *error); sh_decl_tree_free(input);
        }
    }
    input = sh_decl_tree_parse(text("{ inherit = \"deep/4096\"; edit = { target = \"last\"; } }"), NULL, 0);
    expanded = sh_decl_entity_expanded_state(input, &source, error, sizeof(error));
    assert(expanded && sh_decl_tree_member(sh_decl_tree_member(expanded, "edit"), "target"));
    sh_decl_tree_free(expanded); sh_decl_tree_free(input);
}

int main(void)
{
    fixture f = {0};
    expect(&f, "{ class = \"Base\"; edit = { value = 1; } }", "Base", NULL);
    assert(!f.reads);
    expect(&f, "{ inherit = \"/BASE\"; }", "Base", NULL);
    expect(&f, "{ inherit = \"child\"; class = \"\"; }", "Child", NULL);
    expect(&f, "{ inherit = \"child\"; class = \"Grandchild\"; }", "Grandchild", NULL);
    expect(&f, "{ inherit = \"child\"; class = \"Base\"; }", NULL, "does not derive");
    expect(&f, "{ inherit = \"bad\"; class = \"Grandchild\"; }", NULL, "does not derive");
    expect(&f, "{ class = \"base\"; }", NULL, "metadata");
    expect(&f, "{ class = \"NoSuchClass\"; }", NULL, "metadata");
    expect(&f, "{ inherit = \"missing\"; class = \"Base\"; }", NULL, "absent");
    expect(&f, "{ inherit = \"unreadable\"; }", NULL, "unavailable");
    expect(&f, "{ inherit = \"empty\"; }", NULL, "no class");
    expect(&f, "{}", NULL, "no class");
    expect(&f, "{ inherit = \"cycle/a\"; class = \"Grandchild\"; }", NULL, "cyclic");
    expect(&f, "{ class = ! \"Base\"; }", NULL, "malformed");
    expect(&f, "{ class = { name = \"Base\"; } }", NULL, "header");
    expect(&f, "{ inherit = \"ba\\se\"; }", NULL, "header");
    expect(&f, "{ class = \"Base\"; class = \"Child\"; }", NULL, "duplicate");
    expect(&f, "{ inherit = \"deep/4096\"; }", "Base", NULL);
    /* Parent contributions must be read from the selected compilation view,
     * never a stale loaded object or a process-global class cache. */
    expect(&f, "{ inherit = \"base\"; }", "Base", NULL);
    f.view = 1;
    expect(&f, "{ inherit = \"base\"; }", "Child", NULL);
    expect(&f, "{ inherit = \"base\"; class = \"Base\"; }", NULL, "does not derive");
    test_expanded_state();
    puts("decl_entity_class_test: PASS"); return 0;
}
