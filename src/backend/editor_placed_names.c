#include "editor_placed_names.h"
#include "safe_read.h"
#include <stdlib.h>
#include <string.h>

const char *sh_editor_module_name(const uint8_t *records, int index)
{
    void *module = NULL, *name = NULL;
    if (!sh_read_ptr(records + (size_t)index * MOD_STRIDE, &module) || !module ||
        !sh_read_ptr((const uint8_t *)module + MOD_NAME_OFF, &name)) return NULL;
    return (const char *)name;
}

/* Native strings are borrowed. Keep one scratch buffer for the entire walk and emit only complete values. */
typedef struct placed_text {
    char *p;
    size_t cap;
} placed_text;

static int placed_copy(placed_text *text, const char *source)
{
    size_t n;
    __try { n = strlen(source) + 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (n > text->cap) {
        char *p = (char *)realloc(text->p, n);
        if (!p) return 0;
        text->p = p;
        text->cap = n;
    }
    __try { memcpy(text->p, source, n); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

int sh_editor_placed_names(const uint8_t *map, sh_modui_name_fn emit, void *context)
{
    void *array = NULL, *modules = NULL;
    uint32_t count = 0, id;
    int read = 0, mod_count = 0, i;
    placed_text text = {0};
    if (!map || !emit || !sh_read_ptr(map + ARR_ENT_ARRAY_OFF, &array) ||
        !sh_read_u32(map + ARR_ENT_COUNT_OFF, &count) || count > ENT_COUNT_CAP || (count && !array) ||
        !sh_read_i32(map + LM_INSTANCES_CNT_OFF, &mod_count) || mod_count < 0 || mod_count > 8192 ||
        (mod_count && (!sh_read_ptr(map + LM_MODTABLE_OFF, &modules) || !modules))) return -1;
    for (i = 0; i < mod_count; i++) {
        const uint8_t *record = (const uint8_t *)modules + (size_t)i * MOD_STRIDE;
        const char *name;
        uint8_t present;
        if (!sh_read_u8(record + 0x30, &present)) goto failed;
        if (!present) continue;
        name = sh_editor_module_name((const uint8_t *)modules, i);
        if (!name || !placed_copy(&text, name)) goto failed;
        emit(context, text.p);
    }
    for (id = 0; id < count; id++) {
        void *ent = NULL, *valid = NULL, *def = NULL, *inherit = NULL, *blob = NULL;
        char *p;
        if (!sh_read_ptr((const uint8_t *)array + (size_t)id * sizeof ent, &ent)) goto failed;
        if (!ent) continue;
        if (!sh_read_ptr((const uint8_t *)ent + ENT_VALID_OFF, &valid)) goto failed;
        if (!valid) continue;
        if (!sh_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &def)) goto failed;
        if (!def) continue;
        if (!sh_read_ptr((const uint8_t *)def + DEFSUB_INHERIT_OFF, &inherit)) goto failed;
        if (inherit) {
            if (!placed_copy(&text, (const char *)inherit)) goto failed;
            emit(context, text.p);
        }
        read++;
        if (!sh_read_ptr((const uint8_t *)def + DECL_BLOB_B_OFF, &blob)) goto failed;
        if (!blob) continue;
        if (!placed_copy(&text, (const char *)blob)) goto failed;
        for (p = text.p; (p = strchr(p, '"')) != NULL;) {
            char *q = ++p;
            while (*q && *q != '"') q += (q[0] == '\\' && q[1]) ? 2 : 1;
            if (!*q) break;
            *q = '\0';
            if (*p) emit(context, p);
            p = q + 1;
        }
    }
    free(text.p);
    return read;

failed:
    free(text.p);
    return -1;
}
