#include "audio_bank_codec.h"
#include <stdlib.h>
#include <string.h>

static uint32_t ac_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int ac_read(sh_audio_bank_source s, uint64_t at, void *out, size_t length)
{ return at <= s.length && length <= s.length - at && s.read && s.read(s.context, at, out, length); }
static int ac_grow(void **rows, size_t *allocated, size_t count, size_t extra, size_t width)
{
    if (extra > SIZE_MAX - count || count + extra > SIZE_MAX / width) return 0;
    size_t need = count + extra;
    if (need <= *allocated) return 1;
    size_t next = *allocated ? *allocated : 32;
    while (next < need) {
        if (next > SIZE_MAX / width / 2) { next = need; break; }
        next *= 2;
    }
    void *grown = realloc(*rows, next * width);
    if (!grown) return 0;
    *rows = grown; *allocated = next; return 1;
}
static int ac_span(sh_audio_bank_codec *out, size_t *allocated, uint64_t at, uint32_t id,
    sh_audio_identity_domain domain, unsigned flags)
{
    if (at > SIZE_MAX || !ac_grow((void **)&out->spans, allocated, out->span_count, 1, sizeof(*out->spans))) return 0;
    out->spans[out->span_count++] = (sh_audio_identity_span){(size_t)at, id, domain, flags}; return 1;
}
void sh_audio_bank_codec_free(sh_audio_bank_codec *bank)
{
    if (!bank) return;
    sh_audio_bank_free(&bank->metadata); free(bank->spans); free(bank->ordered); free(bank->objects);
    free(bank->names); free(bank->chunks); free(bank->bus_records); free(bank->shared_records); memset(bank, 0, sizeof(*bank));
}
static int ac_media_compare(const void *left, const void *right)
{
    uint32_t a = ((const sh_audio_bank_media *)left)->id, b = ((const sh_audio_bank_media *)right)->id;
    return (a > b) - (a < b);
}
static int ac_media_dependencies(sh_audio_bank_codec *bank)
{
    size_t extra = 0;
    for (size_t i = 0; i < bank->span_count; i++) if (bank->spans[i].domain == SH_AUDIO_ID_MEDIA &&
        bank->spans[i].value && !(bank->spans[i].flags & SH_AUDIO_ID_DEFINITION)) extra++;
    if (extra > SIZE_MAX - bank->metadata.media_count ||
        bank->metadata.media_count + extra > SIZE_MAX / sizeof(*bank->metadata.media)) return 0;
    size_t count = bank->metadata.media_count + extra;
    if (!extra) return 1;
    sh_audio_bank_media *rows = realloc(bank->metadata.media, count * sizeof(*rows)); if (!rows) return 0;
    bank->metadata.media = rows; size_t at = bank->metadata.media_count;
    for (size_t i = 0; i < bank->span_count; i++) if (bank->spans[i].domain == SH_AUDIO_ID_MEDIA &&
        bank->spans[i].value && !(bank->spans[i].flags & SH_AUDIO_ID_DEFINITION))
        rows[at++] = (sh_audio_bank_media){bank->spans[i].value, 0, 0};
    qsort(rows, count, sizeof(*rows), ac_media_compare);
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        if (!used || rows[i].id != rows[used - 1].id) rows[used++] = rows[i];
        else { rows[used - 1].embedded |= rows[i].embedded; if (!rows[used - 1].stream) rows[used - 1].stream = rows[i].stream; }
    }
    bank->metadata.media_count = used; return 1;
}
int sh_audio_bank_codec_read(sh_audio_bank_source source, sh_audio_bank_codec *out,
    char *error, size_t capacity)
{
    unsigned char header[20]; uint64_t at = 0, data_length = 0;
    int status; unsigned have_data = 0, have_didx = 0;
    size_t span_capacity = 0, object_capacity = 0, name_capacity = 0, chunk_capacity = 0;
    size_t ordered_capacity = 0, bus_capacity = 0, shared_capacity = 0;
    sh_audio_bank_codec bank = {0};
    if (error && capacity) *error = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    status = sh_audio_bank_read_source(source, &bank.metadata, error, capacity);
    if (status != 1) return status;
    while (at < source.length) {
        if (source.length - at < 8) goto malformed;
        if (!ac_read(source, at, header, 8)) goto io;
        uint32_t size = ac_u32(header + 4); uint64_t body = at + 8;
        if (size > source.length - body) goto malformed;
        uint64_t end = body + size;
        if (!ac_grow((void **)&bank.chunks, &chunk_capacity, bank.chunk_count, 1, sizeof(*bank.chunks))) goto memory;
        sh_audio_bank_chunk *chunk = &bank.chunks[bank.chunk_count++];
        memset(chunk,0,sizeof(*chunk));
        memcpy(chunk->tag, header, 4); chunk->tag[4] = 0; chunk->offset = at; chunk->length = size;
        if (!memcmp(header, "BKHD", 4)) {
            if (!ac_span(&bank, &span_capacity, body + 4, bank.metadata.id, SH_AUDIO_ID_BANK, SH_AUDIO_ID_DEFINITION)) goto memory;
        } else if (!memcmp(header, "DATA", 4)) {
            if (have_data++) goto malformed;
            data_length = size;
        } else if (!memcmp(header, "DIDX", 4)) {
            if (have_didx++) goto malformed;
            for (uint32_t i = 0; i < size / 12; i++) {
                uint64_t row = body + (uint64_t)i * 12;
                if (!ac_read(source, row, header, 12)) goto io;
                if (!ac_u32(header)) goto malformed;
                if (!ac_span(&bank, &span_capacity, row, ac_u32(header), SH_AUDIO_ID_MEDIA, SH_AUDIO_ID_DEFINITION)) goto memory;
            }
        } else if (!memcmp(header, "HIRC", 4)) {
            if (size < 4) goto malformed;
            if (!ac_read(source, body, header, 4)) goto io;
            uint32_t count = ac_u32(header); uint64_t cursor = body + 4;
            if (count > (end - cursor) / 9) goto malformed;
            if (!ac_grow((void **)&bank.objects, &object_capacity, bank.object_count, count, sizeof(*bank.objects))) goto memory;
            for (uint32_t i = 0; i < count; i++) {
                if (end - cursor < 9) goto malformed;
                if (!ac_read(source, cursor, header, 5)) goto io;
                unsigned kind = header[0]; uint32_t length = ac_u32(header + 1);
                if (length < 4 || length > end - cursor - 5) goto malformed;
                unsigned char *bytes = malloc(length); sh_audio_hirc object = {0};
                if (!bytes) goto memory;
                if (!ac_read(source, cursor + 5, bytes, length)) { free(bytes); goto io; }
                status = sh_audio_hirc_read(kind, bytes, length, &object, error, capacity);
                uint32_t id = ac_u32(bytes); free(bytes);
                if (status != 1) { sh_audio_hirc_free(&object); goto done; }
                if (!ac_grow((void **)&bank.spans, &span_capacity, bank.span_count, object.count, sizeof(*bank.spans))) {
                    sh_audio_hirc_free(&object); goto memory;
                }
                bank.objects[bank.object_count++] = (sh_audio_bank_object){kind, id, cursor + 5,
                    length, bank.span_count, object.count, bank.bus_record_count, object.bus_record_count};
                if (!ac_grow((void **)&bank.bus_records, &bus_capacity, bank.bus_record_count,
                    object.bus_record_count, sizeof(*bank.bus_records))) { sh_audio_hirc_free(&object); goto memory; }
                for (size_t j = 0; j < object.bus_record_count; j++) {
                    if (cursor + 5 + object.bus_records[j].offset > SIZE_MAX) { sh_audio_hirc_free(&object); goto memory; }
                    object.bus_records[j].offset += (size_t)(cursor + 5);
                    bank.bus_records[bank.bus_record_count++] = object.bus_records[j];
                }
                for (size_t j = 0; j < object.count; j++) {
                    if (cursor + 5 + object.spans[j].offset > SIZE_MAX) { sh_audio_hirc_free(&object); goto memory; }
                    object.spans[j].offset += (size_t)(cursor + 5);
                    bank.spans[bank.span_count++] = object.spans[j];
                }
                if (!ac_grow((void **)&bank.ordered, &ordered_capacity, bank.ordered_count,
                    object.ordered_count, sizeof(*bank.ordered))) { sh_audio_hirc_free(&object); goto memory; }
                for (size_t j = 0; j < object.ordered_count; j++) {
                    if (cursor + 5 + object.ordered[j].offset > SIZE_MAX) { sh_audio_hirc_free(&object); goto memory; }
                    object.ordered[j].offset += (size_t)(cursor + 5);
                    bank.ordered[bank.ordered_count++] = object.ordered[j];
                }
                sh_audio_hirc_free(&object); cursor += 5 + (uint64_t)length;
            }
            if (cursor != end) goto malformed;
        } else if (!memcmp(header, "STID", 4)) {
            uint64_t cursor = body;
            while (cursor < end) {
                if (end - cursor < 8) goto malformed;
                if (!ac_read(source, cursor, header, 8)) goto io;
                uint32_t count = ac_u32(header + 4); cursor += 8;
                if (count > (end - cursor) / 5) goto malformed;
                if (!ac_grow((void **)&bank.names, &name_capacity, bank.name_count, count, sizeof(*bank.names))) goto memory;
                for (uint32_t i = 0; i < count; i++) {
                    if (end - cursor < 5) goto malformed;
                    if (!ac_read(source, cursor, header, 5)) goto io;
                    sh_audio_bank_name row = {0}; row.offset = cursor; row.id = ac_u32(header); row.length = header[4];
                    if (!row.id || !row.length || row.length > end - cursor - 5) goto malformed;
                    if (!ac_read(source, cursor + 5, row.name, row.length)) goto io;
                    if (memchr(row.name, 0, row.length) || strchr(row.name, '/') || strchr(row.name, '\\')) goto malformed;
                    row.name[row.length] = 0; bank.names[bank.name_count++] = row;
                    if (!ac_span(&bank, &span_capacity, cursor, row.id, SH_AUDIO_ID_BANK, 0)) goto memory;
                    cursor += 5 + row.length;
                }
            }
        } else if (!memcmp(header, "STMG", 4) || !memcmp(header, "ENVS", 4) || !memcmp(header, "PLAT", 4)) {
            bank.shared_chunks |= !memcmp(header, "STMG", 4) ? 1 : !memcmp(header, "ENVS", 4) ? 2 : 4;
            unsigned char *bytes = malloc(size ? size : 1); sh_audio_hirc shared = {0};
            if (!bytes) goto memory;
            if (!ac_read(source, body, bytes, size)) { free(bytes); goto io; }
            status = sh_audio_shared_chunk_read(chunk->tag, bytes, size, &shared, error, capacity); free(bytes);
            if (status != 1) { sh_audio_hirc_free(&shared); goto done; }
            if(!ac_grow((void **)&bank.shared_records,&shared_capacity,bank.shared_record_count,
                shared.shared_count,sizeof(*bank.shared_records))) { sh_audio_hirc_free(&shared); goto memory; }
            chunk->first_shared_record=bank.shared_record_count;chunk->shared_record_count=shared.shared_count;
            for(size_t j=0;j<shared.shared_count;j++) {
                if(body+shared.shared[j].offset>SIZE_MAX){sh_audio_hirc_free(&shared);goto memory;}
                shared.shared[j].offset+=(size_t)body;bank.shared_records[bank.shared_record_count++]=shared.shared[j];
            }
            if (!ac_grow((void **)&bank.spans, &span_capacity, bank.span_count, shared.count, sizeof(*bank.spans))) {
                sh_audio_hirc_free(&shared); goto memory;
            }
            for (size_t j = 0; j < shared.count; j++) {
                if (body + shared.spans[j].offset > SIZE_MAX) { sh_audio_hirc_free(&shared); goto memory; }
                shared.spans[j].offset += (size_t)body; bank.spans[bank.span_count++] = shared.spans[j];
            }
            sh_audio_hirc_free(&shared);
        } else {
            if (error && capacity) snprintf(error, capacity, "unverified audio bank chunk %.4s at byte %llu", header, (unsigned long long)at);
            status = -2; goto done;
        }
        at = end;
    }
    /* DATA offsets are scalar offsets, not identities. Check them without
     * reading or copying media bytes, including prefetched partial payloads. */
    for (size_t c = 0; c < bank.chunk_count; c++) if (!strcmp(bank.chunks[c].tag, "DIDX")) {
        const sh_audio_bank_chunk *chunk = &bank.chunks[c];
        for (uint32_t i = 0; i < chunk->length / 12; i++) {
            if (!ac_read(source, chunk->offset + 8 + (uint64_t)i * 12, header, 12)) goto io;
            uint64_t offset = ac_u32(header + 4), length = ac_u32(header + 8);
            if (!have_data || offset > data_length || length > data_length - offset) goto malformed;
        }
    }
    /* Effect plug-ins can name resident media outside source descriptors.
     * Feed all verified MEDIA references back into the common bank closure. */
    if (!ac_media_dependencies(&bank)) goto memory;
    *out = bank; return 1;
malformed:
    if (error && capacity) snprintf(error, capacity, "malformed typed audio bank structure at byte %llu", (unsigned long long)at);
    status = 0; goto done;
memory:
    if (error && capacity) snprintf(error, capacity, "cannot allocate typed audio bank structure");
    status = -1; goto done;
io:
    if (error && capacity) snprintf(error, capacity, "cannot read immutable typed audio bank source");
    status = -1;
done:
    sh_audio_bank_codec_free(&bank); return status;
}

static void ac_put32(unsigned char *out, uint32_t value)
{ for (size_t i = 0; i < 4; i++) out[i] = (unsigned char)(value >> (i * 8)); }
static int ac_edit(sh_audio_bank_binding *out, size_t *allocated, uint64_t offset,
    uint64_t remove, const void *bytes, size_t length)
{
    if (length > sizeof(out->edits[0].bytes) || offset > out->input_length || remove > out->input_length - offset ||
        !ac_grow((void **)&out->edits, allocated, out->count, 1, sizeof(*out->edits))) return 0;
    sh_audio_bank_edit *edit = &out->edits[out->count++]; memset(edit, 0, sizeof(*edit));
    edit->offset = offset; edit->remove = remove; edit->length = (unsigned char)length;
    if(length)memcpy(edit->bytes, bytes, length); return 1;
}
static int ac_edit_compare(const void *left, const void *right)
{
    uint64_t a = ((const sh_audio_bank_edit *)left)->offset, b = ((const sh_audio_bank_edit *)right)->offset;
    return (a > b) - (a < b);
}
void sh_audio_bank_binding_free(sh_audio_bank_binding *binding)
{ if (binding) { free(binding->edits); memset(binding, 0, sizeof(*binding)); } }
typedef struct ac_alias { uint64_t key; uint32_t value; } ac_alias;
static ac_alias *ac_alias_slot(ac_alias *aliases, size_t count, uint64_t offset)
{
    uint64_t key = offset + 1;
    size_t slot = (size_t)((key ^ (key >> 33)) * UINT64_C(0xff51afd7ed558ccd)) & (count - 1);
    while (aliases[slot].key && aliases[slot].key != key) slot = (slot + 1) & (count - 1);
    return &aliases[slot];
}
static int ac_ordered_contains(const sh_audio_bank_codec *bank, uint64_t offset)
{
    size_t first = 0, end = bank->ordered_count;
    while (first < end) {
        size_t middle = first + (end - first) / 2;
        if (bank->ordered[middle].offset <= offset) first = middle + 1; else end = middle;
    }
    if (!first) return 0;
    const sh_audio_identity_list *list = &bank->ordered[first - 1];
    return offset - list->offset < (uint64_t)list->count * 4;
}
static int ac_id_compare(const void *left, const void *right)
{ uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right; return (a > b) - (a < b); }

int sh_audio_bank_codec_bind(sh_audio_bank_source source, const sh_audio_bank_codec *bank,
    sh_audio_bank_identity_resolver resolve, void *context, sh_audio_bank_binding *out,
    char *error, size_t capacity)
{
    sh_audio_bank_binding binding = {0}; size_t allocated = 0; int result = 0;
    int64_t *deltas = NULL; ac_alias *aliases = NULL; size_t alias_count = 32;
    if (error && capacity) *error = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out)); binding.input_length = binding.output_length = source.length;
    if (!bank || !resolve || !source.read || bank->chunk_count > SIZE_MAX / sizeof(*deltas)) goto invalid;
    if (bank->span_count > SIZE_MAX / 2) goto invalid;
    while (alias_count < bank->span_count * 2) {
        if (alias_count > SIZE_MAX / 2) goto invalid;
        alias_count *= 2;
    }
    if (alias_count > SIZE_MAX / sizeof(*aliases)) goto invalid;
    aliases = calloc(alias_count, sizeof(*aliases));
    deltas = calloc(bank->chunk_count ? bank->chunk_count : 1, sizeof(*deltas));
    if (!deltas || !aliases) goto invalid;
    uint64_t ordered_end = 0;
    for (size_t i = 0; i < bank->ordered_count; i++) {
        const sh_audio_identity_list *list = &bank->ordered[i];
        if (list->offset < ordered_end || list->offset > source.length ||
            (uint64_t)list->count * 4 > source.length - list->offset || list->fixed_prefix > list->count) goto invalid;
        ordered_end = list->offset + (uint64_t)list->count * 4;
    }
    for (size_t i = 0; i < bank->span_count; i++) {
        const sh_audio_identity_span *field = &bank->spans[i]; unsigned char bytes[16]; uint32_t id = field->value;
        if (!ac_read(source, field->offset, bytes, 4) || ac_u32(bytes) != id) goto stale;
        if (!id || (field->flags & (SH_AUDIO_ID_SHARED | SH_AUDIO_ID_LITERAL))) continue;
        /* One numeric ID can resolve to different authored objects at two
         * fields, for example an explicit cross-bank play target. The graph
         * resolves occurrences; a bank-wide ID cache would lose that scope. */
        ac_alias *alias = ac_alias_slot(aliases, alias_count, field->offset);
        if (!alias->key) {
            if (!resolve(context, field, &id, error, capacity)) {
                if (error && capacity && !*error) snprintf(error, capacity, "typed audio identity could not be resolved at byte %zu", field->offset);
                goto done;
            }
            if (!id) goto invalid;
            *alias = (ac_alias){(uint64_t)field->offset + 1, id};
        } else id = alias->value;
        if (ac_ordered_contains(bank, field->offset)) continue;
        if (id == field->value) continue;
        ac_put32(bytes, id);
        if (!ac_edit(&binding, &allocated, field->offset, 4, bytes, 4)) goto invalid;
        if (field->domain == SH_AUDIO_ID_BANK && !(field->flags & SH_AUDIO_ID_DEFINITION)) {
            for (size_t n = 0; n < bank->name_count; n++) if (bank->names[n].offset == field->offset) {
                const sh_audio_bank_name *name = &bank->names[n];
                int length = snprintf((char *)bytes + 1, sizeof(bytes) - 1, "%u", id);
                if (length < 1 || length > 10) goto invalid;
                bytes[0] = (unsigned char)length;
                if (!ac_edit(&binding, &allocated, name->offset + 4, name->length + 1, bytes, (size_t)length + 1)) goto invalid;
                int found = 0;
                for (size_t c = 0; c < bank->chunk_count; c++) {
                    const sh_audio_bank_chunk *chunk = &bank->chunks[c];
                    if (!strcmp(chunk->tag, "STID") && name->offset >= chunk->offset + 8 &&
                        name->offset - chunk->offset - 8 < chunk->length) {
                        deltas[c] += (int64_t)length - name->length; found = 1; break;
                    }
                }
                if (!found) goto invalid;
            }
        }
    }
    /* Transition matching uses unsigned binary search. Replacing words in
     * place would break ordering whenever allocated IDs change their order. */
    for (size_t i = 0; i < bank->ordered_count; i++) {
        const sh_audio_identity_list *list = &bank->ordered[i];
        if (!list->count) continue;
        if ((uint64_t)list->count * sizeof(uint32_t) > SIZE_MAX) goto invalid;
        size_t size = (size_t)list->count * sizeof(uint32_t);
        uint32_t *values = malloc(size); unsigned char *original = malloc(size);
        if (!values || !original) { free(values); free(original); goto invalid; }
        if (!ac_read(source, list->offset, original, size)) { free(values); free(original); goto stale; }
        for (uint32_t j = 0; j < list->count; j++) {
            values[j] = ac_u32(original + (size_t)j * 4);
            if (j < list->fixed_prefix || !values[j]) continue;
            ac_alias *alias = ac_alias_slot(aliases, alias_count, list->offset + (uint64_t)j * 4);
            if (!alias->key) { free(values); free(original); goto invalid; }
            values[j] = alias->value;
        }
        if (list->count - list->fixed_prefix > 1) qsort(values + list->fixed_prefix,
            list->count - list->fixed_prefix, sizeof(*values), ac_id_compare);
        for (uint32_t j = 0; j < list->count; j++) if (values[j] != ac_u32(original + (size_t)j * 4)) {
            unsigned char bytes[4]; ac_put32(bytes, values[j]);
            if (!ac_edit(&binding, &allocated, list->offset + (uint64_t)j * 4, 4, bytes, 4)) {
                free(values); free(original); goto invalid;
            }
        }
        free(values); free(original);
    }
    for (size_t c = 0; c < bank->chunk_count; c++) if (deltas[c]) {
        const sh_audio_bank_chunk *chunk = &bank->chunks[c]; unsigned char bytes[4];
        int64_t length = (int64_t)chunk->length + deltas[c];
        if (length < 0 || (uint64_t)length > UINT32_MAX) goto invalid;
        ac_put32(bytes, (uint32_t)length);
        if (!ac_edit(&binding, &allocated, chunk->offset + 4, 4, bytes, 4)) goto invalid;
    }
    if (binding.count > 1) qsort(binding.edits, binding.count, sizeof(*binding.edits), ac_edit_compare);
    uint64_t prior = 0;
    for (size_t i = 0; i < binding.count; i++) {
        const sh_audio_bank_edit *edit = &binding.edits[i];
        if (edit->offset < prior) goto invalid;
        prior = edit->offset + edit->remove;
        if (binding.output_length < edit->remove || UINT64_MAX - (binding.output_length - edit->remove) < edit->length) goto invalid;
        binding.output_length = binding.output_length - edit->remove + edit->length;
    }
    *out = binding; memset(&binding, 0, sizeof(binding)); result = 1; goto done;
stale:
    if (error && capacity) snprintf(error, capacity, "typed audio binding source no longer matches its inspected identities");
    goto done;
invalid:
    if (error && capacity) snprintf(error, capacity, "cannot prepare complete typed audio bank binding");
done:
    free(deltas); free(aliases); sh_audio_bank_binding_free(&binding); return result;
}

static int ac_binding_length(const sh_audio_bank_binding *binding,uint64_t *result)
{
    if(!binding || !result || (binding->count && !binding->edits))return 0;
    uint64_t prior=0,length=binding->input_length;
    for (size_t i = 0; i < binding->count; i++) {
        const sh_audio_bank_edit *edit = &binding->edits[i];
        if (edit->offset < prior || edit->offset > binding->input_length || edit->remove > binding->input_length - edit->offset ||
            edit->length > sizeof(edit->bytes) || length < edit->remove || UINT64_MAX - (length - edit->remove) < edit->length)return 0;
        prior = edit->offset + edit->remove; length = length - edit->remove + edit->length;
    }
    *result=length;return 1;
}
typedef struct ac_media_range { uint64_t start,end,output; } ac_media_range;
typedef struct ac_media_row { uint32_t id,offset,length; int keep; } ac_media_row;
static int ac_range_order(const void *a,const void *b)
{
    const ac_media_range *left=a,*right=b;
    if(left->start!=right->start)return left->start<right->start ? -1 : 1;
    return left->end<right->end ? -1 : left->end>right->end;
}
static int ac_select_media(sh_audio_bank_source source,const sh_audio_bank_codec *bank,
    int (*keep)(void *,sh_audio_identity_domain,uint32_t),void *context,
    sh_audio_bank_binding *cuts,size_t *allocated)
{
    const sh_audio_bank_chunk *index=NULL,*data=NULL;
    ac_media_row *rows=NULL;ac_media_range *ranges=NULL;size_t selected=0,used=0;int ok=0;
    uint64_t alignment=1;
    unsigned char bytes[12];
    for(size_t c=0;c<bank->chunk_count;c++) {
        if(!strcmp(bank->chunks[c].tag,"DIDX"))index=bank->chunks+c;
        if(!strcmp(bank->chunks[c].tag,"DATA"))data=bank->chunks+c;
    }
    if(!index)return 1;
    if(!data || index->length%12)return 0;
    size_t count=index->length/12;
    if(count>SIZE_MAX/sizeof(*rows) || count>SIZE_MAX/sizeof(*ranges))return 0;
    rows=calloc(count ? count : 1,sizeof(*rows));ranges=calloc(count ? count : 1,sizeof(*ranges));
    if(!rows || !ranges)goto done;
    for(size_t i=0;i<count;i++) {
        if(!ac_read(source,index->offset+8+i*12,bytes,12))goto done;
        rows[i]=(ac_media_row){ac_u32(bytes),ac_u32(bytes+4),ac_u32(bytes+8),0};
        ac_media_row *row=rows+i;
        if(!row->id || row->offset>data->length || row->length>data->length-row->offset)goto done;
        row->keep=keep(context,SH_AUDIO_ID_MEDIA,row->id);if(row->keep<0)goto done;
        if(!row->keep)continue;
        selected++;
        /* Preserve every retained entry's existing power-of-two alignment;
         * do not assume that all plug-in media uses one fixed alignment. */
        if(row->offset) {
            uint64_t required=(uint64_t)row->offset & (UINT64_C(0)-(uint64_t)row->offset);
            if(required>alignment)alignment=required;
        }
    }
    for(size_t i=0;i<count;i++) {
        const ac_media_row *row=rows+i;if(!row->keep)continue;
        if(!row->length && row->offset==data->length)continue;
        uint64_t end=((uint64_t)row->offset+(row->length ? row->length : 1)+alignment-1)&~(alignment-1);
        if(end>data->length)end=data->length;
        ranges[used++]=(ac_media_range){row->offset&~(alignment-1),end,0};
    }
    if(selected==count){ok=1;goto done;}
    if(!selected) {
        ok=ac_edit(cuts,allocated,index->offset,(uint64_t)index->length+8,NULL,0) &&
            ac_edit(cuts,allocated,data->offset,(uint64_t)data->length+8,NULL,0);
        goto done;
    }
    if(used>1)qsort(ranges,used,sizeof(*ranges),ac_range_order);
    size_t merged=0;
    for(size_t i=0;i<used;i++) {
        if(merged && ranges[i].start<=ranges[merged-1].end) {
            if(ranges[i].end>ranges[merged-1].end)ranges[merged-1].end=ranges[i].end;
        } else ranges[merged++]=ranges[i];
    }
    uint64_t cursor=0,output=0;
    for(size_t i=0;i<merged;i++) {
        ac_media_range *range=ranges+i;range->output=output;
        if(range->start>cursor && !ac_edit(cuts,allocated,data->offset+8+cursor,range->start-cursor,NULL,0))goto done;
        cursor=range->end;output+=range->end-range->start;
    }
    if(cursor<data->length && !ac_edit(cuts,allocated,data->offset+8+cursor,data->length-cursor,NULL,0))goto done;
    ac_put32(bytes,(uint32_t)output);
    if(!ac_edit(cuts,allocated,data->offset+4,4,bytes,4))goto done;
    ac_put32(bytes,(uint32_t)(selected*12));
    if(!ac_edit(cuts,allocated,index->offset+4,4,bytes,4))goto done;
    for(size_t i=0;i<count;i++) {
        ac_media_row *row=rows+i;uint64_t at=index->offset+8+i*12;
        if(!row->keep){if(!ac_edit(cuts,allocated,at,12,NULL,0))goto done;continue;}
        uint64_t offset=output;
        if(row->length || row->offset!=data->length) {
            size_t lo=0,hi=merged;
            while(lo<hi){size_t mid=lo+(hi-lo)/2;if(ranges[mid].start<=row->offset)lo=mid+1;else hi=mid;}
            if(!lo || (uint64_t)row->offset+row->length>ranges[lo-1].end)goto done;
            offset=ranges[lo-1].output+row->offset-ranges[lo-1].start;
        }
        if(offset!=row->offset){ac_put32(bytes,(uint32_t)offset);if(!ac_edit(cuts,allocated,at+4,4,bytes,4))goto done;}
    }
    ok=1;
done: free(rows);free(ranges);return ok;
}
int sh_audio_bank_binding_select(sh_audio_bank_source source,const sh_audio_bank_codec *bank,
    const sh_audio_bank_binding *binding,int (*keep)(void *,sh_audio_identity_domain,uint32_t),
    void *context,sh_audio_bank_binding *out,char *error,size_t capacity)
{
    sh_audio_bank_binding cuts={0},projected={0};size_t cut_capacity=0,allocated=0,object_at=0;int ok=0;
    uint64_t length=0,cursor=0;unsigned char header[8];
    if(error && capacity)*error=0;
    if(!out || out==binding)return 0;memset(out,0,sizeof(*out));
    if(!bank || !binding || !source.read || source.length!=binding->input_length ||
        !ac_binding_length(binding,&length) || length!=binding->output_length ||
        (bank->chunk_count && !bank->chunks) || (bank->object_count && !bank->objects))goto done;
    cuts.input_length=projected.input_length=source.length;
    for(size_t c=0;c<bank->chunk_count;c++) {
        const sh_audio_bank_chunk *chunk=bank->chunks+c;
        if(chunk->offset!=cursor || !ac_read(source,cursor,header,8) ||
            memcmp(header,chunk->tag,4) || ac_u32(header+4)!=chunk->length ||
            chunk->length>source.length-cursor-8)goto done;
        uint64_t body=cursor+8,end=body+chunk->length;cursor=end;
        if(!strcmp(chunk->tag,"STMG") || !strcmp(chunk->tag,"ENVS") || !strcmp(chunk->tag,"PLAT")) {
            if(!ac_edit(&cuts,&cut_capacity,chunk->offset,(uint64_t)chunk->length+8,NULL,0))goto done;
        } else if(!strcmp(chunk->tag,"HIRC")) {
            if(chunk->length<4 || !ac_read(source,body,header,4))goto done;
            uint32_t count=ac_u32(header),seen=0,removed=0;uint64_t at=body+4,removed_bytes=0;
            while(object_at<bank->object_count && bank->objects[object_at].offset<end) {
                const sh_audio_bank_object *object=bank->objects+object_at++;
                if(object->offset!=at+5 || !ac_read(source,at,header,5) || header[0]!=object->kind ||
                    ac_u32(header+1)!=object->length || object->offset>end || object->length>end-object->offset || seen==UINT32_MAX)goto done;
                seen++;at=object->offset+object->length;
                int retain=1;
                if(keep && object->kind!=8 && object->kind!=20) {
                    int found=0;
                    if(object->first_span>bank->span_count || object->span_count>bank->span_count-object->first_span)goto done;
                    for(size_t f=object->first_span;f<object->first_span+object->span_count;f++) {
                        const sh_audio_identity_span *field=bank->spans+f;
                        if(field->offset==object->offset && (field->flags&SH_AUDIO_ID_DEFINITION)) {
                            retain=keep(context,field->domain,field->value);found=1;break;
                        }
                    }
                    if(!found || retain<0)goto done;
                }
                if(object->kind==8 || object->kind==20 || !retain) {
                    uint64_t bytes=(uint64_t)object->length+5;
                    if(!ac_edit(&cuts,&cut_capacity,object->offset-5,bytes,NULL,0))goto done;
                    removed++;removed_bytes+=bytes;
                }
            }
            if(seen!=count || at!=end)goto done;
            if(removed) {
                ac_put32(header,chunk->length-(uint32_t)removed_bytes);
                if(!ac_edit(&cuts,&cut_capacity,chunk->offset+4,4,header,4))goto done;
                ac_put32(header,count-removed);
                if(!ac_edit(&cuts,&cut_capacity,body,4,header,4))goto done;
            }
        }
    }
    if(cursor!=source.length || object_at!=bank->object_count)goto done;
    if(keep && !ac_select_media(source,bank,keep,context,&cuts,&cut_capacity))goto done;
    if(cuts.count>1)qsort(cuts.edits,cuts.count,sizeof(*cuts.edits),ac_edit_compare);
    if(!ac_binding_length(&cuts,&length))goto done;
    size_t cut=0;
    for(size_t i=0;i<binding->count;i++) {
        const sh_audio_bank_edit *edit=binding->edits+i;
        while(cut<cuts.count && cuts.edits[cut].offset+cuts.edits[cut].remove<=edit->offset)cut++;
        if(cut<cuts.count && (edit->offset+edit->remove>cuts.edits[cut].offset ||
            (!edit->remove && edit->offset==cuts.edits[cut].offset))) {
            const sh_audio_bank_edit *replacement=cuts.edits+cut;
            if(edit->offset<replacement->offset || edit->offset+edit->remove>replacement->offset+replacement->remove)goto done;
            continue;
        }
        if(!ac_edit(&projected,&allocated,edit->offset,edit->remove,edit->bytes,edit->length))goto done;
    }
    for(size_t i=0;i<cuts.count;i++) {
        const sh_audio_bank_edit *edit=cuts.edits+i;
        if(!ac_edit(&projected,&allocated,edit->offset,edit->remove,edit->bytes,edit->length))goto done;
    }
    if(projected.count>1)qsort(projected.edits,projected.count,sizeof(*projected.edits),ac_edit_compare);
    if(!ac_binding_length(&projected,&projected.output_length))goto done;
    *out=projected;memset(&projected,0,sizeof(projected));ok=1;
done:
    sh_audio_bank_binding_free(&cuts);sh_audio_bank_binding_free(&projected);
    if(!ok && error && capacity)snprintf(error,capacity,"cannot separate private audio from its shared policy");return ok;
}
int sh_audio_bank_binding_private(sh_audio_bank_source source,const sh_audio_bank_codec *bank,
    const sh_audio_bank_binding *binding,sh_audio_bank_binding *out,char *error,size_t capacity)
{
    return sh_audio_bank_binding_select(source,bank,binding,NULL,NULL,out,error,capacity);
}
typedef struct ac_window { sh_audio_bank_source source;uint64_t offset,length; } ac_window;
static int ac_window_read(void *context,uint64_t at,void *out,size_t length)
{
    const ac_window *window=context;
    return at<=window->length && length<=window->length-at && ac_read(window->source,window->offset+at,out,length);
}
typedef struct ac_memory_sink { unsigned char *body;size_t length,used; } ac_memory_sink;
static int ac_memory_write(void *context,const void *bytes,size_t length)
{
    ac_memory_sink *sink=context;if(length>sink->length-sink->used)return 0;
    if(length)memcpy(sink->body+sink->used,bytes,length);sink->used+=length;return 1;
}
int sh_audio_bank_binding_object(sh_audio_bank_source source,const sh_audio_bank_binding *binding,
    const sh_audio_bank_object *object,unsigned char **body,size_t *length,char *error,size_t capacity)
{
    sh_audio_bank_binding local={0};size_t allocated=0;uint64_t checked=0;unsigned char header[5];ac_memory_sink sink={0};int ok=0;
    if(body)*body=NULL;if(length)*length=0;if(error && capacity)*error=0;
    if(!body || !length || !binding || !object || source.length!=binding->input_length ||
        !ac_binding_length(binding,&checked) || checked!=binding->output_length || object->offset<5 ||
        object->offset>source.length || object->length>source.length-object->offset ||
        !ac_read(source,object->offset-5,header,5) || header[0]!=object->kind || ac_u32(header+1)!=object->length)goto done;
    local.input_length=object->length;uint64_t end=object->offset+object->length;
    for(size_t i=0;i<binding->count;i++) {
        const sh_audio_bank_edit *edit=binding->edits+i;
        if(edit->offset>=end)break;
        if(edit->offset+edit->remove<=object->offset && edit->offset<object->offset)continue;
        if(edit->offset<object->offset || edit->offset+edit->remove>end ||
            !ac_edit(&local,&allocated,edit->offset-object->offset,edit->remove,edit->bytes,edit->length))goto done;
    }
    if(!ac_binding_length(&local,&local.output_length) || local.output_length>SIZE_MAX)goto done;
    sink.length=(size_t)local.output_length;sink.body=malloc(sink.length ? sink.length : 1);if(!sink.body)goto done;
    ac_window window={source,object->offset,object->length};
    if(!sh_audio_bank_binding_write((sh_audio_bank_source){&window,object->length,ac_window_read},&local,
        ac_memory_write,&sink,error,capacity) || sink.used!=sink.length)goto done;
    *body=sink.body;*length=sink.length;sink.body=NULL;ok=1;
done:
    free(sink.body);sh_audio_bank_binding_free(&local);
    if(!ok && error && capacity && !*error)snprintf(error,capacity,"cannot capture bound audio object recipe");return ok;
}
int sh_audio_bank_binding_write(sh_audio_bank_source source, const sh_audio_bank_binding *binding,
    int (*write)(void *, const void *, size_t), void *context, char *error, size_t capacity)
{
    unsigned char buffer[65536]; uint64_t cursor=0,length=0;
    if(error && capacity)*error=0;
    /* Validate every edit before touching the caller's output transaction. */
    if(!binding || !write || !source.read || source.length!=binding->input_length ||
        !ac_binding_length(binding,&length) || length!=binding->output_length)goto bad;
    for (size_t i = 0; i <= binding->count; i++) {
        uint64_t end = i == binding->count ? source.length : binding->edits[i].offset;
        while (cursor < end) {
            size_t size = end - cursor > sizeof(buffer) ? sizeof(buffer) : (size_t)(end - cursor);
            if (!ac_read(source, cursor, buffer, size) || !write(context, buffer, size)) goto bad;
            cursor += size;
        }
        if (i < binding->count) {
            const sh_audio_bank_edit *edit = &binding->edits[i];
            if (!write(context, edit->bytes, edit->length)) goto bad;
            cursor += edit->remove;
        }
    }
    return 1;
bad:
    if (error && capacity) snprintf(error, capacity, "cannot stream complete typed audio bank binding");
    return 0;
}
