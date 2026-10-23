/* Replays a captured native reflection snapshot as the native memory that
 * decl_native_schema.c and decl_native_registry.c read: the class and enum
 * registries with their object/pointer reader tables, reflected fields,
 * bitfield setter bytes, enum members and the declaration manager registry.
 * The input is the line file written by the research capture tool
 * (snaphak-re tools/re/reflection_snapshot.py replay). Reader identities are
 * the snapshot's image-relative values over the image's preferred base, so
 * equal native readers stay equal. A malformed or incomplete file fails the
 * load; nothing is synthesized or defaulted. */
#ifndef TESTS_NATIVE_REFLECTION_REPLAY_H
#define TESTS_NATIVE_REFLECTION_REPLAY_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A shader source receipt observed in the captured process's backend ledger
 * (package_shader_receipt.c sr_entry), in ledger order. The observed resource
 * is its (resource manager, slot) in the replayed lists; identity[k] is 'o'
 * when header slot +8k held the object's own value (vtable, name, owner), which
 * the replay supplies, and 'r' when the observed bytes are kept as they were. */
typedef struct native_replay_input {
    const char *path;
    const unsigned char *body, *seen;
    size_t length, covered;
    int failed;
} native_replay_input;
typedef struct native_replay_receipt {
    int cook, renderer;
    long long manager, slot;
    uint64_t epoch, ticket, preparation_ticket;
    const char *family, *name;
    const unsigned char *body, *metadata;
    size_t length, header_length, metadata_length, input_count;
    int retained;
    unsigned char header[0x90];
    char identity[4];
    native_replay_input *inputs;
} native_replay_receipt;

typedef struct native_replay {
    unsigned char *arena;
    size_t size, used;
    uintptr_t reflection, registry;
    /* The address of the slot holding the first native resource manager, as
     * sh_native_resource_registry_source.head_slot expects. */
    uintptr_t resources;
    char renderer[8], pin[72];
    size_t classes, enums, fields, managers, loaded, resource_managers, resource_entries;
    native_replay_receipt *receipts;
    size_t receipt_count, shader_headers;
} native_replay;

#define NATIVE_REPLAY_BASE ((uintptr_t)0x140000000u)

static int native_replay_read(void *context, uintptr_t address, void *out, size_t length)
{
    native_replay *r = context;
    uintptr_t start = (uintptr_t)r->arena;
    if (!r->arena || address < start || address - start > r->used || length > r->used - (size_t)(address - start))
        return 0;
    memcpy(out, (const void *)address, length); return 1;
}
static void *nr_take(native_replay *r, size_t n)
{
    void *p;
    n = (n + 15) & ~(size_t)15;
    if (n > r->size - r->used) return NULL;
    p = r->arena + r->used; r->used += n; return p;
}
static char *nr_text(native_replay *r, const char *text)
{
    size_t n = strlen(text);
    char *p = nr_take(r, n + 1);
    if (p) memcpy(p, text, n + 1);
    return p;
}
static void nr_put(void *at, size_t offset, uintptr_t value) { memcpy((char *)at + offset, &value, 8); }
static void nr_put32(void *at, size_t offset, int32_t value) { memcpy((char *)at + offset, &value, 4); }
static size_t nr_split(char *line, char **out, size_t capacity)
{
    size_t n = 0;
    out[n++] = line;
    for (char *c = line; *c && n < capacity; c++) if (*c == '\t') { *c = 0; out[n++] = c + 1; }
    return n;
}
static uintptr_t nr_reader(const char *text) { return *text ? NATIVE_REPLAY_BASE + (uintptr_t)_strtoui64(text, NULL, 10) : 0; }
static int nr_fail(char *error, size_t capacity, const char *what, size_t line)
{
    if (error && capacity) snprintf(error, capacity, "reflection replay: %s (line %zu)", what, line);
    return 0;
}
static int nr_nibble(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
/* Captured native text: length bytes, then NUL when the capture saw one there
 * (otherwise a non-NUL byte, which is all the readers test). */
static unsigned char *nr_bytes(native_replay *r, const char *hex, size_t length, int nul)
{
    unsigned char *p;
    if (strlen(hex) != length * 2 || !(p = nr_take(r, length + 1))) return NULL;
    for (size_t i = 0; i < length; i++) {
        int high = nr_nibble(hex[i * 2]), low = nr_nibble(hex[i * 2 + 1]);
        if (high < 0 || low < 0) return NULL;
        p[i] = (unsigned char)(high << 4 | low);
    }
    p[length] = nul ? 0 : 1; return p;
}
/* One manager's loaded entries (decl_native_registry.c dr_capture layout):
 * lines tag<TAB>name<TAB>id<TAB>owner<TAB>present[<TAB>state...]. A declaration
 * entityDef entry's state is the prepared state entity_state reads; a resource
 * renderParm/renderProgFlag entry's state is the shader ABI na_shader_fixed reads. */
static int nr_entries(native_replay *r, char **lines, size_t line_count, size_t *i, const char *tag,
    unsigned char *manager, int linked, char *error, size_t capacity)
{
    size_t count = 0;
    unsigned char *array;
    while (*i + 1 + count < line_count && lines[*i + 1 + count][0] == tag[0] && lines[*i + 1 + count][1] == '\t') count++;
    if (!(array = nr_take(r, count ? count * 8 : 8))) return nr_fail(error, capacity, "arena exhausted", *i + 1);
    nr_put(manager, 0x20, count ? (uintptr_t)array : 0); nr_put32(manager, 0x28, (int32_t)count);
    for (size_t k = 0; k < count; k++) {
        char *e[20];
        unsigned char *object, *text = NULL;
        size_t n;
        int header = 0;
        ++*i;
        n = nr_split(lines[*i], e, 20);
        /* A shader resource may carry its observed fixed header (0x90 bytes,
         * process identity zeroed) and vtable RVA. */
        if (linked && n == 17) { header = 1; n = 15; }
        /* Only a linked resource list may hold a null slot (dr_capture). */
        if (n == 5 && !strcmp(e[4], "0") && linked && !e[1][0] && !e[2][0] && !e[3][0]) continue;
        if (strcmp(e[4], "1") || (n != 5 && n != (linked ? 15u : 13u))) return nr_fail(error, capacity, "malformed entry", *i + 1);
        if (!(object = nr_take(r, 0x148))) return nr_fail(error, capacity, "arena exhausted", *i + 1);
        if (header) {
            unsigned char *raw = nr_bytes(r, e[15], 0x90, 1);
            uint64_t error_value, mask; int32_t id, length32, kind; uint32_t index;
            if (!raw || !e[16][0]) return nr_fail(error, capacity, "malformed shader header", *i + 1);
            memcpy(&error_value, raw + 0x18, 8); memcpy(&id, raw + 0x20, 4); memcpy(&length32, raw + 0x40, 4);
            memcpy(&mask, raw + 0x58, 8); memcpy(&index, raw + 0x68, 4); memcpy(&kind, raw + 0x6c, 4);
            /* The decoded cells must restate the observed bytes. */
            if ((error_value != 0) != (e[5][0] == '1') || raw[0x2c] != (unsigned char)strtoul(e[6], NULL, 10) ||
                id != (int32_t)strtoul(e[2], NULL, 10) || length32 != (int32_t)strtol(e[7], NULL, 10) ||
                mask != _strtoui64(e[14], NULL, 10) || index != (uint32_t)strtoul(e[13], NULL, 10) || kind != (int32_t)strtol(e[12], NULL, 10))
                return nr_fail(error, capacity, "shader header disagrees with its fields", *i + 1);
            memcpy(object, raw, 0x90); nr_put(object, 0, nr_reader(e[16])); r->shader_headers++;
        }
        nr_put(object, 8, (uintptr_t)nr_text(r, e[1])); nr_put(object, 0x10, e[3][0] == '1' ? (uintptr_t)manager : 0);
        nr_put32(object, 0x20, (int32_t)strtoul(e[2], NULL, 10));
        if (n > 5) {
            long length = strtol(e[7], NULL, 10);
            if (!header) nr_put(object, 0x18, e[5][0] == '1');
            object[0x2c] = (unsigned char)strtoul(e[6], NULL, 10);
            if (e[9][0] == '1' && (length < 0 || !(text = nr_bytes(r, e[10], (size_t)length, e[8][0] == '1'))))
                return nr_fail(error, capacity, "malformed native text", *i + 1);
            if (linked) {
                nr_put(object, 0x38, (uintptr_t)text); nr_put32(object, 0x40, (int32_t)length);
                nr_put32(object, 0x6c, (int32_t)strtol(e[12], NULL, 10)); nr_put32(object, 0x68, (int32_t)strtoul(e[13], NULL, 10));
                nr_put(object, 0x58, (uintptr_t)_strtoui64(e[14], NULL, 10));
            } else {
                nr_put(object, 0x140, (uintptr_t)text); nr_put32(object, 0x138, (int32_t)length);
                nr_put(object, 0x60, (uintptr_t)nr_text(r, e[11])); object[0x128] = (unsigned char)strtoul(e[12], NULL, 10);
            }
        }
        nr_put(array, k * 8, (uintptr_t)object);
        if (linked) r->resource_entries++; else r->loaded++;
    }
    return 1;
}

static int nr_receipt_bytes(native_replay *r, const char *hex, size_t length, const unsigned char **out)
{
    *out = NULL;
    if (!strcmp(hex, "-")) return length == 0;
    return (*out = nr_bytes(r, hex, length, 1)) != NULL;
}
/* An S line and its I lines (the capture tool's receipt record). */
static int nr_receipt(native_replay *r, char **lines, size_t line_count, size_t *i, native_replay_receipt *x,
    char *error, size_t capacity)
{
    char *s[24];
    const unsigned char *header;
    size_t inputs;
    if (nr_split(lines[*i], s, 24) != 19 || strcmp(s[0], "S") || (strcmp(s[1], "0") && strcmp(s[1], "1")) ||
        (strlen(s[12]) != 3 && strcmp(s[12], "-"))) return nr_fail(error, capacity, "malformed receipt", *i + 1);
    x->cook = s[1][0] == '1'; x->renderer = (int)strtol(s[2], NULL, 10);
    x->manager = s[3][0] ? strtoll(s[3], NULL, 10) : -1; x->slot = s[4][0] ? strtoll(s[4], NULL, 10) : -1;
    if (x->cook != (x->manager < 0) || (x->manager < 0) != (x->slot < 0)) return nr_fail(error, capacity, "malformed receipt key", *i + 1);
    x->epoch = _strtoui64(s[5], NULL, 10);
    x->family = strcmp(s[6], "-") ? nr_text(r, s[6]) : NULL; x->name = strcmp(s[7], "-") ? nr_text(r, s[7]) : NULL;
    x->length = (size_t)_strtoui64(s[8], NULL, 10); x->header_length = (size_t)_strtoui64(s[9], NULL, 10);
    x->retained = (int)strtol(s[10], NULL, 10);
    if (!(header = nr_bytes(r, s[11], 0x90, 1)) || x->header_length > 0x90) return nr_fail(error, capacity, "malformed receipt header", *i + 1);
    memcpy(x->header, header, 0x90);
    if (strcmp(s[12], "-")) {
        for (int k = 0; k < 3; k++) if (s[12][k] != 'o' && s[12][k] != 'r') return nr_fail(error, capacity, "malformed receipt identity", *i + 1);
        memcpy(x->identity, s[12], 3);
    } else if (!x->cook) return nr_fail(error, capacity, "receipt without object identity", *i + 1);
    x->metadata_length = (size_t)_strtoui64(s[14], NULL, 10);
    if (!nr_receipt_bytes(r, s[13], x->length, &x->body) || !nr_receipt_bytes(r, s[15], x->metadata_length, &x->metadata))
        return nr_fail(error, capacity, "malformed receipt body", *i + 1);
    x->ticket = _strtoui64(s[16], NULL, 10); x->preparation_ticket = _strtoui64(s[17], NULL, 10);
    inputs = (size_t)_strtoui64(s[18], NULL, 10);
    if (inputs > 4096 || (inputs && !(x->inputs = nr_take(r, inputs * sizeof(*x->inputs)))))
        return nr_fail(error, capacity, "malformed receipt inputs", *i + 1);
    for (size_t k = 0; k < inputs; k++) {
        char *v[8];
        native_replay_input *in = &x->inputs[k];
        if (*i + 1 >= line_count || nr_split(lines[++*i], v, 8) != 7 || strcmp(v[0], "I") || !strcmp(v[1], "-"))
            return nr_fail(error, capacity, "malformed receipt input", *i + 1);
        in->path = nr_text(r, v[1]); in->length = (size_t)_strtoui64(v[2], NULL, 10);
        in->covered = (size_t)_strtoui64(v[3], NULL, 10); in->failed = (int)strtol(v[4], NULL, 10);
        if (!(in->body = nr_bytes(r, v[5], in->length ? in->length : 1, 1)) || !(in->seen = nr_bytes(r, v[6], in->length ? in->length : 1, 1)))
            return nr_fail(error, capacity, "malformed receipt input", *i + 1);
    }
    x->input_count = inputs;
    return 1;
}
/* The replayed object at a (resource manager, slot), or NULL. */
static unsigned char *native_replay_resource(const native_replay *r, long long manager, long long slot)
{
    unsigned char *at = NULL, *array = NULL, *object = NULL;
    int32_t count = 0;
    if (!r->resources || manager < 0 || slot < 0) return NULL;
    memcpy(&at, (const void *)r->resources, 8);
    for (long long m = 0; at && m < manager; m++) memcpy(&at, at + 0x18, 8);
    if (!at) return NULL;
    memcpy(&array, at + 0x20, 8); memcpy(&count, at + 0x28, 4);
    if (slot >= count || !array) return NULL;
    memcpy(&object, array + slot * 8, 8); return object;
}
/* The observed receipt header over the replayed object: slots that held the
 * object's own identity take the replayed object's values. */
static unsigned char *native_replay_receipt_header(const native_replay *r, const native_replay_receipt *x, unsigned char out[0x90])
{
    unsigned char *object = native_replay_resource(r, x->manager, x->slot);
    memcpy(out, x->header, 0x90);
    if (!object) return NULL;
    for (int k = 0; k < 3; k++) if (x->identity[k] == 'o') memcpy(out + k * 8, object + k * 8, 8);
    return object;
}

/* 1 with r filled (free with native_replay_free), 0 with an error. */
static int native_replay_load(native_replay *r, const char *path, char *error, size_t capacity)
{
    FILE *file = NULL;
    char *text = NULL, **lines = NULL, *cell[12];
    size_t length = 0, line_count = 0, class_table = 0, enum_table = 0, enum_pointer_table = 0;
    long size;
    memset(r, 0, sizeof(*r));
    if (fopen_s(&file, path, "rb") || !file) return nr_fail(error, capacity, "snapshot replay file is unavailable", 0);
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) ||
        !(text = malloc((size_t)size + 1)) || fread(text, 1, (size_t)size, file) != (size_t)size) {
        fclose(file); free(text); return nr_fail(error, capacity, "snapshot replay file is unreadable", 0);
    }
    fclose(file); length = (size_t)size; text[length] = 0;
    for (size_t i = 0; i < length; i++) if (text[i] == '\n') line_count++;
    if (!line_count || text[length - 1] != '\n' || !(lines = calloc(line_count, sizeof(*lines)))) {
        free(text); return nr_fail(error, capacity, "snapshot replay file is truncated", 0);
    }
    for (size_t i = 0, at = 0; i < line_count; i++) {
        lines[i] = text + at;
        while (text[at] != '\n') at++;
        text[at++] = 0;
    }
    r->size = length * 4 + line_count * 512 + ((size_t)16 << 20);
    if (!(r->arena = calloc(1, r->size))) goto failed;
    if (nr_split(lines[0], cell, 12) != 6 || strcmp(cell[0], "H")) { nr_fail(error, capacity, "missing header", 1); goto failed; }
    snprintf(r->renderer, sizeof(r->renderer), "%s", cell[1]); snprintf(r->pin, sizeof(r->pin), "%s", cell[2]);
    class_table = (size_t)strtoull(cell[3], NULL, 10); enum_table = (size_t)strtoull(cell[4], NULL, 10);
    enum_pointer_table = (size_t)strtoull(cell[5], NULL, 10);
    {
        unsigned char *root = nr_take(r, 0xb0), *container = nr_take(r, 0x30);
        unsigned char *classes = nr_take(r, (class_table + 1) * 0x38), *objects = nr_take(r, (class_table + 1) * 16);
        unsigned char *pointers = nr_take(r, (class_table + 1) * 16), *enums = nr_take(r, (enum_table + 1) * 0x18);
        unsigned char *enum_readers = nr_take(r, (enum_table + 1) * 16), *enum_pointers = nr_take(r, (enum_table + 1) * 16);
        char *empty = nr_text(r, "");
        unsigned char **managers = NULL; size_t manager_count = 0, manager_capacity = 0;
        unsigned char *resource_first = NULL, *resource_last = NULL;
        long long *parents = NULL;
        if (!root || !container || !classes || !objects || !pointers || !enums || !enum_readers || !enum_pointers || !empty) {
            nr_fail(error, capacity, "arena exhausted", 0); goto failed;
        }
        nr_put(root, 0, (uintptr_t)container); nr_put(container, 0x20, (uintptr_t)classes); nr_put(container, 0x10, (uintptr_t)enums);
        nr_put(root, 0x88, (uintptr_t)objects); nr_put32(root, 0x90, (int32_t)class_table);
        nr_put(root, 0xa0, (uintptr_t)pointers); nr_put32(root, 0xa8, (int32_t)class_table);
        nr_put(root, 0x58, (uintptr_t)enum_readers); nr_put32(root, 0x60, (int32_t)enum_table);
        nr_put(root, 0x70, (uintptr_t)enum_pointers); nr_put32(root, 0x78, (int32_t)enum_pointer_table);
        size_t receipt_lines = 0;
        for (size_t i = 1; i < line_count; i++) if (!strncmp(lines[i], "S\t", 2)) receipt_lines++;
        if (receipt_lines && !(r->receipts = nr_take(r, receipt_lines * sizeof(*r->receipts)))) {
            nr_fail(error, capacity, "arena exhausted", 0); goto failed;
        }
        for (size_t i = 1; i < line_count; i++) {
            size_t n;
            if (!strncmp(lines[i], "S\t", 2)) {
                if (!r->resource_managers || !nr_receipt(r, lines, line_count, &i, &r->receipts[r->receipt_count], error, capacity)) {
                    if (!r->resource_managers) nr_fail(error, capacity, "receipt before the resource lists", i + 1);
                    free(managers); free(parents); goto failed;
                }
                r->receipt_count++; continue;
            }
            n = nr_split(lines[i], cell, 12);
            if (!strcmp(cell[0], "C") && n == 10) {
                unsigned char *record = classes + r->classes * 0x38, *element = NULL, *fields = NULL;
                size_t count = 0;
                if (r->classes + 1 >= class_table) { nr_fail(error, capacity, "class table overflow", i + 1); goto failed; }
                while (i + 1 + count < line_count && !strncmp(lines[i + 1 + count], "F\t", 2)) count++;
                if (cell[4][0] == '1' && (!(element = nr_take(r, 16)) || (nr_put(element, 0, (uintptr_t)nr_text(r, cell[5])),
                    nr_put(element, 8, (uintptr_t)nr_text(r, cell[6])), 0))) { nr_fail(error, capacity, "arena exhausted", i + 1); goto failed; }
                if (cell[7][0] == '1' && !(fields = nr_take(r, (count + 1) * 0x48))) { nr_fail(error, capacity, "arena exhausted", i + 1); goto failed; }
                if (cell[7][0] != '1' && count) { nr_fail(error, capacity, "fields listed for a record without fields", i + 1); goto failed; }
                nr_put(record, 0, (uintptr_t)nr_text(r, cell[1])); nr_put(record, 8, (uintptr_t)nr_text(r, cell[2]));
                nr_put32(record, 0x10, (int32_t)strtol(cell[3], NULL, 10));
                nr_put(record, 0x18, (uintptr_t)element); nr_put(record, 0x20, (uintptr_t)fields);
                nr_put(objects, r->classes * 16 + 8, nr_reader(cell[8])); nr_put(pointers, r->classes * 16 + 8, nr_reader(cell[9]));
                for (size_t k = 0; k < count; k++) {
                    char *f[12];
                    unsigned char *field = fields + k * 0x48, *code = NULL;
                    i++;
                    if (nr_split(lines[i], f, 12) != 8) { nr_fail(error, capacity, "malformed field", i + 1); goto failed; }
                    nr_put(field, 0, (uintptr_t)nr_text(r, f[1])); nr_put(field, 8, (uintptr_t)nr_text(r, f[2]));
                    nr_put(field, 0x10, (uintptr_t)nr_text(r, f[3]));
                    nr_put32(field, 0x18, (int32_t)strtol(f[4], NULL, 10)); nr_put32(field, 0x1c, (int32_t)strtol(f[5], NULL, 10));
                    if (f[6][0]) {
                        if (strlen(f[7]) != 64 || !(code = nr_take(r, 32))) { nr_fail(error, capacity, "malformed setter", i + 1); goto failed; }
                        for (size_t b = 0; b < 32; b++) {
                            unsigned value;
                            if (sscanf_s(f[7] + b * 2, "%2x", &value) != 1) { nr_fail(error, capacity, "malformed setter", i + 1); goto failed; }
                            code[b] = (unsigned char)value;
                        }
                        nr_put(field, 0x38, (uintptr_t)code);
                    }
                    r->fields++;
                }
                if (fields) nr_put(fields, count * 0x48 + 0x10, (uintptr_t)empty);
                r->classes++;
            } else if (!strcmp(cell[0], "E") && n == 5) {
                unsigned char *record = enums + r->enums * 0x18, *members = NULL;
                size_t count = 0;
                if (r->enums + 1 >= enum_table) { nr_fail(error, capacity, "enum table overflow", i + 1); goto failed; }
                while (i + 1 + count < line_count && !strncmp(lines[i + 1 + count], "V\t", 2)) count++;
                if (cell[2][0] == '1' && !(members = nr_take(r, (count + 1) * 0x10))) { nr_fail(error, capacity, "arena exhausted", i + 1); goto failed; }
                nr_put(record, 0, (uintptr_t)nr_text(r, cell[1])); nr_put(record, 0x10, (uintptr_t)members);
                nr_put(enum_readers, r->enums * 16 + 8, nr_reader(cell[3])); nr_put(enum_pointers, r->enums * 16 + 8, nr_reader(cell[4]));
                for (size_t k = 0; k < count; k++) {
                    char *v[4];
                    i++;
                    if (nr_split(lines[i], v, 4) != 3 || !members) { nr_fail(error, capacity, "malformed enum member", i + 1); goto failed; }
                    nr_put(members, k * 0x10, (uintptr_t)nr_text(r, v[1]));
                    nr_put32(members, k * 0x10 + 8, (int32_t)strtoul(v[2], NULL, 10));
                }
                r->enums++;
            } else if (!strcmp(cell[0], "M") && n == 4) {
                unsigned char *manager = nr_take(r, 0x90);
                if (manager_count == manager_capacity) {
                    size_t next = manager_capacity ? manager_capacity * 2 : 64;
                    unsigned char **grown = realloc(managers, next * sizeof(*grown));
                    long long *parents_grown = realloc(parents, next * sizeof(*parents_grown));
                    if (grown) managers = grown;
                    if (parents_grown) parents = parents_grown;
                    if (!grown || !parents_grown) { nr_fail(error, capacity, "out of memory", i + 1); goto failed; }
                    manager_capacity = next;
                }
                if (!manager) { nr_fail(error, capacity, "arena exhausted", i + 1); goto failed; }
                nr_put(manager, 8, (uintptr_t)nr_text(r, cell[1])); nr_put(manager, 0x10, (uintptr_t)nr_text(r, cell[2]));
                parents[manager_count] = cell[3][0] ? (long long)strtoll(cell[3], NULL, 10) : -1;
                managers[manager_count++] = manager;
                if (!nr_entries(r, lines, line_count, &i, "L", manager, 0, error, capacity)) { free(managers); free(parents); goto failed; }
            } else if (!strcmp(cell[0], "R") && n == 2) {
                unsigned char *manager = nr_take(r, 0x30);
                if (!manager) { free(managers); free(parents); nr_fail(error, capacity, "arena exhausted", i + 1); goto failed; }
                nr_put(manager, 8, (uintptr_t)nr_text(r, cell[1]));
                if (resource_last) nr_put(resource_last, 0x18, (uintptr_t)manager); else resource_first = manager;
                resource_last = manager; r->resource_managers++;
                if (!nr_entries(r, lines, line_count, &i, "Q", manager, 1, error, capacity)) { free(managers); free(parents); goto failed; }
            } else {
                free(managers); free(parents);
                nr_fail(error, capacity, "unknown or malformed line", i + 1); goto failed;
            }
        }
        nr_put(classes, r->classes * 0x38, (uintptr_t)empty);
        nr_put(enums, r->enums * 0x18, (uintptr_t)empty);
        if (r->classes + 1 != class_table || r->enums + 1 != enum_table || !manager_count || !resource_first) {
            free(managers); free(parents); nr_fail(error, capacity, "snapshot tables are incomplete", 0); goto failed;
        }
        {
            unsigned char *head = nr_take(r, 8);
            if (!head) { free(managers); free(parents); nr_fail(error, capacity, "arena exhausted", 0); goto failed; }
            nr_put(head, 0, (uintptr_t)resource_first); r->resources = (uintptr_t)head;
        }
        {
            unsigned char *registry = nr_take(r, 0x18), *array = nr_take(r, manager_count * 8);
            if (!registry || !array) { free(managers); free(parents); nr_fail(error, capacity, "arena exhausted", 0); goto failed; }
            nr_put(registry, 8, (uintptr_t)array); nr_put32(registry, 0x10, (int32_t)manager_count);
            for (size_t m = 0; m < manager_count; m++) {
                if (parents[m] >= (long long)manager_count) { free(managers); free(parents); nr_fail(error, capacity, "bad manager parent", 0); goto failed; }
                nr_put(array, m * 8, (uintptr_t)managers[m]);
                nr_put(managers[m], 0x88, parents[m] >= 0 ? (uintptr_t)managers[parents[m]] : 0);
            }
            r->registry = (uintptr_t)registry; r->managers = manager_count;
        }
        free(managers); free(parents);
        r->reflection = (uintptr_t)root;
    }
    free(lines); free(text); return 1;
failed:
    free(lines); free(text); free(r->arena); memset(r, 0, sizeof(*r)); return 0;
}
static void native_replay_free(native_replay *r) { free(r->arena); memset(r, 0, sizeof(*r)); }
#endif
