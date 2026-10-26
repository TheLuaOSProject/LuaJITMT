/*
** Semantic regression for active-MT sampled-absent TSETR(nil).
** The function's sole TSETV is patched before recording to the equivalent
** raw integer-key TSETR bytecode, matching a library raw-store producer.
*/
#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_bc.h"
#include "lj_obj.h"

static const char raw_nil_store_setup[] =
  "package.path = 'src/?.lua;src/jit/?.lua;' .. package.path\n"
  "local util = require('jit.util')\n"
  "jit.flush()\n"
  "jit.on(raw_nil_set, true)\n"
  "jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=8')\n"
  "local started, live, aborted, exits = {}, 0, 0, 0\n"
  "local function trace(what, no, func, pc, otr, reason, detail)\n"
  "  if func == raw_nil_set then\n"
  "    if what == 'start' then started[no] = true\n"
  "    elseif what == 'abort' then\n"
  "      if started[no] then aborted = aborted + 1 end\n"
  "      started[no] = nil\n"
  "    end\n"
  "  end\n"
  "end\n"
  "local function exit(no) if started[no] then exits = exits + 1 end end\n"
  "jit.attach(trace, 'trace'); jit.attach(exit, 'texit')\n"
  "local t = raw_nil_store_t\n"
  "for i = 1, 12 do assert(raw_nil_set(t, 1, 32)) end\n"
  "for no in pairs(started) do if util.traceinfo(no) then live = live + 1 end end\n"
  "raw_nil_trace_started = 0\n"
  "for _ in pairs(started) do raw_nil_trace_started = raw_nil_trace_started + 1 end\n"
  "raw_nil_trace_live = live\n"
  "raw_nil_trace_aborted = aborted\n"
  "raw_nil_trace_exits = exits\n"
  "jit.attach(trace); jit.attach(exit)\n";

static const char raw_nil_store_func[] =
  "function raw_nil_set(t, key, n)\n"
  "  local i = 0\n"
  "  while i < n do\n"
  "    t[key] = nil\n"
  "    i = i + 1\n"
  "  end\n"
  "  return true\n"
  "end\n";

static void patch_tsetv_to_tsetr(lua_State *L)
{
  GCfunc *fn;
  GCproto *pt;
  BCIns *bc;
  MSize i;
  unsigned changed = 0;
  lua_getglobal(L, "raw_nil_set");
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  bc = proto_bc(pt);
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_TSETV) {
      setbc_op(&ins, BC_TSETR);
      la_store32_rel((uint32_t *)&bc[i], (uint32_t)ins);
      changed++;
    }
  }
  lua_pop(L, 1);
  assert(changed == 1);
}

static lua_Integer getglobal_integer(lua_State *L, const char *name)
{
  lua_Integer value;
  lua_getglobal(L, name);
  assert(lua_isnumber(L, -1));
  value = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return value;
}

static int call_raw_nil_set(lua_State *L)
{
  lua_getglobal(L, "raw_nil_set");
  lua_getglobal(L, "raw_nil_store_t");
  lua_pushinteger(L, 1);
  lua_pushinteger(L, 32);
  if (lua_pcall(L, 3, 1, 0) != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "raw_nil_set failed: %s\n", msg ? msg : "unknown error");
    return 0;
  }
  if (!lua_toboolean(L, -1)) {
    lua_pop(L, 1);
    return 0;
  }
  lua_pop(L, 1);
  return 1;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  int script_ok;
  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);

  /* Build and patch bytecode before mt_entering or trace publication. */
  assert(luaL_loadstring(L, raw_nil_store_func) == LUA_OK);
  assert(lua_pcall(L, 0, 0, 0) == LUA_OK);
  patch_tsetv_to_tsetr(L);
  lua_newtable(L);
  lua_setglobal(L, "raw_nil_store_t");

  assert(mt_active_acq(g) == 0 && mt_entering_acq(g) == 0);
  assert(mt_entering_add_rlx(g, 1) == 0);
  script_ok = luaL_dostring(L, raw_nil_store_setup) == LUA_OK;
  if (!script_ok) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "trace setup failed");
  } else {
    printf("raw_nil_set traces: started=%lld live=%lld aborted=%lld exits=%lld\n",
           (long long)getglobal_integer(L, "raw_nil_trace_started"),
           (long long)getglobal_integer(L, "raw_nil_trace_live"),
           (long long)getglobal_integer(L, "raw_nil_trace_aborted"),
           (long long)getglobal_integer(L, "raw_nil_trace_exits"));

    /* Mutate after warmup. A published trace that erased TSETR(nil) leaves 4242. */
    lua_getglobal(L, "raw_nil_store_t");
    lua_pushinteger(L, 4242);
    lua_rawseti(L, -2, 1);
    lua_pop(L, 1);
    if (!call_raw_nil_set(L)) {
      script_ok = 0;
    } else {
      lua_getglobal(L, "raw_nil_store_t");
      lua_rawgeti(L, -1, 1);
      if (!lua_isnil(L, -1)) {
        fprintf(stderr, "raw nil store lost: key 1 remains %lld\n",
                (long long)lua_tointeger(L, -1));
        script_ok = 0;
      }
      lua_pop(L, 2);
    }
  }

  if (script_ok) {
    /* A raw nil store must not invoke __newindex, including on an empty
    ** array slot or a key whose value was cleared after recording. This
    ** shared test also runs against the native x64 helper ABI. */
    script_ok = luaL_dostring(L,
      "local calls = 0\n"
      "setmetatable(raw_nil_store_t, { __newindex = function() calls=calls+1 end })\n"
      "for i=1,20 do\n"
      "  assert(raw_nil_set(raw_nil_store_t, 1, 32))\n"
      "  assert(rawget(raw_nil_store_t, 1)==nil and calls==0)\n"
      "  rawset(raw_nil_store_t, 1, i)\n"
      "  assert(raw_nil_set(raw_nil_store_t, 1, 32))\n"
      "  assert(rawget(raw_nil_store_t, 1)==nil and calls==0)\n"
      "end\n") == LUA_OK;
    if (!script_ok) fprintf(stderr, "%s\n", lua_tostring(L, -1));
  }

  /* The entering lease belongs to this test and is always restored before close. */
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, INT_MAX);
  lua_close(L);
  assert(script_ok);
  puts("t-jit-raw-nil-store OK: post-record insertion deleted; raw nil stores bypass __newindex");
  return 0;
}
