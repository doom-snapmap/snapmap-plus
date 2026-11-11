/* script_lstrlib.c -- Lua's string library, compiled with the runtime's
 * pattern-step counter. lstrlib.c's only local change is the l_matchstep
 * call at each 'match' step (src/third_party/lua/README.md); this unit
 * defines it, so the backend compiles lstrlib.c only through here.
 * C pattern matching never reaches the count hook, so without the counter a
 * pattern that backtracks (a*a*a*b on a long run of a) could hold the game
 * thread for minutes. */
struct lua_State;
void shs_match_step(struct lua_State *L);
#define l_matchstep(L) shs_match_step(L)
#include "../../third_party/lua/lstrlib.c"
