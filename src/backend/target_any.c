/* Reveal the editor-decl visibility bits 0xC0 at +0x3CD, and put back exactly
 * what the reveal changed. The same flags also affect wire coloring.
 * GetDeclsOfType returns a registry node with array@+0x20 and count@+0x28.
 * The array is engine-owned and must not be freed. Recheck these offsets when
 * porting engine builds. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "target_any.h"
#include "commands.h"
#include "backend_log.h"

#define DECL_TYPE_NAME      "idDeclSnapEditorEntity"
#define LIST_ARRAY_OFF      0x20     /* decl-manager node -> decl-pointer array */
#define LIST_COUNT_OFF      0x28     /* decl-manager node -> decl count (uint) */
#define DECL_FLAGS_OFF      0x3CD    /* decl -> editor-flags byte; bits 7-6 (0xC0) = the visibility pair */
#define VIS_BITS            0xC0     /* the editor-visibility pair the toggle flips */
#define COUNT_CAP           (1u << 20) /* Bound implausible registry counts. */

typedef void *(*get_decls_fn)(const char *type_name);

/* One decl the reveal changed, and the visibility bits it had before. */
typedef struct revealed { uint8_t *flags; uint8_t before; } revealed;

static void         *g_get_decls = NULL;
static volatile LONG g_shown     = 0;      /* 0 = hidden, 1 = shown. */
static revealed     *g_revealed  = NULL;   /* what the reveal changed, for the hide */
static uint32_t      g_revealed_count;

static int safe_read_ptr(const void *src, void **out)
{
    __try { *out = *(void *const *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int safe_read_u32(const void *src, uint32_t *out)
{
    __try { *out = *(const uint32_t *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Show every decl whose visibility pair is not fully set, remembering each one
 * changed. Returns 0 when the registry cannot be read (nothing changed). */
static int target_any_reveal(uint32_t *out_count, uint32_t *out_changed)
{
    void *list, *array = NULL;
    uint32_t count = 0, i;
    *out_count = *out_changed = 0;
    list = ((get_decls_fn)g_get_decls)(DECL_TYPE_NAME);
    if (!list || !safe_read_ptr((const uint8_t *)list + LIST_ARRAY_OFF, &array) ||
        !safe_read_u32((const uint8_t *)list + LIST_COUNT_OFF, &count) || count > COUNT_CAP)
        return 0;
    *out_count = count;
    free(g_revealed);
    g_revealed = NULL;
    g_revealed_count = 0;
    if (!array || !count) return 1;
    g_revealed = (revealed *)malloc((size_t)count * sizeof *g_revealed);
    if (!g_revealed) return 0;
    for (i = 0; i < count; i++) {
        void *decl = NULL;
        uint8_t *fp;
        if (!safe_read_ptr((const uint8_t *)array + (size_t)i * 8, &decl)) break;
        if (!decl) continue;
        fp = (uint8_t *)decl + DECL_FLAGS_OFF;
        __try {
            uint8_t cur = *fp;
            if ((cur & VIS_BITS) != VIS_BITS) {
                g_revealed[g_revealed_count].flags = fp;
                g_revealed[g_revealed_count].before = (uint8_t)(cur & VIS_BITS);
                *fp = (uint8_t)(cur | VIS_BITS);
                g_revealed_count++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    *out_changed = g_revealed_count;
    return 1;
}

/* Put back the visibility pair of every decl the reveal changed, and nothing
 * else: a decl that was visible before the reveal stays visible. */
static uint32_t target_any_restore(void)
{
    uint32_t i, restored = 0;
    for (i = 0; i < g_revealed_count; i++) {
        __try {
            uint8_t *fp = g_revealed[i].flags;
            *fp = (uint8_t)((*fp & (uint8_t)~VIS_BITS) | g_revealed[i].before);
            restored++;
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    free(g_revealed);
    g_revealed = NULL;
    g_revealed_count = 0;
    return restored;
}

void sh_target_any_install(void *get_decls_of_type)
{
    g_get_decls = get_decls_of_type;
}

int sh_target_any_is_shown(void)
{
    return (int)g_shown;
}

void h_target_any(struct idCmdArgs *a)
{
    char line[160];
    (void)a;
    if (g_get_decls == NULL) {
        sh_printf("sh_target_any is not available on this game build.\n");
        return;
    }
    if (!g_shown) {
        uint32_t count = 0, changed = 0;
        if (!target_any_reveal(&count, &changed)) {
            sh_printf("sh_target_any is not available on this game build.\n");
            backend_log("target_any: the editor entity registry could not be read");
            return;
        }
        InterlockedExchange(&g_shown, 1);
        sh_printf("sh_target_any: hidden palette entries shown (run again to hide them).\n");
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "target_any: revealed %u of %u editor entity decls", changed, count);
    } else {
        uint32_t restored = target_any_restore();
        InterlockedExchange(&g_shown, 0);
        sh_printf("sh_target_any: hidden palette entries hidden again.\n");
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "target_any: hid the %u editor entity decls the reveal showed", restored);
    }
    backend_log(line);
}
