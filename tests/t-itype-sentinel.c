/*
** Focused guard for internal table sentinel TValue classification.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"

static void assert_common_sentinel(TValue *tv)
{
  assert(tvistabinternal(tv));
  assert(tvislightud(tv));
  assert(!tvisnil(tv));
  assert(!tvisfalse(tv));
  assert(!tvistrue(tv));
  assert(!tvisnumber(tv));
  assert(!tvisnum(tv));
  assert(!tvisint(tv));
  assert(!tvisgcv(tv));
#if LJ_64
  assert(lightudseg(tv->u64) == LJ_LIGHTUD_INTERNAL_SEG);
  assert(lightudV(NULL, tv) == NULL);
#endif
}

int main(void)
{
  TValue forward, keylock;
  lua_State *L;
  cTValue *pubnull;

  setforwardV(&forward);
  assert_common_sentinel(&forward);
  assert(tvisforward(&forward));
  assert(!tviskeylock(&forward));

  setkeylockV(&keylock);
  assert_common_sentinel(&keylock);
  assert(tviskeylock(&keylock));
  assert(!tvisforward(&keylock));
  assert(forward.u64 != keylock.u64);

  L = luaL_newstate();
  assert(L != NULL);
  lua_pushlightuserdata(L, NULL);
  pubnull = L->top - 1;
  assert(tvislightud(pubnull));
  assert(!tvistabinternal(pubnull));
  assert(pubnull->u64 != forward.u64);
  assert(pubnull->u64 != keylock.u64);
  assert(lightudV(G(L), pubnull) == NULL);
  lua_close(L);

  printf("t-itype-sentinel OK: internal sentinels use reserved lightuserdata segment\n");
  return 0;
}
