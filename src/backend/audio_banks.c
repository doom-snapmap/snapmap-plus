#include "audio_banks.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static uint32_t ab_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static int ab_compare(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
static int ab_error(char *error, size_t capacity, const char *message)
{ if (error && capacity) snprintf(error, capacity, "%s", message); return 0; }
static int ab_failure(char *error, size_t capacity, const char *message)
{ ab_error(error, capacity, message); return -1; }

static unsigned char ab_lower(unsigned char c)
{ return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c; }

static uint32_t ab_identity(const char *name, size_t length)
{
    uint32_t value = UINT32_C(0x811c9dc5);
    for (size_t i = 0; i < length; i++)
        value = value * UINT32_C(0x01000193) ^ ab_lower((unsigned char)name[i]);
    return value;
}

static int ab_ascii(const char *name, size_t *length)
{
    *length = 0;
    if (!name) return 0;
    while (name[*length]) {
        if ((unsigned char)name[*length] >= 128) return 0;
        ++*length;
    }
    return *length != 0;
}

/* The native name loader copies into 260 bytes before stripping the last
 * extension. Directory paths would change the logical bank identity. */
static int ab_stem(const char *filename, size_t *length);

int sh_audio_bank_identity(const char *filename, uint32_t *id)
{
    size_t length;
    if (!id) return 0;
    *id = 0;
    if (!ab_stem(filename, &length)) return 0;
    *id = ab_identity(filename, length);
    return *id != 0;
}

/* The native loader reopens a bank it knows only by identity through a decimal
 * file stem, and every media file is named that way. Leading zeroes and signs
 * are not native spellings, so they are refused rather than folded. */
static int ab_decimal(const char *name, size_t length, uint32_t *id)
{
    uint64_t value = 0;
    if (!length || length > 10 || (length > 1 && name[0] == '0')) return 0;
    for (size_t i = 0; i < length; i++) {
        if (name[i] < '0' || name[i] > '9') return 0;
        value = value * 10 + (unsigned)(name[i] - '0');
    }
    if (value > UINT32_MAX) return 0;
    *id = (uint32_t)value; return 1;
}

static int ab_stem(const char *filename, size_t *length)
{
    const char *dot;
    if (!ab_ascii(filename, length) || *length >= 260 ||
        strchr(filename, '/') || strchr(filename, '\\')) return 0;
    dot = strrchr(filename, '.');
    if (dot) *length = (size_t)(dot - filename);
    return *length != 0;
}

int sh_audio_bank_names_identity(const char *filename, uint32_t id)
{
    uint32_t named = 0;
    size_t length;
    if (!id || !ab_stem(filename, &length)) return 0;
    if (ab_decimal(filename, length, &named) && named == id) return 1;
    return sh_audio_bank_identity(filename, &named) && named == id;
}

int sh_audio_bank_numeric_identity(const char *filename, uint32_t *id)
{
    size_t length;
    if (!id) return 0;
    *id = 0;
    return ab_stem(filename, &length) && ab_decimal(filename, length, id) && *id != 0;
}

int sh_audio_media_identity(const char *filename, uint32_t *id)
{ return sh_audio_bank_numeric_identity(filename, id); }

int sh_audio_event_identity(const char *declaration, uint32_t *id)
{
    char name[256];
    size_t length, start = 0, end;
    int event_prefix = 0;
    if (!id) return 0;
    *id = 0;
    if (!ab_ascii(declaration, &length) || length >= sizeof(name)) return 0;
    if (length >= 5) {
        char prefix[5];
        for (size_t i = 0; i < sizeof(prefix); i++)
            prefix[i] = (char)ab_lower((unsigned char)declaration[i]);
        event_prefix = !memcmp(prefix, "play_", 5) || !memcmp(prefix, "stop_", 5);
    }
    /* The early return tests forward slashes before normalizing backslashes.
     * Already-prefixed flat event names also retain any filename extension. */
    if (event_prefix && !strchr(declaration, '/')) {
        *id = ab_identity(declaration, length);
        return *id != 0;
    }
    for (size_t i = 0; i < length; i++)
        if (declaration[i] == '/' || declaration[i] == '\\') start = i + 1;
    end = length;
    for (size_t i = length; i > start; i--)
        if (declaration[i-1] == '.') { end = i-1; break; }
    length = end - start;
    /* Refuse conversion that the native fixed string cannot complete, instead
     * of selecting dependencies for a different, accidentally truncated name. */
    if (length + 5 >= sizeof(name)) return 0;
    memcpy(name, "play_", 5);
    memcpy(name + 5, declaration + start, length);
    *id = ab_identity(name, length + 5);
    return *id != 0;
}

void sh_audio_bank_free(sh_audio_bank *bank)
{
    if (!bank) return;
    free(bank->events); free(bank->banks); free(bank->required);
    free(bank->optional); free(bank->media); free(bank->objects); free(bank->targets);
    memset(bank, 0, sizeof(*bank));
}

static int ab_read(sh_audio_bank_source source, uint64_t offset, void *out, size_t length)
{
    return source.read && offset <= source.length && length <= source.length - offset &&
        source.read(source.context, offset, out, length);
}

/* Growing identity sets, sorted and deduplicated once the whole bank is read.
 * Nothing here retains bank bytes; only identities and source descriptors. */
typedef struct ab_set { uint32_t *values; size_t count, allocated; } ab_set;

static int ab_push(ab_set *set, uint32_t value)
{
    if (set->count == set->allocated) {
        size_t grow = set->allocated ? set->allocated * 2 : 64;
        uint32_t *values;
        if (grow < set->allocated || grow > SIZE_MAX / sizeof(*values)) return 0;
        values = (uint32_t *)realloc(set->values, grow * sizeof(*values));
        if (!values) return 0;
        set->values = values; set->allocated = grow;
    }
    set->values[set->count++] = value; return 1;
}

static size_t ab_unique(uint32_t *values, size_t count)
{
    size_t used = 1;
    if (count < 2) return count;
    qsort(values, count, sizeof(*values), ab_compare);
    for (size_t i = 1; i < count; ++i) if (values[i] != values[used-1]) values[used++] = values[i];
    return used;
}

static int ab_contains(const uint32_t *values, size_t count, uint32_t value)
{ return count && bsearch(&value, values, count, sizeof(value), ab_compare) != NULL; }

typedef struct ab_media_set {
    sh_audio_bank_media *rows;
    size_t count, allocated;
} ab_media_set;

static int ab_media_push(ab_media_set *set, uint32_t id, unsigned char stream, unsigned char embedded)
{
    if (set->count == set->allocated) {
        size_t grow = set->allocated ? set->allocated * 2 : 64;
        sh_audio_bank_media *rows;
        if (grow < set->allocated || grow > SIZE_MAX / sizeof(*rows)) return 0;
        rows = (sh_audio_bank_media *)realloc(set->rows, grow * sizeof(*rows));
        if (!rows) return 0;
        set->rows = rows; set->allocated = grow;
    }
    set->rows[set->count].id = id; set->rows[set->count].stream = stream;
    set->rows[set->count].embedded = embedded; ++set->count; return 1;
}

static int ab_media_compare(const void *a, const void *b)
{
    uint32_t x = ((const sh_audio_bank_media *)a)->id, y = ((const sh_audio_bank_media *)b)->id;
    return (x > y) - (x < y);
}

/* One row per identity. A bank that both carries and streams an identity keeps
 * the streaming descriptor with its carried flag, which is what a prefetched
 * source is. Contradictory descriptors keep the first, never a merged guess. */
static size_t ab_media_unique(sh_audio_bank_media *rows, size_t count)
{
    size_t used = 1;
    if (count < 2) return count;
    qsort(rows, count, sizeof(*rows), ab_media_compare);
    for (size_t i = 1; i < count; ++i) {
        if (rows[i].id != rows[used-1].id) { rows[used++] = rows[i]; continue; }
        if (rows[i].embedded) rows[used-1].embedded = 1;
        if (!rows[used-1].stream && rows[i].stream) rows[used-1].stream = rows[i].stream;
    }
    return used;
}

/* Verified v113 source descriptor: plug-in, stream kind, media identity, resident
 * size and source bits, with a length-prefixed extension for source plug-ins. */
static int ab_source(sh_audio_bank_source source, uint64_t body, uint32_t length,
    uint64_t *cursor, ab_media_set *media)
{
    unsigned char bytes[14], stream;
    uint32_t plugin, id, extension;
    if (length - *cursor < sizeof(bytes)) return 0;
    if (!ab_read(source, body + *cursor, bytes, sizeof(bytes))) return -1;
    plugin = ab_u32(bytes); stream = bytes[4]; id = ab_u32(bytes + 5);
    *cursor += sizeof(bytes);
    if ((plugin & 0x0f) == 2 || (plugin & 0x0f) == 5) {
        if (length - *cursor < 4) return 0;
        if (!ab_read(source, body + *cursor, bytes, 4)) return -1;
        extension = ab_u32(bytes);
        if (length - *cursor - 4 < extension) return 0;
        *cursor += 4 + (uint64_t)extension;
    }
    if (stream > 2) return 0;
    /* Only codec sources carry MEDIA here. Other source plug-ins use a
     * custom-object identity, even when its number resembles a .wem name. */
    return (plugin & 0x0f) == 1 && id && !ab_media_push(media, id, stream, 0) ? -1 : 1;
}

int sh_audio_bank_read_source(sh_audio_bank_source source, sh_audio_bank *out,
    char *error, size_t capacity)
{
    unsigned char header[16];
    uint64_t end = source.length, cursor = 0;
    int have_header = 0, have_objects = 0, status;
    ab_set events = {0}, defined = {0}, hard = {0}, control = {0}, banks = {0};
    ab_media_set media = {0};
    if (!out) return ab_failure(error, capacity, "missing audio bank output");
    memset(out, 0, sizeof(*out));
    if (!source.read)
        return ab_failure(error, capacity, "audio bank stream is unavailable");
    while (cursor < end) {
        uint32_t size;
        uint64_t next;
        if (end - cursor < 8) goto malformed;
        if (!ab_read(source, cursor, header, 8)) goto io;
        size = ab_u32(header + 4); cursor += 8;
        if (size > end - cursor) goto malformed;
        next = cursor + size;
        if (!memcmp(header, "BKHD", 4)) {
            if (have_header || cursor != 8 || size < 16) goto malformed;
            if (!ab_read(source, cursor, header, 16)) goto io;
            if (ab_u32(header) != 113) {
                ab_error(error, capacity, "unsupported cooked audio bank format");
                goto refused;
            }
            out->id = ab_u32(header + 4); out->language = ab_u32(header + 8);
            if (!out->id) goto malformed;
            have_header = 1;
        } else if (!memcmp(header, "DIDX", 4)) {
            /* Twelve-byte rows name the media this bank carries in its data. */
            if (!have_header || size % 12) goto malformed;
            for (uint32_t row = 0; row < size / 12; ++row) {
                if (!ab_read(source, cursor + (uint64_t)row * 12, header, 4)) goto io;
                if (ab_u32(header) && !ab_media_push(&media, ab_u32(header), 0, 1)) goto memory;
            }
        } else if (!memcmp(header, "HIRC", 4)) {
            uint32_t objects;
            if (!have_header || have_objects || size < 4) goto malformed;
            if (!ab_read(source, cursor, header, 4)) goto io;
            have_objects = 1; objects = ab_u32(header); cursor += 4;
            if (objects > (next - cursor) / 9) goto malformed;
            for (uint32_t i = 0; i < objects; ++i) {
                uint32_t object_length, id;
                uint64_t body, position;
                unsigned char kind;
                if (next - cursor < 9) goto malformed;
                if (!ab_read(source, cursor, header, 9)) goto io;
                kind = header[0];
                object_length = ab_u32(header + 1); id = ab_u32(header + 5);
                if (object_length < 4 || object_length > next - cursor - 5) goto malformed;
                body = cursor + 5;
                if (id && !ab_push(&defined, id)) goto memory;
                if (kind == 4) {
                    uint32_t actions;
                    if (!id || object_length < 8) goto malformed;
                    if (!ab_read(source, body + 4, header, 4)) goto io;
                    actions = ab_u32(header);
                    if (actions > (object_length - 8) / 4 || 8 + actions * 4u != object_length)
                        goto malformed;
                    for (uint32_t action = 0; action < actions; ++action) {
                        if (!ab_read(source, body + 8 + (uint64_t)action * 4, header, 4)) goto io;
                        /* An event's own actions are part of the event. */
                        if (ab_u32(header) && !ab_push(&hard, ab_u32(header))) goto memory;
                    }
                    if (!ab_push(&events, id)) goto memory;
                } else if (kind == 3) {
                    uint32_t action_type, target, owner = 0;
                    if (object_length < 12) goto malformed;
                    if (!ab_read(source, body + 4, header, 8)) goto io;
                    action_type = (uint32_t)header[0] | (uint32_t)header[1] << 8;
                    target = ab_u32(header + 2);
                    /* Skip the property and ranged-property lists by count. */
                    position = 11;
                    if (!ab_read(source, body + position, header, 1)) goto io;
                    position += 1 + (uint64_t)header[0] * 5;
                    if (position >= object_length) goto malformed;
                    if (!ab_read(source, body + position, header, 1)) goto io;
                    position += 1 + (uint64_t)header[0] * 9;
                    if (position > object_length) goto malformed;
                    /* Only a play action records the bank owning its target. */
                    if (action_type == 0x0403 && object_length - position >= 5) {
                        if (!ab_read(source, body + position + 1, header, 4)) goto io;
                        owner = ab_u32(header);
                        if (owner && !ab_push(&banks, owner)) goto memory;
                    }
                    /* A play target another bank owns is attributed to that bank,
                     * not missing. Anything else a play action names must be here. */
                    if (target && action_type == 0x0403 && (!owner || owner == out->id) &&
                        !ab_push(&hard, target)) goto memory;
                    if (target && action_type == 0x0403 && owner && owner != out->id) {
                        if (out->target_count >= SIZE_MAX / sizeof(*out->targets) - 1) goto memory;
                        sh_audio_bank_target *grown = realloc(out->targets,
                            (out->target_count + 1) * sizeof(*grown));
                        if (!grown) goto memory;
                        out->targets = grown; grown[out->target_count++] = (sh_audio_bank_target){owner, target};
                    }
                    if (target && action_type != 0x0403 && !ab_push(&control, target)) goto memory;
                } else if (kind == 2) {
                    position = 4;
                    status = ab_source(source, body, object_length, &position, &media);
                    if (!status) goto malformed;
                    if (status < 0) goto memory;
                } else if (kind == 11) {
                    uint32_t sources;
                    if (object_length < 9) goto malformed;
                    if (!ab_read(source, body + 5, header, 4)) goto io;
                    sources = ab_u32(header); position = 9;
                    if (sources > (object_length - 9) / 14) goto malformed;
                    for (uint32_t track = 0; track < sources; ++track) {
                        status = ab_source(source, body, object_length, &position, &media);
                        if (!status) goto malformed;
                        if (status < 0) goto memory;
                    }
                }
                cursor = body + object_length;
            }
            if (cursor != next) goto malformed;
        } else if (!have_header) goto malformed;
        cursor = next;
    }
    if (!have_header) goto malformed;
    out->events = events.values; out->event_count = ab_unique(events.values, events.count);
    memset(&events, 0, sizeof(events));
    defined.count = ab_unique(defined.values, defined.count);
    hard.count = ab_unique(hard.values, hard.count);
    control.count = ab_unique(control.values, control.count);
    banks.count = ab_unique(banks.values, banks.count);
    out->media = media.rows; out->media_count = ab_media_unique(media.rows, media.count);
    memset(&media, 0, sizeof(media));
    for (size_t i = 0; i < banks.count; ++i) if (banks.values[i] != out->id)
        banks.values[out->bank_count++] = banks.values[i];
    out->banks = banks.values; memset(&banks, 0, sizeof(banks));
    for (size_t i = 0; i < hard.count; ++i)
        if (!ab_contains(defined.values, defined.count, hard.values[i]))
            hard.values[out->required_count++] = hard.values[i];
    out->required = hard.values; memset(&hard, 0, sizeof(hard));
    for (size_t i = 0; i < control.count; ++i)
        if (!ab_contains(defined.values, defined.count, control.values[i]))
            control.values[out->optional_count++] = control.values[i];
    out->optional = control.values; memset(&control, 0, sizeof(control));
    out->objects = defined.values; out->object_count = defined.count;
    if (error && capacity) error[0] = 0;
    return 1;
malformed:
    ab_error(error, capacity, "malformed or truncated cooked audio bank");
refused:
    status = 0; goto done;
memory:
    ab_failure(error, capacity, "audio event index allocation failed");
    status = -1; goto done;
io:
    ab_failure(error, capacity, "audio bank index read failed");
    status = -1;
done:
    free(events.values); free(defined.values); free(hard.values);
    free(control.values); free(banks.values); free(media.rows);
    sh_audio_bank_free(out);
    return status;
}

static int ab_file_read(void *context, uint64_t offset, void *out, size_t length)
{
    FILE *stream = context;
    return offset <= INT64_MAX && !_fseeki64(stream, (__int64)offset, SEEK_SET) &&
        fread(out, 1, length, stream) == length;
}

int sh_audio_bank_read(FILE *stream, sh_audio_bank *out, char *error, size_t capacity)
{
    __int64 length;
    if (!stream || _fseeki64(stream, 0, SEEK_END) || (length = _ftelli64(stream)) < 0) {
        if (out) memset(out, 0, sizeof(*out));
        return ab_failure(error, capacity, "audio bank stream is unavailable");
    }
    return sh_audio_bank_read_source((sh_audio_bank_source){stream, (uint64_t)length, ab_file_read},
        out, error, capacity);
}

int sh_audio_bank_has_event(const sh_audio_bank *bank, uint32_t event)
{
    if (!bank || !bank->event_count || !bank->events) return 0;
    return bsearch(&event, bank->events, bank->event_count, sizeof(event), ab_compare) != NULL;
}

int sh_audio_bank_needs_bank(const sh_audio_bank *bank, uint32_t id)
{
    if (!bank || !bank->bank_count || !bank->banks) return 0;
    return bsearch(&id, bank->banks, bank->bank_count, sizeof(id), ab_compare) != NULL;
}

const sh_audio_bank_media *sh_audio_bank_find_media(const sh_audio_bank *bank, uint32_t id)
{
    sh_audio_bank_media key = {id, 0, 0};
    if (!bank || !bank->media_count || !bank->media) return NULL;
    return (const sh_audio_bank_media *)bsearch(&key, bank->media, bank->media_count,
        sizeof(*bank->media), ab_media_compare);
}

static int ab_same(const sh_package_file_identity *a, const sh_package_file_identity *b)
{ return a->length == b->length && !memcmp(a->digest, b->digest, sizeof(a->digest)); }

static int ab_release(sh_audio_bank_slot *slot, sh_audio_bank_ops ops,
    char *error, size_t capacity)
{
    int result;
    slot->uncertain = 1;
    result = ops.unload(ops.context, slot->owned ? slot->loaded_id : slot->id);
    if (result != -1) slot->uncertain = 0;
    /* 54 means the native table no longer contains this exact bank key. */
    if (result != 1 && result != 54) {
        if (error && capacity) snprintf(error, capacity,
            "audio bank '%s' could not be released (result %d)", slot->name, result);
        return 0;
    }
    slot->owned = 0; slot->loaded_id = 0; slot->content_known = 0;
    return 1;
}

static int ab_load(sh_audio_bank_slot *slot, sh_audio_bank_ops ops, int *status,
    char *error, size_t capacity)
{
    uint32_t id = 0;
    int result;
    slot->uncertain = 1;
    result = ops.load(ops.context, slot->name, &id);
    if (result != -1) slot->uncertain = 0;
    if (result == 1) { slot->owned = 1; slot->loaded_id = id; slot->content_known = 0; }
    *status = result;
    if ((result != 1 && result != 69) || id != slot->id) {
        if (error && capacity) snprintf(error, capacity,
            "audio bank '%s' could not be activated (result %d, identity %u)",
            slot->name, result, id);
        return 0;
    }
    return 1;
}

int sh_audio_banks_reconcile(sh_audio_bank_slot *slots, size_t count,
    const unsigned char *wanted, const sh_package_file_identity *const *content,
    sh_audio_bank_ops ops, char *error, size_t capacity)
{
    if ((count && (!slots || !wanted)) || !ops.load || !ops.unload)
        return ab_error(error, capacity, "audio bank activation is unavailable");
    /* Validate the entire request before any native side effect. */
    for (size_t i = 0; i < count; ++i) {
        if (!slots[i].name || !slots[i].name[0] || !slots[i].id || slots[i].uncertain ||
            (content && content[i] && !wanted[i]))
            return ab_error(error, capacity, "audio bank state is invalid or interrupted");
        for (size_t j = 0; j < i; ++j)
            if (slots[i].id == slots[j].id)
                return ab_error(error, capacity, "duplicate audio bank identity");
    }
    for (size_t i = 0; i < count; ++i) {
        sh_audio_bank_slot *slot = slots+i;
        const sh_package_file_identity *next = content ? content[i] : NULL;
        int result;
        if (!wanted[i] && !slot->restore_native) continue;
        /* Remove a previous override before asking native code for different
         * bytes. Also retire a mismatched-ID load recorded by a failed pass. */
        if ((slot->owned && slot->loaded_id != slot->id) ||
            (slot->content_known && (!next || !ab_same(&slot->content, next))))
            if (!ab_release(slot, ops, error, capacity)) return 0;
        if (!ab_load(slot, ops, &result, error, capacity)) return 0;
        if (next) {
            if (result == 69 && (!slot->content_known || !ab_same(&slot->content, next))) {
                if (!slot->owned) slot->restore_native = 1;
                if (!ab_release(slot, ops, error, capacity)) return 0;
                if (!ab_load(slot, ops, &result, error, capacity)) return 0;
                if (result != 1)
                    return ab_error(error, capacity, "audio bank changed during override activation");
            }
            slot->content = *next; slot->content_known = 1;
        } else if (slot->restore_native) {
            /* Return ownership to the original native consumer. This survives
             * provider rollback, including a failed load after displacement. */
            slot->owned = 0; slot->loaded_id = 0;
            slot->content_known = 0; slot->restore_native = 0;
        }
    }
    for (size_t i = 0; i < count; ++i) if (!wanted[i] && slots[i].owned)
        if (!ab_release(slots+i, ops, error, capacity)) return 0;
    if (error && capacity) error[0] = 0;
    return 1;
}

static int ab_copy_rows(void **out, const void *source, size_t count, size_t size)
{
    if (!count) return 1;
    if (!source || count > SIZE_MAX / size || !(*out = malloc(count * size))) return 0;
    memcpy(*out, source, count * size); return 1;
}
int sh_audio_bank_copy(const sh_audio_bank *source, sh_audio_bank *out)
{
    sh_audio_bank copy = {0};
    if (!source || !out) return 0;
    if (!ab_copy_rows((void **)&copy.events, source->events, source->event_count, sizeof(*copy.events)) ||
        !ab_copy_rows((void **)&copy.banks, source->banks, source->bank_count, sizeof(*copy.banks)) ||
        !ab_copy_rows((void **)&copy.required, source->required, source->required_count, sizeof(*copy.required)) ||
        !ab_copy_rows((void **)&copy.optional, source->optional, source->optional_count, sizeof(*copy.optional)) ||
        !ab_copy_rows((void **)&copy.media, source->media, source->media_count, sizeof(*copy.media)) ||
        !ab_copy_rows((void **)&copy.objects, source->objects, source->object_count, sizeof(*copy.objects)) ||
        !ab_copy_rows((void **)&copy.targets, source->targets, source->target_count, sizeof(*copy.targets))) {
        sh_audio_bank_free(&copy); return 0;
    }
    copy.id = source->id; copy.language = source->language;
    copy.event_count = source->event_count; copy.bank_count = source->bank_count;
    copy.required_count = source->required_count; copy.optional_count = source->optional_count;
    copy.media_count = source->media_count; copy.object_count = source->object_count;
    copy.target_count = source->target_count; *out = copy; return 1;
}

/* flags[i] SH_AUDIO_CLOSURE_REGISTERED: SnapMap's own sound init registers
 * bank i, so any of its definitions serves a lookup. SH_AUDIO_CLOSURE_NATIVE:
 * it is that registered installed original, owned by SnapMap together with
 * the references SnapMap leaves unresolved. Providers SnapMap never registers
 * still serve banks a program supplies (campaign or imported content), and
 * events defined nowhere else, because those loads are explicitly used. */
#define AB_REGISTERED(i) (!flags || (flags[i] & SH_AUDIO_CLOSURE_REGISTERED))
#define AB_NATIVE(i) (flags && (flags[i] & SH_AUDIO_CLOSURE_NATIVE))
int sh_audio_bank_closure_native(const sh_audio_bank *const *banks, const unsigned char *flags,
    size_t bank_count, const uint32_t *events, size_t event_count, int require_events,
    unsigned char *selected, sh_audio_bank_closure_report *report,
    int (*absent)(void *context, uint32_t id), void *absent_context,
    char *error, size_t capacity)
{
    sh_audio_bank_closure_report result = {0};
    if (report) memset(report, 0, sizeof(*report));
    if (error && capacity) *error = 0;
    if ((bank_count && (!banks || !selected)) || (event_count && !events))
        return ab_error(error, capacity, "audio closure input is missing");
    for (size_t i = 0; i < bank_count; i++) {
        if (!banks[i]) {
            if (selected[i]) return ab_error(error, capacity, "selected audio bank has no effective source");
            continue;
        }
        if (!banks[i]->id || (banks[i]->event_count && !banks[i]->events) ||
            (banks[i]->bank_count && !banks[i]->banks) ||
            (banks[i]->required_count && !banks[i]->required) ||
            (banks[i]->object_count && !banks[i]->objects) ||
            (banks[i]->media_count && !banks[i]->media) ||
            (banks[i]->target_count && !banks[i]->targets))
            return ab_error(error, capacity, "audio closure bank metadata is incomplete");
    }
    for (size_t e = 0; e < event_count; e++) {
        int found = 0;
        for (int pass = 0; pass < 2 && !found; pass++)
            for (size_t b = 0; b < bank_count; b++)
                if ((pass || AB_REGISTERED(b)) && sh_audio_bank_has_event(banks[b], events[e])) {
                    selected[b] = 1; found = 1;
                }
        if (!found && require_events) {
            if (error && capacity) snprintf(error, capacity, "audio event %u has no bank in its source scope", events[e]);
            return 0;
        }
        result.matched_events += found;
    }
    for (int changed = 1; changed;) {
        changed = 0;
        for (size_t b = 0; b < bank_count; b++) if (selected[b]) {
            const sh_audio_bank *bank = banks[b];
            for (size_t d = 0; d < bank->bank_count; d++) {
                int found = 0;
                for (size_t i = 0; i < bank_count; i++) if (banks[i] && banks[i]->id == bank->banks[d]) {
                    found = 1;
                    if (selected[i]) continue;
                    if (!AB_REGISTERED(i) && AB_NATIVE(b)) {
                        result.native_absent++;
                        if (absent && !absent(absent_context, bank->banks[d]))
                            return ab_error(error, capacity, "cannot retain a natively unresolved audio reference");
                        continue;
                    }
                    selected[i] = 1; changed = 1; result.dependency_banks++;
                }
                if (!found) {
                    if (error && capacity) snprintf(error, capacity, "audio bank %u requires absent bank %u", bank->id, bank->banks[d]);
                    return 0;
                }
            }
            /* Native object lookups are shared across loaded banks. A compact
             * publication can keep only a changed event while its unchanged
             * action remains in an original bank. Resolve every hard reference
             * in this already scoped inventory and retain its provider, including
             * that provider's own dependencies. Prefer an existing supplier so
             * dormant duplicate definitions do not become unnecessary loads. */
            for (size_t r = 0; r < bank->required_count; r++) {
                size_t provider = SIZE_MAX, fallback = SIZE_MAX;
                for (size_t i = 0; i < bank_count; i++) {
                    if (!banks[i] || !ab_contains(banks[i]->objects, banks[i]->object_count, bank->required[r])) continue;
                    size_t *slot = AB_REGISTERED(i) ? &provider : &fallback;
                    if (*slot == SIZE_MAX || selected[i]) *slot = i;
                    if (selected[i] && slot == &provider) break;
                }
                if (provider == SIZE_MAX && !AB_NATIVE(b)) provider = fallback;
                if (provider == SIZE_MAX && fallback != SIZE_MAX) {
                    if (!selected[fallback]) {
                        result.native_absent++;
                        if (absent && !absent(absent_context, bank->required[r]))
                            return ab_error(error, capacity, "cannot retain a natively unresolved audio reference");
                    }
                    continue;
                }
                if (provider == SIZE_MAX) {
                    if (error && capacity) snprintf(error, capacity, "audio bank %u requires absent object %u", bank->id, bank->required[r]);
                    return 0;
                }
                if (!selected[provider]) { selected[provider] = 1; changed = 1; result.dependency_banks++; }
            }
            /* A resident source can reuse DATA carried by another bank even
             * without an explicit play-action bank edge. Prefer a selected
             * provider, otherwise retain the first scoped inventory provider.
             * A source without such a provider remains for the media resolver
             * to check; this metadata-only pass cannot query external media. */
            for (size_t m = 0; m < bank->media_count; m++) {
                const sh_audio_bank_media *media = &bank->media[m];
                if (media->stream || media->embedded) continue;
                size_t provider = SIZE_MAX, fallback = SIZE_MAX;
                for (size_t i = 0; i < bank_count; i++) {
                    const sh_audio_bank_media *candidate = sh_audio_bank_find_media(banks[i], media->id);
                    if (!candidate || !candidate->embedded) continue;
                    size_t *slot = AB_REGISTERED(i) ? &provider : &fallback;
                    if (*slot == SIZE_MAX || selected[i]) *slot = i;
                    if (selected[i] && slot == &provider) break;
                }
                if (provider == SIZE_MAX && !AB_NATIVE(b)) provider = fallback;
                if (provider != SIZE_MAX && !selected[provider]) {
                    selected[provider] = 1; changed = 1; result.dependency_banks++;
                }
            }
        }
    }
    for (size_t b = 0; b < bank_count; b++) if (selected[b]) {
        const sh_audio_bank *bank = banks[b];
        for (size_t t = 0; t < bank->target_count; t++) {
            const sh_audio_bank_target *target = bank->targets + t; int found = 0, unregistered = 0;
            for (size_t i = 0; i < bank_count; i++) {
                if (!banks[i] || banks[i]->id != target->bank) continue;
                unregistered |= !AB_REGISTERED(i) && !selected[i];
                if (selected[i] && ab_contains(banks[i]->objects, banks[i]->object_count, target->object)) { found = 1; break; }
            }
            /* SnapMap's own bank keeps a preparation target it never loads. */
            if (!found && unregistered && AB_NATIVE(b)) {
                result.native_absent++;
                if (absent && (!absent(absent_context, target->bank) || !absent(absent_context, target->object)))
                    return ab_error(error, capacity, "cannot retain a natively unresolved audio reference");
                continue;
            }
            if (!found) {
                if (error && capacity) snprintf(error, capacity, "audio bank %u requires absent object %u in bank %u",
                    bank->id, target->object, target->bank);
                return 0;
            }
        }
        result.optional_targets += bank->optional_count;
    }
    if (report) *report = result;
    return 1;
}
#undef AB_REGISTERED
#undef AB_NATIVE
int sh_audio_bank_closure(const sh_audio_bank *const *banks, size_t bank_count,
    const uint32_t *events, size_t event_count, int require_events,
    unsigned char *selected, sh_audio_bank_closure_report *report,
    char *error, size_t capacity)
{
    return sh_audio_bank_closure_native(banks, NULL, bank_count, events, event_count, require_events,
        selected, report, NULL, NULL, error, capacity);
}

int sh_audio_banks_append(sh_audio_bank_slot *slots, size_t count,
    const unsigned char *wanted, const sh_package_file_identity *const *content,
    sh_audio_bank_ops ops, char *error, size_t capacity)
{
    if ((count && (!slots || !wanted)) || !ops.load || !ops.unload)
        return ab_error(error, capacity, "additive audio activation is unavailable");
    /* Complete preflight precedes even an unrelated fresh load. */
    for (size_t i = 0; i < count; i++) {
        const sh_audio_bank_slot *slot = slots + i;
        const sh_package_file_identity *next = content ? content[i] : NULL;
        if (!slot->name || !*slot->name || !slot->id || slot->uncertain ||
            (next && !wanted[i]) || (slot->owned && slot->loaded_id != slot->id))
            return ab_error(error, capacity, "additive audio state is invalid or interrupted");
        for (size_t j = 0; j < i; j++) if (slots[j].id == slot->id)
            return ab_error(error, capacity, "duplicate additive audio bank identity");
        if (wanted[i] && slot->owned &&
            ((slot->content_known && (!next || !ab_same(&slot->content, next))) ||
             (!slot->content_known && next)))
            return ab_error(error, capacity, "additive audio would replace an existing bank");
    }
    for (size_t i = 0; i < count; i++) {
        sh_audio_bank_slot *slot = slots + i;
        const sh_package_file_identity *next = content ? content[i] : NULL;
        int result;
        if (!wanted[i]) continue;
        if (!ab_load(slot, ops, &result, error, capacity)) return 0;
        if (next) {
            /* 69 is evidence of existing native ownership, never permission
             * to unload its voices in order to install different bytes. */
            if (result == 69 && (!slot->owned || !slot->content_known || !ab_same(&slot->content, next)))
                return ab_error(error, capacity, "additive audio requires a distinct identity for already-loaded bank bytes");
            slot->content = *next; slot->content_known = 1;
        }
    }
    if (error && capacity) *error = 0;
    return 1;
}

int sh_audio_banks_restore_additions(sh_audio_bank_slot *slots, size_t count,
    const sh_audio_bank_slot *before, size_t before_count,
    sh_audio_bank_ops ops, char *error, size_t capacity)
{
    if ((count && !slots) || (before_count && !before) || !ops.unload)
        return ab_error(error, capacity, "additive audio recovery snapshot is unavailable");
    for (size_t i = 0; i < before_count; i++) {
        size_t matches = 0;
        for (size_t j = 0; j < count; j++) if (slots[j].id == before[i].id) matches++;
        if (matches != 1)
            return ab_error(error, capacity, "preexisting audio catalog changed during additive admission");
    }
    /* A changed preexisting owner is outside this transaction's authority. */
    for (size_t i = 0; i < count; i++) {
        const sh_audio_bank_slot *old = NULL;
        for (size_t j = 0; j < before_count; j++) if (before[j].id == slots[i].id) { old = before+j; break; }
        if (slots[i].uncertain || (old && old->uncertain))
            return ab_error(error, capacity, "additive audio recovery retains uncertain native ownership");
        if (old && old->owned && (!slots[i].owned || slots[i].loaded_id != old->loaded_id ||
            slots[i].content_known != old->content_known ||
            (old->content_known && !ab_same(&slots[i].content, &old->content))))
            return ab_error(error, capacity, "preexisting audio ownership changed during additive admission");
    }
    for (size_t i = count; i; i--) {
        sh_audio_bank_slot *slot = slots+i-1;
        const sh_audio_bank_slot *old = NULL;
        for (size_t j = 0; j < before_count; j++) if (before[j].id == slot->id) { old = before+j; break; }
        if (old && old->owned) continue;
        if (slot->owned && !ab_release(slot, ops, error, capacity)) return 0;
        if (old) {
            const char *name = slot->name;
            *slot = *old; slot->name = name;
        } else {
            slot->owned = slot->uncertain = slot->content_known = slot->restore_native = 0;
            slot->loaded_id = 0; memset(&slot->content, 0, sizeof(slot->content));
        }
    }
    if (error && capacity) *error = 0;
    return 1;
}
