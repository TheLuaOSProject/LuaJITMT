#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_vm.h"

/* GCC/x64 trap stepping pauses a real external last detach after its saved
** threshold load and before its global threshold store. It verifies the
** emitted MOV, its real base/displacement, and captured MAX source register.
** No GC state, request, threshold, count, or native certificate is fabricated.
*/
typedef struct Probe {
  global_State *g;
  lua_State *child;
  uint64_t starts;
  uint32_t arm, steps, paused, release, attached, leave, one_done, violation;
} Probe;
static Probe probe;

static void wait_for(const uint32_t *p)
{
  while (!la_load32_acq(p)) la_cpu_pause();
}

static void step_start(int signo, siginfo_t *info, void *opaque)
{
  ucontext_t *ctx = opaque;
  (void)signo; (void)info;
  ctx->uc_mcontext.gregs[REG_EFL] |= 0x100u;
}

static void step_trap(int signo, siginfo_t *info, void *opaque)
{
  ucontext_t *ctx = opaque;
  global_State *g = probe.g;
  (void)signo; (void)info;
  const unsigned char *pc = (const unsigned char *)ctx->uc_mcontext.gregs[REG_RIP];
  (void)la_add32_rlx(&probe.steps, 1);
  if (mt_live_acq(g) == 0 && lj_gc_mt_threshold_load(g) == LJ_MAX_MEM &&
      pc[0] == 0x48 && pc[1] == 0x89 && pc[2] == 0x47 &&
      pc[3] == (unsigned char)offsetof(global_State, gc.threshold) &&
      (uintptr_t)ctx->uc_mcontext.gregs[REG_RDI] == (uintptr_t)g &&
      (uint64_t)ctx->uc_mcontext.gregs[REG_RAX] == LJ_MAX_MEM) {
    ctx->uc_mcontext.gregs[REG_EFL] &= ~(greg_t)0x100u;
    la_store32_rel(&probe.paused, 1);
    wait_for(&probe.release);
  }
}

static void *attach_main(void *unused)
{
  TGState *tg;
  (void)unused;
  assert(lj_threading_attach(probe.child));
  tg = lj_thr_get_tg();
  assert(tg != NULL);
  lj_native_enter(tg);
  la_store32_rel(&probe.attached, 1);
  wait_for(&probe.leave);
  (void)lj_native_leave_tg(tg);
  if (la_load32_acq(&probe.arm)) assert(raise(SIGUSR1) == 0);
  lj_threading_detach(probe.child, 1);
  assert(lj_thr_get_tg() == NULL);
  return NULL;
}

static void snapshot(lua_State *L, const char *stage)
{
  global_State *g = G(L);
  printf("{\"stage\":\"%s\",\"running_query\":%d,\"starts\":%" PRIu64
         ",\"completed\":%" PRIu64 ",\"phase\":%u,\"leader\":%u,"
         "\"live\":%u,\"entering\":%u,\"threshold\":%" PRIu64
         ",\"mt_threshold\":%" PRIu64 ",\"local_total\":%" PRIu64
         ",\"since\":%" PRIu64 ",\"hard\":%" PRIu64 ",\"steps\":%u}\n",
         stage, lua_gc(L, LUA_GCISRUNNING, 0), gc2_cycle_starts_acq(g),
         gc2_sweep_to_idle_acq(g), gc2_phase_acq(g), gc2_cycle_leader_acq(g),
         mt_live_acq(g), mt_entering_acq(g), (uint64_t)lj_gc_threshold_load(g),
         (uint64_t)lj_gc_mt_threshold_load(g), lj_tg_local_total_acq(L2TG(L)),
         lj_gc2_alloc_since_load(g), lj_gc2_hard_load(g), la_load32_acq(&probe.steps));
}

int main(void)
{
  lua_State *L;
  global_State *g;
  struct sigaction sa;
  pthread_t attach;
  uint32_t i;
  int fn;
  setvbuf(stdout, NULL, _IOLBF, 0);
  alarm(20);
  L = luaL_newstate(); assert(L);
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF));
  g = G(L); probe.g = g;
  assert(luaL_dostring(L, "return function() local t={} return t end") == 0);
  fn = luaL_ref(L, LUA_REGISTRYINDEX);
  probe.child = lua_newthread(L);  /* Main stack roots it throughout. */
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(pthread_create(&attach, NULL, attach_main, NULL) == 0);
  wait_for(&probe.attached);
  assert(lj_gc_mt_threshold_load(g) != LJ_MAX_MEM);
  for (i = 0; i < 8192 && gc2_cycle_leader_acq(g) == 0; i++) {
    char buf[65];
    int n = snprintf(buf, sizeof(buf), "stop-overlap:%08x:", i);
    assert(n > 0 && n < 64);
    memset(buf + n, 'x', 64u - (unsigned)n);
    lua_pushlstring(L, buf, 64); lua_pop(L, 1);
  }
  assert(gc2_cycle_leader_acq(g) != 0 && gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lj_tg_local_total_acq(L2TG(L)) == 0);
  assert(lj_gc2_alloc_since_load(g) <= lj_gc2_hard_load(g));
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  assert(!lua_gc(L, LUA_GCISRUNNING, 0));
  assert(lj_gc_mt_threshold_load(g) == LJ_MAX_MEM);
  snapshot(L, "stopped_before_last_detach");
  probe.starts = gc2_cycle_starts_acq(g);
  memset(&sa, 0, sizeof(sa)); sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask); sa.sa_sigaction = step_start;
  assert(sigaction(SIGUSR1, &sa, NULL) == 0);
  sa.sa_sigaction = step_trap;
  assert(sigaction(SIGTRAP, &sa, NULL) == 0);
  la_store32_rel(&probe.arm, 1);
  la_store32_rel(&probe.leave, 1);
  wait_for(&probe.paused);
  assert(mt_live_acq(g) == 0);
  snapshot(L, "last_detach_captured_stopped_threshold");
  assert(lua_gc(L, LUA_GCRESTART, 0) == 0);
  assert(lua_gc(L, LUA_GCISRUNNING, 0));
  assert(lj_gc_threshold_load(g) != LJ_MAX_MEM);
  snapshot(L, "restart_completed_before_detach_store");
  la_store32_rel(&probe.release, 1);
  assert(pthread_join(attach, NULL) == 0);
  snapshot(L, "delayed_detach_overwrote_restart");
  la_store32_rel(&probe.violation, !lua_gc(L, LUA_GCISRUNNING, 0));
  for (i = 0; i < 8192; i++) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, fn);
    assert(lua_pcall(L, 0, 1, 0) == 0);
    assert(lua_istable(L, -1)); lua_pop(L, 1);
  }
  snapshot(L, "after_8192_production_tnew");
  assert(mt_live_acq(g) == 0 && mt_entering_acq(g) == 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  snapshot(L, "cleaned_up");
  printf("RESTART_LAST_DETACH violation=%u\n", la_load32_acq(&probe.violation));
  lua_close(L);
  return la_load32_acq(&probe.violation) ? 43 : 0;
}
