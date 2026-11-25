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
  ** exact Q record for the removed key. The paused copy retains its SMR
  ** scope, so no retirement pass can consume the record or body meanwhile. */
  lua_pushstring(L, rescued_name);
  assert(strV(L->top - 1) == rescued);
  assert(lj_str_canon_state(lj_str_canon_acq(rescued)) == LJ_STR_CANON_QRESCUED);
  assert(lj_gc2_reclaim_retired(g, lj_gc2_retire_epoch(g)) == 0);
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

int main(void)
{
  lua_State *L;
  global_State *g;
  GCstr *old, *borrowed;
  StrCanonRec *rec;
  TGState *read_tg;
  Peer peer = {0};
  LJThr thread = {0};
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
