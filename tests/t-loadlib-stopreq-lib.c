#include "lib/test_sleep.h"

typedef struct lua_State lua_State;

__attribute__((constructor)) static void loadlib_stopreq_ctor(void)
{
  sleep_ns(200000000L);
}

int luaopen_lj_loadlib_stopreq(lua_State *L)
{
  (void)L;
  return 0;
}
