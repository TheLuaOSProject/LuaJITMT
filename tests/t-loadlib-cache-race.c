#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

void *__real_dlopen(const char *filename, int flags);

static const char *target_path;
static volatile int target_dlopen_active;
static volatile int target_dlopen_calls;
static volatile int target_dlopen_overlap;

void *__wrap_dlopen(const char *filename, int flags)
{
  int match = target_path && filename && strcmp(filename, target_path) == 0;
  void *ret;
  if (match) {
    struct timespec ts;
    int active = __sync_add_and_fetch(&target_dlopen_active, 1);
    if (active > 1)
      target_dlopen_overlap = 1;
    __sync_add_and_fetch(&target_dlopen_calls, 1);
    ts.tv_sec = 0;
    ts.tv_nsec = 50000000;
    (void)nanosleep(&ts, NULL);
  }
  ret = __real_dlopen(filename, flags);
  if (match)
    __sync_sub_and_fetch(&target_dlopen_active, 1);
  return ret;
}

static const char loadlib_race_lua[] =
"local th = require'threading'\n"
"local path = assert(os.getenv('LJ_LOADLIB_RACE_SO'))\n"
"local n = tonumber(os.getenv('LJ_LOADLIB_RACE_WORKERS') or '') or 8\n"
"local ready = th.channel(n)\n"
"local start = th.channel(n)\n"
"local threads = {}\n"
"for i = 1, n do\n"
"  threads[i] = th.spawn(function(so, ready_ch, start_ch)\n"
"    assert(ready_ch:send('ready', 10) == true)\n"
"    local token, ok = start_ch:recv(10)\n"
"    assert(ok == true and token == 'go')\n"
"    local fn, err, where = package.loadlib(so, 'luaopen_lj_loadlib_stopreq')\n"
"    assert(type(fn) == 'function', tostring(err) .. ':' .. tostring(where))\n"
"    return true\n"
"  end, path, ready, start)\n"
"end\n"
"for i = 1, n do\n"
"  local token, ok = ready:recv(10)\n"
"  assert(ok == true and token == 'ready')\n"
"end\n"
"for i = 1, n do assert(start:send('go', 10) == true) end\n"
"for i = 1, n do\n"
"  local ok, err = threads[i]:join(10)\n"
"  assert(ok == true, tostring(err))\n"
"end\n";

int main(void)
{
  lua_State *L;
  int status;
  target_path = getenv("LJ_LOADLIB_RACE_SO");
  assert(target_path && target_path[0] != '\0');

  L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  status = luaL_loadbuffer(L, loadlib_race_lua, sizeof(loadlib_race_lua) - 1,
			   "loadlib-race");
  assert(status == LUA_OK);
  status = lua_pcall(L, 0, 0, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    return 1;
  }
  lua_close(L);

  assert(target_dlopen_overlap == 0);
  assert(target_dlopen_calls == 1);
  printf("loadlib cache race test passed\n");
  return 0;
}
