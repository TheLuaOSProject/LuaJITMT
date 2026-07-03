/*
** Focused regression fixture for JIT table-store routing while mt_entering
** is nonzero.
*/

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"

static const char jit_entering_store_script[] =
  "package.path = 'src/?.lua;src/jit/?.lua;' .. package.path\n"
  "local dump = require('jit.dump')\n"
  "local util = require('jit.util')\n"
  "JIT_ENTERING_DUMP = os.tmpname()\n"
  "dump.on('im', JIT_ENTERING_DUMP)\n"
  "jit.on()\n"
  "jit.flush()\n"
  "jit.opt.start('hotloop=1', 'hotexit=1')\n"
  "local a = { 0 }\n"
  "for i = 1, 80 do\n"
  "  a[1] = i + 0.5\n"
  "end\n"
  "assert(a[1] == 80.5)\n"
  "assert(util.traceinfo(1), 'entering array store did not trace')\n"
  "jit.flush()\n"
  "jit.opt.start('hotloop=1', 'hotexit=1')\n"
  "local h = { stable = 0 }\n"
  "for i = 1, 80 do\n"
  "  h.stable = i + 0.5\n"
  "end\n"
  "assert(h.stable == 80.5)\n"
  "assert(util.traceinfo(1), 'entering hash store did not trace')\n"
  "dump.off()\n";

static char *read_file(const char *path)
{
  FILE *fp;
  long n;
  char *buf;
  fp = fopen(path, "rb");
  assert(fp != NULL);
  assert(fseek(fp, 0, SEEK_END) == 0);
  n = ftell(fp);
  assert(n >= 0);
  assert(fseek(fp, 0, SEEK_SET) == 0);
  buf = (char *)malloc((size_t)n + 1u);
  assert(buf != NULL);
  assert(fread(buf, 1, (size_t)n, fp) == (size_t)n);
  buf[n] = '\0';
  fclose(fp);
  return buf;
}

static const char *dump_path(lua_State *L)
{
  const char *path;
  lua_getglobal(L, "JIT_ENTERING_DUMP");
  path = lua_tostring(L, -1);
  assert(path != NULL);
  return path;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  const char *path;
  char *dump;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  assert(mt_active_acq(g) == 0);
  assert(mt_entering_acq(g) == 0);

  assert(mt_entering_add_rlx(g, 1) == 0);
  if (luaL_dostring(L, jit_entering_store_script) != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "lua error");
    assert(0);
  }
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, INT_MAX);

  path = dump_path(L);
  dump = read_file(path);
  assert(strstr(dump, "lj_tab_storetv_forjit_array") != NULL ||
	 strstr(dump, "lock cmpxchg") != NULL);
  assert(strstr(dump, "lj_tab_storetv_forjit_hash") != NULL ||
	 strstr(dump, "lock cmpxchg") != NULL);
  assert(strstr(dump, "TRACE 1") != NULL);
  assert(strstr(dump, "TRACE 2") != NULL);
  free(dump);
  remove(path);
  lua_pop(L, 1);
  lua_close(L);
  return 0;
}
