#include "package_consumers.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int keep(void *context, const char *family, const char *name)
{ (void)family; return context ? !strcmp(context, name) : -1; }
int main(void)
{
    char error[512], owner_name[] = "owner";
    sh_package_consumer_target slots[] = {{"second", "native", "cycle"}, {"first", "image", "red"}, {"first", "image", "red"}};
    sh_package_consumer_target reordered[] = {slots[1], slots[0]};
    sh_package_consumer_target cycle[] = {{"return", "font", "owner"}, {"nested", "native", "tail"}};
    sh_package_consumer_target tail[] = {{"back", "native", "cycle"}};
    sh_package_consumer rows[] = {{"font", owner_name, slots, 3}, {"native", "cycle", cycle, 2},
        {"font", "owner", reordered, 2}, {"native", "tail", tail, 1}, {"font", "unused", NULL, 0}};
    sh_package_consumers *view = sh_package_consumers_create(rows, 5, error, sizeof(error));
    assert(view && sh_package_consumers_count(view) == 4);
    memset(owner_name, '?', strlen(owner_name));
    const sh_package_consumer *owner = sh_package_consumers_find(view, "font", "owner");
    assert(owner && owner->count == 2 && owner->targets != slots);
    assert(!strcmp(sh_package_consumer_find(owner, "first")->name, "red"));
    assert(!sh_package_consumer_find(owner, "missing") && !sh_package_consumers_find(view, "material", "owner"));
    assert(!sh_package_consumers_at(view, 4) && !sh_package_consumers_find(view, NULL, "owner"));
    sh_package_consumers *same = sh_package_consumers_join(view, view, error, sizeof(error));
    assert(same && sh_package_consumers_count(same) == 4 && sh_package_consumers_equal(same, view, "font", "owner"));
    assert(sh_package_consumers_equal(NULL, NULL, "font", "absent"));
    assert(!sh_package_consumers_equal(view, view, NULL, "owner") && !sh_package_consumers_equal(view, view, "font", ""));
    sh_package_consumers *filtered = sh_package_consumers_filter(view, keep, "owner", error, sizeof(error));
    assert(filtered && sh_package_consumers_count(filtered) == 3);
    assert(sh_package_consumers_find(filtered, "native", "tail") && !sh_package_consumers_find(filtered, "font", "unused"));
    assert(!sh_package_consumers_filter(view, keep, NULL, error, sizeof(error)) && *error);
    sh_package_consumers *empty = sh_package_consumers_filter(view, keep, "missing", error, sizeof(error));
    assert(empty && !sh_package_consumers_count(empty));
    assert(!sh_package_consumers_equal(view, empty, "font", "unused")); /* Known-empty differs from unknown. */
    sh_package_consumer changed = {"font", "owner", reordered, 1};
    sh_package_consumers *partial = sh_package_consumers_create(&changed, 1, error, sizeof(error)); assert(partial);
    assert(!sh_package_consumers_join(view, partial, error, sizeof(error)) && strstr(error, "different captured"));
    changed.count = 2; reordered[0].name = "green";
    sh_package_consumers *different = sh_package_consumers_create(&changed, 1, error, sizeof(error)); assert(different);
    assert(!sh_package_consumers_equal(view, different, "font", "owner"));
    assert(!sh_package_consumers_join(different, view, error, sizeof(error)));
    reordered[0].slot = "second";
    assert(!sh_package_consumers_create(&changed, 1, error, sizeof(error)));
    changed.targets = NULL; assert(!sh_package_consumers_create(&changed, 1, error, sizeof(error)));
    changed.count = 0; changed.name = ""; assert(!sh_package_consumers_create(&changed, 1, error, sizeof(error)));
    assert(!sh_package_consumers_create(NULL, 1, error, sizeof(error)));
    assert(!sh_package_consumers_create(rows, SIZE_MAX, error, sizeof(error)));
    changed = (sh_package_consumer){"font", "overflow", slots, SIZE_MAX};
    assert(!sh_package_consumers_create(&changed, 1, error, sizeof(error)));
    sh_package_consumers_free(view); sh_package_consumers_free(same);
    assert(!strcmp(sh_package_consumer_find(sh_package_consumers_find(filtered, "font", "owner"), "first")->name, "red"));
    sh_package_consumers_free(filtered); sh_package_consumers_free(empty);
    sh_package_consumers_free(partial); sh_package_consumers_free(different);
    puts("native consumer views: PASS"); return 0;
}
