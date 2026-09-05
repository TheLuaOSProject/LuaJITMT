/* A first real external attachment must interrupt a warmed pre-MT trace.
** The owner exit callback records the exact completed iteration, then stops
** the loop. Requiring iteration < limit proves responsiveness without a
** wall-clock threshold; the unmodified runtime reaches the finite limit.
** Exercise both IR_LOOP polling and the optimizer-disabled terminal poll.
*/
#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_atomic.h"
#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_trace.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"

typedef struct Probe {
  lua_State *L, *child;
  global_State *g;
  TGState *owner;
  uint32_t go, requested, saw_native, attached, detach_go, exit_seen;
  double exit_at;
  unsigned exit_trace, exit_no;
  uint64_t request_ns, attached_ns;
} Probe;

static Probe *active;

static uint64_t now_ns(void)
{
  struct timespec t;
  assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static void pause_briefly(void)
{
  struct timespec t = {0, 100000};
  (void)nanosleep(&t, NULL);
}

static int record_exit(lua_State *L)
{
  Probe *p = active;
  int take = la_load32_acq(&p->requested) && !p->exit_seen;
  if (take) {
    p->exit_at = lua_tonumber(L, 1);
    p->exit_trace = (unsigned)lua_tointeger(L, 2);
    p->exit_no = (unsigned)lua_tointeger(L, 3);
    p->exit_seen = 1;
  }
  lua_pushboolean(L, take);
  return 1;
}

static void *attach_worker(void *arg)
{
  Probe *p = (Probe *)arg;
  uint64_t deadline = now_ns() + UINT64_C(10000000000);
  while (!la_load32_acq(&p->go) || lj_tg_load_jit_base(p->owner) == NULL) {
    assert(now_ns() < deadline);
    pause_briefly();
  }
  la_store32_rel(&p->saw_native, 1);
  la_store64_rel(&p->request_ns, now_ns());
  la_store32_rel(&p->requested, 1);
  assert(lj_threading_attach(p->child));
  la_store64_rel(&p->attached_ns, now_ns());
  la_store32_rel(&p->attached, 1);
  lj_native_enter(lj_thr_get_tg());
  while (!la_load32_acq(&p->detach_go)) pause_briefly();
  (void)lj_native_leave_tg(lj_thr_get_tg());
  lj_threading_detach(p->child, 1);
  return NULL;
}

int main(int argc, char **argv)
{
  const double limit = 1000000000.0;
  Probe p = {0};
  pthread_t attacher;
  double result;
  int rc, progress;
  int no_loop = argc == 2 && strcmp(argv[1], "noloop") == 0;
  assert(argc == 1 || no_loop);
  alarm(25);
  setvbuf(stdout, NULL, _IOLBF, 0);
  active = &p;
  p.L = luaL_newstate(); assert(p.L); luaL_openlibs(p.L);
  p.g = G(p.L); p.owner = L2TG(p.L);
  lua_gc(p.L, LUA_GCSTOP, 0);
  if (no_loop) {
    rc = luaL_dostring(p.L, "require('jit').opt.start('-loop')");
    assert(rc == 0);
  }
  lua_pushcfunction(p.L, record_exit); lua_setglobal(p.L, "record_exit");
  rc = luaL_dostring(p.L,
    "local ffi=require('ffi');local jit=require('jit');"
    "local util=require('jit.util');p=ffi.new('double[2]');local exits=0;"
    "local function witness(tr,ex) exits=exits+1;"
    "if record_exit(p[0],tr,ex) then p[1]=1 end end;"
    "jit.off(witness,true);jit.opt.start('hotloop=1','hotexit=255');"
    "jit.attach(witness,'texit');"
    "function spin(n) for i=1,n do if p[1]~=0 then break end;p[0]=i end;"
    "return p[0] end;"
    "assert(spin(1000)==1000);assert(exits>0,'warm native witness');"
    "assert(util.traceinfo(1));p[0]=0;p[1]=0;print('warm native exits',exits)");
  if (rc) { fprintf(stderr, "%s\n", lua_tostring(p.L, -1)); return 2; }
  {
    GCtrace *T = traceref_safe(G2J(p.g), 1);
    IRIns *ir;
    IRRef i, nins;
    uint32_t polls = 0, loops = 0;
    assert(T && trace_runnable_acq(T, 1));
    ir = trace_ir_acq(T); nins = trace_nins_acq(T);
    for (i = REF_FIRST; i < nins; i++) {
      if (ir[i].o == IR_LOOP) loops++;
      if (ir[i].o == IR_XPOLL) { assert(ir[i].op1 == 0); polls++; }
    }
    assert(polls > 0 && (no_loop ? loops == 0 : loops > 0));
    printf("warm root 1: %u XPOLL instructions, all mode 0; LOOP=%u\n",
           polls, loops);
  }
  assert(mt_active_acq(p.g) == 0 && mt_entering_acq(p.g) == 0);
  assert(gc2_n_threads_acq(p.g) == 1 && gc2_n_workers_acq(p.g) == 0);
  p.child = lua_newthread(p.L); assert(p.child);
  lua_getglobal(p.L, "spin"); lua_pushnumber(p.L, limit);
  assert(pthread_create(&attacher, NULL, attach_worker, &p) == 0);
  la_store32_rel(&p.go, 1);
  rc = lua_pcall(p.L, 1, 1, 0);
  if (rc) fprintf(stderr, "loop error: %s\n", lua_tostring(p.L, -1));
  assert(rc == 0);
  result = lua_tonumber(p.L, -1);
  lj_native_enter(p.owner);
  la_store32_rel(&p.detach_go, 1);
  assert(pthread_join(attacher, NULL) == 0);
  (void)lj_native_leave(p.L);
  assert(la_load32_acq(&p.saw_native) && la_load32_acq(&p.attached));
  assert(mt_active_acq(p.g) && p.exit_seen && p.exit_trace == 1);
  assert(result == p.exit_at && result > 0 && result <= limit);
  progress = p.exit_at < limit;
  printf("first attach: exit trace=%u snap=%u iteration=%.0f limit=%.0f "
         "result=%.0f attach-ms=%.3f early=%d\n",
         p.exit_trace, p.exit_no, p.exit_at, limit, result,
         (double)(la_load64_acq(&p.attached_ns)-la_load64_acq(&p.request_ns))/1e6,
         progress);
  lua_gc(p.L, LUA_GCCOLLECT, 0);
  lua_settop(p.L, 0); lua_close(p.L);
  if (!progress) {
    fprintf(stderr, "FAIL: attachment required natural loop completion\n");
    return 3;
  }
  puts("first-attach mode-0 native backedge progress PASS");
  return 0;
}
