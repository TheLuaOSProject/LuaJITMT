/*
** Guard that traced TG tmpbuf concatenation resets after BUFSTR.
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

int main(void)
{
  lua_State *L = luaL_newstate();
  MSize len;
  assert(L != NULL);
  luaL_openlibs(L);

  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local t = {}\n"
    "for i = 1, 200000 do\n"
    "  t['k'..(i % 8192)] = i\n"
    "end\n"
    "assert(t.k1 == 196609)\n") == 0);

#if LJ_HASJIT && LJ_TARGET_X86ORX64
  len = lj_buf_len_tg(&L2TG(L)->tmpbuf);
  assert(len == 0);
#endif

  lua_close(L);
  printf("t-jit-tg-tmpbuf-reset OK: traced BUFSTR resets TG tmpbuf\n");
  return 0;
}
