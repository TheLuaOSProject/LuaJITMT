/* Exit an active pure cdata root before owner method mutation. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_atomic.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"
static global_State *testg;
static TGState *testtg;
static uint32_t requested, saw_native, early_exit, finished;
static double at_exit;
static int start_worker;
static uint64_t now_ns(void)
{
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static int gate_exit(lua_State *L)
{
  int mutate = 0;
  if (la_load32_acq(&requested) && gc2_jit_phase_gate_acq(testg) == 0 &&
      !la_load32_acq(&early_exit)) {
    at_exit = lua_tonumber(L, 1);
    la_store32_rel(&early_exit, 1);
    mutate = 1;
  }
  lua_pushboolean(L, mutate);
  return 1;
}
static void *closer(void *unused)
{
  uint64_t end = now_ns() + 5000000000ull;
  (void)unused;
  while (lj_tg_load_jit_base(testtg) == NULL) {
    assert(!la_load32_acq(&finished));
    assert(now_ns() < end);
    sched_yield();
  }
  assert(gc2_phase_acq(testg) == LJ_GC2_MARK);
  la_store32_rel(&saw_native, 1);
  la_store32_rel(&requested, 1);
  if (start_worker) {
    /* This public global API does not perform an L-based trace flush. Its real
    ** MARK worker must close the phase gate before payload work. */
    assert(lj_gc2_workers_set(testg, 1) == 1);
    while (!la_load32_acq(&early_exit)) {
      assert(now_ns() < end);
      sched_yield();
    }
    assert(lj_gc2_workers_set(testg, 0) == 1);
  } else {
    lj_gc2_jit_mark_request_exit(testg);
  }
  return NULL;
}
int main(int argc, char **argv)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  pthread_t t;
  assert(argc == 2 && (!strcmp(argv[1], "gate") || !strcmp(argv[1], "worker")));
  start_worker = !strcmp(argv[1], "worker");
  testg = G(L);
  testtg = L2TG(L);
  lua_pushcfunction(L, gate_exit);
  lua_setglobal(L, "gate_exit");
  ljt_lua_dostring(
      L,
      "local "
      "ffi,util,vmdef,bit=require('ffi'),require('jit.util'),require('jit."
      "vmdef'),require('bit')\n"
      "p=ffi.new('struct {double x;double "
      "y;}',0,1);mt=debug.getmetatable(p);oldindex=mt.__index\n"
      "function run(p,n) local s=0;for i=0,n do if i>0 then p.x=i;s=s+p.x+p.y "
      "end end;return s end\n"
      "exits=0;calls=0;cut=0;local function replacement(p,k) "
      "calls=calls+1;return oldindex(p,k)+1000 end\n"
      "local function onexit() exits=exits+1;local x=oldindex(p,'x');if "
      "gate_exit(x) then cut=x;mt.__index=replacement end end\n"
      "jit.off(onexit,true);jit.opt.start('hotloop=1','hotexit=1');jit.attach("
      "onexit,'texit')\n"
      "assert(run(p,80)==3320);assert(exits>0)\n"
      "local info=assert(util.traceinfo(1));local loop,pre,post,poll=0,0,0,0\n"
      "assert(info.link==1 and info.linktype=='loop')\n"
      "for r=1,info.nins do local _,ot,a,b=util.traceir(1,r);if ot then\n"
      " local op=vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6)\n"
      " if op=='LOOP  ' then loop=r end\n"
      " if op=='XPOLL ' then assert(a==0);poll=poll+1 end\n"
      " if op=='FLOAD ' and a==-1 and b==276 then if loop==0 then pre=pre+1 "
      "else post=post+1 end end\n"
      "end end\n"
      "assert(loop>0 and pre==1 and post==0 and poll==1,'gate must challenge "
      "the hoisted native root')\n");
  /* Production MARK entry grants a native turn. The peer either requests
  ** the existing asynchronous gate close or publishes a real global worker.
  ** Method mutation always happens on the Lua owner after native exit. */
  /* Parse/root the continuation before opening MARK; parser allocation can
  ** otherwise complete that cycle before native entry. */
  ljt_lua_loadstring(L, "local result=run(p,2000000);assert(cut>0 and "
                        "cut<2000000);assert(result==2000003000000+2000*("
                        "2000000-cut));assert(calls==2*(2000000-cut))\n");
  lj_gc2_mark_begin(testg);
  assert(gc2_phase_acq(testg) == LJ_GC2_MARK);
  assert(lj_gc2_jit_entry_open(testg));
  assert(pthread_create(&t, NULL, closer, NULL) == 0);
  ljt_lua_pcall(L, 0, 0, "phase continuation");
  la_store32_rel(&finished, 1);
  assert(pthread_join(t, NULL) == 0);
  assert(la_load32_acq(&saw_native));
  assert(la_load32_acq(&early_exit));
  assert(at_exit > 0 && at_exit < 2000000);
  ljt_lua_dostring(L, "calls=0;mt.__index=function(p,k) calls=calls+1;return "
                      "oldindex(p,k)+1000 end\n"
                      "collectgarbage('collect');exits=0;assert(run(p,80)=="
                      "163320);assert(calls==160);assert(exits>0)\n"
                      "mt.__index=oldindex\n");
  assert(gc2_phase_acq(testg) == LJ_GC2_IDLE);
  printf("pure-phase %s early-exit=%.0f limit=2000000 exit-callback-mutation=1 "
         "continuation-calls=%.0f second-calls=160 final-IDLE\n",
         argv[1], at_exit, 2 * (2000000 - at_exit));
  assert(gc2_n_workers_acq(testg) == 0);
  lua_close(L);
  return 0;
}
