/* Class/inherit assignment with owned rollback copies of pooled strings. */
#ifndef SH_EDIT_PAIR_H
#define SH_EDIT_PAIR_H

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#define SH_EDIT_PAIR_APPLIED 1
#define SH_EDIT_PAIR_REFUSED 0
#define SH_EDIT_PAIR_PARTIAL (-2)

typedef void (*sh_edit_pair_assign_fn)(void *field, const char *value);

/* Return PARTIAL only when a fault also prevents restoring the original pair.
 * Never retain pooled pointers across an assignment that could release them. */
static int sh_edit_pair_apply(void *class_field, void *inherit_field,
                              const char *cls, const char *inh,
                              sh_edit_pair_assign_fn assign)
{
    char *old_class = NULL, *old_inherit = NULL;
    int captured = 0, applied = 0, restored = 1;
    if (!assign || (!cls && !inh)) return SH_EDIT_PAIR_REFUSED;
    __try {
        const char *c = cls ? *(const char *const *)class_field : NULL;
        const char *h = inh ? *(const char *const *)inherit_field : NULL;
        if (cls && c) old_class = _strdup(c);
        if (inh && h) old_inherit = _strdup(h);
        captured = (!cls || old_class) && (!inh || old_inherit);
    } __except (EXCEPTION_EXECUTE_HANDLER) { captured = 0; }
    if (captured) {
        __try {
            if (inh) assign(inherit_field, inh);
            if (cls) assign(class_field, cls);
            applied = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { applied = 0; }
        if (!applied) {
            if (inh) {
                __try { assign(inherit_field, old_inherit); }
                __except (EXCEPTION_EXECUTE_HANDLER) { restored = 0; }
            }
            if (cls) {
                __try { assign(class_field, old_class); }
                __except (EXCEPTION_EXECUTE_HANDLER) { restored = 0; }
            }
        }
    }
    free(old_class); free(old_inherit);
    return applied ? SH_EDIT_PAIR_APPLIED : restored ? SH_EDIT_PAIR_REFUSED : SH_EDIT_PAIR_PARTIAL;
}

#endif
