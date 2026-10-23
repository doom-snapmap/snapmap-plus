#include "virtual_material.h"
#include "decl_native_lex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int vm_fail(char *error, size_t capacity, const char *reason)
{
    if (error && capacity) snprintf(error, capacity, "virtual material table: %s", reason);
    return 0;
}

int sh_virtual_material_name_copy(const char *text, size_t length, char *out, size_t capacity)
{
    if (out && capacity) out[0] = 0;
    if (!text || !length || !out || length >= capacity) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32 || c >= 127 || c == '"') { out[0] = 0; return 0; }
        out[i] = c == '\\' ? '/' : (c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : (char)c);
    }
    out[length] = 0; return 1;
}
char *sh_virtual_material_name(const char *text, size_t length)
{
    char *name;
    if (!text || !length || length == SIZE_MAX || !(name = malloc(length + 1))) return NULL;
    if (!sh_virtual_material_name_copy(text, length, name, length + 1)) { free(name); return NULL; }
    return name;
}

static int vm_integer(const sh_decl_tokens *tokens, size_t *index, int32_t *out)
{
    size_t i = *index;
    uint32_t value = 0, limit = INT32_MAX;
    int negative = i < tokens->count && sh_decl_token_is(tokens, i, "-");
    const sh_decl_token *token;
    if (negative) { i++; limit++; }
    if (i == tokens->count || tokens->items[i].kind != SH_DECL_TOKEN_NUMBER) return 0;
    token = &tokens->items[i];
    for (size_t p = token->begin; p < token->end; p++) {
        unsigned char c = (unsigned char)tokens->source.text[p];
        if (c < '0' || c > '9' || value > (limit - (c - '0')) / 10) return 0;
        value = value * 10 + c - '0';
    }
    *out = negative ? (int32_t)(-(int64_t)value) : (int32_t)value;
    *index = i + 1; return 1;
}

static int vm_compare(const void *a, const void *b)
{
    return strcmp(((const sh_virtual_material_record *)a)->name,
        ((const sh_virtual_material_record *)b)->name);
}

static int vm_equal(const sh_virtual_material_record *a, const sh_virtual_material_record *b)
{
    return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height &&
        a->flags == b->flags && a->timestamp == b->timestamp && a->checksum == b->checksum;
}

static int vm_valid(const sh_virtual_material_record *r)
{
    return r->x >= 0 && r->y >= 0 && r->width > 0 && r->height > 0 &&
        r->x <= INT32_MAX - r->width && r->y <= INT32_MAX - r->height;
}

void sh_virtual_material_free(sh_virtual_material_table *table)
{
    if (!table) return;
    for (size_t i = 0; i < table->count; i++) free(table->records[i].name);
    free(table->records); memset(table, 0, sizeof(*table));
}

static int vm_finish(sh_virtual_material_table *table, char *error, size_t capacity)
{
    size_t used = 0;
    if (table->count > 1) qsort(table->records, table->count, sizeof(*table->records), vm_compare);
    /* Validate before compacting, keeping cleanup valid on every failure. */
    for (size_t i = 1; i < table->count; i++) {
        if (!strcmp(table->records[i - 1].name, table->records[i].name) &&
            !vm_equal(&table->records[i - 1], &table->records[i]))
            return vm_fail(error, capacity, "one material name has different atlas records");
    }
    for (size_t i = 0; i < table->count; i++) {
        if (used && !strcmp(table->records[used - 1].name, table->records[i].name))
            free(table->records[i].name);
        else table->records[used++] = table->records[i];
    }
    table->count = used; return 1;
}

int sh_virtual_material_read(sh_decl_source source, sh_virtual_material_table *out,
    char *error, size_t capacity)
{
    sh_decl_tokens tokens = {0};
    sh_virtual_material_table table = {0};
    size_t index = 0;
    int32_t version, count;
    int ok = 0;
    if (error && capacity) error[0] = 0;
    if (!out) return vm_fail(error, capacity, "missing output");
    sh_virtual_material_free(out);
    if (!sh_decl_native_lex(source, &tokens, error, capacity)) return 0;
    if (!vm_integer(&tokens, &index, &version) || version != 26 ||
        !vm_integer(&tokens, &index, &count) || count < 0) {
        vm_fail(error, capacity, "invalid retail table header"); goto done;
    }
    /* Each row needs seven integers and one name, before any allocation. */
    if ((size_t)count > (tokens.count - index) / 8 ||
        (size_t)count > SIZE_MAX / sizeof(*table.records)) {
        vm_fail(error, capacity, "declared row count exceeds available data"); goto done;
    }
    if (count && !(table.records = calloc((size_t)count, sizeof(*table.records)))) goto allocation;
    for (size_t i = 0; i < (size_t)count; i++) {
        sh_virtual_material_record *r = &table.records[i];
        const sh_decl_token *name;
        table.count = i + 1;
        if (!vm_integer(&tokens, &index, &r->x) || !vm_integer(&tokens, &index, &r->y) ||
            !vm_integer(&tokens, &index, &r->width) || !vm_integer(&tokens, &index, &r->height) ||
            !vm_integer(&tokens, &index, &r->flags) || !vm_integer(&tokens, &index, &r->timestamp) ||
            !vm_integer(&tokens, &index, &r->checksum) || index >= tokens.count || !vm_valid(r)) {
            vm_fail(error, capacity, "incomplete or invalid atlas record"); goto done;
        }
        name = &tokens.items[index++];
        if ((name->kind != SH_DECL_TOKEN_STRING && name->kind != SH_DECL_TOKEN_NAME &&
             name->kind != SH_DECL_TOKEN_NUMBER) ||
            !(r->name = sh_virtual_material_name(source.text + name->begin + (name->kind == SH_DECL_TOKEN_STRING),
                name->end - name->begin - (name->kind == SH_DECL_TOKEN_STRING ? 2 : 0)))) {
            vm_fail(error, capacity, "invalid material name"); goto done;
        }
    }
    if (index != tokens.count) { vm_fail(error, capacity, "unexpected trailing table data"); goto done; }
    if (!vm_finish(&table, error, capacity)) goto done;
    *out = table; memset(&table, 0, sizeof(table)); ok = 1; goto done;
allocation:
    vm_fail(error, capacity, "allocation failed");
done:
    sh_decl_tokens_free(&tokens); sh_virtual_material_free(&table); return ok;
}

const sh_virtual_material_record *sh_virtual_material_find(
    const sh_virtual_material_table *table, const char *name)
{
    sh_virtual_material_record key = {0};
    const sh_virtual_material_record *result;
    if (!table || !table->count || !name || !(key.name = sh_virtual_material_name(name, strlen(name)))) return NULL;
    result = bsearch(&key, table->records, table->count, sizeof(*table->records), vm_compare);
    free(key.name); return result;
}

int sh_virtual_material_mapping(const sh_virtual_material_record *record,
    int32_t extent, float mapping[4])
{
    if (!mapping) return 0;
    memset(mapping, 0, 4 * sizeof(*mapping));
    if (!record || !vm_valid(record) || extent <= 0 ||
        record->x > extent - record->width || record->y > extent - record->height) return 0;
    mapping[0] = (float)record->width / (float)extent;
    mapping[1] = (float)record->height / (float)extent;
    mapping[2] = (float)record->x / (float)extent;
    mapping[3] = (float)record->y / (float)extent;
    return 1;
}

char *sh_virtual_material_project(const sh_virtual_material_binding *bindings,
    size_t count, size_t *length, char *error, size_t capacity)
{
    sh_virtual_material_table table = {0};
    char *body = NULL;
    size_t need = 64, used;
    if (length) *length = 0;
    if (error && capacity) error[0] = 0;
    if ((count && !bindings) || count > INT32_MAX || count > SIZE_MAX / sizeof(*table.records)) {
        vm_fail(error, capacity, "invalid binding count"); return NULL;
    }
    if (count && !(table.records = calloc(count, sizeof(*table.records)))) goto allocation;
    for (size_t i = 0; i < count; i++) {
        if (!bindings[i].record || !bindings[i].name || !vm_valid(bindings[i].record)) {
            vm_fail(error, capacity, "invalid binding record"); goto done;
        }
        table.records[i] = *bindings[i].record;
        table.records[i].name = sh_virtual_material_name(bindings[i].name, strlen(bindings[i].name));
        table.count = i + 1;
        if (!table.records[i].name) { vm_fail(error, capacity, "invalid binding name"); goto done; }
    }
    if (!vm_finish(&table, error, capacity)) goto done;
    for (size_t i = 0; i < table.count; i++) {
        size_t n = strlen(table.records[i].name);
        if (n > SIZE_MAX - 128 || need > SIZE_MAX - n - 128) goto allocation;
        need += n + 128;
    }
    if (!(body = malloc(need))) goto allocation;
    used = (size_t)snprintf(body, need, "26\n%zu\n", table.count);
    for (size_t i = 0; i < table.count; i++) {
        const sh_virtual_material_record *r = &table.records[i];
        used += (size_t)snprintf(body + used, need - used, "%d %d %d %d %d %d %d \"%s\"\n",
            r->x, r->y, r->width, r->height, r->flags, r->timestamp, r->checksum, r->name);
    }
    if (length) *length = used;
    goto done;
allocation:
    vm_fail(error, capacity, "allocation failed");
done:
    sh_virtual_material_free(&table); return body;
}
