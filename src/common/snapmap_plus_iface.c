/* Backend-owned shared interface factory, command registry, and work queue.
 * The frontend calls the vtable; this module has no engine link dependency.
 * See snapmap_plus_iface.h for the fixed cross-DLL layout. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "snapmap_plus_iface.h"

/* Small command registry backed by a growable linear array. */
typedef struct cmd_entry {
    char           *name;           /* heap-owned */
    sh_cmd_handler  handler;
    void           *ctx;
} cmd_entry;

typedef struct cmd_map {
    cmd_entry *items;
    size_t     count;
    size_t     cap;
} cmd_map;

/* Keep the fixed ABI header first; private storage and locking follow it. */
typedef struct sub_impl {
    sh_iface_sub  pinned;           /* Must remain first. */
    cmd_map       map;
    CRITICAL_SECTION lock;          /* guards both the map and the work-queue */
} sub_impl;

/* obj + 0x08 remains reserved for ABI compatibility. The subobject's critical
 * section is the live guard for both registry and queue. */


static void iface_register_cmd(sh_iface *self, const char *name, sh_cmd_handler handler, void *ctx)
{
    if (!self || !self->sub || !name) return;
    sub_impl *si = (sub_impl *)self->sub;
    EnterCriticalSection(&si->lock);
    /* Registration replaces an existing name. */
    for (size_t i = 0; i < si->map.count; i++) {
        if (strcmp(si->map.items[i].name, name) == 0) {
            si->map.items[i].handler = handler;
            si->map.items[i].ctx     = ctx;
            LeaveCriticalSection(&si->lock);
            return;
        }
    }
    if (si->map.count == si->map.cap) {
        size_t ncap = si->map.cap ? si->map.cap * 2 : 32;
        cmd_entry *ni = (cmd_entry *)realloc(si->map.items, ncap * sizeof(cmd_entry));
        if (!ni) { LeaveCriticalSection(&si->lock); return; }
        si->map.items = ni;
        si->map.cap   = ncap;
    }
    size_t namelen = strlen(name) + 1;
    char  *namecpy = (char *)malloc(namelen);
    if (!namecpy) { LeaveCriticalSection(&si->lock); return; }
    memcpy(namecpy, name, namelen);
    si->map.items[si->map.count].name    = namecpy;
    si->map.items[si->map.count].handler = handler;
    si->map.items[si->map.count].ctx     = ctx;
    si->map.count++;
    LeaveCriticalSection(&si->lock);
}


static void iface_unregister_cmd(sh_iface *self, const char *name)
{
    if (!self || !self->sub || !name) return;
    sub_impl *si = (sub_impl *)self->sub;
    EnterCriticalSection(&si->lock);
    for (size_t i = 0; i < si->map.count; i++) {
        if (strcmp(si->map.items[i].name, name) == 0) {
            free(si->map.items[i].name);
            si->map.items[i] = si->map.items[si->map.count - 1];
            si->map.count--;
            break;
        }
    }
    LeaveCriticalSection(&si->lock);
}

/* Drain on the calling thread, normally the frontend UI worker. Engine-touch
 * command dispatch now runs inline on the engine main thread instead of
 * enqueueing here. Retain this ABI slot for queue consumers and the tick hook.
 * Detached work owns argv until its handler returns. */
/* Optional drain callback; it runs on the drain caller's thread. */
static void (*g_tick_hook)(void) = NULL;

void sh_iface_set_tick_hook(void (*fn)(void))
{
    g_tick_hook = fn;
}

static void iface_drain_work_queue(sh_iface *self)
{
    if (g_tick_hook) g_tick_hook();
    if (!self || !self->sub) return;
    sub_impl *si = (sub_impl *)self->sub;
    sh_iface_sub *sub = &si->pinned;

    EnterCriticalSection(&si->lock);
    sh_work_item *begin = sub->wq_begin;
    sh_work_item *end   = sub->wq_end;
    /* Detach under the lock so handlers can enqueue without changing this iteration. */
    sub->wq_begin = NULL;
    sub->wq_end   = NULL;
    sub->wq_cap   = NULL;
    LeaveCriticalSection(&si->lock);

    for (sh_work_item *it = begin; it != end; it++) {
        if (it->handler) it->handler(it->ctx, it->argc, (const char **)it->argv);

        if (it->argv) {
            for (int i = 0; i < it->argc; i++) free(it->argv[i]);
            free(it->argv);
        }
    }
    free(begin);
}

/* Locked registry lookup; copy handler/context on a hit and return 1, else 0. */
int sh_iface_lookup_cmd(sh_iface *self, const char *name, sh_cmd_handler *handler, void **ctx)
{
    if (!self || !self->sub || !name) return 0;
    sub_impl *si = (sub_impl *)self->sub;
    int found = 0;
    EnterCriticalSection(&si->lock);
    for (size_t i = 0; i < si->map.count; i++) {
        if (strcmp(si->map.items[i].name, name) == 0) {
            if (handler) *handler = si->map.items[i].handler;
            if (ctx)     *ctx     = si->map.items[i].ctx;
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&si->lock);
    return found;
}

/* Enqueue a deep copy of argv for later execution on the drain caller's thread. */
int sh_iface_enqueue_work(sh_iface *self, sh_cmd_handler handler, void *ctx,
                          int argc, const char **argv)
{
    if (!self || !self->sub || !handler) return 0;
    sub_impl *si = (sub_impl *)self->sub;
    sh_iface_sub *sub = &si->pinned;


    char **argv_copy = NULL;
    if (argc > 0 && argv) {
        argv_copy = (char **)calloc((size_t)argc, sizeof(char *));
        if (!argv_copy) return 0;
        for (int i = 0; i < argc; i++) {
            const char *s = argv[i] ? argv[i] : "";
            size_t n = strlen(s) + 1;
            char *c = (char *)malloc(n);
            if (!c) { for (int j = 0; j < i; j++) free(argv_copy[j]); free(argv_copy); return 0; }
            memcpy(c, s, n);
            argv_copy[i] = c;
        }
    } else {
        argc = 0;
    }

    EnterCriticalSection(&si->lock);

    size_t cur_len = (size_t)(sub->wq_end - sub->wq_begin);
    size_t cur_cap = (size_t)(sub->wq_cap - sub->wq_begin);
    if (cur_len == cur_cap) {
        size_t ncap = cur_cap ? cur_cap * 2 : 8;
        sh_work_item *nb = (sh_work_item *)realloc(sub->wq_begin, ncap * sizeof(sh_work_item));
        if (!nb) {
            LeaveCriticalSection(&si->lock);
            if (argv_copy) { for (int i = 0; i < argc; i++) free(argv_copy[i]); free(argv_copy); }
            return 0;
        }
        sub->wq_begin = nb;
        sub->wq_end   = nb + cur_len;
        sub->wq_cap   = nb + ncap;
    }
    sub->wq_end->handler = handler;
    sub->wq_end->ctx     = ctx;
    sub->wq_end->argc    = argc;
    sub->wq_end->argv    = argv_copy;
    sub->wq_end++;
    LeaveCriticalSection(&si->lock);
    return 1;
}

/* One mutable shared vtable. Registry/drain slots bind immediately; config
 * and engine callbacks bind separately. Optional slots may remain null. */
static sh_iface_vtbl g_iface_vtbl_live = {
    .register_cmd     = iface_register_cmd,     /* +0x188 */
    .unregister_cmd   = iface_unregister_cmd,   /* +0x190 */
    .drain_work_queue = iface_drain_work_queue, /* +0x1a0 */

};

/* Config is available before engine installation. Keep its binding separate
 * so later engine binding does not overwrite these cells. */
void sh_iface_bind_config_slots(const sh_iface_config_slots *slots)
{
    if (!slots) return;
    g_iface_vtbl_live.config_get_json = slots->config_get_json; /* +0x2B0 */
    g_iface_vtbl_live.config_set_json = slots->config_set_json; /* +0x2B8 */
}

/* Copy engine callbacks into the shared vtable without linking this module
 * to the engine layer. Null callbacks clear their corresponding slots. */
void sh_iface_bind_engine_slots(const sh_iface_engine_slots *s)
{
    if (!s) return;
    g_iface_vtbl_live.set_editor_vec3   = s->set_editor_vec3;    /* +0x00  (camera) */
    g_iface_vtbl_live.get_editor_vec3   = s->get_editor_vec3;    /* +0x08  (camera) */
    g_iface_vtbl_live.entity_count      = s->entity_count;       /* +0x10  */
    g_iface_vtbl_live.id_to_string      = s->id_to_string;       /* +0x18  */
    g_iface_vtbl_live.is_valid_id       = s->is_valid_id;        /* +0x28  */
    g_iface_vtbl_live.editor_ready_poll = s->editor_ready_poll;  /* +0x88  (window gate) */
    g_iface_vtbl_live.get_classname_copy= s->get_classname_copy; /* +0x48  */
    g_iface_vtbl_live.get_inherit_copy  = s->get_inherit_copy;   /* +0x50  */
    g_iface_vtbl_live.add_to_selection  = s->add_to_selection;   /* +0x138 */
    g_iface_vtbl_live.clear_selection   = s->clear_selection;    /* +0x148 */
    g_iface_vtbl_live.get_selection     = s->get_selection;      /* +0x150 */
    g_iface_vtbl_live.hovered_id        = s->hovered_id;         /* +0x198 */
    g_iface_vtbl_live.is_entity_mode    = s->is_entity_mode;     /* +0x1c0 (Create-New-Timeline gate) */
    g_iface_vtbl_live.toast             = s->toast;              /* +0x1b8 */

    g_iface_vtbl_live.serialize_entity  = s->serialize_entity;   /* +0xc8 */
    g_iface_vtbl_live.apply_edit        = s->apply_edit;         /* +0xd0 */
    g_iface_vtbl_live.read_prefab       = s->read_prefab;        /* +0xb8 */

    g_iface_vtbl_live.get_declsource_copy    = s->get_declsource_copy;    /* +0x30  */
    g_iface_vtbl_live.rebuild_set_declsource = s->rebuild_set_declsource; /* +0x40  */
    g_iface_vtbl_live.get_displayname        = s->get_displayname;        /* +0x58  */
    g_iface_vtbl_live.set_classname          = s->set_classname;          /* +0x78  */
    g_iface_vtbl_live.set_inherit            = s->set_inherit;            /* +0x80  */
    g_iface_vtbl_live.set_entity_0x170       = s->set_displayname;        /* +0x128 */
    g_iface_vtbl_live.serialize_selection    = s->serialize_selection;    /* +0xb0  */
    g_iface_vtbl_live.resolve_prefab_path    = s->resolve_prefab_path;    /* +0xc0  */
    g_iface_vtbl_live.selection_guard        = s->remove_from_selection;  /* +0x130 */

    g_iface_vtbl_live.enum_decls_of_resclass = s->enum_decls_of_resclass; /* +0x110 */

    g_iface_vtbl_live.apply_class_inherit    = s->apply_class_inherit;    /* +0x268 */

    g_iface_vtbl_live.enum_valid_classes     = s->enum_valid_classes;     /* +0x270 */

    g_iface_vtbl_live.enum_inherits          = s->enum_inherits;          /* +0x278 */

    g_iface_vtbl_live.id_dev_layer_hidden    = s->id_dev_layer_hidden;    /* +0x280 */

    g_iface_vtbl_live.wire_edit_generation   = s->wire_edit_generation;   /* +0x288 */

    g_iface_vtbl_live.apply_sync             = s->apply_sync;             /* +0x290 */

    g_iface_vtbl_live.normalize_timeline_inherit = s->normalize_timeline_inherit; /* +0x298 */

    g_iface_vtbl_live.push_to_stack          = s->push_to_stack;          /* +0x2A0 */

    g_iface_vtbl_live.clear_stack            = s->clear_stack;            /* +0x2A8 */
    g_iface_vtbl_live.manipulation_in_progress = s->manipulation_in_progress; /* +0x2C0 */

    g_iface_vtbl_live.find_material           = s->find_material;           /* +0x2C8 */
    g_iface_vtbl_live.get_preview             = s->get_preview;             /* +0x2D0 */
    g_iface_vtbl_live.request_preview         = s->request_preview;         /* +0x2D8 */
    g_iface_vtbl_live.list_materials          = s->list_materials;          /* +0x2E0 */
    g_iface_vtbl_live.list_assets             = s->list_assets;             /* +0x2E8 */
    g_iface_vtbl_live.material_rect           = s->material_rect;           /* +0x2F0 */
    g_iface_vtbl_live.sound_preview           = s->sound_preview;           /* +0x2F8 */
    g_iface_vtbl_live.sound_session           = s->sound_session;           /* +0x300 */
    g_iface_vtbl_live.resolve_prefab_model    = s->resolve_prefab_model;    /* +0x308 */
    g_iface_vtbl_live.request_prefab_mesh     = s->request_prefab_mesh;     /* +0x310 */
    g_iface_vtbl_live.get_prefab_mesh         = s->get_prefab_mesh;         /* +0x318 */
    g_iface_vtbl_live.resolve_prefab_defaults = s->resolve_prefab_defaults; /* +0x320 */
    /* clone-extension (the File menu's rawmap load/save file surface). */
    g_iface_vtbl_live.rawmap_status           = s->rawmap_status;           /* +0x328 */
    g_iface_vtbl_live.rawmap_configure        = s->rawmap_configure;        /* +0x330 */
    g_iface_vtbl_live.rawmap_load_now         = s->rawmap_load_now;         /* +0x338 */
}

/* Allocate the fixed object and private subobject with an empty registry/queue. */
sh_iface *sh_iface_create(void)
{
    sh_iface *obj = (sh_iface *)calloc(1, sizeof(sh_iface));
    if (!obj) return NULL;

    sub_impl *si = (sub_impl *)calloc(1, sizeof(sub_impl));
    if (!si) { free(obj); return NULL; }
    InitializeCriticalSection(&si->lock);

    si->map.items   = NULL;
    si->map.count   = 0;
    si->map.cap     = 0;
    si->pinned.wq_begin = NULL;
    si->pinned.wq_end   = NULL;
    si->pinned.wq_cap   = NULL;
    /* Preserve the ABI fields while the actual registry resides in si->map. */
    si->pinned.map_nil  = &si->map;
    si->pinned.map_root = NULL;
    si->pinned.map_size = 0;

    obj->vtbl = &g_iface_vtbl_live;     /* +0x00 */
    /* Reserved object mutex bytes stay zero; si->lock guards live state. */
    obj->sub  = (sh_iface_sub *)si;     /* +0x58: sub_impl starts with the fixed header. */

    return obj;
}
