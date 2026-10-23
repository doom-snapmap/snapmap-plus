#include "../src/backend/map_binding_sites.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char json[] = "{\"entities\":[{\"uniqueId\":17,\"entityDef\":{\"inherit\":\"same\",\"state\":{\"edit\":{\"name\":\"\xff\x80\"}}}},"
        "{\"uniqueId\":17,\"entityDef\":{\"inherit\":\"same\"}}],"
        "\"entities/0\":{\"inherit\":\"same\"},\"\":{\"a/b%\\u0000\":[\"same\"]},\"list\":{\"0\":\"same\"}}";
    sh_map_binding_span spans[7] = {0}; char error[256]; size_t n = 0;
    const char *at = json;
    while ((at = strstr(at, "\"same\"")) != NULL) { spans[n++] = (sh_map_binding_span){at, 6}; at += 6; }
    assert(n == 5);
    spans[n++] = (sh_map_binding_span){strstr(json, "\"\xff\x80\""), 4};
    spans[n++] = spans[0];
    assert(sh_map_binding_sites_locate(json, sizeof(json) - 1, spans, n, error, sizeof(error)));
    assert(spans[0].site.kind == SH_PACKAGE_USE_OBJECT && !strcmp(spans[0].site.owner, "entity/0"));
    assert(!strcmp(spans[0].site.reference, "/k:entityDef/k:inherit"));
    assert(!strcmp(spans[1].site.owner, "entity/1"));
    assert(spans[2].site.kind == SH_PACKAGE_USE_WORLD && !strcmp(spans[2].site.owner, "world"));
    assert(!strcmp(spans[2].site.reference, "/k:entities%2f0/k:inherit"));
    assert(!strcmp(spans[3].site.reference, "/k:/k:a%2fb%25%00/i:0"));
    assert(!strcmp(spans[4].site.reference, "/k:list/k:0"));
    assert(!strcmp(spans[5].site.reference, "/k:entityDef/k:state/k:edit/k:name"));
    assert(!strcmp(spans[6].site.owner, spans[0].site.owner));
    sh_map_binding_sites_clear(spans, n);
    spans[0].json++; spans[0].length -= 2;
    assert(!sh_map_binding_sites_locate(json, sizeof(json) - 1, spans, n, error, sizeof(error)));
    for (size_t i = 0; i < n; i++) assert(!spans[i].site.owner && !spans[i].site.reference);
    spans[0] = spans[6];
    assert(!sh_map_binding_sites_locate(json, sizeof(json) - 2, spans, n, error, sizeof(error)));
    assert(sh_map_binding_sites_locate(json, sizeof(json) - 1, NULL, 0, error, sizeof(error)));
    puts("map_binding_sites_test PASS"); return 0;
}
