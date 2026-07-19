/*
** jit.util.tracesnap() optional snapshot-PC position compatibility.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_gc2.h"
#include "lj_jit.h"

#include "lib/lua_fixture_helpers.h"

enum ThirdArg {
  THIRD_OMITTED,
  THIRD_FALSE,
  THIRD_TRUE,
  THIRD_ZERO
};

static int snapshot_pcpos(global_State *g, jit_State *J, TraceNo tr,
			  SnapNo sn, int32_t *pcpos)
{
  GCtrace *T;
  int have = 0;

  assert(lj_gc2_smr_read_try(g));
  T = traceref_safe(J, tr);
  if (T && trace_traceno_acq(T) == tr &&
      la_load64_acq(&T->retire_epoch) == 0 && sn < trace_nsnap_acq(T)) {
    SnapShot *snap = &trace_snap_acq(T)[sn];
    SnapEntry *map = &trace_snapmap_acq(T)[snap_mapofs_acq(snap)];
    const BCIns *pc = snap_pc_acq(&map[snap_nent_acq(snap)]);
    const BCIns *startpc = pc;
    while (bc_op((BCIns)la_load32_acq(
	     (const uint32_t *)startpc)) < BC_FUNCF)
      startpc--;
    *pcpos = (int32_t)(pc - startpc);
    have = 1;
  }
  lj_gc2_smr_read_leave(g);
  return have;
}

static void push_third_arg(lua_State *L, enum ThirdArg third)
{
  switch (third) {
  case THIRD_FALSE:
    lua_pushboolean(L, 0);
    break;
  case THIRD_TRUE:
    lua_pushboolean(L, 1);
    break;
  case THIRD_ZERO:
    lua_pushinteger(L, 0);  /* Zero is truthy in Lua. */
    break;
  default:
    assert(third == THIRD_OMITTED);
    break;
  }
}

static int call_tracesnap(lua_State *L, TraceNo tr, SnapNo sn,
			  enum ThirdArg third, int32_t expected,
			  int expect_result)
{
  global_State *g = G(L);
  int base = lua_gettop(L);
  int nargs = third == THIRD_OMITTED ? 2 : 3;
  int nres;

  assert(base >= 1);
  lua_pushvalue(L, 1);  /* Permanently stack-rooted tracesnap closure. */
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, (lua_Integer)tr);
  lua_pushinteger(L, (lua_Integer)sn);
  push_third_arg(L, third);
  ljt_lua_pcall(L, nargs, LUA_MULTRET, "jit.util.tracesnap getpos");
  nres = lua_gettop(L) - base;

  if (!expect_result) {
    assert(nres == 0);
  } else if (third == THIRD_TRUE || third == THIRD_ZERO) {
    assert(nres == 2);
    assert(lua_istable(L, base + 1));
    assert(lua_isnumber(L, base + 2));
    assert(lua_tointeger(L, base + 2) == (lua_Integer)expected);
  } else {
    assert(nres == 1);
    assert(lua_istable(L, base + 1));
  }

  lua_settop(L, base);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(jit_token_acq(g) == 0);
  return nres;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TraceNo tr, firsttr = 0;
  SnapNo firstsn = 0;
  int32_t firstpos = 0;
  int saw_nonzero = 0;

  lua_getglobal(L, "require");
  lua_pushliteral(L, "jit.util");
  ljt_lua_pcall(L, 1, 1, "require jit.util");
  lua_getfield(L, -1, "tracesnap");
  assert(lua_isfunction(L, -1));
  lua_remove(L, -2);
  assert(lua_gettop(L) == 1);

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function work(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    if i % 3 == 0 then sum = sum + i else sum = sum - i end\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 20 do work(40) end\n"
    "jit.off()\n");

  assert(gc2_smr_readers_acq(g) == 0);
  assert(jit_token_acq(g) == 0);
  for (tr = 1; tr < trace_sizetrace_acq(J); tr++) {
    SnapNo sn = 0;
    int32_t pos;
    while (snapshot_pcpos(g, J, tr, sn, &pos)) {
      if (firsttr == 0) {
	firsttr = tr;
	firstsn = sn;
	firstpos = pos;
      }
      if (pos != 0)
	saw_nonzero = 1;
      (void)call_tracesnap(L, tr, sn, THIRD_TRUE, pos, 1);
      sn++;
    }
  }
  assert(firsttr != 0);
  assert(saw_nonzero);  /* Exercise the backwards owning-header scan. */

  (void)call_tracesnap(L, firsttr, firstsn, THIRD_OMITTED, firstpos, 1);
  (void)call_tracesnap(L, firsttr, firstsn, THIRD_FALSE, firstpos, 1);
  (void)call_tracesnap(L, firsttr, firstsn, THIRD_ZERO, firstpos, 1);

  {
    uint32_t expect = LJ_GC2_SMR_OPEN;
    assert(gc2_smr_reclaiming_cas(
	     g, &expect, LJ_GC2_SMR_META_EXCLUSIVE));
    (void)call_tracesnap(L, firsttr, firstsn, THIRD_TRUE, firstpos, 0);
    gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);
  }

  lua_settop(L, 0);
  lua_close(L);
  puts("jit-util-tracesnap-getpos OK");
  return 0;
}
