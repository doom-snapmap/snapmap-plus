/* Reflection queries for console output, class/inherit validation, and asset metadata.
 * Dependencies resolve at installation. Guarded, bounded record walks return
 * unavailable or partial results when engine data cannot be read. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "typeinfo.h"
#include "commands.h"
#include "clipboard.h"
#include "backend_log.h"
#include "engine_globals.h"
#include "class_universe.h"

/* Engine call contracts. */

/* Decode the decl-manager accessor from a signed call site. Its own lazy-init
 * prologue is shared by other functions, so a prologue-only pattern is ambiguous. */
typedef void *(*declmgr_getter_fn)(void);
#define DECLMGR_ACCESSOR_KNOWN_RVA  0x17F7030u   /* Pinned Vulkan audit reference only; resolved through a signed call site. */

/* Decl-manager vtable +0x80 returns its reflection context. */
#define VSLOT_REFLECT_ACCESSOR  0x80

/* Three-argument type lookup returns a record, despite a void decompiler label.
 * Caller 0x9C79D0 consumes the result; NULL means the type was not found. */
typedef void *(*find_typeinfo_fn)(void *reflect, const char *name, void *scope);

typedef void *(*find_enum_fn)(void *reflect, const char *name);

/* Reflection field layout. Array@record+0x20, stride 0x48, and name@field+0x10
 * were checked against engine caller 0x9C79D0. Remaining fields came from the
 * original command metadata and require rechecking when porting builds. */
#define REC_FIELDS_OFF      0x20    /* type record -> field array */
#define REC_SUPER_OFF       0x08    /* superclass name; original-command evidence */
#define FIELD_STRIDE        0x48    /* field-record stride */
#define FIELD_NAME_OFF      0x10    /* field name; NULL or empty terminates */
#define FIELD_OFFSET_OFF    0x18    /* field offset; original-command evidence */
#define FIELD_SIZE_OFF      0x1c    /* field size; original-command evidence */
#define FIELD_VARTYPE_OFF   0x00    /* primary type string */
#define FIELD_VAROPS_OFF    0x08    /* pointer/array qualifier */
#define FIELD_COMMENT_OFF   0x28    /* field comment; original-command evidence */

/* Enum array/stride/member fields follow engine caller 0x440230.
 * The enum record name offset has original-command evidence only. */
#define ENUM_NAME_OFF       0x00    /* enum name; original-command evidence */
#define ENUM_MEMBERS_OFF    0x10    /* member array */
#define ENUM_MEMBER_STRIDE  0x10    /* enum-member stride */
#define EMEMBER_NAME_OFF    0x00    /* member name; NULL terminates */
#define EMEMBER_VALUE_OFF   0x08    /* unsigned member value */

/* Bound unterminated field and enum records. */
#define TI_WALK_CAP   4096u

/* Collect one bounded dump for both console output and clipboard copy. */
#define TI_DUMP_CAP   0x4000

/* Cached dependencies. */


typedef void *(*decl_find_fn)(void *ctx, const char *name);
typedef int   (*dim_fn)(void *material);

static const uint8_t   *g_doom_base    = NULL;
static find_typeinfo_fn g_find_type    = NULL;
static find_enum_fn     g_find_enum    = NULL;
static volatile LONG    g_installed    = 0;

/* Unresolved dependencies stay NULL; consumers decline rather than use raw RVAs. */
static declmgr_getter_fn g_declmgr_getter   = NULL;   /* glb "declmgr_accessor" (a CODE address) */
static const uint8_t    *g_validator_mgr    = NULL;   /* glb "validator_manager" (slot; deref lazily) */
static void             *g_resource_mgr_ctx = NULL;   /* glb "resource_manager_ctx" (the object itself) */
static void             *g_material_mgr_ctx = NULL;   /* glb "material_manager_ctx" (the object itself) */
static const uint8_t    *g_type_container   = NULL;   /* glb "type_container" (the reflection container P) */
static decl_find_fn      g_decl_find        = NULL;   /* sig "DeclPureFind" */
static dim_fn            g_material_width   = NULL;   /* sig "MaterialWidth" */
static dim_fn            g_material_height  = NULL;   /* sig "MaterialHeight" */

/* Shared guarded accessor for reflection, apply, and event-manager consumers. */
void *sh_typeinfo_get_declmgr(void)
{
    if (!g_declmgr_getter) return NULL;
    __try {
        return g_declmgr_getter();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* Reach reflection through declMgr vtable +0x80; return NULL on failure. */
static void *ti_get_reflect(void)
{
    void *declmgr = sh_typeinfo_get_declmgr();
    if (!declmgr) return NULL;
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)declmgr;
        if (!vtbl) return NULL;
        typedef void *(*reflect_fn)(void *self);
        reflect_fn fn = *(reflect_fn const *)(vtbl + VSLOT_REFLECT_ACCESSOR);
        if (!fn) return NULL;
        return fn(declmgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* Enumerate decl-type instance names through reflection's silent enum lookup.
 * GetDeclsOfType serves asset classes and logs errors for these names. Pack
 * NUL-separated strings with a double-NUL terminator; preserve partial output. */
int sh_typeinfo_enum_decls_of_type(const char *declType, char *out_buf, int cap, int *out_count)
{
    if (out_count) *out_count = 0;
    if (cap > 0 && out_buf) out_buf[0] = '\0';
    if (!declType || !declType[0] || !out_buf || cap <= 1 || !g_find_enum) return 0;

    void *reflect = ti_get_reflect();
    if (!reflect) return 0;
    void *node = NULL;
    __try { node = g_find_enum(reflect, declType); }
    __except (EXCEPTION_EXECUTE_HANDLER) { node = NULL; }
    if (!node) return 0;

    int written = 0, names = 0;
    __try {
        void **list = *(void ***)((const uint8_t *)node + 0x10);
        for (uint32_t i = 0; list && i < TI_WALK_CAP; i++) {
            const char *nm = (const char *)list[(size_t)i * 2];
            if (!nm) break;
            int nlen = (int)strlen(nm);
            if (nlen <= 0 || nlen > 250) continue;
            if (written + nlen + 1 > cap - 1) break;
            memcpy(out_buf + written, nm, (size_t)nlen);
            out_buf[written + nlen] = '\0';
            written += nlen + 1;
            names++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* keep whatever we copied before the fault */ }

    out_buf[written] = '\0';   /* double-NUL end marker */
    if (out_count) *out_count = names;
    return names > 0 ? 1 : 0;
}

/* FindTypeInfoByName(reflect, name, NULL) -> rec*, SEH-guarded. NULL on any fault / missing sig. */
static void *ti_find_type(void *reflect, const char *name)
{
    if (!g_find_type || !reflect || !name) return NULL;
    __try {
        return g_find_type(reflect, name, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* FindEnumByName(reflect, name) -> enumRec*, SEH-guarded. NULL on any fault / missing sig. */
static void *ti_find_enum(void *reflect, const char *name)
{
    if (!g_find_enum || !reflect || !name) return NULL;
    __try {
        return g_find_enum(reflect, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* SEH-guarded scalar/pointer reads at record+offset. NULL/0 on any fault. */
static const char *ti_read_cstr(const void *base, size_t off)
{
    __try { return *(const char * const *)((const uint8_t *)base + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static void *ti_read_ptr(const void *base, size_t off)
{
    __try { return *(void * const *)((const uint8_t *)base + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static int ti_read_u32(const void *base, size_t off, uint32_t *out)
{
    __try { *out = *(const uint32_t *)((const uint8_t *)base + off); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Walk superclass names, matching the decl validator. Reject definite mismatch
 * before reparse: its Error(6) is caught inside the engine, beyond frame recovery.
 * Return 1 for a match, 0 for mismatch/unknown type, or -1 when reflection is
 * unavailable; callers must not reject solely on -1. Walk at most 64 ancestors. */
/* The validator uses manager vtable +0x240, which includes SnapMap classes
 * missed by declMgr +0x80. Resolve the manager slot through its global anchor;
 * fall back to the decl-manager context when unavailable. */
#define VALIDATOR_MGR_RVA         0x4DF9648u   /* Pinned Vulkan audit reference; the global anchor locates the slot. */
#define VSLOT_VALIDATOR_REFLECT   0x240
static void *ti_get_validator_reflect(void)
{
    if (!g_validator_mgr) return NULL;
    __try {
        void *mgr = *(void * const *)g_validator_mgr;
        if (!mgr) return NULL;
        const uint8_t *vtbl = *(const uint8_t * const *)mgr;
        if (!vtbl) return NULL;
        typedef void *(*reflect_fn)(void *self);
        reflect_fn fn = *(reflect_fn const *)(vtbl + VSLOT_VALIDATOR_REFLECT);
        if (!fn) return NULL;
        return fn(mgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

int sh_typeinfo_class_derives(const char *className, const char *baseName)
{
    if (!className || !baseName || !className[0] || !baseName[0]) return -1;
    void *reflect = ti_get_validator_reflect();      /* the validator's context (resolves SnapMap classes) */
    if (!reflect) reflect = ti_get_reflect();        /* fallback: the declMgr->[+0x80] reflect */
    if (!reflect) return -1;
    const char *cur = className;
    for (int i = 0; i < 64 && cur && cur[0]; i++) {
        if (strcmp(cur, baseName) == 0) return 1;
        void *rec = ti_find_type(reflect, cur);
        if (!rec) return 0;                                /* An unknown class cannot establish ancestry. */
        cur = ti_read_cstr(rec, REC_SUPER_OFF);
    }
    return 0;
}

/* Find the inherit declaration through the read-only hash lookup, then copy
 * className@+0x60. Avoid load-or-create: unresolved names can trigger engine
 * fatal paths there. Return NULL if unavailable; the compatibility guard then opens. */
#define RESOURCE_MGR_CTX_RVA   0x59BD8F0u   /* Pinned Vulkan audit reference from the validator call site. */
#define DECL_PURE_FIND_RVA     0x18017A0u   /* Pinned Vulkan audit reference; DeclPureFind resolves the function. */
#define DECL_CLASSNAME_OFF     0x60u        /* idDeclEntityDef.className char* (vtbl+0xb0 = return *(this+0x60)) */
const char *sh_typeinfo_inherit_base(const char *inheritName, char *buf, size_t cap)
{
    if (buf && cap) buf[0] = '\0';
    if (!inheritName || !inheritName[0] || !buf || cap < 2) return NULL;
    if (!g_resource_mgr_ctx || !g_decl_find) return NULL;
    __try {
        void *decl = g_decl_find(g_resource_mgr_ctx, inheritName);
        if (!decl) return NULL;
        const char *cn = *(const char * const *)((const uint8_t *)decl + DECL_CLASSNAME_OFF);
        if (!cn || !cn[0]) return NULL;
        lstrcpynA(buf, cn, (int)cap);
        return buf[0] ? buf : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = '\0'; return NULL; }
}

/* Resolved-text idStr@+0x130 has its data pointer at +0x140. */
#define DECL_RESOLVED_TEXT_OFF 0x140u
#define DECL_RESOLVED_TEXT_CAP (4u * 1024u * 1024u)

static int ti_word_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int ti_token_at(const char *text, size_t len, size_t at, const char *token)
{
    size_t n = strlen(token);
    if (at + n > len || _strnicmp(text + at, token, n) != 0) return 0;
    if (at && ti_word_char((unsigned char)text[at - 1])) return 0;
    if (at + n < len && ti_word_char((unsigned char)text[at + n])) return 0;
    return 1;
}

static int ti_model_from_resolved_text(const char *text, size_t len, char *out, size_t cap)
{
    size_t block_start = len, block_end = len;
    for (size_t i = 0; i < len; ++i) {
        if (!ti_token_at(text, len, i, "renderModelInfo")) continue;
        size_t p = i + strlen("renderModelInfo");
        while (p < len && text[p] != '{' && text[p] != '\n' && text[p] != '\r') ++p;
        if (p >= len || text[p] != '{') continue;
        block_start = p + 1;
        int depth = 1, quote = 0, escape = 0;
        for (++p; p < len; ++p) {
            unsigned char c = (unsigned char)text[p];
            if (quote) {
                if (escape) escape = 0;
                else if (c == '\\') escape = 1;
                else if (c == '"') quote = 0;
            } else if (c == '"') quote = 1;
            else if (c == '{') depth++;
            else if (c == '}' && --depth == 0) { block_end = p; break; }
        }
        if (block_end != len) break;
    }
    if (block_start >= block_end) return 0;

    for (size_t i = block_start; i < block_end; ++i) {
        if (!ti_token_at(text, block_end, i, "model")) continue;
        size_t p = i + 5u;
        while (p < block_end && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n')) ++p;
        if (p < block_end && text[p] == '=') {
            ++p;
            while (p < block_end && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n')) ++p;
        }
        int quoted = p < block_end && text[p] == '"';
        if (quoted) ++p;
        size_t start = p;
        while (p < block_end && ((quoted && text[p] != '"') ||
               (!quoted && text[p] != ' ' && text[p] != '\t' && text[p] != '\r' &&
                text[p] != '\n' && text[p] != ';' && text[p] != '}'))) ++p;
        size_t n = p - start;
        if (n > 0 && n < cap) {
            memcpy(out, text + start, n); out[n] = '\0';
            return 1;
        }
    }
    return 0;
}

int sh_typeinfo_inherit_model(const char *inheritName, char *buf, size_t cap)
{
    if (buf && cap) buf[0] = '\0';
    if (!inheritName || !inheritName[0] || !buf || cap < 2) return 0;
    if (!g_resource_mgr_ctx || !g_decl_find) return 0;
    __try {
        const uint8_t *decl = (const uint8_t *)g_decl_find(g_resource_mgr_ctx, inheritName);
        if (!decl) return 0;
        const char *text = *(const char * const *)(decl + DECL_RESOLVED_TEXT_OFF);
        if (!text) return 0;
        size_t len = 0;
        while (len < DECL_RESOLVED_TEXT_CAP && text[len]) ++len;
        if (len == 0 || len == DECL_RESOLVED_TEXT_CAP) return 0;
        return ti_model_from_resolved_text(text, len, buf, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = '\0'; return 0; }
}

/* Use the read-only declaration lookup with the material-manager context.
 * Shipped material names are available before rendering; this does not imply
 * their GPU images are resident. Dimension getters are best effort. */
/* Pinned Vulkan audit references; all three addresses resolve by signature. */
#define MATERIAL_MGR_CTX_RVA    0x59BD9D0u  /* material type-manager context from the material setter */
#define MATERIAL_WIDTH_FN_RVA   0xD75D40u   /* verified idMaterial width getter                       */
#define MATERIAL_HEIGHT_FN_RVA  0xD75B40u   /* verified idMaterial height getter                      */

/* Probe the virtual texture's resident minimum-LOD image without GPU calls.
 * Re-derive image extent fields from idImage_Vulkan_PC::Create (0xDADB70 on the
 * pinned Vulkan build): VkImageCreateInfo width/height/mips read +0x60/+0x64/+0x70.
 * SetSource (0xE11C50) supplies the _minlod controls type=0, format=0x13, mips=1;
 * mismatched controls invalidate the probe. Image+0xBD reports creation failure
 * and +0xE0 stores VkImage. File-loading fields +0x38/+0x3C are source-art
 * dimensions and must not substitute for the created image extent. */
#define MATERIAL_VTEX_OFF        0x170u  /* material -> idVirtualTexture* (0 if not virtual-textured) */
#define VTEX_MINLOD_IMAGE_OFF    0x3F0u  /* idVirtualTexture -> idImage* (always-resident low-res fallback) */
/* This material branch stores atlas x/y/w/h at +0x18/+0x1C/+0x20/+0x24.
 * +0x1C is atlas Y, not a page count; width conversion uses *128/120. */
#define VTEX_ATLAS_Y_OFF         0x1Cu   /* idVirtualTexture -> atlas Y coordinate (px) -- cross-check only */
#define VTEX_ATLAS_W_OFF         0x20u   /* idVirtualTexture -> atlas width (px); getter does *128/120 */
#define IMAGE_TYPE_OFF           0x54u   /* idImage -> image type; min-LOD control value: 0 (2D) */
#define IMAGE_FMT_OFF            0x58u   /* idImage -> engine format enum; min-LOD control value: 0x13 */
#define IMAGE_EXTENT_W_OFF       0x60u   /* idImage -> VkImageCreateInfo.extent.width  (authoritative) */
#define IMAGE_EXTENT_H_OFF       0x64u   /* idImage -> VkImageCreateInfo.extent.height (authoritative) */
#define IMAGE_MIPS_OFF           0x70u   /* idImage -> VkImageCreateInfo.mipLevels; min-LOD control value: 1 */
#define IMAGE_CREATEFAIL_OFF     0xBDu   /* idImage -> creation-failure byte SetSource tests (expect 0) */
#define IMAGE_VKIMAGE_OFF        0xE0u   /* idImage -> live VkImage handle (expect non-NULL if resident) */

int sh_typeinfo_find_material(const char *name, char *buf, size_t cap)
{
    if (buf && cap) buf[0] = '\0';
    if (!name || !name[0] || !buf || cap < 2) return 0;
    if (!g_material_mgr_ctx || !g_decl_find) return 0;
    __try {
        void *material = g_decl_find(g_material_mgr_ctx, name);
        if (!material) return 0;

        /* A missing dimension getter does not invalidate the material lookup. */
        int w = -1, h = -1;
        if (g_material_width && g_material_height) {
            __try {
                w = g_material_width(material);
                h = g_material_height(material);
            } __except (EXCEPTION_EXECUTE_HANDLER) { w = -1; h = -1; }
        }

        /* Optional structural diagnostics do not change the lookup result. */
        int has_vtex = 0, has_minlod = 0;
        int img_w = -1, img_h = -1, img_type = -1, img_fmt = -1, img_mips = -1;
        int pages = -1, fail = -1, resident = 0;
        __try {
            void *vtex = *(void * const *)((const uint8_t *)material + MATERIAL_VTEX_OFF);
            if (vtex) {
                has_vtex = 1;
                pages = *(const int *)((const uint8_t *)vtex + VTEX_ATLAS_Y_OFF);
                void *minlod = *(void * const *)((const uint8_t *)vtex + VTEX_MINLOD_IMAGE_OFF);
                if (minlod) {
                    const uint8_t *im = (const uint8_t *)minlod;
                    has_minlod = 1;
                    img_type = *(const int *)(im + IMAGE_TYPE_OFF);
                    img_fmt  = *(const int *)(im + IMAGE_FMT_OFF);
                    img_w    = *(const int *)(im + IMAGE_EXTENT_W_OFF);
                    img_h    = *(const int *)(im + IMAGE_EXTENT_H_OFF);
                    img_mips = *(const int *)(im + IMAGE_MIPS_OFF);
                    fail     = *(const uint8_t *)(im + IMAGE_CREATEFAIL_OFF);
                    resident = *(void * const *)(im + IMAGE_VKIMAGE_OFF) != NULL;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            has_vtex = 0; has_minlod = 0;
            img_w = -1; img_h = -1; img_type = -1; img_fmt = -1; img_mips = -1;
            pages = -1; fail = -1; resident = 0;
        }

        char tag[192] = "";
        if (has_vtex) {
            char pagetag[32] = "";
            if (pages > 0) _snprintf_s(pagetag, sizeof pagetag, _TRUNCATE, " atlasY=%d", pages);
            if (has_minlod) {
                /* Verify SetSource's literal controls before trusting image dimensions. */
                int ctl = (img_type == 0 && img_fmt == 0x13 && img_mips == 1);
                _snprintf_s(tag, sizeof tag, _TRUNCATE,
                            " [vt+minlod %dx%d ctl=%s(t%d/f%#x/m%d) fail=%d vk=%s%s]",
                            img_w, img_h, ctl ? "OK" : "BAD", img_type, (unsigned)img_fmt, img_mips,
                            fail, resident ? "yes" : "no", pagetag);
            } else {
                _snprintf_s(tag, sizeof tag, _TRUNCATE, " [vt, no minlod%s]", pagetag);
            }
        }

        if (w > 0 && h > 0)
            _snprintf_s(buf, cap, _TRUNCATE, "found (%dx%d)%s", w, h, tag);
        else
            _snprintf_s(buf, cap, _TRUNCATE, "found%s", tag);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (cap) buf[0] = '\0';
        return 0;
    }
}

/* Reflection types form a NULL-name-terminated array at container+0x20,
 * stride 0x38, name@+0 and superclass@+8. FindTypeInfoByName and the registry
 * builders use this same layout. */
#define REGISTRY_TYPEBASE_OFF   0x20      /* container P -> type-record array base (*(P+0x20)) */
#define REGISTRY_RECORD_STRIDE  0x38      /* per-record stride */
#define REGISTRY_NAME_OFF       0x00      /* record -> className char* (NULL name terminates the array) */
#define REGISTRY_SUPER_OFF      0x08      /* record -> superclass name char* ("" for a root) */
#define REGISTRY_WALK_CAP       65536u    /* Bound invalid or unterminated arrays. */
#define TYPE_CONTAINER_RVA      0x3082b10u /* Pinned Vulkan audit reference; runtime uses the signed type_container anchor. */

/* Prefer the reflection container. When the accessor is unavailable on the UI
 * thread, read the signed static container directly. Both yield array@P+0x20. */
static const uint8_t *ti_type_array_base(void)
{
    const uint8_t *P = NULL;
    void *reflect = ti_get_reflect();
    if (reflect) {
        __try { P = *(const uint8_t * const *)reflect; }
        __except (EXCEPTION_EXECUTE_HANDLER) { P = NULL; }
    }
    if (P == NULL) P = g_type_container;              /* UI-thread fallback (the signed fixed global) */
    if (P == NULL) return NULL;
    const uint8_t *B = NULL;
    __try { B = *(const uint8_t * const *)(P + REGISTRY_TYPEBASE_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { B = NULL; }
    return B;
}

int sh_typeinfo_collect_classnames(const char **out_names, int cap)
{
    if (!out_names || cap <= 0) return -1;
    const uint8_t *B = ti_type_array_base();
    if (!B) return -1;
    int n = 0;
    __try {
        for (uint32_t i = 0; i < REGISTRY_WALK_CAP && n < cap; i++) {
            const uint8_t *rec = B + (size_t)i * REGISTRY_RECORD_STRIDE;
            const char *name = *(const char * const *)(rec + REGISTRY_NAME_OFF);
            if (name == NULL) break;
            out_names[n++] = name;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* Keep the records collected before a memory fault. */
    }
    return n;
}

int sh_typeinfo_collect_records(sh_ti_record *out, int cap)
{
    if (!out || cap <= 0) return -1;
    const uint8_t *B = ti_type_array_base();
    if (!B) return -1;
    int n = 0;
    __try {
        for (uint32_t i = 0; i < REGISTRY_WALK_CAP && n < cap; i++) {
            const uint8_t *rec = B + (size_t)i * REGISTRY_RECORD_STRIDE;
            const char *name = *(const char * const *)(rec + REGISTRY_NAME_OFF);
            if (name == NULL) break;
            out[n].name  = name;
            out[n].super = *(const char * const *)(rec + REGISTRY_SUPER_OFF);
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

/* Read loaded entityDef names from resource_manager_ctx: array@+0x20,
 * count@+0x28, name@decl+0x08. Returned strings remain engine-owned. */
#define ENTITYDEF_MGR_ARRAY_OFF  0x20
#define ENTITYDEF_MGR_COUNT_OFF  0x28
#define DECL_NAME_OFF            0x08
int sh_typeinfo_collect_inherits(const char **out_names, int cap)
{
    if (!out_names || cap <= 0 || !g_resource_mgr_ctx) return -1;
    int n = 0;
    __try {
        const uint8_t *mgr = (const uint8_t *)g_resource_mgr_ctx;
        int count = *(const int *)(mgr + ENTITYDEF_MGR_COUNT_OFF);
        const uint8_t * const *arr = *(const uint8_t * const * const *)(mgr + ENTITYDEF_MGR_ARRAY_OFF);
        if (arr == NULL || count <= 0 || (uint32_t)count > REGISTRY_WALK_CAP) return -1;  /* stale-mgr guard */
        if (count > cap) count = cap;
        for (int i = 0; i < count; i++) {
            const uint8_t *decl = arr[i];
            if (decl == NULL) continue;
            const char *name = *(const char * const *)(decl + DECL_NAME_OFF);
            if (name != NULL && name[0] != '\0') out_names[n++] = name;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

/* Console handlers registered in commands.c. */

/* Print one reflected field's size and offset. */
void h_cs_fieldinfo(idCmdArgs *a)
{
    const char *type  = cmd_argv(a, 1);
    const char *field = cmd_argv(a, 2);
    if (type == NULL || field == NULL) {
        sh_printf("usage: cs_fieldinfo <type> <field>\n");
        return;
    }

    void *reflect = ti_get_reflect();
    if (reflect == NULL) {
        sh_printf("cs_fieldinfo: type manager unavailable.\n");
        return;
    }

    void *rec = ti_find_type(reflect, type);
    if (rec == NULL) {
        sh_printf("cs_fieldinfo: couldn't find type '%s'.\n", type);
        return;
    }

    const uint8_t *fields = (const uint8_t *)ti_read_ptr(rec, REC_FIELDS_OFF);
    if (fields == NULL) {
        sh_printf("cs_fieldinfo: type '%s' has no fields.\n", type);
        return;
    }

    for (uint32_t i = 0; i < TI_WALK_CAP; i++) {
        const uint8_t *f = fields + (size_t)i * FIELD_STRIDE;
        const char *name = ti_read_cstr(f, FIELD_NAME_OFF);
        if (name == NULL || name[0] == '\0') break;
        if (strcmp(name, field) == 0) {
            uint32_t size = 0, off = 0;
            if (!ti_read_u32(f, FIELD_SIZE_OFF, &size) || !ti_read_u32(f, FIELD_OFFSET_OFF, &off)) {
                sh_printf("cs_fieldinfo: field '%s' size/offset unreadable.\n", field);
                return;
            }
            sh_printf("Size %d, offset %d\n", size, off);
            return;
        }
    }
    sh_printf("cs_fieldinfo: type '%s' has no field '%s'.\n", type, field);
}

/* A tiny SEH-safe append into the fixed dump buffer (truncates on overflow; never overruns). */
static void ti_dump_append(char *buf, size_t cap, size_t *len, const char *s)
{
    if (s == NULL || *len >= cap - 1) return;
    size_t room = cap - 1 - *len;
    size_t add  = strlen(s);
    if (add > room) add = room;
    memcpy(buf + *len, s, add);
    *len += add;
    buf[*len] = '\0';
}

/* Console output truncates each sh_printf call near 1 KB. Emit bounded chunks,
 * preferring newline boundaries, while keeping the complete dump for the clipboard. */
static void ti_emit_long(const char *s)
{
    if (s == NULL) return;
    const size_t CHUNK = 1000;            /* < sh_printf's 1024 buf; "%s" expands the content 1:1 */
    char piece[1024];
    size_t n = strlen(s), i = 0;
    while (i < n) {
        size_t take = (n - i < CHUNK) ? (n - i) : CHUNK;
        if (take == CHUNK) {              /* break at the last newline in the window (keep lines whole) */
            size_t br = take;
            while (br > 0 && s[i + br - 1] != '\n') br--;
            if (br > 0) take = br;        /* no newline in the window -> emit the full CHUNK as-is */
        }
        memcpy(piece, s + i, take);
        piece[take] = '\0';
        sh_printf("%s", piece);
        i += take;
    }
}

/* Dump a class or enum and copy the same text to the clipboard. -v adds
 * per-field offsets and sizes; the default omits that diagnostic detail. */
void h_sh_type(idCmdArgs *a)
{
    const char *type = cmd_argv(a, 1);
    if (type == NULL) {
        sh_printf("No type provided!\n");
        return;
    }

    const char *vflag = cmd_argv(a, 2);
    int verbose = (vflag != NULL && _stricmp(vflag, "-v") == 0);

    void *reflect = ti_get_reflect();
    if (reflect == NULL) {
        sh_printf("sh_type: type manager unavailable.\n");
        return;
    }

    static char dump[TI_DUMP_CAP];
    size_t dlen = 0;
    dump[0] = '\0';
    char tmp[1024];

    void *rec = ti_find_type(reflect, type);
    if (rec != NULL) {
        /* Class fields. */
        const char *super = ti_read_cstr(rec, REC_SUPER_OFF);
        sh_printf("Inherits %s\n", (super && super[0]) ? super : "(none)");

        const char *cname = ti_read_cstr(rec, ENUM_NAME_OFF);
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "struct %s {\n", (cname && cname[0]) ? cname : type);
        ti_dump_append(dump, sizeof dump, &dlen, tmp);
        if (super && super[0]) {
            _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "\t%s base;\n", super);
            ti_dump_append(dump, sizeof dump, &dlen, tmp);
        }

        const uint8_t *fields = (const uint8_t *)ti_read_ptr(rec, REC_FIELDS_OFF);
        for (uint32_t i = 0; fields != NULL && i < TI_WALK_CAP; i++) {
            const uint8_t *f = fields + (size_t)i * FIELD_STRIDE;
            const char *fname = ti_read_cstr(f, FIELD_NAME_OFF);
            if (fname == NULL || fname[0] == '\0') break;
            const char *vartype = ti_read_cstr(f, FIELD_VARTYPE_OFF);
            const char *varops  = ti_read_cstr(f, FIELD_VAROPS_OFF);
            const char *fcmt    = ti_read_cstr(f, FIELD_COMMENT_OFF);
            uint32_t foff = 0, fsize = 0;
            ti_read_u32(f, FIELD_OFFSET_OFF, &foff);
            ti_read_u32(f, FIELD_SIZE_OFF, &fsize);
            if (vartype == NULL) vartype = "?";
            if (varops  == NULL) varops  = "";

            /* Pointer qualifiers precede the name; array qualifiers follow it. */
            int is_ptr = (strstr(varops, "*") != NULL);
            if (is_ptr)
                _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "\t%s%s %s", vartype, varops, fname);
            else
                _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "\t%s %s%s", vartype, fname, varops);
            ti_dump_append(dump, sizeof dump, &dlen, tmp);

            if (verbose)
                _snprintf_s(tmp, sizeof tmp, _TRUNCATE, ";//offset %d size %d\n", foff, fsize);
            else
                _snprintf_s(tmp, sizeof tmp, _TRUNCATE, ";\n");
            ti_dump_append(dump, sizeof dump, &dlen, tmp);
            if (fcmt && fcmt[0]) {
                _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "\t// %s\n", fcmt);
                ti_dump_append(dump, sizeof dump, &dlen, tmp);
            }
        }
        ti_dump_append(dump, sizeof dump, &dlen, "};\n");

        ti_emit_long(dump);
        sh_printf("Dumped type is a Class\n");
        if (sh_clipboard_set(dump))
            sh_printf("sh_type: copied type '%s' to the clipboard.\n", type);
        return;
    }

    /* Fall back to enum lookup when no class record exists. */
    void *en = ti_find_enum(reflect, type);
    if (en == NULL) {
        sh_printf("Couldn't find type %s!\n", type);
        return;
    }

    const char *ename = ti_read_cstr(en, ENUM_NAME_OFF);
    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "enum %s {\n", (ename && ename[0]) ? ename : type);
    ti_dump_append(dump, sizeof dump, &dlen, tmp);

    const uint8_t *members = (const uint8_t *)ti_read_ptr(en, ENUM_MEMBERS_OFF);
    for (uint32_t i = 0; members != NULL && i < TI_WALK_CAP; i++) {
        const uint8_t *m = members + (size_t)i * ENUM_MEMBER_STRIDE;
        const char *mname = ti_read_cstr(m, EMEMBER_NAME_OFF);
        if (mname == NULL || mname[0] == '\0') break;
        uint32_t mval = 0;
        ti_read_u32(m, EMEMBER_VALUE_OFF, &mval);
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "\t%s = %d,\n", mname, mval);
        ti_dump_append(dump, sizeof dump, &dlen, tmp);
    }
    ti_dump_append(dump, sizeof dump, &dlen, "};\n");

    ti_emit_long(dump);
    sh_printf("Dumped type is a Enum\n");
    if (sh_clipboard_set(dump))
        sh_printf("sh_type: copied enum '%s' to the clipboard.\n", type);
}

/* Emit a candidate satisfying the class/inherit ancestry rule. */
static void vc_emit(const char *C, const char *Y, int *count, int *y_seen)
{
    if (!C || !C[0]) return;
    int is_y = (strcmp(C, Y) == 0);
    if (is_y) *y_seen = 1;
    if (is_y || sh_typeinfo_class_derives(C, Y) == 1) {
        sh_printf("  %s\n", C);
        (*count)++;
    }
}

/* List live types compatible with an inherit declaration. Fall back to the
 * static candidate table only when the live registry is unavailable. */
#define SH_REGISTRY_MAX  16384   /* Candidate buffer cap. */
void h_sh_validclasses(idCmdArgs *a)
{
    const char *inherit = cmd_argv(a, 1);
    if (inherit == NULL || !inherit[0]) {
        sh_printf("usage: sh_validclasses <inherit>   (e.g. snapmaps/unknown -> every class; the inherit's base type Y gates the list)\n");
        return;
    }
    char ybuf[256];
    const char *Y = sh_typeinfo_inherit_base(inherit, ybuf, sizeof ybuf);
    if (Y == NULL || !Y[0]) {
        sh_printf("sh_validclasses: could not resolve inherit '%s' to a base class (missing decl or empty class).\n", inherit);
        return;
    }
    sh_printf("inherit '%s' -> base type Y = '%s'; engine-valid classes (derive from Y):\n", inherit, Y);

    static const char *names[SH_REGISTRY_MAX];       /* main-thread-serial console handler -> static is safe */
    int count = 0, y_seen = 0;
    int k = sh_typeinfo_collect_classnames(names, SH_REGISTRY_MAX);
    if (k > 0) {
        for (int i = 0; i < k; i++) vc_emit(names[i], Y, &count, &y_seen);
        if (k >= SH_REGISTRY_MAX)
            sh_printf("  (registry list truncated at %d -- raise SH_REGISTRY_MAX)\n", SH_REGISTRY_MAX);
    } else {
        sh_printf("  (live type registry unavailable -- using the static candidate set)\n");
        for (int i = 0; i < SH_CLASS_UNIVERSE_N; i++) vc_emit(SH_CLASS_UNIVERSE[i], Y, &count, &y_seen);
    }
    if (!y_seen) {   /* Include the base itself if the candidate list omitted it. */
        sh_printf("  %s\n", Y);
        count++;
    }
    sh_printf("(%d valid classes for inherit '%s')\n", count, inherit);
}

/* Dependency installation. */

/* An installed address as an RVA off the host image, or 0 if it did not resolve. Log-only. */
static unsigned ti_rva(uintptr_t addr)
{
    if (!addr || !g_doom_base) return 0u;
    return (unsigned)(addr - (uintptr_t)g_doom_base);
}

int sh_typeinfo_install(const sig_result *results, size_t n, const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;
    if (module_base == NULL) {
        backend_log("B2: typeinfo install SKIPPED -- module base NULL");
        return 0;
    }

    g_doom_base = module_base;
    g_find_type = (find_typeinfo_fn)sig_addr_by_name(results, n, "FindTypeInfoByName");
    g_find_enum = (find_enum_fn)sig_addr_by_name(results, n, "FindEnumByName");


    g_decl_find       = (decl_find_fn)sig_addr_by_name(results, n, "DeclPureFind");
    g_material_width  = (dim_fn)sig_addr_by_name(results, n, "MaterialWidth");
    g_material_height = (dim_fn)sig_addr_by_name(results, n, "MaterialHeight");

    /* Resolve code/data anchors once; each consumer handles missing dependencies. */
    g_declmgr_getter   = (declmgr_getter_fn)glb_resolve(module_base, "declmgr_accessor", NULL);
    g_validator_mgr    = (const uint8_t *)glb_resolve(module_base, "validator_manager", NULL);
    g_resource_mgr_ctx = (void *)glb_resolve(module_base, "resource_manager_ctx", NULL);
    g_material_mgr_ctx = (void *)glb_resolve(module_base, "material_manager_ctx", NULL);
    g_type_container   = (const uint8_t *)glb_resolve(module_base, "type_container", NULL);

    /* Log relative addresses for comparison across reports; 0 means unresolved. */
    char line[240];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: typeinfo install -- find_type=0x%x find_enum=0x%x declmgr_acc=0x%x decl_find=0x%x "
        "res_ctx=0x%x mat_ctx=0x%x validator=0x%x type_container=0x%x mat_dims=%s",
        ti_rva((uintptr_t)g_find_type), ti_rva((uintptr_t)g_find_enum),
        ti_rva((uintptr_t)g_declmgr_getter), ti_rva((uintptr_t)g_decl_find),
        ti_rva((uintptr_t)g_resource_mgr_ctx), ti_rva((uintptr_t)g_material_mgr_ctx),
        ti_rva((uintptr_t)g_validator_mgr), ti_rva((uintptr_t)g_type_container),
        (g_material_width && g_material_height) ? "yes" : "no");
    backend_log(line);
    return 1;
}
