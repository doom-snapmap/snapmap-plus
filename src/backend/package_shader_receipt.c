#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "package_shader_receipt.h"

/* Parameter and flag source files are small; an oversized native source still
 * loads normally but cannot acquire a receipt through this bounded observer. */
#define SR_SOURCE_LIMIT (16u * 1024u * 1024u)
typedef struct sr_input {
    struct sr_input *next;
    char *path;
    unsigned char *body, *seen;
    size_t length, covered;
    int failed;
} sr_input;
typedef struct sr_entry {
    struct sr_entry *next;
    uintptr_t resource;
    uint64_t epoch;
    char *family, *name;
    unsigned char *body;
    size_t length, header_length;
    int retained;
    unsigned char header[0x90];
    unsigned char *metadata; size_t metadata_length;
    sr_input *inputs;
    uint64_t ticket;
    uint64_t preparation_ticket;
    int cook, renderer;
} sr_entry;
typedef struct sr_pending {
    sr_entry *entry;
    uint64_t epoch;
    char *path;
    unsigned char *body, *seen;
    size_t length, covered, header_length, stream_length;
    int failed, captured, retained;
    sr_input *inputs;
    unsigned char *metadata; size_t metadata_length, input_bytes, input_count;
    uint64_t ticket;
    int compiled;
    int cook;
} sr_pending;
static SRWLOCK sr_lock = SRWLOCK_INIT;
static sr_entry *sr_entries;
static __declspec(thread) sh_package_shader_receipt_frame *sr_current;
static volatile LONG64 sr_ticket_counter;
static __declspec(thread) unsigned sr_program_depth;
static __declspec(thread) uint64_t sr_program_ticket;
static void sr_inputs_free(sr_input *inputs)
{
    while (inputs) { sr_input *next = inputs->next;
        free(inputs->path); free(inputs->body); free(inputs->seen); free(inputs); inputs = next; }
}

static char sr_fold(char value)
{
    if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
    return value == '\\' ? '/' : value;
}
const char *sh_package_shader_input_path(const char *path)
{
    if (path) while (*path == '/' || *path == '\\') path++;
    return path;
}
static int sr_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    do { if (sr_fold(*a) != sr_fold(*b)) return 0; } while (*a++ && *b++);
    return 1;
}
static char *sr_copy(const char *text)
{
    size_t n = text ? strnlen(text, 4096) : 0;
    if (!n || n == 4096) return NULL;
    char *out = malloc(n + 1);
    if (out) for (size_t i = 0; i <= n; i++) out[i] = sr_fold(text[i]);
    return out;
}
static sr_entry *sr_find(uintptr_t resource)
{
    for (sr_entry *entry = sr_entries; entry; entry = entry->next)
        if (!entry->cook && entry->resource == resource) return entry;
    return NULL;
}
static sr_entry *sr_find_cook(int renderer, const char *name)
{
    for (sr_entry *entry = sr_entries; entry; entry = entry->next)
        if (entry->cook && entry->renderer == renderer && sr_equal(entry->name, name)) return entry;
    return NULL;
}
static void sr_clear(sr_entry *entry)
{
    entry->epoch++; free(entry->body); entry->body = NULL;
    entry->length = entry->header_length = 0;
    free(entry->metadata); entry->metadata = NULL; entry->metadata_length = 0;
    sr_inputs_free(entry->inputs); entry->inputs = NULL; entry->ticket = 0;
}
void sh_package_shader_receipt_invalidate(uintptr_t resource)
{
    if (!resource) return;
    AcquireSRWLockExclusive(&sr_lock);
    sr_entry *entry = sr_find(resource); if (entry) sr_clear(entry);
    ReleaseSRWLockExclusive(&sr_lock);
}
void sh_package_shader_receipt_begin(sh_package_shader_receipt_frame *frame,
    uintptr_t resource, const char *family, const char *name)
{
    memset(frame, 0, sizeof(*frame)); frame->parent = sr_current;
    frame->resource = resource; sr_current = frame;
    if (frame->parent && frame->parent->resource == resource && resource) {
        frame->pending = frame->parent->pending; return;
    }
    frame->owner = 1;
    int program = sr_program_depth && sr_equal(family, "renderprog");
    int fixed = program || sr_equal(family, "renderparm") || sr_equal(family, "renderprogflag");
    sr_pending *pending = fixed && resource ? calloc(1, sizeof(*pending)) : NULL;
    char *canonical_family = pending ? sr_copy(family) : NULL;
    char *canonical_name = canonical_family ? sr_copy(name) : NULL;
    if (canonical_name) {
        size_t n = strlen(canonical_family) + strlen(canonical_name) + 24;
        pending->path = malloc(n);
        if (pending->path) {
            if (strstr(canonical_name, ".decl")) snprintf(pending->path, n, "%s", canonical_name);
            else snprintf(pending->path, n, "generated/decls/%s/%s.decl", canonical_family, canonical_name);
        }
    }
    AcquireSRWLockExclusive(&sr_lock);
    sr_entry *entry = resource ? sr_find(resource) : NULL;
    if (entry) sr_clear(entry);
    else if (pending && pending->path) {
        entry = calloc(1, sizeof(*entry));
        if (entry) { entry->resource = resource; entry->epoch = 1; entry->next = sr_entries; sr_entries = entry; }
    }
    if (entry && pending && pending->path) {
        free(entry->family); free(entry->name);
        entry->family = canonical_family; canonical_family = NULL;
        entry->name = canonical_name; canonical_name = NULL;
        pending->header_length = program ? 0x78 : sr_equal(entry->family, "renderparm") ? 0x90 : 0x60;
        pending->ticket = program ? sr_program_ticket : 0;
        pending->entry = entry; pending->epoch = entry->epoch; frame->pending = pending; pending = NULL;
    }
    ReleaseSRWLockExclusive(&sr_lock);
    free(canonical_family); free(canonical_name);
    if (pending) { free(pending->path); free(pending); }
}
void sh_package_shader_cook_begin(sh_package_shader_receipt_frame *frame,
    uintptr_t token, int renderer, const char *name)
{
    memset(frame, 0, sizeof(*frame)); frame->parent = sr_current;
    frame->resource = token; frame->owner = 1; sr_current = frame;
    if (!token || !sr_program_ticket || renderer < 0 || renderer > 1) return;
    sr_pending *pending = calloc(1, sizeof(*pending));
    char *canonical = pending ? sr_copy(name) : NULL;
    if (canonical) {
        size_t n = strlen(canonical) + 32; pending->path = malloc(n);
        if (pending->path) snprintf(pending->path, n, strstr(canonical, ".decl") ? "%s" :
            "generated/decls/renderprog/%s.decl", canonical);
    }
    AcquireSRWLockExclusive(&sr_lock);
    sr_entry *entry = canonical ? sr_find_cook(renderer, canonical) : NULL;
    if (entry) sr_clear(entry);
    else if (pending && pending->path) {
        entry = calloc(1, sizeof(*entry));
        if (entry) {
            entry->cook = 1; entry->renderer = renderer; entry->epoch = 1;
            entry->name = canonical; canonical = NULL;
            entry->next = sr_entries; sr_entries = entry;
        }
    }
    if (entry && pending && pending->path) {
        pending->entry = entry; pending->epoch = entry->epoch;
        pending->cook = 1; pending->ticket = sr_program_ticket;
        frame->pending = pending; pending = NULL;
    }
    ReleaseSRWLockExclusive(&sr_lock); free(canonical);
    if (pending) { free(pending->path); free(pending); }
}
int sh_package_shader_receipt_wants(const char *path)
{
    path = sh_package_shader_input_path(path);
    if (!path || !*path) return 0;
    for (sh_package_shader_receipt_frame *frame = sr_current; frame; frame = frame->parent) {
        sr_pending *pending = frame->pending;
        if (pending && !pending->failed && (frame == sr_current || pending->cook) &&
            (pending->ticket || sr_equal(pending->path, path))) return 1;
    }
    return 0;
}
static void sr_input_bytes(sr_pending *pending, const char *path, size_t total,
    size_t offset, const void *body, size_t length)
{
    sr_input *input = pending->inputs;
    while (input && !sr_equal(input->path, path)) input = input->next;
    if (!input) {
        if (pending->input_count >= 4096 || total > SR_SOURCE_LIMIT ||
            pending->input_bytes > 128u * 1024u * 1024u - total) { pending->failed = 1; return; }
        input = calloc(1, sizeof(*input));
        if (input) { input->path = sr_copy(path); input->body = malloc(total ? total : 1); input->seen = calloc(total ? total : 1, 1); }
        if (!input || !input->path || !input->body || !input->seen) {
            if (input) { free(input->path); free(input->body); free(input->seen); free(input); }
            pending->failed = 1; return;
        }
        input->length = total; input->next = pending->inputs; pending->inputs = input;
        pending->input_count++; pending->input_bytes += total;
    }
    if (input->failed) return;
    if ((!body && length) || total != input->length || offset > total || length > total - offset) { input->failed = 1; return; }
    for (size_t i = 0; i < length; i++) {
        size_t at = offset + i; unsigned char value = ((const unsigned char *)body)[i];
        if (input->seen[at] && input->body[at] != value) { input->failed = 1; return; }
        if (!input->seen[at]) input->covered++;
        input->seen[at] = 1; input->body[at] = value;
    }
}
static void sr_bytes(sr_pending *pending, const char *path, size_t total,
    size_t offset, const void *body, size_t length)
{
    if (!pending || pending->failed || !path || !*path ||
        (!pending->ticket && !sr_equal(pending->path, path))) return;
    if (pending->ticket && !sr_equal(pending->path, path)) {
        sr_input_bytes(pending, path, total, offset, body, length); return;
    }
    if (!body || !total || total > SR_SOURCE_LIMIT || offset > total || length > total - offset) {
        pending->failed = 1; return;
    }
    if (pending->retained) {
        if (total < pending->length || (pending->stream_length && total != pending->stream_length)) {
            pending->failed = 1; return;
        }
        pending->stream_length = total;
        for (size_t i = 0; i < length; i++) {
            size_t at = offset + i; unsigned char value = ((const unsigned char *)body)[i];
            if ((at < pending->length && pending->body[at] != value) ||
                (at >= pending->length && value != ' ' && value != '\r' && value != '\n')) {
                pending->failed = 1; return;
            }
        }
        return;
    }
    if (!pending->captured) {
        pending->body = malloc(total); pending->seen = calloc(total, 1); pending->length = total;
        if (!pending->body || !pending->seen) { pending->failed = 1; return; }
        pending->captured = 1;
    } else if (pending->length != total) { pending->failed = 1; return; }
    for (size_t i = 0; i < length; i++) {
        size_t at = offset + i;
        if (pending->seen[at] && pending->body[at] != ((const unsigned char *)body)[i]) {
            pending->failed = 1; return;
        }
        if (!pending->seen[at]) pending->covered++;
        pending->seen[at] = 1; pending->body[at] = ((const unsigned char *)body)[i];
    }
}
void sh_package_shader_receipt_bytes(const char *path, size_t total,
    size_t offset, const void *body, size_t length)
{
    path = sh_package_shader_input_path(path);
    sr_pending *last = NULL;
    for (sh_package_shader_receipt_frame *frame = sr_current; frame; frame = frame->parent) {
        sr_pending *pending = frame->pending;
        if (pending && pending != last && (frame == sr_current || pending->cook))
            sr_bytes(pending, path, total, offset, body, length);
        last = pending;
    }
}
void sh_package_shader_receipt_retained(const void *body, size_t length)
{
    sr_pending *pending = sr_current ? sr_current->pending : NULL;
    if (!pending || pending->failed) return;
    if (!body || !length || length > SR_SOURCE_LIMIT) { pending->failed = 1; return; }
    if (pending->retained) {
        if (pending->length != length || memcmp(pending->body, body, length)) pending->failed = 1;
        return;
    }
    if (pending->captured) {
        if (pending->length < length) { pending->failed = 1; return; }
        for (size_t i = 0; i < pending->length; i++) if (pending->seen[i]) {
            unsigned char value = pending->body[i];
            if ((i < length && value != ((const unsigned char *)body)[i]) ||
                (i >= length && value != ' ' && value != '\r' && value != '\n')) {
                pending->failed = 1; return;
            }
        }
        pending->stream_length = pending->length;
    }
    unsigned char *copy = malloc(length);
    if (!copy) { pending->failed = 1; return; }
    memcpy(copy, body, length); free(pending->body); free(pending->seen);
    pending->body = copy; pending->seen = NULL;
    pending->length = pending->covered = length; pending->captured = pending->retained = 1;
}
void sh_package_shader_receipt_text_begin(uintptr_t resource)
{
    if (!sr_current || sr_current->resource != resource || !sr_current->pending)
        sh_package_shader_receipt_invalidate(resource);
}
void sh_package_shader_receipt_text(uintptr_t resource, const void *body, size_t length)
{
    if (sr_current && sr_current->resource == resource)
        sh_package_shader_receipt_retained(body, length);
}
void sh_package_shader_receipt_program_scope(int enter)
{
    if (enter) {
        if (!sr_program_depth++) sr_program_ticket = (uint64_t)InterlockedIncrement64(&sr_ticket_counter);
    } else if (sr_program_depth && !--sr_program_depth) sr_program_ticket = 0;
}
void sh_package_shader_receipt_revoke_scope(void)
{
    if (!sr_program_ticket) return;
    AcquireSRWLockExclusive(&sr_lock);
    for (sr_entry *entry = sr_entries; entry; entry = entry->next)
        if (entry->preparation_ticket == sr_program_ticket) sr_clear(entry);
    ReleaseSRWLockExclusive(&sr_lock);
}
uintptr_t sh_package_shader_receipt_program_current(void)
{
    sr_pending *pending = sr_current ? sr_current->pending : NULL;
    return pending && pending->ticket && !pending->failed ? sr_current->resource : 0;
}
void sh_package_shader_receipt_program_compiled(uintptr_t resource)
{
    if (resource && resource == sh_package_shader_receipt_program_current())
        ((sr_pending *)sr_current->pending)->compiled = 1;
}
int sh_package_shader_receipt_program_wrote(uintptr_t resource, const void *body, size_t length)
{
    sr_pending *pending = sr_current ? sr_current->pending : NULL;
    if (!resource || resource != sh_package_shader_receipt_program_current() || !pending->compiled ||
        !body || !length || length > 64u * 1024u * 1024u) return 0;
    unsigned char *copy = malloc(length);
    if (!copy) { pending->failed = 1; return 0; }
    memcpy(copy, body, length); free(pending->metadata); pending->metadata = copy;
    pending->metadata_length = length; return 1;
}
void sh_package_shader_receipt_end(sh_package_shader_receipt_frame *frame,
    int parsed, sh_package_shader_receipt_read read, void *context)
{
    sr_pending *pending = frame ? frame->pending : NULL;
    if (!frame || sr_current != frame) return;
    sr_current = frame->parent;
    if (!pending) return;
    if (!parsed) pending->failed = 1;
    if (!frame->owner) return;
    unsigned char header[0x90], after[0x90]; uintptr_t error = 0;
    size_t n = pending->header_length;
    int ok = !pending->failed && pending->captured && pending->covered == pending->length;
    if (pending->cook) {
        ok = ok && pending->compiled && pending->metadata && pending->ticket;
        for (sr_input *input = pending->inputs; input; input = input->next)
            if (input->failed || input->covered != input->length) ok = 0;
    }
    else ok = ok && read && read(context, frame->resource, header, n);
    if (ok && !pending->cook) {
        memcpy(&error, header + 0x18, sizeof(error));
        ok = !error && !(header[0x2c] & 7) && read(context, frame->resource, after, n) && !memcmp(header, after, n);
    }
    AcquireSRWLockExclusive(&sr_lock);
    if (ok && pending->entry->epoch == pending->epoch) {
        pending->entry->body = pending->body; pending->body = NULL;
        pending->entry->length = pending->length; pending->entry->header_length = n;
        pending->entry->retained = pending->retained;
        pending->entry->preparation_ticket = sr_program_ticket;
        if (n) memcpy(pending->entry->header, header, n);
        if (pending->compiled && pending->metadata) {
            pending->entry->metadata = pending->metadata; pending->metadata = NULL;
            pending->entry->metadata_length = pending->metadata_length;
            pending->entry->inputs = pending->inputs; pending->inputs = NULL;
            pending->entry->ticket = pending->ticket;
        }
    }
    ReleaseSRWLockExclusive(&sr_lock);
    free(pending->path); free(pending->body); free(pending->seen); free(pending->metadata);
    sr_inputs_free(pending->inputs); free(pending);
    frame->pending = NULL;
}
void sh_package_shader_cook_end(sh_package_shader_receipt_frame *frame, int parsed)
{ sh_package_shader_receipt_end(frame, parsed, NULL, NULL); }
int sh_package_shader_receipt_matches(uintptr_t resource, const char *family,
    const char *name, const void *source, size_t length,
    const unsigned char *header, size_t header_length)
{
    int same = 0;
    if (!resource || !source || !header || header_length < 0x60 || header_length > 0x90) return 0;
    AcquireSRWLockShared(&sr_lock);
    sr_entry *entry = sr_find(resource);
    if (entry && entry->retained) while (length && (((const char *)source)[length - 1] == ' ' ||
        ((const char *)source)[length - 1] == '\r' || ((const char *)source)[length - 1] == '\n')) length--;
    if (entry && entry->body && entry->length == length && entry->header_length == header_length &&
        sr_equal(entry->family, family) && sr_equal(entry->name, name) && !memcmp(entry->body, source, length) &&
        !memcmp(entry->header, header, 0x24) && !memcmp(entry->header + 0x48, header + 0x48, header_length - 0x48)) same = 1;
    ReleaseSRWLockShared(&sr_lock); return same;
}
void sh_package_shader_capture_clear(sh_package_shader_capture *capture)
{
    if (!capture) return;
    free(capture->metadata); free(capture->source);
    while (capture->inputs) { sh_package_shader_input *input = capture->inputs; capture->inputs = input->next;
        free(input->path); free(input->body); free(input); }
    memset(capture, 0, sizeof(*capture));
}
static int sr_capture_copy(sr_entry *entry, sh_package_shader_capture *capture)
{
    capture->metadata = malloc(entry->metadata_length); capture->source = malloc(entry->length ? entry->length : 1);
    if (!capture->metadata || !capture->source) return 0;
    memcpy(capture->metadata, entry->metadata, entry->metadata_length); capture->metadata_length = entry->metadata_length;
    memcpy(capture->source, entry->body, entry->length); capture->source_length = entry->length; capture->ticket = entry->ticket;
    for (sr_input *input = entry->inputs; input; input = input->next) {
        if (input->failed || input->covered != input->length) continue;
        sh_package_shader_input *copy = calloc(1, sizeof(*copy));
        if (copy) { copy->path = _strdup(input->path); copy->body = malloc(input->length ? input->length : 1); }
        if (!copy || !copy->path || !copy->body) {
            if (copy) { free(copy->path); free(copy->body); free(copy); } return 0;
        }
        memcpy(copy->body, input->body, input->length); copy->length = input->length;
        copy->next = capture->inputs; capture->inputs = copy;
    }
    return 1;
}
int sh_package_shader_cook_copy(int renderer, const char *name,
    const void *source, size_t length, sh_package_shader_capture *capture)
{
    int ok = 0;
    if (!capture) return 0;
    memset(capture, 0, sizeof(*capture)); AcquireSRWLockShared(&sr_lock);
    sr_entry *entry = sr_find_cook(renderer, name);
    if (!entry || !entry->body || !entry->metadata || !entry->ticket) goto done;
    if (source) {
        if (entry->retained) while (length && (((const char *)source)[length - 1] == ' ' ||
            ((const char *)source)[length - 1] == '\r' || ((const char *)source)[length - 1] == '\n')) length--;
        if (length != entry->length || memcmp(source, entry->body, length)) goto done;
    }
    ok = sr_capture_copy(entry, capture);
done:
    ReleaseSRWLockShared(&sr_lock); if (!ok) sh_package_shader_capture_clear(capture); return ok;
}
int sh_package_shader_receipt_program_copy(uintptr_t resource, const char *name,
    const void *source, size_t length, const unsigned char *header, sh_package_shader_capture *capture)
{
    int ok = 0;
    if (!capture || !header) return 0;
    memset(capture, 0, sizeof(*capture)); AcquireSRWLockShared(&sr_lock);
    sr_entry *entry = sr_find(resource);
    if (!entry || !entry->body || !entry->metadata || !entry->ticket || entry->header_length != 0x78 ||
        !sr_equal(entry->family, "renderprog") || !sr_equal(entry->name, name) ||
        memcmp(entry->header, header, 0x24) || memcmp(entry->header + 0x48, header + 0x48, 0x30)) goto done;
    if (source) {
        if (entry->retained) while (length && (((const char *)source)[length - 1] == ' ' ||
            ((const char *)source)[length - 1] == '\r' || ((const char *)source)[length - 1] == '\n')) length--;
        if (length != entry->length || memcmp(source, entry->body, length)) goto done;
    }
    ok = sr_capture_copy(entry, capture);
done:
    ReleaseSRWLockShared(&sr_lock); if (!ok) sh_package_shader_capture_clear(capture); return ok;
}
#ifdef SH_PACKAGE_SHADER_RECEIPT_TESTING
void sh_package_shader_receipt_test_reset(void)
{
    AcquireSRWLockExclusive(&sr_lock);
    while (sr_entries) {
        sr_entry *entry = sr_entries; sr_entries = entry->next;
        sr_clear(entry); free(entry->family); free(entry->name); free(entry);
    }
    ReleaseSRWLockExclusive(&sr_lock); sr_current = NULL;
}
#endif
