/* level_templates_rule.c -- see level_templates.h. The parts with no engine
 * dependency: the call decoding and the list filter. */
#include <string.h>
#include "level_templates.h"

uintptr_t sh_level_templates_call_target(const uint8_t *call)
{
    int32_t rel;
    if (!call || call[0] != 0xE8) return 0;
    memcpy(&rel, call + 1, sizeof rel);
    return (uintptr_t)(call + 5) + (intptr_t)rel;
}

int sh_level_templates_filter(void **items, int count, const char *(*name_of)(const void *item),
                              int (*hides)(const char *name), int *hidden, const char **first)
{
    int i, kept = 0, gone = 0;
    if (!items || count <= 0 || !name_of || !hides) {
        if (hidden) *hidden = 0;
        return count < 0 ? 0 : count;
    }
    for (i = 0; i < count; ++i) {
        const char *name = items[i] ? name_of(items[i]) : NULL;
        if (name && hides(name)) {
            if (!gone && first) *first = name;
            gone++;
            continue;
        }
        items[kept++] = items[i];
    }
    if (hidden) *hidden = gone;
    return kept;
}
