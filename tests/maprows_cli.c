/* maprows_cli <map.json> -- reads a map file's Snapmap+ mod rows the way a load does (sh_map_old_shape first, then
 * sh_modmanifest_read on the member's "mods" part), for the research benches' map writers (snaphak-re
 * tools/snapmap_contract/maprows.py). Output: "shape <none|variables|rows>", "rows <read> rejected <n>", then per row
 * "row <salt> <local|pinned> names <count> choices <0|1> attached <0|1>". Exit 0 when the map would load with every
 * row read, 1 when it would not, 2 on a usage or file error. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/backend/map_carrier.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modserve.h"

/* The serving layer a load activates rows with: a read needs none of it (modmanifest_test has the same doubles). */
void backend_log(const char *line) { (void)line; }
int sh_modserve_active(void) { return 0; }
int sh_modserve_spawn_pins(void (*fn)(void *context, const char *salt, const char *slot), void *context)
{ (void)fn; (void)context; return 0; }
int sh_modserve_scope_set_choices(const char *const *slots, const char *const *keys, int n, int recompute)
{ (void)slots; (void)keys; (void)n; (void)recompute; return 0; }
int sh_modserve_salt_installed(const char *salt) { (void)salt; return 0; }
int sh_modserve_row_build(const struct sh_modmanifest_row *row, sh_modserve_bundle_ref *out)
{ (void)row; (void)out; return 0; }
void sh_modserve_bundle_ref_free(sh_modserve_bundle_ref *ref) { (void)ref; }
size_t modserve_bundles_for_names_visit(const char *const *names, size_t count, sh_modserve_closure_fn visit,
                                        void *context)
{ (void)names; (void)count; (void)visit; (void)context; return 0; }
int modserve_activate_bundles(const char *const *bundle_hashes, int n) { (void)bundle_hashes; (void)n; return 0; }

int main(int argc, char **argv)
{
    FILE *f;
    char *text;
    long len;
    int shape, ok;
    unsigned rejected = 0;
    size_t i, read;
    sh_modmanifest_list list = { 0 };
    if (argc != 2) {
        fprintf(stderr, "usage: maprows_cli <map.json>\n");
        return 2;
    }
    if (fopen_s(&f, argv[1], "rb") || !f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char *)malloc((size_t)len + 1);
    if (!text || fread(text, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(text); return 2; }
    fclose(f);
    text[len] = 0;
    shape = sh_map_old_shape(text, (size_t)len);
    printf("shape %s\n", shape == SH_MAP_OLD_NONE ? "none" : shape == SH_MAP_OLD_ROWS ? "rows" : "variables");
    read = shape == SH_MAP_OLD_NONE ? sh_modmanifest_read(text, (size_t)len, &list, &rejected) : 0;
    printf("rows %u rejected %u\n", (unsigned)read, rejected);
    for (i = 0; i < list.count; i++) {
        const sh_modmanifest_row *r = &list.rows[i];
        printf("row %s %s names %u choices %d attached %d\n", r->salt, r->pinned ? "pinned" : "local",
               (unsigned)r->name_count, r->choices != NULL, r->attached);
    }
    ok = shape == SH_MAP_OLD_NONE && rejected == 0;
    sh_modmanifest_list_free(&list);
    free(text);
    return ok ? 0 : 1;
}
