#include "audio_link_graph.h"
#include <stdlib.h>
#include <string.h>

/* A symbol owns an ordered construction recipe, not a winning object body.
 * Native loaders reuse some duplicate definitions and partially reparse others.
 * Keeping their order lets the native constructors retain those semantics. */
typedef struct ag_node {
    sh_package_link_node shape;
    char path[96];
    sh_package_link_edge *edges;
    size_t edge_capacity, scope;
    sh_audio_bank_source source;
    const char *source_path;
    uint64_t offset;
    sh_audio_identity_domain domain;
    uint32_t authored;
    uint32_t value_mask;
    unsigned symbol, required, external, original, original_references, native_references;
    const char *required_path;
    size_t required_offset;
} ag_node;
typedef struct ag_slot { size_t scope, node; uint32_t id; unsigned domain; } ag_slot;
typedef struct ag_shadow { size_t symbol, record; } ag_shadow;
typedef struct ag_bank {
    const sh_audio_graph_bank *input;
    size_t scope, record;
    size_t *targets, *bus_nodes, *object_nodes, *shared_nodes;
    unsigned char *native_transport;
    sh_audio_bank_binding binding,private_binding;
    size_t private_kept; /* payload the private projection keeps (records, media, chunks) */
} ag_bank;
struct sh_audio_link_graph {
    ag_node *nodes;
    size_t count, allocated;
    ag_slot *slots;
    size_t slot_count, symbols;
    ag_bank *banks;
    size_t bank_count, scope_count;
    size_t *starts;
    sh_package_link_classes classes;
    unsigned char *originals;
    uint32_t *ids;
    ag_shadow *shadows;
    size_t shadow_count;
};
struct sh_audio_link_binding {
    sh_audio_link_graph *graph;
    uint32_t *ids;
    sh_audio_bank_binding *banks,*private_banks;
    size_t *private_kept;
};

static int ag_fail(char *error, size_t capacity, const char *text)
{ if (error && capacity) snprintf(error, capacity, "%s", text); return 0; }
static uint32_t ag_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int ag_grow(void **rows, size_t *allocated, size_t count, size_t width)
{
    if (count <= *allocated) return 1;
    if (count > SIZE_MAX / width) return 0;
    size_t next = *allocated ? *allocated : 32;
    while (next < count) {
        if (next > SIZE_MAX / width / 2) { next = count; break; }
        next *= 2;
    }
    void *grown = realloc(*rows, next * width);
    if (!grown) return 0;
    *rows = grown; *allocated = next; return 1;
}
static size_t ag_hash(size_t scope, unsigned domain, uint32_t id)
{
    uint64_t key = ((uint64_t)id << 5 | domain) ^ (uint64_t)scope * UINT64_C(0x9e3779b97f4a7c15);
    key = (key ^ (key >> 33)) * UINT64_C(0xff51afd7ed558ccd);
    return (size_t)(key ^ (key >> 33));
}
static ag_slot *ag_slot_at(ag_slot *slots, size_t count, size_t scope, unsigned domain, uint32_t id)
{
    size_t at = ag_hash(scope, domain, id) & (count - 1);
    while (slots[at].node && (slots[at].scope != scope || slots[at].domain != domain || slots[at].id != id))
        at = (at + 1) & (count - 1);
    return &slots[at];
}
static int ag_node_add(sh_audio_link_graph *g, const char *family, uint32_t id,
    size_t scope, size_t *index)
{
    if (g->count == SIZE_MAX || !ag_grow((void **)&g->nodes, &g->allocated, g->count + 1, sizeof(*g->nodes))) return 0;
    *index = g->count++; ag_node *n = &g->nodes[*index]; memset(n, 0, sizeof(*n));
    n->shape.family = family; n->scope = scope; n->authored = id;
    n->original = n->original_references = n->native_references = 1;
    snprintf(n->path, sizeof(n->path), "%u", id); return 1;
}
static int ag_symbol(sh_audio_link_graph *g, size_t scope, sh_audio_identity_domain domain,
    uint32_t id, size_t *index)
{
    static const char *families[] = {"audio-node", "audio-event", "audio-action", "audio-state",
        "audio-attenuation", "audio-effect-shared", "audio-effect-custom", "audio-modulator",
        "audio-layer", "audio-media", "audio-bank"};
    if (!id || domain > SH_AUDIO_ID_BANK) return 0;
    if (!g->slot_count || g->symbols >= g->slot_count / 2) {
        size_t next = g->slot_count ? g->slot_count * 2 : 64;
        if (next < g->slot_count || next > SIZE_MAX / sizeof(*g->slots)) return 0;
        ag_slot *slots = calloc(next, sizeof(*slots)); if (!slots) return 0;
        for (size_t i = 0; i < g->slot_count; i++) if (g->slots[i].node) {
            ag_slot old = g->slots[i]; *ag_slot_at(slots, next, old.scope, old.domain, old.id) = old;
        }
        free(g->slots); g->slots = slots; g->slot_count = next;
    }
    ag_slot *slot = ag_slot_at(g->slots, g->slot_count, scope, domain, id);
    if (slot->node) { *index = slot->node - 1; return 1; }
    if (!ag_node_add(g, families[domain], id, scope, index)) return 0;
    g->nodes[*index].symbol = 1; g->nodes[*index].domain = domain;
    *slot = (ag_slot){scope, *index + 1, id, domain}; g->symbols++; return 1;
}
static size_t ag_find(const sh_audio_link_graph *g, size_t scope, sh_audio_identity_domain domain, uint32_t id)
{
    if (!g || !g->slot_count || scope >= g->scope_count || !id) return SIZE_MAX;
    ag_slot *slot = ag_slot_at(g->slots, g->slot_count, scope, domain, id);
    return slot->node ? slot->node - 1 : SIZE_MAX;
}
static int ag_edge(sh_audio_link_graph *g, size_t owner, const char *slot, size_t target)
{
    ag_node *n = &g->nodes[owner]; size_t count = n->shape.edge_count;
    if (count == SIZE_MAX || !ag_grow((void **)&n->edges, &n->edge_capacity, count + 1, sizeof(*n->edges))) return 0;
    char *name = _strdup(slot); if (!name) return 0;
    n->edges[count] = (sh_package_link_edge){name, target}; n->shape.edge_count++; return 1;
}
/* Hashes are an index only. Every positive source match is checked in bounded
 * reads, including packed media, before it can authorize shared ownership. */
static int ag_read(void *context,uint64_t at,void *output,size_t length)
{
    const ag_node *node=context;
    if(at>node->shape.length || length>node->shape.length-at ||
        !node->source.read(node->source.context,node->offset+at,output,length))return 0;
    if(node->value_mask) {
        if(node->shape.length>4)return 0;
        for(size_t i=0;i<length;i++)((unsigned char *)output)[i]&=(unsigned char)(node->value_mask>>(8*(at+i)));
    }
    return 1;
}
static int ag_bytes_equal(const ag_node *a, const ag_node *b)
{
    unsigned char left[8192], right[8192];
    if (a->value_mask!=b->value_mask || a->shape.length != b->shape.length || memcmp(a->shape.content, b->shape.content, 32)) return 0;
    for (uint64_t at = 0; at < a->shape.length;) {
        size_t length = a->shape.length - at > sizeof(left) ? sizeof(left) : (size_t)(a->shape.length - at);
        if (!ag_read((void *)a,at,left,length) || !ag_read((void *)b,at,right,length)) return -1;
        if (memcmp(left, right, length)) return 0;
        at += length;
    }
    return 1;
}
static int ag_verify(void *context, size_t left, size_t right, char *error, size_t capacity)
{
    sh_audio_link_graph *g = context;
    int same = ag_bytes_equal(&g->nodes[left], &g->nodes[right]);
    return same == 1 ? 1 : ag_fail(error, capacity, same < 0 ?
        "cannot compare captured audio sources" : "audio fingerprint matched unequal captured bytes");
}
static int ag_record(sh_audio_link_graph *g, size_t scope, const char *family, uint32_t id,
    unsigned kind, sh_audio_bank_source source, uint64_t offset, uint64_t length, int original,
    const char *path, size_t *index, char *error, size_t capacity)
{
    sh_package_file_identity identity;
    if (!source.read || offset > source.length || length > source.length - offset ||
        !sh_package_reader_identity(source.context, offset, length, source.read, &identity))
        return ag_fail(error, capacity, "cannot fingerprint captured audio source");
    if (!ag_node_add(g, family, id, scope, index)) return ag_fail(error, capacity, "cannot allocate audio graph");
    ag_node *n = &g->nodes[*index]; n->source = source; n->offset = offset; n->original = !!original;
    n->source_path = path;
    n->shape.length = length; memcpy(n->shape.content, identity.digest, 32);
    n->shape.semantics[0] = (unsigned char)kind; return 1;
}
static int ag_definition(sh_audio_link_graph *g, size_t symbol, size_t record, char *error, size_t capacity)
{
    ag_node *n = &g->nodes[symbol];
    /* Repeating the exact immediately preceding cooked definition cannot
     * introduce another variant. A/B/A is retained: A may reparse B's source. */
    if (n->shape.edge_count) {
        ag_node *previous = &g->nodes[n->edges[n->shape.edge_count - 1].target], *next = &g->nodes[record];
        if (!strcmp(previous->shape.family, next->shape.family) &&
            !memcmp(previous->shape.semantics, next->shape.semantics, 32)) {
            int same = ag_bytes_equal(previous, next);
            if (same < 0) return ag_fail(error, capacity, "cannot compare repeated audio definition");
            if (same) { previous->original &= next->original; return 1; }
        }
    }
    char slot[40]; snprintf(slot, sizeof(slot), "definition/%020llu", (unsigned long long)n->shape.edge_count);
    return ag_edge(g, symbol, slot, record) || ag_fail(error, capacity, "cannot allocate audio definition edge");
}
static int ag_fields(sh_audio_link_graph *g, ag_bank *bank, size_t record,
    uint64_t base, size_t first, size_t count)
{
    const sh_audio_bank_codec *codec = bank->input->codec;
    for (size_t i = first; i < first + count; i++) if (bank->targets[i] != SIZE_MAX) {
        /* A bank locator is not the sound's construction recipe. Native Play
         * actions keep their original preparation/release bank. Private actions
         * acquire the complete bank dependency after native witnesses resolve. */
        if (codec->spans[i].domain == SH_AUDIO_ID_BANK &&
            !(codec->spans[i].flags & SH_AUDIO_ID_DEFINITION)) continue;
        char slot[40]; snprintf(slot, sizeof(slot), "field/%020llu",
            (unsigned long long)(codec->spans[i].offset - base));
        if (!ag_edge(g, record, slot, bank->targets[i])) return 0;
    }
    return 1;
}
static int ag_bus_records(sh_audio_link_graph *g,ag_bank *bank,const sh_audio_bank_object *object,
    char *error,size_t capacity)
{
    const sh_audio_bank_codec *codec=bank->input->codec;
    if(object->first_bus_record>codec->bus_record_count ||
        object->bus_record_count>codec->bus_record_count-object->first_bus_record)
        return ag_fail(error,capacity,"audio bus records exceed checked bank");
    for(size_t i=object->first_bus_record;i<object->first_bus_record+object->bus_record_count;i++) {
        const sh_audio_bus_record *field=codec->bus_records+i;size_t record;
        if(bank->bus_nodes[i]!=SIZE_MAX || (object->kind!=8 && object->kind!=20) || field->offset<object->offset ||
            field->offset-object->offset>object->length ||
            field->length>object->length-(field->offset-object->offset))
            return ag_fail(error,capacity,"audio bus value exceeds checked object");
        if(!ag_record(g,bank->scope,"audio-bus-value",object->id,object->kind,bank->input->source,
            field->offset,field->length,bank->input->original,bank->input->path,&record,error,capacity))return 0;
        bank->bus_nodes[i]=record;
        uint32_t mask=sh_audio_bus_record_mask(field);
        if(mask) {
            if(!field->length || field->length>4 || (field->length<4 && mask>>(8*field->length)))
                return ag_fail(error,capacity,"audio bus value mask exceeds checked field");
            ag_node *node=g->nodes+record;sh_package_file_identity identity;node->value_mask=mask;
            for(unsigned byte=0;byte<4;byte++)node->shape.semantics[byte+1]=(unsigned char)(mask>>(8*byte));
            if(!sh_package_reader_identity(node,0,node->shape.length,ag_read,&identity))
                return ag_fail(error,capacity,"cannot fingerprint masked audio bus value");
            memcpy(node->shape.content,identity.digest,32);
        }
        snprintf(g->nodes[record].path,sizeof(g->nodes[record].path),"%u/%u/%u/%u",
            object->id,(unsigned)field->field,field->key[0],field->key[1]);
        for(size_t j=object->first_span;j<object->first_span+object->span_count;j++) {
            const sh_audio_identity_span *span=codec->spans+j;
            if(bank->targets[j]==SIZE_MAX || span->offset<field->offset ||
                span->offset-field->offset>=field->length)continue;
            if(field->length-(span->offset-field->offset)<4)
                return ag_fail(error,capacity,"audio bus value splits a typed reference");
            if(!ag_fields(g,bank,record,field->offset,j,1))
                return ag_fail(error,capacity,"cannot retain audio bus dependencies");
        }
    }
    return 1;
}
/* Shared STMG/ENVS/PLAT records compose globally, but a modulator-driven switch
 * names a private HIRC object. Each record receives its own exact identity with
 * only the private dependencies inside its span, as bus values do, so equal
 * cooked numbers naming different scoped definitions stay distinct. */
static int ag_shared_records(sh_audio_link_graph *g,ag_bank *bank,char *error,size_t capacity)
{
    const sh_audio_bank_codec *codec=bank->input->codec;
    for(size_t c=0;c<codec->chunk_count;c++) {
        const sh_audio_bank_chunk *chunk=codec->chunks+c;
        if(!chunk->shared_record_count)continue;
        if(chunk->first_shared_record>codec->shared_record_count ||
            chunk->shared_record_count>codec->shared_record_count-chunk->first_shared_record)
            return ag_fail(error,capacity,"audio shared records exceed checked bank");
        uint64_t body=chunk->offset+8,end=body+chunk->length;size_t first=SIZE_MAX,last=0;
        /* One chunk's spans are appended together while the codec reads it. */
        for(size_t j=0;j<codec->span_count;j++)if(codec->spans[j].offset>=body && codec->spans[j].offset<end) {
            if(first==SIZE_MAX)first=j;last=j;
        }
        for(size_t r=chunk->first_shared_record;r<chunk->first_shared_record+chunk->shared_record_count;r++) {
            const sh_audio_shared_record *field=codec->shared_records+r;size_t record;
            if(bank->shared_nodes[r]!=SIZE_MAX || field->offset<body || field->offset>end ||
                field->length>end-field->offset)
                return ag_fail(error,capacity,"audio shared record exceeds checked chunk");
            if(!ag_record(g,bank->scope,"audio-shared-value",field->key[0],(unsigned)field->kind+1u,
                bank->input->source,field->offset,field->length,bank->input->original,bank->input->path,
                &record,error,capacity))return 0;
            ag_node *node=g->nodes+record;
            for(unsigned k=0;k<3;k++)for(unsigned byte=0;byte<4;byte++)
                node->shape.semantics[1+k*4+byte]=(unsigned char)(field->key[k]>>(8*byte));
            memcpy(node->shape.semantics+13,chunk->tag,4);
            snprintf(node->path,sizeof(node->path),"%s/%u/%u/%u/%u",chunk->tag,(unsigned)field->kind,
                field->key[0],field->key[1],field->key[2]);
            bank->shared_nodes[r]=record;
            for(size_t j=first;first!=SIZE_MAX && j<=last;j++) {
                const sh_audio_identity_span *span=codec->spans+j;
                if(bank->targets[j]==SIZE_MAX || span->offset<field->offset ||
                    span->offset-field->offset>=field->length)continue;
                if(field->length-(span->offset-field->offset)<4)
                    return ag_fail(error,capacity,"audio shared record splits a typed reference");
                if(!ag_fields(g,bank,record,field->offset,j,1))
                    return ag_fail(error,capacity,"cannot retain audio shared dependencies");
            }
        }
    }
    for(size_t r=0;r<codec->shared_record_count;r++)if(bank->shared_nodes[r]==SIZE_MAX)
        return ag_fail(error,capacity,"audio shared record has no owning chunk");
    return 1;
}
static int ag_is_name(const sh_audio_bank_codec *codec, size_t offset)
{
    for (size_t i = 0; i < codec->name_count; i++) if (codec->names[i].offset == offset) return 1;
    return 0;
}
static int ag_bank_scan(sh_audio_link_graph *g, ag_bank *bank, char *error, size_t capacity)
{
    const sh_audio_graph_bank *input = bank->input; const sh_audio_bank_codec *codec = input->codec;
    if (!codec || !codec->metadata.id || codec->span_count > SIZE_MAX / sizeof(*bank->targets) ||
        codec->bus_record_count > SIZE_MAX / sizeof(*bank->bus_nodes))
        return ag_fail(error, capacity, "audio graph needs a checked bank inventory");
    bank->targets = malloc((codec->span_count ? codec->span_count : 1) * sizeof(*bank->targets));
    bank->native_transport = calloc(codec->span_count ? codec->span_count : 1, 1);
    bank->bus_nodes = malloc((codec->bus_record_count ? codec->bus_record_count : 1) * sizeof(*bank->bus_nodes));
    if(codec->object_count>SIZE_MAX/sizeof(*bank->object_nodes) ||
        codec->shared_record_count>SIZE_MAX/sizeof(*bank->shared_nodes))return ag_fail(error,capacity,"audio object count overflows");
    bank->object_nodes=malloc((codec->object_count ? codec->object_count : 1)*sizeof(*bank->object_nodes));
    bank->shared_nodes=malloc((codec->shared_record_count ? codec->shared_record_count : 1)*sizeof(*bank->shared_nodes));
    if (!bank->targets || !bank->native_transport || !bank->bus_nodes || !bank->object_nodes || !bank->shared_nodes)
        return ag_fail(error, capacity, "cannot allocate audio fields");
    for(size_t i=0;i<codec->bus_record_count;i++)bank->bus_nodes[i]=SIZE_MAX;
    for(size_t i=0;i<codec->shared_record_count;i++)bank->shared_nodes[i]=SIZE_MAX;
    for (size_t i = 0; i < codec->span_count; i++) {
        const sh_audio_identity_span *f = &codec->spans[i]; bank->targets[i] = SIZE_MAX;
        if (!f->value || f->flags & (SH_AUDIO_ID_SHARED | SH_AUDIO_ID_LITERAL)) continue;
        if (!ag_symbol(g, bank->scope, f->domain, f->value, &bank->targets[i]))
            return ag_fail(error, capacity, "cannot allocate scoped audio identity");
        ag_node *n = &g->nodes[bank->targets[i]];
        if (!(f->flags & SH_AUDIO_ID_DEFINITION)) {
            n->original_references &= !!input->original; n->native_references &= !!input->native;
        }
        if (!(f->flags & (SH_AUDIO_ID_DEFINITION | SH_AUDIO_ID_OPTIONAL)) && !ag_is_name(codec, f->offset) && !n->required) {
            n->required = 1; n->required_path = input->path; n->required_offset = f->offset;
        }
        if (f->domain == SH_AUDIO_ID_MEDIA && !(f->flags & SH_AUDIO_ID_DEFINITION)) {
            const sh_audio_bank_media *media = sh_audio_bank_find_media(&codec->metadata, f->value);
            if (media && media->stream) n->external = 1;
        }
    }
    if (!ag_record(g, bank->scope, "audio-bank-source", codec->metadata.id, 0,
        input->source, 0, input->source.length, input->original, input->path, &bank->record, error, capacity)) return 0;
    size_t symbol = ag_find(g, bank->scope, SH_AUDIO_ID_BANK, codec->metadata.id);
    if (symbol == SIZE_MAX || !ag_definition(g, symbol, bank->record, error, capacity)) return 0;
    if (!ag_fields(g, bank, bank->record, 0, 0, codec->span_count)) goto memory;
    for (size_t i = 0; i < codec->object_count; i++) {
        const sh_audio_bank_object *object = &codec->objects[i]; size_t record;
        if (object->first_span > codec->span_count || object->span_count > codec->span_count - object->first_span)
            return ag_fail(error, capacity, "audio object fields exceed checked bank");
        if (!ag_record(g, bank->scope, "audio-object-source", object->id, object->kind, input->source,
            object->offset, object->length, input->original, input->path, &record, error, capacity)) return 0;
        bank->object_nodes[i]=record;
        if (!ag_fields(g, bank, record, object->offset, object->first_span, object->span_count)) goto memory;
        if (!ag_bus_records(g,bank,object,error,capacity)) return 0;
        for (size_t j = object->first_span; j < object->first_span + object->span_count; j++) {
            if (!(codec->spans[j].flags & SH_AUDIO_ID_DEFINITION) || bank->targets[j] == SIZE_MAX) continue;
            /* Event readers (VK 141d2f9b0, GL 141d48dd0) register an event only
             * when none with that ID exists, so a later bank's copy never runs.
             * Banks arrive in native registration order. Other kinds keep every
             * definition: a Play action is reparsed when flag 0x20 is clear and
             * sounds may reparse their source (R039, R134). */
            if (object->kind == 4 && g->nodes[bank->targets[j]].shape.edge_count) {
                if (g->shadow_count == SIZE_MAX / sizeof(*g->shadows)) goto memory;
                ag_shadow *grown = realloc(g->shadows, (g->shadow_count + 1) * sizeof(*grown));
                if (!grown) goto memory;
                g->shadows = grown; g->shadows[g->shadow_count++] = (ag_shadow){bank->targets[j], record};
                continue;
            }
            if (!ag_definition(g, bank->targets[j], record, error, capacity)) return 0;
        }
    }
    for(size_t i=0;i<codec->bus_record_count;i++)if(bank->bus_nodes[i]==SIZE_MAX)
        return ag_fail(error,capacity,"audio bus record has no owning object");
    if(!ag_shared_records(g,bank,error,capacity))return 0;
    /* Retain every resident/prefetched contribution to a media identity. The
     * opaque payload is never searched for IDs or rewritten. */
    const sh_audio_bank_chunk *data = NULL;
    for (size_t i = 0; i < codec->chunk_count; i++) if (!strcmp(codec->chunks[i].tag, "DATA")) data = &codec->chunks[i];
    for (size_t i = 0; i < codec->chunk_count; i++) if (!strcmp(codec->chunks[i].tag, "DIDX")) {
        const sh_audio_bank_chunk *chunk = &codec->chunks[i];
        for (uint32_t j = 0; j < chunk->length / 12; j++) {
            unsigned char row[12]; uint64_t at = chunk->offset + 8 + (uint64_t)j * 12; size_t record;
            if (!input->source.read(input->source.context, at, row, sizeof(row)) || !data)
                return ag_fail(error, capacity, "cannot read checked audio media index");
            uint32_t id = ag_u32(row), offset = ag_u32(row + 4), length = ag_u32(row + 8);
            if (offset > data->length || length > data->length - offset)
                return ag_fail(error, capacity, "audio media exceeds retained DATA chunk");
            symbol = ag_find(g, bank->scope, SH_AUDIO_ID_MEDIA, id);
            if (symbol == SIZE_MAX || !ag_record(g, bank->scope, "audio-resident-media", id, 0,
                input->source, data->offset + 8 + offset, length, input->original, input->path, &record, error, capacity) ||
                !ag_definition(g, symbol, record, error, capacity)) return 0;
        }
    }
    return 1;
memory: return ag_fail(error, capacity, "cannot allocate audio field edges");
}
void sh_audio_link_graph_close(sh_audio_link_graph *g)
{
    if (!g) return;
    for (size_t i = 0; i < g->count; i++) {
        for (size_t j = 0; j < g->nodes[i].shape.edge_count; j++) free((void *)g->nodes[i].edges[j].slot);
        free(g->nodes[i].edges);
    }
    if (g->banks) for (size_t i = 0; i < g->bank_count; i++) {
        free(g->banks[i].targets);free(g->banks[i].native_transport);free(g->banks[i].bus_nodes);free(g->banks[i].object_nodes);free(g->banks[i].shared_nodes);
        sh_audio_bank_binding_free(&g->banks[i].binding);sh_audio_bank_binding_free(&g->banks[i].private_binding);
    }
    sh_package_link_classes_free(&g->classes);
    free(g->nodes); free(g->slots); free(g->banks); free(g->starts); free(g->ids); free(g->originals);
    free(g->shadows); free(g);
}
static int ag_classify(sh_audio_link_graph *g, char *error, size_t capacity)
{
    sh_package_link_node *shapes = NULL;
    unsigned char *local = NULL, *classes = NULL;
    int ok = 0;
    sh_package_link_classes_free(&g->classes); free(g->originals); g->originals = NULL;
    if (g->count > SIZE_MAX / sizeof(*shapes)) goto done;
    shapes = calloc(g->count ? g->count : 1, sizeof(*shapes)); local = malloc(g->count ? g->count : 1);
    g->originals = malloc(g->count ? g->count : 1); if (!shapes || !local || !g->originals) goto done;
    for (size_t i = 0; i < g->count; i++) {
        ag_node *n = &g->nodes[i]; n->shape.path = n->path; n->shape.edges = n->edges;
        shapes[i] = n->shape; local[i] = (unsigned char)n->original;
    }
    if (!sh_package_link_identify_verified(shapes, g->count, ag_verify, g, &g->classes, error, capacity)) goto done;
    classes = calloc(g->classes.count ? g->classes.count : 1, 1); if (!classes) goto done;
    /* Exact checked classes may share a certified original witness. Bound
     * dependency classes still distinguish different source environments. */
    for (size_t i = 0; i < g->count; i++) classes[g->classes.nodes[i]] |= local[i];
    for (size_t i = 0; i < g->count; i++) local[i] = classes[g->classes.nodes[i]];
    ok = sh_package_link_originals(shapes, g->count, local, g->originals, error, capacity);
done:
    free(classes); free(local); free(shapes);
    if (!ok && error && capacity && !*error) ag_fail(error, capacity, "cannot classify audio graph");
    return ok;
}
static int ag_transport(sh_audio_link_graph *g, char *error, size_t capacity)
{
    int changed = 0;
    for (size_t b = 0; b < g->bank_count; b++) {
        ag_bank *bank = g->banks + b; const sh_audio_bank_codec *codec = bank->input->codec;
        for (size_t o = 0; o < codec->object_count; o++) {
            const sh_audio_bank_object *object = codec->objects + o;
            if (object->kind != 3) continue;
            size_t action = ag_find(g, bank->scope, SH_AUDIO_ID_ACTION, object->id);
            if (action == SIZE_MAX) return ag_fail(error, capacity, "audio action has no scoped definition");
            for (size_t f = object->first_span; f < object->first_span + object->span_count; f++) {
                const sh_audio_identity_span *field = codec->spans + f;
                if (field->domain != SH_AUDIO_ID_BANK || bank->targets[f] == SIZE_MAX) continue;
                if (g->originals[action]) bank->native_transport[f] = 1;
                else {
                    char slot[40]; snprintf(slot, sizeof(slot), "field/%020llu",
                        (unsigned long long)(field->offset - object->offset));
                    if (!ag_edge(g, bank->object_nodes[o], slot, bank->targets[f]))
                        return ag_fail(error, capacity, "cannot retain private audio preparation bank");
                    changed = 1;
                }
            }
        }
    }
    /* Private actions keep their scoped bank lifetime and cannot alias another
     * action whose preparation bank has different contents. Native actions never
     * get redirected to an unrelated edit in that same container. */
    return !changed || ag_classify(g, error, capacity);
}
sh_audio_link_graph *sh_audio_link_graph_open(const sh_audio_graph_scope *scopes,
    size_t count, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    sh_audio_link_graph *g = calloc(1, sizeof(*g));
    if (!g) { ag_fail(error, capacity, "cannot allocate audio graph"); return NULL; }
    if ((count && !scopes) || count >= SIZE_MAX / sizeof(*g->starts)) goto invalid;
    g->scope_count = count; g->starts = calloc(count + 1, sizeof(*g->starts)); if (!g->starts) goto memory;
    for (size_t i = 0; i < count; i++) {
        if ((scopes[i].bank_count && !scopes[i].banks) || scopes[i].bank_count > SIZE_MAX - g->bank_count) goto invalid;
        g->starts[i] = g->bank_count; g->bank_count += scopes[i].bank_count;
    }
    g->starts[count] = g->bank_count;
    if (g->bank_count > SIZE_MAX / sizeof(*g->banks)) goto memory;
    g->banks = calloc(g->bank_count ? g->bank_count : 1, sizeof(*g->banks)); if (!g->banks) goto memory;
    for (size_t i = 0; i < count; i++) for (size_t j = 0; j < scopes[i].bank_count; j++) {
        ag_bank *bank = &g->banks[g->starts[i] + j]; bank->input = &scopes[i].banks[j]; bank->scope = i;
        if (!ag_bank_scan(g, bank, error, capacity)) goto fail;
    }
    /* Media lookup is captured and locale-specific. It never searches peers.
     * A prefetched prefix and its streamed body both enter the identity. */
    size_t symbols_end = g->count;
    for (size_t i = 0; i < symbols_end; i++) if (g->nodes[i].symbol && g->nodes[i].domain == SH_AUDIO_ID_MEDIA &&
        (g->nodes[i].external || !g->nodes[i].shape.edge_count)) {
        size_t scope = g->nodes[i].scope; uint32_t id = g->nodes[i].authored; sh_audio_graph_media media = {0};
        int result = scopes[scope].media ? scopes[scope].media(scopes[scope].context, id, &media, error, capacity) : 0;
        if (result < 0) { if (error && capacity && !*error) ag_fail(error, capacity, "cannot resolve captured audio media"); goto fail; }
        if (result) {
            size_t record;
            if (!ag_record(g, scope, "audio-streamed-media", id, 0, media.source, 0, media.source.length,
                media.original, media.path, &record, error, capacity) || !ag_definition(g, i, record, error, capacity)) goto fail;
        } else if (g->nodes[i].external) {
            if (error && capacity) snprintf(error, capacity, "audio scope %llu needs streamed media %u", (unsigned long long)scope, id);
            goto fail;
        }
    }
    for (size_t i = 0; i < g->count; i++) if (g->nodes[i].symbol && !g->nodes[i].shape.edge_count) {
        size_t scope = g->nodes[i].scope;
        int unresolved = g->nodes[i].native_references && scopes[scope].absent &&
            scopes[scope].absent(scopes[scope].context, g->nodes[i].authored);
        if (g->nodes[i].required && !unresolved) {
            if (error && capacity) snprintf(error, capacity, "audio scope %llu needs %s %u (%s, byte %zu)",
                (unsigned long long)g->nodes[i].scope, g->nodes[i].shape.family, g->nodes[i].authored,
                g->nodes[i].required_path ? g->nodes[i].required_path : "captured bank", g->nodes[i].required_offset);
            goto fail;
        }
        /* Exact engine-source references retain the native original lookup,
         * including controls for objects another vanilla bank may load later.
         * Authored unresolved references instead reserve private absence.
         * These are different bindings even when their numeric names agree. */
        g->nodes[i].original = g->nodes[i].original_references;
        g->nodes[i].shape.semantics[0] = (unsigned char)g->nodes[i].original;
    }
    if (!ag_classify(g, error, capacity) || !ag_transport(g, error, capacity)) goto fail;
    return g;
invalid: ag_fail(error, capacity, "invalid audio graph source scopes"); goto fail;
memory: ag_fail(error, capacity, "cannot allocate audio graph");
fail: sh_audio_link_graph_close(g); return NULL;
}
int sh_audio_link_graph_event(const sh_audio_link_graph *g, size_t scope,
    uint32_t event, sh_audio_graph_root *out, char *error, size_t capacity)
{
    if (out) memset(out, 0, sizeof(*out));
    if (error && capacity) *error = 0;
    size_t index = ag_find(g, scope, SH_AUDIO_ID_EVENT, event);
    if (!out || index == SIZE_MAX || !g->nodes[index].shape.edge_count)
        return ag_fail(error, capacity, "played event is absent from its authored audio scope");
    out->node = index; out->original = g->originals[index];
    memcpy(out->identity, g->classes.identities[g->classes.nodes[index]], sizeof(out->identity)); return 1;
}
size_t sh_audio_link_graph_shadowed(const sh_audio_link_graph *g) { return g ? g->shadow_count : 0; }
int sh_audio_link_graph_shadow(const sh_audio_link_graph *g, size_t index, sh_audio_graph_shadow *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!g || !out || index >= g->shadow_count) return 0;
    const ag_node *symbol = g->nodes + g->shadows[index].symbol, *record = g->nodes + g->shadows[index].record;
    if (!symbol->shape.edge_count) return 0;
    out->scope = symbol->scope; out->event = symbol->authored; out->record = g->shadows[index].record;
    out->bank = record->source_path; out->first = g->nodes[symbol->edges[0].target].source_path; return 1;
}
int sh_audio_link_graph_same_bytes(const sh_audio_link_graph *g, size_t left, size_t right)
{ return !g || left >= g->count || right >= g->count ? -1 : ag_bytes_equal(g->nodes + left, g->nodes + right); }
int sh_audio_link_graph_equal(const sh_audio_link_graph *g, size_t left, size_t right)
{ return g && left < g->count && right < g->count && g->classes.nodes[left] == g->classes.nodes[right]; }

static const ag_bank *ag_bank_at(const sh_audio_link_graph *g,size_t scope,size_t bank)
{
    return g && scope<g->scope_count && bank<g->starts[scope+1]-g->starts[scope] ?
        g->banks+g->starts[scope]+bank : NULL;
}
int sh_audio_link_graph_bus_record(const sh_audio_link_graph *g,size_t scope,size_t bank,
    size_t record,sh_audio_graph_root *out)
{
    if(out)memset(out,0,sizeof(*out));
    const ag_bank *input=ag_bank_at(g,scope,bank);
    if(!out || !input || record>=input->input->codec->bus_record_count)return 0;
    size_t node=input->bus_nodes[record];if(node>=g->count)return 0;
    out->node=node;out->original=g->originals[node];
    memcpy(out->identity,g->classes.identities[g->classes.nodes[node]],sizeof(out->identity));return 1;
}
int sh_audio_link_graph_bus_object(const sh_audio_link_graph *g,size_t scope,size_t bank,
    size_t object,sh_audio_graph_root *out)
{
    if(out)memset(out,0,sizeof(*out));const ag_bank *input=ag_bank_at(g,scope,bank);
    if(!out || !input || object>=input->input->codec->object_count)return 0;
    unsigned kind=input->input->codec->objects[object].kind;if(kind!=8 && kind!=20)return 0;
    size_t node=input->object_nodes[object];out->node=node;out->original=g->originals[node];
    memcpy(out->identity,g->classes.identities[g->classes.nodes[node]],sizeof(out->identity));return 1;
}
/* Owned bytes of one checked value span. bound=1 relocates only the typed
 * private references inside that span; masked packed fields stay masked. */
static int ag_span_value(const sh_audio_link_graph *g,const uint32_t *ids,const ag_bank *input,
    size_t node,uint64_t offset,size_t span,int bound,unsigned char **body,size_t *length,
    char *error,size_t capacity)
{
    unsigned char *bytes=malloc(span ? span : 1);
    if(!bytes)return ag_fail(error,capacity,"cannot retain audio policy value");
    if(!ag_read(g->nodes+node,0,bytes,span)) {
        free(bytes);return ag_fail(error,capacity,"cannot read retained audio policy value");
    }
    if(bound)for(size_t i=0;i<input->input->codec->span_count;i++) {
        const sh_audio_identity_span *field=input->input->codec->spans+i;
        if(input->targets[i]==SIZE_MAX || field->offset<offset || field->offset-offset>=span)continue;
        size_t at=(size_t)(field->offset-offset);
        if(span-at<4 || !ids[input->targets[i]]) {
            free(bytes);return ag_fail(error,capacity,"audio policy value has an invalid typed binding");
        }
        uint32_t id=ids[input->targets[i]];
        for(unsigned j=0;j<4;j++)bytes[at+j]=(unsigned char)(id>>(8*j));
    }
    *body=bytes;*length=span;return 1;
}
static int ag_bus_value(const sh_audio_link_graph *g,const uint32_t *ids,size_t scope,size_t bank,
    size_t record,int bound,unsigned char **body,size_t *length,char *error,size_t capacity)
{
    if(body)*body=NULL;if(length)*length=0;if(error && capacity)*error=0;
    const ag_bank *input=ag_bank_at(g,scope,bank);sh_audio_graph_root root;
    if(!body || !length || !sh_audio_link_graph_bus_record(g,scope,bank,record,&root) || (bound && !ids))
        return ag_fail(error,capacity,"audio bus value is unavailable or unbound");
    const sh_audio_bus_record *value=input->input->codec->bus_records+record;
    return ag_span_value(g,ids,input,root.node,value->offset,value->length,bound,body,length,error,capacity);
}
int sh_audio_link_graph_bus_value(const sh_audio_link_graph *g,size_t scope,size_t bank,
    size_t record,int bound,unsigned char **body,size_t *length,char *error,size_t capacity)
{ return ag_bus_value(g,g ? g->ids : NULL,scope,bank,record,bound,body,length,error,capacity); }
int sh_audio_link_binding_bus_value(const sh_audio_link_binding *binding,size_t scope,size_t bank,
    size_t record,unsigned char **body,size_t *length,char *error,size_t capacity)
{
    return ag_bus_value(binding ? binding->graph : NULL,binding ? binding->ids : NULL,
        scope,bank,record,1,body,length,error,capacity);
}
int sh_audio_link_graph_shared_record(const sh_audio_link_graph *g,size_t scope,size_t bank,
    size_t record,sh_audio_graph_root *out)
{
    if(out)memset(out,0,sizeof(*out));
    const ag_bank *input=ag_bank_at(g,scope,bank);
    if(!out || !input || record>=input->input->codec->shared_record_count)return 0;
    size_t node=input->shared_nodes[record];if(node>=g->count)return 0;
    out->node=node;out->original=g->originals[node];
    memcpy(out->identity,g->classes.identities[g->classes.nodes[node]],sizeof(out->identity));return 1;
}
static int ag_shared_value(const sh_audio_link_graph *g,const uint32_t *ids,size_t scope,size_t bank,
    size_t record,int bound,unsigned char **body,size_t *length,char *error,size_t capacity)
{
    if(body)*body=NULL;if(length)*length=0;if(error && capacity)*error=0;
    const ag_bank *input=ag_bank_at(g,scope,bank);sh_audio_graph_root root;
    if(!body || !length || !sh_audio_link_graph_shared_record(g,scope,bank,record,&root) || (bound && !ids))
        return ag_fail(error,capacity,"audio shared value is unavailable or unbound");
    const sh_audio_shared_record *value=input->input->codec->shared_records+record;
    return ag_span_value(g,ids,input,root.node,value->offset,value->length,bound,body,length,error,capacity);
}
int sh_audio_link_graph_shared_value(const sh_audio_link_graph *g,size_t scope,size_t bank,
    size_t record,int bound,unsigned char **body,size_t *length,char *error,size_t capacity)
{ return ag_shared_value(g,g ? g->ids : NULL,scope,bank,record,bound,body,length,error,capacity); }
int sh_audio_link_binding_shared_value(const sh_audio_link_binding *binding,size_t scope,size_t bank,
    size_t record,unsigned char **body,size_t *length,char *error,size_t capacity)
{
    return ag_shared_value(binding ? binding->graph : NULL,binding ? binding->ids : NULL,
        scope,bank,record,1,body,length,error,capacity);
}

struct sh_audio_graph_comparison {
    const sh_audio_link_graph *left, *right;
    sh_package_link_classes classes;
};
static const ag_node *ag_compared_node(const sh_audio_graph_comparison *c, size_t index)
{
    const sh_audio_link_graph *g = c->left;
    if (index >= g->classes.count) { index -= g->classes.count; g = c->right; }
    return g->nodes + g->classes.representatives[index];
}
static int ag_compare_verify(void *context, size_t left, size_t right, char *error, size_t capacity)
{
    const sh_audio_graph_comparison *c = context;
    int equal = ag_bytes_equal(ag_compared_node(c, left), ag_compared_node(c, right));
    return equal == 1 || ag_fail(error, capacity, equal < 0 ?
        "cannot compare retained audio recipes" : "retained audio fingerprint matched unequal bytes");
}
void sh_audio_link_graph_comparison_close(sh_audio_graph_comparison *c)
{ if (c) { sh_package_link_classes_free(&c->classes); free(c); } }
sh_audio_graph_comparison *sh_audio_link_graph_compare(const sh_audio_link_graph *a,
    const sh_audio_link_graph *b, char *error, size_t capacity)
{
    sh_audio_graph_comparison *c = NULL;
    sh_package_link_node *nodes = NULL; sh_package_link_edge *edges = NULL;
    size_t count = 0, edge_count = 0, edge_at = 0; int ok = 0;
    if (error && capacity) *error = 0;
    if (!a || !b || a->classes.count > SIZE_MAX - b->classes.count) goto done;
    c = calloc(1, sizeof(*c)); if (!c) goto done;
    c->left = a; c->right = b;
    if (a == b) { ok = 1; goto done; }
    count = a->classes.count + b->classes.count;
    if (count > SIZE_MAX / sizeof(*nodes)) goto done;
    nodes = calloc(count ? count : 1, sizeof(*nodes)); if (!nodes) goto done;
    for (size_t i = 0; i < count; i++) {
        nodes[i] = ag_compared_node(c, i)->shape;
        if (nodes[i].edge_count > SIZE_MAX - edge_count) goto done;
        edge_count += nodes[i].edge_count;
    }
    if (edge_count > SIZE_MAX / sizeof(*edges)) goto done;
    edges = calloc(edge_count ? edge_count : 1, sizeof(*edges)); if (!edges) goto done;
    for (size_t i = 0; i < count; i++) {
        const sh_audio_link_graph *g = i < a->classes.count ? a : b;
        size_t base = i < a->classes.count ? 0 : a->classes.count;
        const ag_node *node = ag_compared_node(c, i);
        nodes[i].edges = edges + edge_at;
        for (size_t j = 0; j < node->shape.edge_count; j++) {
            edges[edge_at] = node->edges[j];
            edges[edge_at++].target = base + g->classes.nodes[node->edges[j].target];
        }
    }
    ok = sh_package_link_equivalence_verified(nodes, count, ag_compare_verify, c, &c->classes, error, capacity);
done:
    free(edges); free(nodes);
    if (!ok) {
        sh_audio_link_graph_comparison_close(c); c = NULL;
        if (error && capacity && !*error) ag_fail(error, capacity, "cannot compare retained audio graphs");
    }
    return c;
}
int sh_audio_link_graph_compared(const sh_audio_graph_comparison *c, size_t left, size_t right)
{
    if (!c || left >= c->left->count || right >= c->right->count) return 0;
    if (c->left == c->right) return sh_audio_link_graph_equal(c->left, left, right);
    return c->classes.nodes[c->left->classes.nodes[left]] ==
        c->classes.nodes[c->left->classes.count + c->right->classes.nodes[right]];
}

int sh_audio_link_graph_root_sources(const sh_audio_link_graph *g, size_t root,
    int (*visit)(void *, const char *, uint64_t, uint64_t), void *context, char *error, size_t capacity)
{
    size_t head = 0, count = 0;
    size_t *queue = NULL; unsigned char *seen = NULL; int ok = 0;
    if (!g || root >= g->count || !visit ||
        g->count > SIZE_MAX / sizeof(*queue)) goto done;
    size_t scope=g->nodes[root].scope;
    queue = malloc(g->count * sizeof(*queue)); seen = calloc(g->count, 1);
    if (!queue || !seen) goto done;
    queue[count++] = root; seen[root] = 1;
    while (head < count) {
        const ag_node *node = g->nodes + queue[head++];
        if (node->scope != scope) goto done;
        if (node->source.read && (!node->source_path || !*node->source_path ||
            !visit(context, node->source_path, node->offset, node->shape.length))) goto done;
        for (size_t i = 0; i < node->shape.edge_count; i++) {
            size_t target = node->edges[i].target;
            if (target >= g->count) goto done;
            if (!seen[target]) { seen[target] = 1; queue[count++] = target; }
        }
    }
    ok = 1;
done:
    free(queue); free(seen);
    return ok || ag_fail(error, capacity, "cannot retain scoped audio source ownership");
}
static int ag_sources(const sh_audio_link_graph *g, size_t scope, sh_audio_identity_domain domain, uint32_t identity,
    int (*visit)(void *, const char *, uint64_t, uint64_t), void *context, char *error, size_t capacity)
{
    size_t root=ag_find(g,scope,domain,identity);
    return (g && root<g->count && g->nodes[root].shape.edge_count &&
        sh_audio_link_graph_root_sources(g,root,visit,context,error,capacity)) ||
        ag_fail(error,capacity,"cannot retain scoped audio source ownership");
}
int sh_audio_link_graph_sources(const sh_audio_link_graph *g, size_t scope, uint32_t event,
    int (*visit)(void *, const char *, uint64_t, uint64_t), void *context, char *error, size_t capacity)
{ return ag_sources(g, scope, SH_AUDIO_ID_EVENT, event, visit, context, error, capacity); }
int sh_audio_link_graph_bank_sources(const sh_audio_link_graph *g, size_t scope, uint32_t bank,
    int (*visit)(void *, const char *, uint64_t, uint64_t), void *context, char *error, size_t capacity)
{ return ag_sources(g, scope, SH_AUDIO_ID_BANK, bank, visit, context, error, capacity); }

typedef struct ag_resolver { const ag_bank *bank; const uint32_t *ids; const sh_audio_link_graph *graph; } ag_resolver;
static int ag_resolve(void *context, const sh_audio_identity_span *field, uint32_t *id, char *error, size_t capacity)
{
    const ag_resolver *r = context; const sh_audio_bank_codec *codec = r->bank->input->codec;
    /* The codec calls with its retained span, including non-file-order tree
     * traversals. Do not binary-search an array that is not offset-sorted. */
    size_t first = (size_t)(field - codec->spans);
    if (first >= codec->span_count || r->bank->targets[first] == SIZE_MAX)
        return ag_fail(error, capacity, "audio relocation has no retained field binding");
    if (r->bank->native_transport[first]) { *id = field->value; return 1; }
    if (field->domain == SH_AUDIO_ID_BANK && ag_is_name(codec, field->offset)) {
        size_t bank = ag_find(r->graph, r->bank->scope, SH_AUDIO_ID_BANK, codec->metadata.id);
        if (bank != SIZE_MAX && r->graph->originals[bank]) { *id = field->value; return 1; }
    }
    *id = r->ids[r->bank->targets[first]]; return *id != 0;
}
typedef struct ag_assigned { unsigned domain; uint32_t id; size_t binding,node; } ag_assigned;
static int ag_keep_private(void *context,sh_audio_identity_domain domain,uint32_t id)
{
    const ag_resolver *r=context;
    size_t node=ag_find(r->graph,r->bank->scope,domain,id);
    return node==SIZE_MAX ? -1 : !r->graph->originals[node];
}
static int ag_assignment_order(const void *left, const void *right)
{
    const ag_assigned *a = left, *b = right;
    if (a->domain != b->domain) return a->domain < b->domain ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id;
}
sh_audio_link_binding *sh_audio_link_graph_prepare_bind(sh_audio_link_graph *g, sh_audio_graph_assign assign,
    void *context, char *error, size_t capacity)
{
    uint32_t *ids = NULL, *class_ids = NULL; sh_audio_bank_binding *bindings = NULL,*private_bindings=NULL;
    ag_assigned *assigned = NULL; size_t assigned_count = 0,*kept=NULL;sh_audio_link_binding *result=NULL;
    if (error && capacity) *error = 0;
    if (!g || !assign) {ag_fail(error, capacity, "audio binding needs an ID reservation transaction");return NULL;}
    ids = calloc(g->count ? g->count : 1, sizeof(*ids));
    class_ids = calloc(g->classes.count ? g->classes.count : 1, sizeof(*class_ids));
    bindings = calloc(g->bank_count ? g->bank_count : 1, sizeof(*bindings));
    private_bindings=calloc(g->bank_count ? g->bank_count : 1,sizeof(*private_bindings));
    kept=calloc(g->bank_count ? g->bank_count : 1,sizeof(*kept));
    assigned = calloc(g->symbols ? g->symbols : 1, sizeof(*assigned));
    if (!ids || !class_ids || !bindings || !private_bindings || !kept || !assigned) { ag_fail(error, capacity, "cannot allocate audio binding transaction"); goto done; }
    for (size_t i = 0; i < g->count; i++) if (g->nodes[i].symbol) {
        const ag_node *n = &g->nodes[i]; size_t cls = g->classes.nodes[i];
        if (!class_ids[cls]) {
            if (!assign(context, g, i, n->domain, n->authored, g->classes.identities[cls], g->originals[i],
                !n->shape.edge_count, &class_ids[cls], error, capacity)) goto done;
            if (!class_ids[cls] || (g->originals[i] && class_ids[cls] != n->authored)) {
                ag_fail(error, capacity, "audio allocator changed an original or returned a null identity"); goto done;
            }
            assigned[assigned_count++] = (ag_assigned){n->domain, class_ids[cls], cls,i};
        }
        ids[i] = class_ids[cls];
    }
    if (assigned_count > 1) qsort(assigned, assigned_count, sizeof(*assigned), ag_assignment_order);
    for (size_t i = 1; i < assigned_count; i++) if (!ag_assignment_order(&assigned[i - 1], &assigned[i])) {
        /* Certified native references share the engine ABI, even when captures
         * contain different subsets of original definitions/optional lookups.
         * This is native name reuse, not proof that the scoped recipes dedupe.
         * An authored/private binding can never use this exception. */
        if(g->originals[assigned[i-1].node] && g->originals[assigned[i].node])continue;
        if(error && capacity)snprintf(error,capacity,
            "audio allocator assigned one native ID to different bindings: domain=%u id=%u scopes=%zu/%zu originals=%u/%u definitions=%zu/%zu",
            assigned[i].domain,assigned[i].id,g->nodes[assigned[i-1].node].scope,g->nodes[assigned[i].node].scope,
            g->originals[assigned[i-1].node],g->originals[assigned[i].node],
            g->nodes[assigned[i-1].node].shape.edge_count,g->nodes[assigned[i].node].shape.edge_count);goto done;
    }
    for (size_t i = 0; i < g->bank_count; i++) {
        ag_resolver resolver = {&g->banks[i], ids, g};
        if (!sh_audio_bank_codec_bind(g->banks[i].input->source, g->banks[i].input->codec,
            ag_resolve, &resolver, &bindings[i], error, capacity)) goto done;
        size_t bank=ag_find(g,g->banks[i].scope,SH_AUDIO_ID_BANK,g->banks[i].input->codec->metadata.id);
        if(bank==SIZE_MAX)goto done;
        if(!g->originals[bank] &&
            !sh_audio_bank_binding_select(g->banks[i].input->source,g->banks[i].input->codec,
                bindings+i,ag_keep_private,&resolver,private_bindings+i,kept+i,error,capacity))goto done;
    }
    result=calloc(1,sizeof(*result));if(!result){ag_fail(error,capacity,"cannot retain audio binding transaction");goto done;}
    result->graph=g;result->ids=ids;result->banks=bindings;result->private_banks=private_bindings;result->private_kept=kept;
    ids=NULL;bindings=NULL;private_bindings=NULL;kept=NULL;
done:
    if (bindings) for (size_t i = 0; i < g->bank_count; i++) sh_audio_bank_binding_free(&bindings[i]);
    if(private_bindings)for(size_t i=0;i<g->bank_count;i++)sh_audio_bank_binding_free(private_bindings+i);
    free(private_bindings);free(bindings); free(kept); free(class_ids); free(ids); free(assigned); return result;
}
void sh_audio_link_graph_cancel_bind(sh_audio_link_binding *binding)
{
    if(!binding)return;
    for(size_t i=0;i<binding->graph->bank_count;i++) {
        sh_audio_bank_binding_free(binding->banks+i);
        sh_audio_bank_binding_free(binding->private_banks+i);
    }
    free(binding->banks);free(binding->private_banks);free(binding->private_kept);free(binding->ids);free(binding);
}
void sh_audio_link_graph_commit_bind(sh_audio_link_binding *binding)
{
    if(!binding)return;sh_audio_link_graph *g=binding->graph;
    for(size_t i=0;i<g->bank_count;i++) {
        sh_audio_bank_binding_free(&g->banks[i].binding);g->banks[i].binding=binding->banks[i];
        memset(binding->banks+i,0,sizeof(*binding->banks));
        sh_audio_bank_binding_free(&g->banks[i].private_binding);g->banks[i].private_binding=binding->private_banks[i];
        memset(binding->private_banks+i,0,sizeof(*binding->private_banks));
        g->banks[i].private_kept=binding->private_kept[i];
    }
    free(g->ids);g->ids=binding->ids;binding->ids=NULL;sh_audio_link_graph_cancel_bind(binding);
}
int sh_audio_link_graph_bind(sh_audio_link_graph *g,sh_audio_graph_assign assign,
    void *context,char *error,size_t capacity)
{
    sh_audio_link_binding *binding=sh_audio_link_graph_prepare_bind(g,assign,context,error,capacity);
    if(!binding)return 0;sh_audio_link_graph_commit_bind(binding);return 1;
}
const sh_audio_bank_binding *sh_audio_link_graph_bank(const sh_audio_link_graph *g, size_t scope, size_t bank)
{
    if (!g || !g->ids || scope >= g->scope_count || bank >= g->starts[scope + 1] - g->starts[scope]) return NULL;
    return &g->banks[g->starts[scope] + bank].binding;
}
const sh_audio_bank_binding *sh_audio_link_graph_private_bank(const sh_audio_link_graph *g,size_t scope,size_t bank)
{
    const sh_audio_bank_binding *full=sh_audio_link_graph_bank(g,scope,bank);if(!full)return NULL;
    const sh_audio_bank_binding *private=&g->banks[g->starts[scope]+bank].private_binding;
    return private->input_length ? private : full;
}
int sh_audio_link_graph_private_empty(const sh_audio_link_graph *g,size_t scope,size_t bank)
{
    if(!sh_audio_link_graph_bank(g,scope,bank))return 0;
    const ag_bank *input=g->banks+g->starts[scope]+bank;
    if(!input->private_binding.input_length || input->private_kept)return 0;
    /* A retained private action's bank locator still prepares and releases this
     * scoped bank identity (R134/R135), even when every object it names resolves
     * in another bank. Only object records carry such field edges; a bank source
     * record's edge to its own identity is its definition, not a use. */
    size_t symbol=ag_find(g,scope,SH_AUDIO_ID_BANK,input->input->codec->metadata.id);
    if(symbol==SIZE_MAX)return 0;
    for(size_t i=0;i<g->count;i++) {
        const ag_node *n=g->nodes+i;
        if(n->symbol || strcmp(n->shape.family,"audio-object-source"))continue;
        for(size_t e=0;e<n->shape.edge_count;e++)if(n->edges[e].target==symbol)return 0;
    }
    return 1;
}
static int ag_bound_id(const sh_audio_link_graph *g, size_t scope, sh_audio_identity_domain domain, uint32_t authored, uint32_t *id)
{
    if (id) *id = 0;
    size_t at = ag_find(g, scope, domain, authored);
    if (!id || !g || !g->ids || at == SIZE_MAX || !g->nodes[at].shape.edge_count) return 0;
    *id = g->ids[at]; return 1;
}
int sh_audio_link_graph_event_id(const sh_audio_link_graph *g, size_t scope, uint32_t authored, uint32_t *id)
{ return ag_bound_id(g, scope, SH_AUDIO_ID_EVENT, authored, id); }
int sh_audio_link_graph_media_id(const sh_audio_link_graph *g, size_t scope, uint32_t authored, uint32_t *id)
{ return ag_bound_id(g, scope, SH_AUDIO_ID_MEDIA, authored, id); }
int sh_audio_link_graph_bank_id(const sh_audio_link_graph *g, size_t scope, uint32_t authored, uint32_t *id)
{ return ag_bound_id(g, scope, SH_AUDIO_ID_BANK, authored, id); }
