#include "audio_hirc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct bytes { unsigned char body[4096]; size_t count; } bytes;
static void put(bytes *b, uint32_t value, size_t size)
{
    assert(size <= sizeof(b->body) - b->count);
    for (size_t i = 0; i < size; i++) b->body[b->count++] = (unsigned char)(value >> (i * 8));
}
static void zeros(bytes *b, size_t size)
{ while (size--) put(b, 0, 1); }
static int span(const sh_audio_hirc *o, size_t offset, sh_audio_identity_domain domain, unsigned flags)
{
    for (size_t i = 0; i < o->count; i++) if (o->spans[i].offset == offset)
        return o->spans[i].domain == domain && o->spans[i].flags == flags;
    return 0;
}
static void minimal_node(bytes *b)
{
    zeros(b, 3); put(b, 99, 4); put(b, 100, 4); zeros(b, 5); zeros(b, 6); zeros(b, 6);
}
static void basic(void)
{
    bytes b = {0}; sh_audio_hirc object = {0}; char error[512];
    put(&b, 71, 4); put(&b, 2, 4); put(&b, 72, 4); put(&b, 73, 4);
    assert(sh_audio_hirc_read(4, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(object.count == 3 && span(&object, 0, SH_AUDIO_ID_EVENT, SH_AUDIO_ID_DEFINITION));
    assert(span(&object, 8, SH_AUDIO_ID_ACTION, 0) && span(&object, 12, SH_AUDIO_ID_ACTION, 0));
    sh_audio_hirc_free(&object);
    for (size_t i = 0; i < b.count; i++) {
        assert(sh_audio_hirc_read(4, b.body, i, &object, error, sizeof(error)) == 0);
        assert(!object.spans && !object.count && *error);
    }
    b.body[4] = 255;
    assert(sh_audio_hirc_read(4, b.body, b.count, &object, error, sizeof(error)) == 0);
    assert(sh_audio_hirc_read(250, b.body, b.count, &object, error, sizeof(error)) == -2);

    /* A value equal to the media ID also occurs as an ordinary media-size
     * scalar. Only the typed identity field receives a span. */
    b.count = 0; put(&b, 100, 4); put(&b, 0x40001, 4); put(&b, 1, 1);
    put(&b, 900, 4); put(&b, 900, 4); put(&b, 0, 1); minimal_node(&b);
    unsigned char before[4096]; memcpy(before, b.body, b.count);
    assert(sh_audio_hirc_read(2, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(object.count == 4 && span(&object, 9, SH_AUDIO_ID_MEDIA, 0));
    assert(span(&object, 21, SH_AUDIO_ID_BUS, SH_AUDIO_ID_SHARED));
    assert(span(&object, 25, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL));
    assert(!memcmp(before, b.body, b.count)); sh_audio_hirc_free(&object);
    for (size_t i = 0; i < b.count; i++) {
        assert(sh_audio_hirc_read(2, b.body, i, &object, error, sizeof(error)) != 1);
        assert(!object.spans && !object.count);
    }

    /* State names are shared controls, while their typed HIRC state bodies
     * remain independent objects. RTPC type 2 points at a modulator. */
    b.count = 0; put(&b, 100, 4); put(&b, 0x40001, 4); put(&b, 1, 1);
    put(&b, 900, 4); put(&b, 0, 4); put(&b, 0, 1); minimal_node(&b);
    b.count -= 6; put(&b, 1, 4); size_t group = b.count; put(&b, 200, 4); put(&b, 0, 1); put(&b, 1, 2);
    size_t state = b.count; put(&b, 201, 4); put(&b, 202, 4); put(&b, 1, 2);
    size_t rtpc = b.count; put(&b, 300, 4); put(&b, 2, 1); zeros(&b, 9);
    assert(sh_audio_hirc_read(2, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(span(&object, group, SH_AUDIO_ID_STATE_GROUP, SH_AUDIO_ID_SHARED));
    assert(span(&object, state, SH_AUDIO_ID_STATE_VALUE, SH_AUDIO_ID_SHARED));
    assert(span(&object, state + 4, SH_AUDIO_ID_STATE, 0));
    assert(span(&object, rtpc, SH_AUDIO_ID_MODULATOR, 0)); sh_audio_hirc_free(&object);
    b.body[rtpc + 4] = 3;
    assert(sh_audio_hirc_read(2, b.body, b.count, &object, error, sizeof(error)) == -2);
}

static void buses(void)
{
    bytes b={0};sh_audio_hirc object={0};char error[512];size_t offsets[SH_AUDIO_BUS_PARTS];
    put(&b,71,4);offsets[SH_AUDIO_BUS_PARENT]=b.count;put(&b,72,4);
    offsets[SH_AUDIO_BUS_PROPERTIES]=b.count;
    /* Property IDs precede the contiguous value array; they are not interleaved. */
    put(&b,2,1);put(&b,1,1);put(&b,7,1);put(&b,100,4);put(&b,200,4);
    offsets[SH_AUDIO_BUS_SETTINGS]=b.count;zeros(&b,9);
    offsets[SH_AUDIO_BUS_DUCKING]=b.count;put(&b,50,4);put(&b,60,4);put(&b,1,4);put(&b,73,4);zeros(&b,14);
    offsets[SH_AUDIO_BUS_EFFECTS]=b.count;put(&b,2,1);put(&b,5,1);
    size_t first_slot=b.count;put(&b,0,1);put(&b,74,4);put(&b,1,1);put(&b,0,1);
    put(&b,3,1);put(&b,75,4);put(&b,0,1);put(&b,1,1);put(&b,76,4);put(&b,0,1);
    offsets[SH_AUDIO_BUS_FLAGS]=b.count;put(&b,1,1);
    offsets[SH_AUDIO_BUS_RTPCS]=b.count;put(&b,1,2);put(&b,77,4);put(&b,2,1);
    put(&b,3,1);put(&b,12,1);put(&b,88,4);put(&b,0,1);put(&b,1,2);zeros(&b,12);
    offsets[SH_AUDIO_BUS_STATES]=b.count;put(&b,1,4);put(&b,78,4);put(&b,0,1);put(&b,1,2);put(&b,79,4);put(&b,80,4);
    unsigned char before[4096];memcpy(before,b.body,b.count);
    for(unsigned kind=8;kind<=20;kind+=12) {
        assert(sh_audio_hirc_read(kind,b.body,b.count,&object,error,sizeof(error))==1 && object.bus.present);
        size_t cursor=4;
        for(size_t i=0;i<SH_AUDIO_BUS_PARTS;i++) {
            const sh_audio_bus_section *part=object.bus.parts+i;
            assert(part->offset==offsets[i] && part->offset==cursor && part->length);
            cursor+=part->length;
        }
        assert(cursor==b.count && !memcmp(before,b.body,b.count));
        assert(object.bus.parts[SH_AUDIO_BUS_PROPERTIES].length==11);
        assert(object.bus.parts[SH_AUDIO_BUS_SETTINGS].length==9);
        assert(object.bus.parts[SH_AUDIO_BUS_DUCKING].length==30);
        assert(span(&object,first_slot+1,SH_AUDIO_ID_EFFECT_SHARED,0));
        assert(span(&object,first_slot+8,SH_AUDIO_ID_EFFECT_CUSTOM,0));
        assert(span(&object,offsets[SH_AUDIO_BUS_RTPCS]+2,SH_AUDIO_ID_MODULATOR,0));
        assert(span(&object,b.count-4,SH_AUDIO_ID_STATE,0));
        size_t properties=0,effects=0,curves=0,states=0;
        for(size_t i=0;i<object.bus_record_count;i++) {
            const sh_audio_bus_record *r=object.bus_records+i;
            assert(r->offset<=b.count && r->length<=b.count-r->offset);
            if(r->field==SH_AUDIO_BUS_FIELD_PROPERTY) {
                assert(r->key[0]==(properties ? 7u : 1u));
                assert(r->length==4 && b.body[r->offset]==(properties ? 200 : 100));properties++;
            } else if(r->field==SH_AUDIO_BUS_FIELD_EFFECT) {
                assert(r->key[0]==(effects ? 3u : 0u) && r->length==5);effects++;
            } else if(r->field==SH_AUDIO_BUS_FIELD_RTPC_CURVE) {
                assert(r->key[0]==12 && r->key[1]==88 && r->length==26 && b.body[r->offset]==77);curves++;
            } else if(r->field==SH_AUDIO_BUS_FIELD_RTPC_MODE) {
                assert(r->key[0]==12 && r->length==1 && b.body[r->offset]==3);
            } else if(r->field==SH_AUDIO_BUS_FIELD_STATE) {
                assert(r->key[0]==78 && r->key[1]==79 && r->length==4 && b.body[r->offset]==80);states++;
            }
        }
        assert(properties==2 && effects==2 && curves==1 && states==1);
        sh_audio_hirc_free(&object);
        for(size_t i=0;i<b.count;i++) {
            assert(sh_audio_hirc_read(kind,b.body,i,&object,error,sizeof(error))!=1);
            assert(!object.bus.present && !object.spans && !object.bus_records && !object.bus_record_count);
        }
        b.body[first_slot]=4;
        assert(sh_audio_hirc_read(kind,b.body,b.count,&object,error,sizeof(error))==0);
        assert(strstr(error,"bus effect slot") && !object.bus.present && !object.spans);
        /* The native reader skips a null effect even for an out-of-range slot. */
        b.body[first_slot+1]=0;
        assert(sh_audio_hirc_read(kind,b.body,b.count,&object,error,sizeof(error))==1);
        effects=0;for(size_t i=0;i<object.bus_record_count;i++)if(object.bus_records[i].field==SH_AUDIO_BUS_FIELD_EFFECT) {
            assert(object.bus_records[i].key[0]==3);effects++;
        }
        assert(effects==1);
        sh_audio_hirc_free(&object);b.body[first_slot]=0;b.body[first_slot+1]=74;
        /* Repeating a valid slot is native ordered assignment, not malformed. */
        b.body[first_slot+7]=0;
        assert(sh_audio_hirc_read(kind,b.body,b.count,&object,error,sizeof(error))==1);
        sh_audio_hirc_free(&object);b.body[first_slot+7]=3;
    }
}
static uint32_t u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void actions(void)
{
    bytes b = {0}; sh_audio_hirc object = {0}; char error[512];
    put(&b, 5, 4); put(&b, 0x0403, 2); put(&b, 71, 4); zeros(&b, 3);
    put(&b, 4, 1); put(&b, 72, 4);
    assert(sh_audio_hirc_read(3, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(object.count == 3 && span(&object, 0, SH_AUDIO_ID_ACTION, SH_AUDIO_ID_DEFINITION));
    assert(span(&object, 6, SH_AUDIO_ID_NODE, 0) && span(&object, 14, SH_AUDIO_ID_BANK, 0));
    sh_audio_hirc_free(&object);
    for (size_t i = 0; i < b.count; i++) {
        assert(sh_audio_hirc_read(3, b.body, i, &object, error, sizeof(error)) != 1);
        assert(!object.count && !object.spans);
    }
    b.count = 13; b.body[5] = 0x12; b.body[4] = 4;
    put(&b, 72, 4); put(&b, 73, 4);
    assert(sh_audio_hirc_read(3, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(span(&object, 13, SH_AUDIO_ID_STATE_GROUP, SH_AUDIO_ID_SHARED));
    assert(span(&object, 17, SH_AUDIO_ID_STATE_VALUE, SH_AUDIO_ID_SHARED)); sh_audio_hirc_free(&object);
    b.count = 13; b.body[5] = 0x1d; b.body[4] = 2;
    assert(sh_audio_hirc_read(3, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(span(&object, 6, SH_AUDIO_ID_TRIGGER, SH_AUDIO_ID_SHARED | SH_AUDIO_ID_OPTIONAL)); sh_audio_hirc_free(&object);
    b.body[5] = 1; b.body[10] = 1; put(&b, 4, 1); put(&b, 2, 4);
    size_t exceptions = b.count; put(&b, 74, 4); put(&b, 0, 1); put(&b, 75, 4); put(&b, 1, 1);
    assert(sh_audio_hirc_read(3, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(span(&object, 6, SH_AUDIO_ID_BUS, SH_AUDIO_ID_SHARED | SH_AUDIO_ID_OPTIONAL));
    assert(span(&object, exceptions, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL));
    assert(span(&object, exceptions + 5, SH_AUDIO_ID_BUS, SH_AUDIO_ID_SHARED | SH_AUDIO_ID_OPTIONAL)); sh_audio_hirc_free(&object);
    b.body[5] = 0xff;
    assert(sh_audio_hirc_read(3, b.body, b.count, &object, error, sizeof(error)) == -2);
    assert(!object.count && !object.spans);
}
static void optional_children(void)
{
    bytes b = {0}; sh_audio_hirc object = {0}; char error[512];
    put(&b, 400, 4); minimal_node(&b); zeros(&b, 24);
    put(&b, 1, 4); size_t required = b.count; put(&b, 501, 4);
    put(&b, 1, 2); size_t choice = b.count; put(&b, 502, 4); put(&b, 50000, 4);
    assert(sh_audio_hirc_read(5, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(span(&object, required, SH_AUDIO_ID_NODE, 0));
    assert(span(&object, choice, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL)); sh_audio_hirc_free(&object);
    b.count = 0; put(&b, 400, 4); minimal_node(&b); put(&b, 0, 1);
    put(&b, 600, 4); put(&b, 601, 4); put(&b, 0, 1); put(&b, 1, 4);
    required = b.count; put(&b, 501, 4); put(&b, 1, 4); put(&b, 601, 4); put(&b, 1, 4);
    choice = b.count; put(&b, 502, 4); put(&b, 1, 4); size_t settings = b.count;
    put(&b, 502, 4); zeros(&b, 10);
    assert(sh_audio_hirc_read(6, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(span(&object, required, SH_AUDIO_ID_NODE, 0));
    assert(span(&object, choice, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL));
    assert(span(&object, settings, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL)); sh_audio_hirc_free(&object);
}
static void music_base(bytes *b)
{ put(b, 400, 4); put(b, 0, 1); minimal_node(b); zeros(b, 4); zeros(b, 23); zeros(b, 4); }
static void music(void)
{
    bytes b = {0}; sh_audio_hirc object = {0}; char error[512];
    music_base(&b); size_t stingers = b.count - 4; b.body[stingers] = 1;
    size_t trigger = b.count; put(&b, 501, 4); put(&b, 502, 4); zeros(&b, 16);
    zeros(&b, 8); put(&b, 1, 4); size_t marker = b.count;
    put(&b, 502, 4); zeros(&b, 8); put(&b, 4, 4); put(&b, 502, 4);
    assert(sh_audio_hirc_read(10, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(span(&object, trigger, SH_AUDIO_ID_TRIGGER, SH_AUDIO_ID_SHARED));
    assert(span(&object, trigger + 4, SH_AUDIO_ID_NODE, 0));
    assert(!span(&object, marker, SH_AUDIO_ID_NODE, 0) && !span(&object, marker + 16, SH_AUDIO_ID_NODE, 0));
    sh_audio_hirc_free(&object);

    b.count = 0; music_base(&b); zeros(&b, 4); zeros(&b, 1); put(&b, 1, 4);
    size_t group = b.count; put(&b, 600, 4); put(&b, 1, 1); put(&b, 36, 4); zeros(&b, 1);
    size_t tree = b.count;
    put(&b, 0, 4); put(&b, 1, 2); put(&b, 2, 2); zeros(&b, 4);
    put(&b, 601, 4); put(&b, 602, 4); zeros(&b, 4);
    put(&b, 603, 4); put(&b, 604, 4); zeros(&b, 4);
    assert(sh_audio_hirc_read(12, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(span(&object, group, SH_AUDIO_ID_STATE_GROUP, SH_AUDIO_ID_SHARED));
    assert(!span(&object, tree + 4, SH_AUDIO_ID_NODE, 0));
    assert(span(&object, tree + 12, SH_AUDIO_ID_STATE_VALUE, SH_AUDIO_ID_SHARED));
    assert(span(&object, tree + 16, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL));
    assert(span(&object, tree + 28, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL)); sh_audio_hirc_free(&object);
    for (size_t i = 0; i < b.count; i++) {
        assert(sh_audio_hirc_read(12, b.body, i, &object, error, sizeof(error)) != 1);
        assert(!object.count && !object.spans);
    }
    b.body[tree + 4] = 0; /* Self-reference cannot turn an internal record into a leaf. */
    assert(sh_audio_hirc_read(12, b.body, b.count, &object, error, sizeof(error)) == 0);
    b.body[tree + 4] = 1; b.body[tree + 6] = 1; /* Unreachable second leaf. */
    assert(sh_audio_hirc_read(12, b.body, b.count, &object, error, sizeof(error)) == 0);
    b.body[tree + 6] = 3; /* Child range extends beyond the bounded tree bytes. */
    assert(sh_audio_hirc_read(12, b.body, b.count, &object, error, sizeof(error)) == 0);

    /* With no switch groups the root is the selection, including absent IDs.
     * This is not a special-case exemption for any particular numeric ID. */
    b.count = 0; music_base(&b); zeros(&b, 4); zeros(&b, 1); zeros(&b, 4);
    put(&b, 12, 4); zeros(&b, 1); zeros(&b, 4);
    size_t root = b.count; put(&b, 789, 4); put(&b, 50, 2); put(&b, 100, 2);
    assert(sh_audio_hirc_read(12, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(span(&object, root, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL));
    sh_audio_hirc_free(&object);

    b.count = 0; music_base(&b); put(&b, 1, 4);
    put(&b, 2, 4); size_t filter = b.count; put(&b, 701, 4); put(&b, 702, 4);
    put(&b, 1, 4); size_t wildcard = b.count; put(&b, UINT32_MAX, 4);
    zeros(&b, 45); put(&b, 1, 1); size_t transition = b.count;
    put(&b, 703, 4); zeros(&b, 26); zeros(&b, 4);
    assert(sh_audio_hirc_read(13, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(span(&object, filter, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL));
    assert(span(&object, filter + 4, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL));
    assert(span(&object, wildcard, SH_AUDIO_ID_NODE, SH_AUDIO_ID_LITERAL));
    assert(span(&object, transition, SH_AUDIO_ID_NODE, 0));
    sh_audio_hirc_free(&object);

    b.count = 0; put(&b, 400, 4); put(&b, 0, 1); put(&b, 1, 4);
    put(&b, 0x20001, 4); put(&b, 1, 1); put(&b, 900, 4); zeros(&b, 5);
    put(&b, 1, 4); size_t track = b.count; put(&b, 900, 4); put(&b, 900, 4); zeros(&b, 32);
    zeros(&b, 4); zeros(&b, 4); minimal_node(&b); zeros(&b, 5);
    assert(sh_audio_hirc_read(11, b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(!span(&object, track, SH_AUDIO_ID_MEDIA, 0));
    assert(span(&object, track + 4, SH_AUDIO_ID_MEDIA, 0)); sh_audio_hirc_free(&object);
    b.body[track + 4] = 0x85; /* Playlist source absent from this track's source table. */
    assert(sh_audio_hirc_read(11, b.body, b.count, &object, error, sizeof(error)) == 0);
}
static void shared(void)
{
    bytes b = {0}; sh_audio_hirc object = {0}; char error[512];
    zeros(&b, 6); put(&b, 1, 4); size_t group = b.count;
    put(&b, 91, 4); put(&b, 91, 4); put(&b, 1, 4);
    size_t transition = b.count; put(&b, 92, 4); put(&b, 93, 4); put(&b, 91, 4);
    put(&b, 1, 4); size_t control = b.count;
    put(&b, 94, 4); put(&b, 95, 4); put(&b, 0, 1); put(&b, 1, 4); zeros(&b, 12);
    put(&b, 1, 4); size_t parameter = b.count; put(&b, 96, 4); zeros(&b, 17);
    assert(sh_audio_shared_chunk_read("STMG", b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(object.count == 6 && span(&object, group, SH_AUDIO_ID_STATE_GROUP, SH_AUDIO_ID_SHARED));
    assert(!span(&object, group + 4, SH_AUDIO_ID_STATE_GROUP, SH_AUDIO_ID_SHARED));
    assert(span(&object, transition, SH_AUDIO_ID_STATE_VALUE, SH_AUDIO_ID_SHARED));
    assert(span(&object, transition + 4, SH_AUDIO_ID_STATE_VALUE, SH_AUDIO_ID_SHARED));
    assert(span(&object, control, SH_AUDIO_ID_SWITCH_GROUP, SH_AUDIO_ID_SHARED));
    assert(span(&object, control + 4, SH_AUDIO_ID_RTPC, SH_AUDIO_ID_SHARED));
    assert(span(&object, parameter, SH_AUDIO_ID_RTPC, SH_AUDIO_ID_SHARED)); sh_audio_hirc_free(&object);
    for (size_t i = 1; i < b.count; i++) {
        assert(sh_audio_shared_chunk_read("STMG", b.body, i, &object, error, sizeof(error)) != 1);
        assert(!object.count && !object.spans);
    }
    b.count = 0; zeros(&b, 24);
    assert(sh_audio_shared_chunk_read("ENVS", b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(!object.count); sh_audio_hirc_free(&object);
    assert(sh_audio_shared_chunk_read("ENVS", b.body, b.count - 1, &object, error, sizeof(error)) == 0);
    b.count = 0; put(&b, 7, 4); memcpy(b.body + b.count, "Windows", 7); b.count += 7;
    assert(sh_audio_shared_chunk_read("PLAT", b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(!object.count); sh_audio_hirc_free(&object);
    assert(sh_audio_shared_chunk_read("PLAT", b.body, b.count - 1, &object, error, sizeof(error)) == 0);
}
/* The native subscription dispatch uses the modulator registry only for
 * controller type 2; every other type is a shared parameter. An empty curve
 * never reaches the setter, so its controller is not a dependency. */
static void switch_controllers(void)
{
    bytes b = {0}; sh_audio_hirc object = {0}; char error[512];
    zeros(&b, 6); put(&b, 0, 4); put(&b, 3, 4);
    size_t modulated = b.count; put(&b, 700, 4); put(&b, 555, 4); put(&b, 2, 1); put(&b, 1, 4); zeros(&b, 12);
    size_t midi = b.count; put(&b, 701, 4); put(&b, 556, 4); put(&b, 1, 1); put(&b, 1, 4); zeros(&b, 12);
    size_t empty = b.count; put(&b, 702, 4); put(&b, 557, 4); put(&b, 2, 1); put(&b, 0, 4);
    put(&b, 0, 4);
    assert(sh_audio_shared_chunk_read("STMG", b.body, b.count, &object, error, sizeof(error)) == 1);
    assert(span(&object, modulated, SH_AUDIO_ID_SWITCH_GROUP, SH_AUDIO_ID_SHARED));
    assert(span(&object, modulated + 4, SH_AUDIO_ID_MODULATOR, 0));
    assert(span(&object, midi + 4, SH_AUDIO_ID_RTPC, SH_AUDIO_ID_SHARED));
    assert(span(&object, empty, SH_AUDIO_ID_SWITCH_GROUP, SH_AUDIO_ID_SHARED) && !span(&object, empty + 4, SH_AUDIO_ID_MODULATOR, 0));
    size_t switches = 0;
    for (size_t i = 0; i < object.shared_count; i++) if (object.shared[i].kind == SH_AUDIO_SHARED_SWITCH) {
        assert(object.shared[i].key[0] == 700 || object.shared[i].key[0] == 701);
        assert(object.shared[i].length == 25); switches++;
    }
    assert(switches == 2); sh_audio_hirc_free(&object);
}
static int corpus_file(const char *path, size_t *accepted, size_t *pending)
{
    FILE *file = fopen(path, "rb"); unsigned char header[8]; int ok = 1;
    if (!file) return 0;
    while (fread(header, 1, 8, file) == 8) {
        uint32_t size = u32(header + 4); __int64 next = _ftelli64(file) + size;
        if (!memcmp(header, "HIRC", 4)) {
            if (fread(header, 1, 4, file) != 4) { ok = 0; break; }
            uint32_t count = u32(header);
            for (uint32_t i = 0; i < count; i++) {
                __int64 at = _ftelli64(file);
                if (fread(header, 1, 5, file) != 5) { ok = 0; break; }
                unsigned kind = header[0]; size_t length = u32(header + 1);
                unsigned char *body = malloc(length ? length : 1);
                if (!body || fread(body, 1, length, file) != length) { free(body); ok = 0; break; }
                sh_audio_hirc object = {0}; char error[512];
                int status = sh_audio_hirc_read(kind, body, length, &object, error, sizeof(error));
                if (status == 1) {
                    ++accepted[kind];
                    assert(object.bus.present == (kind == 8 || kind == 20));
                    if(object.bus.present) {
                        size_t cursor=4;
                        for(size_t part=0;part<SH_AUDIO_BUS_PARTS;part++) {
                            assert(object.bus.parts[part].offset==cursor && object.bus.parts[part].length);
                            cursor+=object.bus.parts[part].length;
                        }
                        assert(cursor==length);
                        assert(object.bus_record_count && object.bus_records);
                        for(size_t r=0;r<object.bus_record_count;r++) {
                            const sh_audio_bus_record *value=object.bus_records+r;
                            assert(value->length && value->offset<=length && value->length<=length-value->offset);
                        }
                    }
                    else assert(!object.bus_records && !object.bus_record_count);
                }
                else if (status == -2) ++pending[kind];
                else { fprintf(stderr, "%s at %lld: %s\n", path, at, error); ok = 0; }
                sh_audio_hirc_free(&object); free(body); if (!ok) break;
            }
            if (!ok || _ftelli64(file) != next) { ok = 0; break; }
        }
        if (_fseeki64(file, next, SEEK_SET)) { ok = 0; break; }
    }
    fclose(file); return ok;
}
int main(int argc, char **argv)
{
    basic(); actions(); optional_children(); music(); shared(); buses(); switch_controllers();
    if (argc == 2) {
        WIN32_FIND_DATAA entry; char search[4096], path[4096], localized[4096];
        size_t accepted[256] = {0}, pending[256] = {0};
        snprintf(search, sizeof(search), "%s/*.bnk", argv[1]);
        HANDLE find = FindFirstFileA(search, &entry); assert(find != INVALID_HANDLE_VALUE);
        do {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            snprintf(path, sizeof(path), "%s/%s", argv[1], entry.cFileName);
            snprintf(localized, sizeof(localized), "%s/English(US)/%s", argv[1], entry.cFileName);
            assert(corpus_file(GetFileAttributesA(localized) != INVALID_FILE_ATTRIBUTES ? localized : path, accepted, pending));
        } while (FindNextFileA(find, &entry));
        FindClose(find);
        snprintf(search, sizeof(search), "%s/English(US)/*.bnk", argv[1]);
        find = FindFirstFileA(search, &entry);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                snprintf(path, sizeof(path), "%s/%s", argv[1], entry.cFileName);
                if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) continue;
                snprintf(localized, sizeof(localized), "%s/English(US)/%s", argv[1], entry.cFileName);
                assert(corpus_file(localized, accepted, pending));
            } while (FindNextFileA(find, &entry));
            FindClose(find);
        }
        for (size_t i = 0; i < 256; i++) if (accepted[i] || pending[i])
            printf("kind %zu: accepted=%zu pending=%zu\n", i, accepted[i], pending[i]);
    }
    puts("typed audio hierarchy checks passed"); return 0;
}
