/* Toggle editor-decl visibility bits 0xC0 at +0x3CD, keeping idInfoPath visible
 * when hiding. The same flags also affect wire coloring.
 * GetDeclsOfType returns a registry node with array@+0x20 and count@+0x28.
 * The array is engine-owned and must not be freed. Recheck these offsets and
 * the entityDef/className hops when porting engine builds. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "target_any.h"
#include "commands.h"
#include "backend_log.h"

#define DECL_TYPE_NAME      "idDeclSnapEditorEntity"
#define LIST_ARRAY_OFF      0x20     /* decl-manager node -> decl-pointer array */
#define LIST_COUNT_OFF      0x28     /* decl-manager node -> decl count (uint) */
#define DECL_FLAGS_OFF      0x3CD    /* decl -> editor-flags byte; bits 7-6 (0xC0) = the visibility pair */
#define DECL_ENTITYDEF_OFF  0x1C8    /* decl -> entityDef pointer */
#define ENTITYDEF_CLASS_OFF 0x60     /* entityDef -> className idStr ptr (re-hide idInfoPath spare only) */
#define VIS_BITS            0xC0     /* the editor-visibility pair the toggle flips */
#define COUNT_CAP           (1u << 20) /* Bound implausible registry counts. */

typedef void *(*get_decls_fn)(const char *type_name);

static void         *g_get_decls        = NULL;
static volatile LONG g_shown            = 0;      /* 0 = hidden, 1 = shown. */


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

/* Check the class only when hiding. Unreadable hops compare unequal. */
static int decl_class_is_infopath(const uint8_t *decl)
{
    __try {
        const uint8_t *entdef = *(const uint8_t *const *)(decl + DECL_ENTITYDEF_OFF);
        if (entdef == NULL) return 0;
        const char *name = *(const char *const *)(entdef + ENTITYDEF_CLASS_OFF);
        if (name == NULL) return 0;
        return strcmp(name, "idInfoPath") == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* Apply visibility and return registry/touched counts. Return 0 with an error
 * if lookup fails; inaccessible individual decls are skipped. */
static int target_any_apply(int show, uint32_t *out_count, uint32_t *out_touched, const char **out_err)
{
    *out_count = 0; *out_touched = 0; *out_err = NULL;

    if (g_get_decls == NULL) { *out_err = "GetDeclsOfType not resolved"; return 0; }

    void *list = ((get_decls_fn)g_get_decls)(DECL_TYPE_NAME);
    if (list == NULL) { *out_err = "GetDeclsOfType returned null"; return 0; }

    void    *array = NULL;
    uint32_t count = 0;
    if (!safe_read_ptr((const uint8_t *)list + LIST_ARRAY_OFF, &array) ||
        !safe_read_u32((const uint8_t *)list + LIST_COUNT_OFF, &count)) {
        *out_err = "decl list array/count unreadable";
        return 0;
    }
    *out_count = count;
    if (array == NULL || count == 0) return 1;
    if (count > COUNT_CAP) { *out_err = "decl count implausible (stale manager node?)"; return 0; }

    uint32_t touched = 0;
    for (uint32_t i = 0; i < count; i++) {
        void *decl_v = NULL;
        if (!safe_read_ptr((const uint8_t *)array + (size_t)i * 8, &decl_v)) break;
        uint8_t *decl = (uint8_t *)decl_v;
        if (decl == NULL) continue;

        uint8_t *fp = decl + DECL_FLAGS_OFF;
        uint8_t  cur;
        __try { cur = *fp; }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }

        if (show) {
            __try { *fp = (uint8_t)(cur | VIS_BITS); touched++; }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
        } else {
            if (decl_class_is_infopath(decl)) continue;
            __try { *fp = (uint8_t)(cur & (uint8_t)~VIS_BITS); touched++; }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
    }

    *out_touched = touched;
    return 1;
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
    (void)a;
    if (g_get_decls == NULL) {
        sh_printf("sh_target_any: GetDeclsOfType unresolved -- cannot toggle.\n");
        return;
    }

    LONG shown = InterlockedExchange(&g_shown, !g_shown);
    shown = !shown;

    uint32_t count = 0, touched = 0;
    const char *err = NULL;
    if (target_any_apply((int)shown, &count, &touched, &err)) {
        sh_printf("sh_target_any: %s %u/%u editor entity decls.\n",
                  shown ? "revealed" : "re-hid", touched, count);
        backend_log(shown ? "B2: sh_target_any REVEAL" : "B2: sh_target_any RE-HIDE");
    } else {
        sh_printf("sh_target_any: could not run (%s).\n", err ? err : "unknown");
    }
}
