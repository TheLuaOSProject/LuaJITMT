/* A cold, allocation-free loop can be the only mutator driving SWEEP. */
#include <assert.h>
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_dispatch.h"
#include "lj_gc2.h"
#include "lj_trace.h"

typedef struct Probe {
  GCproto *pt;
  unsigned in_hot, sweep_retries, finalized_here;
  uint32_t finalized;
} Probe;

static __thread Probe *current;

void __real_lj_trace_hot(jit_State *J, const BCIns *pc, lua_State *L);
void __wrap_lj_trace_hot(jit_State *J, const BCIns *pc, lua_State *L)
{
  Probe *p = current;
  if (p && (uintptr_t)pc > (uintptr_t)proto_bc(p->pt) &&
      (uintptr_t)pc <= (uintptr_t)(proto_bc(p->pt) + p->pt->sizebc)) {
    if (gc2_phase_acq(G(L)) == LJ_GC2_SWEEP) p->sweep_retries++;
    p->in_hot++;
    __real_lj_trace_hot(J, pc, L);
    p->in_hot--;
  } else {
    __real_lj_trace_hot(J, pc, L);
  }
}

static int finalize(lua_State *L)
{
  Probe *p = (Probe *)lua_touserdata(L, lua_upvalueindex(1));
  (void)la_add32_acqrel(&p->finalized, 1);
  if (current == p && p->in_hot) {
    p->finalized_here++;
    assert(jit_owner_word_acq(G(L)) == 0 && !jit_owner_l_acq(G2J(G(L))));
    assert(lua_checkstack(L, 2048));
    assert(luaL_dostring(L,
      "local marker = {}; local ok, err = pcall(function() error(marker) end)\n"
      "assert(not ok and err == marker)") == 0);
  }
  return 0;
}

static void check(int pool, int peer, int running)
{
  Probe p = {0};
  lua_State *L = luaL_newstate();
  global_State *g;
  uint64_t completed;
  unsigned i;
  int ref;
  assert(L);
  luaL_openlibs(L);
  g = G(L);
  lua_pushinteger(L, pool);
  lua_setglobal(L, "pool");
  lua_pushboolean(L, peer);
  lua_setglobal(L, "peer");
  assert(luaL_dostring(L,
    "local th = require('threading'); th.gcworkers(pool)\n"
    "if peer then local ready = th.channel(1); stop = th.channel(1)\n"
    "  worker = th.spawn(function() ready:send(true); stop:recv() end)\n"
    "  assert(ready:recv(10)) end") == 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(luaL_dostring(L,
    "return function(n) local s=0; for i=1,n do s=s+i end; return s end") == 0);
  p.pt = funcproto(funcV(L->top-1));
  ref = luaL_ref(L, LUA_REGISTRYINDEX);
  (void)lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushlightuserdata(L, &p);
  lua_pushcclosure(L, finalize, 1);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
  for (i = 0; i < 5000; i++) {
    lua_newtable(L);
    lua_pop(L, 1);
  }
  completed = gc2_sweep_to_idle_acq(g);
  for (i = 0; i < 20000 && gc2_phase_acq(g) != LJ_GC2_SWEEP; i++)
    (void)lj_gc2_step_explicit(L, 1);
  assert(i < 20000);
  if (running) lua_gc(L, LUA_GCRESTART, -1);
  current = &p;
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
  lua_pushinteger(L, 500000);
  assert(lua_pcall(L, 1, 1, 0) == 0);
  assert(lua_tonumber(L, -1) == 125000250000.0);
  current = NULL;
  assert(lua_gc(L, LUA_GCISRUNNING, 0) == running);
  if (running) {
    assert(gc2_sweep_to_idle_acq(g) > completed && proto_trace_acq(p.pt));
    assert(la_load32_acq(&p.finalized) == 1);
    if (!pool) assert(p.sweep_retries > 0 && p.finalized_here == 1);
  } else {
    assert(p.sweep_retries > 0 && !proto_trace_acq(p.pt));
    assert(gc2_sweep_to_idle_acq(g) == completed && !la_load32_acq(&p.finalized));
  }
  if (peer)
    assert(luaL_dostring(L, "stop:send(true); assert(worker:join())") == 0);
  lua_close(L);
  assert(la_load32_acq(&p.finalized) == 1);
}

int main(void)
{
  check(0, 0, 1);
  check(0, 1, 1);
  check(2, 0, 1);
  check(2, 1, 1);
  check(0, 0, 0);
  puts("t-jit-sweep-hotloop OK");
  return 0;
}
