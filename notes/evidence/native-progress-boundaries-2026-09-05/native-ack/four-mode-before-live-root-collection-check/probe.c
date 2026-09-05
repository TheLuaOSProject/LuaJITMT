#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"

/* An observation-only Linux probe. The only interposition pauses an already
** admitted real remote-native call (or its completed handshake's global pass).
** It does not manufacture request, epoch, pending, native or state ownership. */
typedef struct Probe {
  global_State *g;
  TGState *owner;
  TGState peer;
  lua_State *L;
  GCtab *root;
  LJStateOwner owner_word;
  uint64_t epoch;
  uint32_t mode, paused, release, returned, peer_done, allow_detach;
  uint32_t observed_futex, scan_calls, global_calls, flush_calls, armed;
  int scan_result;
  pid_t owner_sysid;
  pthread_t peer_thread, observer;
} Probe;
static Probe ctx;
static void until(const uint32_t *p, uint32_t v)
{
  while (la_load32_acq(p) != v) la_cpu_pause();
}
static void pause_owner(Probe *p)
{
  assert(lj_thr_get_tg() == &p->peer);
  assert(gc2_hs_leader_acq(p->g) == lj_tg_tid_acq(&p->peer));
  assert(lj_tg_reqmask_acq(p->owner) == 0);
  assert(lj_tg_poll_acq(p->owner) == 1);
  assert(lj_tg_in_native_acq(p->owner) == 1);
  assert(lj_state_owner_word_acq(p->L) == p->owner_word);
  p->epoch = gc2_hs_epoch_acq(p->g);
  if (p->mode == 2) {
    assert(gc2_hs_pending_acq(p->g) == 0);
    assert(lj_tg_hs_epoch_ack_acq(p->owner) == p->epoch);
  } else {
    assert(gc2_hs_pending_acq(p->g) != 0);
    if (p->mode == 3)
      assert(lj_tg_hs_epoch_ack_acq(p->owner) == p->epoch);
    else
      assert(lj_tg_hs_epoch_ack_acq(p->owner) != p->epoch);
  }
  la_store32_rel(&p->paused, 1);
  until(&p->release, 1);
  assert(lj_state_owner_word_acq(p->L) == p->owner_word);
  assert(lj_tg_in_native_acq(p->owner) == 0);
  assert(la_load32_acq(&p->returned) == 0);
}
extern int __real_lj_gc2_scan_cycle_owner_tg_roots_native_parked(global_State *, TGState *);
int __wrap_lj_gc2_scan_cycle_owner_tg_roots_native_parked(global_State *g, TGState *tg)
{
  int r;
  if (g != ctx.g || tg != ctx.owner || !la_load32_acq(&ctx.armed))
    return __real_lj_gc2_scan_cycle_owner_tg_roots_native_parked(g, tg);
  ctx.scan_calls++;
  assert(ctx.scan_calls == 1);
  if (ctx.mode == 0) pause_owner(&ctx);
  r = __real_lj_gc2_scan_cycle_owner_tg_roots_native_parked(g, tg);
  ctx.scan_result = r;
  assert(r == 1);
  assert(lj_gc2_ismarked(g, obj2gco(ctx.root)) == 1);
  if (ctx.mode == 1) pause_owner(&ctx);
  return r;
}
extern void __real_lj_gc2_scan_cycle_global_roots(global_State *);
void __wrap_lj_gc2_scan_cycle_global_roots(global_State *g)
{
  if (g == ctx.g && la_load32_acq(&ctx.armed)) {
    ctx.global_calls++;
    if (ctx.mode == 2) {
      assert(ctx.scan_calls == 1 && ctx.scan_result == 1);
      pause_owner(&ctx);
    }
  }
  __real_lj_gc2_scan_cycle_global_roots(g);
}
extern uint32_t __real_lj_gc2_flush_ssb(global_State *, TGState *);
uint32_t __wrap_lj_gc2_flush_ssb(global_State *g, TGState *tg)
{
  if (g == ctx.g && tg == ctx.owner && ctx.mode == 3 &&
      la_load32_acq(&ctx.armed) && lj_thr_get_tg() == &ctx.peer) {
    ctx.flush_calls++;
    assert(ctx.flush_calls == 1 && ctx.scan_calls == 1 && ctx.scan_result == 1);
    pause_owner(&ctx);
  }
  return __real_lj_gc2_flush_ssb(g, tg);
}
static void *leader(void *arg)
{
  Probe *p = arg;
  uint32_t n;
  lj_tg_init_thread(p->g, &p->peer, NULL, 0);
  lj_tg_tid_rel(&p->peer, lj_thr_newid());
  assert(lj_thr_id_is_owner(lj_tg_tid_acq(&p->peer)));
  lj_arena_alloc_owner_rel(&p->peer.alloc, lj_tg_tid_acq(&p->peer));
  lj_thr_set_tg(&p->peer);
  lj_tg_attach(p->g, &p->peer);
  assert(lj_thr_get_tg() == &p->peer);
  assert(lj_tg_in_native_acq(p->owner) == 1);
  n = lj_safepoint_handshake(p->g, LJ_GC2_HS_SCAN_ROOTS | LJ_GC2_HS_FLUSH_SSB);
  assert(n == 2);
  assert(gc2_hs_pending_acq(p->g) == 0);
  assert(gc2_hs_leader_acq(p->g) == 0);
  la_store32_rel(&p->peer_done, 1);
  until(&p->allow_detach, 1);
  lj_tg_detach(p->g, &p->peer);
  lj_thr_set_tg(NULL);
  return NULL;
}
static void *observer(void *arg)
{
  Probe *p = arg;
  char path[96], line[320];
  unsigned i;
  until(&p->paused, 1);
  snprintf(path, sizeof(path), "/proc/self/task/%ld/syscall", (long)p->owner_sysid);
  for (i=0; i<2000; i++) {
    long nr;
    unsigned long long addr, op, val, timeout_arg;
    FILE *f = fopen(path, "r");
    assert(f);
    if (!fgets(line, sizeof(line), f)) line[0] = 0;
    fclose(f);
    if (sscanf(line, "%ld %llx %llx %llx %llx", &nr, &addr, &op, &val, &timeout_arg) == 5 &&
        nr == SYS_futex && addr == (unsigned long long)(uintptr_t)&p->owner->poll &&
        op == FUTEX_WAIT_PRIVATE && val == 1 && timeout_arg == 0) {
      assert(la_load32_acq(&p->returned) == 0);
      assert(la_load32_acq(&p->release) == 0);
      assert(lj_tg_in_native_acq(p->owner) == 0);
      assert(lj_tg_reqmask_acq(p->owner) == 0);
      assert(lj_tg_poll_acq(p->owner) == 1);
      assert(gc2_hs_epoch_acq(p->g) == p->epoch);
      assert(lj_state_owner_word_acq(p->L) == p->owner_word);
      printf("mode=%u actual_kernel_wait=%s", p->mode, line);
      printf("held: native=0 reqmask=0 poll=1 pending=%u ack=%" PRIu64 " epoch=%" PRIu64 " returned=0 scan_calls=%u\n",
        gc2_hs_pending_acq(p->g), lj_tg_hs_epoch_ack_acq(p->owner), p->epoch, p->scan_calls);
      la_store32_rel(&p->observed_futex, 1);
      la_store32_rel(&p->release, 1);
      return NULL;
    }
    usleep(1000);
  }
  fprintf(stderr, "no exact native-return futex observed; last syscall=%s", line);
  abort();
}
static int native_probe(lua_State *L)
{
  LJNativeFrame frame;
  TValue saved;
  assert(tvistab(L->base));
  ctx.L=L; ctx.g=G(L); ctx.owner=L2TG(L); ctx.root=tabV(L->base);
  ctx.owner_word=lj_state_owner_word_acq(L);
  ctx.owner_sysid=(pid_t)syscall(SYS_gettid);
  assert(lj_thr_get_tg() == ctx.owner);
  assert(lj_tg_owns_state_acq(ctx.owner,L));
  lj_tv_load_acq(&saved,L->base);
  lj_gc2_mark_begin(ctx.g);
  assert(gc2_phase_acq(ctx.g) == LJ_GC2_MARK);
  assert(lj_gc2_ismarked(ctx.g,obj2gco(ctx.root)) == 0);
  la_store32_rel(&ctx.armed,1);
  lj_native_enter_l(L,&frame);
  assert(pthread_create(&ctx.observer,NULL,observer,&ctx)==0);
  assert(pthread_create(&ctx.peer_thread,NULL,leader,&ctx)==0);
  until(&ctx.paused,1);
  assert(lj_native_leave_l(L,&frame)==0);
  la_store32_rel(&ctx.returned,1);
  assert(lj_tg_poll_acq(ctx.owner)==0);
  assert(lj_tg_reqmask_acq(ctx.owner)==0);
  assert(lj_tg_hs_epoch_ack_acq(ctx.owner)==ctx.epoch);
  assert(tv_rawload(L->base)==tv_rawload(&saved));
  assert(lj_gc2_ismarked(ctx.g,obj2gco(ctx.root))==1);
  until(&ctx.peer_done,1);
  la_store32_rel(&ctx.allow_detach,1);
  assert(pthread_join(ctx.observer,NULL)==0);
  assert(pthread_join(ctx.peer_thread,NULL)==0);
  assert(ctx.observed_futex && ctx.scan_calls==1 && ctx.global_calls==1);
  assert(ctx.mode != 3 || ctx.flush_calls == 1);
  la_store32_rel(&ctx.armed,0);
  /* Dead-TG physical retirement has its own grace. Static peer storage stays
  ** alive until the final collection/close performs that normal cleanup. */
  puts("release: leader completed, native return resumed, exact root still marked and source unchanged");
  return 0;
}
int main(int argc,char **argv)
{
  lua_State *L;
  assert(argc==2);
  ctx.mode=(uint32_t)strtoul(argv[1],NULL,10);
  assert(ctx.mode<=3);
  alarm(20);
  setvbuf(stdout,NULL,_IOLBF,0);
  L=luaL_newstate(); assert(L);
  luaL_openlibs(L);
  lua_gc(L,LUA_GCCOLLECT,0);
  lua_gc(L,LUA_GCSTOP,0);
  lua_pushcfunction(L,native_probe); lua_setglobal(L,"native_probe");
  if (luaL_dostring(L,"jit.off(true,true); local t={value=2718}; native_probe(t); assert(t.value==2718)")) {
    fprintf(stderr,"%s\n",lua_tostring(L,-1)); abort();
  }
  lua_gc(L,LUA_GCCOLLECT,0);
  assert(gc2_phase_acq(G(L))==LJ_GC2_IDLE);
  lua_close(L);
  puts("real consumed native acknowledgement probe PASS");
  return 0;
}
