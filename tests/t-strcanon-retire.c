/*
** Canonical quarantine commit and reuse grace with an attached mutator.
** Build the fork with -DLJ_STR_TEST_HELPERS before linking this fixture.
*/
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_str.h"
#include "lj_thr.h"

static char target_name[LJ_HUGE_THRESHOLD + 1];
static const char borrowed_name[] = "canonical-public-borrow-fixture";

typedef struct Peer {
  lua_State *L;
  const char *borrowed;
  const char *replacement;
  uint32_t stage;
} Peer;

enum {
  PEER_BORROWED = 1,
  PEER_INTERN,
  PEER_INTERNED,
  PEER_FINISH
};

static void wait_stage(Peer *peer, lua_State *L, uint32_t stage)
{
  while (la_load32_acq(&peer->stage) < stage)
    (void)lj_thr_retry_yield(L);
}

static void *check_identity(void *arg)
{
  Peer *peer = (Peer *)arg;
  lua_State *L = peer->L;
  size_t len;
  assert(lj_threading_attach_wait(L));
  lua_pushlstring(L, borrowed_name, sizeof(borrowed_name)-1);
  peer->borrowed = lua_tolstring(L, -1, &len);
  assert(len == sizeof(borrowed_name)-1);
  la_store32_rel(&peer->stage, PEER_BORROWED);
  wait_stage(peer, L, PEER_INTERN);
  lua_pushlstring(L, target_name, sizeof(target_name)-1);
  peer->replacement = lua_tolstring(L, -1, NULL);
  la_store32_rel(&peer->stage, PEER_INTERNED);
  while (la_load32_acq(&peer->stage) < PEER_FINISH) {
    lua_pushlstring(L, target_name, sizeof(target_name)-1);
    assert(lua_tolstring(L, -1, NULL) == peer->replacement);
    lua_pop(L, 1);
    (void)lj_thr_retry_yield(L);
  }
  assert(lua_tolstring(L, -2, NULL) == peer->borrowed);
  assert(memcmp(peer->borrowed, borrowed_name, sizeof(borrowed_name)) == 0);
  assert(memcmp(peer->replacement, target_name, sizeof(target_name)) == 0);
  lj_threading_detach(L, 1);
  return NULL;
}

static StrTabHdr *claimed_main;
static StrCanonRec *cancelled;

static void force_cancel(lua_State *L, GCstr *s, StrCanonRec *rec,
                         uint32_t stage)
{
  (void)s;
  if (stage == LJ_STR_TEST_CANON_Q_PINNED_BEFORE_PUBLISH) {
    claimed_main = lj_str_tabh_acq(G(L));
    assert(lj_str_sweep_claim(L, claimed_main));
    cancelled = rec;
  }
}

static void advance_grace(global_State *g)
{
  uint32_t i;
  for (i = 0; i < LJ_GC2_GRACE_EPOCHS; i++)
    (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_reclaim_retired(g, lj_gc2_retire_epoch(g));
}

static void check_cancelled_record(lua_State *L)
{
  global_State *g = G(L);
  const char name[] = "canonical-cancelled-fixture";
  GCstr *s = lj_str_newz(L, name);
  lj_str_test_set_canon_hook(force_cancel);
  assert(!lj_str_test_quarantine_detach(L, s));
  lj_str_test_set_canon_hook(NULL);
  assert(claimed_main && cancelled);
  lj_str_sweep_release(claimed_main);
  assert(lj_str_qcount_acq(g) == 0);
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_reclaim_retired(g, lj_gc2_retire_epoch(g));
  assert(la_load32_acq(&cancelled->status) & LJ_STR_CANONREC_Q_UNLINKED);
  advance_grace(g);
  assert(!lj_gc2_mem_registered_known(g, cancelled));
  assert(lj_str_newz(L, name) == s);
}

/* A mutable source is still strong when copied to a new natural root. */
static int push_closing_upvalue(lua_State *L)
{
  lua_pushvalue(L, lua_upvalueindex(1));
  assert(lj_str_canon_state(lj_str_canon_acq(strV(L->top - 1))) ==
	 LJ_STR_CANON_QRESCUED);
  return 1;
}

static void check_stack_publication(lua_State *L)
{
  const char name[] = "canonical-stack-publication-fixture";
  GCstr *s;
  StrCanonRec *rec;
  lua_pushlstring(L, name, sizeof(name)-1);
  s = strV(L->top - 1);
  lua_pushcclosure(L, push_closing_upvalue, 1);
  assert(lj_str_test_quarantine_detach(L, s));
  rec = lj_str_canon_record(lj_str_canon_acq(s));
  assert(rec);
  /* A synthetic reversible close exercises the publication arbitration
  ** while the old body remains rooted in the closure. */
  lj_str_canon_rel(s, lj_str_canon_pack(rec, LJ_STR_CANON_QCLOSING));
  lua_call(L, 0, 1);
  assert(strV(L->top - 1) == s);
  advance_grace(G(L));
  assert(lj_str_canon_acq(s) == LJ_STR_CANON_LIVE);
  lua_pop(L, 1);
}

static void check_table_publication(lua_State *L)
{
  const char name[] = "canonical-table-publication-fixture";
  GCstr *s;
  StrCanonRec *rec;

  lua_newtable(L);
  lua_pushlstring(L, name, sizeof(name)-1);
  s = strV(L->top - 1);
  lua_setfield(L, -2, "value");

  assert(lj_str_test_quarantine_detach(L, s));
  rec = lj_str_canon_record(lj_str_canon_acq(s));
  assert(rec);
  assert(luaL_loadstring(L, "local t = ...; return t.value") == LUA_OK);
  lua_pushvalue(L, -2);
  lj_str_canon_rel(s, lj_str_canon_pack(rec, LJ_STR_CANON_QCLOSING));
  lua_call(L, 1, 1);
  assert(strV(L->top - 1) == s);
  assert(lj_str_canon_state(lj_str_canon_acq(s)) == LJ_STR_CANON_QRESCUED);
  lua_pop(L, 1);

  lj_str_canon_rel(s, lj_str_canon_pack(rec, LJ_STR_CANON_QCLOSING));
  lua_pushnil(L);
  assert(lua_next(L, -2));
  assert(strV(L->top - 1) == s);
  assert(lj_str_canon_state(lj_str_canon_acq(s)) == LJ_STR_CANON_QRESCUED);
  advance_grace(G(L));
  assert(lj_str_canon_acq(s) == LJ_STR_CANON_LIVE);
  lua_pop(L, 3);
}

static void check_rooted_upvalue_publication(lua_State *L)
{
  const char name[] = "canonical-upvalue-publication-fixture";
  GCstr *s;
  StrCanonRec *rec;
  assert(luaL_loadstring(L, "local x = ...; return function() return x end") ==
	 LUA_OK);
  lua_pushlstring(L, name, sizeof(name)-1);
  s = strV(L->top - 1);
  lua_call(L, 1, 1);

  assert(lj_str_test_quarantine_detach(L, s));
  rec = lj_str_canon_record(lj_str_canon_acq(s));
  assert(rec);
  lj_str_canon_rel(s, lj_str_canon_pack(rec, LJ_STR_CANON_QCLOSING));
  lua_pushnil(L);
  assert(lj_func_getuv_rooted_try(L, L->top - 2, 0, L->top - 1));
  assert(strV(L->top - 1) == s);
  assert(lj_str_canon_state(lj_str_canon_acq(s)) == LJ_STR_CANON_QRESCUED);
  advance_grace(G(L));
  assert(lj_str_canon_acq(s) == LJ_STR_CANON_LIVE);
  lua_pop(L, 2);
}

typedef struct CopyPeer {
  lua_State *L;
  MSize target_mask;
  uint32_t frozen, release;
} CopyPeer;

static CopyPeer *copy_peer;

static void pause_generation_copy(lua_State *L, GCstr *s, StrCanonRec *rec,
                                  uint32_t stage)
{
  CopyPeer *peer = copy_peer;
  if (L != peer->L || stage != LJ_STR_TEST_CANON_EXPAND_FROZEN ||
      la_load32_acq(&peer->frozen))
    return;
  assert(s == NULL && rec == NULL);
  la_store32_rel(&peer->frozen, 1);
  while (!la_load32_acq(&peer->release))
    (void)lj_thr_retry_yield(L);
}

static void *resize_copy_peer(void *opaque)
{
  CopyPeer *peer = opaque;
  assert(lj_threading_attach_wait(peer->L));
  lj_str_resize(peer->L, peer->target_mask);
  lj_threading_detach(peer->L, 1);
  return NULL;
}

static void check_dual_generation_quarantine(void)
{
  static const char rescued_name[] = "quarantine-rescued-across-generation-copy";
  static char closed_name[LJ_HUGE_THRESHOLD + 1];
  lua_State *L = luaL_newstate();
  global_State *g;
  StrTabHdr *source, *target;
  StrCanonRec *record;
  GCstr *rescued, *old;
  CopyPeer peer = {0};
  LJThr thread = {0};
  const char *replacement;
  MSize first_slot;
  assert(L);
  g = G(L);
  (void)lua_gc(L, LUA_GCSTOP, 0);
  source = lj_str_tabh_acq(g);
  first_slot = source->link_slot;
  lj_str_resize(L, (source->mask << 1) + 1u);
  source = lj_str_tabh_acq(g);
  assert(source->link_slot != first_slot);

  memset(closed_name, 'r', sizeof(closed_name)-1);
  rescued = lj_str_newz(L, rescued_name);
  old = lj_str_newz(L, closed_name);
  assert(lj_str_test_quarantine_detach(L, rescued));
  assert(lj_str_test_quarantine_detach(L, old));
  assert(lj_str_test_quarantine_close(g, old));
  record = lj_str_canon_record(lj_str_canon_acq(old));
  assert(record);

  peer.L = lua_newthread(L);
  peer.target_mask = (source->mask << 1) + 1u;
  copy_peer = &peer;
  lj_str_test_set_canon_hook(pause_generation_copy);
  assert(lj_thr_create(&thread, resize_copy_peer, &peer) == 0);
  while (!la_load32_acq(&peer.frozen))
    (void)lj_thr_retry_yield(L);
  target = lj_str_expand_to_acq(source);
  assert(target && target->link_slot == first_slot);
  assert(lj_str_tabh_acq(g) == source);

  /* Main-table lookup assists the frozen generation and then finds the
  ** exact Q record for the removed key. The old header pin keeps this
  ** exact close in flight while the reclaimer may free unrelated data. */
  lua_pushstring(L, rescued_name);
  assert(strV(L->top - 1) == rescued);
  assert(lj_str_canon_state(lj_str_canon_acq(rescued)) == LJ_STR_CANON_QRESCUED);
  assert(gc2_smr_readers_acq(g) == 0);
  (void)lj_gc2_reclaim_retired(g, lj_gc2_retire_epoch(g));
  assert(lj_str_canon_state(lj_str_canon_acq(old)) == LJ_STR_CANON_QCLOSING);
  assert(lj_str_tabh_acq(g) == source);
  la_store32_rel(&peer.release, 1);
  assert(lj_thr_join(&thread, NULL) == 0);
  lj_str_test_set_canon_hook(NULL);
  copy_peer = NULL;
  assert(lj_str_tabh_acq(g) == target);

  advance_grace(g);
  assert(lj_str_canon_acq(rescued) == LJ_STR_CANON_LIVE);
  assert(lj_str_canon_state(lj_str_canon_acq(old)) == LJ_STR_CANON_QCOMMIT);
  assert(la_load32_acq(&record->status) & LJ_STR_CANONREC_Q_UNLINKED);
  assert(lj_gc2_mem_registered_known(g, old));
  assert(lj_str_qcount_acq(g) == 0);

  /* Reuse the first successor slot again while the distinct closed body
  ** still waits for its second grace. The live body follows the new index. */
  lj_str_resize(L, (target->mask << 1) + 1u);
  assert(lj_str_tabh_acq(g)->link_slot != first_slot);
  lua_pushstring(L, rescued_name);
  assert(strV(L->top - 1) == rescued);
  lua_pushstring(L, closed_name);
  replacement = lua_tolstring(L, -1, NULL);
  assert(replacement != strdata(old));
  advance_grace(g);
  assert(!lj_gc2_mem_registered_known(g, old));
  assert(!lj_gc2_mem_registered_known(g, record));
  lua_pushstring(L, closed_name);
  assert(lua_tolstring(L, -1, NULL) == replacement);
  lua_close(L);
}

#ifdef LJ_GC2_TEST_HELPERS
typedef struct IdleRetire {
  global_State *g;
  uint32_t started, release;
} IdleRetire;

static void *hold_idle_retirement(void *arg)
{
  IdleRetire *r = arg;
  assert(lj_gc2_test_idle_reclaim_enter(r->g));
  la_store32_rel(&r->started, 1);
  while (!la_load32_acq(&r->release))
    (void)lj_thr_retry_yield(NULL);
  lj_gc2_test_idle_reclaim_leave(r->g);
  return NULL;
}

/* A C API pop leaves its word in unused stack capacity. A remote scan
** must still rescue the adjacent live slot while that stale mapping
** waits for the second grace period. */
static void *scan_parked_stack(void *arg)
{
  TGState *tg = arg;
  global_State *g = tg->gl;
  lj_tg_tid_rel(tg, lj_thr_newid());
  lj_arena_alloc_owner_rel(&tg->alloc, lj_tg_tid_acq(tg));
  lj_thr_set_tg(tg);
  lj_tg_attach(g, tg);
  assert(lj_gc2_handshake(g, LJ_GC2_HS_SCAN_OWNER_ROOTS) == 2);
  lj_tg_detach(g, tg);
  lj_thr_set_tg(NULL);
  return NULL;
}

static uint32_t remote_scan_consumed;

static void resume_during_scan(global_State *g, TGState *tg, uint64_t epoch,
			       uint32_t point)
{
  (void)epoch;
  if (point == LJ_SAFEPOINT_COMPLETION_TEST_CONSUMED && tg == g->main_tg &&
      tg != lj_thr_get_tg()) {
    la_store32_rel(&remote_scan_consumed, 1);
    while (lj_tg_in_native_acq(tg))
      la_cpu_pause();
  }
}

static int check_popped_native_slot(lua_State *L)
{
  global_State *g = G(L);
  LJNativeFrame frame;
  LJThr thread = {0};
  TGState *pin;
  GCstr *live, *old;
  const char *bytes;
  static char payload[LJ_HUGE_THRESHOLD + 63];
  TGState *scanner = lua_touserdata(L, 1);
  memset(payload, 'p', sizeof(payload));
  lua_pushliteral(L, "live across parked stack scan");
  live = strV(L->top - 1);
  bytes = lua_tolstring(L, -1, NULL);
  lua_pushlstring(L, payload, sizeof(payload));
  old = strV(L->top - 1);
  lua_pop(L, 1);
  assert(tvisstr(L->top) && strV(L->top) == old);
  assert(lj_str_test_quarantine_detach(L, old));
  assert(lj_str_test_quarantine_close(g, old));
  advance_grace(g);
  assert(lj_str_canon_state(lj_str_canon_acq(old)) == LJ_STR_CANON_QCOMMIT);
  pin = lj_str_source_pin(L, old);
  assert(pin);
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(lj_str_test_quarantine_detach(L, live));
  assert(lj_str_test_quarantine_close(g, live));
  assert(tvisstr(L->top) && strV(L->top) == old);
  lj_safepoint_test_completion_hook(resume_during_scan);
  lj_native_enter_l(L, &frame);
  assert(lj_thr_create(&thread, scan_parked_stack, scanner) == 0);
  while (!la_load32_acq(&remote_scan_consumed))
    la_cpu_pause();
  /* Native leave clears the depth and must retain its parked stack until
  ** the remote actor finishes the request it already consumed. */
  (void)lj_native_leave_l(L, &frame);
  assert(lj_thr_join(&thread, NULL) == 0);
  lj_safepoint_test_completion_hook(NULL);
  assert(!lj_gc2_activation_reclaim_veto(g));
  assert(lj_str_canon_state(lj_str_canon_acq(live)) == LJ_STR_CANON_QRESCUED);
  assert(lua_tolstring(L, -1, NULL) == bytes);
  assert(strcmp(bytes, "live across parked stack scan") == 0);
  lj_str_source_unpin(pin);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  advance_grace(g);
  assert(!lj_gc2_mem_registered_known(g, old));
  return 0;
}

static void check_parked_stack(void)
{
  lua_State *L = luaL_newstate();
  TGState scanner;
  assert(L);
  lua_gc(L, LUA_GCSTOP, 0);
  lj_tg_init_thread(G(L), &scanner, NULL, 0);
  lua_pushcfunction(L, check_popped_native_slot);
  lua_pushlightuserdata(L, &scanner);
  lua_call(L, 1, 0);
  (void)lj_tg_reclaim_dead(G(L));
  assert(lj_tg_fini_thread(G(L), &scanner));
  lua_close(L);
}

static void check_huge_idle_identity(void)
{
  static char bytes[LJ_HUGE_THRESHOLD + 77];
  IdleRetire r = {0};
  LJThr thr = {0};
  lua_State *L = luaL_newstate();
  const char *first;
  GCstr *s;
  assert(L);
  r.g = G(L);
  lua_gc(L, LUA_GCSTOP, 0);
  memset(bytes, 'I', sizeof(bytes));
  lua_pushlstring(L, bytes, sizeof(bytes));
  first = lua_tolstring(L, -1, NULL);
  s = strV(L->top - 1);
  assert(lj_str_test_quarantine_detach(L, s));
  assert(lj_str_test_quarantine_close(r.g, s));
  assert(lj_thr_create(&thr, hold_idle_retirement, &r) == 0);
  while (!la_load32_acq(&r.started))
    (void)lj_thr_retry_yield(L);

  /* Obtain the same huge mapping from quarantine while another actor
  ** owns retirement and ordinary registry readers cannot enter. */
  assert(!lj_gc2_smr_read_try(r.g));
  lua_pushlstring(L, bytes, sizeof(bytes));
  assert(lua_tolstring(L, -1, NULL) == first);
  assert(memcmp(first, bytes, sizeof(bytes)) == 0);
  assert(lj_str_canon_state(lj_str_canon_acq(s)) == LJ_STR_CANON_QRESCUED);
  la_store32_rel(&r.release, 1);
  assert(lj_thr_join(&thr, NULL) == 0);
  advance_grace(r.g);
  assert(lj_str_canon_acq(s) == LJ_STR_CANON_LIVE);
  lua_close(L);
}
#endif

#if LJ_TARGET_X64 && defined(LJ_GC2_TEST_HELPERS)
static GCstr *vm_source;
static StrCanonRec *vm_source_rec;

static int arm_vm_source(lua_State *L)
{
  lj_str_canon_rel(vm_source,
		   lj_str_canon_pack(vm_source_rec, LJ_STR_CANON_QCLOSING));
  assert(lj_gc2_test_idle_reclaim_enter(G(L)));
  return 0;
}

static void check_vm_upvalue_publication(lua_State *L, int cell)
{
  const char name[] = "canonical-VM-cell-fixture";
  const char *chunk = cell ?
    "local x,arm = ...; local function store(v) x=v end; arm(); return x,store" :
    "local x = ...; return function(arm) arm(); return x end";
  assert(luaL_loadstring(L, chunk) == LUA_OK);
  lua_pushlstring(L, name, sizeof(name)-1);
  vm_source = strV(L->top - 1);
  assert(lj_str_test_quarantine_detach(L, vm_source));
  vm_source_rec = lj_str_canon_record(lj_str_canon_acq(vm_source));
  assert(vm_source_rec);

  if (!cell)
    lua_call(L, 1, 1);
  lua_pushcfunction(L, arm_vm_source);
  lua_call(L, cell ? 2 : 1, cell ? 2 : 1);
  lj_gc2_test_idle_reclaim_leave(G(L));
  assert(strV(L->top - (cell ? 2 : 1)) == vm_source);
  assert(lj_str_canon_state(lj_str_canon_acq(vm_source)) ==
	 LJ_STR_CANON_QRESCUED);
  advance_grace(G(L));
  assert(lj_str_canon_acq(vm_source) == LJ_STR_CANON_LIVE);
  lua_pop(L, cell ? 2 : 1);
}
#endif

int main(void)
{
  lua_State *L;
  global_State *g;
  GCstr *old, *borrowed;
  StrCanonRec *rec;
  TGState *read_tg;
  Peer peer = {0};
  LJThr thread = {0};
#ifdef LJ_GC2_TEST_HELPERS
  check_huge_idle_identity();
  check_parked_stack();
#endif
  check_dual_generation_quarantine();
  L = luaL_newstate();
  assert(L);
  g = G(L);
  check_cancelled_record(L);
  check_stack_publication(L);
  memset(target_name, 'q', sizeof(target_name)-1);
  peer.L = lua_newthread(L);
  assert(peer.L);

  old = lj_str_newz(L, target_name); /* Use the fixture bytes, not old storage. */
  borrowed = lj_str_newz(L, borrowed_name);
  assert(lj_str_test_quarantine_detach(L, old));
  assert(lj_str_test_quarantine_detach(L, borrowed));
  assert(lj_str_test_quarantine_close(g, borrowed));
  rec = lj_str_canon_record(lj_str_canon_acq(old));
  assert(rec);
  assert(lj_thr_create(&thread, check_identity, &peer) == 0);
  wait_stage(&peer, L, PEER_BORROWED);
  assert(peer.borrowed == strdata(borrowed));
  assert(lj_str_canon_state(lj_str_canon_acq(borrowed)) == LJ_STR_CANON_QRESCUED);
  assert(!lj_str_test_quarantine_close(g, borrowed));
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  check_table_publication(L);
  check_rooted_upvalue_publication(L);
#if LJ_TARGET_X64 && defined(LJ_GC2_TEST_HELPERS)
  check_vm_upvalue_publication(L, 0);
  check_vm_upvalue_publication(L, 1);
#endif

  /* Only the fixture-owned, unrooted target is authorized to close. */
  assert(lj_str_test_quarantine_close(g, old));
  (void)lj_gc2_reclaim_retired(g, lj_gc2_retire_epoch(g));
  assert(lj_str_canon_state(lj_str_canon_acq(old)) == LJ_STR_CANON_QCLOSING);
  advance_grace(g);
  assert(lj_str_canon_state(lj_str_canon_acq(old)) == LJ_STR_CANON_QCOMMIT);
  assert(la_load32_acq(&rec->status) & LJ_STR_CANONREC_Q_UNLINKED);
  assert(lj_str_canon_acq(borrowed) == LJ_STR_CANON_LIVE);
  assert(lj_str_qcount_acq(g) == 0);
  (void)lj_gc2_reclaim_retired(g, lj_gc2_retire_epoch(g));
  assert(lj_gc2_mem_registered_known(g, old));

  /* The peer can obtain a replacement while the old address is reserved. */
  la_store32_rel(&peer.stage, PEER_INTERN);
  wait_stage(&peer, L, PEER_INTERNED);
  assert(peer.replacement != strdata(old));
  lua_pushlstring(L, target_name, sizeof(target_name)-1);
  assert(lua_tolstring(L, -1, NULL) == peer.replacement);
  /* A C source reader may still hold an exact pre-commit pointer.
  ** Its read scope must veto reuse even after both grace periods. */
  read_tg = lj_str_source_pin(L, old);
  assert(read_tg);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  advance_grace(g);
  assert(lj_gc2_mem_registered_known(g, old));
  assert(lj_gc2_mem_registered_known(g, rec));
  lj_str_source_unpin(read_tg);
  advance_grace(g);
  assert(!lj_gc2_mem_registered_known(g, old));
  assert(!lj_gc2_mem_registered_known(g, rec));
  assert(lua_tolstring(L, -1, NULL) == peer.replacement);

  la_store32_rel(&peer.stage, PEER_FINISH);
  assert(lj_thr_join(&thread, NULL) == 0);
  lua_close(L);
  puts("t-strcanon-retire OK");
  return 0;
}
