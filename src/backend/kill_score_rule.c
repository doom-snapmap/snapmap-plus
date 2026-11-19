/* kill_score_rule.c -- see kill_score.h. The part with no engine dependency. */
#include <stddef.h>
#include "kill_score.h"

unsigned sh_kill_score_type(unsigned type, unsigned (*standin)(unsigned type))
{
    unsigned s = standin ? standin(type) : 0;
    return s ? s : type;
}
