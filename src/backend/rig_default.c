#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "rig_default.h"
#include "host_image.h"

/* R-358: both native defaults copy this 85-byte constant, then call the
 * file-reading virtual instead of the buffer parser. Missing retired files
 * therefore replace the usable fallback with a read error. Only this backend's
 * retirement invocation uses the parser directly. Normal reads are unchanged. */
static const unsigned char rig_text[] =
    "{\n \"groups\": [\n  {\n   \"name\": \"Default\",\n   \"layer\": 0,\n"
    "   \"components\": []\n  }\n ]\n}\n";
typedef struct rig_profile {
    uint32_t table, default_fn, read_fn, parse_fn, resize_fn, string_ctor, string_dtor, text;
    unsigned char ctor_head[24], dtor_head[24];
} rig_profile;
static const rig_profile rig_profiles[2] = {
    {0x2564230,0x149c7f0,0x149c6e0,0x149d1b0,0x142d6f0,0x19ef2a0,0x19ef380,0x25642f0,
     {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8d,0x05,0xd3,0x21,0xdb,0,0x48,0x8b,0xd9,0x48,0x89,0x01,0xe8,0x18,0xf8,0xff,0xff},
     {0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48,0x8b,0xd9,0x48,0x8d,0x05,0xe7,0x20,0xdb}},
    {0x254dfc0,0x14a90c0,0x14a8fb0,0x14a9a80,0x14b5090,0x19fd040,0x19fd120,0x254e080,
     {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8d,0x05,0x23,0xe2,0xd8,0,0x48,0x8b,0xd9,0x48,0x89,0x01,0xe8,0x28,0xf8,0xff,0xff},
     {0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48,0x8b,0xd9,0x48,0x8d,0x05,0x37,0xe1,0xd8}}
};
static const unsigned char rig_default_head[] = {0x40,0x57,0x48,0x83,0xec,0x60,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48,0x89,0x5c,0x24,0x78};
static const unsigned char rig_read_head[] = {0x4c,0x8b,0xdc,0x56,0x57,0x41,0x56,0x48,0x81,0xec,0x80,0,0,0};
static const unsigned char rig_parse_head[] = {0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0x70,0xff,0xff,0xff,0x48,0x81,0xec,0x90,0x01,0,0};
static const unsigned char rig_resize_head[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x40,0x48,0x63,0xf2,0x48,0x8b,0xd9,0x85,0xd2,0x7f};
typedef unsigned char (*rig_resize_fn)(void *, int);
typedef uintptr_t (*rig_parse_fn)(void *, void *, unsigned char);
typedef void (*rig_string_fn)(void *);
typedef struct rig_json_method { uint32_t slot, function; unsigned char head[16]; } rig_json_method;
typedef struct rig_json_binding {
    uint32_t holder, object, table, getter;
    unsigned getter_length;
    unsigned char getter_head[98];
    rig_json_method methods[4];
} rig_json_binding;
#include "rig_default_bindings.h"
static const uint8_t *rig_base;
static size_t rig_size;
static const rig_json_binding *rig_json;
static const rig_profile *rig_current;
static int rig_verified;
static rig_resize_fn rig_resize;
static rig_parse_fn rig_parse;
static rig_string_fn rig_ctor, rig_dtor;

static int rig_pin(const uint8_t *base, size_t size, uint32_t rva, const void *bytes, size_t count)
{
    return rva <= size && count <= size - rva && !memcmp(base + rva, bytes, count);
}
int sh_rig_default_install(const uint8_t *base, size_t size, int renderer)
{
    const rig_profile *p;
    rig_verified = 0; rig_base = base; rig_size = size;
    rig_json = renderer == 0 || renderer == 1 ? rig_json_profiles + renderer : NULL;
    rig_current = renderer == 0 || renderer == 1 ? rig_profiles + renderer : NULL;
    if (!base || !rig_current || !sh_host_is_pinned_renderer_build(renderer)) return 0;
    p = rig_current;
    __try {
        if (!rig_pin(base,size,p->default_fn,rig_default_head,sizeof rig_default_head) ||
            !rig_pin(base,size,p->read_fn,rig_read_head,sizeof rig_read_head) ||
            !rig_pin(base,size,p->parse_fn,rig_parse_head,sizeof rig_parse_head) ||
            !rig_pin(base,size,p->resize_fn,rig_resize_head,sizeof rig_resize_head) ||
            !rig_pin(base,size,p->string_ctor,p->ctor_head,sizeof p->ctor_head) ||
            !rig_pin(base,size,p->string_dtor,p->dtor_head,sizeof p->dtor_head) ||
            !rig_pin(base,size,p->text,rig_text,sizeof rig_text - 1) ||
            p->table > size || 0x58 > size - p->table ||
            *(const void **)(base+p->table+0x40) != base+p->read_fn ||
            *(const void **)(base+p->table+0x50) != base+p->default_fn) return 0;
        rig_resize = (rig_resize_fn)(base+p->resize_fn);
        rig_parse = (rig_parse_fn)(base+p->parse_fn);
        rig_ctor = (rig_string_fn)(base+p->string_ctor);
        rig_dtor = (rig_string_fn)(base+p->string_dtor);
        if (!rig_pin(base,size,rig_json->getter,rig_json->getter_head,rig_json->getter_length)) return 0;
        for (unsigned i=0;i<4;i++)
            if (!rig_pin(base,size,rig_json->methods[i].function,rig_json->methods[i].head,16)) return 0;
        rig_verified = 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
int sh_rig_default_ready(const char *family)
{
    return !family || _stricmp(family,"md6rig") || rig_verified;
}
static int rig_callback(void *actual)
{
    return rig_base && rig_current && actual == rig_base+rig_current->default_fn;
}
static int rig_identity(void *resource, void *actual)
{
    return rig_verified && rig_callback(actual) && resource &&
        *(const void **)resource == rig_base+rig_current->table &&
        *(const void **)(rig_base+rig_current->table+0x40) == rig_base+rig_current->read_fn &&
        *(const void **)(rig_base+rig_current->table+0x50) == actual;
}
static int rig_json_binding_ready(void)
{
    if (!rig_json || rig_json->holder>rig_size || 8>rig_size-rig_json->holder ||
        rig_json->object>rig_size || 8>rig_size-rig_json->object ||
        rig_json->table>rig_size || 0x28>rig_size-rig_json->table) return 0;
    if (!rig_pin(rig_base,rig_size,rig_json->getter,rig_json->getter_head,rig_json->getter_length)) return 0;
    if (*(const void **)(rig_base+rig_json->holder)!=rig_base+rig_json->object ||
        *(const void **)(rig_base+rig_json->object)!=rig_base+rig_json->table) return 0;
    for (unsigned i=0;i<4;i++) {
        const rig_json_method *m=rig_json->methods+i;
        if (*(const void **)(rig_base+rig_json->table+m->slot)!=rig_base+m->function ||
            !rig_pin(rig_base,rig_size,m->function,m->head,16)) return 0;
    }
    return 1;
}
int sh_rig_default_invoke(void *resource, void *actual)
{
    unsigned char *object = (unsigned char *)resource;
    __declspec(align(16)) unsigned char error[48];
    uintptr_t result;
    void *group;
    if (!rig_callback(actual)) {
        if (!actual) return 0;
        ((void (*)(void *))actual)(resource);
        return 1;
    }
    if (!rig_identity(resource,actual) || !rig_json_binding_ready() || *(int *)(object+0x40) != 0 ||
        *(int *)(object+0x44) < 0 || *(int *)(object+0x58) < 0 ||
        *(int *)(object+0x5c) < *(int *)(object+0x58)) return 0;
    /* Native list resizing retains its allocator and ownership rules. Check its
     * actual success before copying; the engine's default ignores this result. */
    if (!rig_resize(object+0x50,(int)sizeof rig_text-1) ||
        !*(void **)(object+0x50) || *(int *)(object+0x5c) < (int)sizeof rig_text-1) return 0;
    memcpy(*(void **)(object+0x50),rig_base+rig_current->text,sizeof rig_text-1);
    *(int *)(object+0x58) = (int)sizeof rig_text-1;
    rig_ctor(error);
    __try { result = rig_parse(resource,error,0); }
    __finally { rig_dtor(error); }
    if (result || *(int *)(object+0x40) != 1 || *(int *)(object+0x44) < 1 ||
        !*(void **)(object+0x38)) return 0;
    group = **(void ***)(object+0x38);
    return group && *(int *)((uint8_t *)group+0x08) == 7 &&
        *(void **)((uint8_t *)group+0x10) &&
        !memcmp(*(void **)((uint8_t *)group+0x10),"Default",8) &&
        *(int *)((uint8_t *)group+0x30) == 0 &&
        *(int *)((uint8_t *)group+0x40) == 0;
}
int sh_rig_default_observed(void *resource, void *actual)
{
    __try { return rig_identity(resource,actual); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
