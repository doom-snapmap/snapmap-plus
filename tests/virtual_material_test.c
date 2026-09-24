#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "virtual_material.h"

static sh_decl_source source(const char *text)
{ return (sh_decl_source){text, strlen(text)}; }

int main(void)
{
    static const char input[] =
        "26 // native format\r\n3\r\n// arbitrary whitespace and negative checksums\n"
        "64 128 32 64 0 2 -2147483648 \"Stone\\B\"\n"
        "0 0 64 64 1 0 2147483647 \"stone/a\"\n"
        "64 128 32 64 0 2 -2147483648 \"stone/b\"\n";
    static const char *bad[] = {
        "", "25 0", "26 -1", "26 2147483647", "26 0 garbage",
        "26 1 0 0 1 1 0 0", "26 1 0 0 1 1 0 0 2147483648 \"a\"",
        "26 1 0 0 1 1 0 0 -2147483649 \"a\"",
        "26 1 0 0 1 1 0 0 1.5 \"a\"", "26 1 0 0 1 1 0 0 0 ;",
        "26 1 0 0 1 1 0 0 0 \"\"", "26 1 -1 0 1 1 0 0 0 \"a\"",
        "26 1 0 0 0 1 0 0 0 \"a\"", "26 1 2147483647 0 1 1 0 0 0 \"a\"",
        "26 2 0 0 1 1 0 0 0 \"a\" 1 0 1 1 0 0 0 \"A\"",
        "26 1 0 0 1 1 0 0 0 \"bad\nname\"",
        "26 1 0 0 1 1 0 0 0 \"bad\xe9\""
    };
    sh_virtual_material_table table = {0}, roundtrip = {0};
    sh_virtual_material_binding bindings[3], reverse[3];
    sh_virtual_material_record altered;
    const sh_virtual_material_record *a, *b;
    char error[256], *text, *other;
    size_t length, other_length;
    float mapping[4];
    assert(sh_virtual_material_read(source(input), &table, error, sizeof(error)));
    assert(table.count == 2);
    a = sh_virtual_material_find(&table, "STONE\\A");
    b = sh_virtual_material_find(&table, "stone/b");
    assert(a && b && b->checksum == INT32_MIN && a->checksum == INT32_MAX);
    assert(!sh_virtual_material_find(&table, "missing"));
    assert(sh_virtual_material_mapping(b, 256, mapping));
    assert(mapping[0] == 0.125f && mapping[1] == 0.25f && mapping[2] == 0.25f && mapping[3] == 0.5f);
    assert(!sh_virtual_material_mapping(b, 128, mapping));
    assert(!mapping[0] && !mapping[1] && !mapping[2] && !mapping[3]);
    assert(!sh_virtual_material_mapping(b, 0, mapping));
    assert(!sh_virtual_material_mapping(NULL, 256, mapping));

    /* Both variants share exactly the same atlas region. Each gets an own row;
     * a duplicate of one variant does not add a third native identity. */
    bindings[0] = (sh_virtual_material_binding){b, "variants/green"};
    bindings[1] = (sh_virtual_material_binding){b, "variants/red"};
    bindings[2] = (sh_virtual_material_binding){b, "VARIANTS\\RED"};
    text = sh_virtual_material_project(bindings, 3, &length, error, sizeof(error));
    assert(text && length == strlen(text));
    assert(sh_virtual_material_read((sh_decl_source){text, length}, &roundtrip, error, sizeof(error)));
    assert(roundtrip.count == 2 && sh_virtual_material_find(&roundtrip, "variants/red")->x == b->x);
    for (size_t i = 0; i < 3; i++) reverse[i] = bindings[2 - i];
    other = sh_virtual_material_project(reverse, 3, &other_length, error, sizeof(error));
    assert(other && other_length == length && !memcmp(other, text, length));
    free(text); free(other);

    altered = *b; altered.x++;
    bindings[2].record = &altered;
    length = 999;
    assert(!sh_virtual_material_project(bindings, 3, &length, error, sizeof(error)) && !length);
    assert(strstr(error, "different atlas records"));
    bindings[2].record = b; bindings[2].name = "bad\"name";
    assert(!sh_virtual_material_project(bindings, 3, &length, error, sizeof(error)));
    assert(!sh_virtual_material_project(NULL, 1, &length, error, sizeof(error)));
    text = sh_virtual_material_project(NULL, 0, &length, error, sizeof(error));
    assert(text && !strcmp(text, "26\n0\n")); free(text);
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        assert(!sh_virtual_material_read(source(bad[i]), &table, error, sizeof(error)));
        assert(!table.records && !table.count && error[0]);
    }
    assert(sh_virtual_material_read(source("26 0"), &table, error, sizeof(error)));
    assert(!table.count);
    assert(sh_virtual_material_read(source("26 1 0 0 1 1 0 0 0 bare/name"), &table, error, sizeof(error)));
    assert(sh_virtual_material_find(&table, "bare/name"));
    sh_virtual_material_free(&table); sh_virtual_material_free(&roundtrip);
    puts("virtual material tests passed"); return 0;
}
