/*
** no-argument math.randomseed native STOPREQ guard.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_tg.h"

static void set_stopreq(TGState *tg)
{
  (void)lj_tg_flags_or_rlx(tg, TGF_STOPREQ);
}

static void clear_stopreq(TGState *tg)
{
  uint8_t flags = lj_tg_flags_acq(tg);
  lj_tg_flags_store_rlx(tg, (uint8_t)(flags & ~TGF_STOPREQ));
}

static void run_ok(lua_State *L, const char *chunk)
{
  int rc = luaL_dostring(L, chunk);
  if (rc != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "unexpected Lua error: %s\n", err ? err : "(nil)");
  }
  assert(rc == LUA_OK);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  TGState *tg;

  assert(L != NULL);
  luaL_openlibs(L);
  tg = G2TG(G(L));
  assert(tg != NULL);

  set_stopreq(tg);
  run_ok(L,
    "assert(math.randomseed() == nil)\n"
    "local x = math.random()\n"
    "assert(x >= 0 and x < 1)\n");
  clear_stopreq(tg);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) == 0);

  run_ok(L,
    "assert(math.randomseed() == nil)\n"
    "local y = math.random(1, 32)\n"
    "assert(y >= 1 and y <= 32)\n");

  lua_close(L);
  printf("t-prng-seed-native OK: sticky STOPREQ reseed verified\n");
  return 0;
}
