#include "package_audio_capture.h"
#include <stdlib.h>
#include <string.h>

typedef struct pac_file {
    struct pac_file *next;
    char *path;
    unsigned char *body;
    sh_audio_originals *originals;
    FILE *stream;
    sh_audio_bank_source source;
    sh_audio_bank_codec codec;
    int original, bank, cached;
    uint32_t media_id;
} pac_file;
struct sh_package_audio_capture {
    const sh_package_compilation *compiled;
    sh_audio_originals *originals;
    sh_package_audio_scope *scope;
    pac_file *files, *tail;
    sh_audio_graph_bank *banks;
    sh_audio_graph_scope input;
    size_t file_count;
    int inventory;
    uint32_t *roots;
    size_t root_count;
};
typedef struct pac_visit {
    sh_package_audio_capture *capture;
    pac_file *last;
    char *error;
    size_t capacity;
} pac_visit;
static int pac_fail(char *error, size_t capacity, const char *message, const char *path)
{ if (error && capacity) snprintf(error, capacity, "%s: %s", message, path ? path : ""); return 0; }
static int pac_read(void *context, uint64_t offset, void *out, size_t length)
{
    pac_file *file = context;
    if (offset > file->source.length || length > file->source.length - offset) return 0;
    if (file->originals) return sh_audio_originals_read(file->originals, file->path, offset, out, length, NULL, NULL, 0) == 1;
    if (file->body) { if (length) memcpy(out, file->body + (size_t)offset, length); return 1; }
    if (offset > INT64_MAX) return 0;
    _lock_file(file->stream);
    int ok = !_fseeki64(file->stream, (__int64)offset, SEEK_SET) && fread(out, 1, length, file->stream) == length;
    _unlock_file(file->stream); return ok;
}
static void pac_file_close(pac_file *file)
{
    if (!file) return;
    if (file->stream) fclose(file->stream);
    sh_audio_bank_codec_free(&file->codec); free(file->body); free(file->path); free(file);
}
/* A positive digest match alone does not certify native-original equivalence. */
static int pac_original(pac_file *file, sh_audio_originals *originals, char *error, size_t capacity)
{
    sh_package_original_identity baseline = {0}; sh_package_file_identity identity;
    int result = sh_audio_originals_identity(originals, file->path, &baseline, error, capacity);
    if (result < 0) return 0;
    if (!result || !baseline.scope || baseline.file.length != file->source.length) return 1;
    if (!sh_package_reader_identity(file, 0, file->source.length, pac_read, &identity))
        return pac_fail(error, capacity, "cannot fingerprint captured audio", file->path);
    if (memcmp(identity.digest, baseline.file.digest, sizeof(identity.digest))) return 1;
    unsigned char a[8192], b[8192];
    for (uint64_t offset = 0; offset < file->source.length;) {
        size_t length = file->source.length - offset > sizeof(a) ? sizeof(a) : (size_t)(file->source.length - offset);
        if (!pac_read(file, offset, a, length) ||
            sh_audio_originals_read(originals, file->path, offset, b, length, NULL, error, capacity) != 1)
            return pac_fail(error, capacity, "cannot compare captured audio original", file->path);
        if (memcmp(a, b, length)) return pac_fail(error, capacity, "audio original fingerprint collision", file->path);
        offset += length;
    }
    file->original = 1; return 1;
}
static int pac_path(void *context, const char *path, int original)
{
    pac_visit *visit = context; sh_package_audio_capture *capture = visit->capture;
    for (pac_file *file = capture->files; file; file = file->next) if (!strcmp(file->path, path)) {
        visit->last = file; return 1;
    }
    pac_file *file = calloc(1, sizeof(*file)); if (!file) goto failed;
    file->path = _strdup(path); if (!file->path) goto failed;
    file->source = (sh_audio_bank_source){file, 0, pac_read}; file->original = !!original;
    if (original) {
        file->originals = capture->originals;
        if (sh_audio_originals_read(capture->originals, path, 0, NULL, 0, &file->source.length, visit->error, visit->capacity) != 1) goto failed;
    } else {
        const sh_compiled_resource *r = sh_package_compilation_find(capture->compiled, path);
        if (!r) goto failed;
        if (r->body) {
            file->source.length = r->body_length;
            file->body = malloc(r->body_length ? r->body_length : 1);
            if (!file->body) goto failed;
            memcpy(file->body, r->body, r->body_length);
        }
        else if (r->cache_file) {
            file->cached = 1;
            file->stream = sh_package_file_open(r->cache_file, &file->source.length, visit->error, visit->capacity);
            if (!file->stream) goto failed;
        } else {
            if (!capture->compiled->sources || r->source >= capture->compiled->sources->file_count) goto failed;
            sh_package_source_file source_file = capture->compiled->sources->files[r->source];
            if (r->cache_path) source_file.absolute = r->cache_path;
            file->cached = r->cache_path != NULL;
            file->stream = sh_package_source_open(&source_file, visit->error, visit->capacity);
            if (!file->stream) goto failed;
            file->source.length = source_file.length;
        }
        if (!pac_original(file, capture->originals, visit->error, visit->capacity)) goto failed;
    }
    size_t length = strlen(path); file->bank = length > 4 && !strcmp(path + length - 4, ".bnk");
    if (file->bank && sh_audio_bank_codec_read(file->source, &file->codec, visit->error, visit->capacity) != 1) goto failed;
    if (file->bank && !sh_package_audio_scope_refine(capture->scope, path, &file->codec.metadata)) goto failed;
    if (file->bank && !sh_package_audio_scope_references(capture->scope, path, file->codec.spans, file->codec.span_count)) goto failed;
    if (capture->tail) capture->tail->next = file; else capture->files = file;
    capture->tail = file; capture->file_count++; visit->last = file; return 1;
failed:
    if (visit->error && visit->capacity && !*visit->error) pac_fail(visit->error, visit->capacity, "cannot capture selected audio source", path);
    pac_file_close(file); return 0;
}
static int pac_media(void *context, uint32_t id, sh_audio_graph_media *out, char *error, size_t capacity)
{
    sh_package_audio_capture *capture = context; pac_visit visit = {capture, NULL, error, capacity};
    if (!capture->scope) { pac_fail(error, capacity, "audio capture is sealed", NULL); return -1; }
    int result = sh_package_audio_scope_media(capture->scope, id, pac_path, &visit, error, capacity);
    if (result == 1) {
        visit.last->media_id = id;
        *out = (sh_audio_graph_media){visit.last->path, visit.last->source, visit.last->original};
    }
    return result;
}
static int pac_collect(void *context, const char *path, int original)
{
    sh_package_audio_capture *capture = context; (void)original;
    for (pac_file *file = capture->files; file; file = file->next) if (!strcmp(file->path, path)) {
        if (file->bank) capture->banks[capture->input.bank_count++] =
            (sh_audio_graph_bank){file->path, file->source, &file->codec, file->original};
        return 1;
    }
    return 0;
}
void sh_package_audio_capture_close(sh_package_audio_capture *capture)
{
    if (!capture) return;
    while (capture->files) { pac_file *file = capture->files; capture->files = file->next; pac_file_close(file); }
    free(capture->roots);free(capture->banks); sh_package_audio_scope_close(capture->scope); free(capture);
}
static int pac_select(sh_package_audio_capture *capture,const uint32_t *events,size_t count,
    sh_package_audio_path_visit visit,void *context,char *error,size_t capacity)
{
    if(capture->inventory==2)return !count && sh_package_audio_scope_roots(capture->scope,
        capture->roots,capture->root_count,visit,context,error,capacity);
    return (capture->inventory ? sh_package_audio_scope_inventory : sh_package_audio_scope_events)
        (capture->scope,events,count,visit,context,error,capacity);
}
int sh_package_audio_capture_extend(sh_package_audio_capture *capture,
    const uint32_t *events, size_t count, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    if (!capture || !capture->scope) return pac_fail(error, capacity, "audio capture cannot be extended after seal", NULL);
    pac_visit visit = {capture, NULL, error, capacity};
    /* Typed readers can discover dependencies absent from the lightweight
     * inventory. Repeat the shared closure until those captures add no files. */
    for (;;) {
        size_t before = capture->file_count;
        if (!pac_select(capture, events, count, pac_path, &visit, error, capacity)) goto failed;
        if (capture->file_count == before) break;
    }
    size_t banks = 0;
    for (pac_file *file = capture->files; file; file = file->next) if (file->bank) banks++;
    if (banks > SIZE_MAX / sizeof(*capture->banks)) goto failed;
    sh_audio_graph_bank *selected = calloc(banks ? banks : 1, sizeof(*selected)); if (!selected) goto failed;
    free(capture->banks); capture->banks = selected;
    capture->input = (sh_audio_graph_scope){capture->banks, 0, capture, pac_media};
    if (!pac_select(capture, events, count, pac_collect, capture, error, capacity)) goto failed;
    return 1;
failed:
    if (error && capacity && !*error) pac_fail(error, capacity, "cannot prepare captured audio scope", NULL);
    return 0;
}
static sh_package_audio_capture *pac_open(const sh_package_compilation *compiled,
    sh_audio_originals *originals, const uint32_t *events, size_t count, int inventory, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    sh_package_audio_capture *capture = calloc(1, sizeof(*capture));
    if (!capture) { pac_fail(error, capacity, "cannot allocate audio capture", NULL); return NULL; }
    capture->compiled = compiled; capture->originals = originals; capture->inventory = inventory;
    if(inventory==2) {
        if((count && !events) || count>SIZE_MAX/sizeof(*capture->roots))goto failed;
        capture->roots=malloc((count ? count : 1)*sizeof(*capture->roots));if(!capture->roots)goto failed;
        if(count)memcpy(capture->roots,events,count*sizeof(*capture->roots));capture->root_count=count;events=NULL;count=0;
    }
    capture->scope = sh_package_audio_scope_open(compiled, originals, error, capacity);
    if (!capture->scope) goto failed;
    if (inventory) {
        pac_visit visit = {capture, NULL, error, capacity};
        if (!sh_package_audio_scope_banks(capture->scope, pac_path, &visit)) goto failed;
    }
    if (!sh_package_audio_capture_extend(capture, events, count, error, capacity)) goto failed;
    return capture;
failed:
    sh_package_audio_capture_close(capture); return NULL;
}
sh_package_audio_capture *sh_package_audio_capture_open(const sh_package_compilation *compiled,
    sh_audio_originals *originals, const uint32_t *events, size_t count, char *error, size_t capacity)
{ return pac_open(compiled, originals, events, count, 0, error, capacity); }
sh_package_audio_capture *sh_package_audio_capture_open_inventory(const sh_package_compilation *compiled,
    sh_audio_originals *originals, char *error, size_t capacity)
{ return pac_open(compiled, originals, NULL, 0, 1, error, capacity); }
sh_package_audio_capture *sh_package_audio_capture_open_roots(const sh_package_compilation *compiled,
    sh_audio_originals *originals,const uint32_t *banks,size_t count,char *error,size_t capacity)
{ return pac_open(compiled,originals,banks,count,2,error,capacity); }
const sh_audio_graph_scope *sh_package_audio_capture_input(sh_package_audio_capture *capture)
{ return capture ? &capture->input : NULL; }

void sh_package_audio_capture_seal(sh_package_audio_capture *capture)
{
    if (!capture) return;
    sh_package_audio_scope_close(capture->scope); capture->scope = NULL;
    free(capture->roots);capture->roots=NULL;capture->root_count=0;
    capture->compiled = NULL;
}

int sh_package_audio_capture_media(const sh_package_audio_capture *capture,
    int (*visit)(void *, uint32_t, const sh_audio_graph_media *), void *context)
{
    if (!visit) return 0;
    for (const pac_file *file = capture ? capture->files : NULL; file; file = file->next) if (file->media_id) {
        sh_audio_graph_media media = {file->path, file->source, file->original};
        if (!visit(context, file->media_id, &media)) return 0;
    }
    return 1;
}
static int pac_cache_write(void *context, int (*write)(void *, const void *, size_t),
    void *sink, char *error, size_t capacity)
{
    pac_file *file = context; unsigned char bytes[65536];
    for (uint64_t offset = 0; offset < file->source.length;) {
        size_t length = file->source.length - offset < sizeof(bytes) ? (size_t)(file->source.length - offset) : sizeof(bytes);
        if (!pac_read(file, offset, bytes, length) || !write(sink, bytes, length))
            return pac_fail(error, capacity, "cannot snapshot captured audio file", file->path);
        offset += length;
    }
    return 1;
}
int sh_package_audio_capture_cache(sh_package_audio_capture *capture,
    const char *directory, char *error, size_t capacity)
{
    typedef struct replacement { pac_file *file; FILE *stream; } replacement;
    replacement *rows = NULL; size_t count = 0, at = 0; int ok = 0;
    if (!capture) return 1;
    for (pac_file *f = capture->files; f; f = f->next) if (f->stream && !f->cached) count++;
    if (!count) return 1;
    if (count > SIZE_MAX / sizeof(*rows)) goto done;
    rows = calloc(count, sizeof(*rows)); if (!rows) goto done;
    for (pac_file *f = capture->files; f; f = f->next) if (f->stream && !f->cached) {
        char *path = NULL; uint64_t length = 0;
        sh_package_file *cached = sh_package_file_generate(directory, f->source.length, NULL,
            pac_cache_write, f, &path, error, capacity);
        rows[at].file = f;
        rows[at].stream = cached ? sh_package_file_open(cached, &length, error, capacity) : NULL;
        sh_package_file_release(cached); free(path);
        if (!rows[at].stream || length != f->source.length) goto done;
        at++;
    }
    for (size_t i = 0; i < count; i++) {
        fclose(rows[i].file->stream); rows[i].file->stream = rows[i].stream;
        rows[i].file->cached = 1; rows[i].stream = NULL;
    }
    ok = 1;
done:
    if (rows) for (size_t i = 0; i < count; i++) if (rows[i].stream) fclose(rows[i].stream);
    free(rows);
    if (!ok && error && capacity && !*error) pac_fail(error, capacity, "cannot snapshot captured audio files", NULL);
    return ok;
}
