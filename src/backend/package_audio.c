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

static int pa_identity(const sh_package_compilation *compiled,
    const sh_compiled_resource *resource, sh_package_file_identity *out,
    char *error, size_t capacity)
{
    memset(out, 0, sizeof(*out));
    if (resource->body || resource->cache_file) {
        if (resource->body ? sh_package_bytes_identity(resource->body, resource->body_length, out)
                           : sh_package_file_get_identity(resource->cache_file, out)) return 1;
        snprintf(error, capacity, "audio dependency index allocation failed"); return 0;
    }
    if (!compiled->sources || !compiled->sources->files ||
        resource->source >= compiled->sources->file_count) {
        snprintf(error, capacity, "%s: audio resource has no captured source", resource->engine_path);
        return 0;
    }
    out->length = compiled->sources->files[resource->source].length;
    memcpy(out->digest, compiled->sources->files[resource->source].digest, sizeof(out->digest));
    return 1;
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
