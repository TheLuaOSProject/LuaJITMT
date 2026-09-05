/*
** Focused regression for READY-safe userdata constructor roots and explicit
** error cleanup through both Lua pcall and the public C lua_pcall API.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_udata.h"

#if !defined(LJ_UDATA_TEST_HELPERS)
#error "t-udata-construction-roots requires LJ_UDATA_TEST_HELPERS"
#endif

static void full_cycle(lua_State *L)
{
  lua_gc(L, LUA_GCRESTART, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}

static void assert_rooted_identity(lua_State *L, LJUdataRoot *root,
				   GCudata *ud)
{
  TValue snap;
  TValue *slot = lj_tg_root_anchor_slot_acq(root->tg, root->idx);
  assert(slot != NULL);
  lj_tv_load_acq(&snap, slot);
  assert(tvisudata(&snap));
  assert(udataV(&snap) == ud);
  assert(lj_gc2_obj_valid_queued(G(L), obj2gco(ud)));
}

static void require_specialized_libs(lua_State *L)
{
  const char *src =
    "th = assert(require('threading'))\n"
    "buffer = assert(require('string.buffer'))\n"
    "ffi = assert(require('ffi'))\n";
  assert(luaL_dostring(L, src) == LUA_OK);
}

static void c_pcall_finreg_failure(lua_State *L, TGState *tg)
{
  uint32_t baseline = lj_tg_root_anchor_top_acq(tg);
  int status;
  lj_udata_test_fail_finreg_after(1);
  lua_getglobal(L, "th");
  lua_getfield(L, -1, "mutex");
  lua_remove(L, -2);
  status = lua_pcall(L, 0, 1, 0);
  assert(status == LUA_ERRMEM);
  lua_pop(L, 1);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);
  full_cycle(L);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);
}

static void lua_pcall_finreg_failures(lua_State *L, TGState *tg)
{
  uint32_t baseline = lj_tg_root_anchor_top_acq(tg);
  lj_udata_test_fail_finreg_after(1);
  assert(luaL_dostring(L,
    "local ok, err = pcall(th.channel, 2)\n"
    "assert(not ok and type(err) == 'string')\n") == LUA_OK);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);

  lj_udata_test_fail_finreg_after(1);
  assert(luaL_dostring(L,
    "local ok, err = xpcall(function() return buffer.new(64) end,\n"
    "  function(e) return e end)\n"
    "assert(not ok and type(err) == 'string')\n") == LUA_OK);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);
  full_cycle(L);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);
}

static void specialized_stress(lua_State *L)
{
  const char *src =
    "collectgarbage('restart')\n"
    "for i = 1, 16 do\n"
    "  local b = buffer.new(33)\n"
    "  b:put('payload', i)\n"
    "  local m = th.mutex()\n"
    "  assert(m:trylock()); m:unlock()\n"
    "  local ch = th.channel(2)\n"
    "  ch:send({ value = i, buffer = b })\n"
    "  collectgarbage('collect')\n"
    "  local v, ok = ch:recv()\n"
    "  assert(ok and v.value == i and tostring(v.buffer) == 'payload'..i)\n"
    "  local worker = th.spawn(function(x)\n"
    "    collectgarbage('collect')\n"
    "    return x.value, tostring(x.buffer)\n"
    "  end, v)\n"
    "  collectgarbage('collect')\n"
    "  local joined, value, text = worker:join()\n"
    "  assert(joined and value == i and text == 'payload'..i)\n"
    "end\n"
    "do\n"
    "  local ok = pcall(ffi.load, 'lj_lockless_library_that_does_not_exist')\n"
    "  assert(not ok)\n"
    "end\n"
    "collectgarbage('collect')\n"
    "collectgarbage('collect')\n";
  assert(luaL_dostring(L, src) == LUA_OK);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  TGState *tg;
  LJUdataRoot root;
  GCudata *ud;
  LJMutex *mutex;
  uint32_t baseline;

  assert(L != NULL);
  luaL_openlibs(L);
  lua_gc(L, LUA_GCSTOP, 0);
  tg = L2TG(L);
  assert(tg != NULL);
  baseline = lj_tg_root_anchor_top_acq(tg);

  /* The object survives complete cycles before it has any stack/root-spine
  ** semantic root other than the constructor anchor. */
  lj_state_checkstack(L, 1);
  ud = lj_udata_newrooted(L, sizeof(LJMutex), NULL, &root);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline + 1u);
  assert_rooted_identity(L, &root, ud);
  full_cycle(L);
  assert_rooted_identity(L, &root, ud);

  mutex = (LJMutex *)uddata(ud);
  mutex->state = LJ_MUTEX_UNLOCKED;
  lj_udata_specialize(L, ud, UDTYPE_MUTEX);
  full_cycle(L);
  assert_rooted_identity(L, &root, ud);
  lj_udata_pushrooted(L, ud, &root);
  assert(root.tg == NULL);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);
  assert(tvisudata(L->top - 1) && udataV(L->top - 1) == ud);
  full_cycle(L);
  lua_pop(L, 1);
  full_cycle(L);

  require_specialized_libs(L);
  c_pcall_finreg_failure(L, tg);
  lua_pcall_finreg_failures(L, tg);
  specialized_stress(L);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);

  lua_close(L);
  printf("t-udata-construction-roots OK\n");
  return 0;
}
