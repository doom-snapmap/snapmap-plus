#include "audio_bank_codec.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct bytes { unsigned char data[1024]; size_t length, media_at, media_length, reads; } bytes;
static void put(bytes *b, uint32_t value, size_t width)
{ assert(b->length + width <= sizeof(b->data)); for (size_t i = 0; i < width; i++) b->data[b->length++] = (unsigned char)(value >> (i * 8)); }
static void zeros(bytes *b, size_t count) { while (count--) put(b, 0, 1); }
static void chunk(bytes *b, const char *tag, uint32_t length)
{ memcpy(b->data + b->length, tag, 4); b->length += 4; put(b, length, 4); }
static int read_memory(void *context, uint64_t at, void *out, size_t length)
{
    bytes *b = context;
    if (at > b->length || length > b->length - at) return 0;
    assert(!length || at + length <= b->media_at || at >= b->media_at + b->media_length);
    b->reads += length; memcpy(out, b->data + at, length); return 1;
}
static void fixture(bytes *b)
{
    memset(b, 0, sizeof(*b));
    chunk(b, "BKHD", 16); put(b, 113, 4); put(b, 42, 4); zeros(b, 8);
    chunk(b, "DIDX", 12); put(b, 900, 4); put(b, 0, 4); put(b, 4, 4);
    chunk(b, "DATA", 4); b->media_at = b->length; b->media_length = 4; put(b, 900, 4);
    chunk(b, "HIRC", 68); put(b, 2, 4);
    put(b, 4, 1); put(b, 8, 4); put(b, 50, 4); put(b, 0, 4);
    put(b, 2, 1); put(b, 46, 4); put(b, 100, 4); put(b, 0x20001, 4); put(b, 0, 1);
    put(b, 900, 4); put(b, 4, 4); put(b, 0, 1); zeros(b, 28);
    chunk(b, "STID", 16); put(b, 1, 4); put(b, 1, 4); put(b, 42, 4); put(b, 3, 1);
    memcpy(b->data + b->length, "abc", 3); b->length += 3;
}
static void test_structure(void)
{
    bytes b; fixture(&b); sh_audio_bank_codec bank = {0}; char error[512];
    unsigned char before[1024]; memcpy(before, b.data, sizeof(before));
    sh_audio_bank_source source = {&b, b.length, read_memory};
    assert(sh_audio_bank_codec_read(source, &bank, error, sizeof(error)) == 1);
    assert(bank.metadata.id == 42 && bank.object_count == 2 && bank.chunk_count == 5);
    assert(bank.objects[0].kind == 4 && bank.objects[0].id == 50 && bank.objects[0].span_count == 1);
    assert(bank.name_count == 1 && bank.names[0].id == 42 && !strcmp(bank.names[0].name, "abc"));
    assert(bank.span_count == 8 && bank.spans[0].offset == 12 && bank.spans[0].domain == SH_AUDIO_ID_BANK);
    assert(bank.spans[1].offset == 32 && bank.spans[1].domain == SH_AUDIO_ID_MEDIA);
    assert(!bank.shared_chunks && !memcmp(before, b.data, sizeof(before))); sh_audio_bank_codec_free(&bank);
    /* Media lengths are checked while the DATA payload is never read. */
    b.data[40] = 5;
    assert(sh_audio_bank_codec_read(source, &bank, error, sizeof(error)) == 0 && !bank.spans);
    b.data[40] = 4; b.data[b.length - 2] = '/';
    assert(sh_audio_bank_codec_read(source, &bank, error, sizeof(error)) == 0 && !bank.names);
    b.data[b.length - 2] = 'b'; chunk(&b, "TEST", 0); source.length = b.length;
    assert(sh_audio_bank_codec_read(source, &bank, error, sizeof(error)) == -2 && !bank.chunks);
    assert(*error);
}
static uint32_t get32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void set32(unsigned char *p, uint32_t value)
{ for (size_t i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (i * 8)); }
static void add_music(bytes *b)
{
    bytes object = {0}; put(&object, 13, 1); put(&object, 146, 4);
    put(&object, 400, 4); zeros(&object, 1); zeros(&object, 28); zeros(&object, 4); zeros(&object, 23); zeros(&object, 4);
    put(&object, 1, 4); put(&object, 3, 4); put(&object, 5, 4); put(&object, 6, 4); put(&object, 7, 4);
    put(&object, 2, 4); put(&object, UINT32_MAX, 4); put(&object, 5, 4); zeros(&object, 46); zeros(&object, 4);
    assert(object.length == 151);
    size_t stid = b->length - 24;
    memmove(b->data + stid + object.length, b->data + stid, 24);
    memcpy(b->data + stid, object.data, object.length); b->length += object.length;
    set32(b->data + 60, 68 + (uint32_t)object.length); set32(b->data + 64, 3);
}
typedef struct resolver { size_t calls, fail_after; } resolver;
static int assign(void *context, const sh_audio_identity_span *field, uint32_t *value, char *error, size_t capacity)
{
    resolver *r = context;
    assert(!(field->flags & (SH_AUDIO_ID_SHARED | SH_AUDIO_ID_LITERAL)));
    if (r->calls++ == r->fail_after) { snprintf(error, capacity, "fixture assignment refused"); return 0; }
    if (field->domain == SH_AUDIO_ID_NODE && field->value >= 5 && field->value <= 7) *value = 9012 - field->value;
    else *value = field->value + 1000;
    return 1;
}
static int assign_occurrence(void *context, const sh_audio_identity_span *field, uint32_t *value, char *error, size_t capacity)
{
    if (!assign(context, field, value, error, capacity)) return 0;
    if (field->domain == SH_AUDIO_ID_NODE && field->value == 5) *value = (uint32_t)field->offset + 2000;
    return 1;
}
static int read_copy(void *context, uint64_t at, void *out, size_t length)
{
    bytes *b = context;
    if (at > b->length || length > b->length - at) return 0;
    memcpy(out, b->data + at, length); return 1;
}
static int sink(void *context, const void *data, size_t length)
{
    bytes *b = context; ++b->reads;
    if (length > sizeof(b->data) - b->length) return 0;
    memcpy(b->data + b->length, data, length); b->length += length; return 1;
}
static void test_binding(void)
{
    bytes b, output = {0}; fixture(&b); add_music(&b);
    sh_audio_bank_source source = {&b, b.length, read_memory}; sh_audio_bank_codec bank = {0}; char error[512];
    assert(sh_audio_bank_codec_read(source, &bank, error, sizeof(error)) == 1);
    assert(bank.ordered_count == 2 && bank.ordered[0].count == 3 && bank.ordered[1].fixed_prefix == 1);
    resolver r = {0, SIZE_MAX}; sh_audio_bank_binding binding = {0};
    assert(sh_audio_bank_codec_bind(source, &bank, assign, &r, &binding, error, sizeof(error)) == 1);
    assert(binding.count && binding.output_length == b.length + 1);
    source.read = read_copy;
    assert(sh_audio_bank_binding_write(source, &binding, sink, &output, error, sizeof(error)) == 1);
    assert(output.length == binding.output_length && get32(b.data + 12) == 42);
    assert(get32(output.data + 12) == 1042 && get32(output.data + 32) == 1900);
    assert(get32(output.data + b.media_at) == 900); /* Same-number opaque DATA untouched. */
    size_t list = bank.ordered[0].offset;
    assert(get32(output.data + list) == 9005 && get32(output.data + list + 4) == 9006 && get32(output.data + list + 8) == 9007);
    list = bank.ordered[1].offset;
    assert(get32(output.data + list) == UINT32_MAX && get32(output.data + list + 4) == 9007);
    sh_audio_bank_codec bound = {0}; sh_audio_bank_source bound_source = {&output, output.length, read_copy};
    assert(sh_audio_bank_codec_read(bound_source, &bound, error, sizeof(error)) == 1);
    assert(bound.metadata.id == 1042 && bound.name_count == 1 && !strcmp(bound.names[0].name, "1042"));
    sh_audio_bank_codec_free(&bound);
    binding.edits[0].offset = source.length + 1; output.length = output.reads = 0;
    assert(!sh_audio_bank_binding_write(source, &binding, sink, &output, error, sizeof(error)) && !output.reads);
    sh_audio_bank_binding_free(&binding);
    /* Equal numeric names at two typed fields can bind different scoped
     * children. Resolution is per field, not a bank-wide ID substitution. */
    r = (resolver){0, SIZE_MAX};
    assert(sh_audio_bank_codec_bind(source, &bank, assign_occurrence, &r, &binding, error, sizeof(error)) == 1);
    output.length = 0;
    assert(sh_audio_bank_binding_write(source, &binding, sink, &output, error, sizeof(error)) == 1);
    assert(get32(output.data + bank.ordered[0].offset) == bank.ordered[0].offset + 2000);
    assert(get32(output.data + bank.ordered[1].offset + 4) == bank.ordered[1].offset + 2004);
    sh_audio_bank_binding_free(&binding);
    r = (resolver){0, 1};
    assert(!sh_audio_bank_codec_bind(source, &bank, assign, &r, &binding, error, sizeof(error)) && !binding.count && !binding.edits);
    b.data[12] = 43; r = (resolver){0, SIZE_MAX};
    assert(!sh_audio_bank_codec_bind(source, &bank, assign, &r, &binding, error, sizeof(error)) && !binding.edits);
    sh_audio_bank_codec_free(&bank);
}
static void bus(bytes *b, unsigned kind, uint32_t id, uint32_t parent, uint32_t effect)
{
    put(b, kind, 1); put(b, 43, 4); put(b, id, 4); put(b, parent, 4);
    zeros(b, 23); put(b, effect, 4); zeros(b, 8);
}
static void shared_fixture(bytes *b)
{
    fixture(b);
    /* The cooked-bank reader accepts one mixed hierarchy per bank. */
    size_t stid = b->length - 24;
    memmove(b->data + stid + 96, b->data + stid, 24);
    b->length = stid; bus(b, 8, 200, 0, 777); bus(b, 20, 201, 200, 888); b->length += 24;
    set32(b->data + 60, 164); set32(b->data + 64, 4);
    chunk(b, "STMG", 18); zeros(b, 18);
    chunk(b, "ENVS", 24); zeros(b, 24);
    chunk(b, "PLAT", 11); put(b, 7, 4);
    memcpy(b->data + b->length, "Windows", 7); b->length += 7;
}
static void test_private_projection(void)
{
    bytes b, output = {0}, again = {0}; shared_fixture(&b);
    unsigned char before[1024]; memcpy(before, b.data, sizeof(before));
    sh_audio_bank_source source = {&b, b.length, read_memory};
    sh_audio_bank_codec bank = {0}, parsed = {0}; char error[512];
    sh_audio_bank_binding full = {0}, private = {0}, twice = {0}; resolver r = {0, SIZE_MAX};
    int status = sh_audio_bank_codec_read(source, &bank, error, sizeof(error));
    if (status != 1) fprintf(stderr, "shared fixture: %s\n", error);
    assert(status == 1 && bank.object_count == 4 && bank.shared_chunks);
    assert(sh_audio_bank_codec_bind(source, &bank, assign, &r, &full, error, sizeof(error)));
    assert(sh_audio_bank_binding_private(source, &bank, &full, &private, error, sizeof(error)));
    assert(sh_audio_bank_binding_private(source, &bank, &private, &twice, error, sizeof(error)));
    assert(private.output_length == full.output_length - 96 - 26 - 32 - 19);
    /* A bus keeps its shared ID/parent while referring to the privately bound
     * effect. Export does not read the unrelated embedded media. */
    for (size_t i = 2; i < 4; i++) {
        unsigned char *body = NULL; size_t length = 0;
        assert(sh_audio_bank_binding_object(source, &full, bank.objects + i, &body, &length, error, sizeof(error)));
        assert(length == 43 && get32(body) == (i == 2 ? 200 : 201));
        assert(get32(body + 4) == (i == 2 ? 0 : 200));
        assert(get32(body + 31) == (i == 2 ? 1777 : 1888)); free(body);
        assert(!sh_audio_bank_binding_object(source, &private, bank.objects + i, &body, &length, error, sizeof(error)));
        assert(!body && !length);
    }
    assert(!memcmp(before, b.data, sizeof(before)));
    source.read = read_copy;
    assert(sh_audio_bank_binding_write(source, &private, sink, &output, error, sizeof(error)));
    assert(sh_audio_bank_binding_write(source, &twice, sink, &again, error, sizeof(error)));
    assert(output.length == again.length && !memcmp(output.data, again.data, output.length));
    assert(get32(output.data + b.media_at) == 900);
    assert(sh_audio_bank_codec_read((sh_audio_bank_source){&output, output.length, read_copy}, &parsed, error, sizeof(error)) == 1);
    assert(parsed.object_count == 2 && parsed.chunk_count == 5 && !parsed.shared_chunks);
    assert(parsed.objects[0].id == 1050 && parsed.objects[1].id == 1100);
    assert(!strcmp(parsed.names[0].name, "1042"));
    assert(!strcmp(parsed.chunks[3].tag, "HIRC") && parsed.chunks[3].length == 68);
    sh_audio_bank_codec_free(&parsed); sh_audio_bank_binding_free(&twice);
    /* Stale metadata and edits straddling retained/removed bytes must fail
     * without changing either the retained full binding or the source. */
    source.read = read_memory; b.data[bank.objects[2].offset - 5] = 20;
    assert(!sh_audio_bank_binding_private(source, &bank, &full, &twice, error, sizeof(error)) && !twice.edits);
    b.data[bank.objects[2].offset - 5] = 8;
    sh_audio_bank_edit crossing = {0}; crossing.offset = bank.objects[2].offset - 6; crossing.remove = 2;
    sh_audio_bank_binding malformed = {&crossing, 1, b.length, b.length - 2};
    assert(!sh_audio_bank_binding_private(source, &bank, &malformed, &twice, error, sizeof(error)) && !twice.edits);
    unsigned char *body = NULL; size_t length = 0;
    crossing.offset = bank.objects[2].offset - 1;
    assert(!sh_audio_bank_binding_object(source, &malformed, bank.objects + 2, &body, &length, error, sizeof(error)));
    assert(!body && !length && *error);
    assert(full.output_length == b.length + 1 && !memcmp(before, b.data, sizeof(before)));
    sh_audio_bank_binding_free(&private); sh_audio_bank_binding_free(&full); sh_audio_bank_codec_free(&bank);
    /* Removing the last shared definition produces an empty valid hierarchy. */
    memset(&b, 0, sizeof(b)); chunk(&b, "BKHD", 16); put(&b, 113, 4); put(&b, 42, 4); zeros(&b, 8);
    chunk(&b, "HIRC", 52); put(&b, 1, 4); bus(&b, 20, 201, 0, 0);
    source = (sh_audio_bank_source){&b, b.length, read_memory};
    assert(sh_audio_bank_codec_read(source, &bank, error, sizeof(error)) == 1);
    full.input_length = full.output_length = b.length;
    assert(sh_audio_bank_binding_private(source, &bank, &full, &private, error, sizeof(error)));
    output.length = 0;
    assert(sh_audio_bank_binding_write(source, &private, sink, &output, error, sizeof(error)));
    assert(output.length == 36 && get32(output.data + 28) == 4 && get32(output.data + 32) == 0);
    assert(sh_audio_bank_codec_read((sh_audio_bank_source){&output, output.length, read_copy}, &parsed, error, sizeof(error)) == 1);
    assert(!parsed.object_count); sh_audio_bank_codec_free(&parsed);
    sh_audio_bank_binding_free(&private); sh_audio_bank_codec_free(&bank);
}
typedef struct sparse_bank { unsigned char head[32], tail[12]; uint64_t tail_at; size_t reads; } sparse_bank;
static int read_sparse(void *context, uint64_t at, void *out, size_t length)
{
    sparse_bank *b = context; b->reads += length;
    if (at <= sizeof(b->head) && length <= sizeof(b->head) - at) { memcpy(out, b->head + at, length); return 1; }
    if (at >= b->tail_at && at - b->tail_at <= sizeof(b->tail) && length <= sizeof(b->tail) - (at - b->tail_at)) {
        memcpy(out, b->tail + (at - b->tail_at), length); return 1;
    }
    assert(!"projection attempted to read the discarded payload"); return 0;
}
static void test_large_removal(void)
{
    /* Structural projection uses 64-bit extents even when a chunk's 32-bit
     * length plus its header exceeds UINT32_MAX. No giant allocation needed. */
    sparse_bank b = {0}; b.tail_at = (uint64_t)UINT32_MAX + 32;
    memcpy(b.head, "BKHD", 4); set32(b.head + 4, 16); set32(b.head + 8, 113); set32(b.head + 12, 42);
    memcpy(b.head + 24, "STMG", 4); set32(b.head + 28, UINT32_MAX);
    memcpy(b.tail, "HIRC", 4); set32(b.tail + 4, 4);
    sh_audio_bank_source source = {&b, b.tail_at + sizeof(b.tail), read_sparse};
    sh_audio_bank_chunk chunks[] = {{"BKHD", 0, 16}, {"STMG", 24, UINT32_MAX}, {"HIRC", b.tail_at, 4}};
    sh_audio_bank_codec bank = {0}; bank.chunks = chunks; bank.chunk_count = 3;
    sh_audio_bank_binding full = {NULL, 0, source.length, source.length}, private = {0};
    bytes output = {0}; char error[512];
    assert(sh_audio_bank_binding_private(source, &bank, &full, &private, error, sizeof(error)));
    assert(private.count == 1 && private.edits[0].remove == (uint64_t)UINT32_MAX + 8 && private.output_length == 36);
    assert(sh_audio_bank_binding_write(source, &private, sink, &output, error, sizeof(error)));
    assert(output.length == 36 && !memcmp(output.data, b.head, 24) && !memcmp(output.data + 24, b.tail, 12));
    assert(b.reads < 100); sh_audio_bank_binding_free(&private);
}
typedef struct file_source { FILE *file; size_t read_bytes; } file_source;
static int read_file(void *context, uint64_t offset, void *out, size_t length)
{
    file_source *f = context; f->read_bytes += length;
    return !_fseeki64(f->file, (__int64)offset, SEEK_SET) && fread(out, 1, length, f->file) == length;
}
static void corpus(const char *path)
{
    FILE *file = fopen(path, "rb"); assert(file); assert(!_fseeki64(file, 0, SEEK_END));
    uint64_t length = (uint64_t)_ftelli64(file); file_source reader = {file, 0};
    sh_audio_bank_source source = {&reader, length, read_file}; sh_audio_bank_codec bank = {0}; char error[512];
    int status = sh_audio_bank_codec_read(source, &bank, error, sizeof(error));
    if (status != 1) fprintf(stderr, "%s: %s\n", path, error);
    assert(status == 1);
    sh_audio_bank_binding full = {0}, private = {0}; resolver r = {0, SIZE_MAX}; size_t buses = 0;
    assert(sh_audio_bank_codec_bind(source, &bank, assign, &r, &full, error, sizeof(error)));
    assert(sh_audio_bank_binding_private(source, &bank, &full, &private, error, sizeof(error)));
    for (size_t i = 0; i < bank.object_count; i++) if (bank.objects[i].kind == 8 || bank.objects[i].kind == 20) {
        unsigned char *body = NULL; size_t size = 0; sh_audio_hirc parsed = {0}; buses++;
        assert(sh_audio_bank_binding_object(source, &full, bank.objects + i, &body, &size, error, sizeof(error)));
        assert(sh_audio_hirc_read(bank.objects[i].kind, body, size, &parsed, error, sizeof(error)) == 1);
        assert(size == bank.objects[i].length && get32(body) == bank.objects[i].id);
        sh_audio_hirc_free(&parsed); free(body);
    }
    printf("%s: objects=%zu spans=%zu bank_names=%zu shared=%u inspected=%zu file=%llu private=%llu buses=%zu\n", path,
        bank.object_count, bank.span_count, bank.name_count, bank.shared_chunks,
        reader.read_bytes, (unsigned long long)length, (unsigned long long)private.output_length, buses);
    sh_audio_bank_binding_free(&private); sh_audio_bank_binding_free(&full);
    sh_audio_bank_codec_free(&bank); fclose(file);
}
static int selected_media(void *context,sh_audio_identity_domain domain,uint32_t id)
{
    if(context)return -1;
    return domain==SH_AUDIO_ID_MEDIA && (id==901 || id==902);
}
static void test_selected_projection(void)
{
    bytes b={0},out={0};
    chunk(&b,"BKHD",16);put(&b,113,4);put(&b,42,4);zeros(&b,8);
    chunk(&b,"DIDX",48);
    put(&b,900,4);put(&b,0,4);put(&b,4,4);
    put(&b,902,4);put(&b,34,4);put(&b,2,4); /* Unsorted and overlapping kept ranges. */
    put(&b,901,4);put(&b,32,4);put(&b,4,4);
    put(&b,903,4);put(&b,64,4);put(&b,4,4);
    chunk(&b,"DATA",80);b.media_at=b.length;b.media_length=80;
    for(unsigned i=0;i<80;i++)put(&b,i,1);
    chunk(&b,"HIRC",4);put(&b,0,4);
    sh_audio_bank_source source={&b,b.length,read_memory};
    sh_audio_bank_codec bank={0},parsed={0};sh_audio_bank_binding full={0},projected={0},bad={0};
    char error[512];resolver ids={0,SIZE_MAX};
    assert(sh_audio_bank_codec_read(source,&bank,error,sizeof(error))==1);
    assert(sh_audio_bank_codec_bind(source,&bank,assign,&ids,&full,error,sizeof(error)));
    assert(sh_audio_bank_binding_select(source,&bank,&full,selected_media,NULL,&projected,error,sizeof(error)));
    /* Compilation above must not read media. Only final streaming reads it. */
    source.read=read_copy;
    assert(sh_audio_bank_binding_write(source,&projected,sink,&out,error,sizeof(error)));
    assert(sh_audio_bank_codec_read((sh_audio_bank_source){&out,out.length,read_copy},&parsed,error,sizeof(error))==1);
    assert(parsed.chunk_count==4 && parsed.chunks[1].length==24 && parsed.chunks[2].length==32);
    assert(get32(out.data+32)==1902 && get32(out.data+36)==2 && get32(out.data+40)==2);
    assert(get32(out.data+44)==1901 && get32(out.data+48)==0 && get32(out.data+52)==4);
    assert(!memcmp(out.data+parsed.chunks[2].offset+8,b.data+b.media_at+32,32));
    assert(!sh_audio_bank_binding_select(source,&bank,&full,selected_media,&ids,&bad,error,sizeof(error)) && !bad.edits);
    sh_audio_bank_binding_free(&projected);sh_audio_bank_binding_free(&full);
    sh_audio_bank_codec_free(&parsed);sh_audio_bank_codec_free(&bank);
}
int main(int argc, char **argv)
{
    test_structure(); test_binding(); test_private_projection(); test_large_removal(); test_selected_projection();
    if (argc == 2) {
        WIN32_FIND_DATAA entry; char search[4096], path[4096], localized[4096];
        snprintf(search, sizeof(search), "%s/*.bnk", argv[1]); HANDLE find = FindFirstFileA(search, &entry);
        assert(find != INVALID_HANDLE_VALUE);
        do {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            snprintf(path, sizeof(path), "%s/%s", argv[1], entry.cFileName);
            snprintf(localized, sizeof(localized), "%s/English(US)/%s", argv[1], entry.cFileName);
            corpus(GetFileAttributesA(localized) != INVALID_FILE_ATTRIBUTES ? localized : path);
        } while (FindNextFileA(find, &entry));
        FindClose(find);
        snprintf(search, sizeof(search), "%s/English(US)/*.bnk", argv[1]); find = FindFirstFileA(search, &entry);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                snprintf(path, sizeof(path), "%s/%s", argv[1], entry.cFileName);
                if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) continue;
                snprintf(localized, sizeof(localized), "%s/English(US)/%s", argv[1], entry.cFileName); corpus(localized);
            } while (FindNextFileA(find, &entry));
            FindClose(find);
        }
    }
    puts("typed bank structure checks passed"); return 0;
}
