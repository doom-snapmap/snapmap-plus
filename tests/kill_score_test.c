/* kill_score_test.c -- the rule half of kill_score (src/backend/kill_score_rule.c): the Player Score switch
 * reads a ported demon's type as the stock type it stands in for, and every other type as it is. */
#include <stdio.h>
#include "kill_score.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

#define BARON    (1u << 7)
#define MANCUBUS (1u << 8)
#define PORT_A   (1u << 19)
#define PORT_B   (1u << 20)

/* The listing: PORT_A stands in for the Baron, PORT_B for the Mancubus; nothing else is a port. */
static unsigned standin(unsigned type)
{
    if (type == PORT_A) return BARON;
    if (type == PORT_B) return MANCUBUS;
    return 0;
}

int main(void)
{
    CHECK(sh_kill_score_type(PORT_A, standin) == BARON);
    CHECK(sh_kill_score_type(PORT_B, standin) == MANCUBUS);
    /* stock types and types no declared mod was given reach the switch unchanged */
    CHECK(sh_kill_score_type(BARON, standin) == BARON);
    CHECK(sh_kill_score_type(1u << 21, standin) == (1u << 21));
    CHECK(sh_kill_score_type(0, standin) == 0);
    /* without the listing (scope off) nothing changes */
    CHECK(sh_kill_score_type(PORT_A, NULL) == PORT_A);
    printf(g_fail ? "kill_score_test: FAIL\n" : "kill_score_test: all checks passed\n");
    return g_fail;
}
