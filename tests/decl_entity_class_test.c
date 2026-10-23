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
    else if (!strcmp(name, "annotated")) value =
        "{ class = \"Base\"; \"Base\" \"\" edit = { \"int\" \"\" target = 1; target = 2; } }";
    else if (!strcmp(name, "bad")) value = "{ inherit = \"base\"; class = \"Other\"; }";
    else if (!strcmp(name, "cycle/a")) value = "{ inherit = \"/CYCLE/B\"; class = \"Child\"; }";
    else if (!strcmp(name, "cycle/b")) value = "{ inherit = \"cycle/a\"; }";
    else if (!strcmp(name, "unref")) value = "{ class = \"NotReflected\"; }";
    else if (!strcmp(name, "unreadable")) return -1;
    /* Ordered native writes (M80): the first stateful layer appends every
     * occurrence; later layers update the first match. */
    else if (!strcmp(name, "ord/root")) value = "{ class = \"Base\"; edit = { x = 1; x = 2; b = { p = 1; } b = { p = 2; }"
        " list = { num = 3; item[0] = 1; item[1] = 2; item[2] = 3; } } }";
    else if (!strcmp(name, "ord/shrink")) value = "{ inherit = \"ord/root\"; edit = { x = 3; b = { q = 1; } list = { num = 1; } } }";
    else if (!strcmp(name, "ord/stateless")) value = "{ class = \"Base\"; }";
    else if (!strcmp(name, "ord/first")) value = "{ inherit = \"ord/stateless\"; edit = { y = 1; y = 2; } }";
    else if (!strcmp(name, "ord/data")) value = "{ class = \"Base\"; data = { y = 1; } }";
    else if (!strcmp(name, "ord/twice")) value = "{ class = \"Base\"; edit = { y = 1; } edit = { z = 2; } }";
    /* An ancestor stating its own expandInheritance false (M97). */
    else if (!strcmp(name, "ord/unexpanded")) value =
        "{ inherit = \"ord/root\"; expandInheritance = false; edit = { x = 7; } }";
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
/* Native Parse class contract (M91): a layer's own class is checked only
 * against its parent's class, through reflected ancestry; an unreflected class
 * that differs fails that walk (declaration invalid). A parentless class is
 * not checked by Parse; unreflected it stays coverage, unreadable environment. */
static int known(void *context, const char *name)
{
    (void)context;
    if (!strcmp(name, "Unreadable")) return -1;
    return !strcmp(name, "Base") || !strcmp(name, "Child") || !strcmp(name, "Grandchild") || !strcmp(name, "Other");
}
static void expect_kind(const char *definition, sh_decl_entity_merge_failure want, const char *diagnostic)
{
    fixture f = {0};
    sh_decl_entity_class_source source = {&f, read_parent, derives, known};
    sh_decl_entity_merge_failure failure = SH_DECL_ENTITY_MERGE_DECIDED;
    char error[512] = "";
    sh_decl_node *tree = sh_decl_tree_parse_ordered(text(definition), error, sizeof(error));
    char *result;
    assert(tree);
    result = sh_decl_entity_tree_class_classified(tree, &source, &failure, error, sizeof(error));
    if (result || failure != want || !strstr(error, diagnostic))
        fprintf(stderr, "definition: %s; failure: %d (want %d); error: %s\n", definition, (int)failure, (int)want, error);
    assert(!result && failure == want && strstr(error, diagnostic));
    sh_decl_tree_free(tree);
}
static void test_native_parse_class_contract(void)
{
    expect_kind("{ inherit = \"child\"; class = \"Other\"; }", SH_DECL_ENTITY_MERGE_MALFORMED, "Parse rejects");
    expect_kind("{ inherit = \"child\"; class = \"Base\"; }", SH_DECL_ENTITY_MERGE_MALFORMED, "does not derive");
    expect_kind("{ inherit = \"base\"; class = \"NotReflected\"; }", SH_DECL_ENTITY_MERGE_MALFORMED, "Parse rejects");
    expect_kind("{ inherit = \"base\"; class = \"Unreadable\"; }", SH_DECL_ENTITY_MERGE_UNAVAILABLE, "cannot verify");
    expect_kind("{ class = \"NotReflected\"; }", SH_DECL_ENTITY_MERGE_COVERAGE, "not a reflected native class");
    expect_kind("{ class = \"Unreadable\"; }", SH_DECL_ENTITY_MERGE_UNAVAILABLE, "metadata is unavailable");
    /* Under an unreflected root Parse still checks derived layers: an empty or
     * equal class inherits it (coverage); a differing class fails the walk. */
    expect_kind("{ inherit = \"unref\"; }", SH_DECL_ENTITY_MERGE_COVERAGE, "not a reflected native class");
    expect_kind("{ inherit = \"unref\"; class = \"NotReflected\"; }", SH_DECL_ENTITY_MERGE_COVERAGE, "not a reflected");
    expect_kind("{ inherit = \"unref\"; class = \"Base\"; }", SH_DECL_ENTITY_MERGE_MALFORMED, "Parse rejects");
    expect_kind("{ inherit = \"unref\"; class = \"Unreadable\"; }", SH_DECL_ENTITY_MERGE_UNAVAILABLE, "cannot verify");
    /* Parent lookup precedes the class check, as in Parse. */
    expect_kind("{ inherit = \"missing\"; class = \"NotReflected\"; }", SH_DECL_ENTITY_MERGE_MISSING, "absent");
    expect_kind("{ inherit = \"unreadable\"; class = \"Other\"; }", SH_DECL_ENTITY_MERGE_UNAVAILABLE, "unavailable");
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
            "{ inherit = \"missing\"; }", "{ inherit = \"state/base\"; expandInheritance = False; }",
            "{ inherit = \"state/base\"; expandInheritance = \"false\"; }",
            "{ inherit = \"state/base\"; expandInheritance = yes; }",
            "{ inherit = \"state/base\"; expandInheritance = false; edit = { list = { num = 1; item[1] = 2; } } }",
            "{ inherit = \"state/base\"; edit = { nested = 1; } }",
            "{ inherit = \"state/base\"; edit = { list = { num = -2147483649; } } }",
            "{ inherit = \"state/base\"; edit = { list = { num = 1; item[1] = \"outside\"; } } }",
            "{ inherit = \"state/base\"; edit = { list = { item[3] = \"outside\"; } } }",
            "{ inherit = \"state/base\"; edit = { Target = \"different-case\"; } }"
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            input = sh_decl_tree_parse(text(bad[i]), NULL, 0); assert(input);
            expanded = sh_decl_entity_expanded_state(input, &source, error, sizeof(error));
            assert(!expanded && *error); sh_decl_tree_free(input);
        }
    }
    input = sh_decl_tree_parse(text("{ inherit = \"state/base\"; edit = { list = { num = -1; } } }"), NULL, 0);
    expanded = sh_decl_entity_expanded_state(input, &source, error, sizeof(error));
    assert(expanded);
    list = sh_decl_tree_member(sh_decl_tree_member(expanded, "edit"), "list");
    assert(sh_decl_tree_member(list, "num") && !sh_decl_tree_member(list, "item[0]") &&
        !sh_decl_tree_member(list, "item[1]"));
    sh_decl_tree_free(expanded); sh_decl_tree_free(input);
    input = sh_decl_tree_parse(text("{ inherit = \"deep/4096\"; edit = { target = \"last\"; } }"), NULL, 0);
    expanded = sh_decl_entity_expanded_state(input, &source, error, sizeof(error));
    assert(expanded && sh_decl_tree_member(sh_decl_tree_member(expanded, "edit"), "target"));
    sh_decl_tree_free(expanded); sh_decl_tree_free(input);
}

static void ord_write(const sh_decl_node *node, char *out, size_t capacity)
{
    for (const sh_decl_node *child = node->children; child; child = child->next) {
        size_t at = strlen(out);
        if (child->compound) {
            snprintf(out + at, capacity - at, "%s{", child->key);
            ord_write(child, out, capacity);
            at = strlen(out); snprintf(out + at, capacity - at, "}");
        } else snprintf(out + at, capacity - at, "%s=%s;", child->key, child->value ? child->value : "");
    }
}
static void ord_expect(const char *definition, const char *want, const char *diagnostic)
{
    fixture f = {0};
    sh_decl_entity_class_source source = {&f, read_parent, derives};
    char error[512] = "", written[512] = "";
    sh_decl_node *input = sh_decl_tree_parse_ordered(text(definition), NULL, 0);
    sh_decl_node *expanded = input ? sh_decl_entity_expanded_state(input, &source, error, sizeof(error)) : NULL;
    const sh_decl_node *edit = expanded ? sh_decl_tree_member(expanded, "edit") : NULL;
    if (edit) ord_write(edit, written, sizeof(written));
    if (want ? !edit || strcmp(written, want) : expanded || !strstr(error, diagnostic))
        fprintf(stderr, "ordered: %s\ngot: %s (%s)\nwant: %s\n", definition, written, error, want ? want : diagnostic);
    assert(input && (want ? edit && !strcmp(written, want) : !expanded && strstr(error, diagnostic)));
    sh_decl_tree_free(expanded); sh_decl_tree_free(input);
}
static void test_ordered_writes(void)
{
    /* Root repeats survive; each later occurrence updates the first match,
     * recursively for blocks; per-layer num shrinks then regrows; a new block
     * collapses its own repeats; a repeated block applies after the earlier. */
    ord_expect("{ inherit = \"ord/shrink\"; edit = { x = 4; x = 5; list = { num = 2; item[1] = 5; item[1] = 6; }"
        " n = { g = 1; g = 2; } m = { g = 1; } m = { g = 2; } } }",
        "x=5;x=2;b{p=1;q=1;}b{p=2;}list{num=2;item[0]=1;item[1]=6;}n{g=2;}m{g=2;}", NULL);
    /* The flag stays clear until the tree has a node (VK 0x141a65560). */
    ord_expect("{ inherit = \"ord/first\"; edit = { y = 3; } }", "y=3;y=2;", NULL);
    /* Repeated num (M81): the post-pass (VK 0x141a65fa0 / GL 0x141a58470) and
     * the idList prescan read a block's first num. Merged into an existing
     * list, each num updates that first one, so the layer's last counts;
     * appended by a first stateful layer, its first counts. */
    ord_expect("{ inherit = \"ord/root\"; edit = { list = { num = 1; num = 3; item[2] = 9; } } }",
        "x=1;x=2;b{p=1;}b{p=2;}list{num=3;item[0]=1;item[1]=2;item[2]=9;}", NULL);
    ord_expect("{ inherit = \"ord/stateless\"; edit = { list = { num = 2; num = 1; item[0] = 4; item[1] = 5; } } }",
        "list{num=2;num=1;item[0]=4;item[1]=5;}", NULL);
    /* Repeated top-level statements (M85): SetEntityEditState reads only the
     * prepared text's first statement. A later layer's edit blocks each update
     * that first block; a first stateful layer appends both and only its first
     * is read, so a later layer never reaches the appended second one. */
    ord_expect("{ inherit = \"ord/root\"; edit = { x = 1; } edit = { x = 2; } }",
        "x=2;x=2;b{p=1;}b{p=2;}list{num=3;item[0]=1;item[1]=2;item[2]=3;}", NULL);
    ord_expect("{ inherit = \"ord/stateless\"; edit = { y = 1; } edit = { y = 2; } }", "y=1;", NULL);
    ord_expect("{ inherit = \"ord/twice\"; edit = { z = 3; } }", "y=1;z=3;", NULL);
    /* An unconsumed header is state text: harmless after a stateful parent,
     * but the first prepared statement of a first stateful layer. */
    ord_expect("{ inherit = \"ord/root\"; inherit = \"x\"; edit = { x = 9; } }",
        "x=9;x=2;b{p=1;}b{p=2;}list{num=3;item[0]=1;item[1]=2;item[2]=3;}", NULL);
    ord_expect("{ inherit = \"ord/stateless\"; inherit = \"x\"; edit = { y = 1; } }", NULL,
        "first prepared state statement is not a block assignment");
    ord_expect("{ inherit = \"ord/stateless\"; edit { y = 1; } }", NULL, "not a block assignment");
    /* The header reader checks no name: a differently named first block is
     * the state, and a later layer reaches it only under that name. */
    ord_expect("{ inherit = \"ord/stateless\"; data = { y = 1; } edit = { y = 2; } }", "y=1;", NULL);
    ord_expect("{ inherit = \"ord/data\"; data = { y = 3; } edit = { y = 4; } }", "y=3;", NULL);
    /* expandInheritance is positional too: out of order it is state text. */
    ord_expect("{ inherit = \"ord/stateless\"; edit = { y = 1; } expandInheritance = false; }", "y=1;", NULL);
    /* Own mode (M97, ParseBool VK 0x141a14340 / GL 0x141a06580): false or 0
     * prepares the own state text alone, repeats kept and only its first
     * statement read; true or a nonzero integer merges the chain. */
    ord_expect("{ inherit = \"ord/root\"; expandInheritance = false; edit = { x = 7; x = 8; } edit = { z = 1; } }",
        "x=7;x=8;", NULL);
    ord_expect("{ inherit = \"ord/root\"; expandInheritance = 0; data = { y = 1; } }", "y=1;", NULL);
    ord_expect("{ inherit = \"ord/root\"; expandInheritance = 2; edit = { x = 7; } }",
        "x=7;x=2;b{p=1;}b{p=2;}list{num=3;item[0]=1;item[1]=2;item[2]=3;}", NULL);
    ord_expect("{ inherit = \"ord/root\"; expandInheritance = true; edit = { x = 7; } }",
        "x=7;x=2;b{p=1;}b{p=2;}list{num=3;item[0]=1;item[1]=2;item[2]=3;}", NULL);
    ord_expect("{ inherit = \"ord/root\"; expandInheritance = false; edit = { x = 7; } }", "x=7;", NULL);
    /* An ancestor's flag is not the leaf's policy: the leaf merges every
     * layer's own state, the unexpanded ancestor's included. */
    ord_expect("{ inherit = \"ord/unexpanded\"; edit = { y = 1; } }",
        "x=7;x=2;b{p=1;}b{p=2;}list{num=3;item[0]=1;item[1]=2;item[2]=3;}y=1;", NULL);
    /* No parent is read for the own text, so an absent one leaves the state
     * decided; the class walk still refuses it (main). */
    ord_expect("{ inherit = \"missing\"; expandInheritance = false; edit = { y = 1; } }", "y=1;", NULL);
    ord_expect("{ inherit = \"ord/root\"; expandInheritance = false; x = 1; }", NULL, "not a block assignment");
    /* The value is one native ParseBool token followed by ";" (M97
     * continuation). A number's 32-bit integer decides it at any length:
     * radix before exponent, wrapping integers, float32 truncation; another
     * name may be a define (coverage), while a sign, second dot, float
     * exception, quoted string or trailing number is a native Parse error. */
    {
        static const char merged[] = "x=7;x=2;b{p=1;}b{p=2;}list{num=3;item[0]=1;item[1]=2;item[2]=3;}";
        static const struct { const char *value; char mode; } modes[] = {
            {"4294967297", 'm'}, {"2147483648", 'm'}, {"0x100000001", 'm'}, {"0b10", 'm'}, {"010", 'm'},
            {"1u", 'm'}, {"1UL", 'm'}, {"0.99999999", 'm'}, {"1.5f", 'm'}, {"1e10", 'm'}, {".5e1", 'm'},
            {"1e", 'm'}, {"1.2.3e0", 'm'},
            {"4294967296", 'o'}, {"0x100000000", 'o'}, {"00", 'o'}, {"0x", 'o'}, {"0b", 'o'},
            {"0.5", 'o'}, {"0.5L", 'o'}, {"0.9999999", 'o'}, {"1e-5", 'o'},
            {"-1", 'e'}, {"+1", 'e'}, {"1.2.3", 'e'}, {"1.#INF", 'e'}, {"08", 'e'}, {"1 2", 'e'},
            {"0x1.5", 'e'}, {"\"false\"", 'e'},
            {"False", 'c'}, {"yes", 'c'}, {"1E5", 'c'}, {"1abc", 'c'}, {"0e5", 'c'}, {"true x", 'c'}
        };
        for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
            char definition[160];
            snprintf(definition, sizeof(definition),
                "{ inherit = \"ord/root\"; expandInheritance = %s; edit = { x = 7; } }", modes[i].value);
            ord_expect(definition, modes[i].mode == 'm' ? merged : modes[i].mode == 'o' ? "x=7;" : NULL,
                modes[i].mode == 'e' ? "not a native boolean" : "native preprocessor");
        }
    }
}

/* Merge mode (M85): update once an ancestor has state, append before. */
static int merges_of(const char *definition)
{
    fixture f = {0};
    sh_decl_entity_class_source source = {&f, read_parent, derives};
    char error[512] = "";
    sh_decl_node *tree = sh_decl_tree_parse_ordered(text(definition), NULL, 0);
    int result = tree ? sh_decl_entity_merges(tree, &source, NULL, error, sizeof(error)) : -2;
    sh_decl_tree_free(tree); return result;
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
    expect(&f, "{ inherit = \"ba\\se\"; }", NULL, "ba/se");
    expect(&f, "{ inherit = \"state\\base\"; }", "Base", NULL);
    /* Headers are positional (M85): a repeated or out-of-order header is
     * state text, and a differently cased one stops header matching there. */
    expect(&f, "{ class = \"Base\"; class = \"Child\"; }", "Base", NULL);
    expect(&f, "{ class = \"Base\"; inherit = \"child\"; }", "Base", NULL);
    expect(&f, "{ Inherit = \"child\"; class = \"Base\"; }", NULL, "no class");
    expect(&f, "{ inherit = \"annotated\"; edit = { target = 3; target = 4; } }", "Base", NULL);
    expect(&f, "{ inherit = \"base\"; inherit = \"child\"; }", "Base", NULL);
    expect(&f, "{ inherit = \"deep/4096\"; }", "Base", NULL);
    /* Parent contributions must be read from the selected compilation view,
     * never a stale loaded object or a process-global class cache. */
    expect(&f, "{ inherit = \"base\"; }", "Base", NULL);
    f.view = 1;
    expect(&f, "{ inherit = \"base\"; }", "Child", NULL);
    expect(&f, "{ inherit = \"base\"; class = \"Base\"; }", NULL, "does not derive");
    test_native_parse_class_contract();
    test_expanded_state();
    test_ordered_writes();
    assert(merges_of("{ class = \"Base\"; edit = { y = 1; y = 2; } }") == 0);
    /* An unexpanded definition merges nothing; its first statement is state. */
    assert(merges_of("{ inherit = \"ord/root\"; edit = { x = 1; } }") == 1);
    assert(merges_of("{ inherit = \"ord/root\"; expandInheritance = false; edit = { x = 1; } }") == 0);
    assert(merges_of("{ inherit = \"missing\"; expandInheritance = false; edit = { x = 1; } }") == 0);
    assert(merges_of("{ inherit = \"ord/root\"; expandInheritance = False; edit = { x = 1; } }") == -1);
    assert(merges_of("{ inherit = \"ord/root\"; expandInheritance = 4294967296; edit = { x = 1; } }") == 0);
    {
        fixture f = {0};
        sh_decl_entity_class_source source = {&f, read_parent, derives};
        const char *refused[] = {"-1", "False"};
        sh_decl_entity_merge_failure want[] = {SH_DECL_ENTITY_MERGE_MALFORMED, SH_DECL_ENTITY_MERGE_COVERAGE};
        for (size_t i = 0; i < 2; i++) {
            char definition[160], error[256] = "";
            sh_decl_entity_merge_failure failure = SH_DECL_ENTITY_MERGE_DECIDED;
            snprintf(definition, sizeof(definition),
                "{ inherit = \"ord/root\"; expandInheritance = %s; edit = { x = 1; } }", refused[i]);
            sh_decl_node *tree = sh_decl_tree_parse_ordered(text(definition), NULL, 0);
            assert(tree && sh_decl_entity_merges_classified(tree, &source, NULL, &failure, error, sizeof(error)) == -1);
            assert(failure == want[i]);
            sh_decl_tree_free(tree);
        }
    }
    assert(merges_of("{ inherit = \"ord/unexpanded\"; edit = { x = 1; } }") == 1);
    {
        int update = -1;
        sh_decl_node *tree = sh_decl_tree_parse_ordered(text(
            "{ inherit = \"ord/root\"; expandInheritance = false; data = { y = 1; } edit = { y = 2; } }"), NULL, 0);
        const sh_decl_node *own = sh_decl_entity_state_statement(tree, NULL, &update);
        assert(own && !strcmp(own->key, "data") && update == 0);
        sh_decl_tree_free(tree);
        tree = sh_decl_tree_parse_ordered(text("{ inherit = \"ord/root\"; data = { y = 1; } edit = { y = 2; } }"), NULL, 0);
        own = sh_decl_entity_state_statement(tree, NULL, &update);
        assert(own && !strcmp(own->key, "edit") && update == 1);
        sh_decl_tree_free(tree);
    }
    /* The class still comes from the chain of an unexpanded definition. */
    expect(&f, "{ inherit = \"child\"; expandInheritance = false; }", "Child", NULL);
    expect(&f, "{ inherit = \"missing\"; expandInheritance = false; }", NULL, "absent");
    assert(merges_of("{ inherit = \"ord/stateless\"; edit = { y = 1; } }") == 0);
    assert(merges_of("{ inherit = \"ord/first\"; edit = { y = 1; } }") == 1);
    assert(merges_of("{ inherit = \"ord/root\"; edit = { } }") == 1);
    assert(merges_of("{ inherit = \"missing\"; edit = { } }") == -1);
    puts("decl_entity_class_test: PASS"); return 0;
}
