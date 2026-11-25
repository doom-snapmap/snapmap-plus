/* script_lstrlib.c -- Lua's string library compiled with the runtime's pattern-step counter (l_matchstep at each
 * 'match' step, src/third_party/lua/README.md), since C pattern matching never reaches the count hook and a
 * backtracking pattern could otherwise hold the game thread. */
struct lua_State;
void shs_match_step(struct lua_State *L);
#define l_matchstep(L) shs_match_step(L)
#include "../../third_party/lua/lstrlib.c"
