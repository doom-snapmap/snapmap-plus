#include "audio_hirc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ah_cursor {
    const unsigned char *body;
    size_t length, at, allocated, list_allocated, shared_allocated, bus_allocated;
    sh_audio_hirc out;
    int status;
    const char *field;
    int bus;
} ah_cursor;

static void ah_skip(ah_cursor *c, uint64_t count)
{
    if (c->status != 1) return;
    if (count > c->length - c->at) { c->status = 0; return; }
    c->at += (size_t)count;
}
static uint32_t ah_number(ah_cursor *c, size_t size)
{
    size_t at = c->at; uint32_t value = 0;
    ah_skip(c, size);
    if (c->status == 1)
        for (size_t i = 0; i < size; i++) value |= (uint32_t)c->body[at + i] << (i * 8);
    return value;
}
static uint32_t ah_u8(ah_cursor *c) { return ah_number(c, 1); }
static uint32_t ah_u16(ah_cursor *c) { return ah_number(c, 2); }
static uint32_t ah_u32(ah_cursor *c) { return ah_number(c, 4); }
static unsigned ah_shared(sh_audio_identity_domain domain)
{
    return domain == SH_AUDIO_ID_BUS || domain == SH_AUDIO_ID_STATE_GROUP ||
        domain == SH_AUDIO_ID_STATE_VALUE || domain == SH_AUDIO_ID_SWITCH_GROUP ||
        domain == SH_AUDIO_ID_SWITCH_VALUE || domain == SH_AUDIO_ID_RTPC ||
        domain == SH_AUDIO_ID_TRIGGER ? SH_AUDIO_ID_SHARED : 0;
}
static void ah_span(ah_cursor *c, size_t offset, uint32_t value,
    sh_audio_identity_domain domain, unsigned flags)
{
    if (c->status != 1) return;
    if (c->out.count == c->allocated) {
        size_t count = c->allocated ? c->allocated * 2 : 32;
        if (count < c->allocated || count > SIZE_MAX / sizeof(*c->out.spans)) { c->status = -1; return; }
        sh_audio_identity_span *grown = realloc(c->out.spans, count * sizeof(*grown));
        if (!grown) { c->status = -1; return; }
        c->out.spans = grown; c->allocated = count;
    }
    c->out.spans[c->out.count++] = (sh_audio_identity_span){offset, value, domain, flags | ah_shared(domain)};
}
static uint32_t ah_ref(ah_cursor *c, sh_audio_identity_domain domain, unsigned flags)
{
    size_t at = c->at; uint32_t value = ah_u32(c);
    ah_span(c, at, value, domain, flags); return value;
}
static void ah_ordered(ah_cursor *c, size_t offset, uint32_t count, unsigned fixed_prefix)
{
    if (c->status != 1) return;
    if (c->out.ordered_count == c->list_allocated) {
        size_t next = c->list_allocated ? c->list_allocated * 2 : 8;
        if (next < c->list_allocated || next > SIZE_MAX / sizeof(*c->out.ordered)) { c->status = -1; return; }
        sh_audio_identity_list *grown = realloc(c->out.ordered, next * sizeof(*grown));
        if (!grown) { c->status = -1; return; }
        c->out.ordered = grown; c->list_allocated = next;
    }
    c->out.ordered[c->out.ordered_count++] = (sh_audio_identity_list){offset, count, fixed_prefix};
}
static void ah_properties(ah_cursor *c, int ranged)
{
    c->field = ranged ? "ranged properties" : "properties";
    ah_skip(c, (uint64_t)ah_u8(c) * (ranged ? 9 : 5));
}
static void ah_bus_record(ah_cursor *c,sh_audio_bus_field field,uint32_t first,
    uint32_t second,size_t offset,size_t length)
{
    if(!c->bus || c->status!=1)return;
    if(offset>c->length || length>c->length-offset){c->status=0;return;}
    if(c->out.bus_record_count==c->bus_allocated) {
        size_t next=c->bus_allocated ? c->bus_allocated*2 : 32;
        if(next<c->bus_allocated || next>SIZE_MAX/sizeof(*c->out.bus_records)){c->status=-1;return;}
        sh_audio_bus_record *rows=realloc(c->out.bus_records,next*sizeof(*rows));
        if(!rows){c->status=-1;return;}c->out.bus_records=rows;c->bus_allocated=next;
    }
    c->out.bus_records[c->out.bus_record_count++]=(sh_audio_bus_record){field,{first,second},offset,length};
}
uint32_t sh_audio_bus_record_mask(const sh_audio_bus_record *record)
{
    return record && (record->field==SH_AUDIO_BUS_FIELD_SETTING ||
        record->field==SH_AUDIO_BUS_FIELD_BYPASS || record->field==SH_AUDIO_BUS_FIELD_FLAGS) ? record->key[1] : 0;
}
static void ah_bus_bits(ah_cursor *c,sh_audio_bus_field field,uint32_t key,size_t at,unsigned bits)
{
    for(unsigned bit=1;bit<=bits;bit<<=1)if(bits&bit)ah_bus_record(c,field,key,bit,at,1);
    if(bits!=255)ah_bus_record(c,field,key,255u^bits,at,1);
}
static void ah_control(ah_cursor *c)
{
    size_t at = c->at; uint32_t value = ah_u32(c), type = ah_u8(c);
    if (type == 0) ah_span(c, at, value, SH_AUDIO_ID_RTPC, 0);
    else if (type == 2) ah_span(c, at, value, SH_AUDIO_ID_MODULATOR, 0);
    else if (c->status == 1) c->status = -2;
}
static void ah_rtpcs(ah_cursor *c)
{
    c->field = "RTPC curves";
    uint32_t count = ah_u16(c);
    for (uint32_t i = 0; c->status == 1 && i < count; i++) {
        size_t start=c->at;
        ah_control(c);
        /* Parameter enums and per-consumer curve keys are scalar ABI values,
         * even when numerically equal to an object ID. */
        size_t mode=c->at;ah_skip(c,1);uint32_t parameter=ah_u8(c),curve=ah_u32(c);ah_skip(c,1);
        ah_skip(c, (uint64_t)ah_u16(c) * 12);
        ah_bus_record(c,SH_AUDIO_BUS_FIELD_RTPC_MODE,parameter,0,mode,1);
        ah_bus_record(c,SH_AUDIO_BUS_FIELD_RTPC_CURVE,parameter,curve,start,c->at-start);
    }
}
static void ah_states(ah_cursor *c)
{
    c->field = "state groups";
    uint32_t count = ah_u32(c);
    for (uint32_t i = 0; c->status == 1 && i < count; i++) {
        uint32_t group=ah_ref(c, SH_AUDIO_ID_STATE_GROUP, 0);size_t mode=c->at;ah_skip(c, 1);
        ah_bus_record(c,SH_AUDIO_BUS_FIELD_STATE_GROUP,group,0,mode,1);
        uint32_t states = ah_u16(c);
        for (uint32_t j = 0; c->status == 1 && j < states; j++) {
            uint32_t state=ah_ref(c, SH_AUDIO_ID_STATE_VALUE, 0);size_t value=c->at;
            ah_ref(c, SH_AUDIO_ID_STATE, 0);
            ah_bus_record(c,SH_AUDIO_BUS_FIELD_STATE,group,state,value,4);
        }
    }
}
static uint32_t ah_effect_ref(ah_cursor *c)
{
    size_t at = c->at; uint32_t value = ah_u32(c), shared = ah_u8(c);
    ah_span(c, at, value, shared ? SH_AUDIO_ID_EFFECT_SHARED : SH_AUDIO_ID_EFFECT_CUSTOM, 0);
    return value;
}
static void ah_effects(ah_cursor *c, int node)
{
    c->field = "effects";
    if (node) ah_skip(c, 1);
    uint32_t count = ah_u8(c);
    if (count) {size_t at=c->at;ah_skip(c, 1);ah_bus_bits(c,SH_AUDIO_BUS_FIELD_BYPASS,0,at,255);}
    for (uint32_t i = 0; c->status == 1 && i < count; i++) {
        unsigned slot=ah_u8(c);size_t at=c->at;uint32_t effect=ah_effect_ref(c);ah_skip(c,1);
        /* The bus reader skips null entries. A non-null entry calls the native
         * setter, which has exactly four slots; other indices fail loading. */
        if(!node && effect && slot>=4 && c->status==1){c->field="bus effect slot";c->status=0;}
        /* Null entries do not clear slots, and the final cooked byte is skipped
         * by the native bus parser. Neither is an assignment to compose. */
        if(effect)ah_bus_record(c,SH_AUDIO_BUS_FIELD_EFFECT,slot,0,at,5);
    }
    if (!node) {size_t at=c->at;ah_effect_ref(c);ah_bus_record(c,SH_AUDIO_BUS_FIELD_MIXER,0,0,at,5);}
}
static void ah_positioning(ah_cursor *c)
{
    c->field = "positioning";
    uint32_t bits = ah_u8(c);
    if (!(bits & 1) || !(bits & 8)) return;
    uint32_t mode = ah_u8(c); ah_ref(c, SH_AUDIO_ID_ATTENUATION, 0);
    if ((mode & 3) == 1) return;
    ah_skip(c, 5);
    ah_skip(c, (uint64_t)ah_u32(c) * 16);
    uint32_t paths = ah_u32(c);
    ah_skip(c, (uint64_t)paths * 20);
}
static void ah_node(ah_cursor *c)
{
    ah_effects(c, 1); c->field = "node ancestry"; ah_skip(c, 1);
    ah_ref(c, SH_AUDIO_ID_BUS, 0);
    /* The native parent lookup attaches only when present. An absent parent
     * is not a missing primary child or a failed output-bus lookup. */
    ah_ref(c, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL); ah_skip(c, 1);
    ah_properties(c, 0); ah_properties(c, 1); ah_positioning(c);
    c->field = "auxiliary buses";
    if (ah_u8(c) & 8) for (size_t i = 0; i < 4; i++) ah_ref(c, SH_AUDIO_ID_BUS, 0);
    c->field = "instance settings"; ah_skip(c, 6); ah_states(c); ah_rtpcs(c);
}
static void ah_source(ah_cursor *c)
{
    c->field = "audio source";
    uint32_t plugin = ah_u32(c), type = plugin & 15, stream = ah_u8(c);
    if (stream > 2) { c->status = 0; return; }
    ah_ref(c, type == 1 ? SH_AUDIO_ID_MEDIA : SH_AUDIO_ID_EFFECT_CUSTOM, 0); ah_skip(c, 5);
    if (type == 2 || type == 5) ah_skip(c, ah_u32(c));
    else if (type != 0 && type != 1) c->status = -2;
}
static void ah_child_fields(ah_cursor *c, unsigned flags)
{
    c->field = "child objects";
    uint32_t count = ah_u32(c);
    for (uint32_t i = 0; c->status == 1 && i < count; i++) ah_ref(c, SH_AUDIO_ID_NODE, flags);
}
static void ah_children(ah_cursor *c) { ah_child_fields(c, 0); }
static void ah_action_exceptions(ah_cursor *c)
{
    c->field = "action exceptions";
    uint32_t count = ah_u32(c);
    for (uint32_t i = 0; c->status == 1 && i < count; i++) {
        size_t at = c->at; uint32_t value = ah_u32(c), bus = ah_u8(c);
        ah_span(c, at, value, bus ? SH_AUDIO_ID_BUS : SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL);
    }
}
static void ah_action(ah_cursor *c)
{
    c->field = "action type and target";
    uint32_t type = ah_u16(c);
    size_t at = c->at; uint32_t value = ah_u32(c), bus = ah_u8(c);
    ah_span(c, at, value, (type >> 8) == 0x1d ? SH_AUDIO_ID_TRIGGER :
        bus ? SH_AUDIO_ID_BUS : SH_AUDIO_ID_NODE, type == 0x0403 ? 0 : SH_AUDIO_ID_OPTIONAL);
    ah_properties(c, 0); ah_properties(c, 1); c->field = "action parameters";
    switch (type) {
    case 0x0403: ah_skip(c, 1); ah_ref(c, SH_AUDIO_ID_BANK, 0); break;
    case 0x0102: case 0x0103:
        ah_skip(c, 1); ah_action_exceptions(c); break;
    case 0x0202: case 0x0302:
        ah_skip(c, 2); ah_action_exceptions(c); break;
    case 0x0a03: case 0x0c02:
        ah_skip(c, 14); ah_action_exceptions(c); break;
    case 0x1204:
        ah_ref(c, SH_AUDIO_ID_STATE_GROUP, 0); ah_ref(c, SH_AUDIO_ID_STATE_VALUE, 0); break;
    case 0x1a02: ah_skip(c, 2); ah_action_exceptions(c); break;
    case 0x1d02: case 0x1d03: break;
    case 0x1e03: ah_skip(c, 14); ah_action_exceptions(c); break;
    default: if (c->status == 1) c->status = -2; break;
    }
}
static void ah_bus_part(ah_cursor *c,sh_audio_bus_part part,size_t start)
{
    if(c->status==1)c->out.bus.parts[part]=(sh_audio_bus_section){start,c->at-start};
}
static void ah_bus(ah_cursor *c)
{
    c->bus=1;
    size_t start=c->at;ah_ref(c, SH_AUDIO_ID_BUS, 0);ah_bus_part(c,SH_AUDIO_BUS_PARENT,start);
    ah_bus_record(c,SH_AUDIO_BUS_FIELD_PARENT,0,0,start,4);
    start=c->at;c->field="properties";uint32_t properties=ah_u8(c);size_t keys=c->at;
    ah_skip(c,properties);
    for(uint32_t i=0;c->status==1 && i<properties;i++) {
        size_t at=c->at;ah_skip(c,4);ah_bus_record(c,SH_AUDIO_BUS_FIELD_PROPERTY,c->body[keys+i],0,at,4);
    }
    ah_bus_part(c,SH_AUDIO_BUS_PROPERTIES,start);
    start=c->at;c->field = "bus settings";
    static const unsigned widths[]={1,1,2,4,1};
    for(uint32_t i=0;i<sizeof(widths)/sizeof(*widths);i++) {
        size_t at=c->at;ah_skip(c,widths[i]);
        if(i==0 || i==4)ah_bus_bits(c,SH_AUDIO_BUS_FIELD_SETTING,i,at,3);
        else if(i==1)ah_bus_bits(c,SH_AUDIO_BUS_FIELD_SETTING,i,at,15);
        else if(i==2) {
            ah_bus_record(c,SH_AUDIO_BUS_FIELD_SETTING,i,0x3ff,at,2);
            ah_bus_record(c,SH_AUDIO_BUS_FIELD_SETTING,i,0xfc00,at,2);
        } else ah_bus_record(c,SH_AUDIO_BUS_FIELD_SETTING,i,0,at,widths[i]);
    }
    ah_bus_part(c,SH_AUDIO_BUS_SETTINGS,start);
    start=c->at;
    for(uint32_t i=0;i<2;i++) {size_t at=c->at;ah_skip(c,4);ah_bus_record(c,SH_AUDIO_BUS_FIELD_DUCK_SETTING,i,0,at,4);}
    uint32_t ducks = ah_u32(c);
    for (uint32_t i = 0; c->status == 1 && i < ducks; i++) {
        uint32_t target=ah_ref(c, SH_AUDIO_ID_BUS, 0);size_t at=c->at;ah_skip(c,14);
        ah_bus_record(c,SH_AUDIO_BUS_FIELD_DUCK,target,0,at,14);
    }
    ah_bus_part(c,SH_AUDIO_BUS_DUCKING,start);
    start=c->at;ah_effects(c, 0);ah_bus_part(c,SH_AUDIO_BUS_EFFECTS,start);
    if(c->status!=1)return;
    start=c->at;ah_skip(c, 1);ah_bus_part(c,SH_AUDIO_BUS_FLAGS,start);
    ah_bus_bits(c,SH_AUDIO_BUS_FIELD_FLAGS,0,start,1);
    start=c->at;ah_rtpcs(c);ah_bus_part(c,SH_AUDIO_BUS_RTPCS,start);
    start=c->at;ah_states(c);ah_bus_part(c,SH_AUDIO_BUS_STATES,start);
    if(c->status==1)c->out.bus.present=1;
}
static void ah_switch(ah_cursor *c)
{
    ah_node(c); c->field = "switch container";
    uint32_t type = ah_u8(c);
    if (type > 1) { c->status = -2; return; }
    ah_ref(c, type ? SH_AUDIO_ID_STATE_GROUP : SH_AUDIO_ID_SWITCH_GROUP, 0);
    ah_ref(c, type ? SH_AUDIO_ID_STATE_VALUE : SH_AUDIO_ID_SWITCH_VALUE, 0);
    ah_skip(c, 1); ah_children(c);
    uint32_t count = ah_u32(c);
    for (uint32_t i = 0; c->status == 1 && i < count; i++) {
        ah_ref(c, type ? SH_AUDIO_ID_STATE_VALUE : SH_AUDIO_ID_SWITCH_VALUE, 0); ah_child_fields(c, SH_AUDIO_ID_OPTIONAL);
    }
    count = ah_u32(c);
    for (uint32_t i = 0; c->status == 1 && i < count; i++) { ah_ref(c, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL); ah_skip(c, 10); }
}
static void ah_layers(ah_cursor *c)
{
    ah_node(c); ah_children(c); c->field = "blend layers";
    uint32_t count = ah_u32(c);
    for (uint32_t i = 0; c->status == 1 && i < count; i++) {
        ah_ref(c, SH_AUDIO_ID_LAYER, SH_AUDIO_ID_DEFINITION); ah_rtpcs(c);
        ah_control(c);
        uint32_t children = ah_u32(c);
        for (uint32_t j = 0; c->status == 1 && j < children; j++) {
            ah_ref(c, SH_AUDIO_ID_NODE, 0); ah_skip(c, (uint64_t)ah_u32(c) * 12);
        }
    }
}
static void ah_attenuation(ah_cursor *c)
{
    c->field = "attenuation";
    if (ah_u8(c)) ah_skip(c, 20);
    ah_skip(c, 7); uint32_t curves = ah_u8(c);
    if (!curves && c->status == 1) { c->status = 0; return; }
    for (uint32_t i = 0; c->status == 1 && i < curves; i++) { ah_skip(c, 1); ah_skip(c, (uint64_t)ah_u16(c) * 12); }
    ah_rtpcs(c);
}
static void ah_plugin(ah_cursor *c)
{
    c->field = "effect plugin"; ah_skip(c, 4); ah_skip(c, ah_u32(c));
    uint32_t media = ah_u8(c);
    for (uint32_t i = 0; c->status == 1 && i < media; i++) { ah_skip(c, 1); ah_ref(c, SH_AUDIO_ID_MEDIA, 0); }
    ah_rtpcs(c); ah_skip(c, (uint64_t)ah_u16(c) * 5);
}
static void ah_music(ah_cursor *c)
{
    ah_skip(c, 1); ah_node(c); ah_children(c);
    c->field = "music timing and stingers"; ah_skip(c, 23);
    uint32_t count = ah_u32(c);
    for (uint32_t i = 0; c->status == 1 && i < count; i++) {
        ah_ref(c, SH_AUDIO_ID_TRIGGER, 0); ah_ref(c, SH_AUDIO_ID_NODE, 0); ah_skip(c, 16);
    }
}
static void ah_transitions(ah_cursor *c)
{
    ah_music(c); c->field = "music transitions";
    uint32_t count = ah_u32(c);
    for (uint32_t i = 0; c->status == 1 && i < count; i++) {
        for (size_t side = 0; c->status == 1 && side < 2; side++) {
            uint32_t targets = ah_u32(c); size_t list_at = c->at; unsigned fixed_prefix = 0;
            for (uint32_t j = 0; c->status == 1 && j < targets; j++) {
                size_t at = c->at; uint32_t value = ah_u32(c);
                /* Native selection treats only the first -1 as wildcard.
                 * Other keys match an existing segment/ancestor; these filters
                 * do not require a node to be loaded just to register a rule. */
                ah_span(c, at, value, SH_AUDIO_ID_NODE,
                    j == 0 && value == UINT32_MAX ? SH_AUDIO_ID_LITERAL : SH_AUDIO_ID_OPTIONAL);
                if (!j && value == UINT32_MAX) fixed_prefix = 1;
            }
            ah_ordered(c, list_at, targets, fixed_prefix);
        }
        ah_skip(c, 45);
        if (ah_u8(c)) { ah_ref(c, SH_AUDIO_ID_NODE, 0); ah_skip(c, 26); }
    }
}
static void ah_music_track(ah_cursor *c)
{
    ah_skip(c, 1); uint32_t count = ah_u32(c);
    /* Each source's last span names its media or custom source object. Retain
     * those domains for playlist references; the playlist's first uint32 is
     * a local subtrack index and its four doubles are ordinary timing. */
    size_t start = c->out.count;
    for (uint32_t i = 0; c->status == 1 && i < count; i++) ah_source(c);
    size_t end = c->out.count;
    c->field = "music track playlist"; count = ah_u32(c);
    for (uint32_t i = 0; c->status == 1 && i < count; i++) {
        ah_skip(c, 4); size_t at = c->at; uint32_t value = ah_u32(c);
        sh_audio_identity_domain domain = SH_AUDIO_ID_MEDIA; int found = value == 0;
        for (size_t j = start; j < end; j++) if (c->out.spans[j].value == value) {
            if (found && domain != c->out.spans[j].domain) { c->status = 0; return; }
            domain = c->out.spans[j].domain; found = 1;
        }
        if (!found && c->status == 1) { c->status = 0; return; }
        ah_span(c, at, value, domain, 0); ah_skip(c, 32);
    }
    if (count) ah_skip(c, 4);
    c->field = "music track automation"; count = ah_u32(c);
    for (uint32_t i = 0; c->status == 1 && i < count; i++) { ah_skip(c, 8); ah_skip(c, (uint64_t)ah_u32(c) * 12); }
    ah_node(c); c->field = "music track switching"; uint32_t type = ah_u8(c);
    if (type > 3 && c->status == 1) { c->status = -2; return; }
    if (type == 3) {
        uint32_t group = ah_u8(c);
        if (group > 1 && c->status == 1) { c->status = -2; return; }
        ah_ref(c, group ? SH_AUDIO_ID_STATE_GROUP : SH_AUDIO_ID_SWITCH_GROUP, 0);
        ah_ref(c, group ? SH_AUDIO_ID_STATE_VALUE : SH_AUDIO_ID_SWITCH_VALUE, 0);
        count = ah_u32(c);
        for (uint32_t i = 0; c->status == 1 && i < count; i++)
            ah_ref(c, group ? SH_AUDIO_ID_STATE_VALUE : SH_AUDIO_ID_SWITCH_VALUE, 0);
        ah_skip(c, 32);
    }
    ah_skip(c, 4);
}
typedef struct ah_tree_visit { uint32_t index, depth; } ah_tree_visit;
static void ah_decision_tree(ah_cursor *c, uint32_t size, const unsigned char *kinds, uint32_t depth)
{
    c->field = "music decision tree";
    size_t start = c->at; ah_skip(c, size);
    if (c->status != 1 || !size) return;
    if (size % 12) { c->status = 0; return; }
    size_t count = size / 12, used = 0, queued = 1;
    if (count > SIZE_MAX / sizeof(ah_tree_visit) || count > SIZE_MAX / sizeof(uint32_t)) { c->status = -1; return; }
    ah_tree_visit *queue = malloc(count * sizeof(*queue));
    uint32_t *seen = malloc(count * sizeof(*seen));
    if (!queue || !seen) { free(queue); free(seen); c->status = -1; return; }
    for (size_t i = 0; i < count; i++) seen[i] = UINT32_MAX;
    queue[0] = (ah_tree_visit){0, 0}; seen[0] = 0;
    while (c->status == 1 && used < queued) {
        ah_tree_visit row = queue[used++]; size_t at = start + (size_t)row.index * 12;
        size_t resume = c->at; c->at = at;
        uint32_t key = ah_u32(c), value = ah_u32(c); c->at = resume;
        if (row.depth) ah_span(c, at, key,
            kinds[row.depth - 1] ? SH_AUDIO_ID_STATE_VALUE : SH_AUDIO_ID_SWITCH_VALUE, 0);
        /* A selected music node may be absent: native playback constructs an
         * empty context in that case. Keep a typed optional reference so an
         * isolated bank cannot acquire a peer's node with the same number. */
        if (row.depth == depth) ah_span(c, at + 4, value, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL);
        else {
            size_t first = value & 65535, children = value >> 16;
            if (!children || first >= count || children > count - first) { c->status = 0; break; }
            for (size_t i = first; i < first + children; i++) {
                if (seen[i] == UINT32_MAX) {
                    seen[i] = row.depth + 1; queue[queued++] = (ah_tree_visit){(uint32_t)i, row.depth + 1};
                } else if (seen[i] != row.depth + 1) { c->status = 0; break; }
            }
        }
    }
    if (c->status == 1 && queued != count) c->status = 0;
    free(queue); free(seen);
}
static void ah_music_switch(ah_cursor *c)
{
    ah_transitions(c); c->field = "music switch groups"; ah_skip(c, 1);
    uint32_t depth = ah_u32(c); size_t groups = c->at;
    ah_skip(c, (uint64_t)depth * 4); size_t kinds_at = c->at; ah_skip(c, depth);
    if (c->status != 1) return;
    const unsigned char *kinds = c->body + kinds_at;
    for (uint32_t i = 0; c->status == 1 && i < depth; i++) {
        if (kinds[i] > 1) { c->status = -2; return; }
        size_t resume = c->at; c->at = groups + (size_t)i * 4;
        ah_ref(c, kinds[i] ? SH_AUDIO_ID_STATE_GROUP : SH_AUDIO_ID_SWITCH_GROUP, 0); c->at = resume;
    }
    uint32_t size = ah_u32(c); ah_skip(c, 1); ah_decision_tree(c, size, kinds, depth);
}

void sh_audio_hirc_free(sh_audio_hirc *object)
{ if (object) { free(object->spans); free(object->ordered); free(object->shared); free(object->bus_records); memset(object, 0, sizeof(*object)); } }

static void ah_shared_record(ah_cursor *c,sh_audio_shared_kind kind,size_t start,
    uint32_t a,uint32_t b,uint32_t d)
{
    if(c->status!=1)return;
    if(c->out.shared_count==c->shared_allocated) {
        size_t next=c->shared_allocated ? c->shared_allocated*2 : 16;
        if(next<c->shared_allocated || next>SIZE_MAX/sizeof(*c->out.shared)){c->status=-1;return;}
        sh_audio_shared_record *grown=realloc(c->out.shared,next*sizeof(*grown));
        if(!grown){c->status=-1;return;}c->out.shared=grown;c->shared_allocated=next;
    }
    c->out.shared[c->out.shared_count++]=(sh_audio_shared_record){kind,{a,b,d},start,c->at-start};
}

int sh_audio_shared_chunk_read(const char *tag, const void *body, size_t length,
    sh_audio_hirc *out, char *error, size_t capacity)
{
    ah_cursor c = {0}; c.body = body; c.length = length; c.status = 1; c.field = "shared settings";
    if (error && capacity) *error = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!body && length) c.status = 0;
    else if (!tag) c.status = -2;
    else if (!strcmp(tag, "STMG")) {
        if (length) {
            ah_skip(&c, 4); ah_shared_record(&c,SH_AUDIO_SHARED_THRESHOLD,0,0,0,0);
            ah_skip(&c, 2); ah_shared_record(&c,SH_AUDIO_SHARED_VOICES,4,0,0,0);
            uint32_t count = ah_u32(&c);
            for (uint32_t i = 0; c.status == 1 && i < count; i++) {
                size_t start=c.at;uint32_t group=ah_ref(&c, SH_AUDIO_ID_STATE_GROUP, 0); ah_skip(&c, 4);
                ah_shared_record(&c,SH_AUDIO_SHARED_GROUP,start,group,0,0);
                uint32_t transitions = ah_u32(&c);
                for (uint32_t j = 0; c.status == 1 && j < transitions; j++) {
                    start=c.at;uint32_t from=ah_ref(&c, SH_AUDIO_ID_STATE_VALUE, 0);
                    uint32_t to=ah_ref(&c, SH_AUDIO_ID_STATE_VALUE, 0); ah_skip(&c, 4);
                    ah_shared_record(&c,SH_AUDIO_SHARED_TRANSITION,start,group,from,to);
                }
            }
            count = ah_u32(&c);
            for (uint32_t i = 0; c.status == 1 && i < count; i++) {
                size_t start=c.at;uint32_t group=ah_ref(&c, SH_AUDIO_ID_SWITCH_GROUP, 0);
                size_t controller_at=c.at;uint32_t controller=ah_u32(&c),type=ah_u8(&c);
                uint32_t points=ah_u32(&c);ah_skip(&c,(uint64_t)points*12);
                /* The native reader does not call the setter for an empty curve. */
                if(points) {
                    /* The native subscription dispatch selects modulators only
                     * for type 2; every other byte uses the parameter manager. */
                    ah_span(&c,controller_at,controller,type==2 ? SH_AUDIO_ID_MODULATOR : SH_AUDIO_ID_RTPC,0);
                    ah_shared_record(&c,SH_AUDIO_SHARED_SWITCH,start,group,0,0);
                }
            }
            count = ah_u32(&c);
            for (uint32_t i = 0; c.status == 1 && i < count; i++) {
                size_t start=c.at;uint32_t id=ah_ref(&c, SH_AUDIO_ID_RTPC, 0); ah_skip(&c, 16);
                ah_shared_record(&c,SH_AUDIO_SHARED_RTPC,start,id,0,0);
                start=c.at;unsigned selector=ah_u8(&c);
                if(selector>6)c.status=0;
                /* Zero leaves the previous binding unchanged, unlike the four
                 * preceding values which replace their previous values. */
                if(selector)ah_shared_record(&c,SH_AUDIO_SHARED_BUILTIN,start,id,0,0);
            }
        }
    } else if (!strcmp(tag, "ENVS")) {
        for (size_t i = 0; c.status == 1 && i < 6; i++) {
            size_t start=c.at;ah_skip(&c, 2); ah_skip(&c, (uint64_t)ah_u16(&c) * 12);
            ah_shared_record(&c,SH_AUDIO_SHARED_ENVIRONMENT,start,(uint32_t)i,0,0);
        }
    } else if (!strcmp(tag, "PLAT")) {
        if (length) { ah_skip(&c, ah_u32(&c));ah_shared_record(&c,SH_AUDIO_SHARED_PLATFORM,0,0,0,0); }
    } else c.status = -2;
    if (c.status == 1 && c.at != length) c.status = -2;
    if (c.status == 1) { *out = c.out; return 1; }
    if (error && capacity) snprintf(error, capacity, "audio shared chunk %s at byte %zu: %s",
        tag ? tag : "", c.at, c.status == -1 ? "allocation failed" : c.status == -2 ? "unverified grammar" : "malformed field");
    sh_audio_hirc_free(&c.out); return c.status;
}

int sh_audio_hirc_read(unsigned kind, const void *body, size_t length,
    sh_audio_hirc *out, char *error, size_t capacity)
{
    static const sh_audio_identity_domain definitions[] = {
        SH_AUDIO_ID_NODE, SH_AUDIO_ID_STATE, SH_AUDIO_ID_NODE, SH_AUDIO_ID_ACTION, SH_AUDIO_ID_EVENT,
        SH_AUDIO_ID_NODE, SH_AUDIO_ID_NODE, SH_AUDIO_ID_NODE, SH_AUDIO_ID_BUS, SH_AUDIO_ID_NODE,
        SH_AUDIO_ID_NODE, SH_AUDIO_ID_NODE, SH_AUDIO_ID_NODE, SH_AUDIO_ID_NODE, SH_AUDIO_ID_ATTENUATION,
        SH_AUDIO_ID_NODE, SH_AUDIO_ID_NODE, SH_AUDIO_ID_NODE, SH_AUDIO_ID_EFFECT_SHARED,
        SH_AUDIO_ID_EFFECT_CUSTOM, SH_AUDIO_ID_BUS, SH_AUDIO_ID_MODULATOR, SH_AUDIO_ID_MODULATOR
    };
    ah_cursor c = {0}; c.body = body; c.length = length; c.status = 1; c.field = "object identity";
    if (error && capacity) *error = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!body || length < 4) c.status = 0;
    else if (!kind || kind >= sizeof(definitions) / sizeof(*definitions)) c.status = -2;
    else if (!ah_ref(&c, definitions[kind], SH_AUDIO_ID_DEFINITION) && c.status == 1) c.status = 0;
    if (c.status == 1) switch (kind) {
    case 1: ah_properties(&c, 0); break;
    case 2: ah_source(&c); ah_node(&c); break;
    case 3: ah_action(&c); break;
    case 4: {
        c.field = "event actions"; uint32_t count = ah_u32(&c);
        for (uint32_t i = 0; c.status == 1 && i < count; i++) ah_ref(&c, SH_AUDIO_ID_ACTION, 0);
        break;
    }
    case 5: {
        ah_node(&c); c.field = "random sequence settings"; ah_skip(&c, 24); ah_children(&c);
        c.field = "playlist"; uint32_t count = ah_u16(&c);
        for (uint32_t i = 0; c.status == 1 && i < count; i++) { ah_ref(&c, SH_AUDIO_ID_NODE, SH_AUDIO_ID_OPTIONAL); ah_skip(&c, 4); }
        break;
    }
    case 6: ah_switch(&c); break;
    case 7: ah_node(&c); ah_children(&c); break;
    case 8: case 20: ah_bus(&c); break;
    case 9: ah_layers(&c); break;
    case 10: {
        ah_music(&c); c.field = "music segment markers"; ah_skip(&c, 8);
        uint32_t count = ah_u32(&c);
        for (uint32_t i = 0; c.status == 1 && i < count; i++) { ah_skip(&c, 12); ah_skip(&c, ah_u32(&c)); }
        break;
    }
    case 11: ah_music_track(&c); break;
    case 12: ah_music_switch(&c); break;
    case 13: {
        ah_transitions(&c); c.field = "music playlist"; uint32_t count = ah_u32(&c);
        for (uint32_t i = 0; c.status == 1 && i < count; i++) { ah_ref(&c, SH_AUDIO_ID_NODE, 0); ah_skip(&c, 26); }
        break;
    }
    case 14: ah_attenuation(&c); break;
    case 18: case 19: ah_plugin(&c); break;
    case 21: case 22: ah_properties(&c, 0); ah_properties(&c, 1); ah_rtpcs(&c); break;
    default: c.status = -2; break;
    }
    if (c.status == 1 && c.at != length) { c.field = "unrecognized trailing fields"; c.status = -2; }
    if (c.status == 1) { *out = c.out; return 1; }
    if (error && capacity) snprintf(error, capacity, "audio hierarchy kind %u at byte %zu: %s (%s)",
        kind, c.at, c.status == -1 ? "allocation failed" : c.status == -2 ? "unverified grammar" : "malformed field", c.field);
    sh_audio_hirc_free(&c.out); return c.status;
}
