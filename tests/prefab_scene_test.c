#include "prefab_scene.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char *scene = NULL; size_t length = 0; int count = 0;
    const char *entity = "[{\"uniqueId\":0,\"displayName\":\"byte \x97\",\"nested\":{\"entities\":[{}]}},{}]";
    size_t payload = 32u * 1024u * 1024u;
    char *source = malloc(payload + strlen(entity) + 256); assert(source);
    size_t used = (size_t)sprintf(source, "{\"variables\":{\"package\":\"");
    memset(source + used, 'a', payload); used += payload;
    used += (size_t)sprintf(source + used, "\",\"prose\":\"idSnapEntity\"},\"entities\":%s,\"~type\":\"idSnapEntityPrefab\"}", entity);
    assert(sh_prefab_scene(source, used, &scene, &length, &count));
    assert(count == 2 && length < 256 && !strstr(scene, "variables") && strstr(scene, entity));
    assert(strlen(scene) == length); free(scene);
    /* A malformed trailing carrier cannot make a partially visited file valid. */
    source[used - 1] = ',';
    assert(!sh_prefab_scene(source, used, &scene, &length, &count) && !scene && !length && !count);
    free(source);
    const char *bad[] = {"{}", "[]", "{\"entities\":null}", "{\"entities\":[1]}",
        "{\"entities\":[],\"entities\":[]}", "{\"nested\":{\"entities\":[{}]}}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
        assert(!sh_prefab_scene(bad[i], strlen(bad[i]), &scene, &length, &count));
    assert(sh_prefab_scene("{\"entities\":[]}", 15, &scene, &length, &count) && count == 0);
    free(scene); puts("prefab scene tests passed"); return 0;
}
