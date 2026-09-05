/*
** VM-event failure native STOPREQ guard.
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
    "jit.attach(function()\n"
    "  error('vmevent sticky stopreq smoke')\n"
    "end, 'bc')\n"
    "local f = assert(loadstring('return 1'))\n"
    "assert(f() == 1)\n");
  ljt_tg_clear_stopreq(tg);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) == 0);

  ljt_lua_dostring(L,
    "jit.attach(function()\n"
    "  error('vmevent recovery smoke')\n"
    "end, 'bc')\n"
    "local f = assert(loadstring('return 2'))\n"
    "assert(f() == 2)\n");

  lua_close(L);
  printf("t-vmevent-native-stopreq OK: sticky STOPREQ failure report verified\n");
  return 0;
}
