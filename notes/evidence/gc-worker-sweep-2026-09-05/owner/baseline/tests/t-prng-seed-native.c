/*
** no-argument math.randomseed native STOPREQ guard.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lib/lua_fixture_helpers.h"
#include "lib/tg_stopreq_fixture_helpers.h"

#include "lj_obj.h"
#include "lj_tg.h"

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  TGState *tg;

  tg = G2TG(G(L));
  assert(tg != NULL);

  ljt_tg_set_stopreq(tg);
  ljt_lua_dostring(L,
    "assert(math.randomseed() == nil)\n"
    "local x = math.random()\n"
    "assert(x >= 0 and x < 1)\n");
  ljt_tg_clear_stopreq(tg);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) == 0);

  ljt_lua_dostring(L,
    "assert(math.randomseed() == nil)\n"
    "local y = math.random(1, 32)\n"
    "assert(y >= 1 and y <= 32)\n");

  lua_close(L);
  printf("t-prng-seed-native OK: sticky STOPREQ reseed verified\n");
  return 0;
}
