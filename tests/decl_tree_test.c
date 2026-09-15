#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/decl_tree.h"

static sh_decl_node *parse(const char *text)
{
    sh_decl_source source = {text, strlen(text)};
    return sh_decl_tree_parse(source, NULL, 0);
}

int main(void)
{
    sh_decl_node *root, *node;
    const char *literal;
    size_t length, i, depth = 12000, payload = 9u * 1024u * 1024u;
    char *text, *cursor;
    const char *bad[] = {"", "{} trailing", "{ a = 1; a = 2; }", "{ edit = { x = 1; }",
        "{ a = (1, 2; }", "{ a = 1); }", "{ a = \"broken; }", "{ a = !1; }", "{ /* broken }",
        "{ a = 1 /* unsupported expression comment */; }", "{ a b; }"};
    root = parse("/* lead */ { editorVars { placeable = true; } edit = ! {"
                 " position = (1, 2, 3); model = \"models/test\"; plain = bare/name;"
                 " empty = \"\"; quoted = \"escaped\\\"name\"; expression = a+b; } } // end");
    assert(root && root->compound);
    node = (sh_decl_node *)sh_decl_tree_member(root, "edit");
    assert(node && node->reset && node->assignment);
    assert(sh_decl_tree_literal(sh_decl_tree_member(node, "model"), &literal, &length));
    assert(length == 11 && !memcmp(literal, "models/test", length));
    assert(sh_decl_tree_literal(sh_decl_tree_member(node, "plain"), &literal, &length));
    assert(length == 9 && !memcmp(literal, "bare/name", length));
    assert(sh_decl_tree_literal(sh_decl_tree_member(node, "empty"), &literal, &length) && !length);
    assert(!sh_decl_tree_literal(sh_decl_tree_member(node, "position"), &literal, &length));
    assert(!sh_decl_tree_literal(sh_decl_tree_member(node, "quoted"), &literal, &length));
    assert(!sh_decl_tree_literal(sh_decl_tree_member(node, "expression"), &literal, &length));
    sh_decl_tree_free(root);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) assert(!parse(bad[i]));
    {
        const char *ordered = "{ nodes = { node = { name = \"first\"; } node = { name = \"second\"; } } }";
        sh_decl_node *again;
        char *written;
        size_t size;
        assert(!parse(ordered));
        root = sh_decl_tree_parse_ordered((sh_decl_source){ordered, strlen(ordered)}, NULL, 0);
        assert(root);
        node = (sh_decl_node *)sh_decl_tree_member(root, "nodes")->children;
        assert(node && node->next && !node->next->next && !strcmp(node->key, node->next->key));
        assert(!strcmp(sh_decl_tree_member(node, "name")->value, "\"first\""));
        assert(!strcmp(sh_decl_tree_member(node->next, "name")->value, "\"second\""));
        written = sh_decl_tree_write(root, &size); assert(written && size == strlen(written));
        assert(!parse(written)); /* Ordered parse never weakens the default contract. */
        again = sh_decl_tree_parse_ordered((sh_decl_source){written, size}, NULL, 0);
        assert(again); sh_decl_tree_free(again); sh_decl_tree_free(root); free(written);
        assert(!sh_decl_tree_parse_ordered((sh_decl_source){"{ a = 1", 7}, NULL, 0));
    }
    {
        sh_decl_source embedded = {"{ a = 1; }\0ignored", 18};
        char error[128];
        assert(!sh_decl_tree_parse(embedded, error, sizeof(error)) && strstr(error, "NUL"));
    }

    /* Deep syntax must not consume the C call stack during parse or cleanup. */
    text = (char *)malloc(depth * 6 + 3); assert(text); cursor = text;
    *cursor++ = '{';
    for (i = 0; i < depth; i++) { memcpy(cursor, "x = {", 5); cursor += 5; }
    for (i = 0; i <= depth; i++) *cursor++ = '}';
    *cursor = 0;
    root = parse(text); assert(root);
    {
        size_t size;
        char *written = sh_decl_tree_write(root, &size);
        sh_decl_node *again;
        assert(written && size == strlen(written));
        again = parse(written); assert(again);
        sh_decl_tree_free(again); free(written);
    }
    sh_decl_tree_free(root); free(text);

    /* Native text is not rejected at an arbitrary 8 MiB boundary. */
    text = (char *)malloc(payload + 32); assert(text);
    memcpy(text, "{ value = \"", 11); memset(text + 11, 'x', payload);
    memcpy(text + 11 + payload, "\"; }", 5);
    root = parse(text); assert(root);
    assert(sh_decl_tree_literal(sh_decl_tree_member(root, "value"), &literal, &length) && length == payload);
    sh_decl_tree_free(root); free(text);

    /* Wide sibling lists use an index to detect duplicates. */
    text = (char *)malloc(4u * 1024u * 1024u); assert(text); cursor = text;
    *cursor++ = '{';
    for (i = 0; i < 140000; i++) cursor += sprintf(cursor, "x%zu=0;", i);
    *cursor++ = '}'; *cursor = 0;
    root = parse(text); assert(root); sh_decl_tree_free(root); free(text);
    puts("decl_tree_test: PASS");
    return 0;
}
