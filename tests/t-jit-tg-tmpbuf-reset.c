/*
** Guard TG tmpbuf append state for traced concatenation.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_arch.h"
#include "lj_buf.h"
#include "lj_obj.h"
#include "lj_tg.h"

static void run_script(lua_State *L, const char *code)
{
  if (luaL_dostring(L, code) != 0) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    assert(0);
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  MSize len;
  assert(L != NULL);
  luaL_openlibs(L);

  run_script(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function loop(n)\n"
    "  local out = ''\n"
    "  for i = 1, n do out = out .. '/' .. i end\n"
    "  return out\n"
    "end\n"
    "for i = 1, 20 do assert(loop(3) == '/1/2/3') end\n");

#if LJ_HASJIT && LJ_TARGET_X86ORX64
  len = lj_buf_len_tg(&L2TG(L)->tmpbuf);
  assert(len == 6);
#endif

  run_script(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function twice(n)\n"
    "  local out = ''\n"
    "  for i = 1, n do out = out .. 'q' end\n"
    "  return out\n"
    "end\n"
    "for i = 1, 20 do assert(twice(2) == 'qq') end\n");

#if LJ_HASJIT && LJ_TARGET_X86ORX64
  len = lj_buf_len_tg(&L2TG(L)->tmpbuf);
  assert(len == 2);
#endif

  lua_close(L);
  printf("t-jit-tg-tmpbuf-reset OK: traced TG tmpbuf append state preserved\n");
  return 0;
}
