#include <assert.h>
#include <stdio.h>
#include "../src/backend/edit_pair.h"

static unsigned failures;
static int calls;

/* Replace the pooled allocation before faulting, so recovery must cover a
 * partially completed assignment and cannot borrow an old pointer. */
static void assign(void *field, const char *value)
{
    char *copy = _strdup(value);
    assert(copy);
    free(*(char **)field);
    *(char **)field = copy;
    if (failures & (1u << calls++)) RaiseException(0xe0000001, 0, 0, NULL);
}

static void run(unsigned faults, int expected)
{
    char *cls = _strdup("old-class"), *inh = _strdup("old-inherit");
    failures = faults; calls = 0;
    int result = sh_edit_pair_apply(&cls, &inh, "new-class", "new-inherit", assign);
    assert(result == expected);
    if (expected == SH_EDIT_PAIR_APPLIED) {
        assert(!strcmp(cls, "new-class") && !strcmp(inh, "new-inherit"));
    } else if (expected == SH_EDIT_PAIR_REFUSED) {
        assert(!strcmp(cls, "old-class") && !strcmp(inh, "old-inherit"));
    }
    free(cls); free(inh);
}

int main(void)
{
    run(0, SH_EDIT_PAIR_APPLIED);
    run(1, SH_EDIT_PAIR_REFUSED);
    run(2, SH_EDIT_PAIR_REFUSED);
    run(2 | 4, SH_EDIT_PAIR_PARTIAL);
    run(2 | 8, SH_EDIT_PAIR_PARTIAL);
    assert(sh_edit_pair_apply(NULL, NULL, NULL, NULL, assign) == SH_EDIT_PAIR_REFUSED);
    assert(sh_edit_pair_apply(NULL, NULL, "class", NULL, assign) == SH_EDIT_PAIR_REFUSED);
    puts("edit_pair_test OK");
    return 0;
}
