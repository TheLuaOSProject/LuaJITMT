/*
** Focused regression test for M5 string table CAS publication scaffolding.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lj_tg.h"
#include "lj_thr.h"

#define TEST_STRTAB_RESIZE	((MSize)0x80000000u)
#define TEST_STRTAB_SWEEP	((MSize)0x40000000u)
#define TEST_STRTAB_OWNER_LOBITS	(TEST_STRTAB_SWEEP - 1u)

typedef struct ActiveReleaseCtx {
  TGState *tg;
  StrTabHdr *hdr;
  uint32_t claimed;
  uint32_t cleared;
} ActiveReleaseCtx;

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
typedef struct FailAllocCtx {
  int fail_next;
  size_t fail_min;
} FailAllocCtx;

typedef struct ResizeOOMCtx {
  MSize newmask;
} ResizeOOMCtx;
#endif

typedef struct ReclaimReaderCtx {
  global_State *g;
  uint64_t epoch;
  uint32_t done;
  uint32_t reclaimed;
} ReclaimReaderCtx;

enum {
  TAGGED_HOOK_NONE,
  TAGGED_HOOK_TAG_AFTER_COMPARE,
  TAGGED_HOOK_CLEAR_BEFORE_CAS
};

typedef struct TaggedMatchCtx {
  GCobj *target;
  uint32_t action;
  uint32_t after_compare;
  uint32_t before_cas;
  uint32_t mutations;
} TaggedMatchCtx;

static TaggedMatchCtx tagged_match_ctx;

typedef struct CanonRaceCtx {
  lua_State *driver_L;
  lua_State *worker_L;
  TGState *worker_tg;
  GCstr *s;
  StrCanonRec *rec;
  StrCanonHdr *qhdr;
  int driver_top;
  uint32_t pause_stage;
  uint32_t reached;
  uint32_t release;
  uint32_t done;
  uint32_t active_cleared;
  int result;
} CanonRaceCtx;

static CanonRaceCtx *canon_race_ctx;

static void test_sleep_without_tg(int64_t ns)
{
  TGState *saved = lj_thr_get_tg();
  /* Keep deterministic barrier sleeps outside the caller's attached TG. The
  ** raw unbind/rebind remains on the same physical actor. */
  lj_thr_set_tg(NULL);
  (void)lj_thr_sleep_ns(NULL, ns);
  lj_thr_set_tg(saved);
}

static void tagged_match_hook(lua_State *L, GCRef *link, GCobj *target,
			      uintptr_t observed, uint32_t stage)
{
  uintptr_t expect;
  if (target != tagged_match_ctx.target)
    return;
  assert(lj_str_link_target(observed) == target);
  if (stage == LJ_STR_TEST_MATCH_AFTER_COMPARE) {
    tagged_match_ctx.after_compare++;
    if (tagged_match_ctx.action == TAGGED_HOOK_TAG_AFTER_COMPARE) {
      assert((observed & LJ_STRHASH_DEAD) == 0);
      expect = observed;
      assert(lj_str_link_cas_acqrel(link, &expect,
				    observed | LJ_STRHASH_DEAD));
      tagged_match_ctx.mutations++;
      tagged_match_ctx.action = TAGGED_HOOK_NONE;
    }
  } else {
    assert(stage == LJ_STR_TEST_MATCH_BEFORE_RESCUE_CAS);
    tagged_match_ctx.before_cas++;
    if (tagged_match_ctx.action == TAGGED_HOOK_CLEAR_BEFORE_CAS) {
      assert((observed & LJ_STRHASH_DEAD) != 0);
      expect = observed;
      assert(lj_str_link_cas_acqrel(link, &expect,
				    observed & ~(uintptr_t)LJ_STRHASH_DEAD));
      (void)lj_gc2_preserve_sweep_root(G(L), target);
      tagged_match_ctx.mutations++;
      tagged_match_ctx.action = TAGGED_HOOK_NONE;
    }
  }
}

static void reset_tagged_match_hook(GCobj *target, uint32_t action)
{
  memset(&tagged_match_ctx, 0, sizeof(tagged_match_ctx));
  tagged_match_ctx.target = target;
  tagged_match_ctx.action = action;
  lj_str_test_set_match_hook(tagged_match_hook);
}

static void exercise_tagged_link_protocol(lua_State *L)
{
  enum { TEST_MASK = 4095u };
  global_State *g = G(L);
  StrTabHdr *hdr;
  GCRef synthetic;
  GCRef *head;
  GCstr *target, *prepended;
  uintptr_t expect, raw;
  StrHash target_hash;
  MSize mask, bucket;
  char target_buf[64], prepend_buf[64];
  uint32_t n;

  /* Raw link CAS preserves unrelated tags, and next links strip SECONDARY only. */
  synthetic.gcptr64 = (uint64_t)((uintptr_t)obj2gco(&g->strempty) |
				 LJ_STRHASH_DEAD | LJ_STRHASH_SECONDARY);
  expect = lj_str_link_load_acq(&synthetic);
  assert(lj_str_link_target(expect) == obj2gco(&g->strempty));
  assert(lj_str_link_cas_acqrel(&synthetic, &expect,
				expect & ~(uintptr_t)LJ_STRHASH_DEAD));
  raw = lj_str_link_load_acq(&synthetic);
  assert((raw & LJ_STRHASH_DEAD) == 0);
  assert((raw & LJ_STRHASH_SECONDARY) != 0);

  lj_str_resize(L, TEST_MASK);
  hdr = lj_str_tabh_acq(g);
  assert(hdr != NULL);
  mask = hdr->mask;
  assert(mask >= TEST_MASK);

  /* Pick an empty primary bucket, then calculate a distinct colliding name. */
  for (n = 0;; n++) {
    MSize len;
    snprintf(target_buf, sizeof(target_buf),
	     "m5-strtab-tag-target-%08x", n);
    len = (MSize)strlen(target_buf);
    target_hash = lj_str_test_hash(g, target_buf, len);
    bucket = target_hash & mask;
    raw = lj_str_link_load_acq(&hdr->bucket[bucket]);
    if (raw == 0)
      break;
  }
  for (n = 0;; n++) {
    MSize len;
    snprintf(prepend_buf, sizeof(prepend_buf),
	     "m5-strtab-tag-prepend-%08x", n);
    len = (MSize)strlen(prepend_buf);
    if ((lj_str_test_hash(g, prepend_buf, len) & mask) == bucket)
      break;
  }

  target = lj_str_new(L, target_buf, strlen(target_buf));
  assert(target != NULL && target->dense_valid == 0);
  head = &hdr->bucket[bucket];
  raw = lj_str_link_load_acq(head);
  assert(lj_str_link_target(raw) == obj2gco(target));
  assert((raw & LJ_STRHASH_LINKMASK) == 0);

  /* A prepend moves the old head tag to the new object's next link. */
  expect = raw;
  assert(lj_str_link_cas_acqrel(head, &expect, raw | LJ_STRHASH_DEAD));
  prepended = lj_str_new(L, prepend_buf, strlen(prepend_buf));
  assert(prepended != NULL && prepended != target);
  raw = lj_str_link_load_acq(head);
  assert(lj_str_link_target(raw) == obj2gco(prepended));
  assert((raw & LJ_STRHASH_DEAD) == 0);
  assert((raw & LJ_STRHASH_SECONDARY) == 0);
  raw = lj_str_link_load_acq(lj_str_nodelink(hdr, obj2gco(prepended)));
  assert(lj_str_link_target(raw) == obj2gco(target));
  assert((raw & LJ_STRHASH_DEAD) != 0);
  assert((raw & LJ_STRHASH_SECONDARY) == 0);

  /* A competing rescue wins the CAS; the loser restarts and keeps identity. */
  reset_tagged_match_hook(obj2gco(target), TAGGED_HOOK_CLEAR_BEFORE_CAS);
  assert(lj_str_new(L, target_buf, strlen(target_buf)) == target);
  lj_str_test_set_match_hook(NULL);
  assert(tagged_match_ctx.mutations == 1);
  assert(tagged_match_ctx.before_cas == 1);
  assert(tagged_match_ctx.after_compare == 2);  /* Retry reached the match. */
  raw = lj_str_link_load_acq(lj_str_nodelink(hdr, obj2gco(prepended)));
  assert((raw & LJ_STRHASH_DEAD) == 0);

  /* Tagging after bytes match is observed, cleared and rescued before return. */
  reset_tagged_match_hook(obj2gco(prepended),
			  TAGGED_HOOK_TAG_AFTER_COMPARE);
  assert(lj_str_new(L, prepend_buf, strlen(prepend_buf)) == prepended);
  lj_str_test_set_match_hook(NULL);
  assert(tagged_match_ctx.mutations == 1);
  assert(tagged_match_ctx.after_compare == 1);
  assert(tagged_match_ctx.before_cas == 1);
  raw = lj_str_link_load_acq(head);
  assert(lj_str_link_target(raw) == obj2gco(prepended));
  assert((raw & LJ_STRHASH_LINKMASK) == 0);
}

static void exercise_resize_ignores_legacy_colors(lua_State *L)
{
  enum { TEST_MASK = 4095u, TEST_CHAIN = 34u };
  global_State *g = G(L);
  StrTabHdr *hdr;
  GCstr *members[TEST_CHAIN];
  char names[TEST_CHAIN][64];
  size_t lens[TEST_CHAIN];
  MSize bucket = 0, found = 0;
  uint8_t deadwhite;
  uint32_t n;
  int oldstate;

  /* The compatibility collector state is deliberately adversarial. The
  ** atomic string topology claim, not GCSsweepstring, owns resize exclusion. */
  oldstate = g->gc.state;
  g->gc.state = GCSsweepstring;
  lj_str_resize(L, TEST_MASK);
  g->gc.state = (uint8_t)oldstate;
  hdr = lj_str_tabh_acq(g);
  assert(hdr != NULL && hdr->mask == TEST_MASK);

  /* Keep a real collision chain through an ordinary directory replacement. */
  for (n = 0; found < TEST_CHAIN; n++) {
    char candidate[64];
    MSize len, b;
    int written = snprintf(candidate, sizeof(candidate),
			   "m5-strtab-rehash-color-%08x", n);
    assert(written > 0 && (size_t)written < sizeof(candidate));
    len = (MSize)written;
    b = lj_str_test_hash(g, candidate, len) & hdr->mask;
    if (found == 0) {
      if (lj_str_link_load_acq(&hdr->bucket[b]) != 0)
	continue;
      bucket = b;
    } else if (b != bucket) {
      continue;
    }
    memcpy(names[found], candidate, len + 1u);
    lens[found] = len;
    found++;
  }

  assert(lj_str_sweep_claim(L, hdr) == 1);
  assert(lj_str_resize_state_acq(hdr) == TEST_STRTAB_SWEEP);
  for (n = 0; n < TEST_CHAIN; n++) {
    members[n] = lj_str_new(L, names[n], lens[n]);
    assert(members[n] != NULL && members[n]->dense_valid == 0);
  }
  lj_str_sweep_release(hdr);
  assert(lj_str_resize_state_acq(hdr) == 0);

  /* The retired implementation swept this chain as a side effect whenever
  ** the compatibility state and header whites happened to look old. Those
  ** fields are not GC2 liveness input and rehash must only change topology. */
  deadwhite = (uint8_t)(otherwhite(g) & LJ_GC_WHITES);
  for (n = 0; n < TEST_CHAIN; n++) {
    lj_obj_masksetgcflags(obj2gco(members[n]), LJ_GC_COLORS, deadwhite);
    assert(isdead(g, obj2gco(members[n])));
  }
  oldstate = g->gc.state;
  g->gc.state = GCSsweepstring;
  lj_str_resize(L, (TEST_MASK << 1) + 1u);
  g->gc.state = (uint8_t)oldstate;
  assert(lj_str_tabh_acq(g) != hdr);

  for (n = 0; n < TEST_CHAIN; n++) {
    assert(members[n]->gct == (uint8_t)~LJ_TSTR);
    assert(members[n]->len == lens[n]);
    assert(memcmp(strdata(members[n]), names[n], lens[n] + 1u) == 0);
    assert(lj_str_new(L, names[n], lens[n]) == members[n]);
    lj_obj_masksetgcflags(obj2gco(members[n]), LJ_GC_COLORS,
			 (uint8_t)curwhite(g));
  }
}

typedef struct PausedTopology {
  lua_State *peer;
  lua_State *observer;
  StrTabHdr *hdr;
  MSize target_mask;
  uint32_t allocations;
  MSize destination;
  const char *quarantined;
  uint32_t stage;
  uint32_t stopped;
  uint32_t release;
  uint32_t returned;
  uint32_t done;
} PausedTopology;

static PausedTopology *paused_topology;

static void pause_moved_string(lua_State *L, GCstr *s, StrCanonRec *rec,
			       uint32_t stage)
{
  PausedTopology *p = paused_topology;
  MSize owner;
  if (L == p->observer && stage == LJ_STR_TEST_CANON_RESIZE_ALLOC) {
    p->allocations++;
    return;
  }
  if (L != p->peer || stage != p->stage || la_load32_acq(&p->stopped))
    return;
  assert(rec == NULL);
  owner = lj_str_resize_state_acq(p->hdr);
  assert(owner == TEST_STRTAB_RESIZE);
  if (stage == LJ_STR_TEST_CANON_EXPAND_BUCKET_DONE ||
      stage == LJ_STR_TEST_CANON_EXPAND_FROZEN ||
      stage == LJ_STR_TEST_CANON_EXPAND_PUBLISHED) {
    StrTabHdr *target = lj_str_expand_to_acq(p->hdr);
    assert(s == NULL && target != NULL);
    p->destination = p->hdr->mask < target->mask ? p->hdr->mask :
                    target->mask;
  } else {
    assert(s != NULL);
  }
  la_store32_rel(&p->stopped, 1);
  while (!la_load32_acq(&p->release))
    test_sleep_without_tg(100000);
}

static void *move_string_worker(void *ud)
{
  PausedTopology *p = (PausedTopology *)ud;
  assert(lj_threading_attach_wait(p->peer));
  lj_str_resize(p->peer, p->target_mask);
  la_store32_rel(&p->returned, 1);
  lj_threading_detach(p->peer, 1);
  la_store32_rel(&p->done, 1);
  return NULL;
}

typedef struct GrowthRace {
  lua_State *worker;
  uint32_t parked, resume, moved;
  char last[48];
  const char *identity;
} GrowthRace;

static GrowthRace *growth_race;

static void pause_growth_allocation(lua_State *L, GCstr *s, StrCanonRec *rec,
                                    uint32_t stage)
{
  GrowthRace *race = growth_race;
  UNUSED(s); UNUSED(rec);
  if (L != race->worker)
    return;
  if (stage == LJ_STR_TEST_CANON_RESIZE_ALLOC && !la_load32_acq(&race->parked)) {
    la_store32_rel(&race->parked, 1);
    while (!la_load32_acq(&race->resume))
      test_sleep_without_tg(100000);
  } else if (stage == LJ_STR_TEST_CANON_RESIZE_MOVED) {
    la_store32_rel(&race->moved, 1);
  }
}

static void *insert_until_growth(void *ud)
{
  GrowthRace *race = (GrowthRace *)ud;
  lua_State *L = race->worker;
  unsigned i;
  assert(lj_threading_attach_wait(L));
  for (i = 0; !la_load32_acq(&race->parked); i++) {
    int len = snprintf(race->last, sizeof(race->last), "growth-owner-%08x", i);
    assert(len > 0 && (size_t)len < sizeof(race->last));
    lua_pushlstring(L, race->last, (size_t)len);
    race->identity = lua_tostring(L, -1);
    lua_pop(L, 1);
  }
  lj_threading_detach(L, 1);
  return NULL;
}

static void exercise_stale_growth(int generations)
{
  lua_State *L = luaL_newstate();
  GrowthRace race = { 0 };
  StrTabHdr *current;
  LJThr peer;
  MSize mask, i;
  char bytes[48];
  assert(L);
  lua_gc(L, LUA_GCSTOP, 0);
  race.worker = lua_newthread(L);
  growth_race = &race;
  lj_str_test_set_canon_hook(pause_growth_allocation);
  assert(lj_thr_create(&peer, insert_until_growth, &race) == 0);
  while (!la_load32_acq(&race.parked))
    test_sleep_without_tg(100000);
  mask = lj_str_tabh_acq(G(L))->mask;
  mask = ((mask + 1u) << generations) - 1u;
  for (i = 0; lj_str_tabh_acq(G(L))->mask < mask; i++) {
    int len = snprintf(bytes, sizeof(bytes), "growth-peer-%08x", i);
    assert(i < 4u * (mask + 1u) && len > 0 && (size_t)len < sizeof(bytes));
    lua_pushlstring(L, bytes, (size_t)len);
    lua_pop(L, 1);
  }
  current = lj_str_tabh_acq(G(L));
  assert(current->mask == mask);
  la_store32_rel(&race.resume, 1);
  assert(lj_thr_join(&peer, NULL) == 0);
  lj_str_test_set_canon_hook(NULL);
  growth_race = NULL;
  assert(!la_load32_acq(&race.moved) && lj_str_tabh_acq(G(L)) == current);
  lua_pushstring(L, race.last);
  assert(lua_tostring(L, -1) == race.identity);
  lua_close(L);
}

static void check_paused_topology(lua_State *L, PausedTopology *p,
				  const char *bytes, size_t len)
{
  LJThr peer;
  uint32_t i;
  GCstr *expected, *qexpected = NULL, *inserted = NULL;
  const char *identity;
  enum { MERGE_SOURCES = 4 };
  GCstr *merged[MERGE_SOURCES];
  char new_key[72], merged_keys[MERGE_SOURCES][64];
  size_t new_len = 0, observed = 0;
  MSize merges = 0, m;
  int top = lua_gettop(L);
  lua_pushlstring(L, bytes, len);
  expected = strV(L->top - 1);
  identity = lua_tolstring(L, -1, NULL);
  if (p->quarantined) {
    qexpected = lj_str_newz(L, p->quarantined);
    assert(!qexpected->dense_valid);
    assert(lj_str_test_quarantine_detach(L, qexpected));
  }
  p->peer = lua_newthread(L);
  p->hdr = lj_str_tabh_acq(G(L));
  if (p->stage == LJ_STR_TEST_CANON_EXPAND_FROZEN &&
      p->target_mask < p->hdr->mask && !lj_str_second_acq(G(L))) {
    global_State *g = G(L);
    merges = (p->hdr->mask + 1u) / (p->target_mask + 1u);
    assert(merges == MERGE_SOURCES);
    for (m = 0; m < merges; m++) {
      MSize source = (m + 1u) * (p->target_mask + 1u) - 1u;
      for (i = 0;; i++) {
        int len = snprintf(merged_keys[m], sizeof(merged_keys[m]),
                           "%08x-paused-shrink-source-%u-%08x",
                           i * UINT32_C(0x9e3779b9), m, i);
        assert(len > 0 && (size_t)len < sizeof(merged_keys[m]));
        if ((lj_str_test_hash(g, merged_keys[m], (MSize)len) &
             p->hdr->mask) == source)
          break;
      }
      lua_pushstring(L, merged_keys[m]);
      merged[m] = strV(L->top - 1);
    }
  }
  paused_topology = p;
  lj_str_test_set_canon_hook(pause_moved_string);
  assert(lj_thr_create(&peer, move_string_worker, p) == 0);
  for (i = 0; i < 50000u && !la_load32_acq(&p->stopped) &&
       !la_load32_acq(&p->done); i++)
    test_sleep_without_tg(100000);
  assert(la_load32_acq(&p->stopped) && !la_load32_acq(&p->done));
  if (p->stage == LJ_STR_TEST_CANON_EXPAND_BUCKET_DONE ||
             p->stage == LJ_STR_TEST_CANON_EXPAND_FROZEN ||
             p->stage == LJ_STR_TEST_CANON_EXPAND_PUBLISHED) {
    global_State *g = G(L);
    StrTabHdr *target = lj_str_expand_to_acq(p->hdr);
    MSize hash;
    assert(target && lj_str_tabh_acq(g) == p->hdr);
    if (p->stage != LJ_STR_TEST_CANON_EXPAND_BUCKET_DONE) {
      uintptr_t oldhead = lj_str_link_load_acq(&p->hdr->bucket[p->destination]);
      assert(la_load32_acq(&target->copy_cursor) == p->destination + 1u);
      assert(!!(oldhead & LJ_STRHASH_FROZEN) ==
             (p->stage == LJ_STR_TEST_CANON_EXPAND_FROZEN));
    }
    for (i = 0;; i++) {
      int n = snprintf(new_key, sizeof(new_key), "%08x-paused-resize-insert-%x-%u-%u-%08x",
                       i * UINT32_C(0x9e3779b9), p->target_mask, p->stage,
                       (unsigned)expected->dense_valid, i);
      assert(n > 0 && (size_t)n < sizeof(new_key));
      new_len = (size_t)n;
      hash = lj_str_test_hash(g, new_key, (MSize)n);
      if ((hash & p->destination) == p->destination &&
          !lj_str_bucket_dense(p->hdr, hash))
        break;
    }
    for (m = 1; m < merges; m++) {
      MSize source = (m + 1u) * (target->mask + 1u) - 1u;
      assert(!(lj_str_link_load_acq(&p->hdr->bucket[source]) &
               LJ_STRHASH_FROZEN));
    }
    lua_pushlstring(L, new_key, new_len);
    inserted = strV(L->top - 1);
    assert(lj_str_tabh_acq(g) == p->hdr);
    for (m = 0; m < merges; m++) {
      MSize source = (m + 1u) * (target->mask + 1u) - 1u;
      assert(lj_str_link_load_acq(&p->hdr->bucket[source]) &
             LJ_STRHASH_FROZEN);
      lua_pushstring(L, merged_keys[m]);
      assert(strV(L->top - 1) == merged[m]);
    }
    assert(lj_str_link_target(lj_str_link_load_acq(
      &target->bucket[hash & target->mask])) == obj2gco(inserted));
    lua_pushlstring(L, new_key, new_len);
    assert(strV(L->top - 1) == inserted);

  }
  /* Run the ordinary public interner after the writer has already changed
  ** a chain link, while its claim remains installed on the old header. */
  lua_pushlstring(L, bytes, len);
  assert(strV(L->top - 1) == expected);
  assert(lua_tolstring(L, -1, &observed) == identity && observed == len);
  if (p->quarantined) {
    lua_pushstring(L, p->quarantined);
    assert(strV(L->top - 1) == qexpected);
    assert(lj_str_canon_state(lj_str_canon_acq(qexpected)) ==
           LJ_STR_CANON_QRESCUED);
  }
  assert(lj_str_resize_state_acq(p->hdr) == TEST_STRTAB_RESIZE);
  assert(!la_load32_acq(&p->done));
  la_store32_rel(&p->release, 1);
  assert(lj_thr_join(&peer, NULL) == 0);
  assert(la_load32_acq(&p->done));
  lj_str_test_set_canon_hook(NULL);
  paused_topology = NULL;
  lua_pushlstring(L, bytes, len);
  assert(strV(L->top - 1) == expected);
  if (inserted) {
    lua_pushlstring(L, new_key, new_len);
    assert(strV(L->top - 1) == inserted);
  }
  if (qexpected) {
    global_State *g = G(L);
    for (i = 0; i < LJ_GC2_GRACE_EPOCHS; i++)
      (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
    (void)lj_gc2_reclaim_retired(g, lj_gc2_retire_epoch(g));
    assert(lj_str_canon_acq(qexpected) == LJ_STR_CANON_LIVE);
    assert(lj_str_newz(L, p->quarantined) == qexpected);
  }
  lua_settop(L, top);
}

static void exercise_paused_allocation_pressure(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  PausedTopology pause = { 0 };
  StrTabHdr *target;
  LJThr peer = { 0 };
  MSize count, before, i;
  uint32_t waits, mode = LJ_GC2_SMR_OPEN;
  int anchors;
  char bytes[64];
  assert(L);
  g = G(L);
  (void)lua_gc(L, LUA_GCSTOP, 0);
  pause.observer = L;
  pause.peer = lua_newthread(L);
  pause.hdr = lj_str_tabh_acq(g);
  lj_str_resize(L, (pause.hdr->mask << 1) + 1u);
  pause.hdr = lj_str_tabh_acq(g);
  assert(!lj_str_second_acq(g));
  pause.target_mask = (pause.hdr->mask << 1) + 1u;
  pause.stage = LJ_STR_TEST_CANON_EXPAND_FROZEN;
  count = 2u * (pause.target_mask + 1u);
  lua_createtable(L, (int)count, 0);
  assert(lj_str_retired_head_acq(g));
  anchors = lua_gettop(L);
  paused_topology = &pause;
  lj_str_test_set_canon_hook(pause_moved_string);
  assert(lj_thr_create(&peer, move_string_worker, &pause) == 0);
  for (waits = 0; waits < 50000u && !la_load32_acq(&pause.stopped); waits++)
    test_sleep_without_tg(100000);
  assert(la_load32_acq(&pause.stopped) && !la_load32_acq(&pause.done));
  target = lj_str_expand_to_acq(pause.hdr);
  assert(target && lj_str_tabh_acq(g) == pause.hdr);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(lj_gc2_reclaim_retired(g, lj_gc2_retire_epoch(g) + 1u) != 0);
  assert(lj_str_retired_head_acq(g) == NULL);
  assert(lj_str_tabh_acq(g) == pause.hdr);
  assert(lj_str_expand_to_acq(pause.hdr) == target);

  lj_str_flush_num_credit(g, L2TG(L));
  before = lj_str_num_acq(g);
  for (i = 0; i < count; i++) {
    uint32_t sample = i;
    int len;
    do {
      len = snprintf(bytes, sizeof(bytes), "%08x-paused-pressure-%08x",
                     sample++ * UINT32_C(0x9e3779b9), i);
      assert(len > 0 && (size_t)len < sizeof(bytes));
    } while (i == 0 &&
             (lj_str_test_hash(g, bytes, (MSize)len) & pause.hdr->mask));
    lua_pushlstring(L, bytes, (size_t)len);
    lua_rawseti(L, anchors, (int)i+1);
  }
  lj_str_flush_num_credit(g, L2TG(L));
  assert(lj_str_num_acq(g) == before + count);
  assert(la_load32_acq(&target->copy_cursor) == 0);
  assert(lj_str_tabh_acq(g) == pause.hdr && !la_load32_acq(&pause.done));
  assert(pause.allocations == 0);

  /* Model a metadata reader refusal at the old helper's final handoff.
  ** It must drop the source pin and return without dereferencing raw state. */
  assert(gc2_smr_reclaiming_cas(g, &mode, LJ_GC2_SMR_META_EXCLUSIVE));
  la_store32_rel(&pause.release, 1);
  for (waits = 0; waits < 50000u && !la_load32_acq(&pause.returned); waits++)
    test_sleep_without_tg(100000);
  assert(la_load32_acq(&pause.returned));
  assert(lj_str_tabh_acq(g) == pause.hdr && gc2_smr_readers_acq(g) == 0);
  gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);
  assert(lj_thr_join(&peer, NULL) == 0);
  lua_pushlstring(L, bytes, strlen(bytes));
  lua_pop(L, 1);
  assert(lj_str_tabh_acq(g) == target);
  lj_str_test_set_canon_hook(NULL);
  paused_topology = NULL;

  lj_str_resize(L, (target->mask << 1) + 1u);
  target = lj_str_tabh_acq(g);
  lj_str_resize(L, (target->mask << 1) + 1u);
  assert(lj_str_tabh_acq(g)->mask == (target->mask << 1) + 1u);
  for (i = 0; i < count; i++) {
    const char *identity;
    size_t len;
    lua_rawgeti(L, anchors, (int)i+1);
    identity = lua_tolstring(L, -1, &len);
    assert(identity);
    lua_pushlstring(L, identity, len);
    assert(lua_tolstring(L, -1, NULL) == identity);
    lua_pop(L, 2);
  }
  lua_close(L);
}

static void check_resize_hit_invalidation(lua_State *L, const char *bytes,
					  size_t len)
{
  global_State *g = G(L);
  GCstr *live, *dead;
  LJGC2Lease lease;
  TValue dead_value;
  MSize only_slot;
  MSize slot = lj_str_test_hash(g, bytes, (MSize)len) &
	       (LJ_STR_RESIZE_HIT_COUNT - 1u);
  char other[64];
  uint32_t i;
  for (i = 0;; i++) {
    int n = snprintf(other, sizeof(other), "retired-cached-collision-%08x", i);
    assert(n > 0 && (size_t)n < sizeof(other));
    if ((lj_str_test_hash(g, other, (MSize)n) &
	 (LJ_STR_RESIZE_HIT_COUNT - 1u)) == slot)
      break;
  }
  dead = lj_str_new(L, other, strlen(other));
  assert(la_loadptr_acq((void *const *)&g->str.resize_hits[slot]) == dead);
  setgcVraw(&dead_value, obj2gco(dead), LJ_TSTR);
  lua_pushlstring(L, bytes, len);
  live = strV(L->top - 1);
  assert(live != dead);
  assert(la_loadptr_acq((void *const *)&g->str.resize_hits[slot]) == live);
  do {
    int n = snprintf(other, sizeof(other), "unrooted-cached-string-%08x", i++);
    assert(n > 0 && (size_t)n < sizeof(other));
    only_slot = lj_str_test_hash(g, other, (MSize)n) &
		(LJ_STR_RESIZE_HIT_COUNT - 1u);
  } while (only_slot == slot);
  assert(lj_str_new(L, other, strlen(other)));
  assert(la_loadptr_acq((void *const *)&g->str.resize_hits[only_slot]));
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lj_gc2_tv_lease_acquire(g, &dead_value, &lease) !=
	 LJ_GC2_TV_EDGE_VALID);
  assert(la_loadptr_acq((void *const *)&g->str.resize_hits[only_slot]) == NULL);
  assert(la_loadptr_acq((void *const *)&g->str.resize_hits[slot]) == live);
  lua_pop(L, 1);
}

static void exercise_paused_topology_hits(void)
{
  static const char bytes[] = "string-intern-hit-during-moving-topology";
  lua_State *L = luaL_newstate();
  global_State *g;
  PausedTopology resize = { 0 }, grow = { 0 }, publish = { 0 }, shrink = { 0 };
  assert(L != NULL);
  g = G(L);
  check_resize_hit_invalidation(L, bytes, sizeof(bytes)-1u);
  (void)lua_gc(L, LUA_GCSTOP, 0);
  resize.stage = LJ_STR_TEST_CANON_RESIZE_MOVED;
  resize.target_mask = 4095u;
  check_paused_topology(L, &resize, bytes, sizeof(bytes)-1u);
  assert(lj_str_mask_acq(g) == resize.target_mask);
  grow.stage = LJ_STR_TEST_CANON_EXPAND_FROZEN;
  grow.target_mask = 8191u;
  grow.quarantined = "canonical identity during a paused directory replacement";
  assert(!lj_str_second_acq(g));
  check_paused_topology(L, &grow, bytes, sizeof(bytes)-1u);
  assert(lj_str_mask_acq(g) == grow.target_mask);
  publish.stage = LJ_STR_TEST_CANON_EXPAND_PUBLISHED;
  publish.target_mask = (grow.target_mask << 1) + 1u;
  check_paused_topology(L, &publish, bytes, sizeof(bytes)-1u);
  assert(lj_str_mask_acq(g) == publish.target_mask);
  shrink.stage = LJ_STR_TEST_CANON_EXPAND_FROZEN;
  shrink.target_mask = 4095u;
  check_paused_topology(L, &shrink, bytes, sizeof(bytes)-1u);
  assert(lj_str_mask_acq(g) == shrink.target_mask);

  lua_close(L);
}

static void exercise_canonical_layout(lua_State *L)
{
  global_State *g = G(L);
  static const MSize lens[] = { 1u, 3u, 4u, 7u, 8u, 15u, 16u, 31u };
  char buf[64];
  size_t i;
  assert(strdata(&g->strempty) == (const char *)&g->stremptyz);
  assert(strdata(&g->strempty)[0] == '\0');
  assert(lj_str_canon_acq(&g->strempty) == LJ_STR_CANON_LIVE);
  for (i = 0; i < sizeof(lens)/sizeof(lens[0]); i++) {
    MSize len = lens[i];
    GCstr *s;
    MSize j;
    for (j = 0; j < len; j++)
      buf[j] = (char)('a' + (j + (MSize)i) % 26u);
    s = lj_str_new(L, buf, len);
    assert(s != NULL);
    assert(s->len == len);
    assert(lj_str_canon_acq(s) == LJ_STR_CANON_LIVE);
    assert(memcmp(strdata(s), buf, len) == 0);
    assert(strdata(s)[len] == '\0');
    assert(lj_str_new(L, buf, len) == s);
  }
}

static void canonical_race_hook(lua_State *L, GCstr *s, StrCanonRec *rec,
				uint32_t stage)
{
  CanonRaceCtx *ctx = canon_race_ctx;
  if (!ctx || ctx->pause_stage != stage || ctx->s != s || ctx->worker_L != L)
    return;
  assert(rec != NULL);
  ctx->rec = rec;
  if (stage == LJ_STR_TEST_CANON_Q_PINNED_BEFORE_PUBLISH ||
      stage == LJ_STR_TEST_CANON_Q_BODY_HELD) {
    ctx->qhdr = lj_tg_strq_active_hdr_acq(L2TG(L));
    assert(ctx->qhdr != NULL);
    assert(lj_tg_strq_active_depth_acq(L2TG(L)) != 0);
  }
  la_store32_rel(&ctx->reached, 1);
  while (la_load32_acq(&ctx->release) == 0)
    test_sleep_without_tg(100000);
}

static void *canonical_race_worker(void *arg)
{
  CanonRaceCtx *ctx = (CanonRaceCtx *)arg;
  lua_State *L = ctx->worker_L;
  if (!lj_threading_attach_wait(L)) {
    la_store32_rel(&ctx->done, 1);
    return NULL;
  }
  ctx->worker_tg = lj_thr_get_tg();
  assert(ctx->worker_tg != NULL && L2TG(L) == ctx->worker_tg);
  if (ctx->pause_stage == LJ_STR_TEST_CANON_Q_BODY_HELD)
    ctx->result = lj_str_new(L, strdata(ctx->s), ctx->s->len) == ctx->s;
  else
    ctx->result = lj_str_test_quarantine_detach(L, ctx->s);
  ctx->active_cleared =
    lj_tg_strq_active_hdr_acq(ctx->worker_tg) == NULL &&
    lj_tg_strq_active_depth_acq(ctx->worker_tg) == 0;
  lj_threading_detach(L, 1);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void canonical_race_start(CanonRaceCtx *ctx, lua_State *L, GCstr *s,
				 uint32_t stage, LJThr *thr)
{
  int i;
  memset(ctx, 0, sizeof(*ctx));
  ctx->driver_L = L;
  ctx->driver_top = lua_gettop(L);
  ctx->worker_L = lua_newthread(L);  /* Driver stack is the lifetime root. */
  assert(ctx->worker_L != NULL && lua_gettop(L) == ctx->driver_top + 1);
  ctx->s = s;
  ctx->pause_stage = stage;
  canon_race_ctx = ctx;
  lj_str_test_set_canon_hook(canonical_race_hook);
  assert(lj_thr_create(thr, canonical_race_worker, ctx) == 0);
  for (i = 0; i < 50000 && la_load32_acq(&ctx->reached) == 0 &&
	 la_load32_acq(&ctx->done) == 0; i++)
    test_sleep_without_tg(100000);
  assert(la_load32_acq(&ctx->reached) != 0);
  assert(la_load32_acq(&ctx->done) == 0);
}

static void canonical_race_finish(CanonRaceCtx *ctx, LJThr *thr)
{
  la_store32_rel(&ctx->release, 1);
  assert(lj_thr_join(thr, NULL) == 0);
  assert(la_load32_acq(&ctx->done) != 0);
  assert(ctx->result == 1);
  assert(ctx->active_cleared != 0);
  lj_str_test_set_canon_hook(NULL);
  canon_race_ctx = NULL;
  /* Detach leaves a registry-retired TG for the ordinary opportunistic
  ** reclaimer. Drain it before later fixture stages assert that no unrelated
  ** metadata owner vetoes string-header SMR reclamation. */
  assert(lj_tg_reclaim_dead(G(ctx->driver_L)) >= 1u);
  assert(lua_gettop(ctx->driver_L) == ctx->driver_top + 1);
  assert(lua_tothread(ctx->driver_L, -1) == ctx->worker_L);
  lua_pop(ctx->driver_L, 1);
}

static int canonical_qbucket_contains(StrCanonHdr *hdr, StrCanonRec *want)
{
  StrCanonRec *rec;
  for (rec = lj_str_qbucket_acq(&hdr->bucket[want->hash & hdr->mask]);
       rec != NULL; rec = lj_str_qnext_acq(rec)) {
    if (rec == want)
      return 1;
  }
  return 0;
}

static void exercise_canonical_quarantine(lua_State *L)
{
  static const char publish_prefix[] =
    "m5-strtab-canonical-q-publish-resize-race";
  static const char unlink_name[] =
    "m5-strtab-canonical-unlink-qcount-race";
  char publish_name[LJ_HUGE_THRESHOLD + 1u];
  global_State *g = G(L);
  CanonRaceCtx ctx;
  LJThr worker;
  MSize before = lj_str_qcount_acq(g);
  MSize oldmask;
  StrCanonHdr *oldhdr;
  GCstr *spublish, *sunlink;
  LJHugeInfo huge;
  uintptr_t canon;

  /* Exercise the zero -> non-zero correctness gate first in a fresh state.
  ** The test barrier makes this a program-order regression, not a hardware
  ** memory-order litmus: once the exact main edge is absent, a new interner
  ** must take the now-enabled Q lookup and return the identical body. */
  assert(before == 0);
  sunlink = lj_str_new(L, unlink_name, sizeof(unlink_name)-1u);
  assert(sunlink != NULL);
  assert(lj_str_canon_acq(sunlink) == LJ_STR_CANON_LIVE);
  canonical_race_start(&ctx, L, sunlink, LJ_STR_TEST_CANON_MAIN_UNLINKED,
		       &worker);
  assert(ctx.rec != NULL);
  assert(lj_str_qcount_acq(g) == 1u);
#ifdef LJ_GC2_TEST_HELPERS
  /* Refusing a lease cannot permit a competing equal identity. */
  lj_gc2_test_stack_admission_retry_once(obj2gco(sunlink));
#endif
  assert(lj_str_new(L, unlink_name, sizeof(unlink_name)-1u) == sunlink);
#ifdef LJ_GC2_TEST_HELPERS
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
#endif
  canon = lj_str_canon_acq(sunlink);
  assert(lj_str_canon_state(canon) == LJ_STR_CANON_QRESCUED);
  canonical_race_finish(&ctx, &worker);

  /* A publisher parked before the bucket CAS can finish in its original
  ** directory after two larger directories have become current. */
  oldhdr = lj_str_qtabh_acq(g);
  oldmask = oldhdr->mask;
  memset(publish_name, 'z', sizeof(publish_name));
  memcpy(publish_name, publish_prefix, sizeof(publish_prefix)-1u);
  spublish = lj_str_new(L, publish_name, sizeof(publish_name));
  assert(spublish != NULL);
  assert(lj_arena_ishuge(lj_arena_of(spublish)));
  assert(lj_str_canon_acq(spublish) == LJ_STR_CANON_LIVE);
  canonical_race_start(&ctx, L, spublish,
	LJ_STR_TEST_CANON_Q_PINNED_BEFORE_PUBLISH, &worker);
  assert(ctx.rec != NULL);
  assert(ctx.qhdr == oldhdr);
  assert(!canonical_qbucket_contains(ctx.qhdr, ctx.rec));
  assert(la_load32_acq(&ctx.rec->status) == LJ_STR_CANONREC_LIST_ONLY);
  assert(lj_str_canon_acq(spublish) == LJ_STR_CANON_LIVE);
  assert(ctx.worker_tg != NULL);
  assert(lj_tg_strq_active_hdr_acq(ctx.worker_tg) == oldhdr);
  assert(lj_tg_strq_active_depth_acq(ctx.worker_tg) == 1u);
  assert(lj_str_quarantine_resize(L, (oldmask << 1) + 1u));
  assert(lj_str_quarantine_resize(L, (oldmask << 2) + 3u));
  assert(lj_str_qtabh_acq(g)->older->older == oldhdr);
  canonical_race_finish(&ctx, &worker);
  assert(lj_str_qcount_acq(g) == 2u);
  canon = lj_str_canon_acq(spublish);
  assert(lj_str_canon_state(canon) == LJ_STR_CANON_QACTIVE);
  assert(lj_str_canon_record(canon) != NULL);
  assert(canonical_qbucket_contains(oldhdr, lj_str_canon_record(canon)));
  assert(lj_str_qtabh_acq(g)->mask == (oldmask << 2) + 3u);
  /* Ownership-only preservation does not rescue this close intent.
  ** An independently admitted semantic SWEEP root must rescue it. */
  {
    uint32_t phase = gc2_phase_acq(g);
    lj_str_canon_rel(spublish,
		     lj_str_canon_pack(ctx.rec, LJ_STR_CANON_QCLOSING));
    gc2_phase_rel(g, LJ_GC2_SWEEP);
    (void)lj_gc2_preserve_sweep_root(g, obj2gco(spublish));
    assert(lj_str_canon_state(lj_str_canon_acq(spublish)) ==
	   LJ_STR_CANON_QCLOSING);
    (void)lj_gc2_trace_sweep_root(g, obj2gco(spublish));
    assert(lj_str_canon_state(lj_str_canon_acq(spublish)) ==
	   LJ_STR_CANON_QRESCUED);
    gc2_phase_rel(g, phase);
  }

  /* Actor-owned roots retain a closing string during MARK and WEAK.
  ** Install the rooted value before asserting the synthetic close intent. */
  {
    TGState *tg = L2TG(L);
    TValue value;
    uint32_t idx, phase = gc2_phase_acq(g);
    setstrV(L, &value, spublish);
    assert(lj_tg_root_anchor_push(L, tg, &value, &idx));
    lj_str_canon_rel(spublish,
		     lj_str_canon_pack(ctx.rec, LJ_STR_CANON_QCLOSING));
    gc2_phase_rel(g, LJ_GC2_MARK);
    lj_gc2_scan_cycle_owner_tg_roots(g, tg);
    assert(lj_str_canon_state(lj_str_canon_acq(spublish)) ==
	   LJ_STR_CANON_QRESCUED);
    lj_str_canon_rel(spublish,
		     lj_str_canon_pack(ctx.rec, LJ_STR_CANON_QCLOSING));
    gc2_phase_rel(g, LJ_GC2_WEAK);
    lj_gc2_scan_cycle_owner_tg_roots(g, tg);
    assert(lj_str_canon_state(lj_str_canon_acq(spublish)) ==
	   LJ_STR_CANON_QRESCUED);
    gc2_phase_rel(g, phase);
    lj_tg_root_anchor_pop(tg, idx);
  }

  /* Simulate close intent while the directory still owns the huge body.
  ** Pause one interner before it compares the borrowed bytes. Its exact
  ** mapping reader defers reclamation while another interner can rescue. */
  lj_str_canon_rel(spublish,
		   lj_str_canon_pack(ctx.rec, LJ_STR_CANON_QCLOSING));
  canonical_race_start(&ctx, L, spublish, LJ_STR_TEST_CANON_Q_BODY_HELD,
		       &worker);
  assert(lj_arena_hugetab_lookup(&L2TG(L)->huge, spublish, &huge));
  assert(huge.readers != 0);
  assert(lj_str_quarantine_resize(L, (ctx.qhdr->mask << 1) + 1u));
  assert(lj_str_new(L, strdata(spublish), spublish->len) == spublish);
  canonical_race_finish(&ctx, &worker);
  assert(lj_arena_hugetab_lookup(&L2TG(L)->huge, spublish, &huge));
  assert(huge.readers == 0);
  canon = lj_str_canon_acq(spublish);
  assert(lj_str_canon_state(canon) == LJ_STR_CANON_QRESCUED);

  /* An ordinary C API borrow of a rooted canonical string rescues it
  ** before returning the borrowed bytes, with or without a length result. */
  {
    size_t len = 0;
    lua_pushlstring(L, strdata(spublish), spublish->len);
    lj_str_canon_rel(spublish,
		     lj_str_canon_pack(ctx.rec, LJ_STR_CANON_QCLOSING));
    assert(lua_tolstring(L, -1, &len) == strdata(spublish));
    assert(len == spublish->len);
    assert(lj_str_canon_state(lj_str_canon_acq(spublish)) ==
	   LJ_STR_CANON_QRESCUED);
    lj_str_canon_rel(spublish,
		     lj_str_canon_pack(ctx.rec, LJ_STR_CANON_QCLOSING));
    assert(lua_tolstring(L, -1, NULL) == strdata(spublish));
    assert(lj_str_canon_state(lj_str_canon_acq(spublish)) ==
	   LJ_STR_CANON_QRESCUED);
    lua_pop(L, 1);
  }

  /* A guarded table store retains exact string inputs before publishing its
  ** descriptor and re-arbitrates close before authorizing the actual CAS. */
  {
    TValue key, value;
    LJGC2TableStoreGuard guard;
    GCtab *tab;
    StrCanonRec *other = lj_str_canon_record(lj_str_canon_acq(sunlink));
    lua_newtable(L);
    tab = tabV(L->top - 1);
    setstrV(L, &key, spublish);
    setstrV(L, &value, sunlink);
    lj_str_canon_rel(spublish,
		     lj_str_canon_pack(ctx.rec, LJ_STR_CANON_QCLOSING));
    lj_str_canon_rel(sunlink,
		     lj_str_canon_pack(other, LJ_STR_CANON_QCLOSING));
    (void)lj_gc2_table_store_begin(L, &guard, tab, &key, &value);
    assert(guard.begun && !guard.finished);
    assert(lj_str_canon_state(lj_str_canon_acq(spublish)) ==
	   LJ_STR_CANON_QRESCUED);
    assert(lj_str_canon_state(lj_str_canon_acq(sunlink)) ==
	   LJ_STR_CANON_QRESCUED);
    (void)lj_gc2_table_store_admit(L, &guard);
    assert(guard.gate_admitted);
    lj_str_canon_rel(sunlink,
		     lj_str_canon_pack(other, LJ_STR_CANON_QCLOSING));
    (void)lj_gc2_table_store_revalidate(L, &guard);
    assert(guard.store_authorized);
    assert(lj_str_canon_state(lj_str_canon_acq(sunlink)) ==
	   LJ_STR_CANON_QRESCUED);
    (void)lj_gc2_table_store_finish(L, &guard, 0);
    assert(guard.finished && !guard.cleanup_failed);
    lua_pop(L, 1);
  }

  /* The collector must also rescue a value reached by traversing a
  ** strong table, even when the Lua stack never contains the string. */
  lua_newtable(L);
  lua_pushlstring(L, strdata(sunlink), sunlink->len);
  lua_rawseti(L, -2, 1);
  canon = lj_str_canon_acq(sunlink);
  lj_str_canon_rel(sunlink,
		   lj_str_canon_pack(lj_str_canon_record(canon),
				     LJ_STR_CANON_QCLOSING));
  lua_pushlstring(L, publish_name, sizeof(publish_name));
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  {
    uint32_t i;
    for (i = 0; i < LJ_GC2_GRACE_EPOCHS; i++)
      (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  }
  assert(lj_str_qcount_acq(g) == before);
  assert(lj_str_canon_acq(spublish) == LJ_STR_CANON_LIVE);
  assert(lj_str_canon_acq(sunlink) == LJ_STR_CANON_LIVE);
  assert(lj_str_new(L, publish_name, sizeof(publish_name)) == spublish);
  assert(lj_str_new(L, unlink_name, sizeof(unlink_name)-1u) == sunlink);
  lua_pop(L, 2);

  /* Saturation cannot wrap the Q presence test back to a false zero. */
  lj_str_qcount_store_rlx(g, UINT32_MAX - 1u);
  assert(lj_str_qcount_inc_sat_acqrel(g) == UINT32_MAX - 1u);
  assert(lj_str_qcount_acq(g) == UINT32_MAX);
  assert(lj_str_qcount_inc_sat_acqrel(g) == UINT32_MAX);
  assert(lj_str_qcount_acq(g) == UINT32_MAX);
}

static MSize assert_resize_state(StrTabHdr *hdr)
{
  MSize state = lj_str_resize_state_acq(hdr);
  assert((state & TEST_STRTAB_OWNER_LOBITS) == 0);
  assert(state == 0 || state == TEST_STRTAB_RESIZE ||
	 state == TEST_STRTAB_SWEEP);
  return state;
}

static void assert_resize_idle(StrTabHdr *hdr)
{
  assert(assert_resize_state(hdr) == 0);
}

static void assert_resize_claimed(StrTabHdr *hdr)
{
  assert(assert_resize_state(hdr) == TEST_STRTAB_RESIZE);
}

static void assert_sweep_claimed(StrTabHdr *hdr)
{
  assert(assert_resize_state(hdr) == TEST_STRTAB_SWEEP);
}

static int cmp_strid(const void *a, const void *b)
{
  StrID x = *(const StrID *)a;
  StrID y = *(const StrID *)b;
  return (x > y) - (x < y);
}

static void exercise_string_id_blocks(lua_State *L)
{
  enum { N = 192 };
  GCstr *s[N];
  StrID ids[N];
  uint32_t refills0, refills1;
  int i;

  lj_str_test_reset_id_refills();
  refills0 = lj_str_test_id_refills();

  for (i = 0; i < N; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-id-block-%03d", i);
    s[i] = lj_str_new(L, buf, strlen(buf));
    assert(s[i] != NULL);
    ids[i] = s[i]->sid;
  }

  refills1 = lj_str_test_id_refills();
  assert(refills1 > refills0);
  assert(refills1 - refills0 < N / 4);

  qsort(ids, N, sizeof(ids[0]), cmp_strid);
  for (i = 1; i < N; i++)
    assert(ids[i] != ids[i - 1]);

  for (i = 0; i < N; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-id-block-%03d", i);
    assert(lj_str_new(L, buf, strlen(buf)) == s[i]);
  }
}

static void exercise_string_count_blocks(lua_State *L)
{
  enum { N = 96 };
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCstr *s[N];
  MSize before, published, exact;
  uint32_t refills0, refills1;
  int i;

  lj_str_flush_num_credit(g, tg);
  assert(tg->strnum_credit == 0);
  before = lj_str_num_acq(g);
  lj_str_test_reset_num_refills();
  refills0 = lj_str_test_num_refills();

  for (i = 0; i < N; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-num-block-%03d", i);
    s[i] = lj_str_new(L, buf, strlen(buf));
    assert(s[i] != NULL);
  }

  refills1 = lj_str_test_num_refills();
  assert(refills1 > refills0);
  assert(refills1 - refills0 < N / 4);
  published = lj_str_num_acq(g);
  assert(published >= before + N);
  assert(published == before + N + tg->strnum_credit);

  lj_str_flush_num_credit(g, tg);
  exact = lj_str_num_acq(g);
  assert(tg->strnum_credit == 0);
  assert(exact == before + N);

  for (i = 0; i < N; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-num-block-%03d", i);
    assert(lj_str_new(L, buf, strlen(buf)) == s[i]);
  }
  assert(lj_str_test_num_refills() == refills1);
  assert(lj_str_num_acq(g) == exact);
}

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
static void *fail_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  FailAllocCtx *ctx = (FailAllocCtx *)ud;
  UNUSED(osize);
  if (nsize == 0) {
    free(ptr);
    return NULL;
  }
  if (ctx->fail_next && nsize >= ctx->fail_min) {
    ctx->fail_next = 0;
    return NULL;
  }
  return realloc(ptr, nsize);
}
#endif

static void *release_active_after_claim(void *arg)
{
  ActiveReleaseCtx *ctx = (ActiveReleaseCtx *)arg;
  while (assert_resize_state(ctx->hdr) == 0)
    (void)lj_thr_sleep_ns(NULL, 100000);
  assert(assert_resize_state(ctx->hdr) != 0);
  la_store32_rel(&ctx->claimed, 1);
  assert(lj_tg_strtab_active_hdr_acq(ctx->tg) == ctx->hdr);
  assert(lj_tg_strtab_active_depth_acq(ctx->tg) == 1u);
  lj_tg_strtab_active_depth_rel(ctx->tg, 0);
  lj_tg_strtab_active_hdr_rel(ctx->tg, NULL);
  la_store32_rel(&ctx->cleared, 1);
  return NULL;
}

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
static int protected_resize(lua_State *L)
{
  ResizeOOMCtx *ctx = (ResizeOOMCtx *)lua_touserdata(L, 1);
  lj_str_resize(L, ctx->newmask);
  return 0;
}
#endif

static void *reclaim_retired_worker(void *arg)
{
  ReclaimReaderCtx *ctx = (ReclaimReaderCtx *)arg;
  ctx->reclaimed = lj_gc2_reclaim_retired(ctx->g, ctx->epoch);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static StrBodyRetire *synthetic_pending_record(lua_State *L, GCstr *s,
					       uint32_t status)
{
  StrBodyRetire *ret = lj_mem_newt(L, sizeof(*ret), StrBodyRetire);
  memset(ret, 0, sizeof(*ret));
  lj_str_body_retired_next_rel(ret, NULL);
  lj_str_body_retired_str_rel(ret, s);
  lj_str_body_retired_hdr_rel(ret, lj_str_tabh_acq(G(L)));
  lj_str_body_retired_epoch_rel(ret, lj_gc2_retire_epoch(G(L)));
  lj_str_body_retired_main_linked_rel(ret, 1);
  la_store32_rel(&ret->status, status);
  return ret;
}

static void exercise_terminal_pending_ownership(lua_State *L, GCstr *s)
{
  global_State *g = G(L);
  StrBodyRetire *ret, *expect;
  assert(lj_str_sweep_pending_acq(g) == NULL);
  assert(lj_str_body_retired_head_acq(g) == NULL);

  /* A pre-CAS pending record owns metadata only. Terminal cleanup must leave
  ** the still-interned body to its bucket and retire exactly this record. */
  ret = synthetic_pending_record(L, s, 0);
  lj_str_sweep_pending_rel(g, ret);
  lj_str_free_retired_bodies(g);
  assert(lj_str_sweep_pending_acq(g) == NULL);
  assert(lj_str_body_retired_head_acq(g) == NULL);
  assert(lj_str_new(L, strdata(s), s->len) == s);

  /* Model the push-before-pending-clear overlap at the worker handoff. The
  ** retired list is the durable owner, so the redundant slot must not cause a
  ** second record free. LIST_ONLY deliberately leaves the bucket-owned body. */
  ret = synthetic_pending_record(L, s, LJ_STR_CANONREC_LIST_ONLY);
  expect = NULL;
  assert(lj_str_body_retired_head_cas(g, &expect, ret));
  lj_str_sweep_pending_rel(g, ret);
  lj_str_free_retired_bodies(g);
  assert(lj_str_sweep_pending_acq(g) == NULL);
  assert(lj_str_body_retired_head_acq(g) == NULL);
  assert(lj_str_new(L, strdata(s), s->len) == s);

  /* A duplicate intrusive push forms a self/cycle. Detection must happen
  ** before the first mutation/free, so repairing this synthetic edge leaves a
  ** complete one-record list which terminal cleanup can consume normally. */
  ret = synthetic_pending_record(L, s, LJ_STR_CANONREC_LIST_ONLY);
  lj_str_body_retired_next_rel(ret, ret);
  assert(!lj_str_test_body_retired_chain_valid(g, ret));
  lj_str_body_retired_next_rel(ret, NULL);
  expect = NULL;
  assert(lj_str_body_retired_head_cas(g, &expect, ret));
  lj_str_free_retired_bodies(g);
  assert(lj_str_body_retired_head_acq(g) == NULL);
  assert(lj_str_new(L, strdata(s), s->len) == s);
}

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
static void exercise_resize_oom_does_not_claim(void)
{
  FailAllocCtx allocctx = { 0, 0 };
  lua_State *L = lua_newstate(fail_alloc, &allocctx);
  global_State *g;
  StrTabHdr *hdr;
  ResizeOOMCtx resizectx;
  int rc;

  assert(L != NULL);
  g = G(L);
  hdr = lj_str_tabh_acq(g);
  assert(hdr != NULL);
  assert_resize_idle(hdr);

  resizectx.newmask = (hdr->mask << 1) + 1u;
  lua_pushcfunction(L, protected_resize);
  lua_pushlightuserdata(L, &resizectx);
  allocctx.fail_min = lj_str_tabsize(resizectx.newmask);
  allocctx.fail_next = 1;
  rc = lua_pcall(L, 1, 0, 0);
  assert(rc == LUA_ERRMEM);
  assert(allocctx.fail_next == 0);
  assert(lj_str_tabh_acq(g) == hdr);
  assert_resize_idle(hdr);
  lua_settop(L, 0);
  assert(lj_str_new(L, "m5-strtab-resize-oom",
		    strlen("m5-strtab-resize-oom")) != NULL);

  lua_close(L);
}
#endif

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  StrTabHdr *hdr;
  TGState *tg, *oldtls;
  TGState extra;
  MSize oldmask, wantmask;
  uint64_t retire_epoch;
  uint64_t smr_runs0, smr_reclaimed0;
  GCstr *s1, *s2;
  ActiveReleaseCtx ctx;
  ReclaimReaderCtx reclaim_ctx;
  LJThr release_thr;
  LJThr reclaim_thr;
  int i;

  assert(L != NULL);
  g = G(L);
  tg = L2TG(L);
  assert(tg != NULL);

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  exercise_resize_oom_does_not_claim();
#endif

  hdr = lj_str_tabh_acq(g);
  assert(hdr != NULL);
  assert_resize_idle(hdr);
  assert(lj_tg_strtab_active_hdr_acq(tg) == NULL);
  assert(lj_tg_strtab_active_depth_acq(tg) == 0);

  s1 = lj_str_new(L, "m5-strtab-cas-same", strlen("m5-strtab-cas-same"));
  s2 = lj_str_new(L, "m5-strtab-cas-same", strlen("m5-strtab-cas-same"));
  assert(s1 == s2);
  exercise_terminal_pending_ownership(L, s1);
  exercise_string_id_blocks(L);
  exercise_string_count_blocks(L);
  exercise_tagged_link_protocol(L);
  exercise_resize_ignores_legacy_colors(L);
  exercise_canonical_layout(L);
  /* Allocation volume may legitimately leave an asynchronous IDLE request.
  ** Root the identity used by later assertions and service that request before
  ** beginning the isolated retired-header grace-period model. */
  lua_pushlstring(L, strdata(s1), s1->len);
  assert(strV(L->top - 1) == s1);
  for (i = 0; i < 64 &&
       (gc2_phase_acq(g) != LJ_GC2_IDLE ||
	gc2_cycle_leader_acq(g) != 0); i++)
    (void)lj_gc2_step_explicit(L, 1u << 20);
  assert(i < 64 && gc2_phase_acq(g) == LJ_GC2_IDLE &&
	 gc2_cycle_leader_acq(g) == 0 &&
	 gc2_jit_phase_gate_acq(g) != 0);
  (void)lua_gc(L, LUA_GCSTOP, 0);
  assert(lj_gc_threshold_load(g) == LJ_MAX_MEM);
  retire_epoch = gc2_hs_epoch_acq(g);
  (void)lj_gc2_reclaim_retired(g, retire_epoch + 1u);
  assert(lj_str_retired_head_acq(g) == NULL);
  hdr = lj_str_tabh_acq(g);
  assert(hdr != NULL);
  assert(lj_str_resize_state_acq(hdr) == 0);

  oldmask = lj_str_mask_acq(g);
  wantmask = (oldmask << 2) + 3u;

  /* A paused interner must not keep the resize claimant waiting.
  ** The original header remains current until retry. */
  lj_tg_strtab_active_hdr_rel(tg, hdr);
  lj_tg_strtab_active_depth_rel(tg, 1);
  lj_str_resize(L, wantmask);
  assert(lj_str_tabh_acq(g) == hdr);
  assert(lj_str_mask_acq(g) == oldmask);
  assert_resize_idle(hdr);
  assert(lj_str_retired_head_acq(g) == NULL);
  assert(lj_tg_strtab_active_hdr_acq(tg) == hdr);
  assert(lj_tg_strtab_active_depth_acq(tg) == 1u);
  lj_tg_strtab_active_depth_rel(tg, 0);
  lj_tg_strtab_active_hdr_rel(tg, NULL);
  assert(lj_str_new(L, strdata(s1), s1->len) == s1);
  lj_str_resize(L, wantmask);
  assert(lj_str_tabh_acq(g) != hdr);
  assert(lj_str_mask_acq(g) == wantmask);
  assert_resize_idle(lj_str_tabh_acq(g));
  assert_resize_claimed(hdr);
  assert(lj_str_retired_head_acq(g) == hdr);
  assert(lj_str_retired_next_acq(hdr) == NULL);
  retire_epoch = gc2_hs_epoch_acq(g);
  assert(lj_str_retire_epoch_acq(hdr) == retire_epoch);
  smr_runs0 = gc2_smr_reclaim_runs_acq(g);
  smr_reclaimed0 = gc2_smr_reclaimed_acq(g);
  assert(lj_gc2_reclaim_retired(g, retire_epoch) == 0);
  assert(gc2_smr_reclaim_runs_acq(g) == smr_runs0);
  assert(gc2_smr_reclaimed_acq(g) == smr_reclaimed0);
  assert(lj_str_retired_head_acq(g) == hdr);
  gc2_phase_rel(g, LJ_GC2_MARK);
  assert(lj_gc2_reclaim_retired(g, retire_epoch + 1u) == 0);
  gc2_phase_rel(g, LJ_GC2_IDLE);
  g->gc.state = GCSpropagate;
  assert(lj_gc2_reclaim_retired(g, retire_epoch + 1u) == 0);
  g->gc.state = GCSpause;
  gc2_worker_active_rel(g, 1);
  assert(lj_gc2_reclaim_retired(g, retire_epoch + 1u) == 0);
  gc2_worker_active_rel(g, 0);
  assert(gc2_smr_reclaim_runs_acq(g) == smr_runs0);
  assert(gc2_smr_reclaimed_acq(g) == smr_reclaimed0);
  assert(lj_str_retired_head_acq(g) == hdr);
  reclaim_ctx.g = g;
  reclaim_ctx.epoch = retire_epoch + 1u;
  reclaim_ctx.done = 0;
  reclaim_ctx.reclaimed = 0;
  lj_gc2_smr_read_enter(g);
  assert(lj_thr_create(&reclaim_thr, reclaim_retired_worker,
		       &reclaim_ctx) == 0);
  for (i = 0; i < 1000 && la_load32_acq(&reclaim_ctx.done) == 0; i++)
    (void)lj_thr_sleep_ns(NULL, 100000);
  assert(la_load32_acq(&reclaim_ctx.done) != 0);
  assert(reclaim_ctx.reclaimed == 0);
  assert(lj_str_retired_head_acq(g) == hdr);
  lj_gc2_smr_read_leave(g);
  assert(lj_thr_join(&reclaim_thr, NULL) == 0);
  assert(gc2_smr_reclaiming_acq(g) == 0);
  assert(gc2_cycle_leader_acq(g) == 0 &&
	 gc2_jit_phase_gate_acq(g) != 0);
  assert(lj_gc2_reclaim_retired(g, retire_epoch + 1u) >= 1u);
  assert(lj_str_retired_head_acq(g) == NULL);
  (void)lua_gc(L, LUA_GCRESTART, 0);
  assert(lj_str_new(L, "m5-strtab-cas-same",
		    strlen("m5-strtab-cas-same")) == s1);

  hdr = lj_str_tabh_acq(g);
  lj_tg_strtab_active_hdr_rel(tg, hdr);
  lj_tg_strtab_active_depth_rel(tg, 1);
  ctx.tg = tg;
  ctx.hdr = hdr;
  ctx.claimed = 0;
  ctx.cleared = 0;
  assert(lj_thr_create(&release_thr, release_active_after_claim, &ctx) == 0);
  assert(lj_str_sweep_claim(L, hdr) == 1);
  assert(lj_thr_join(&release_thr, NULL) == 0);
  assert(la_load32_acq(&ctx.claimed) != 0);
  assert(la_load32_acq(&ctx.cleared) != 0);
  assert(lj_tg_strtab_active_hdr_acq(tg) == NULL);
  assert(lj_tg_strtab_active_depth_acq(tg) == 0);
  assert_sweep_claimed(hdr);
  lj_str_sweep_release(hdr);
  assert_resize_idle(hdr);

  hdr = lj_str_tabh_acq(g);
  oldmask = lj_str_mask_acq(g);
  wantmask = (oldmask << 2) + 3u;
  lj_tg_init_thread(g, &extra, NULL, 0);
  lj_tg_tid_rel(&extra, lj_thr_newid());
  oldtls = lj_thr_get_tg();
  lj_thr_set_tg(&extra);
  lj_tg_strtab_active_hdr_rel(&extra, hdr);
  lj_tg_strtab_active_depth_rel(&extra, 1);
  lj_str_resize(L, wantmask);
  assert(lj_str_tabh_acq(g) == hdr);
  assert(lj_str_mask_acq(g) == oldmask);
  assert_resize_idle(hdr);
  assert(lj_tg_strtab_active_hdr_acq(&extra) == hdr);
  assert(lj_tg_strtab_active_depth_acq(&extra) == 1u);
  /* The actual mutator can still make progress while the other pin exists. */
  assert(lj_str_new(L, strdata(s1), s1->len) == s1);
  lj_tg_strtab_active_depth_rel(&extra, 0);
  lj_tg_strtab_active_hdr_rel(&extra, NULL);
  lj_thr_set_tg(oldtls);
  lj_tg_fini_thread(g, &extra);
  lj_str_resize(L, wantmask);
  assert(lj_str_tabh_acq(g) != hdr);
  assert(lj_str_mask_acq(g) == wantmask);
  assert_resize_idle(lj_str_tabh_acq(g));
  assert_resize_claimed(hdr);
  assert(lj_str_retired_head_acq(g) == hdr);
  retire_epoch = gc2_hs_epoch_acq(g);
  assert(lj_gc2_reclaim_retired(g, retire_epoch + 1u) >= 1u);
  assert(lj_str_retired_head_acq(g) == NULL);

  for (i = 0; i < 8192; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-cas-%d-%d", i, i * 31);
    assert(lj_str_new(L, buf, strlen(buf)) != NULL);
  }
  assert_resize_idle(lj_str_tabh_acq(g));
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  assert(gc2_hs_epoch_acq(g) > retire_epoch);
  assert(lj_str_retired_head_acq(g) == NULL);
  assert(gc2_smr_reclaim_runs_acq(g) > smr_runs0);
  assert(gc2_smr_reclaimed_acq(g) >= smr_reclaimed0 + 1u);

  lua_close(L);
  L = luaL_newstate();
  assert(L != NULL);
  (void)lua_gc(L, LUA_GCSTOP, 0);
  exercise_canonical_quarantine(L);
  lua_close(L);
  exercise_paused_topology_hits();
  exercise_paused_allocation_pressure();
  exercise_stale_growth(1);
  exercise_stale_growth(2);
#if LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  printf("t-strtab-cas OK: tagged-link rescue/prepend races, color-independent string directory replacement, canonical layout, Q-publish/resize and unlink/qcount identity races, saturating Q presence, nonwaiting resize claims with registered and TLS-only pins, GC2 epoch retire, duplicate intern guard, and string ID/count block reservation verified (custom-allocator resize OOM injection skipped by the temporary internal-allocator-only policy)\n");
#else
  printf("t-strtab-cas OK: tagged-link rescue/prepend races, color-independent string directory replacement, canonical layout, Q-publish/resize and unlink/qcount identity races, saturating Q presence, resize OOM, nonwaiting resize claims with registered and TLS-only pins, GC2 epoch retire, duplicate intern guard, and string ID/count block reservation verified\n");
#endif
  return 0;
}
