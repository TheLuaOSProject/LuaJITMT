/*
** Focused regression test for cdata get/set conversion ctype snapshots.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_tg.h"

#include "lib/ctype_parse_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

static void assert_cdata_conversion_waits(lua_State *L, CTState *cts,
					  TGState *tg, const char *chunk)
{
  LJTCTypeParseReleaseCtx ctx;
  pthread_t thread;

  ljt_ctype_release_when_native_start(&ctx, &thread, cts, tg);
  ljt_lua_dostring(L, chunk);
  ljt_ctype_release_when_native_join(&ctx, thread);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "jit.off()\n"
    "ffi.cdef[[\n"
    "typedef enum { LJ_M7_CDATA_CONV_A = 7 } lj_m7_cdata_conv_enum_t;\n"
    "typedef struct { int x; } lj_m7_cdata_conv_node_t;\n"
    "typedef struct {\n"
    "  lj_m7_cdata_conv_enum_t e;\n"
    "  lj_m7_cdata_conv_node_t *p;\n"
    "} lj_m7_cdata_conv_box_t;\n"
    "]]\n"
    "lj_m7_cdata_conv_box = ffi.new('lj_m7_cdata_conv_box_t[1]')\n"
    "lj_m7_cdata_conv_node = ffi.new('lj_m7_cdata_conv_node_t[1]')\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);

  assert_cdata_conversion_waits(L, cts, tg,
    "lj_m7_cdata_conv_box[0].e = 'LJ_M7_CDATA_CONV_A'\n"
    "assert(tonumber(lj_m7_cdata_conv_box[0].e) == 7)\n");

  assert_cdata_conversion_waits(L, cts, tg,
    "lj_m7_cdata_conv_node[0].x = 99\n"
    "lj_m7_cdata_conv_box[0].p = lj_m7_cdata_conv_node\n"
    "assert(lj_m7_cdata_conv_box[0].p[0].x == 99)\n");

  lua_close(L);
  printf("t-ffi-cdata-conv-snapshot OK: cdata conversions wait on ctype snapshots\n");
  return 0;
}
