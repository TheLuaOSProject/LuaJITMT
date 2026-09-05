#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_atomic.h"
#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"

typedef struct Probe {
  lua_State *L, *child;
  global_State *g;
  TGState *main_tg;
  uint32_t go, attached, detach_go, saw_pending, observed;
  uint64_t start_ns, attached_ns, observed_ns;
} Probe;

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

static void *attach_worker(void *arg)
{
  Probe *p = (Probe *)arg;
  uint64_t deadline = now_ns() + UINT64_C(10000000000);
  while (!la_load32_acq(&p->go) || lj_tg_load_jit_base(p->main_tg) == NULL) {
    assert(now_ns() < deadline);
    pause_briefly();
  }
  la_store64_rel(&p->start_ns, now_ns());
  assert(lj_threading_attach(p->child));
  la_store64_rel(&p->attached_ns, now_ns());
  la_store32_rel(&p->attached, 1);
  lj_native_enter(lj_thr_get_tg());
  while (!la_load32_acq(&p->detach_go)) pause_briefly();
  (void)lj_native_leave_tg(lj_thr_get_tg());
  lj_threading_detach(p->child, 1);
  return NULL;
}

static int pending(Probe *p)
{
  return lj_tg_load_jit_base(p->main_tg) != NULL &&
    lj_tg_poll_acq(p->main_tg) != 0 &&
    lj_tg_reqmask_acq(p->main_tg) != 0 &&
    gc2_hs_leader_acq(p->g) != 0 &&
    gc2_jit_phase_gate_acq(p->g) != 0 &&
    mt_entering_acq(p->g) != 0 && mt_active_acq(p->g) == 0 &&
    la_load32_acq(&p->attached) == 0;
}

static void *observe(void *arg)
{
  Probe *p = (Probe *)arg;
  uint64_t deadline = now_ns() + UINT64_C(10000000000), began = 0;
  while (now_ns() < deadline && !la_load32_acq(&p->attached)) {
    if (pending(p)) {
      uint64_t now = now_ns();
      la_store32_rel(&p->saw_pending, 1);
      if (!began) began = now;
      if (now - began >= UINT64_C(200000000)) {
        la_store64_rel(&p->observed_ns, now - began);
        la_store32_rel(&p->observed, 1);
        printf("pending first attach for %.3f ms: native=1 poll=%u "
               "reqmask=%u leader=%u gate=%u entering=%u active=%u\n",
               (double)(now - began) / 1e6,
               lj_tg_poll_acq(p->main_tg), lj_tg_reqmask_acq(p->main_tg),
               gc2_hs_leader_acq(p->g), gc2_jit_phase_gate_acq(p->g),
               mt_entering_acq(p->g), mt_active_acq(p->g));
        return NULL;
      }
    } else {
      began = 0;
    }
    pause_briefly();
  }
  return NULL;
}

int main(void)
{
  Probe p = {0};
  pthread_t attacher, observer;
  uint64_t before, after;
  int rc;
  alarm(25);
  setvbuf(stdout, NULL, _IOLBF, 0);
  p.L = luaL_newstate(); assert(p.L); luaL_openlibs(p.L);
  p.g = G(p.L); p.main_tg = L2TG(p.L);
  lua_gc(p.L, LUA_GCSTOP, 0);
  rc = luaL_dostring(p.L,
    "local jit=require('jit');local u=require('jit.util');local exits=0;"
    "local function witness() exits=exits+1 end;jit.off(witness);"
    "jit.opt.start('hotloop=1');jit.attach(witness,'texit');"
    "function spin(n) local x=0;for i=1,n do x=x+1 end;return x end;"
    "assert(spin(1000)==1000);assert(exits>0,'warm native witness');"
    "local found=false;for i=1,100 do if u.traceinfo(i) then found=true end end;"
    "assert(found);jit.attach(witness);print('warm native exits',exits)");
  if (rc) { fprintf(stderr, "%s\n", lua_tostring(p.L, -1)); return 2; }
  assert(mt_active_acq(p.g) == 0 && mt_entering_acq(p.g) == 0);
  assert(gc2_n_threads_acq(p.g) == 1 && gc2_n_workers_acq(p.g) == 0);
  p.child = lua_newthread(p.L); assert(p.child);
  lua_getglobal(p.L, "spin"); lua_pushnumber(p.L, 10000000000.0);
  assert(pthread_create(&attacher, NULL, attach_worker, &p) == 0);
  assert(pthread_create(&observer, NULL, observe, &p) == 0);
  before = now_ns(); la_store32_rel(&p.go, 1);
  rc = lua_pcall(p.L, 1, 1, 0);
  after = now_ns();
  if (rc) fprintf(stderr, "loop error: %s\n", lua_tostring(p.L, -1));
  assert(rc == 0 && lua_tonumber(p.L, -1) == 10000000000.0);
  lj_native_enter(p.main_tg);
  la_store32_rel(&p.detach_go, 1);
  assert(pthread_join(attacher, NULL) == 0);
  assert(pthread_join(observer, NULL) == 0);
  (void)lj_native_leave(p.L);
  assert(la_load32_acq(&p.attached) && mt_active_acq(p.g));
  printf("loop %.3f ms; attach %.3f ms; observed=%u saw_pending=%u\n",
         (double)(after-before)/1e6,
         (double)(la_load64_acq(&p.attached_ns)-la_load64_acq(&p.start_ns))/1e6,
         la_load32_acq(&p.observed), la_load32_acq(&p.saw_pending));
  assert(la_load32_acq(&p.observed));
  lua_settop(p.L, 0); lua_close(p.L);
  return 0;
}
