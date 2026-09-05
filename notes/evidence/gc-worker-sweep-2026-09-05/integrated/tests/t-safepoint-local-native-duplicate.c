#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_lex.h"

typedef struct Probe {
  global_State *g;
  TGState owner;
  uint32_t ready, armed, refused, consumed, remote_scan;
  uint32_t release_scan, detached, teardown_overlap, observed_wait;
  uint32_t local_clear, real_remote_scan, owner_scan;
  pid_t owner_sysid;
} Probe;
static Probe ctx;
static uint64_t now_ns(void)
{
  struct timespec t;
  assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static void until(const uint32_t *p, uint32_t value)
{
  uint64_t end = now_ns() + 5000000000ull;
  while (la_load32_acq(p) != value) {
    assert(now_ns() < end);
    la_cpu_pause();
  }
}
static void completion_hook(global_State *g, TGState *tg,
                            uint64_t epoch, uint32_t point)
{
  Probe *p = &ctx;
  if (g != p->g || tg != &p->owner || !la_load32_acq(&p->armed)) return;
  if (point == LJ_SAFEPOINT_COMPLETION_TEST_CONSUMED &&
      lj_thr_get_tg() == tg && lj_tg_hs_epoch_ack_acq(tg) != epoch) {
    assert(lj_tg_in_native_acq(tg) == 1);
    {
      /* A late signal or remote refusal can republish this same counted
      ** request after our consume and before its epoch claim. Observe only
      ** valid masks here; the original owner/remote overlap and final pending
      ** assertions below prove completion of every admitted action. */
      uint32_t mask = lj_tg_reqmask_acq(tg);
      if (mask != 0) {
        uint32_t expected = LJ_GC2_HS_SCAN_OWNER_ROOTS|LJ_GC2_HS_FLUSH_SSB;
        assert(mask == expected);
        assert(gc2_hs_epoch_acq(g) == epoch);
        assert(gc2_hs_actions_acq(g) == expected);
      }
    }
    la_store32_rel(&p->consumed, 1);
    until(&p->remote_scan, 1);
  }
  if (point == LJ_SAFEPOINT_COMPLETION_TEST_CLEARED && lj_thr_get_tg() == tg) {
    assert(la_load32_acq(&p->remote_scan));
    assert(!la_load32_acq(&p->release_scan));
    assert(lj_tg_reqmask_acq(tg) == 0);
    assert(lj_tg_poll_acq(tg) == 0);
    la_store32_rel(&p->local_clear, 1);
  }
}
extern int __real_lj_gc2_scan_cycle_owner_tg_roots_native_parked(global_State *, TGState *);
int __wrap_lj_gc2_scan_cycle_owner_tg_roots_native_parked(global_State *g, TGState *tg)
{
  if (g == ctx.g && tg == &ctx.owner && la_load32_acq(&ctx.armed) &&
      !la_load32_acq(&ctx.consumed)) {
    /* Keep legitimate remote refusal until the owner actually consumes its
    ** requeued request; scheduling may make the leader retry first. The target
    ** performs its real local acknowledgement from lj_tg_detach below. */
    la_store32_rel(&ctx.refused, 1);
    return 0;
  }
  return __real_lj_gc2_scan_cycle_owner_tg_roots_native_parked(g, tg);
}
extern void __real_lj_lex_gc2_markroots(global_State *, TGState *);
void __wrap_lj_lex_gc2_markroots(global_State *g, TGState *tg)
{
  Probe *p = &ctx;
  if (g == p->g && tg == &p->owner && la_load32_acq(&p->armed)) {
    if (lj_thr_get_tg() != tg) {
      /* This is inside actual owner-root traversal, after its DEAD check and
      ** tmpbuf access but before parser/anchor/thread-root accesses. */
      assert(la_load32_acq(&p->consumed));
      assert(lj_tg_in_native_acq(tg) == 1);
      assert(lj_tg_reqmask_acq(tg) == 0);
      assert(lj_tg_hs_epoch_ack_acq(tg) != gc2_hs_epoch_acq(g));
      (void)la_add32_rlx(&p->real_remote_scan, 1);
      la_store32_rel(&p->remote_scan, 1);
      until(&p->release_scan, 1);
    } else {
      (void)la_add32_rlx(&p->owner_scan, 1);
    }
  }
  __real_lj_lex_gc2_markroots(g, tg);
}
static void *owner(void *arg)
{
  Probe *p = arg;
  TGState *tg = &p->owner;
  lj_tg_init_thread(p->g, tg, NULL, 0);
  lj_tg_tid_rel(tg, lj_thr_newid());
  lj_arena_alloc_owner_rel(&tg->alloc, lj_tg_tid_acq(tg));
  lj_thr_set_tg(tg);
  p->owner_sysid = (pid_t)syscall(SYS_gettid);
  /* Exercise an attached native owner whose detach services an outstanding
  ** TG-only acknowledgement. GC workers now close their startup native scope
  ** first; this fixture retains the local pre-claim duplicate-action hold. */
  lj_native_enter(tg);
  lj_tg_attach(p->g, tg);
  la_store32_rel(&p->ready, 1);
  until(&p->refused, 1);
  while (lj_tg_reqmask_acq(tg) == 0) la_cpu_pause();
  lj_tg_detach(p->g, tg);
  if (!la_load32_acq(&p->release_scan))
    la_store32_rel(&p->teardown_overlap, 1);
  assert(lj_tg_flags_test_acq(tg, TGF_DEAD));
  assert(lj_tg_in_native_acq(tg) == 0);
  la_store32_rel(&p->detached, 1);
  lj_thr_set_tg(NULL);
  return NULL;
}
static void *observer(void *arg)
{
  Probe *p = arg;
  char path[96], line[320];
  uint64_t end;
  until(&p->remote_scan, 1);
  end = now_ns() + 5000000000ull;
  snprintf(path, sizeof(path), "/proc/self/task/%ld/syscall", (long)p->owner_sysid);
  for (;;) {
    long nr;
    unsigned long long addr, op, val, timeout_arg;
    FILE *f;
    if (la_load32_acq(&p->detached)) {
      assert(la_load32_acq(&p->teardown_overlap));
      break;
    }
    assert(now_ns() < end);
    f = fopen(path, "r");
    if (f) {
      if (!fgets(line, sizeof(line), f)) line[0] = 0;
      fclose(f);
      if (sscanf(line, "%ld %llx %llx %llx %llx", &nr, &addr, &op, &val, &timeout_arg) == 5 &&
          nr == SYS_futex && addr == (unsigned long long)(uintptr_t)&p->owner.poll &&
          op == FUTEX_WAIT_PRIVATE && val == 1 && timeout_arg == 0) {
        assert(!la_load32_acq(&p->local_clear));
        assert(lj_tg_in_native_acq(&p->owner) == 1);
        assert(lj_tg_reqmask_acq(&p->owner) == 0);
        assert(lj_tg_hs_epoch_ack_acq(&p->owner) == gc2_hs_epoch_acq(p->g));
        printf("local native winner remains in exact consumed-poll wait: %s", line);
        la_store32_rel(&p->observed_wait, 1);
        break;
      }
    }
    usleep(1000);
  }
  la_store32_rel(&p->release_scan, 1);
  return NULL;
}
int main(void)
{
  lua_State *L;
  pthread_t w, o;
  uint32_t signaled;
  alarm(15);
  setvbuf(stdout, NULL, _IOLBF, 0);
  L = luaL_newstate(); assert(L); luaL_openlibs(L);
  lua_gc(L, LUA_GCCOLLECT, 0); lua_gc(L, LUA_GCSTOP, 0);
  ctx.g = G(L);
  assert(pthread_create(&w, NULL, owner, &ctx) == 0);
  until(&ctx.ready, 1);
  lj_gc2_mark_begin(ctx.g);
  assert(gc2_phase_acq(ctx.g) == LJ_GC2_MARK);
  la_store32_rel(&ctx.armed, 1);
  lj_safepoint_test_completion_hook(completion_hook);
  assert(pthread_create(&o, NULL, observer, &ctx) == 0);
  signaled = lj_safepoint_handshake(ctx.g,
      LJ_GC2_HS_SCAN_OWNER_ROOTS|LJ_GC2_HS_FLUSH_SSB);
  assert(pthread_join(w, NULL) == 0);
  assert(pthread_join(o, NULL) == 0);
  la_store32_rel(&ctx.armed, 0);
  lj_safepoint_test_completion_hook(NULL);
  printf("signaled=%u real-remote-scan=%u owner-scan=%u local-clear=%u teardown-during-scan=%u exact-owner-wait=%u pending=%u\n",
      signaled,ctx.real_remote_scan,ctx.owner_scan,ctx.local_clear,
      ctx.teardown_overlap,ctx.observed_wait,gc2_hs_pending_acq(ctx.g));
  assert(ctx.real_remote_scan == 1 && ctx.owner_scan == 1);
  assert(gc2_hs_pending_acq(ctx.g) == 0 && gc2_hs_leader_acq(ctx.g) == 0);
  assert(!ctx.teardown_overlap && ctx.observed_wait);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_close(L);
  puts("local native duplicate scan hold PASS");
  return 0;
}
