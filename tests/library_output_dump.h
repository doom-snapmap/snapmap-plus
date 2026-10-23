/* Canonical text of a compiled library's outputs, for comparing two compiles that must agree: every field of each
 * compiled resource, the compilation's policy (requirements, strings, HUD, localized strings and HUD bindings), the
 * provenance index, constructor consumers, state-machine webs, key groups and the accounting outputs (coverage,
 * link diagnostics, shader and composed inputs, recovered images, the image gate). Bodies are compared by length
 * and FNV-1a. Phase timings, memory meters and repeated-work diagnostics are left out. lod_same also compares each
 * linked material's virtual material binding. Reading every output string after the compile returns is also the
 * check that no output borrows memory its source capture freed. */
#ifndef LIBRARY_OUTPUT_DUMP_H
#define LIBRARY_OUTPUT_DUMP_H
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_library.h"
#include "package_consumers.h"
#include "package_fsm.h"
#include "package_keys.h"
#include "virtual_material_runtime.h"
#include "config_json.h"

typedef struct lod_text { char *text; size_t length, capacity; int failed; } lod_text;
static void lod_put(lod_text *t, const char *format, ...)
{
    if (t->failed) return;
    for (;;) {
        size_t room = t->capacity - t->length;
        int n = -1;
        if (t->capacity) { va_list args; va_start(args, format); n = vsnprintf(t->text + t->length, room, format, args); va_end(args); }
        if (n >= 0 && (size_t)n < room) { t->length += (size_t)n; return; }
        size_t next = t->capacity ? t->capacity * 2 : 4096;
        if (n >= 0 && next < t->length + (size_t)n + 1) next = t->length + (size_t)n + 1;
        char *grown = realloc(t->text, next);
        if (!grown) { t->failed = 1; return; }
        t->text = grown; t->capacity = next;
    }
}
static unsigned long long lod_hash(const unsigned char *bytes, size_t length)
{
    unsigned long long h = 14695981039346656037ull;
    for (size_t i = 0; bytes && i < length; i++) { h ^= bytes[i]; h *= 1099511628211ull; }
    return h;
}
static const char *lod_s(const char *s) { return s ? s : "(null)"; }
static void lod_owners(lod_text *t, const char *label, const sh_package_owners *o)
{
    lod_put(t, " %s=%llx", label, (unsigned long long)o->bits);
    for (size_t i = 0; o->more && i < o->words; i++) lod_put(t, ",%llx", (unsigned long long)o->more[i]);
}
static void lod_json(lod_text *t, const char *label, const sh_json_object *object)
{
    size_t length = 0;
    char *text = sh_json_serialize_object(object, 0, &length);
    lod_put(t, "%s %s\n", label, text ? text : "(unserializable)");
    free(text);
}
static void lod_resource(lod_text *t, const sh_compiled_resource *r)
{
    lod_put(t, "resource %s type=%s name=%s native=%s/%s original=%s\n", lod_s(r->engine_path), lod_s(r->type),
        lod_s(r->name), lod_s(r->native_type), lod_s(r->native_name), lod_s(r->original_path));
    lod_put(t, "  known=%d composed=%d native=%d restored=%d coverage=%d generated=%d layer=%d rebound=%d owns=%d"
        " cache=%s file=%d source=%zu sound=%u/%u", r->baseline_known, r->composed, r->native_original,
        r->restored_original, r->semantic_coverage, r->generated, r->generated_layer, r->rebound_original,
        r->owns_engine_path, lod_s(r->cache_path), r->cache_file != NULL, r->source,
        (unsigned)r->sound_authored_event, (unsigned)r->sound_event);
    lod_owners(t, "owners", &r->owners); lod_owners(t, "gameplay", &r->gameplay_owners);
    lod_put(t, "\n  body %zu %016llx presented %zu %016llx\n  sources", r->body_length,
        lod_hash(r->body, r->body_length), r->presented_length, lod_hash(r->presented, r->presented_length));
    for (size_t i = 0; i < r->source_count; i++) lod_put(t, " %zu", r->sources[i]);
    lod_put(t, "\n  inputs");
    for (size_t i = 0; i < r->generated_input_count; i++) lod_put(t, " %s", lod_s(r->generated_inputs[i]));
    lod_put(t, "\n  input sources");
    for (size_t i = 0; i < r->generated_source_count; i++) lod_put(t, " %zu", r->generated_sources[i]);
    lod_put(t, "\n  bound");
    for (size_t i = 0; i < r->bound_dependency_count; i++) lod_put(t, " %s", lod_s(r->bound_dependencies[i]));
    lod_put(t, "\n  aliases");
    for (size_t i = 0; i < r->native_alias_count; i++)
        lod_put(t, " %s/%s", lod_s(r->native_aliases[i].type), lod_s(r->native_aliases[i].name));
    lod_put(t, "\n");
}
/* malloc-owned canonical text of library; NULL when it cannot be built. */
static char *lod_dump(const sh_package_library *library)
{
    lod_text t = {0};
    const sh_package_compilation *c = library->compiled;
    lod_put(&t, "compiled %d\n", c != NULL);
    if (c) {
        lod_put(&t, "resources %zu duplicates %zu composed %zu independent %d/%d overlay %d owner begin %zu\n",
            c->resource_count, c->duplicate_count, c->composed_count, c->independent_sources,
            c->map_independent_sources, c->map_overlay, c->map_owner_begin);
        for (size_t i = 0; i < c->resource_count; i++) lod_resource(&t, c->resources + i);
        lod_json(&t, "requirements", &c->policy.requirements);
        lod_json(&t, "strings", &c->policy.strings);
        lod_json(&t, "hud", &c->policy.hud);
        for (size_t i = 0; i < c->policy.localized_count; i++) {
            lod_put(&t, "localized %s %s", lod_s(c->policy.localized[i].key), lod_s(c->policy.localized[i].mapping));
            lod_owners(&t, "owners", &c->policy.localized[i].owners); lod_put(&t, "\n");
        }
        lod_owners(&t, "legacy string", &c->policy.legacy_string_owners);
        lod_put(&t, "\nhud bound %d\n", c->policy.hud_bound);
        for (size_t i = 0; i < c->policy.hud_binding_count; i++) {
            lod_put(&t, "hud binding %s %d", lod_s(c->policy.hud_bindings[i].name), c->policy.hud_bindings[i].weapon_mode);
            lod_owners(&t, "owners", &c->policy.hud_bindings[i].owners); lod_put(&t, "\n");
        }
    }
    const sh_package_provenance *p = &library->bindings;
    lod_put(&t, "provenance semantics %u contexts %zu bindings %zu uses %zu\n", p->semantics, p->context_count,
        p->binding_count, p->use_count);
    for (size_t i = 0; i < p->context_count; i++) {
        lod_put(&t, "context %u %s", p->contexts[i].mode, lod_s(p->contexts[i].component));
        for (size_t k = 0; k < p->contexts[i].tree_count; k++) lod_put(&t, " %016llx", lod_hash(p->contexts[i].trees[k], 32));
        lod_put(&t, "\n");
    }
    for (size_t i = 0; i < p->binding_count; i++) {
        lod_put(&t, "binding %s %s %s", lod_s(p->bindings[i].family), lod_s(p->bindings[i].original), lod_s(p->bindings[i].name));
        for (size_t k = 0; k < p->bindings[i].context_count; k++) lod_put(&t, " %zu", p->bindings[i].contexts[k]);
        lod_put(&t, "\n");
    }
    for (size_t i = 0; i < p->use_count; i++) {
        lod_put(&t, "use %u %s %s %zu %s", p->uses[i].kind, lod_s(p->uses[i].owner), lod_s(p->uses[i].reference),
            p->uses[i].binding, lod_s(p->uses[i].row));
        for (size_t k = 0; k < p->uses[i].context_count; k++) lod_put(&t, " %zu", p->uses[i].contexts[k]);
        lod_put(&t, "\n");
    }
    size_t consumers = sh_package_consumers_count(library->consumers);
    lod_put(&t, "consumers %zu\n", consumers);
    for (size_t i = 0; i < consumers; i++) {
        const sh_package_consumer *owner = sh_package_consumers_at(library->consumers, i);
        lod_put(&t, "consumer %s %s", lod_s(owner->family), lod_s(owner->name));
        for (size_t k = 0; k < owner->count; k++)
            lod_put(&t, " [%s %s %s]", lod_s(owner->targets[k].slot), lod_s(owner->targets[k].family), lod_s(owner->targets[k].name));
        lod_put(&t, "\n");
    }
    const sh_package_fsm_webs *webs = library->fsm_webs;
    lod_put(&t, "fsm webs %zu owned %zu generated %zu\n", webs ? webs->count : 0, webs ? webs->owned_count : 0,
        webs ? webs->generated_count : 0);
    for (size_t i = 0; webs && i < webs->count; i++) {
        const sh_package_fsm_web *w = webs->webs + i;
        lod_put(&t, "web %s behaviors", lod_s(w->config));
        for (size_t k = 0; k < w->behavior_count; k++) lod_put(&t, " %s", lod_s(w->behaviors[k]));
        lod_put(&t, " replacements");
        for (size_t k = 0; k < w->count; k++) lod_put(&t, " %s>%s", lod_s(w->from[k]), lod_s(w->to[k]));
        lod_put(&t, " extra");
        for (size_t k = 0; k < w->extra_count; k++) lod_put(&t, " %s", lod_s(w->extra[k]));
        lod_put(&t, "\n");
    }
    for (size_t i = 0; webs && i < webs->owned_count; i++) lod_put(&t, "owned %s\n", lod_s(webs->owned[i]));
    for (size_t i = 0; webs && i < webs->generated_count; i++)
        lod_put(&t, "generated %s %zu %016llx\n", lod_s(webs->generated[i].name), webs->generated[i].length,
            lod_hash((const unsigned char *)webs->generated[i].text, webs->generated[i].length));
    const sh_package_key_groups *keys = library->key_groups;
    lod_put(&t, "key groups %d count %zu historical %lu\n", keys != NULL, keys ? keys->count : 0,
        keys ? (unsigned long)keys->historical : 0ul);
    for (size_t i = 0; keys && i < SH_PACKAGE_KEY_PATTERNS; i++) if (keys->defined[i]) {
        lod_put(&t, "key pattern %zu", i);
        for (size_t k = 0; k < sizeof(keys->members[i].words) / sizeof(keys->members[i].words[0]); k++)
            lod_put(&t, " %llx", (unsigned long long)keys->members[i].words[k]);
        lod_put(&t, "\n");
    }
    lod_put(&t, "coverage %s\nlink diagnostics %s\n", lod_s(library->coverage), lod_s(library->link_diagnostics));
    lod_put(&t, "shader inputs %zu lost %d\n", library->shader_input_count, library->shader_inputs_lost);
    for (size_t i = 0; i < library->shader_input_count; i++) {
        const sh_package_unpublished_shader *s = library->shader_inputs + i;
        lod_put(&t, "shader %zu %zu %s %d %d %08lx\n", s->file, s->pair, lod_s(s->pair_path), s->reason, s->renderer,
            (unsigned long)s->checksum);
    }
    lod_put(&t, "composed inputs %zu lost %d\n", library->composed_input_count, library->composed_inputs_lost);
    for (size_t i = 0; i < library->composed_input_count; i++) {
        lod_put(&t, "composed %zu", library->composed_inputs[i].file);
        for (size_t k = 0; k < library->composed_inputs[i].peer_count; k++) lod_put(&t, " %zu", library->composed_inputs[i].peers[k]);
        lod_put(&t, "\n");
    }
    lod_put(&t, "image gate %d recovered %zu", library->image_gate, library->recovered_image_count);
    for (size_t i = 0; i < library->recovered_image_count; i++) lod_put(&t, " %s", lod_s(library->recovered_images[i]));
    lod_put(&t, "\nvirtual materials %d audio %d\n", library->virtual_materials != NULL, library->audio != NULL);
    if (t.failed) { free(t.text); return NULL; }
    return t.text ? t.text : calloc(1, 1);
}
/* 1 when a and b are the same compiled outputs; otherwise reports the first difference under label. */
static int lod_same(const sh_package_library *a, const sh_package_library *b, const char *label)
{
    char *x = lod_dump(a), *y = lod_dump(b);
    int same = x && y && !strcmp(x, y);
    if (x && y && !same) {
        size_t at = 0, line = 1;
        while (x[at] && x[at] == y[at]) { if (x[at] == '\n') line++; at++; }
        size_t from = at; while (from && x[from - 1] != '\n') from--;
        fprintf(stderr, "%s: outputs differ at line %zu:\n  %.300s\n  %.300s\n", label, line, x + from, y + from);
    } else if (!x || !y) fprintf(stderr, "%s: cannot dump the outputs\n", label);
    for (size_t i = 0; same && a->compiled && i < a->compiled->resource_count; i++) {
        const sh_compiled_resource *r = a->compiled->resources + i;
        const char *name = r->native_name ? r->native_name : r->name;
        if (!name || !sh_package_resource_native_matches(r, "material", name)) continue;
        if ((a->virtual_materials != NULL) != (b->virtual_materials != NULL) || (a->virtual_materials &&
            !sh_virtual_material_runtime_binding_equal(a->virtual_materials, b->virtual_materials, name))) {
            fprintf(stderr, "%s: virtual material binding differs for %s\n", label, lod_s(r->engine_path)); same = 0;
        }
    }
    free(x); free(y); return same;
}
#endif
