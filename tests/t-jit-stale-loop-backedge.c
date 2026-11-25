/* A peer can blacklist the loop between decoding JFORL/JITERL and declining
** native entry. Both instructions have already updated the control slot. */
#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lj_trace.h"

static GCproto *sum_proto;
static unsigned polls;
static unsigned interruptions;

void __real_lj_trace_hot(jit_State *J, const BCIns *pc, lua_State *L);
void __wrap_lj_trace_hot(jit_State *J, const BCIns *pc, lua_State *L)
{
  __real_lj_trace_hot(J, pc, L);
  if (!sum_proto || lj_trace_state_load(J) != LJ_TRACE_RECORD ||
      J->parent || J->pt != sum_proto || bc_op(J->cur.startins) != BC_FORL)
    return;
  if (interruptions) {
    interruptions--;
    lj_trace_abort(G(L));
  } else if (!polls) {
    global_State *g = G(L);
    TGState *tg = L2TG(L);
    /* Deliver a counted FLUSH_SSB after the recorder installs its dispatch
    ** table, before the initiating FORL has advanced the index. */
    assert(gc2_hs_pending_acq(g) == 0 && lj_tg_reqmask_acq(tg) == 0);
    gc2_hs_actions_rel(g, LJ_GC2_HS_FLUSH_SSB);
    gc2_hs_pending_rel(g, 1);
    gc2_hs_epoch_rel(g, gc2_hs_epoch_acq(g)+1u);
    lj_tg_reqmask_rel(tg, LJ_GC2_HS_FLUSH_SSB);
    lj_tg_poll_rel(tg, 1);
    polls++;
  }
}

static void check_interrupted_recording(lua_State *L)
{
  assert(luaL_dostring(L,
    "jit.flush()\n"
    "function interrupted_sum(n) local s=0; for i=1,n do s=s+i end; return s end\n")
    == LUA_OK);
  lua_getglobal(L, "interrupted_sum");
  sum_proto = funcproto(funcV(L->top-1));
  interruptions = 16;
  lua_pushinteger(L, 250000);
  assert(lua_pcall(L, 1, 1, 0) == LUA_OK);
  assert(lua_tonumber(L, -1) == 31250125000.0);
  /* The same arithmetic loop must still be eligible once GC stops asking
  ** the recorder to leave. No bytecode or JIT settings have changed. */
  assert(interruptions == 0 && proto_trace_acq(sum_proto) != 0);
  lua_pop(L, 1);
  sum_proto = NULL;
}

static void check_record_start_poll(lua_State *L)
{
  int i;
  assert(luaL_dostring(L,
    "jit.on(); jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "function sum(n,s) for i=1,n do s=s+i end; return s end\n") == LUA_OK);
  lua_getglobal(L, "sum");
  sum_proto = funcproto(funcV(L->top-1));
  for (i = 0; i < 80; i++) {
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 80);
    lua_pushinteger(L, i);
    assert(lua_pcall(L, 2, 1, 0) == LUA_OK);
    assert(lua_tointeger(L, -1) == 3240+i);
    lua_pop(L, 1);
  }
  assert(polls == 1 && gc2_hs_pending_acq(G(L)) == 0);
  assert(proto_trace_acq(sum_proto) != 0);
  lua_pop(L, 1);
  sum_proto = NULL;
}

static void check_loop(lua_State *L, const char *name, BCOp original,
                       BCOp blacklisted, BCOp traced)
{
  global_State *g = G(L);
  TGState *tg = g->main_tg;
  ASMFunction saved = tg->dispatch[blacklisted];
  GCproto *pt;
  BCIns *pc, ins, disabled;
  BCPos i;

  lua_getglobal(L, name);
  pt = funcproto(funcV(L->top-1));
  for (i = 0; i < pt->sizebc; i++)
    if (bc_op(proto_bc(pt)[i]) == original)
      break;
  assert(i < pt->sizebc);
  pc = &proto_bc(pt)[i];
  ins = *pc;
  disabled = ins;
  setbc_op(&disabled, blacklisted);
  bc_publish(pc, disabled);
  /* Dispatch the previously decoded handler with the replacement word visible
  ** to its entry gate. The unchanged D field holds the real backward jump. */
  tg->dispatch[blacklisted] = tg->dispatch[GG_LEN_DDISP + traced];
  gc2_jit_phase_gate_rel(g, 0);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  gc2_jit_phase_gate_rel(g, 1);
  tg->dispatch[blacklisted] = saved;
  bc_publish(pc, ins);
  assert(lua_tointeger(L, -1) == 55);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "function numeric() local s=0; for i=1,10 do s=s+i end; return s end\n"
    "local function step(_,i) if i<10 then return i+1 end end\n"
    "function iterator() local s=0; for i in step,nil,0 do s=s+i end; return s end\n"
    "jit.off(); collectgarbage('stop')\n") == LUA_OK);
  check_loop(L, "numeric", BC_FORL, BC_IFORL, BC_JFORL);
  check_loop(L, "iterator", BC_ITERL, BC_IITERL, BC_JITERL);
  check_record_start_poll(L);
  check_interrupted_recording(L);
  lua_close(L);
  puts("t-jit-stale-loop-backedge OK");
  return 0;
}
