#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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

/* All requests, claims, scans, native transitions and registry changes are
** real. Mode 5 only forces native-scanner refusal; the owner must then perform
** its genuine root scan. Hooks pause the executor around its poll publication,
** never manufacture an epoch, request, poll, pending slot or state owner. */
typedef struct Probe {
  global_State *g;
  TGState *owner;
  TGState peer, late;
  lua_State *L;
  GCtab *root;
  LJStateOwner owner_word;
  uint64_t epoch;
  GC2SSBNode *fresh;
  uint32_t mode, armed, before, cleared, release_before, release_tail;
  uint32_t returned, peer_done, allow_detach, refused, remote_scans;
  uint32_t owner_scans, observed_futex, late_done;
  pid_t owner_sysid;
  pthread_t peer_thread, observer;
} Probe;
static Probe ctx;
static void until(const uint32_t *p, uint32_t v)
{
  while (la_load32_acq(p) != v) la_cpu_pause();
}
static void completion_hook(global_State *g, TGState *tg,
                            uint64_t epoch, uint32_t point)
{
  Probe *p = &ctx;
  if (g != p->g || tg != p->owner || !la_load32_acq(&p->armed)) return;
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch);
  assert(gc2_hs_pending_acq(g) != 0);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(lj_gc2_ismarked(g, obj2gco(p->root)) == 1);
  assert(lj_state_owner_word_acq(p->L) == p->owner_word);
  if (p->mode == 5) {
    assert(lj_thr_get_tg() == p->owner);
    if (point == LJ_SAFEPOINT_COMPLETION_TEST_CLEARED)
      la_store32_rel(&p->cleared, 1);
    return;
  }
  assert(lj_thr_get_tg() == &p->peer);
  assert(gc2_hs_leader_acq(g) == lj_tg_tid_acq(&p->peer));
  p->epoch = epoch;
  if (point == LJ_SAFEPOINT_COMPLETION_TEST_BEFORE_CLEAR) {
    assert(lj_tg_poll_acq(tg) == 1);
    la_store32_rel(&p->before, 1);
    if (p->mode == 1) until(&p->release_before, 1);
  } else {
    assert(point == LJ_SAFEPOINT_COMPLETION_TEST_CLEARED);
    assert(lj_tg_poll_acq(tg) == 0);
    p->fresh = lj_tg_ssb_active_acq(tg);
    assert(lj_tg_ssb_next_acq(tg) == lj_tg_ssb_base_acq(tg));
    la_store32_rel(&p->cleared, 1);
    until(&p->release_tail, 1);
    assert(la_load32_acq(&p->returned) == 1);
    assert(lj_tg_in_native_acq(tg) == 0);
    assert(lj_state_owner_word_acq(p->L) == p->owner_word);
  }
}
extern int __real_lj_gc2_scan_cycle_owner_tg_roots_native_parked(global_State *, TGState *);
int __wrap_lj_gc2_scan_cycle_owner_tg_roots_native_parked(global_State *g, TGState *tg)
{
  int r;
  if (g != ctx.g || tg != ctx.owner || !la_load32_acq(&ctx.armed))
    return __real_lj_gc2_scan_cycle_owner_tg_roots_native_parked(g, tg);
  if (ctx.mode == 5) {
    assert(la_load32_acq(&ctx.cleared) == 0);
    la_store32_rel(&ctx.refused, 1);
    return 0;
  }
  r = __real_lj_gc2_scan_cycle_owner_tg_roots_native_parked(g, tg);
  assert(r == 1);
  (void)la_add32_rlx(&ctx.remote_scans, 1);
  return r;
}
extern void __real_lj_gc2_scan_cycle_owner_tg_roots(global_State *, TGState *);
void __wrap_lj_gc2_scan_cycle_owner_tg_roots(global_State *g, TGState *tg)
{
  __real_lj_gc2_scan_cycle_owner_tg_roots(g, tg);
  if (g == ctx.g && tg == ctx.owner && la_load32_acq(&ctx.armed))
    (void)la_add32_rlx(&ctx.owner_scans, 1);
}
static void init_peer(Probe *p, TGState *tg)
{
  lj_tg_init_thread(p->g, tg, NULL, 0);
  lj_tg_tid_rel(tg, lj_thr_newid());
  assert(lj_thr_id_is_owner(lj_tg_tid_acq(tg)));
  lj_arena_alloc_owner_rel(&tg->alloc, lj_tg_tid_acq(tg));
  lj_thr_set_tg(tg);
  lj_tg_attach(p->g, tg);
}
static void *leader(void *arg)
{
  Probe *p = arg;
  uint64_t epoch;
  init_peer(p, &p->peer);
  assert(lj_safepoint_handshake(p->g,
    LJ_GC2_HS_SCAN_OWNER_ROOTS|LJ_GC2_HS_FLUSH_SSB) == 2);
  epoch = gc2_hs_epoch_acq(p->g);
  assert(gc2_hs_pending_acq(p->g) == 0);
  assert(gc2_hs_leader_acq(p->g) == 0);
  if (p->mode == 2 || p->mode == 3) {
    uint32_t actions = p->mode == 2 ? LJ_GC2_HS_FLUSH_SSB : LJ_GC2_HS_STOPREQ;
    assert(lj_safepoint_handshake(p->g, actions) == 2);
    assert(gc2_hs_epoch_acq(p->g) == epoch + 1);
  }
  la_store32_rel(&p->peer_done, 1);
  until(&p->allow_detach, 1);
  lj_tg_detach(p->g, &p->peer);
  lj_thr_set_tg(NULL);
  return NULL;
}
static void *late_attach(void *arg)
{
  Probe *p = arg;
  init_peer(p, &p->late);
  assert(lj_tg_hs_epoch_ack_acq(&p->late) == p->epoch);
  assert(lj_tg_reqmask_acq(&p->late) == 0);
  assert(lj_tg_poll_acq(&p->late) == 0);
  assert(lj_tg_mark_active_acq(&p->late) == 1);
  assert(lj_tg_alloc_black_acq(&p->late) == 1);
  lj_tg_detach(p->g, &p->late);
  assert(lj_tg_flags_test_acq(&p->late, TGF_DEAD));
  assert(lj_tg_reqmask_acq(&p->late) == 0);
  assert(lj_tg_poll_acq(&p->late) == 0);
  lj_thr_set_tg(NULL);
  la_store32_rel(&p->late_done, 1);
  return NULL;
}
static void *observer(void *arg)
{
  Probe *p = arg;
  char path[96], line[320];
  unsigned i;
  until(&p->before, 1);
  snprintf(path, sizeof(path), "/proc/self/task/%ld/syscall", (long)p->owner_sysid);
  for (i = 0; i < 2000; i++) {
    long nr;
    unsigned long long addr, op, val, timeout_arg;
    FILE *f = fopen(path, "r");
    assert(f);
    if (!fgets(line, sizeof(line), f)) line[0] = 0;
    fclose(f);
    if (sscanf(line, "%ld %llx %llx %llx %llx", &nr, &addr, &op, &val, &timeout_arg) == 5 &&
        nr == SYS_futex && addr == (unsigned long long)(uintptr_t)&p->owner->poll &&
        op == FUTEX_WAIT_PRIVATE && val == 1 && timeout_arg == 0) {
      assert(lj_tg_reqmask_acq(p->owner) == 0);
      assert(lj_tg_in_native_acq(p->owner) == 0);
      assert(la_load32_acq(&p->returned) == 0);
      assert(la_load32_acq(&p->cleared) == 0);
      printf("before completion actual kernel wait: %s", line);
      la_store32_rel(&p->observed_futex, 1);
      la_store32_rel(&p->release_before, 1);
      return NULL;
    }
    usleep(1000);
  }
  fprintf(stderr, "no exact pre-clear wait observed: %s", line);
  abort();
}
static int native_probe(lua_State *L)
{
  Probe *p = &ctx;
  LJNativeFrame frame;
  TValue saved;
  uint32_t returned_actions;
  assert(tvistab(L->base));
  p->L = L; p->g = G(L); p->owner = L2TG(L); p->root = tabV(L->base);
  p->owner_word = lj_state_owner_word_acq(L);
  p->owner_sysid = (pid_t)syscall(SYS_gettid);
  assert(lj_thr_get_tg() == p->owner && lj_tg_owns_state_acq(p->owner, L));
  lj_tv_load_acq(&saved, L->base);
  lj_gc2_mark_begin(p->g);
  assert(gc2_phase_acq(p->g) == LJ_GC2_MARK);
  assert(lj_gc2_ismarked(p->g, obj2gco(p->root)) == 0);
  la_store32_rel(&p->armed, 1);
  lj_safepoint_test_completion_hook(completion_hook);
  lj_native_enter_l(L, &frame);
  if (p->mode == 1) assert(pthread_create(&p->observer, NULL, observer, p) == 0);
  assert(pthread_create(&p->peer_thread, NULL, leader, p) == 0);
  if (p->mode == 5) {
    until(&p->refused, 1);
    assert(la_load32_acq(&p->cleared) == 0);
    assert(lj_tg_poll_acq(p->owner) == 1);
    assert(gc2_hs_pending_acq(p->g) != 0);
  } else if (p->mode == 1) {
    until(&p->before, 1);
  } else {
    until(&p->cleared, 1);
  }
  returned_actions = lj_native_leave_l(L, &frame);
  assert((returned_actions & ~(LJ_GC2_HS_SCAN_OWNER_ROOTS|LJ_GC2_HS_FLUSH_SSB)) == 0);
  if (p->mode == 1) until(&p->cleared, 1);
  la_store32_rel(&p->returned, 1);
  assert(lj_tg_poll_acq(p->owner) == 0);
  assert(lj_tg_reqmask_acq(p->owner) == 0);
  assert(tv_rawload(L->base) == tv_rawload(&saved));
  assert(lj_gc2_ismarked(p->g, obj2gco(p->root)) == 1);
  if (p->mode != 5) {
    assert(gc2_hs_pending_acq(p->g) != 0);
    assert(gc2_hs_leader_acq(p->g) == lj_tg_tid_acq(&p->peer));
    assert(la_load32_acq(&p->release_tail) == 0);
    assert(lj_tg_ssb_active_acq(p->owner) == p->fresh);
    /* A newly produced below-capacity suffix remains visible while the
    ** executor is still paused. Local completion cannot certify SSB empty. */
    assert(lj_gc2_test_ssb_push(p->g, obj2gco(p->root)) == 1);
    assert(lj_tg_ssb_next_acq(p->owner) > lj_tg_ssb_base_acq(p->owner));
    assert(lj_tg_ssb_next_acq(p->owner) < lj_tg_ssb_end_acq(p->owner));
    assert(!lj_gc2_test_ssb_empty(p->g));
    lua_pushinteger(L, 3141); lua_setfield(L, 1, "value");
    printf("mode=%u returned with executor paused: pending=%u poll=0 epoch=%" PRIu64 " active suffix retained\n",
      p->mode, gc2_hs_pending_acq(p->g), p->epoch);
    if (p->mode == 4) {
      pthread_t late;
      assert(pthread_create(&late, NULL, late_attach, p) == 0);
      assert(pthread_join(late, NULL) == 0);
      assert(la_load32_acq(&p->late_done) == 1);
    }
    la_store32_rel(&p->release_tail, 1);
  } else {
    assert(la_load32_acq(&p->owner_scans) >= 1);
    assert(la_load32_acq(&p->cleared) == 1);
    lua_pushinteger(L, 3141); lua_setfield(L, 1, "value");
  }
  if (p->mode == 2 || p->mode == 3) {
    uint32_t next_actions = p->mode == 2 ? LJ_GC2_HS_FLUSH_SSB : LJ_GC2_HS_STOPREQ;
    while (lj_tg_reqmask_acq(p->owner) == 0) la_cpu_pause();
    assert(gc2_hs_epoch_acq(p->g) == p->epoch + 1);
    assert(lj_tg_reqmask_acq(p->owner) == next_actions);
    assert(lj_tg_poll_acq(p->owner) == 1);
    assert(lj_tg_hs_epoch_ack_acq(p->owner) == p->epoch);
    assert(lj_tg_in_native_acq(p->owner) == 0);
    assert(la_load32_acq(&p->remote_scans) == 1);
    assert(lj_safepoint_poll_tg(p->owner) == next_actions);
    assert(lj_tg_reqmask_acq(p->owner) == 0);
  }
  until(&p->peer_done, 1);
  la_store32_rel(&p->allow_detach, 1);
  assert(pthread_join(p->peer_thread, NULL) == 0);
  if (p->mode == 1) {
    assert(pthread_join(p->observer, NULL) == 0);
    assert(la_load32_acq(&p->observed_futex) == 1);
  }
  la_store32_rel(&p->armed, 0);
  lj_safepoint_test_completion_hook(NULL);
  assert(gc2_hs_pending_acq(p->g) == 0 && gc2_hs_leader_acq(p->g) == 0);
  if (p->mode == 3) {
    assert(lj_tg_flags_test_acq(p->owner, TGF_STOPREQ_FRESH));
    assert(lj_tg_poll_acq(p->owner) == 1);
    assert(lj_safepoint_ack(L) == LJ_GC2_HS_STOPREQ);
    lj_safepoint_checkstop(L, LJ_GC2_HS_STOPREQ);
    abort();
  }
  assert(lj_tg_poll_acq(p->owner) == 0);
  return 0;
}
int main(int argc, char **argv)
{
  lua_State *L;
  assert(argc == 2);
  ctx.mode = (uint32_t)strtoul(argv[1], NULL, 10);
  assert(ctx.mode <= 5);
  alarm(20); setvbuf(stdout, NULL, _IOLBF, 0);
  L = luaL_newstate(); assert(L); luaL_openlibs(L);
  lua_gc(L, LUA_GCCOLLECT, 0); lua_gc(L, LUA_GCSTOP, 0);
  lua_pushcfunction(L, native_probe); lua_setglobal(L, "native_probe");
  lua_pushinteger(L, ctx.mode); lua_setglobal(L, "probe_mode");
  if (luaL_dostring(L,
    "jit.off(true,true); local t={value=2718}; local ok,err=pcall(native_probe,t); "
    "if probe_mode==3 then assert(not ok and tostring(err):find('interrupt')) else assert(ok,err) end; "
    "collectgarbage('collect'); assert(t.value==3141)")) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1)); abort();
  }
  lua_gc(L, LUA_GCCOLLECT, 0); assert(gc2_phase_acq(G(L)) == LJ_GC2_IDLE);
  lua_close(L);
  puts("real local-completion native return probe PASS");
  return 0;
}
