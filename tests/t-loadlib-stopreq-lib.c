#include <time.h>

typedef struct lua_State lua_State;

__attribute__((constructor)) static void loadlib_stopreq_ctor(void)
{
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 200000000;
  (void)nanosleep(&ts, NULL);
}

int luaopen_lj_loadlib_stopreq(lua_State *L)
{
  (void)L;
  return 0;
}
