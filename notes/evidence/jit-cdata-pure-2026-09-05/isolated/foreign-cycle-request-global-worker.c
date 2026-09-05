#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
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
static uint64_t now_ns(void) { struct timespec ts;assert(clock_gettime(CLOCK_MONOTONIC,&ts)==0);return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec; }
static int gate_exit(lua_State *L) {
 int mutate=0;
 if(la_load32_acq(&requested) && gc2_jit_phase_gate_acq(testg)==0 && !la_load32_acq(&early_exit)) {
  at_exit=lua_tonumber(L,1);la_store32_rel(&early_exit,1);mutate=1;
 }
 lua_pushboolean(L,mutate);
 return 1;
}
static void *closer(void *unused) {
 uint64_t end=now_ns()+5000000000ull;(void)unused;
 while(lj_tg_load_jit_base(testtg)==NULL) {assert(!la_load32_acq(&finished));assert(now_ns()<end);sched_yield();}
 assert(gc2_phase_acq(testg)==LJ_GC2_IDLE);
 la_store32_rel(&saw_native,1);
 fprintf(stderr,"worker before set native=%d phase=%u gate=%u\n",lj_tg_load_jit_base(testtg)!=NULL,gc2_phase_acq(testg),gc2_jit_phase_gate_acq(testg));
 assert(lj_gc2_workers_set(testg,1)==1);
 fprintf(stderr,"worker after set native=%d phase=%u gate=%u\n",lj_tg_load_jit_base(testtg)!=NULL,gc2_phase_acq(testg),gc2_jit_phase_gate_acq(testg));
 la_store32_rel(&requested,1);
 lj_gc2_mark_begin(testg);
 fprintf(stderr,"worker after request native=%d phase=%u gate=%u leader=%u\n",lj_tg_load_jit_base(testtg)!=NULL,gc2_phase_acq(testg),gc2_jit_phase_gate_acq(testg),gc2_cycle_leader_acq(testg));
 return NULL;
}
int main(void) {
 lua_State *L=ljt_lua_newstate_openlibs();pthread_t t;
 testg=G(L);testtg=L2TG(L);
 lua_pushcfunction(L,gate_exit);lua_setglobal(L,"gate_exit");
 ljt_lua_dostring(L,
  "local ffi,util=require('ffi'),require('jit.util')\n"
  "p=ffi.new('struct {double x;double y;}',0,1);mt=debug.getmetatable(p);oldindex=mt.__index\n"
  "function run(p,n) local s=0;for i=0,n do if i>0 then p.x=i;s=s+p.x+p.y end end;return s end\n"
  "exits=0;calls=0;cut=0;local function replacement(p,k) calls=calls+1;return oldindex(p,k)+1000 end\n"
  "local function onexit() exits=exits+1;local x=oldindex(p,'x');if gate_exit(x) then cut=x;mt.__index=replacement end end\n"
  "jit.off(onexit,true);jit.opt.start('hotloop=1','hotexit=1');jit.attach(onexit,'texit')\n"
  "assert(run(p,80)==3320);assert(exits>0);assert(util.traceinfo(1))\n");
 /* Production MARK entry grants a native turn. Peer requests only its existing
 ** asynchronous gate close; it performs no Lua mutation or root scanning. */
 /* Parse/root the continuation before opening MARK; parser allocation can
 ** otherwise complete that cycle before native entry. */
 ljt_lua_loadstring(L,"local result=run(p,20000000);print('worker continuation',result,cut,calls);io.stdout:flush();assert(cut>0 and cut<20000000);assert(result==200000030000000+2000*(20000000-cut));assert(calls==2*(20000000-cut))\n");
 assert(gc2_phase_acq(testg)==LJ_GC2_IDLE);
 assert(lj_gc2_jit_entry_open(testg));
 assert(pthread_create(&t,NULL,closer,NULL)==0);
 ljt_lua_pcall(L,0,0,"phase continuation");
 la_store32_rel(&finished,1);assert(pthread_join(t,NULL)==0);
 assert(la_load32_acq(&saw_native));assert(la_load32_acq(&early_exit));
 assert(at_exit>0 && at_exit<20000000);
 ljt_lua_dostring(L,
  "calls=0;mt.__index=function(p,k) calls=calls+1;return oldindex(p,k)+1000 end\n"
  "collectgarbage('collect');exits=0;assert(run(p,80)==163320);assert(calls==160);assert(exits>0)\n"
  "mt.__index=oldindex\n");
 assert(gc2_phase_acq(testg)==LJ_GC2_IDLE);
 printf("global-worker early-exit=%.0f limit=20000000 exit-callback-mutation=1 continuation-calls=%.0f second-calls=160 final-IDLE\n",at_exit,2*(20000000-at_exit));
 assert(lj_gc2_workers_set(testg,0)==1);
 lua_close(L);return 0;
}
