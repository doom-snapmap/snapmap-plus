#include "prefab_scene.h"
#include "config_json.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct scene_capture {
    const char *entities;
    size_t length;
    int count, object;
} scene_capture;
static int entities_path(const sh_json_path *path)
{
    return path && !path->parent && path->key && path->key_length == 8 && !memcmp(path->key, "entities", 8);
}
static int capture(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    scene_capture *view = context;
    if (!path) view->object = kind == SH_JSON_OBJECT;
    else if (entities_path(path)) {
        if (kind != SH_JSON_ARRAY) return 0;
        view->entities = json; view->length = length;
    } else if (!path->key && entities_path(path->parent)) {
        if (kind != SH_JSON_OBJECT || view->count == INT_MAX) return 0;
        view->count++;
    }
    return 1;
}
int sh_prefab_scene(const char *json, size_t length, char **scene, size_t *scene_length, int *count)
{
    static const char prefix[] = "{\"entities\":";
    scene_capture view = {0};
    if (scene) *scene = NULL;
    if (scene_length) *scene_length = 0;
    if (count) *count = 0;
    if (!scene || !scene_length || !count || !json || length > INT_MAX ||
        !sh_native_json_visit_values(json, length, 128, capture, &view) || !view.object || !view.entities ||
        view.length > (size_t)-1 - sizeof(prefix) - 1) return 0;
    size_t total = sizeof(prefix) - 1 + view.length + 1;
    char *out = malloc(total + 1);
    if (!out) return 0;
    memcpy(out, prefix, sizeof(prefix) - 1);
    memcpy(out + sizeof(prefix) - 1, view.entities, view.length);
    out[total - 1] = '}'; out[total] = 0;
    *scene = out; *scene_length = total; *count = view.count; return 1;
}
