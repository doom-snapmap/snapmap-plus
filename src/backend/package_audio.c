#include "package_audio.h"
#include <stdlib.h>
#include <string.h>

typedef struct pa_bank {
    char *path;
    const char *filename;
    sh_audio_bank bank;
    sh_package_file_identity identity;
    int contributed;
} pa_bank;
struct sh_package_audio {
    pa_bank *banks;
    size_t count;
    sh_package_audio_reference *events;
    size_t event_count;
    sh_package_audio_bank *inventory;
    sh_package_audio_media *media;
    size_t media_count;
};

static int pa_memory(void *context, uint64_t offset, void *out, size_t length)
{
    const sh_compiled_resource *resource = context;
    if (offset > resource->body_length || length > resource->body_length - offset) return 0;
    memcpy(out, resource->body + (size_t)offset, length); return 1;
}

/* Native audio files live under one virtual root, with at most one language
 * directory between it and the file. suffix selects the resource kind. */
static const char *pa_filename(const char *path, const char *suffix)
{
    static const char root[] = "sound/soundbanks/pc/";
    const char *name;
    size_t length;
    if (!path || strncmp(path, root, sizeof(root)-1)) return NULL;
    name = strrchr(path, '/') + 1; length = strlen(name);
    return length > 4 && !strcmp(name + length - 4, suffix) ? name : NULL;
}
int sh_package_audio_path_kind(const char *path)
{ return pa_filename(path, ".bnk") ? 1 : pa_filename(path, ".wem") ? 2 : 0; }

static int pa_identity(const sh_package_compilation *compiled,
    const sh_compiled_resource *resource, sh_package_file_identity *out,
    char *error, size_t capacity)
{
    return sh_package_compilation_identity(compiled, resource, out, error, capacity);
}

static int pa_contributed(const sh_compiled_resource *resource)
{
    return !resource->restored_original &&
        (sh_package_owners_count(&resource->owners) != 0 || resource->generated);
}

static int pa_media_compare(const void *left, const void *right)
{
    const sh_package_audio_media *a = left, *b = right;
    if (a->id != b->id) return (a->id > b->id) - (a->id < b->id);
    return strcmp(a->path, b->path);
}

static int pa_compare(const void *left, const void *right)
{
    const sh_package_audio_reference *a = left, *b = right;
    if (a->event != b->event) return (a->event > b->event) - (a->event < b->event);
    return strcmp(a->path, b->path);
}

void sh_package_audio_close(sh_package_audio *audio)
{
    if (!audio) return;
    for (size_t i = 0; i < audio->count; i++) {
        free(audio->banks[i].path); sh_audio_bank_free(&audio->banks[i].bank);
    }
    for (size_t i = 0; i < audio->media_count; i++) free((void *)audio->media[i].path);
    free(audio->banks); free(audio->events); free(audio->inventory);
    free(audio->media); free(audio);
}

sh_package_audio *sh_package_audio_open(const sh_package_compilation *compiled,
    sh_package_audio_report *report, char *error, size_t capacity)
{
    sh_package_audio *audio = NULL;
    sh_package_audio_report detail = {0};
    size_t count = 0, media = 0, cursor = 0;
    char fallback[1024];
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (report) memset(report, 0, sizeof(*report));
    if (!compiled || (compiled->resource_count && !compiled->resources)) {
        snprintf(error, capacity, "audio dependency provider is unavailable"); return NULL;
    }
    for (size_t i = 0; i < compiled->resource_count; i++) {
        if (pa_filename(compiled->resources[i].engine_path, ".bnk")) count++;
        if (pa_filename(compiled->resources[i].engine_path, ".wem")) media++;
    }
    audio = calloc(1, sizeof(*audio));
    if (!audio || count > SIZE_MAX / sizeof(*audio->banks) ||
        media > SIZE_MAX / sizeof(*audio->media)) goto memory;
    if (count && !(audio->banks = calloc(count, sizeof(*audio->banks)))) goto memory;
    if (media && !(audio->media = calloc(media, sizeof(*audio->media)))) goto memory;
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = compiled->resources + i;
        const char *filename = pa_filename(resource->engine_path, ".bnk");
        sh_audio_bank bank = {0};
        sh_package_file_identity file_identity = {0};
        int parsed;
        char diagnostic[512];
        if (!filename) {
            sh_package_audio_media *entry;
            uint32_t id;
            filename = pa_filename(resource->engine_path, ".wem");
            if (!filename) continue;
            if (!sh_audio_media_identity(filename, &id)) { detail.unnamed_media++; continue; }
            if (!pa_identity(compiled, resource, &file_identity, error, capacity)) goto failed;
            entry = audio->media + audio->media_count;
            entry->path = _strdup(resource->engine_path);
            if (!entry->path) goto memory;
            entry->filename = strrchr(entry->path, '/') + 1;
            entry->id = id; entry->identity = file_identity;
            entry->contributed = pa_contributed(resource);
            audio->media_count++; continue;
        }
        if (!pa_identity(compiled, resource, &file_identity, error, capacity)) goto failed;
        if (resource->body) {
            parsed = sh_audio_bank_read_source((sh_audio_bank_source){(void *)resource,
                resource->body_length, pa_memory}, &bank, diagnostic, sizeof(diagnostic));
        } else {
            FILE *stream;
            uint64_t length;
            if (resource->cache_file)
                stream = sh_package_file_open(resource->cache_file, &length, error, capacity);
            else {
                sh_package_source_file file = compiled->sources->files[resource->source];
                if (resource->cache_path) file.absolute = resource->cache_path;
                stream = sh_package_source_open(&file, error, capacity);
            }
            if (!stream) goto failed;
            parsed = sh_audio_bank_read(stream, &bank, diagnostic, sizeof(diagnostic));
            fclose(stream);
        }
        if (parsed < 0) {
            snprintf(error, capacity, "%s: %s", resource->engine_path, diagnostic); goto failed;
        }
        if (parsed && !sh_audio_bank_names_identity(filename, bank.id)) {
            snprintf(diagnostic, sizeof(diagnostic), "filename does not match native bank identity"); parsed = 0;
        }
        if (!parsed) {
            detail.invalid++;
            if (!detail.first_gap[0]) snprintf(detail.first_gap, sizeof(detail.first_gap),
                "%s: %s", resource->engine_path, diagnostic);
            sh_audio_bank_free(&bank); continue;
        }
        if (bank.event_count > SIZE_MAX - audio->event_count) { sh_audio_bank_free(&bank); goto memory; }
        pa_bank *entry = audio->banks + audio->count;
        entry->path = _strdup(resource->engine_path);
        if (!entry->path) { sh_audio_bank_free(&bank); goto memory; }
        entry->filename = strrchr(entry->path, '/') + 1;
        entry->bank = bank; entry->identity = file_identity;
        entry->contributed = pa_contributed(resource);
        audio->count++; audio->event_count += bank.event_count;
    }
    if (audio->event_count > SIZE_MAX / sizeof(*audio->events)) goto memory;
    if (audio->count > SIZE_MAX / sizeof(*audio->inventory)) goto memory;
    if (audio->count && !(audio->inventory = calloc(audio->count,sizeof(*audio->inventory)))) goto memory;
    if (audio->event_count && !(audio->events = malloc(audio->event_count * sizeof(*audio->events)))) goto memory;
    for (size_t i = 0; i < audio->count; i++) {
        pa_bank *entry = audio->banks + i;
        audio->inventory[i] = (sh_package_audio_bank){entry->path,entry->filename,&entry->bank,entry->identity,entry->contributed};
        for (size_t j = 0; j < entry->bank.event_count; j++)
            audio->events[cursor++] = (sh_package_audio_reference){entry->bank.events[j],
                entry->bank.id, entry->bank.language, entry->path, entry->filename, &entry->bank};
    }
    if (audio->event_count > 1) qsort(audio->events, audio->event_count, sizeof(*audio->events), pa_compare);
    if (audio->media_count > 1)
        qsort(audio->media, audio->media_count, sizeof(*audio->media), pa_media_compare);
    detail.banks = audio->count; detail.media = audio->media_count;
    if (report) *report = detail;
    return audio;
memory:
    snprintf(error, capacity, "audio dependency index allocation failed");
failed:
    sh_package_audio_close(audio); return NULL;
}

size_t sh_package_audio_banks(const sh_package_audio *audio,
    const sh_package_audio_bank **banks)
{
    if (banks) *banks = audio ? audio->inventory : NULL;
    return audio && banks ? audio->count : 0;
}

size_t sh_package_audio_media_files(const sh_package_audio *audio,
    const sh_package_audio_media **media)
{
    if (media) *media = audio ? audio->media : NULL;
    return audio && media ? audio->media_count : 0;
}

size_t sh_package_audio_find_media(const sh_package_audio *audio, uint32_t id,
    const sh_package_audio_media **media)
{
    size_t low = 0, high = audio ? audio->media_count : 0, end;
    if (media) *media = NULL;
    if (!audio || !media || !id) return 0;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (audio->media[middle].id < id) low = middle + 1;
        else high = middle;
    }
    end = low;
    while (end < audio->media_count && audio->media[end].id == id) end++;
    if (end != low) *media = audio->media + low;
    return end - low;
}

size_t sh_package_audio_find(const sh_package_audio *audio, uint32_t event,
    const sh_package_audio_reference **references)
{
    size_t low = 0, high = audio ? audio->event_count : 0, end;
    if (references) *references = NULL;
    if (!audio || !references || !event) return 0;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (audio->events[middle].event < event) low = middle + 1;
        else high = middle;
    }
    end = low;
    while (end < audio->event_count && audio->events[end].event == event) end++;
    if (end != low) *references = audio->events + low;
    return end - low;
}

typedef struct pa_effective_bank {
    const char *path;
    const sh_audio_bank *metadata;
    int original;
    const sh_audio_identity_span *spans;
    size_t span_count;
} pa_effective_bank;
typedef struct pa_scope_ref { uint32_t id; unsigned domain, definition, required; size_t bank; } pa_scope_ref;
struct sh_package_audio_scope {
    sh_package_audio *candidate;
    sh_audio_originals *originals;
    pa_effective_bank *banks;
    const sh_audio_bank **metadata;
    size_t count;
    pa_scope_ref *refs;
    size_t ref_count;
    int refs_ready;
};
void sh_package_audio_scope_close(sh_package_audio_scope *scope)
{
    if (!scope) return;
    sh_package_audio_close(scope->candidate); free(scope->banks); free(scope->metadata); free(scope->refs); free(scope);
}
static int pa_scope_bank(sh_package_audio_scope *scope, const char *filename,
    const sh_audio_original_bank *originals, size_t original_count, char *error, size_t capacity)
{
    char *paths[2] = {0}; pa_effective_bank selected = {0};
    if (!sh_audio_originals_paths(scope->originals, filename, paths, paths + 1)) return 0;
    for (size_t p = 0; p < 2 && !selected.path; p++) {
        for (size_t i = 0; i < scope->candidate->count; i++) {
            pa_bank *bank = scope->candidate->banks + i;
            if (!strcmp(bank->path, paths[p])) { selected = (pa_effective_bank){bank->path, &bank->bank, 0}; break; }
        }
        for (size_t i = 0; i < original_count && !selected.path; i++)
            if (!strcmp(originals[i].path, paths[p]))
                selected = (pa_effective_bank){originals[i].path, &originals[i].metadata, 1};
    }
    free(paths[0]); free(paths[1]);
    if (!selected.path) return 1; /* another locale is retained in its delivery */
    for (size_t i = 0; i < scope->count; i++) if (scope->banks[i].metadata->id == selected.metadata->id) {
        if (!strcmp(scope->banks[i].path, selected.path)) return 1;
        if (error && capacity) snprintf(error, capacity, "audio bank %u has multiple native lookup names", selected.metadata->id);
        return 0;
    }
    scope->banks[scope->count] = selected; scope->metadata[scope->count++] = selected.metadata; return 1;
}
sh_package_audio_scope *sh_package_audio_scope_open(const sh_package_compilation *compiled,
    sh_audio_originals *originals, char *error, size_t capacity)
{
    sh_package_audio_scope *scope = calloc(1, sizeof(*scope));
    const sh_audio_original_bank *banks = NULL; size_t count = 0, total;
    sh_package_audio_report report = {0};
    if (error && capacity) *error = 0;
    if (!scope) goto failed;
    scope->originals = originals;
    scope->candidate = sh_package_audio_open(compiled, &report, error, capacity);
    if (!scope->candidate) goto failed;
    if (report.invalid) { if (error && capacity) snprintf(error, capacity, "%s", report.first_gap); goto failed; }
    if (sh_audio_originals_banks(originals, &banks, &count, error, capacity) != 1) goto failed;
    if (scope->candidate->count > SIZE_MAX - count) goto failed;
    total = scope->candidate->count + count;
    if (total > SIZE_MAX / sizeof(*scope->banks) || total > SIZE_MAX / sizeof(*scope->metadata)) goto failed;
    scope->banks = calloc(total ? total : 1, sizeof(*scope->banks));
    scope->metadata = calloc(total ? total : 1, sizeof(*scope->metadata));
    if (!scope->banks || !scope->metadata) goto failed;
    for (size_t i = 0; i < count; i++)
        if (!pa_scope_bank(scope, banks[i].filename, banks, count, error, capacity)) goto failed;
    for (size_t i = 0; i < scope->candidate->count; i++)
        if (!pa_scope_bank(scope, scope->candidate->banks[i].filename, banks, count, error, capacity)) goto failed;
    return scope;
failed:
    if (error && capacity && !*error) snprintf(error, capacity, "cannot capture audio source scope");
    sh_package_audio_scope_close(scope); return NULL;
}
int sh_package_audio_scope_media(sh_package_audio_scope *scope, uint32_t id,
    sh_package_audio_path_visit visit, void *context, char *error, size_t capacity)
{
    char name[32], *paths[2] = {0}; int result = -1;
    if (error && capacity) *error = 0;
    if (!scope || !id || !visit) goto done;
    snprintf(name, sizeof(name), "%u.wem", id);
    if (!sh_audio_originals_paths(scope->originals, name, paths, paths + 1)) goto done;
    const sh_package_audio_media *media = NULL;
    size_t media_count = sh_package_audio_find_media(scope->candidate, id, &media);
    for (size_t p = 0; p < 2; p++) {
        for (size_t i = 0; i < media_count; i++)
            if (!strcmp(media[i].path, paths[p])) {
                result = visit(context, media[i].path, 0) ? 1 : -1; goto done;
            }
        int present = sh_audio_originals_read(scope->originals, paths[p], 0, NULL, 0, NULL, error, capacity);
        if (present < 0) goto done;
        if (present) { result = visit(context, paths[p], 1) ? 1 : -1; goto done; }
    }
    result = 0;
done:
    if (result < 0 && error && capacity && !*error) snprintf(error, capacity, "cannot retain audio media %u dependency", id);
    free(paths[0]); free(paths[1]); return result;
}
static int pa_candidate(void *context, const char *path, int original)
{ (void)path; *(int *)context = !original; return 1; }
static int pa_ref_order(const void *left, const void *right)
{
    const pa_scope_ref *a = left, *b = right;
    if (a->domain != b->domain) return a->domain < b->domain ? -1 : 1;
    return a->id < b->id ? -1 : a->id != b->id;
}
static int pa_scope_links(sh_package_audio_scope *scope, unsigned char *selected,int reverse)
{
    if (!scope->refs_ready) {
        size_t count = 0;
        for (size_t b = 0; b < scope->count; b++) {
            if (scope->banks[b].span_count > SIZE_MAX - count) return 0;
            count += scope->banks[b].span_count;
        }
        if (count > SIZE_MAX / sizeof(*scope->refs)) return 0;
        pa_scope_ref *refs = calloc(count ? count : 1, sizeof(*refs)); if (!refs) return 0;
        size_t at = 0;
        for (size_t b = 0; b < scope->count; b++) for (size_t i = 0; i < scope->banks[b].span_count; i++) {
            const sh_audio_identity_span *span = scope->banks[b].spans + i;
            if (!span->value || span->domain > SH_AUDIO_ID_BANK || (span->flags & (SH_AUDIO_ID_SHARED | SH_AUDIO_ID_LITERAL))) continue;
            unsigned definition=!!(span->flags & SH_AUDIO_ID_DEFINITION);
            refs[at++] = (pa_scope_ref){span->value, span->domain, definition,
                !definition && !(span->flags & SH_AUDIO_ID_OPTIONAL) && span->domain<SH_AUDIO_ID_MEDIA,b};
        }
        qsort(refs, at, sizeof(*refs), pa_ref_order);
        free(scope->refs); scope->refs = refs; scope->ref_count = at; scope->refs_ready = 1;
    }
    /* A changed definition can be used by events in a different bank. Include
     * every such consumer (and duplicate definition recipe) before forward
     * closure. Exact event comparison later removes conservative nominations. */
    for (int changed = 1; changed;) {
        changed = 0;
        for (size_t first = 0; first < scope->ref_count;) {
            size_t end = first + 1,provider=SIZE_MAX; int affected = 0,required=0,provided=0;
            while (end < scope->ref_count && !pa_ref_order(scope->refs + first, scope->refs + end)) end++;
            for (size_t i = first; i < end; i++) {
                if(scope->refs[i].definition && scope->refs[i].bank<provider)provider=scope->refs[i].bank;
                if(selected[scope->refs[i].bank]) {
                    provided|=scope->refs[i].definition;
                    affected|=reverse && scope->refs[i].definition;required|=scope->refs[i].required;
                }
            }
            /* A satisfied forward lookup does not load every dormant duplicate
             * definition in the game. Otherwise choose the first scoped native
             * provider, as the existing resident-media closure does. */
            if(required && !provided && provider!=SIZE_MAX){selected[provider]=1;changed=1;}
            for (size_t i = first; i < end; i++) if (affected &&
                !selected[scope->refs[i].bank]) {
                selected[scope->refs[i].bank] = 1; changed = 1;
            }
            first = end;
        }
    }
    return 1;
}
static int pa_scope_select(sh_package_audio_scope *scope, const uint32_t *events, size_t count, int inventory,
    const uint32_t *roots, size_t root_count,
    sh_package_audio_path_visit visit, void *context, char *error, size_t capacity)
{
    unsigned char *selected = NULL; int ok = 0;
    if (error && capacity) *error = 0;
    if (!scope || (count && !events) || (root_count && !roots) || !visit) goto done;
    for (size_t i = 0; i < count; i++) if (!events[i]) goto done;
    selected = calloc(scope->count ? scope->count : 1, 1); if (!selected) goto done;
    for(size_t r=0;r<root_count;r++) {
        size_t b=0;while(b<scope->count && scope->banks[b].metadata->id!=roots[r])b++;
        if(!roots[r] || b==scope->count) {
            if(error && capacity)snprintf(error,capacity,"audio bank %u is absent from its source scope",roots[r]);
            goto done;
        }
        selected[b]=1;
    }
    if (inventory) for (size_t i = 0; i < scope->count; i++) {
        const pa_effective_bank *bank = scope->banks + i;
        selected[i] = !bank->original;
        for (size_t m = 0; !selected[i] && m < bank->metadata->media_count; m++) {
            uint32_t id = bank->metadata->media[m].id;
            const sh_package_audio_media *candidate = NULL;
            if (!sh_package_audio_find_media(scope->candidate, id, &candidate)) continue;
            int authored = 0;
            int found = sh_package_audio_scope_media(scope, id, pa_candidate, &authored, error, capacity);
            if (found < 0) goto done;
            selected[i] = found && authored;
        }
    }
    /* Light bank/media closure and typed object closure can each add roots the
     * other needs. Iterate both to a fixed point; optional object lookups never
     * pull in a definition merely because it exists in an unrelated bank. */
    for(;;) {
        size_t before=0,after=0;for(size_t i=0;i<scope->count;i++)before+=!!selected[i];
        if(!pa_scope_links(scope,selected,inventory) ||
            !sh_audio_bank_closure(scope->metadata, scope->count, events, count, 1, selected, NULL, error, capacity))goto done;
        for(size_t i=0;i<scope->count;i++)after+=!!selected[i];if(before==after)break;
    }
    for (size_t i = 0; i < scope->count; i++) if (selected[i]) {
        const pa_effective_bank *bank = scope->banks + i;
        if (!visit(context, bank->path, bank->original)) goto done;
        for (size_t m = 0; m < bank->metadata->media_count; m++)
            if (bank->metadata->media[m].stream && sh_package_audio_scope_media(scope, bank->metadata->media[m].id,
                visit, context, error, capacity) != 1) {
                if (error && capacity && !*error) snprintf(error, capacity,
                    "audio media %u is absent from its source scope", bank->metadata->media[m].id);
                goto done;
            }
    }
    ok = 1;
done:
    free(selected);
    if (!ok && error && capacity && !*error) snprintf(error, capacity, "cannot resolve audio source dependencies");
    return ok;
}
int sh_package_audio_scope_events(sh_package_audio_scope *scope, const uint32_t *events, size_t count,
    sh_package_audio_path_visit visit, void *context, char *error, size_t capacity)
{ return pa_scope_select(scope, events, count, 0, NULL, 0, visit, context, error, capacity); }
int sh_package_audio_scope_inventory(sh_package_audio_scope *scope, const uint32_t *events, size_t count,
    sh_package_audio_path_visit visit, void *context, char *error, size_t capacity)
{ return pa_scope_select(scope, events, count, 1, NULL, 0, visit, context, error, capacity); }
int sh_package_audio_scope_roots(sh_package_audio_scope *scope,const uint32_t *banks,size_t count,
    sh_package_audio_path_visit visit,void *context,char *error,size_t capacity)
{ return pa_scope_select(scope,NULL,0,0,banks,count,visit,context,error,capacity); }
int sh_package_audio_scope_banks(sh_package_audio_scope *scope,
    sh_package_audio_path_visit visit, void *context)
{
    if (!scope || !visit) return 0;
    for (size_t i = 0; i < scope->count; i++)
        if (!visit(context, scope->banks[i].path, scope->banks[i].original)) return 0;
    return 1;
}
int sh_package_audio_scope_event(sh_package_audio_scope *scope, uint32_t event,
    sh_package_audio_path_visit visit, void *context, char *error, size_t capacity)
{ return sh_package_audio_scope_events(scope, &event, 1, visit, context, error, capacity); }
int sh_package_audio_scope_implements(const sh_package_audio_scope *scope, uint32_t event)
{
    if (!scope || !event) return 0;
    for (size_t i = 0; i < scope->count; i++)
        if (sh_audio_bank_has_event(scope->metadata[i], event)) return 1;
    return 0;
}
int sh_package_audio_scope_refine(sh_package_audio_scope *scope, const char *path, const sh_audio_bank *metadata)
{
    if (!scope || !path || !metadata) return 0;
    for (size_t i = 0; i < scope->count; i++) if (!strcmp(scope->banks[i].path, path)) {
        if (metadata->id != scope->banks[i].metadata->id || metadata->language != scope->banks[i].metadata->language) return 0;
        scope->banks[i].metadata = scope->metadata[i] = metadata; return 1;
    }
    return 0;
}
int sh_package_audio_scope_references(sh_package_audio_scope *scope, const char *path,
    const sh_audio_identity_span *spans, size_t count)
{
    if (!scope || !path || (count && !spans)) return 0;
    for (size_t i = 0; i < scope->count; i++) if (!strcmp(scope->banks[i].path, path)) {
        scope->banks[i].spans = spans; scope->banks[i].span_count = count; scope->refs_ready = 0; return 1;
    }
    return 0;
}
