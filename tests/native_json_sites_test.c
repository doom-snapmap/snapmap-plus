#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config_json.h"

typedef struct probe {
    const char *json;
    size_t length, count, roots, nested, byte_strings, empty_keys, zero_keys;
    int refuse;
} probe;

static int visit(void *context, const sh_json_path *path,
    const char *json, size_t length, sh_json_kind kind)
{
    probe *p = context;
    assert(json >= p->json && length <= p->length - (size_t)(json - p->json));
    p->count++;
    if (p->refuse) return 0;
    if (!path) { p->roots++; assert(kind == SH_JSON_OBJECT); }
    else if (path->key && !path->key_length) p->empty_keys++;
    else if (path->key && path->key_length == 3 && !memcmp(path->key, "a\0b", 3)) p->zero_keys++;
    else if (path->key && !strcmp(path->key, "same")) {
        const sh_json_path *item = path->parent, *array;
        assert(item && !item->key && item->index == p->nested);
        array = item->parent;
        assert(array && array->key && !strcmp(array->key, "items") && !array->parent);
        assert(kind == SH_JSON_STRING && length == 6 && !memcmp(json, "\"same\"", 6));
        p->nested++;
    } else if (path->key && !strcmp(path->key, "bytes")) {
        assert(kind == SH_JSON_STRING && length == 5 && (unsigned char)json[1] == 0xff);
        p->byte_strings++;
    }
    return 1;
}

int main(void)
{
    const char input[] = " {\"items\":[{\"same\":\"same\"},{\"same\":\"same\"}],"
        "\"bytes\":\"\xff\x80z\",\"\":null,\"a\\u0000b\":true,\"esc/a~b\":[0,[]]} ";
    char before[sizeof(input)]; memcpy(before, input, sizeof(input));
    probe p = {input, strlen(input)};
    assert(sh_native_json_visit_values(input, p.length, 16, visit, &p));
    assert(p.roots == 1 && p.nested == 2 && p.byte_strings == 1 &&
        p.empty_keys == 1 && p.zero_keys == 1 && p.count == 12);
    assert(!memcmp(before, input, sizeof(input)));
    p = (probe){input, strlen(input)}; p.refuse = 1;
    assert(!sh_native_json_visit_values(input, p.length, 16, visit, &p) && p.count == 1);
    assert(!sh_native_json_visit_values(input, strlen(input), 2, visit, &p));
    const char duplicate[] = "{\"x\":0,\"x\":1}";
    p = (probe){duplicate, strlen(duplicate)};
    assert(!sh_native_json_visit_values(duplicate, p.length, 16, visit, &p));
    const char incomplete[] = "{\"items\":[{\"same\":\"same\"}]";
    p = (probe){incomplete, strlen(incomplete)};
    assert(!sh_native_json_visit_values(incomplete, p.length, 16, visit, &p));
    assert(!sh_native_json_visit_values(NULL, 0, 16, visit, &p));
    assert(!sh_native_json_visit_values("{}", 2, 16, NULL, &p));
    puts("native JSON site tests passed"); return 0;
}
