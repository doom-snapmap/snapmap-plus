#include "package_consumers.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sh_package_consumers { sh_package_consumer *owners; size_t count; };
static void pc_error(char *error, size_t capacity, const char *reason)
{ if (error && capacity && !*error) snprintf(error, capacity, "%s", reason); }
static int pc_owner_order(const void *a, const void *b)
{
    const sh_package_consumer *x = a, *y = b;
    int order = strcmp(x->family, y->family); return order ? order : strcmp(x->name, y->name);
}
static int pc_target_order(const void *a, const void *b)
{ return strcmp(((const sh_package_consumer_target *)a)->slot, ((const sh_package_consumer_target *)b)->slot); }
static int pc_target_equal(const sh_package_consumer_target *a, const sh_package_consumer_target *b)
{ return !strcmp(a->slot, b->slot) && !strcmp(a->family, b->family) && !strcmp(a->name, b->name); }
static unsigned char pc_native_char(unsigned char c)
{ return c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static int pc_native_equal(const char *canonical, const char *name)
{
    if ((*name == '/' || *name == '\\') && name[1] != '/' && name[1] != '\\') name++;
    while (*canonical && *canonical == pc_native_char((unsigned char)*name)) { canonical++; name++; }
    return !*canonical && !*name;
}
const char *sh_package_consumer_request(const sh_package_consumer *owner,
    const char *family, const char *name)
{
    if (!owner || !family || !name) return NULL;
    size_t n = strlen(family);
    for (size_t i = 0; i < owner->count; i++) {
        const sh_package_consumer_target *target = owner->targets + i;
        if (strncmp(target->slot, "request/", 8) || !pc_native_equal(target->family, family)) continue;
        const char *slot = target->slot + 8;
        size_t at = 0;
        while (at < n && slot[at] && slot[at] == pc_native_char((unsigned char)family[at])) at++;
        if (at == n && slot[at] == '/' && pc_native_equal(slot + at + 1, name)) return target->name;
    }
    return NULL;
}
static int pc_owner_equal(const sh_package_consumer *a, const sh_package_consumer *b)
{
    if (!a || !b) return a == b;
    if (a->count != b->count) return 0;
    for (size_t i = 0; i < a->count; i++) if (!pc_target_equal(a->targets + i, b->targets + i)) return 0;
    return 1;
}
static void pc_target_clear(sh_package_consumer_target *target)
{
    free((char *)target->slot); free((char *)target->family); free((char *)target->name);
    memset(target, 0, sizeof(*target));
}
static void pc_owner_clear(sh_package_consumer *owner)
{
    for (size_t i = 0; i < owner->count; i++) pc_target_clear((sh_package_consumer_target *)owner->targets + i);
    free((void *)owner->targets); free((char *)owner->family); free((char *)owner->name);
    memset(owner, 0, sizeof(*owner));
}
void sh_package_consumers_free(sh_package_consumers *view)
{
    if (!view) return;
    for (size_t i = 0; i < view->count; i++) pc_owner_clear(view->owners + i);
    free(view->owners); free(view);
}
static int pc_owner_copy(sh_package_consumer *out, const sh_package_consumer *input)
{
    if (!input->family || !*input->family || !input->name || !*input->name ||
        (input->count && !input->targets) || input->count > SIZE_MAX / sizeof(*input->targets)) return 0;
    out->family = _strdup(input->family); out->name = _strdup(input->name);
    if (!out->family || !out->name) return 0;
    if (!input->count) return 1;
    sh_package_consumer_target *targets = calloc(input->count, sizeof(*targets));
    if (!targets) return 0;
    out->targets = targets;
    for (size_t i = 0; i < input->count; i++) {
        const sh_package_consumer_target *source = input->targets + i;
        if (!source->slot || !*source->slot || !source->family || !*source->family || !source->name || !*source->name) return 0;
        sh_package_consumer_target *target = targets + out->count++;
        target->slot = _strdup(source->slot); target->family = _strdup(source->family); target->name = _strdup(source->name);
        if (!target->slot || !target->family || !target->name) return 0;
    }
    qsort(targets, input->count, sizeof(*targets), pc_target_order);
    size_t kept = 0;
    for (size_t i = 0; i < input->count; i++) {
        if (kept && !strcmp(targets[kept - 1].slot, targets[i].slot)) {
            if (!pc_target_equal(targets + kept - 1, targets + i)) return 0;
            pc_target_clear(targets + i);
        } else {
            if (kept != i) { targets[kept] = targets[i]; memset(targets + i, 0, sizeof(*targets)); }
            kept++;
        }
    }
    out->count = kept; return 1;
}
sh_package_consumers *sh_package_consumers_create(const sh_package_consumer *owners,
    size_t count, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    sh_package_consumers *view = calloc(1, sizeof(*view));
    if (!view || (count && !owners) || count > SIZE_MAX / sizeof(*owners)) goto failed;
    view->owners = calloc(count ? count : 1, sizeof(*owners));
    if (!view->owners) goto failed;
    for (size_t i = 0; i < count; i++)
        if (!pc_owner_copy(view->owners + view->count++, owners + i)) goto failed;
    qsort(view->owners, count, sizeof(*owners), pc_owner_order);
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (kept && !pc_owner_order(view->owners + kept - 1, view->owners + i)) {
            if (!pc_owner_equal(view->owners + kept - 1, view->owners + i)) {
                pc_error(error, capacity, "one native consumer has different captured dependency sets"); goto failed;
            }
            pc_owner_clear(view->owners + i);
        } else {
            if (kept != i) { view->owners[kept] = view->owners[i]; memset(view->owners + i, 0, sizeof(*owners)); }
            kept++;
        }
    }
    view->count = kept; return view;
failed:
    pc_error(error, capacity, "native consumer bindings are invalid, contradictory or unavailable");
    sh_package_consumers_free(view); return NULL;
}
size_t sh_package_consumers_count(const sh_package_consumers *view) { return view ? view->count : 0; }
const sh_package_consumer *sh_package_consumers_at(const sh_package_consumers *view, size_t index)
{ return view && index < view->count ? view->owners + index : NULL; }
const sh_package_consumer *sh_package_consumers_find(const sh_package_consumers *view, const char *family, const char *name)
{
    sh_package_consumer key = {family, name};
    return view && view->count && family && name ? bsearch(&key, view->owners, view->count, sizeof(key), pc_owner_order) : NULL;
}
const sh_package_consumer_target *sh_package_consumer_find(const sh_package_consumer *owner, const char *slot)
{
    sh_package_consumer_target key = {slot};
    return owner && owner->count && slot ? bsearch(&key, owner->targets, owner->count, sizeof(key), pc_target_order) : NULL;
}
int sh_package_consumers_equal(const sh_package_consumers *a, const sh_package_consumers *b, const char *family, const char *name)
{ return family && *family && name && *name &&
    pc_owner_equal(sh_package_consumers_find(a, family, name), sh_package_consumers_find(b, family, name)); }
sh_package_consumers *sh_package_consumers_join(const sh_package_consumers *a,
    const sh_package_consumers *b, char *error, size_t capacity)
{
    size_t ac = sh_package_consumers_count(a), bc = sh_package_consumers_count(b);
    if (error && capacity) *error = 0;
    if (bc > SIZE_MAX - ac || ac + bc > SIZE_MAX / sizeof(sh_package_consumer)) {
        pc_error(error, capacity, "native consumer catalog exceeds addressable size"); return NULL;
    }
    sh_package_consumer *rows = calloc(ac + bc ? ac + bc : 1, sizeof(*rows));
    if (!rows) { pc_error(error, capacity, "cannot join native consumer catalogs"); return NULL; }
    if (ac) memcpy(rows, a->owners, ac * sizeof(*rows));
    if (bc) memcpy(rows + ac, b->owners, bc * sizeof(*rows));
    sh_package_consumers *out = sh_package_consumers_create(rows, ac + bc, error, capacity); free(rows); return out;
}
sh_package_consumers *sh_package_consumers_filter(const sh_package_consumers *view,
    int (*keep)(void *, const char *, const char *), void *context, char *error, size_t capacity)
{
    size_t count = sh_package_consumers_count(view), queued = 0, visited = 0;
    unsigned char *selected = NULL; size_t *queue = NULL; sh_package_consumer *rows = NULL;
    sh_package_consumers *out = NULL;
    if (error && capacity) *error = 0;
    if (!keep || count > SIZE_MAX / sizeof(*queue) || count > SIZE_MAX / sizeof(*rows)) goto done;
    selected = calloc(count ? count : 1, 1); queue = calloc(count ? count : 1, sizeof(*queue));
    rows = calloc(count ? count : 1, sizeof(*rows));
    if (!selected || !queue || !rows) goto done;
    for (size_t i = 0; i < count; i++) {
        int result = keep(context, view->owners[i].family, view->owners[i].name);
        if (result < 0) goto done;
        if (result) { selected[i] = 1; queue[queued++] = i; }
    }
    while (visited < queued) {
        const sh_package_consumer *owner = view->owners + queue[visited++];
        for (size_t i = 0; i < owner->count; i++) {
            const sh_package_consumer_target *target = owner->targets + i;
            const sh_package_consumer *next = sh_package_consumers_find(view, target->family, target->name);
            if (next) {
                size_t index = (size_t)(next - view->owners);
                if (!selected[index]) { selected[index] = 1; queue[queued++] = index; }
            }
        }
    }
    size_t used = 0;
    for (size_t i = 0; i < count; i++) if (selected[i]) rows[used++] = view->owners[i];
    out = sh_package_consumers_create(rows, used, error, capacity);
done:
    free(selected); free(queue); free(rows);
    if (!out) pc_error(error, capacity, "cannot retain native consumer dependency closure");
    return out;
}
