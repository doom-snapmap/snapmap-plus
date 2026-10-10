#include "audio_link_graph.h"
#include "audio_id_reservations.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct bytes { unsigned char data[2048]; size_t size, reads, fail; } bytes;
typedef struct fixture {
    bytes bank, media;
    sh_audio_bank_codec codec;
    sh_audio_graph_bank input;
    int media_present, media_error, media_original;
} fixture;
static void put(bytes *b, uint32_t value, size_t width)
{ assert(width <= sizeof(b->data) - b->size); for (size_t i = 0; i < width; i++) b->data[b->size++] = (unsigned char)(value >> (8 * i)); }
static void zero(bytes *b, size_t width) { while (width--) put(b, 0, 1); }
static void chunk(bytes *b, const char *tag, uint32_t size)
{ assert(b->size + 8 <= sizeof(b->data)); memcpy(b->data + b->size, tag, 4); b->size += 4; put(b, size, 4); }
static int read_bytes(void *context, uint64_t at, void *out, size_t size)
{
    bytes *b = context;
    if (b->fail || at > b->size || size > b->size - at) return 0;
    memcpy(out, b->data + at, size); b->reads += size; return 1;
}
static int write_bytes(void *context, const void *data, size_t size)
{
    bytes *b = context; if (size > sizeof(b->data) - b->size) return 0;
    memcpy(b->data + b->size, data, size); b->size += size; return 1;
}
static uint32_t u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static sh_audio_bank_source source(bytes *b) { return (sh_audio_bank_source){b, b->size, read_bytes}; }
static int media(void *context, uint32_t id, sh_audio_graph_media *out, char *error, size_t capacity)
{
    fixture *f = context; assert(id == 900);
    if (f->media_error) { snprintf(error, capacity, "fixture media IO failed"); return -1; }
    if (!f->media_present) return 0;
    *out = (sh_audio_graph_media){"sound/900.wem", source(&f->media), f->media_original}; return 1;
}
static void make(fixture *f, uint32_t bank_id, unsigned stream, unsigned tone, uint32_t parent)
{
    memset(f, 0, sizeof(*f)); bytes *b = &f->bank;
    chunk(b, "BKHD", 16); put(b, 113, 4); put(b, bank_id, 4); zero(b, 8);
    if (stream != 1) {
        chunk(b, "DIDX", 12); put(b, 900, 4); zero(b, 4); put(b, 4, 4);
        chunk(b, "DATA", 4); put(b, tone, 4);
    }
    chunk(b, "HIRC", 95); put(b, 3, 4);
    put(b, 4, 1); put(b, 12, 4); put(b, 50, 4); put(b, 1, 4); put(b, 60, 4);
    put(b, 3, 1); put(b, 18, 4); put(b, 60, 4); put(b, 0x0403, 2); put(b, 100, 4); zero(b, 4); put(b, bank_id, 4);
    put(b, 2, 1); put(b, 46, 4); put(b, 100, 4); put(b, 0x20001, 4); put(b, stream, 1);
    put(b, 900, 4); put(b, 4, 4); zero(b, 1); zero(b, 7); put(b, parent, 4); zero(b, 17);
    put(&f->media, tone, 4); f->media_present = stream != 0;
    char error[512]; assert(sh_audio_bank_codec_read(source(b), &f->codec, error, sizeof(error)) == 1);
    f->input = (sh_audio_graph_bank){"sound/bank.bnk", source(b), &f->codec, 0};
}
static sh_audio_graph_scope scope(fixture *f)
{ return (sh_audio_graph_scope){&f->input, 1, f, media}; }
typedef struct reservation {
    sh_audio_identity_domain domain; unsigned char binding[32]; uint32_t id;
} reservation;
typedef struct allocator { reservation rows[256]; size_t count, calls, fail_after; int collide, absent; } allocator;
static int assign(void *context, const sh_audio_link_graph *graph, size_t node, sh_audio_identity_domain domain, uint32_t authored,
    const unsigned char binding[32], int original, int absent, uint32_t *id, char *error, size_t capacity)
{
    allocator *a = context; a->absent += absent; assert(graph); (void)node;
    if (a->calls++ == a->fail_after) { snprintf(error, capacity, "fixture reservation failed"); return 0; }
    for (size_t i = 0; i < a->count; i++) if (a->rows[i].domain == domain && !memcmp(a->rows[i].binding, binding, 32)) {
        *id = a->rows[i].id; return 1;
    }
    assert(a->count < 256); reservation *r = &a->rows[a->count++];
    r->domain = domain; memcpy(r->binding, binding, 32);
    *id = r->id = original ? authored : a->collide ? 12345 : (uint32_t)(10000 + a->count); return 1;
}
static sh_audio_graph_root root(sh_audio_link_graph *g, size_t scope_id)
{
    sh_audio_graph_root out; char error[512];
    assert(sh_audio_link_graph_event(g, scope_id, 50, &out, error, sizeof(error))); return out;
}
static void check_output(sh_audio_link_graph *g, size_t scope_id, size_t bank_index,
    fixture *f, uint32_t *sound_id, uint32_t *parent_id)
{
    char error[512]; bytes out = {0}; sh_audio_bank_codec codec = {0};
    const sh_audio_bank_binding *binding = sh_audio_link_graph_bank(g, scope_id, bank_index); assert(binding);
    assert(sh_audio_bank_binding_write(source(&f->bank), binding, write_bytes, &out, error, sizeof(error)));
    assert(sh_audio_bank_codec_read(source(&out), &codec, error, sizeof(error)) == 1);
    uint32_t event_id, media_id;
    assert(sh_audio_link_graph_event_id(g, scope_id, 50, &event_id));
    assert(sh_audio_link_graph_media_id(g, scope_id, 900, &media_id));
    assert(codec.objects[0].id == event_id);
    assert(u32(out.data + codec.objects[0].offset + 8) == codec.objects[1].id);
    assert(u32(out.data + codec.objects[1].offset + 6) == codec.objects[2].id);
    assert(u32(out.data + codec.objects[1].offset + 14) == codec.metadata.id);
    assert(u32(out.data + codec.objects[2].offset + 9) == media_id);
    *sound_id = codec.objects[2].id; *parent_id = u32(out.data + codec.objects[2].offset + 25);
    sh_audio_bank_codec_free(&codec);
}
static void variants(unsigned stream)
{
    fixture a, b, copy; make(&a, 42, stream, 111, 0); make(&b, 42, stream, 222, 0); make(&copy, 42, stream, 111, 0);
    sh_audio_graph_scope scopes[] = {scope(&a), scope(&b), scope(&copy)}; char error[512];
    sh_audio_link_graph *g = sh_audio_link_graph_open(scopes, 3, error, sizeof(error));
    if (!g) fprintf(stderr, "%s\n", error); assert(g);
    sh_audio_graph_root ra = root(g, 0), rb = root(g, 1), rc = root(g, 2);
    assert(sh_audio_link_graph_equal(g, ra.node, rc.node) && !sh_audio_link_graph_equal(g, ra.node, rb.node));
    assert(!memcmp(ra.identity, rc.identity, 32) && memcmp(ra.identity, rb.identity, 32));
    assert(!ra.original && !sh_audio_link_graph_bank(g, 0, 0));
    allocator ids = {0}; ids.fail_after = SIZE_MAX;
    assert(sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)));
    uint32_t sa, sb, sc, parent;
    check_output(g, 0, 0, &a, &sa, &parent); assert(!parent);
    check_output(g, 1, 0, &b, &sb, &parent); check_output(g, 2, 0, &copy, &sc, &parent);
    assert(sa == sc && sa != sb); assert(u32(a.bank.data + 12) == 42 && u32(b.bank.data + 12) == 42);
    uint32_t before; assert(sh_audio_link_graph_event_id(g, 0, 50, &before));
    ids.fail_after = ids.calls; assert(!sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)));
    uint32_t after; assert(sh_audio_link_graph_event_id(g, 0, 50, &after) && before == after);
    allocator collision = {0}; collision.fail_after = SIZE_MAX; collision.collide = 1;
    assert(!sh_audio_link_graph_bind(g, assign, &collision, error, sizeof(error)));
    assert(strstr(error, "different bindings")); assert(sh_audio_link_graph_event_id(g, 0, 50, &after) && before == after);
    /* Input scope order cannot rename an existing binding. */
    sh_audio_graph_scope reversed[] = {scope(&b), scope(&copy), scope(&a)};
    sh_audio_link_graph *other = sh_audio_link_graph_open(reversed, 3, error, sizeof(error)); assert(other);
    sh_audio_graph_root moved = root(other, 2); assert(!memcmp(ra.identity, moved.identity, 32));
    sh_audio_link_graph_close(other); sh_audio_link_graph_close(g);
    sh_audio_bank_codec_free(&a.codec); sh_audio_bank_codec_free(&b.codec); sh_audio_bank_codec_free(&copy.codec);
}
static void shared_projection_transaction(void)
{
    fixture a, b; make(&a, 42, 0, 111, 0); make(&b, 42, 0, 222, 0);
    fixture *files[] = {&a, &b}; char error[512];
    for (size_t i = 0; i < 2; i++) {
        chunk(&files[i]->bank, "STMG", 18); zero(&files[i]->bank, 18);
        sh_audio_bank_codec_free(&files[i]->codec);
        assert(sh_audio_bank_codec_read(source(&files[i]->bank), &files[i]->codec, error, sizeof(error)) == 1);
        files[i]->input.source = source(&files[i]->bank);
    }
    sh_audio_graph_scope scopes[] = {scope(&a), scope(&b)};
    sh_audio_link_graph *g = sh_audio_link_graph_open(scopes, 2, error, sizeof(error)); assert(g);
    allocator ids = {0}; ids.fail_after = SIZE_MAX;
    assert(!sh_audio_link_graph_private_bank(g, 0, 0));
    assert(sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)));
    const sh_audio_bank_edit *retained[2]; uint32_t events[2];
    for (size_t i = 0; i < 2; i++) {
        const sh_audio_bank_binding *full = sh_audio_link_graph_bank(g, i, 0);
        const sh_audio_bank_binding *private = sh_audio_link_graph_private_bank(g, i, 0);
        assert(full != private && private->output_length + 26 == full->output_length);
        retained[i] = private->edits; assert(sh_audio_link_graph_event_id(g, i, 50, events + i));
        bytes output = {0}; sh_audio_bank_codec parsed = {0};
        assert(sh_audio_bank_binding_write(source(&files[i]->bank), private, write_bytes, &output, error, sizeof(error)));
        assert(sh_audio_bank_codec_read(source(&output), &parsed, error, sizeof(error)) == 1 && !parsed.shared_chunks);
        sh_audio_bank_codec_free(&parsed);
    }
    /* Fail the second projection after the first pending output is prepared.
     * Previously published graph views and numeric bindings stay intact. */
    b.codec.chunks[b.codec.chunk_count - 1].offset++;
    assert(!sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)) && strstr(error, "separate private audio"));
    b.codec.chunks[b.codec.chunk_count - 1].offset--;
    for (size_t i = 0; i < 2; i++) {
        uint32_t id; assert(sh_audio_link_graph_event_id(g, i, 50, &id) && id == events[i]);
        assert(sh_audio_link_graph_private_bank(g, i, 0)->edits == retained[i]);
    }
    assert(sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)));
    sh_audio_link_graph_close(g);
    a.input.original = 1; scopes[0] = scope(&a);
    g = sh_audio_link_graph_open(scopes, 1, error, sizeof(error)); assert(g);
    ids = (allocator){0}; ids.fail_after = SIZE_MAX;
    assert(sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)));
    assert(!sh_audio_link_graph_bank(g, 0, 0)->count);
    assert(sh_audio_link_graph_private_bank(g, 0, 0) == sh_audio_link_graph_bank(g, 0, 0));
    sh_audio_link_graph_close(g); sh_audio_bank_codec_free(&a.codec); sh_audio_bank_codec_free(&b.codec);
}
static void patch32(bytes *b,size_t at,uint32_t value)
{ assert(at+4<=b->size);for(unsigned i=0;i<4;i++)b->data[at+i]=(unsigned char)(value>>(8*i)); }
static void bus_fixture(fixture *f,unsigned tone,unsigned property)
{
    memset(f,0,sizeof(*f));bytes *b=&f->bank;char error[512];
    chunk(b,"BKHD",16);put(b,113,4);put(b,42,4);zero(b,8);
    chunk(b,"HIRC",0);put(b,2,4);put(b,8,1);size_t size=b->size;zero(b,4);size_t start=b->size;
    put(b,71,4);zero(b,4);put(b,1,1);put(b,7,1);put(b,property,4);zero(b,9+12);
    put(b,1,1);zero(b,1);put(b,2,1);put(b,74,4);put(b,1,1);zero(b,1+5+1+2+4);
    patch32(b,size,(uint32_t)(b->size-start));
    put(b,18,1);put(b,18,4);put(b,74,4);put(b,1234,4);put(b,1,4);put(b,tone,1);zero(b,5);
    patch32(b,28,(uint32_t)(b->size-32));
    int ready=sh_audio_bank_codec_read(source(b),&f->codec,error,sizeof(error));
    if(ready!=1)fprintf(stderr,"bus fixture: %s\n",error);assert(ready==1);
    f->input=(sh_audio_graph_bank){"sound/bus.bnk",source(b),&f->codec,0};
}
static size_t bus_field(const fixture *f,sh_audio_bus_field field)
{
    for(size_t i=0;i<f->codec.bus_record_count;i++)if(f->codec.bus_records[i].field==field)return i;
    assert(0);return SIZE_MAX;
}
typedef struct bus_sources { const fixture *file;size_t count; } bus_sources;
static int bus_source(void *context,const char *path,uint64_t offset,uint64_t length)
{
    bus_sources *seen=context;assert(!strcmp(path,"sound/bus.bnk"));
    const sh_audio_bank_codec *codec=&seen->file->codec;
    assert((offset==codec->bus_records[bus_field(seen->file,SH_AUDIO_BUS_FIELD_EFFECT)].offset && length==5) ||
        (offset==codec->objects[1].offset && length==codec->objects[1].length));
    seen->count++;return 1;
}
static void bus_bindings(void)
{
    fixture a,b,copy;bus_fixture(&a,11,10);bus_fixture(&b,22,10);bus_fixture(&copy,11,20);
    sh_audio_graph_scope scopes[]={scope(&a),scope(&b),scope(&copy)};char error[512];
    sh_audio_link_graph *g=sh_audio_link_graph_open(scopes,3,error,sizeof(error));assert(g);
    size_t fx=bus_field(&a,SH_AUDIO_BUS_FIELD_EFFECT),property=bus_field(&a,SH_AUDIO_BUS_FIELD_PROPERTY);
    sh_audio_graph_root effects[3],props[3];
    for(size_t i=0;i<3;i++) {
        assert(sh_audio_link_graph_bus_record(g,i,0,fx,effects+i));
        assert(sh_audio_link_graph_bus_record(g,i,0,property,props+i));
    }
    assert(sh_audio_link_graph_equal(g,effects[0].node,effects[2].node));
    assert(!sh_audio_link_graph_equal(g,effects[0].node,effects[1].node));
    assert(sh_audio_link_graph_equal(g,props[0].node,props[1].node));
    assert(!sh_audio_link_graph_equal(g,props[0].node,props[2].node));
    assert(!sh_audio_link_graph_bus_record(g,3,0,fx,props));
    unsigned char *body=NULL;size_t length=0;
    assert(!sh_audio_link_graph_bus_value(g,0,0,fx,1,&body,&length,error,sizeof(error)) && !body && !length);
    assert(sh_audio_link_graph_bus_value(g,0,0,fx,0,&body,&length,error,sizeof(error)));
    assert(length==5 && u32(body)==74 && body[4]==1);free(body);
    bus_sources sources={&a,0};
    assert(sh_audio_link_graph_root_sources(g,effects[0].node,bus_source,&sources,error,sizeof(error)) && sources.count==2);
    allocator ids={0};ids.fail_after=SIZE_MAX;assert(sh_audio_link_graph_bind(g,assign,&ids,error,sizeof(error)));
    uint32_t bound[3];
    for(size_t i=0;i<3;i++) {
        assert(sh_audio_link_graph_bus_value(g,i,0,fx,1,&body,&length,error,sizeof(error)));
        assert(length==5 && body[4]==1);bound[i]=u32(body);assert(bound[i]!=74);free(body);
        assert(sh_audio_link_graph_bus_value(g,i,0,property,1,&body,&length,error,sizeof(error)));
        assert(length==4 && u32(body)==(i==2 ? 20u : 10u));free(body);
    }
    assert(bound[0]==bound[2] && bound[0]!=bound[1]);
    ids.fail_after=ids.calls;assert(!sh_audio_link_graph_bind(g,assign,&ids,error,sizeof(error)));
    assert(sh_audio_link_graph_bus_value(g,0,0,fx,1,&body,&length,error,sizeof(error)) && u32(body)==bound[0]);free(body);
    /* A dependent output can reject preparation after every bank was bound.
     * Neither pending IDs nor partial values become visible on cancellation. */
    allocator pending={0};pending.count=32;pending.fail_after=SIZE_MAX;
    sh_audio_link_binding *transaction=sh_audio_link_graph_prepare_bind(g,assign,&pending,error,sizeof(error));assert(transaction);
    assert(sh_audio_link_binding_bus_value(transaction,0,0,fx,&body,&length,error,sizeof(error)));
    uint32_t next=u32(body);assert(next!=bound[0]);free(body);
    a.bank.fail=1;assert(!sh_audio_link_binding_bus_value(transaction,0,0,fx,&body,&length,error,sizeof(error)));
    assert(!body && !length);a.bank.fail=0;sh_audio_link_graph_cancel_bind(transaction);
    assert(sh_audio_link_graph_bus_value(g,0,0,fx,1,&body,&length,error,sizeof(error)) && u32(body)==bound[0]);free(body);
    transaction=sh_audio_link_graph_prepare_bind(g,assign,&pending,error,sizeof(error));assert(transaction);
    assert(sh_audio_link_graph_bus_value(g,0,0,fx,1,&body,&length,error,sizeof(error)) && u32(body)==bound[0]);free(body);
    sh_audio_link_graph_commit_bind(transaction);
    assert(sh_audio_link_graph_bus_value(g,0,0,fx,1,&body,&length,error,sizeof(error)) && u32(body)==next);free(body);
    assert(u32(a.bank.data+a.codec.bus_records[fx].offset)==74);
    sh_audio_graph_scope reversed[]={scope(&copy),scope(&b),scope(&a)};
    sh_audio_link_graph *other=sh_audio_link_graph_open(reversed,3,error,sizeof(error));assert(other);
    sh_audio_graph_root moved;assert(sh_audio_link_graph_bus_record(other,2,0,fx,&moved));
    sh_audio_graph_comparison *comparison=sh_audio_link_graph_compare(g,other,error,sizeof(error));assert(comparison);
    assert(!memcmp(effects[0].identity,moved.identity,32) && sh_audio_link_graph_compared(comparison,effects[0].node,moved.node));
    sh_audio_link_graph_comparison_close(comparison);sh_audio_link_graph_close(other);sh_audio_link_graph_close(g);
    sh_audio_bank_codec_free(&a.codec);sh_audio_bank_codec_free(&b.codec);sh_audio_bank_codec_free(&copy.codec);
}
static void absence_and_failure(void)
{
    fixture a, b; make(&a, 42, 1, 111, 123); make(&b, 42, 1, 222, 0);
    sh_audio_graph_scope scopes[] = {scope(&a), scope(&b)}; char error[512];
    a.media_present = 0;
    assert(!sh_audio_link_graph_open(scopes, 2, error, sizeof(error)) && strstr(error, "streamed media 900"));
    a.media_present = 1; a.media_error = 1;
    assert(!sh_audio_link_graph_open(scopes, 2, error, sizeof(error)) && strstr(error, "fixture media IO"));
    a.media_error = 0;
    sh_audio_link_graph *g = sh_audio_link_graph_open(scopes, 2, error, sizeof(error)); assert(g);
    allocator ids = {0}; ids.fail_after = SIZE_MAX;
    assert(sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)) && ids.absent == 1);
    uint32_t sound, parent; check_output(g, 0, 0, &a, &sound, &parent); assert(parent && parent != sound && parent != 123);
    sh_audio_link_graph_close(g);
    /* A missing played node remains required, unlike its optional parent. */
    size_t target = (size_t)a.codec.objects[1].offset + 6; a.bank.data[target] = 124;
    sh_audio_bank_codec_free(&a.codec); assert(sh_audio_bank_codec_read(source(&a.bank), &a.codec, error, sizeof(error)) == 1);
    assert(!sh_audio_link_graph_open(scopes, 2, error, sizeof(error)) && strstr(error, "audio-node 124"));
    sh_audio_bank_codec_free(&a.codec); sh_audio_bank_codec_free(&b.codec);
}
static void originals_and_order(void)
{
    fixture a, b; make(&a, 42, 0, 111, 0); make(&b, 43, 0, 222, 0); a.input.original = 1;
    sh_audio_graph_scope original = scope(&a); char error[512];
    sh_audio_link_graph *g = sh_audio_link_graph_open(&original, 1, error, sizeof(error)); assert(g && root(g, 0).original);
    allocator ids = {0}; ids.fail_after = SIZE_MAX;
    assert(sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)) && !sh_audio_link_graph_bank(g, 0, 0)->count);
    sh_audio_link_graph_close(g); a.input.original = 0;
    sh_audio_graph_bank forward[] = {a.input, b.input}, backward[] = {b.input, a.input};
    sh_audio_graph_scope scopes[] = {{forward, 2, NULL, NULL}, {backward, 2, NULL, NULL}};
    g = sh_audio_link_graph_open(scopes, 2, error, sizeof(error)); if (!g) fprintf(stderr, "%s\n", error); assert(g);
    sh_audio_graph_root first = root(g, 0), second = root(g, 1);
    assert(!sh_audio_link_graph_equal(g, first.node, second.node));
    ids = (allocator){0}; ids.fail_after = SIZE_MAX;
    assert(sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)));
    uint32_t sound1, sound2, parent;
    check_output(g, 0, 0, &a, &sound1, &parent); check_output(g, 0, 1, &b, &sound2, &parent);
    assert(sound1 == sound2); /* Both native definitions retain their one scoped registry identity. */
    sh_audio_link_graph_close(g); sh_audio_bank_codec_free(&a.codec); sh_audio_bank_codec_free(&b.codec);
}
static void set32(unsigned char *p, uint32_t value)
{ for (size_t i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (i * 8)); }
static void unrelated_native_event(void)
{
    fixture native, edited; make(&native, 42, 0, 111, 0); make(&edited, 42, 0, 111, 0);
    native.input.original = 1;
    size_t hirc = (size_t)edited.codec.chunks[3].offset;
    assert(!strcmp(edited.codec.chunks[3].tag, "HIRC"));
    put(&edited.bank, 4, 1); put(&edited.bank, 8, 4); put(&edited.bank, 51, 4); put(&edited.bank, 0, 4);
    set32(edited.bank.data + hirc + 4, 108); set32(edited.bank.data + hirc + 8, 4);
    sh_audio_bank_codec_free(&edited.codec); char error[512];
    assert(sh_audio_bank_codec_read(source(&edited.bank), &edited.codec, error, sizeof(error)) == 1);
    edited.input.source = source(&edited.bank);
    sh_audio_graph_scope scopes[] = {scope(&native), scope(&edited)};
    sh_audio_link_graph *g = sh_audio_link_graph_open(scopes, 2, error, sizeof(error)); assert(g);
    assert(root(g, 0).original && root(g, 1).original);
    allocator ids = {0}; ids.fail_after = SIZE_MAX;
    assert(sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)));
    uint32_t bank, event; assert(sh_audio_link_graph_bank_id(g, 1, 42, &bank) && bank != 42);
    assert(sh_audio_link_graph_event_id(g, 1, 50, &event) && event == 50);
    bytes out = {0};
    assert(sh_audio_bank_binding_write(source(&edited.bank), sh_audio_link_graph_bank(g, 1, 0),
        write_bytes, &out, error, sizeof(error)));
    /* A shared native action must keep the native preparation/release bank. */
    assert(u32(out.data + edited.codec.objects[1].offset + 14) == 42);
    out = (bytes){0}; sh_audio_bank_codec projected = {0};
    assert(sh_audio_bank_binding_write(source(&edited.bank), sh_audio_link_graph_private_bank(g, 1, 0),
        write_bytes, &out, error, sizeof(error)));
    assert(sh_audio_bank_codec_read(source(&out), &projected, error, sizeof(error)) == 1);
    assert(projected.object_count == 1 && projected.objects[0].kind == 4 && projected.objects[0].id != 51);
    assert(projected.chunk_count == 2 && out.size == 49); /* Header plus new event; no duplicate native media. */
    sh_audio_bank_codec_free(&projected);
    sh_audio_link_graph_close(g); sh_audio_bank_codec_free(&native.codec); sh_audio_bank_codec_free(&edited.codec);
}
static void music_choice(fixture *f, int define_choice)
{
    bytes body = {0}; bytes *bank = &f->bank; char error[512];
    size_t hierarchy = 0;
    for (size_t i = 0; i < f->codec.chunk_count; i++)
        if (!strcmp(f->codec.chunks[i].tag, "HIRC")) hierarchy = (size_t)f->codec.chunks[i].offset;
    assert(hierarchy && hierarchy + 8 + u32(bank->data + hierarchy + 4) == bank->size);
    set32(bank->data + f->codec.objects[1].offset + 6, 400);
    if (define_choice) set32(bank->data + f->codec.objects[2].offset, 789);
    put(&body, 400, 4); zero(&body, 1); zero(&body, 28); /* Music node. */
    zero(&body, 4); zero(&body, 23); zero(&body, 4); zero(&body, 4);
    zero(&body, 1); zero(&body, 4); put(&body, 12, 4); zero(&body, 1);
    zero(&body, 4); put(&body, 789, 4); put(&body, 50, 2); put(&body, 100, 2);
    put(bank, 12, 1); put(bank, (uint32_t)body.size, 4);
    memcpy(bank->data + bank->size, body.data, body.size); bank->size += body.size;
    set32(bank->data + hierarchy + 4, (uint32_t)(bank->size - hierarchy - 8));
    set32(bank->data + hierarchy + 8, 4);
    sh_audio_bank_codec_free(&f->codec);
    assert(sh_audio_bank_codec_read(source(bank), &f->codec, error, sizeof(error)) == 1);
    f->input.source = source(bank);
}
static void music_absence(void)
{
    fixture absent, present; make(&absent, 42, 0, 111, 0); make(&present, 42, 0, 111, 0);
    music_choice(&absent, 0); music_choice(&present, 1);
    sh_audio_graph_scope scopes[] = {scope(&absent), scope(&present)}; char error[512];
    sh_audio_link_graph *g = sh_audio_link_graph_open(scopes, 2, error, sizeof(error));
    if (!g) fprintf(stderr, "%s\n", error); assert(g);
    sh_audio_graph_root a = root(g, 0), b = root(g, 1);
    assert(!sh_audio_link_graph_equal(g, a.node, b.node));
    allocator ids = {0}; ids.fail_after = SIZE_MAX;
    assert(sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)) && ids.absent == 1);
    uint32_t choices[2], sounds[2];
    for (size_t i = 0; i < 2; i++) {
        fixture *f = i ? &present : &absent; bytes output = {0}; sh_audio_bank_codec codec = {0};
        assert(sh_audio_bank_binding_write(source(&f->bank), sh_audio_link_graph_bank(g, i, 0),
            write_bytes, &output, error, sizeof(error)));
        assert(sh_audio_bank_codec_read(source(&output), &codec, error, sizeof(error)) == 1);
        const sh_audio_bank_object *music = codec.objects + 3;
        choices[i] = u32(output.data + music->offset + music->length - 8);
        sounds[i] = codec.objects[2].id;
        assert(u32(output.data + codec.objects[1].offset + 6) == music->id);
        sh_audio_bank_codec_free(&codec);
    }
    assert(choices[0] && choices[0] != 789 && choices[0] != choices[1]);
    assert(choices[0] != sounds[0] && choices[0] != sounds[1] && choices[1] == sounds[1]);
    sh_audio_link_graph_close(g);
    /* An exact vanilla bank must retain native optional lookup semantics;
     * a private peer cannot rename that reference or occupy its original ID. */
    absent.input.original = 1;
    g = sh_audio_link_graph_open(scopes, 2, error, sizeof(error)); assert(g && root(g, 0).original);
    ids = (allocator){0}; ids.fail_after = SIZE_MAX;
    assert(sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)));
    assert(!sh_audio_link_graph_bank(g, 0, 0)->count);
    bytes output = {0}; sh_audio_bank_codec codec = {0};
    assert(sh_audio_bank_binding_write(source(&present.bank), sh_audio_link_graph_bank(g, 1, 0),
        write_bytes, &output, error, sizeof(error)));
    assert(sh_audio_bank_codec_read(source(&output), &codec, error, sizeof(error)) == 1);
    assert(codec.objects[2].id != 789); sh_audio_bank_codec_free(&codec);
    sh_audio_link_graph_close(g); sh_audio_bank_codec_free(&absent.codec); sh_audio_bank_codec_free(&present.codec);
}
typedef struct source_visit { const char *path; uint64_t offset, length; size_t calls; int refuse; } source_visit;
static int visit_source(void *context, const char *path, uint64_t offset, uint64_t length)
{
    source_visit *visit = context;
    assert(!strcmp(path, visit->path) && offset == visit->offset && length == visit->length);
    visit->calls++; return !visit->refuse;
}
static void source_ownership(void)
{
    bytes banks[2] = {0}; sh_audio_bank_codec codecs[2] = {0}; sh_audio_graph_bank inputs[2];
    sh_audio_graph_scope scopes[2]; char error[512];
    for (size_t i = 0; i < 2; i++) {
        bytes *bank = banks + i;
        chunk(bank, "BKHD", 16); put(bank, 113, 4); put(bank, 42, 4); zero(bank, 8);
        chunk(bank, "HIRC", 30); put(bank, 2, 4);
        put(bank, 4, 1); put(bank, 8, 4); put(bank, 100, 4); zero(bank, 4);
        put(bank, 4, 1); put(bank, 8, 4); put(bank, 101 + (uint32_t)i, 4); zero(bank, 4);
        assert(sh_audio_bank_codec_read(source(bank), codecs + i, error, sizeof(error)) == 1);
        inputs[i] = (sh_audio_graph_bank){i ? "b.bnk" : "a.bnk", source(bank), codecs + i, !i};
        scopes[i] = (sh_audio_graph_scope){inputs + i, 1, NULL, NULL};
    }
    sh_audio_link_graph *g = sh_audio_link_graph_open(scopes, 2, error, sizeof(error)); assert(g);
    sh_audio_graph_root a, b;
    assert(sh_audio_link_graph_event(g, 0, 100, &a, error, sizeof(error)) &&
        sh_audio_link_graph_event(g, 1, 100, &b, error, sizeof(error)) &&
        sh_audio_link_graph_equal(g, a.node, b.node) && a.original && b.original);
    for (size_t i = 0; i < 2; i++) {
        source_visit visit = {inputs[i].path, codecs[i].objects[0].offset, codecs[i].objects[0].length, 0, 0};
        assert(sh_audio_link_graph_sources(g, i, 100, visit_source, &visit, error, sizeof(error)) && visit.calls == 1);
        visit.refuse = 1;
        assert(!sh_audio_link_graph_sources(g, i, 100, visit_source, &visit, error, sizeof(error)));
        /* Refusing a delivery visitor cannot mutate the captured graph. */
        assert(sh_audio_link_graph_equal(g, a.node, b.node));
    }
    sh_audio_link_graph_close(g);
    sh_audio_bank_codec_free(codecs); sh_audio_bank_codec_free(codecs + 1);
}

typedef struct native_ids { uint32_t busy; size_t calls, fail_at; int fail, original; } native_ids;
static int occupied(void *context, sh_audio_identity_domain domain, uint32_t id, char *error, size_t capacity)
{
    native_ids *native = context; (void)domain; native->calls++;
    if (native->fail || native->calls == native->fail_at) { snprintf(error, capacity, "native inventory unavailable in fixture"); return -1; }
    return native->busy == id ? (native->original ? 2 : 1) : 0;
}
static void original_capture_closures(void)
{
    fixture a,b,private;make(&a,42,0,111,0);make(&b,42,0,111,0);make(&private,42,0,222,0);
    music_choice(&a,0);music_choice(&b,1);music_choice(&private,1);a.input.original=b.input.original=1;
    sh_audio_graph_scope scopes[]={scope(&a),scope(&b),scope(&private)};char error[512];
    sh_audio_link_graph *g=sh_audio_link_graph_open(scopes,3,error,sizeof(error));assert(g);
    sh_audio_graph_root left=root(g,0),right=root(g,1),edited=root(g,2);
    assert(left.original && right.original && !edited.original && !sh_audio_link_graph_equal(g,left.node,right.node));
    native_ids native={0};sh_audio_id_pool *pool=sh_audio_id_pool_open(occupied,&native);assert(pool);
    sh_audio_id_lease *lease=sh_audio_id_lease_begin(pool);assert(lease);
    assert(sh_audio_link_graph_bind(g,sh_audio_id_reserve,lease,error,sizeof(error)));
    uint32_t id=0;assert(sh_audio_link_graph_event_id(g,0,50,&id) && id==50);
    assert(sh_audio_link_graph_event_id(g,1,50,&id) && id==50);
    assert(sh_audio_link_graph_event_id(g,2,50,&id) && id!=50);
    assert(!sh_audio_link_graph_bank(g,0,0)->count && !sh_audio_link_graph_bank(g,1,0)->count);
    assert(sh_audio_link_graph_bank(g,2,0)->count && sh_audio_id_lease_ready(lease,error,sizeof(error)));
    sh_audio_id_lease_activate(lease);
    /* A later provider with the smaller vanilla capture shares only native
     * names; the previous provider and its private variant stay retained. */
    sh_audio_link_graph *other=sh_audio_link_graph_open(scopes,1,error,sizeof(error));assert(other);
    sh_audio_id_lease *next=sh_audio_id_lease_begin(pool);assert(next);
    assert(sh_audio_link_graph_bind(other,sh_audio_id_reserve,next,error,sizeof(error)));
    assert(sh_audio_id_lease_ready(next,error,sizeof(error)) && !sh_audio_link_graph_bank(other,0,0)->count);
    sh_audio_id_lease_release(lease);sh_audio_link_graph_close(g);
    assert(sh_audio_id_lease_ready(next,error,sizeof(error)));sh_audio_id_lease_release(next);
    sh_audio_link_graph_close(other);assert(sh_audio_id_pool_close(pool));
    sh_audio_bank_codec_free(&a.codec);sh_audio_bank_codec_free(&b.codec);sh_audio_bank_codec_free(&private.codec);
}
typedef struct absence_lease { sh_audio_id_lease *lease; uint32_t absent; } absence_lease;
static int reserve_absence(void *context, const sh_audio_link_graph *graph, size_t node,
    sh_audio_identity_domain domain, uint32_t authored, const unsigned char binding[32],
    int original, int absent, uint32_t *id, char *error, size_t capacity)
{
    absence_lease *capture = context;
    int ok = sh_audio_id_reserve(capture->lease, graph, node, domain, authored, binding, original, absent, id, error, capacity);
    if (ok && absent) capture->absent = *id;
    return ok;
}
static void retained_reservations(void)
{
    fixture a, b, c;
    make(&a, 42, 1, 111, 100); make(&b, 42, 1, 111, 100); make(&c, 42, 1, 222, 100);
    sh_audio_graph_scope inputs[] = {scope(&a), scope(&b), scope(&c)};
    char error[512];
    sh_audio_link_graph *ga = sh_audio_link_graph_open(inputs, 1, error, sizeof(error));
    sh_audio_link_graph *gb = sh_audio_link_graph_open(inputs + 1, 1, error, sizeof(error));
    sh_audio_link_graph *gc = sh_audio_link_graph_open(inputs + 2, 1, error, sizeof(error));
    assert(ga && gb && gc);
    sh_audio_graph_root ra = root(ga, 0), rb = root(gb, 0), rc = root(gc, 0);
    sh_audio_graph_comparison *ab = sh_audio_link_graph_compare(ga, gb, error, sizeof(error));
    sh_audio_graph_comparison *ac = sh_audio_link_graph_compare(ga, gc, error, sizeof(error));
    assert(ab && ac && sh_audio_link_graph_compared(ab, ra.node, rb.node));
    assert(!sh_audio_link_graph_compared(ac, ra.node, rc.node) && !sh_audio_link_graph_compared(ab, SIZE_MAX, rb.node));
    sh_audio_link_graph_comparison_close(ab); sh_audio_link_graph_comparison_close(ac);
    sh_audio_link_graph *larger = sh_audio_link_graph_open(inputs + 1, 2, error, sizeof(error)); assert(larger);
    sh_audio_graph_comparison *mixed = sh_audio_link_graph_compare(ga, larger, error, sizeof(error)); assert(mixed);
    assert(sh_audio_link_graph_compared(mixed, ra.node, root(larger, 0).node));
    assert(!sh_audio_link_graph_compared(mixed, ra.node, root(larger, 1).node));
    sh_audio_link_graph_comparison_close(mixed);
    mixed = sh_audio_link_graph_compare(larger, ga, error, sizeof(error)); assert(mixed);
    assert(sh_audio_link_graph_compared(mixed, root(larger, 0).node, ra.node));
    sh_audio_link_graph_comparison_close(mixed); sh_audio_link_graph_close(larger);
    mixed = sh_audio_link_graph_compare(ga, ga, error, sizeof(error)); assert(mixed);
    assert(sh_audio_link_graph_compared(mixed, ra.node, ra.node)); sh_audio_link_graph_comparison_close(mixed);
    a.bank.fail = 1;
    assert(!sh_audio_link_graph_compare(ga, gb, error, sizeof(error)) && strstr(error, "cannot compare"));
    a.bank.fail = 0;
    /* Positive fingerprint matches still read the retained bytes. This injected
     * mutation violates the immutable-source contract and must never dedupe. */
    b.media.data[0] ^= 1;
    assert(!sh_audio_link_graph_compare(ga, gb, error, sizeof(error)) && strstr(error, "unequal bytes"));
    b.media.data[0] ^= 1;
    native_ids native = {0}; sh_audio_id_pool *pool = sh_audio_id_pool_open(occupied, &native); assert(pool);
    sh_audio_id_lease *la = sh_audio_id_lease_begin(pool), *lb = sh_audio_id_lease_begin(pool), *lc = sh_audio_id_lease_begin(pool);
    assert(la && lb && lc && !sh_audio_id_pool_close(pool));
    assert(sh_audio_link_graph_bind(ga, sh_audio_id_reserve, la, error, sizeof(error)));
    uint32_t ia, ib, ic;
    assert(sh_audio_link_graph_event_id(ga, 0, 50, &ia) && ia != 50);
    assert(sh_audio_id_lease_ready(la, error, sizeof(error)));
    native.busy = ia;
    assert(!sh_audio_id_lease_ready(la, error, sizeof(error))); /* Foreign occupant before commit. */
    native.busy = 0; assert(sh_audio_id_lease_ready(la, error, sizeof(error)));
    sh_audio_id_lease_activate(la); native.busy = ia;
    assert(sh_audio_id_lease_ready(la, error, sizeof(error)));
    assert(sh_audio_link_graph_bind(gb, sh_audio_id_reserve, lb, error, sizeof(error)));
    assert(sh_audio_link_graph_event_id(gb, 0, 50, &ib) && ib == ia);
    uint32_t media_a, media_b;
    assert(sh_audio_link_graph_media_id(ga, 0, 900, &media_a));
    assert(sh_audio_link_graph_media_id(gb, 0, 900, &media_b) && media_a == media_b);
    assert(sh_audio_id_lease_private(la, SH_AUDIO_ID_MEDIA, media_a));
    assert(sh_audio_id_lease_private(lb, SH_AUDIO_ID_MEDIA, media_b)); /* Shared is still owned. */
    assert(!sh_audio_id_lease_private(lb, SH_AUDIO_ID_BANK, media_b));
    assert(!sh_audio_id_lease_private(lb, SH_AUDIO_ID_MEDIA, 900));
    assert(!sh_audio_id_lease_private(NULL, SH_AUDIO_ID_MEDIA, media_b));
    assert(sh_audio_id_lease_ready(lb, error, sizeof(error)));
    native.original = 1;
    assert(!sh_audio_id_lease_ready(la, error, sizeof(error)));
    assert(!sh_audio_id_lease_ready(lb, error, sizeof(error))); /* An active peer cannot authorize a newly installed original. */
    native.original = 0;
    assert(sh_audio_id_lease_ready(lb, error, sizeof(error)));
    assert(sh_audio_link_graph_bind(gc, sh_audio_id_reserve, lc, error, sizeof(error)));
    assert(sh_audio_link_graph_event_id(gc, 0, 50, &ic) && ic != ia);
    sh_audio_id_lease_release(lc); /* Failed later preparation rolls back only this candidate. */
    assert(sh_audio_id_lease_ready(la, error, sizeof(error)));
    lc = sh_audio_id_lease_begin(pool); assert(lc); native.fail_at = native.calls + 2;
    assert(!sh_audio_link_graph_bind(gc, sh_audio_id_reserve, lc, error, sizeof(error)));
    uint32_t old; assert(sh_audio_link_graph_event_id(gc, 0, 50, &old) && old == ic);
    assert(native.calls == native.fail_at); sh_audio_id_lease_release(lc); native.fail_at = 0;
    lc = sh_audio_id_lease_begin(pool); assert(lc);
    assert(sh_audio_link_graph_bind(gc, sh_audio_id_reserve, lc, error, sizeof(error)));
    assert(sh_audio_link_graph_event_id(gc, 0, 50, &old) && old == ic);
    /* The pool must stop borrowing a retired representative graph while an
     * equivalent provider still owns the same identities. */
    sh_audio_id_lease_activate(lb);
    sh_audio_id_lease_release(la); sh_audio_link_graph_close(ga); ga = NULL;
    assert(sh_audio_id_lease_private(lb, SH_AUDIO_ID_MEDIA, media_b));
    assert(sh_audio_id_lease_ready(lb, error, sizeof(error)));
    native.fail = 1; assert(!sh_audio_id_lease_ready(lb, error, sizeof(error)));
    native.fail = 0; assert(sh_audio_id_lease_ready(lb, error, sizeof(error)));
    assert(!sh_audio_link_graph_bind(gb, sh_audio_id_reserve, lb, error, sizeof(error))); /* Published leases cannot grow. */
    sh_audio_id_lease_release(lc);
    native.busy = 0; sh_audio_id_lease_release(lb);
    /* An unowned native occupant cannot be mistaken for an exact retained peer. */
    native.busy = ia; lb = sh_audio_id_lease_begin(pool); assert(lb);
    assert(sh_audio_link_graph_bind(gb, sh_audio_id_reserve, lb, error, sizeof(error)));
    assert(sh_audio_link_graph_event_id(gb, 0, 50, &ib) && ib != ia);
    sh_audio_id_lease_release(lb); assert(sh_audio_id_pool_close(pool));
    sh_audio_link_graph_close(gb); sh_audio_link_graph_close(gc);
    sh_audio_bank_codec_free(&a.codec); sh_audio_bank_codec_free(&b.codec); sh_audio_bank_codec_free(&c.codec);
}
static void reservation_absence_and_originals(void)
{
    fixture f; make(&f, 42, 0, 111, 0); music_choice(&f, 0);
    sh_audio_graph_scope input = scope(&f); char error[512]; native_ids native = {0};
    sh_audio_id_pool *pool = sh_audio_id_pool_open(occupied, &native); assert(pool);
    sh_audio_link_graph *graph = sh_audio_link_graph_open(&input, 1, error, sizeof(error)); assert(graph);
    absence_lease capture = {sh_audio_id_lease_begin(pool), 0}; assert(capture.lease);
    assert(sh_audio_link_graph_bind(graph, reserve_absence, &capture, error, sizeof(error)) && capture.absent);
    assert(sh_audio_id_lease_ready(capture.lease, error, sizeof(error))); sh_audio_id_lease_activate(capture.lease);
    for (int domain = 0; domain <= SH_AUDIO_ID_BANK; domain++)
        assert(!sh_audio_id_lease_private(capture.lease, (sh_audio_identity_domain)domain, capture.absent));
    native.busy = capture.absent;
    assert(!sh_audio_id_lease_ready(capture.lease, error, sizeof(error)));
    native.busy = 0; sh_audio_id_lease_release(capture.lease); sh_audio_link_graph_close(graph);
    f.input.original = 1;
    graph = sh_audio_link_graph_open(&input, 1, error, sizeof(error)); assert(graph);
    sh_audio_id_lease *lease = sh_audio_id_lease_begin(pool); assert(lease); native.busy = 50;
    assert(sh_audio_link_graph_bind(graph, sh_audio_id_reserve, lease, error, sizeof(error)));
    uint32_t event; assert(sh_audio_link_graph_event_id(graph, 0, 50, &event) && event == 50);
    assert(sh_audio_id_lease_ready(lease, error, sizeof(error)) && !sh_audio_link_graph_bank(graph, 0, 0)->count);
    assert(!sh_audio_id_lease_private(lease, SH_AUDIO_ID_MEDIA, 900));
    sh_audio_id_lease_release(lease); assert(sh_audio_id_pool_close(pool));
    sh_audio_link_graph_close(graph); sh_audio_bank_codec_free(&f.codec);
}
static void reservation_growth(void)
{
    const size_t count = 80;
    fixture *files = calloc(count, sizeof(*files)); sh_audio_graph_scope *scopes = calloc(count, sizeof(*scopes));
    assert(files && scopes);
    for (size_t i = 0; i < count; i++) { make(files + i, 42, 0, (unsigned)i, 0); scopes[i] = scope(files + i); }
    char error[512]; native_ids native = {0};
    sh_audio_id_pool *pool = sh_audio_id_pool_open(occupied, &native);
    sh_audio_id_lease *lease = sh_audio_id_lease_begin(pool);
    sh_audio_link_graph *graph = sh_audio_link_graph_open(scopes, count, error, sizeof(error)); assert(graph && lease);
    assert(sh_audio_link_graph_bind(graph, sh_audio_id_reserve, lease, error, sizeof(error)));
    uint32_t ids[80];
    for (size_t i = 0; i < count; i++) {
        assert(sh_audio_link_graph_event_id(graph, i, 50, ids + i));
        for (size_t j = 0; j < i; j++) assert(ids[i] != ids[j]);
    }
    assert(sh_audio_id_lease_ready(lease, error, sizeof(error)));
    sh_audio_id_lease_release(lease); assert(sh_audio_id_pool_close(pool)); sh_audio_link_graph_close(graph);
    for (size_t i = 0; i < count; i++) sh_audio_bank_codec_free(&files[i].codec);
    free(scopes); free(files);
}
/* A modulator-driven STMG switch names a private HIRC object. Equal cooked
 * bytes whose modulators differ are different shared values; exact copies
 * dedupe, originals certify, and binding relocates only the modulator. */
static void switch_bank(fixture *f, uint32_t bank_id, unsigned tone, unsigned type)
{
    memset(f, 0, sizeof(*f)); bytes *b = &f->bank;
    chunk(b, "BKHD", 16); put(b, 113, 4); put(b, bank_id, 4); zero(b, 8);
    chunk(b, "HIRC", 22); put(b, 1, 4);
    put(b, 22, 1); put(b, 13, 4); put(b, 555, 4); put(b, 1, 1); put(b, 7, 1); put(b, tone, 4); zero(b, 1); zero(b, 2);
    chunk(b, "STMG", 43); put(b, 0, 4); put(b, 64, 2); zero(b, 4); put(b, 1, 4);
    put(b, 700, 4); put(b, 555, 4); put(b, type, 1); put(b, 1, 4); zero(b, 4); put(b, 3, 4); put(b, 4, 4);
    zero(b, 4);
    char error[512]; assert(sh_audio_bank_codec_read(source(b), &f->codec, error, sizeof(error)) == 1);
    f->input = (sh_audio_graph_bank){"sound/init.bnk", source(b), &f->codec, 0};
}
static size_t switch_record(const fixture *f)
{
    for (size_t i = 0; i < f->codec.shared_record_count; i++)
        if (f->codec.shared_records[i].kind == SH_AUDIO_SHARED_SWITCH) return i;
    assert(0); return SIZE_MAX;
}
static uint32_t bound_controller(sh_audio_link_graph *g, size_t scope_id, const fixture *f)
{
    unsigned char *body = NULL; size_t length = 0; char error[512];
    assert(sh_audio_link_graph_shared_value(g, scope_id, 0, switch_record(f), 1, &body, &length, error, sizeof(error)));
    assert(length == 25); uint32_t controller = u32(body + 4); free(body); return controller;
}
static void shared_switch_modulators(void)
{
    fixture a, b, copy, native; char error[512];
    switch_bank(&a, 42, 1, 2); switch_bank(&b, 42, 2, 2); switch_bank(&copy, 42, 1, 2); switch_bank(&native, 42, 1, 2);
    native.input.original = 1;
    int private_controller = 0;
    for (size_t i = 0; i < a.codec.span_count; i++)
        if (a.codec.spans[i].value == 555 && a.codec.spans[i].domain == SH_AUDIO_ID_MODULATOR &&
            !(a.codec.spans[i].flags & (SH_AUDIO_ID_SHARED | SH_AUDIO_ID_DEFINITION))) private_controller = 1;
    assert(private_controller);
    sh_audio_graph_scope scopes[] = {scope(&a), scope(&b), scope(&copy), scope(&native)};
    sh_audio_link_graph *g = sh_audio_link_graph_open(scopes, 4, error, sizeof(error));
    if (!g) fprintf(stderr, "%s\n", error); assert(g);
    sh_audio_graph_root ra, rb, rc, rn;
    assert(sh_audio_link_graph_shared_record(g, 0, 0, switch_record(&a), &ra));
    assert(sh_audio_link_graph_shared_record(g, 1, 0, switch_record(&b), &rb));
    assert(sh_audio_link_graph_shared_record(g, 2, 0, switch_record(&copy), &rc));
    assert(sh_audio_link_graph_shared_record(g, 3, 0, switch_record(&native), &rn));
    unsigned char *left = NULL, *right = NULL; size_t left_length = 0, right_length = 0;
    assert(sh_audio_link_graph_shared_value(g, 0, 0, switch_record(&a), 0, &left, &left_length, error, sizeof(error)));
    assert(sh_audio_link_graph_shared_value(g, 1, 0, switch_record(&b), 0, &right, &right_length, error, sizeof(error)));
    assert(left_length == right_length && !memcmp(left, right, left_length)); free(left); free(right);
    assert(!sh_audio_link_graph_equal(g, ra.node, rb.node) && memcmp(ra.identity, rb.identity, 32));
    assert(sh_audio_link_graph_equal(g, ra.node, rc.node) && sh_audio_link_graph_equal(g, ra.node, rn.node));
    assert(rn.original && ra.original && !rb.original);
    /* Settings without private references stay independent of the modulator. */
    sh_audio_graph_root ta, tb;
    assert(sh_audio_link_graph_shared_record(g, 0, 0, 0, &ta) && sh_audio_link_graph_shared_record(g, 1, 0, 0, &tb));
    assert(sh_audio_link_graph_equal(g, ta.node, tb.node));
    assert(!sh_audio_link_graph_shared_record(g, 0, 0, a.codec.shared_record_count, &ta));
    allocator ids = {0}; ids.fail_after = SIZE_MAX;
    assert(sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)));
    uint32_t ca = bound_controller(g, 0, &a), cb = bound_controller(g, 1, &b), cc = bound_controller(g, 2, &copy);
    uint32_t cn = bound_controller(g, 3, &native);
    assert(ca == 555 && cn == 555 && cc == 555 && cb != 555);
    /* The bound value names the same identity as the bank's bound modulator. */
    bytes out = {0}; sh_audio_bank_codec codec = {0};
    assert(sh_audio_bank_binding_write(source(&b.bank), sh_audio_link_graph_bank(g, 1, 0), write_bytes, &out, error, sizeof(error)));
    assert(sh_audio_bank_codec_read(source(&out), &codec, error, sizeof(error)) == 1);
    assert(codec.object_count == 1 && codec.objects[0].kind == 22 && codec.objects[0].id == cb);
    sh_audio_bank_codec_free(&codec);
    /* A prepared transaction exposes its own bound value; cancel keeps the
     * committed view and a failed bind changes nothing. */
    allocator other = {0}; other.fail_after = SIZE_MAX;
    sh_audio_link_binding *pending = sh_audio_link_graph_prepare_bind(g, assign, &other, error, sizeof(error)); assert(pending);
    unsigned char *value = NULL; size_t value_length = 0;
    assert(sh_audio_link_binding_shared_value(pending, 1, 0, switch_record(&b), &value, &value_length, error, sizeof(error)));
    assert(value_length == 25 && u32(value + 4) != 555); free(value);
    sh_audio_link_graph_cancel_bind(pending);
    assert(bound_controller(g, 1, &b) == cb);
    ids.fail_after = ids.calls; assert(!sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)));
    assert(bound_controller(g, 1, &b) == cb);
    sh_audio_link_graph_close(g);
    /* A missing modulator definition is a required gap, never borrowed from a peer. */
    fixture missing; switch_bank(&missing, 43, 1, 2);
    missing.bank.data[24 + 8 + 4 + 1 + 4] = 0x99;
    sh_audio_bank_codec_free(&missing.codec);
    assert(sh_audio_bank_codec_read(source(&missing.bank), &missing.codec, error, sizeof(error)) == 1);
    missing.input.source = source(&missing.bank);
    sh_audio_graph_scope gap[] = {scope(&missing), scope(&a)};
    assert(!sh_audio_link_graph_open(gap, 2, error, sizeof(error)) && strstr(error, "audio-modulator"));
    /* A parameter-driven switch has no private dependency. */
    fixture pa, pb; switch_bank(&pa, 42, 1, 0); switch_bank(&pb, 42, 2, 0);
    sh_audio_graph_scope parameters[] = {scope(&pa), scope(&pb)};
    g = sh_audio_link_graph_open(parameters, 2, error, sizeof(error)); assert(g);
    assert(sh_audio_link_graph_shared_record(g, 0, 0, switch_record(&pa), &ra) &&
        sh_audio_link_graph_shared_record(g, 1, 0, switch_record(&pb), &rb));
    assert(sh_audio_link_graph_equal(g, ra.node, rb.node));
    ids = (allocator){0}; ids.fail_after = SIZE_MAX;
    assert(sh_audio_link_graph_bind(g, assign, &ids, error, sizeof(error)));
    assert(bound_controller(g, 0, &pa) == 555 && bound_controller(g, 1, &pb) == 555);
    sh_audio_link_graph_close(g);
    fixture *all[] = {&a, &b, &copy, &native, &missing, &pa, &pb};
    for (size_t i = 0; i < sizeof(all) / sizeof(*all); i++) sh_audio_bank_codec_free(&all[i]->codec);
}
int main(void)
{
    unrelated_native_event(); variants(0); variants(1); variants(2); absence_and_failure(); originals_and_order(); music_absence();
    source_ownership(); shared_projection_transaction(); bus_bindings();
    retained_reservations(); reservation_absence_and_originals(); reservation_growth();original_capture_closures();
    shared_switch_modulators();
    puts("audio_link_graph_test: passed"); return 0;
}
