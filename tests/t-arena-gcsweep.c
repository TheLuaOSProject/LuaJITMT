/*
** Focused test for the runtime traversable arena sweep bridge.
*/

#ifndef LJ_GC2_TEST_HELPERS
#define LJ_GC2_TEST_HELPERS
#endif

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_tg.h"

static uint32_t ptr_state(void *p)
{
  GCArena *a = lj_arena_of(p);
  uint32_t cell = lj_arena_cellof(p);
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  return lj_arena_state(a, cell);
}

static uint32_t sweep_nonwhite32_scalar(uint64_t sweep)
{
  uint32_t bits = 0;
  uint32_t i;
  for (i = 0; i < 32u; i++)
    if (((sweep >> (i << 1)) & 3u) != LJ_ARENA_SWEEP_WHITE)
      bits |= (uint32_t)1u << i;
  return bits;
}

static int sweep_dtor_supported_scalar(uint32_t kind)
{
  return kind == LJ_ARENA_DTOR_LFUNC1 ||
	 kind == LJ_ARENA_DTOR_CLOSED_UV ||
	 kind == LJ_ARENA_DTOR_LFUNC0;
}

static void sweep_partition64_scalar(uint64_t block, uint64_t mark,
	uint64_t sweep0, uint64_t sweep1, uint64_t p0, uint64_t p1,
	uint64_t p2, uint64_t p3, uint64_t valid, uint64_t *pinp,
	uint64_t *candidatep)
{
  uint64_t pin = 0, candidates = 0;
  uint32_t i;
  for (i = 0; i < 64u; i++) {
    uint64_t bit = (uint64_t)1 << i;
    uint64_t sweep = i < 32u ? sweep0 : sweep1;
    uint32_t lane = i & 31u;
    uint32_t state = (uint32_t)((sweep >> (lane << 1)) & 3u);
    uint32_t kind = (p0 & bit ? 1u : 0u) |
	(p1 & bit ? 2u : 0u) | (p2 & bit ? 4u : 0u) |
	(p3 & bit ? 8u : 0u);
    if (!(valid & bit) || !(block & bit) || (mark & bit) ||
	state != LJ_ARENA_SWEEP_WHITE)
      continue;
    if (sweep_dtor_supported_scalar(kind))
      candidates |= bit;
    else
      pin |= bit;
  }
  *pinp = pin;
  *candidatep = candidates;
}

static uint64_t sweep_classifier_rand(uint64_t *state)
{
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

static uint64_t selected_cas_hook_mask;
static uint64_t selected_cas_hook_bits;

static void selected_cas_mutate_hook(uint64_t *word)
{
  uint64_t old = la_load64_acq(word);
  la_store64_rel(word,
	(old & ~selected_cas_hook_mask) | selected_cas_hook_bits);
}

static void test_selected_lane_cas(void)
{
  const uint64_t lifetime_lsb = UINT64_C(0x1111111111111111);
  const uint64_t sweep_lsb = UINT64_C(0x5555555555555555);
  uint32_t lane;

  for (lane = 0; lane < 16u; lane++) {
    uint32_t shift = lane << 2;
    uint64_t lsb = (uint64_t)1 << shift;
    uint64_t mask = UINT64_C(0xf) << shift;
    uint64_t word = (UINT64_C(0x2222222222222222) & ~mask) | lsb;
    uint64_t expected = (word & ~mask) |
	((uint64_t)LJ_ARENA_LIFETIME_DESTRUCT << shift);
    assert(lj_gc_test_lifetime_selected_cas(&word, lsb,
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT));
    assert(word == expected);
  }
  for (lane = 0; lane < 32u; lane++) {
    uint32_t shift = lane << 1;
    uint64_t lsb = (uint64_t)1 << shift;
    uint64_t mask = UINT64_C(3) << shift;
    uint64_t word = UINT64_C(0x5555555555555555) & ~mask;
    uint64_t expected = word | mask;
    assert(lj_gc_test_sweep_selected_cas(&word, lsb,
	LJ_ARENA_SWEEP_WHITE, LJ_ARENA_SWEEP_FREEING));
    assert(word == expected);
  }

  {
    uint64_t word = lifetime_lsb;
    assert(lj_gc_test_lifetime_selected_cas(&word, lifetime_lsb,
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT));
    assert(word == UINT64_C(0x4444444444444444));
  }
  {
    uint64_t word = 0;
    assert(lj_gc_test_sweep_selected_cas(&word, sweep_lsb,
	LJ_ARENA_SWEEP_WHITE, LJ_ARENA_SWEEP_FREEING));
    assert(word == ~(uint64_t)0);
  }

  /* One selected disagreement changes no selected lane. */
  {
    uint64_t lanes = UINT64_C(1) | (UINT64_C(1) << 60);
    uint64_t word = UINT64_C(1) |
	((uint64_t)LJ_ARENA_LIFETIME_RESCUE << 60);
    uint64_t before = word;
    assert(!lj_gc_test_lifetime_selected_cas(&word, lanes,
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT));
    assert(word == before);
  }

  /* A selected mask may contain only the low bit of each packed lane. */
  {
    uint64_t lifetime = LJ_ARENA_LIFETIME_LIVE;
    uint64_t sweep = 0;
    assert(!lj_gc_test_lifetime_selected_cas(&lifetime, UINT64_C(2),
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT));
    assert(!lj_gc_test_sweep_selected_cas(&sweep, UINT64_C(2),
	LJ_ARENA_SWEEP_WHITE, LJ_ARENA_SWEEP_FREEING));
    assert(lifetime == LJ_ARENA_LIFETIME_LIVE && sweep == 0);
  }

  /* Deterministically alter an unrelated lane after the helper's load. Its
  ** losing CAS must rebuild around that lane and preserve it exactly. */
  {
    uint64_t word = LJ_ARENA_LIFETIME_LIVE;
    selected_cas_hook_mask = UINT64_C(0xf) << 60;
    selected_cas_hook_bits = (uint64_t)LJ_ARENA_LIFETIME_MUTATING << 60;
    lj_gc_test_set_selected_cas_hook(selected_cas_mutate_hook);
    assert(lj_gc_test_lifetime_selected_cas(&word, UINT64_C(1),
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT));
    lj_gc_test_set_selected_cas_hook(NULL);
    assert(word == ((uint64_t)LJ_ARENA_LIFETIME_DESTRUCT |
	((uint64_t)LJ_ARENA_LIFETIME_MUTATING << 60)));
  }
  {
    uint64_t word = 0;
    selected_cas_hook_mask = UINT64_C(3);
    selected_cas_hook_bits = LJ_ARENA_SWEEP_LIVE;
    lj_gc_test_set_selected_cas_hook(selected_cas_mutate_hook);
    assert(lj_gc_test_sweep_selected_cas(&word, UINT64_C(1) << 62,
	LJ_ARENA_SWEEP_WHITE, LJ_ARENA_SWEEP_FREEING));
    lj_gc_test_set_selected_cas_hook(NULL);
    assert(word == ((UINT64_C(3) << 62) | LJ_ARENA_SWEEP_LIVE));
  }

  /* Altering a selected lane after the load makes the retry reject the whole
  ** selected set without stealing the new RESCUE state. */
  {
    uint64_t word = LJ_ARENA_LIFETIME_LIVE;
    selected_cas_hook_mask = UINT64_C(0xf);
    selected_cas_hook_bits = LJ_ARENA_LIFETIME_RESCUE;
    lj_gc_test_set_selected_cas_hook(selected_cas_mutate_hook);
    assert(!lj_gc_test_lifetime_selected_cas(&word, UINT64_C(1),
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT));
    lj_gc_test_set_selected_cas_hook(NULL);
    assert(word == LJ_ARENA_LIFETIME_RESCUE);
  }
}

static void test_packed_unmarked_classifier(void)
{
  static const uint32_t boundaries[] = { 0u, 31u, 32u, 63u };
  const uint64_t all = ~(uint64_t)0;
  const uint32_t first_lane = LJ_AFIRST_CELL & 63u;
  const uint64_t first_valid = all << first_lane;
  uint64_t rng = UINT64_C(0x9e3779b97f4a7c15);
  uint32_t group, pattern, lane, state, kind, i;

  /* Every state in every packed lane, plus every possible eight-lane word
  ** fragment at all four offsets, must match the scalar expansion. */
  for (lane = 0; lane < 32u; lane++) {
    for (state = 0; state < 4u; state++) {
      uint64_t sweep = (uint64_t)state << (lane << 1);
      uint32_t expected = state == LJ_ARENA_SWEEP_WHITE ?
	0 : (uint32_t)1u << lane;
      assert(lj_gc_test_sweep_nonwhite32(sweep) == expected);
    }
  }
  for (group = 0; group < 4u; group++) {
    for (pattern = 0; pattern <= UINT16_MAX; pattern++) {
      uint64_t sweep = (uint64_t)pattern << (group << 4);
      assert(lj_gc_test_sweep_nonwhite32(sweep) ==
	     sweep_nonwhite32_scalar(sweep));
    }
  }

  /* All sixteen destructor-plane combinations are exact: only codes 1, 2
  ** and 4 are supported, including at the 31/32 bitmap split and bit 63. */
  for (kind = 0; kind <= LJ_ARENA_DTOR_MAX; kind++) {
    uint64_t p0 = kind & 1u ? all : 0;
    uint64_t p1 = kind & 2u ? all : 0;
    uint64_t p2 = kind & 4u ? all : 0;
    uint64_t p3 = kind & 8u ? all : 0;
    uint64_t expected = sweep_dtor_supported_scalar(kind) ? all : 0;
    assert(lj_gc_test_sweep_supported_dtor64(p0, p1, p2, p3) ==
	   expected);
    for (i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
      uint64_t bit = (uint64_t)1 << boundaries[i];
      p0 = kind & 1u ? bit : 0;
      p1 = kind & 2u ? bit : 0;
      p2 = kind & 4u ? bit : 0;
      p3 = kind & 8u ? bit : 0;
      expected = sweep_dtor_supported_scalar(kind) ? bit : 0;
      assert(lj_gc_test_sweep_supported_dtor64(p0, p1, p2, p3) ==
	     expected);
    }
  }

  {
    uint64_t block = (UINT64_C(1) << 31) | (UINT64_C(1) << 32) |
	(UINT64_C(1) << 63);
    uint64_t sweep0 = (uint64_t)LJ_ARENA_SWEEP_LIVE << 62;
    uint64_t pin, candidates;
    lj_gc_test_sweep_partition64(block, 0, sweep0, 0, block, 0, 0, 0,
	all, &pin, &candidates);
    assert(pin == 0);
    assert(candidates == ((UINT64_C(1) << 32) | (UINT64_C(1) << 63)));
  }

  /* The first scanned bitmap word begins at arena cell 576, while usable
  ** storage begins at cell 616 (lane 40). No lower structural bit may leak. */
  assert(first_lane != 0);
  {
    uint64_t below = (uint64_t)1 << (first_lane - 1u);
    uint64_t first = (uint64_t)1 << first_lane;
    uint64_t last = UINT64_C(1) << 63;
    uint64_t block = below | first | last;
    uint64_t pin, candidates;
    lj_gc_test_sweep_partition64(block, 0, 0, 0, block, 0, 0, 0,
	first_valid, &pin, &candidates);
    assert(pin == 0);
    assert(candidates == (first | last));
  }

  /* Random packed snapshots must partition identically to a scalar lane
  ** oracle. Applying the pin mask may set bits but never clear old marks. */
  for (i = 0; i < 20000u; i++) {
    uint64_t block = sweep_classifier_rand(&rng);
    uint64_t mark = sweep_classifier_rand(&rng);
    uint64_t sweep0 = sweep_classifier_rand(&rng);
    uint64_t sweep1 = sweep_classifier_rand(&rng);
    uint64_t p0 = sweep_classifier_rand(&rng);
    uint64_t p1 = sweep_classifier_rand(&rng);
    uint64_t p2 = sweep_classifier_rand(&rng);
    uint64_t p3 = sweep_classifier_rand(&rng);
    uint64_t valid = i % 3u == 0 ? all :
	(i % 3u == 1 ? first_valid : sweep_classifier_rand(&rng));
    uint64_t pin, candidates, scalar_pin, scalar_candidates;
    uint64_t pinned = mark;
    uint64_t old;
    lj_gc_test_sweep_partition64(block, mark, sweep0, sweep1,
	p0, p1, p2, p3, valid, &pin, &candidates);
    sweep_partition64_scalar(block, mark, sweep0, sweep1,
	p0, p1, p2, p3, valid, &scalar_pin, &scalar_candidates);
    assert(pin == scalar_pin);
    assert(candidates == scalar_candidates);
    assert((pin & candidates) == 0);
    old = lj_gc_test_sweep_bulk_pin64(&pinned, pin);
    assert(old == mark);
    assert(pinned == (mark | pin));
  }
}

static void test_packed_unmarked_outer_scan(global_State *g, TGState *tg)
{
  GCArena *a = lj_arena_map(&tg->prng, LJ_AF_TRAVERSABLE);
  uint64_t marks0 = la_load64_acq(&g->gc2.marks_this_round);
  const uint32_t first = LJ_AFIRST_CELL;
  const uint32_t below = first - 1u;
  const uint32_t word = (first + 63u) & ~63u;
  const uint32_t first_last = word - 1u;
  const uint32_t cell31 = word + 31u;
  const uint32_t cell32 = word + 32u;
  const uint32_t cell63 = word + 63u;
  const uint32_t raw = word + 4u;
  const uint32_t malformed = word + 5u;
  const uint32_t premarked = word + 6u;
  const uint32_t cells[] = {
    below, first, first_last, cell31, cell32, cell63,
    raw, malformed, premarked
  };
  uint32_t i;

  assert(a != NULL);
  assert((first >> 6) + 1u == (word >> 6));
  assert(cell63 < LJ_ARENA_CELLS);
  for (i = 0; i < sizeof(cells) / sizeof(cells[0]); i++)
    lj_arena_bm_set(a->block, cells[i]);
  lj_arena_bm_set(a->dtor[0], below);
  lj_arena_bm_set(a->dtor[0], first);
  lj_arena_bm_set(a->dtor[0], first_last);
  lj_arena_bm_set(a->dtor[0], cell31);
  lj_arena_bm_set(a->dtor[0], cell32);
  lj_arena_bm_set(a->dtor[0], cell63);
  lj_arena_bm_set(a->dtor[0], malformed);
  lj_arena_bm_set(a->dtor[1], malformed);
  lj_arena_bm_set(a->mark, premarked);
  assert(lj_arena_sweep_state_cas(a, cell31,
	LJ_ARENA_SWEEP_WHITE, LJ_ARENA_SWEEP_LIVE));

  /* Supported starts with an intentionally incomplete exact descriptor enter
  ** the scalar conservative fallback. Untyped/malformed starts take the bulk
  ** OR, while a non-WHITE lane and the pre-arena prefix remain untouched. */
  assert(lj_gc_sweep_gc2_arena_unmarked(g, a) == 0);
  assert(la_load64_acq(&g->gc2.marks_this_round) == marks0);
  assert(!lj_arena_bm_get(a->mark, below));
  assert(lj_arena_bm_get(a->mark, first));
  assert(lj_arena_bm_get(a->mark, first_last));
  assert(!lj_arena_bm_get(a->mark, cell31));
  assert(lj_arena_bm_get(a->mark, cell32));
  assert(lj_arena_bm_get(a->mark, cell63));
  assert(lj_arena_bm_get(a->mark, raw));
  assert(lj_arena_bm_get(a->mark, malformed));
  assert(lj_arena_bm_get(a->mark, premarked));

  assert(lj_arena_sweep_state_cas(a, cell31,
	LJ_ARENA_SWEEP_LIVE, LJ_ARENA_SWEEP_WHITE));
  for (i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
    lj_arena_bm_clear(a->block, cells[i]);
    lj_arena_bm_clear(a->mark, cells[i]);
    lj_arena_bm_clear(a->dtor[0], cells[i]);
    lj_arena_bm_clear(a->dtor[1], cells[i]);
  }
  lj_arena_unmap(a);
}

static int arena_list_contains(GCArena *a, GCArena *needle)
{
  while (a) {
    if (a == needle)
      return 1;
    a = lj_arena_next_acq(a);
  }
  return 0;
}

static int gc_root_list_contains(global_State *g, GCobj *needle)
{
  GCobj *o;
  uint32_t seen = 0;
  for (o = lj_gc_root_acq(g);
       o != NULL && seen++ < LJ_GC2_ROOT_SCAN_LIMIT;) {
    GCobj *next;
    if (o == needle)
      return 1;
    next = lj_obj_gcw_acq(o);
    if (next == o)
      break;
    o = next;
  }
  return 0;
}

/* Synthetic sweep fixtures bypass root/weak work intentionally. Keep their
** veto-only activation mirror coherent without pretending to close its gate. */
static LJGC2ActivationSnap test_publish_sweep_phase(global_State *g)
{
  LJGC2ActivationSnap idle, mark, weak, sweep;
  uint64_t epoch;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  idle = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(idle.state == LJ_GC2_ACT_IDLE);
  assert(idle.gate == LJ_GC2_ROOT_GATE_OPEN);
  epoch = idle.mark_epoch == UINT64_MAX ? UINT64_MAX : idle.mark_epoch + 1u;
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &idle, epoch,
           LJ_GC2_ACT_MARK, &mark) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &mark, epoch,
           LJ_GC2_ACT_WEAK, &weak) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&g->gc2.activation, &weak, epoch,
           LJ_GC2_ACT_SWEEP_OPEN, &sweep) == LJ_GC2_TRANSITION_OK);
  gc2_phase_rel(g, LJ_GC2_SWEEP);
  /* These fixtures intentionally bypass the semantic root driver. Publish
  ** its prerequisite explicitly before any synthetic READY edge. */
  gc2_sweep_root_scanned_rel(g, 1);
  /* Synthetic fixtures intentionally skip string reclamation. Mirror the real
  ** WEAK->SWEEP initialization with a completed non-major string cycle so the
  ** paranoia close oracle sees a valid DONE state. */
  lj_str_gc2_sweep_begin(g, 0);
  assert(!lj_gc2_activation_reclaim_veto(g));
  return sweep;
}

static void arena_needsweep_move_head(TGAlloc *alloc, uint32_t kind,
				      GCArena *target)
{
  GCArena *head, *prev = NULL, *a;
  assert(alloc != NULL && kind < LJ_ARENA_NKINDS && target != NULL);
  head = alloc->needsweep[kind];
  if (head == target)
    return;
  for (a = head; a != NULL && a != target;) {
    GCArena *next = lj_arena_next_acq(a);
    assert(next != a);
    prev = a;
    a = next;
  }
  assert(a == target && prev != NULL);
  lj_arena_next_rel(prev, lj_arena_next_acq(target));
  lj_arena_next_rel(target, head);
  alloc->needsweep[kind] = target;
}

/* These stopped-owner fixtures temporarily inject/detach NEEDSWEEP lists.
** Keep their scalar publication explicit when bypassing production helpers. */
static void arena_fixture_needsweep(TGAlloc *alloc, GCArena *head)
{
  GCArena *a;
  uint32_t n = 0;
  for (a = head; a; a = lj_arena_next_acq(a))
    assert(++n < LJ_GC2_ROOT_SCAN_LIMIT);
  alloc->needsweep[LJ_ARENAK_TRAVERSABLE] = head;
  la_store32_rel(&alloc->needsweep_count[LJ_ARENAK_TRAVERSABLE], n);
}

static int noop_finalizer(lua_State *L)
{
  (void)L;
  return 0;
}

typedef struct TypedDtorFixture {
  lua_State *L;
  global_State *g;
  TGState *tg;
  GCfunc *f0;
  GCfunc *f1;
  GCupval *uv;
  GCArena *a;
  uint32_t f0cell;
  uint32_t f1cell;
  uint32_t uvcell;
  GCSize f0size;
  GCSize f1size;
  GCSize uvsize;
} TypedDtorFixture;

static void typed_dtor_fixture_init(TypedDtorFixture *fx)
{
  lua_State *L;
  uint32_t i;

  memset(fx, 0, sizeof(*fx));
  L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "typed_dtor_fixture = {}\n"
    "typed_dtor_fixture.make0 = function()\n"
    "  return function() return 17 end\n"
    "end\n"
    "typed_dtor_fixture.make1 = function(v)\n"
    "  local x = v\n"
    "  return function() x = x + 1; return x end\n"
    "end\n"
    "collectgarbage('collect')\n"
    "collectgarbage('stop')\n") == LUA_OK);
  lua_getglobal(L, "typed_dtor_fixture");
  assert(tvistab(L->top - 1));

  /* The batch tests require an LFUNC0 in one lifetime word and the atomic
  ** LFUNC1/CLOSED_UV pair together in another. Same-arena placement alone is
  ** insufficient: valid allocator packing can put all three in one word or
  ** split the pair. Select the intended geometry instead of depending on the
  ** first free-run choice, retrying across arena boundaries as needed. */
  for (i = 0; i < 64u; i++) {
    lua_getfield(L, -1, "make0");
    assert(tvisfunc(L->top - 1));
    lua_call(L, 0, 1);
    assert(tvisfunc(L->top - 1));
    fx->f0 = funcV(L->top - 1);
    lua_setfield(L, -2, "f0");

    lua_getfield(L, -1, "make1");
    assert(tvisfunc(L->top - 1));
    lua_pushnumber(L, (lua_Number)(i + 1u));
    lua_call(L, 1, 1);
    assert(tvisfunc(L->top - 1));
    fx->f1 = funcV(L->top - 1);
    lua_setfield(L, -2, "f1");

    assert(isluafunc(fx->f0));
    assert(isluafunc(fx->f1));
    assert(lj_funcL_nupvalues(&fx->f0->l) == 0);
    assert(lj_funcL_nupvalues(&fx->f1->l) == 1);
    fx->uv = func_uv_acq(&fx->f1->l, 0);
    assert(fx->uv != NULL && fx->uv->closed);
    assert(uvval(fx->uv) == &fx->uv->tv);
    fx->a = lj_arena_of(fx->f1);
    if (lj_arena_of(fx->f0) == fx->a &&
	lj_arena_of(fx->uv) == fx->a &&
	(lj_arena_cellof(fx->f0) >> 4) !=
	(lj_arena_cellof(fx->f1) >> 4) &&
	(lj_arena_cellof(fx->f1) >> 4) ==
	(lj_arena_cellof(fx->uv) >> 4))
      break;
  }
  assert(i < 64u);

  fx->L = L;
  fx->g = G(L);
  fx->tg = G2TG(fx->g);
  assert(fx->tg != NULL && fx->tg == fx->g->main_tg);
  fx->f0cell = lj_arena_cellof(fx->f0);
  fx->f1cell = lj_arena_cellof(fx->f1);
  fx->uvcell = lj_arena_cellof(fx->uv);
  fx->f0size = (GCSize)sizeLfunc(0);
  fx->f1size = (GCSize)sizeLfunc(1);
  fx->uvsize = (GCSize)sizeof(GCupval);
  assert(fx->uvcell == fx->f1cell + lj_arena_ncells(fx->f1size));
  assert(lj_arena_root_state_acq(fx->a, fx->f0cell) ==
	 LJ_ARENA_ROOT_NONE);
  assert(lj_arena_root_state_acq(fx->a, fx->f1cell) ==
	 LJ_ARENA_ROOT_NONE);
  assert(lj_arena_root_state_acq(fx->a, fx->uvcell) ==
	 LJ_ARENA_ROOT_NONE);
  assert(lj_arena_dtor_kind_acq(fx->a, fx->f0cell) ==
	 LJ_ARENA_DTOR_LFUNC0);
  assert(lj_arena_dtor_kind_acq(fx->a, fx->f1cell) ==
	 LJ_ARENA_DTOR_LFUNC1);
  assert(lj_arena_dtor_kind_acq(fx->a, fx->uvcell) ==
	 LJ_ARENA_DTOR_CLOSED_UV);
}

static void typed_dtor_drop_fixture_roots(TypedDtorFixture *fx)
{
  lua_State *L = fx->L;
  assert(tvistab(L->top - 1));
  lua_pushnil(L);
  lua_setfield(L, -2, "f0");
  lua_pushnil(L);
  lua_setfield(L, -2, "f1");
}

static void typed_dtor_select_dead(TypedDtorFixture *fx, uint32_t deadmask)
{
  uint32_t w;
  GCArena *a = fx->a;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t starts = la_load64_acq(&a->block[w]);
    (void)la_or64_rlx(&a->mark[w], starts);
  }
  if (deadmask & LJ_ARENA_DTOR_LFUNC0)
    lj_arena_bm_clear(a->mark, fx->f0cell);
  if (deadmask & LJ_ARENA_DTOR_LFUNC1)
    lj_arena_bm_clear(a->mark, fx->f1cell);
  if (deadmask & LJ_ARENA_DTOR_CLOSED_UV)
    lj_arena_bm_clear(a->mark, fx->uvcell);
}

static void typed_dtor_publish_ready_sweep(TypedDtorFixture *fx)
{
  global_State *g = fx->g;
  uint64_t hs_epoch;

  g->gc2.cycle++;
  (void)test_publish_sweep_phase(g);
  fx->tg->alloc.prepare_epoch = g->gc2.cycle;
  gc2_sweep_bridge_ready_store_rlx(g, 0);
  gc2_sweep_root_done_rel(g, 1);
  assert(lj_gc2_handshake(g,
	LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_FLUSH_SSB) == 1);
  while (lj_gc2_test_ssb_drain(g) != 0)
    ;
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_thread_scan_needscan_pending_acq(g) == 0);
  (void)gc2_marks_this_round_xchg_acqrel(g, 0);
  hs_epoch = gc2_hs_epoch_acq(g);
  assert(hs_epoch != 0);
  lj_gc2_sweep_bridge_ready(g);
  assert(gc2_sweep_bridge_ready_acq(g));
  assert(gc2_jit_phase_gate_acq(g) != 0);
}

static GCArena *typed_dtor_prepare_target(TypedDtorFixture *fx,
					  uint32_t deadmask)
{
  TGAlloc *alloc = &fx->tg->alloc;
  GCArena *other;
  assert(lj_arena_alloc_prepare_sweep_kind(
	alloc, LJ_ARENAK_TRAVERSABLE));
  assert(arena_list_contains(
	alloc->needsweep[LJ_ARENAK_TRAVERSABLE], fx->a));
  arena_needsweep_move_head(alloc, LJ_ARENAK_TRAVERSABLE, fx->a);
  typed_dtor_select_dead(fx, deadmask);
  typed_dtor_publish_ready_sweep(fx);
  if (deadmask & LJ_ARENA_DTOR_LFUNC0)
    assert(!lj_arena_bm_get(fx->a->mark, fx->f0cell));
  if (deadmask & LJ_ARENA_DTOR_LFUNC1)
    assert(!lj_arena_bm_get(fx->a->mark, fx->f1cell));
  if (deadmask & LJ_ARENA_DTOR_CLOSED_UV)
    assert(!lj_arena_bm_get(fx->a->mark, fx->uvcell));

  assert(lj_gc2_test_sweep_owner_progress(fx->g, fx->tg, 1u) == 1u);
  assert(fx->tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] == fx->a);
  other = alloc->needsweep[LJ_ARENAK_TRAVERSABLE];
  arena_fixture_needsweep(alloc, NULL);
  return other;
}

static GCSize typed_dtor_finish_target(TypedDtorFixture *fx, GCArena *other)
{
  TGAlloc *alloc = &fx->tg->alloc;
  GCSize total_after_target;
  uint32_t i;

  for (i = 0; i < 256u &&
	 !arena_list_contains(lj_arena_alloc_reclaimed_head(
	   alloc, LJ_ARENAK_TRAVERSABLE), fx->a); i++)
    assert(lj_gc2_test_sweep_owner_progress(
	fx->g, fx->tg, 1u) != 0);
  assert(i < 256u);
  /* Target semantic/post-grace work is complete at reclaimed publication.
  ** Snapshot accounting before cycle_to_idle also reclaims unrelated retired
  ** table vectors accumulated by fixture setup. */
  total_after_target = lj_gc_total_load(fx->g);
  while (lj_gc2_test_ssb_drain(fx->g) != 0)
    ;
  assert(lj_gc2_test_ssb_empty(fx->g));
  assert(gc2_thread_scan_needscan_pending_acq(fx->g) == 0);
  lj_gc2_cycle_to_idle(fx->g);
  assert(gc2_phase_acq(fx->g) == LJ_GC2_IDLE);

  arena_fixture_needsweep(alloc, other);
  assert(lj_arena_alloc_restore_sweep_kind(
	alloc, LJ_ARENAK_TRAVERSABLE));
  return total_after_target;
}

#define DENSE_TYPED_MAX_ENTRIES 192u

typedef struct DenseTypedEntry {
  GCobj *o;
  GCArena *a;
  GCSize size;
  uint32_t cell;
  uint32_t kind;
} DenseTypedEntry;

typedef struct DenseTypedFixture {
  lua_State *L;
  global_State *g;
  TGState *tg;
  GCArena *a;
  DenseTypedEntry selected[6];
  uint32_t nselected;
} DenseTypedFixture;

static void dense_typed_fixture_init(DenseTypedFixture *fx,
	uint32_t wanted)
{
  DenseTypedEntry entries[DENSE_TYPED_MAX_ENTRIES];
  lua_State *L = luaL_newstate();
  uint32_t nentry = 0;
  uint32_t i, j;
  int base;

  memset(fx, 0, sizeof(*fx));
  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "function dense_typed_make1(v)\n"
    "  local x = v\n"
    "  return function() x = x + 1; return x end\n"
    "end\n"
    "collectgarbage('collect')\n"
    "collectgarbage('stop')\n") == LUA_OK);
  assert(wanted == 5u || wanted == 6u);
  assert(lua_checkstack(L, 128));
  base = lua_gettop(L);
  for (i = 0; i < DENSE_TYPED_MAX_ENTRIES / 2u; i++) {
    GCfunc *fn;
    GCupval *uv;
    lua_getglobal(L, "dense_typed_make1");
    assert(tvisfunc(L->top - 1));
    lua_pushnumber(L, (lua_Number)(i + 1u));
    lua_call(L, 1, 1);
    assert(tvisfunc(L->top - 1));
    fn = funcV(L->top - 1);
    assert(isluafunc(fn) && lj_funcL_nupvalues(&fn->l) == 1u);
    uv = func_uv_acq(&fn->l, 0);
    assert(uv != NULL && uv->closed && uvval(uv) == &uv->tv);

    entries[nentry].o = obj2gco(fn);
    entries[nentry].a = lj_arena_of(fn);
    entries[nentry].cell = lj_arena_cellof(fn);
    entries[nentry].kind = LJ_ARENA_DTOR_LFUNC1;
    entries[nentry].size = (GCSize)sizeLfunc(1);
    nentry++;
    entries[nentry].o = obj2gco(uv);
    entries[nentry].a = lj_arena_of(uv);
    entries[nentry].cell = lj_arena_cellof(uv);
    entries[nentry].kind = LJ_ARENA_DTOR_CLOSED_UV;
    entries[nentry].size = (GCSize)sizeof(GCupval);
    nentry++;
  }
  assert(nentry == DENSE_TYPED_MAX_ENTRIES);

  for (i = 0; i < nentry && fx->nselected == 0; i++) {
    uint32_t count = 0;
    uint32_t kinds = 0;
    for (j = 0; j < nentry; j++) {
      if (entries[j].a == entries[i].a &&
	  (entries[j].cell >> 4) == (entries[i].cell >> 4)) {
	count++;
	kinds |= entries[j].kind;
      }
    }
    if (count >= wanted &&
	(kinds & (LJ_ARENA_DTOR_LFUNC1|LJ_ARENA_DTOR_CLOSED_UV)) ==
	  (LJ_ARENA_DTOR_LFUNC1|LJ_ARENA_DTOR_CLOSED_UV)) {
      for (j = 0; j < nentry && fx->nselected < wanted; j++) {
	if (entries[j].a == entries[i].a &&
	    (entries[j].cell >> 4) == (entries[i].cell >> 4))
	  fx->selected[fx->nselected++] = entries[j];
      }
      fx->a = entries[i].a;
    }
  }
  assert(fx->nselected == wanted && fx->a != NULL);
  for (i = 0; i < fx->nselected; i++) {
    assert(fx->selected[i].a == fx->a);
    assert((fx->selected[i].cell >> 4) ==
	   (fx->selected[0].cell >> 4));
    assert(lj_arena_dtor_kind_acq(fx->a, fx->selected[i].cell) ==
	   fx->selected[i].kind);
    assert(lj_arena_root_state_acq(fx->a, fx->selected[i].cell) ==
	   LJ_ARENA_ROOT_NONE);
  }

  /* Drop every generated closure root only after the exact selected group has
  ** been captured. GC is stopped, so the sidecars/bodies remain stable until
  ** the explicit sweep fixture takes ownership. */
  lua_settop(L, base);
  fx->L = L;
  fx->g = G(L);
  fx->tg = G2TG(fx->g);
  assert(fx->tg != NULL && fx->tg == fx->g->main_tg);
}

static void test_typed_dtor_dense_batch(uint32_t wanted)
{
  DenseTypedFixture fx;
  TypedDtorFixture bridge;
  TGAlloc *alloc;
  GCArena *other;
  GCSize total0, total1, selected_size = 0;
  uint32_t w, i;

  dense_typed_fixture_init(&fx, wanted);
  alloc = &fx.tg->alloc;
  assert(lj_arena_alloc_prepare_sweep_kind(
	alloc, LJ_ARENAK_TRAVERSABLE));
  assert(arena_list_contains(
	alloc->needsweep[LJ_ARENAK_TRAVERSABLE], fx.a));
  arena_needsweep_move_head(alloc, LJ_ARENAK_TRAVERSABLE, fx.a);
  for (w = 0; w < LJ_ARENA_WORDS; w++)
    (void)la_or64_rlx(&fx.a->mark[w], la_load64_acq(&fx.a->block[w]));
  for (i = 0; i < fx.nselected; i++) {
    lj_arena_bm_clear(fx.a->mark, fx.selected[i].cell);
    selected_size += fx.selected[i].size;
  }
  memset(&bridge, 0, sizeof(bridge));
  bridge.g = fx.g;
  bridge.tg = fx.tg;
  typed_dtor_publish_ready_sweep(&bridge);
  total0 = lj_gc_total_load(fx.g);
  lj_gc_test_sweep_batch_stats_reset();
  assert(lj_gc2_test_sweep_owner_progress(fx.g, fx.tg, 1u) == 1u);
  assert(fx.tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] == fx.a);
  assert(lj_gc_test_sweep_batch_commits() == 1u);
  assert(lj_gc_test_sweep_batch_objects() == wanted);
  total1 = lj_gc_total_load(fx.g);
  assert(total1 == total0 - selected_size);
  assert(lj_arena_reclaim_deferred_acq(fx.a) == 0);
  for (i = 0; i < fx.nselected; i++) {
    uint32_t cell = fx.selected[i].cell;
    assert(lj_arena_sweep_state_acq(fx.a, cell) ==
	   LJ_ARENA_SWEEP_FREEING);
    assert(lj_arena_lifetime_state_acq(fx.a, cell) ==
	   LJ_ARENA_LIFETIME_FREE);
    assert(lj_arena_bm_get(fx.a->block, cell));
    assert(lj_arena_ready_get(fx.a, cell));
    assert(lj_arena_dtor_kind_acq(fx.a, cell) == fx.selected[i].kind);
  }

  other = alloc->needsweep[LJ_ARENAK_TRAVERSABLE];
  arena_fixture_needsweep(alloc, NULL);
  memset(&bridge, 0, sizeof(bridge));
  bridge.g = fx.g;
  bridge.tg = fx.tg;
  bridge.a = fx.a;
  assert(typed_dtor_finish_target(&bridge, other) == total1);
  for (i = 0; i < fx.nselected; i++) {
    assert(!lj_arena_bm_get(fx.a->block, fx.selected[i].cell));
    assert(!lj_arena_ready_get(fx.a, fx.selected[i].cell));
    assert(lj_arena_dtor_kind_acq(fx.a, fx.selected[i].cell) ==
	   LJ_ARENA_DTOR_NONE);
  }
  lua_close(fx.L);
}

static void test_typed_dtor_pregrace_exclusive(void)
{
  TypedDtorFixture fx;
  GCArena *other;
  GCSize total0, total1;
  unsigned char f0body[sizeof(GCfunc)];
  unsigned char f1body[sizeof(GCfunc)];
  unsigned char uvbody[sizeof(GCupval)];

  typed_dtor_fixture_init(&fx);
  assert(fx.f0size <= sizeof(f0body));
  assert(fx.f1size <= sizeof(f1body));
  memcpy(f0body, fx.f0, fx.f0size);
  memcpy(f1body, fx.f1, fx.f1size);
  memcpy(uvbody, fx.uv, fx.uvsize);
  typed_dtor_drop_fixture_roots(&fx);
  total0 = lj_gc_total_load(fx.g);
  lj_gc_test_sweep_batch_stats_reset();
  other = typed_dtor_prepare_target(&fx,
	LJ_ARENA_DTOR_LFUNC0|LJ_ARENA_DTOR_LFUNC1|
	LJ_ARENA_DTOR_CLOSED_UV);
  assert(lj_gc_test_sweep_batch_commits() == 2u);
  assert(lj_gc_test_sweep_batch_objects() == 3u);

  total1 = lj_gc_total_load(fx.g);
  assert(total1 == total0 - fx.f0size - fx.f1size - fx.uvsize);
  assert(lj_arena_reclaim_deferred_acq(fx.a) == 0);
  assert(lj_arena_sweep_state_acq(fx.a, fx.f0cell) ==
	 LJ_ARENA_SWEEP_FREEING);
  assert(lj_arena_sweep_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_SWEEP_FREEING);
  assert(lj_arena_sweep_state_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_SWEEP_FREEING);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.f0cell) ==
	 LJ_ARENA_LIFETIME_FREE);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_LIFETIME_FREE);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_LIFETIME_FREE);
  assert(memcmp(f0body, fx.f0, fx.f0size) == 0);
  assert(memcmp(f1body, fx.f1, fx.f1size) == 0);
  assert(memcmp(uvbody, fx.uv, fx.uvsize) == 0);
  assert(lj_arena_bm_get(fx.a->block, fx.f0cell));
  assert(lj_arena_bm_get(fx.a->block, fx.f1cell));
  assert(lj_arena_bm_get(fx.a->block, fx.uvcell));
  assert(lj_arena_ready_get(fx.a, fx.f0cell));
  assert(lj_arena_ready_get(fx.a, fx.f1cell));
  assert(lj_arena_ready_get(fx.a, fx.uvcell));
  assert(lj_arena_dtor_kind_acq(fx.a, fx.f0cell) ==
	 LJ_ARENA_DTOR_LFUNC0);
  assert(lj_arena_dtor_kind_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_DTOR_LFUNC1);
  assert(lj_arena_dtor_kind_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_DTOR_CLOSED_UV);
  assert(lj_arena_root_state_acq(fx.a, fx.f0cell) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_root_state_acq(fx.a, fx.f1cell) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_root_state_acq(fx.a, fx.uvcell) == LJ_ARENA_ROOT_NONE);

  /* No selected body is charged again by post-grace physical completion. */
  assert(typed_dtor_finish_target(&fx, other) == total1);
  assert(!lj_arena_bm_get(fx.a->block, fx.f0cell));
  assert(!lj_arena_bm_get(fx.a->block, fx.f1cell));
  assert(!lj_arena_bm_get(fx.a->block, fx.uvcell));
  assert(!lj_arena_ready_get(fx.a, fx.f0cell));
  assert(!lj_arena_ready_get(fx.a, fx.f1cell));
  assert(!lj_arena_ready_get(fx.a, fx.uvcell));
  assert(lj_arena_dtor_kind_acq(fx.a, fx.f0cell) == LJ_ARENA_DTOR_NONE);
  assert(lj_arena_dtor_kind_acq(fx.a, fx.f1cell) == LJ_ARENA_DTOR_NONE);
  assert(lj_arena_dtor_kind_acq(fx.a, fx.uvcell) == LJ_ARENA_DTOR_NONE);
  lua_close(fx.L);
}

static void test_typed_dtor_gc2_token_veto(void)
{
  TypedDtorFixture fx;
  GCArena *other;
  LJGC2TabStamp *stamp;
  LJGC2TableTokenTicket ticket;
  GCSize total0;
  unsigned char body[sizeof(GCfunc)];

  typed_dtor_fixture_init(&fx);
  assert(fx.f1size <= sizeof(body));
  memcpy(body, fx.f1, fx.f1size);
  stamp = lj_arena_gc2_stamp_acq(fx.f1);
  assert(stamp != NULL);
  assert(lj_gc2_table_token_refresh(&stamp->token, &ticket) ==
	 LJ_GC2_TABLE_TOKEN_RESULT_OK);
  typed_dtor_drop_fixture_roots(&fx);
  total0 = lj_gc_total_load(fx.g);
  other = typed_dtor_prepare_target(&fx,
	LJ_ARENA_DTOR_LFUNC0|LJ_ARENA_DTOR_LFUNC1|
	LJ_ARENA_DTOR_CLOSED_UV);

  /* The token is a type-independent physical side owner. Even an injected
  ** impossible function/token pairing must prevent pre-grace body access and
  ** terminal lifetime commit while independent objects keep progressing. */
  assert(lj_arena_sweep_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_bm_get(fx.a->block, fx.f1cell));
  assert(memcmp(body, fx.f1, fx.f1size) == 0);
  assert(lj_gc_total_load(fx.g) == total0 - fx.f0size - fx.uvsize);

  assert(lj_gc2_table_token_complete(&stamp->token, &ticket) ==
	 LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(typed_dtor_finish_target(&fx, other) ==
	 total0 - fx.f0size - fx.f1size - fx.uvsize);
  assert(!lj_arena_bm_get(fx.a->block, fx.f1cell));
  lua_close(fx.L);
}

static LJGC2TableDesc *typed_dtor_tabledesc(TypedDtorFixture *fx)
{
  LJGC2TableDesc *desc = &fx->g->gc2.table_rescan_desc;
  assert(lj_arena_gc2_tabledesc_acq(fx->a) == desc);
  return desc;
}

/* PINNED is deliberately absorbing in production. These fixtures inject a
** terminal fault into an otherwise isolated global_State, then use one exact
** CX16 transition solely to restore that fixture for ordinary lua_close.
** Never use this helper in runtime code. */
static void tabledesc_fixture_exact_reset(LJGC2TableDesc *desc,
	uint64_t from_table, uint64_t generation)
{
  la_u128 expected, desired;
  expected.lo = from_table;
  expected.hi = generation;
  desired.lo = 0;
  desired.hi = generation;
  assert(la_cas128(&desc->value, &expected, desired));
}

static void test_typed_dtor_gc2_desc_exact_veto(void)
{
  TypedDtorFixture fx;
  GCArena *other;
  LJGC2TableDesc *desc;
  LJGC2TableDescTicket first, second;
  LJGC2TableDescSnap initial, observed;
  GCSize total0;
  unsigned char body[sizeof(GCfunc)];

  typed_dtor_fixture_init(&fx);
  assert(fx.f1size <= sizeof(body));
  memcpy(body, fx.f1, fx.f1size);
  desc = typed_dtor_tabledesc(&fx);
  initial = lj_gc2_tabledesc_snapshot(desc);
  assert(initial.state == LJ_GC2_TABLEDESC_IDLE);
  assert(lj_gc2_tabledesc_try_publish(desc, fx.f1, &first, &observed) ==
	 LJ_GC2_TABLEDESC_RESULT_OK);
  assert(observed.state == LJ_GC2_TABLEDESC_ACTIVE &&
	 observed.table == (uintptr_t)fx.f1 &&
	 observed.generation == initial.generation + 1u);
  assert(!lj_arena_gc2_desc_clear_acq(fx.a, fx.f1cell));
  assert(!lj_arena_gc2_reclaim_clear_acq(fx.a, fx.f1cell));
  assert(lj_arena_gc2_desc_clear_acq(fx.a, fx.f0cell));
  assert(lj_arena_gc2_desc_clear_acq(fx.a, fx.uvcell));

  typed_dtor_drop_fixture_roots(&fx);
  total0 = lj_gc_total_load(fx.g);
  other = typed_dtor_prepare_target(&fx,
	LJ_ARENA_DTOR_LFUNC0|LJ_ARENA_DTOR_LFUNC1|
	LJ_ARENA_DTOR_CLOSED_UV);

  /* ACTIVE is a type-independent physical lifetime claim. The injected
  ** function identity protects only its exact start; independent bodies in
  ** the same arena still make progress. */
  assert(lj_arena_sweep_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_bm_get(fx.a->block, fx.f1cell));
  assert(memcmp(body, fx.f1, fx.f1size) == 0);
  assert(lj_gc_total_load(fx.g) == total0 - fx.f0size - fx.uvsize);

  assert(lj_gc2_tabledesc_finish_help(desc, &first, &observed) ==
	 LJ_GC2_TABLEDESC_RESULT_OK);
  assert(observed.state == LJ_GC2_TABLEDESC_IDLE &&
	 observed.generation == first.generation);
  assert(lj_arena_gc2_reclaim_clear_acq(fx.a, fx.f1cell));

  /* Reusing the exact address immediately creates a new authority. A stale
  ** release must not create a false IDLE interval for reclamation. */
  assert(lj_gc2_tabledesc_try_publish(desc, fx.f1, &second, &observed) ==
	 LJ_GC2_TABLEDESC_RESULT_OK);
  assert(second.generation == first.generation + 1u);
  assert(!lj_arena_gc2_reclaim_clear_acq(fx.a, fx.f1cell));
  assert(lj_gc2_tabledesc_finish_help(desc, &first, &observed) ==
	 LJ_GC2_TABLEDESC_RESULT_BUSY);
  assert(observed.state == LJ_GC2_TABLEDESC_ACTIVE &&
	 observed.table == (uintptr_t)fx.f1 &&
	 observed.generation == second.generation);
  assert(!lj_arena_gc2_reclaim_clear_acq(fx.a, fx.f1cell));
  assert(lj_gc2_tabledesc_finish_help(desc, &second, &observed) ==
	 LJ_GC2_TABLEDESC_RESULT_OK);
  assert(observed.state == LJ_GC2_TABLEDESC_IDLE &&
	 observed.generation == second.generation);
  assert(lj_arena_gc2_reclaim_clear_acq(fx.a, fx.f1cell));

  assert(typed_dtor_finish_target(&fx, other) ==
	 total0 - fx.f0size - fx.f1size - fx.uvsize);
  assert(!lj_arena_bm_get(fx.a->block, fx.f1cell));
  lua_close(fx.L);
}

static void test_typed_dtor_gc2_desc_unrelated_progress(void)
{
  TypedDtorFixture fx;
  GCArena *other;
  LJGC2TableDesc *desc;
  LJGC2TableDescTicket ticket;
  LJGC2TableDescSnap observed;
  GCSize total0;

  typed_dtor_fixture_init(&fx);
  desc = typed_dtor_tabledesc(&fx);
  assert(lj_gc2_tabledesc_try_publish(desc, fx.f0, &ticket, &observed) ==
	 LJ_GC2_TABLEDESC_RESULT_OK);
  assert(!lj_arena_gc2_desc_clear_acq(fx.a, fx.f0cell));
  assert(lj_arena_gc2_desc_clear_acq(fx.a, fx.f1cell));
  assert(lj_arena_gc2_desc_clear_acq(fx.a, fx.uvcell));

  typed_dtor_drop_fixture_roots(&fx);
  total0 = lj_gc_total_load(fx.g);
  other = typed_dtor_prepare_target(&fx,
	LJ_ARENA_DTOR_LFUNC1|LJ_ARENA_DTOR_CLOSED_UV);

  /* An ACTIVE identity in this same arena must not turn the single global
  ** descriptor into a global stop-the-world reclamation gate. */
  assert(lj_gc_total_load(fx.g) == total0 - fx.f1size - fx.uvsize);
  assert(lj_arena_sweep_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_SWEEP_FREEING);
  assert(lj_arena_sweep_state_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_SWEEP_FREEING);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_LIFETIME_FREE);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_LIFETIME_FREE);
  assert(lj_gc2_tabledesc_finish_help(desc, &ticket, &observed) ==
	 LJ_GC2_TABLEDESC_RESULT_OK);
  assert(observed.state == LJ_GC2_TABLEDESC_IDLE);
  assert(typed_dtor_finish_target(&fx, other) ==
	 total0 - fx.f1size - fx.uvsize);
  lua_close(fx.L);
}

static void test_typed_dtor_gc2_desc_pinned_veto(void)
{
  TypedDtorFixture fx;
  GCArena *other;
  LJGC2TableDesc *desc;
  LJGC2TableDescSnap snap, observed;
  GCSize total0;

  typed_dtor_fixture_init(&fx);
  desc = typed_dtor_tabledesc(&fx);
  snap = lj_gc2_tabledesc_snapshot(desc);
  assert(snap.state == LJ_GC2_TABLEDESC_IDLE);
  assert(lj_gc2_tabledesc_pin(desc, snap, &observed) ==
	 LJ_GC2_TABLEDESC_RESULT_PINNED);
  assert(observed.state == LJ_GC2_TABLEDESC_PINNED);
  assert(!lj_arena_gc2_desc_clear_acq(fx.a, fx.f0cell));
  assert(!lj_arena_gc2_desc_clear_acq(fx.a, fx.f1cell));
  assert(!lj_arena_gc2_desc_clear_acq(fx.a, fx.uvcell));

  typed_dtor_drop_fixture_roots(&fx);
  total0 = lj_gc_total_load(fx.g);
  other = typed_dtor_prepare_target(&fx,
	LJ_ARENA_DTOR_LFUNC0|LJ_ARENA_DTOR_LFUNC1|
	LJ_ARENA_DTOR_CLOSED_UV);
  assert(lj_gc_total_load(fx.g) == total0);
  assert(lj_arena_sweep_state_acq(fx.a, fx.f0cell) ==
	 LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_sweep_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_sweep_state_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.f0cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_LIFETIME_LIVE);

  tabledesc_fixture_exact_reset(desc, 1u, observed.generation);
  assert(lj_arena_gc2_reclaim_clear_acq(fx.a, fx.f0cell));
  assert(lj_arena_gc2_reclaim_clear_acq(fx.a, fx.f1cell));
  assert(lj_arena_gc2_reclaim_clear_acq(fx.a, fx.uvcell));
  assert(typed_dtor_finish_target(&fx, other) ==
	 total0 - fx.f0size - fx.f1size - fx.uvsize);
  lua_close(fx.L);
}

static void test_typed_dtor_gc2_desc_malformed_veto(void)
{
  TypedDtorFixture fx;
  GCArena *other;
  LJGC2TableDesc *desc;
  LJGC2TableDescSnap snap;
  la_u128 expected, malformed;
  GCSize total0;

  typed_dtor_fixture_init(&fx);
  desc = typed_dtor_tabledesc(&fx);
  snap = lj_gc2_tabledesc_snapshot(desc);
  assert(snap.state == LJ_GC2_TABLEDESC_IDLE);
  expected.lo = 0;
  expected.hi = snap.generation;
  malformed.lo = 3;
  malformed.hi = snap.generation;
  assert(la_cas128(&desc->value, &expected, malformed));
  assert(lj_gc2_tabledesc_snapshot(desc).state ==
	 LJ_GC2_TABLEDESC_INVALID);
  assert(!lj_arena_gc2_desc_clear_acq(fx.a, fx.f0cell));
  assert(!lj_arena_gc2_desc_clear_acq(fx.a, fx.f1cell));
  assert(!lj_arena_gc2_desc_clear_acq(fx.a, fx.uvcell));

  typed_dtor_drop_fixture_roots(&fx);
  total0 = lj_gc_total_load(fx.g);
  other = typed_dtor_prepare_target(&fx,
	LJ_ARENA_DTOR_LFUNC0|LJ_ARENA_DTOR_LFUNC1|
	LJ_ARENA_DTOR_CLOSED_UV);
  assert(lj_gc_total_load(fx.g) == total0);
  assert(lj_arena_sweep_state_acq(fx.a, fx.f0cell) ==
	 LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_sweep_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_sweep_state_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.f0cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_LIFETIME_LIVE);

  tabledesc_fixture_exact_reset(desc, 3u, snap.generation);
  assert(lj_arena_gc2_reclaim_clear_acq(fx.a, fx.f0cell));
  assert(lj_arena_gc2_reclaim_clear_acq(fx.a, fx.f1cell));
  assert(lj_arena_gc2_reclaim_clear_acq(fx.a, fx.uvcell));
  assert(typed_dtor_finish_target(&fx, other) ==
	 total0 - fx.f0size - fx.f1size - fx.uvsize);
  lua_close(fx.L);
}

static void test_typed_dtor_no_adjacency_match(void)
{
  TypedDtorFixture fx;
  GCArena *other;
  GCSize total0;

  typed_dtor_fixture_init(&fx);
  typed_dtor_drop_fixture_roots(&fx);
  lj_arena_bm_set(fx.a->cdata, fx.uvcell);
  total0 = lj_gc_total_load(fx.g);
  lj_gc_test_sweep_batch_stats_reset();
  other = typed_dtor_prepare_target(&fx,
	LJ_ARENA_DTOR_LFUNC1|LJ_ARENA_DTOR_CLOSED_UV);
  assert(lj_gc_test_sweep_batch_commits() == 0);
  assert(lj_gc_test_sweep_batch_objects() == 0);

  /* The closure's immutable identity is sufficient by itself. Its adjacent
  ** upvalue disagreement must not force a pair match or veto this destructor. */
  assert(lj_gc_total_load(fx.g) == total0 - fx.f1size);
  assert(lj_arena_sweep_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_SWEEP_FREEING);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_LIFETIME_FREE);
  assert(lj_arena_sweep_state_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_reclaim_deferred_acq(fx.a) == 1u);
  assert(lj_arena_dtor_kind_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_DTOR_LFUNC1);
  assert(lj_arena_dtor_kind_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_DTOR_CLOSED_UV);
  assert(lj_arena_cdata_get(fx.a, fx.uvcell));

  /* Removing the independent disagreement admits exactly the UV destructor
  ** after the already-required arena grace; the closure is not charged twice. */
  lj_arena_bm_clear(fx.a->cdata, fx.uvcell);
  assert(typed_dtor_finish_target(&fx, other) ==
	 total0 - fx.f1size - fx.uvsize);
  assert(!lj_arena_bm_get(fx.a->block, fx.f1cell));
  assert(!lj_arena_bm_get(fx.a->block, fx.uvcell));
  assert(lj_arena_dtor_kind_acq(fx.a, fx.f1cell) == LJ_ARENA_DTOR_NONE);
  assert(lj_arena_dtor_kind_acq(fx.a, fx.uvcell) == LJ_ARENA_DTOR_NONE);
  lua_close(fx.L);
}

enum {
  TYPED_BATCH_REJECT_FIXED = 1,
  TYPED_BATCH_REJECT_HEADER = 2
};

static void test_typed_dtor_batch_body_reject(uint32_t mode)
{
  TypedDtorFixture fx;
  GCArena *other;
  GCSize total0, after_scalar;
  uint8_t saved_gct = 0;

  typed_dtor_fixture_init(&fx);
  typed_dtor_drop_fixture_roots(&fx);
  if (mode == TYPED_BATCH_REJECT_FIXED) {
    lj_obj_addgcflags_atomic(obj2gco(fx.f1), LJ_GC_FIXED|LJ_GC_SFIXED);
  } else {
    assert(mode == TYPED_BATCH_REJECT_HEADER);
    saved_gct = la_load8_acq(&fx.uv->gct);
    la_store8_rel(&fx.uv->gct, 0);
  }
  total0 = lj_gc_total_load(fx.g);
  lj_gc_test_sweep_batch_stats_reset();
  other = typed_dtor_prepare_target(&fx,
	LJ_ARENA_DTOR_LFUNC1|LJ_ARENA_DTOR_CLOSED_UV);

  /* Both starts were selected and claimed together. One invalid body makes
  ** the batch dispatch none; scalar fallback can still destroy the independent
  ** valid peer and retires the rejected start for post-grace validation. */
  assert(lj_gc_test_sweep_batch_commits() == 0);
  assert(lj_gc_test_sweep_batch_objects() == 0);
  if (mode == TYPED_BATCH_REJECT_FIXED) {
    assert(lj_arena_sweep_state_acq(fx.a, fx.f1cell) ==
	   LJ_ARENA_SWEEP_RETIRED);
    assert(lj_arena_sweep_state_acq(fx.a, fx.uvcell) ==
	   LJ_ARENA_SWEEP_FREEING);
    after_scalar = total0 - fx.uvsize;
    lj_obj_cleargcflags_atomic(obj2gco(fx.f1), LJ_GC_FIXED|LJ_GC_SFIXED);
  } else {
    assert(lj_arena_sweep_state_acq(fx.a, fx.f1cell) ==
	   LJ_ARENA_SWEEP_FREEING);
    assert(lj_arena_sweep_state_acq(fx.a, fx.uvcell) ==
	   LJ_ARENA_SWEEP_RETIRED);
    after_scalar = total0 - fx.f1size;
    la_store8_rel(&fx.uv->gct, saved_gct);
  }
  assert(lj_gc_total_load(fx.g) == after_scalar);
  assert(lj_arena_reclaim_deferred_acq(fx.a) == 1u);
  assert(typed_dtor_finish_target(&fx, other) ==
	 total0 - fx.f1size - fx.uvsize);
  lua_close(fx.L);
}

static void test_typed_dtor_batch_extent_reject(void)
{
  TypedDtorFixture fx;
  TGAlloc *alloc;
  GCArena *other;
  GCSize total0;

  typed_dtor_fixture_init(&fx);
  typed_dtor_drop_fixture_roots(&fx);
  alloc = &fx.tg->alloc;
  assert(lj_arena_alloc_prepare_sweep_kind(
	alloc, LJ_ARENAK_TRAVERSABLE));
  assert(arena_list_contains(
	alloc->needsweep[LJ_ARENAK_TRAVERSABLE], fx.a));
  arena_needsweep_move_head(alloc, LJ_ARENAK_TRAVERSABLE, fx.a);
  typed_dtor_select_dead(&fx,
	LJ_ARENA_DTOR_LFUNC1|LJ_ARENA_DTOR_CLOSED_UV);
  assert(fx.f1cell + 1u < fx.uvcell);
  lj_arena_bm_set(fx.a->mark, fx.f1cell + 1u);
  typed_dtor_publish_ready_sweep(&fx);
  total0 = lj_gc_total_load(fx.g);
  lj_gc_test_sweep_batch_stats_reset();
  assert(lj_gc2_test_sweep_owner_progress(fx.g, fx.tg, 1u) == 1u);
  assert(fx.tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] == fx.a);

  /* The interior mark shortens the exact closure extent, so only the valid UV
  ** is batch-admitted. The closure takes unchanged scalar retirement. */
  assert(lj_gc_test_sweep_batch_commits() == 1u);
  assert(lj_gc_test_sweep_batch_objects() == 1u);
  assert(lj_gc_total_load(fx.g) == total0 - fx.uvsize);
  assert(lj_arena_sweep_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_sweep_state_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_SWEEP_FREEING);
  assert(lj_arena_reclaim_deferred_acq(fx.a) == 1u);

  lj_arena_bm_clear(fx.a->mark, fx.f1cell + 1u);
  other = alloc->needsweep[LJ_ARENAK_TRAVERSABLE];
  arena_fixture_needsweep(alloc, NULL);
  assert(typed_dtor_finish_target(&fx, other) ==
	 total0 - fx.f1size - fx.uvsize);
  lua_close(fx.L);
}

static GCobj *typed_batch_recovery_target;
static uint32_t typed_batch_recovery_fired;
static int typed_batch_recovery_result;

static void typed_batch_recovery_hook(global_State *g, GCArena *a,
	uint32_t path)
{
  (void)a;
  if (path == LJ_GC_SWEEP_BATCH_TEST_AFTER_CLAIM &&
      !typed_batch_recovery_fired) {
    typed_batch_recovery_fired = 1;
    lj_gc_test_set_sweep_batch_hook(NULL);
    typed_batch_recovery_result =
      lj_gc2_test_recovery_publish(g, typed_batch_recovery_target);
  }
}

static void test_typed_dtor_batch_recovery_cancel(void)
{
  TypedDtorFixture fx;
  GCArena *other;
  GCSize total0;

  typed_dtor_fixture_init(&fx);
  typed_dtor_drop_fixture_roots(&fx);
  total0 = lj_gc_total_load(fx.g);
  typed_batch_recovery_target = obj2gco(fx.f1);
  typed_batch_recovery_fired = 0;
  typed_batch_recovery_result = 0;
  lj_gc_test_sweep_batch_stats_reset();
  lj_gc_test_set_sweep_batch_hook(typed_batch_recovery_hook);
  other = typed_dtor_prepare_target(&fx,
	LJ_ARENA_DTOR_LFUNC1|LJ_ARENA_DTOR_CLOSED_UV);
  lj_gc_test_set_sweep_batch_hook(NULL);

  /* Recovery reserves durable work, steals one DESTRUCT lane, and restores it
  ** to LIVE. The batch must commit no lane; rollback restores only the peer's
  ** still-owned DESTRUCT and scalar fallback retires both without body access. */
  assert(typed_batch_recovery_fired);
  assert(typed_batch_recovery_result == 1);
  assert(lj_gc_test_sweep_batch_commits() == 0);
  assert(lj_gc_test_sweep_batch_objects() == 0);
  assert(lj_gc_total_load(fx.g) == total0);
  assert(lj_arena_recovery_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_sweep_state_acq(fx.a, fx.f1cell) ==
	 LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_sweep_state_acq(fx.a, fx.uvcell) ==
	 LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_reclaim_deferred_acq(fx.a) == 2u);

  assert(gc2_recovery_items_acq(fx.g) == 1u);
  assert(lj_gc2_test_recovery_drain(fx.g, 1u) == 1u);
  assert(gc2_recovery_items_acq(fx.g) == 0);
  /* Recovery deliberately preserves the selected object for this cycle, so
  ** this target need not reach reclaimed publication. Reattach the unrelated
  ** NEEDSWEEP suffix and let joined-world lua_close drain the live quarantine. */
  arena_fixture_needsweep(&fx.tg->alloc, other);
  lua_close(fx.L);
}

static void test_typed_dtor_denied_capability_retires(int smr_busy)
{
  TypedDtorFixture fx;
  GCArena *other;
  GCSize total0;
  uint32_t expect = LJ_GC2_SMR_OPEN;

  typed_dtor_fixture_init(&fx);
  typed_dtor_drop_fixture_roots(&fx);
  assert(lj_arena_alloc_prepare_sweep_kind(
	&fx.tg->alloc, LJ_ARENAK_TRAVERSABLE));
  assert(arena_list_contains(
	fx.tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE], fx.a));
  arena_needsweep_move_head(&fx.tg->alloc, LJ_ARENAK_TRAVERSABLE, fx.a);
  typed_dtor_select_dead(&fx, LJ_ARENA_DTOR_LFUNC0);
  typed_dtor_publish_ready_sweep(&fx);
  total0 = lj_gc_total_load(fx.g);

  assert(mt_gc_exclusive_acq(fx.g) == 0);
  assert(gc2_smr_reclaiming_acq(fx.g) == 0);
  if (smr_busy) {
    /* Losing the body lease after winning MT exclusivity must still execute
    ** the original sidecar-only classification before quarantine. */
    assert(gc2_smr_reclaiming_cas(
	 fx.g, &expect, LJ_GC2_SMR_META_EXCLUSIVE));
    assert(lj_gc_sweep_gc2_arena_unmarked_exclusive(fx.g, fx.a) == 0);
    assert(mt_gc_exclusive_acq(fx.g) == 0);
    gc2_smr_reclaiming_rel(fx.g, LJ_GC2_SMR_OPEN);
    assert(lj_arena_alloc_quarantine_one(&fx.tg->alloc,
	LJ_ARENAK_TRAVERSABLE, gc2_hs_epoch_acq(fx.g)) == fx.a);
    gc2_sweep_grace_needed_rel(fx.g, 1);
  } else {
    /* This token has no local acquisition capability in the arena helper. A
    ** mere observed value of one must not authorize body reads. */
    mt_gc_exclusive_rel(fx.g, 1);
    assert(lj_gc2_test_sweep_owner_progress(fx.g, fx.tg, 1u) == 1u);
  }
  assert(lj_gc_total_load(fx.g) == total0);
  assert(lj_arena_sweep_state_acq(fx.a, fx.f0cell) ==
	 LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_lifetime_state_acq(fx.a, fx.f0cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_reclaim_deferred_acq(fx.a) == 1u);
  assert(lj_arena_bm_get(fx.a->block, fx.f0cell));
  assert(lj_arena_ready_get(fx.a, fx.f0cell));
  assert(lj_arena_dtor_kind_acq(fx.a, fx.f0cell) ==
	 LJ_ARENA_DTOR_LFUNC0);
  assert(lj_arena_root_state_acq(fx.a, fx.f0cell) == LJ_ARENA_ROOT_NONE);
  if (!smr_busy) {
    mt_gc_exclusive_rel(fx.g, 0);
    mt_gc_exclusive_futex_wake(fx.g, INT_MAX);
  }

  assert(fx.tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] == fx.a);
  other = fx.tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE];
  arena_fixture_needsweep(&fx.tg->alloc, NULL);
  assert(typed_dtor_finish_target(&fx, other) == total0 - fx.f0size);
  assert(!lj_arena_bm_get(fx.a->block, fx.f0cell));
  assert(lj_arena_dtor_kind_acq(fx.a, fx.f0cell) == LJ_ARENA_DTOR_NONE);
  lua_close(fx.L);
}

static void test_preserve_abort_waits_for_restore_publisher(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *t;
  GCArena *a;
  LJGC2ActivationSnap sweep, idle;
  uint32_t cycle;
  int admission;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  t = lj_tab_new(L, 0, 0);
  assert(t != NULL);
  a = lj_arena_of(t);
  assert(!lj_arena_ishuge(a));
  assert((lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) != 0);

  cycle = ++g->gc2.cycle;
  sweep = test_publish_sweep_phase(g);
  assert(lj_arena_alloc_prepare_sweep_kind(
	&tg->alloc, LJ_ARENAK_TRAVERSABLE));
  tg->alloc.prepare_epoch = cycle;
  assert(arena_list_contains(
	tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE], a));

  /* Hold a legal counted terminal reader across the abort handshake. Its
  ** count defeats this owner's exact restore LP without blocking either side. */
  admission = lj_arena_rescue_enter(a);
  assert(admission == LJ_ARENA_RESCUE_FULL);
  lj_gc2_preserve_abort_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  idle = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&sweep, &idle));
  assert(tg->alloc.prepare_epoch == cycle);
  assert(lj_gc2_sweep_needs_restore(g));

  lj_arena_rescue_leave(a);
  lj_gc2_preserve_abort_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  idle = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(idle.state == LJ_GC2_ACT_IDLE);
  assert(idle.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(idle.generation == sweep.generation + 1u);
  assert(idle.mark_epoch == sweep.mark_epoch);
  assert(!lj_gc2_activation_reclaim_veto(g));
  assert(tg->alloc.prepare_epoch == 0);
  assert(!lj_gc2_sweep_needs_restore(g));
  assert(arena_list_contains(
	tg->alloc.owned[LJ_ARENAK_TRAVERSABLE], a));
  lua_close(L);
}

static void test_prepare_collision_detaches_allocator(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCArena *first = NULL, *second = NULL, *fresh_a;
  void *fresh;
  uint32_t i;
  int admission;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  for (i = 0; i < 16u && !second; i++) {
    void *p = lj_arena_alloc(&tg->alloc, &tg->prng,
	LJ_HUGE_THRESHOLD, LJ_AF_TRAVERSABLE);
    GCArena *a;
    assert(p != NULL);
    a = lj_arena_of(p);
    if (!first)
      first = a;
    else if (a != first)
      second = a;
  }
  assert(first != NULL && second != NULL);

  /* A live OPEN publisher makes one arena's nonwaiting seal fail. Every old
  ** arena must nevertheless be detached from bump/bins/owned before ACK lets
  ** the mutator allocate again. */
  admission = lj_arena_rescue_enter(second);
  assert(admission == LJ_ARENA_RESCUE_FULL);
  assert(!lj_arena_alloc_prepare_sweep_kind(
	&tg->alloc, LJ_ARENAK_TRAVERSABLE));
  assert(!arena_list_contains(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE],
	first));
  assert(!arena_list_contains(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE],
	second));
  assert(arena_list_contains(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
	first));
  assert(arena_list_contains(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
	second));
  fresh = lj_arena_alloc(&tg->alloc, &tg->prng, 64u,
			 LJ_AF_TRAVERSABLE);
  assert(fresh != NULL);
  fresh_a = lj_arena_of(fresh);
  assert(fresh_a != first && fresh_a != second);

  lj_arena_rescue_leave(second);
  assert(lj_arena_alloc_prepare_sweep_kind(
	&tg->alloc, LJ_ARENAK_TRAVERSABLE));
  assert(arena_list_contains(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
	fresh_a));
  assert(lj_arena_alloc_restore_sweep_kind(
	&tg->alloc, LJ_ARENAK_TRAVERSABLE));
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(arena_list_contains(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE],
	first));
  assert(arena_list_contains(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE],
	second));
  assert(arena_list_contains(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE],
	fresh_a));
  lua_close(L);
}

static void test_reclaim_noop_word_summary(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  TGAlloc alloc;
  GCArena *a;
  uint32_t first_end;
  uint32_t word_start = (LJ_AFIRST_CELL + 63u) & ~63u;
  uint32_t word_budget = LJ_ARENA_WORDS - (LJ_AFIRST_CELL >> 6);
  uint32_t action = LJ_AFIRST_CELL;
  uint32_t later = word_start + 17u;
  uint32_t reason = LJ_ARENA_FINISH_NONE;
  int done = 0;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  a = lj_arena_map(&tg->prng, LJ_AF_TRAVERSABLE);
  assert(a != NULL);
  a->hdr.owner_tid = tg->alloc.owner_tid;
  a->hdr.reclaim_cell = LJ_AFIRST_CELL;
  la_store32_rel(&a->hdr.flags,
	lj_arena_flags_acq(a) | LJ_AF_QUARANTINE);

  /* One budget unit covers exactly the remaining portion of the first empty
  ** bitmap word, rather than reverting to a 64-cell sequence of no-op loads. */
  first_end = (LJ_AFIRST_CELL | 63u) + 1u;
  assert(lj_gc_reclaim_gc2_arena(g, a, 1u, &done) == 1u);
  assert(!done);
  assert(a->hdr.reclaim_cell == first_end);

  /* Any semantic lane keeps its word on the original per-cell budget. A
  ** transient root claim is body-free and therefore safe for this fixture. */
  lj_arena_bm_set(a->block, action);
  assert(lj_arena_root_state_cas(a, action, LJ_ARENA_ROOT_NONE,
					 LJ_ARENA_ROOT_LINKING));
  a->hdr.reclaim_cell = action;
  done = 0;
  assert(lj_gc_reclaim_gc2_arena(g, a, 1u, &done) == 1u);
  assert(!done);
  assert(a->hdr.reclaim_cell == action + 1u);
  assert(lj_arena_root_state_cas(a, action, LJ_ARENA_ROOT_LINKING,
					 LJ_ARENA_ROOT_NONE));
  lj_arena_bm_clear(a->block, action);

  /* The packed summary must inspect the complete remaining word, not merely
  ** its cursor lane. Exercise each non-root blocker at a later allocation
  ** start; one budget unit must retain the original one-cell path. */
  assert(later < LJ_ARENA_CELLS && (later >> 6) == (word_start >> 6));
  lj_arena_bm_set(a->block, later);
  assert(lj_arena_sweep_state_cas(a, later, LJ_ARENA_SWEEP_WHITE,
					  LJ_ARENA_SWEEP_RETIRED));
  a->hdr.reclaim_cell = word_start;
  done = 0;
  assert(lj_gc_reclaim_gc2_arena(g, a, 1u, &done) == 1u);
  assert(!done);
  assert(a->hdr.reclaim_cell == word_start + 1u);
  assert(lj_arena_sweep_state_acq(a, later) == LJ_ARENA_SWEEP_RETIRED);
  assert(lj_arena_sweep_state_cas(a, later, LJ_ARENA_SWEEP_RETIRED,
					  LJ_ARENA_SWEEP_WHITE));

  assert(lj_arena_recovery_state_cas(a, later, LJ_ARENA_RECOVERY_IDLE,
					     LJ_ARENA_RECOVERY_PENDING));
  a->hdr.reclaim_cell = word_start;
  done = 0;
  assert(lj_gc_reclaim_gc2_arena(g, a, 1u, &done) == 1u);
  assert(!done);
  assert(a->hdr.reclaim_cell == word_start + 1u);
  assert(lj_arena_recovery_state_acq(a, later) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(lj_arena_recovery_state_cas(a, later, LJ_ARENA_RECOVERY_PENDING,
					     LJ_ARENA_RECOVERY_IDLE));

  (void)la_bit_test_and_set64(&a->late[later >> 6], later & 63u);
  a->hdr.reclaim_cell = word_start;
  done = 0;
  assert(lj_gc_reclaim_gc2_arena(g, a, 1u, &done) == 1u);
  assert(!done);
  assert(a->hdr.reclaim_cell == word_start + 1u);
  assert(lj_arena_late_get(a, later));
  (void)la_and64_rlx(&a->late[later >> 6],
			 ~((uint64_t)1 << (later & 63u)));
  lj_arena_bm_clear(a->block, later);

  /* The exact remaining-word budget covers the complete arena, including the
  ** partial first word. EOF with no semantic change reports completion. */
  a->hdr.reclaim_cell = LJ_AFIRST_CELL;
  done = 0;
  assert(lj_gc_reclaim_gc2_arena(
	g, a, word_budget, &done) == 0u);
  assert(done);
  assert(a->hdr.reclaim_cell == LJ_ARENA_CELLS);

  /* Publish actionable state behind that summarized EOF. Finish must retain
  ** the arena and lower the cursor to the exact cell before any bitmap apply.
  ** No body is needed: readiness rejects LIVE before header admission. */
  lj_arena_bm_set(a->block, action);
  assert(lj_arena_sweep_state_cas(a, action, LJ_ARENA_SWEEP_WHITE,
					  LJ_ARENA_SWEEP_LIVE));
  lj_arena_alloc_init(&alloc);
  alloc.quarantine[LJ_ARENAK_TRAVERSABLE] = a;
  assert(lj_arena_reclaim_seal(a));
  assert(!lj_arena_alloc_quarantine_finish(&alloc,
	LJ_ARENAK_TRAVERSABLE, a, 1u, 0, &reason));
  assert(reason == LJ_ARENA_FINISH_ACTIONABLE);
  assert(a->hdr.reclaim_cell == action);
  lj_arena_reclaim_unseal(a, 1);

  assert(lj_arena_sweep_state_cas(a, action, LJ_ARENA_SWEEP_LIVE,
					  LJ_ARENA_SWEEP_WHITE));
  lj_arena_bm_clear(a->block, action);
  alloc.quarantine[LJ_ARENAK_TRAVERSABLE] = NULL;
  lj_arena_unmap(a);
  lua_close(L);
}

static void test_quarantine_late_live_after_eof(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *t;
  GCArena *a, *other_needsweep;
  LJGC2ActivationSnap sweep, idle;
  uint64_t hs_epoch;
  uint32_t cell, step, i;
  int done = 0;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  t = lj_tab_new(L, 0, 0);
  assert(t != NULL);
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  assert(gc_root_list_contains(g, obj2gco(t)));
  a = lj_arena_of(t);
  cell = lj_arena_cellof(t);
  assert(!lj_arena_ishuge(a));
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  assert((lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) != 0);

  lj_arena_alloc_prepare_sweep_kind(&tg->alloc,
				    LJ_ARENAK_TRAVERSABLE);
  assert(arena_list_contains(
	    tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE], a));
  arena_needsweep_move_head(&tg->alloc, LJ_ARENAK_TRAVERSABLE, a);
  /* Make every still-WHITE allocation in this isolated target arena an
  ** explicit retained/raw cell. The table remains linked until the deliberate
  ** late detach below, so no invalid header is manufactured for reclamation. */
  (void)lj_gc_sweep_gc2_arena_unmarked(g, a);
  assert(lj_arena_bm_get(a->mark, cell));
  assert(lj_arena_alloc_quarantine_one(&tg->alloc,
	    LJ_ARENAK_TRAVERSABLE, 1u) == a);
  /* Keep unrelated prepared arenas out of the focused owner-progress choice;
  ** restore this owner-local list before teardown. */
  other_needsweep = tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE];
  arena_fixture_needsweep(&tg->alloc, NULL);

  /* First complete the summarized bounded pass with no actionable sidecar
  ** state. The later LIVE publication is therefore behind its numeric EOF. */
  while (!done) {
    step = lj_gc_reclaim_gc2_arena(g, a, 64u, &done);
    assert(step != 0 || done);
  }
  assert(a->hdr.reclaim_cell == LJ_ARENA_CELLS);
  assert(lj_arena_reclaim_deferred_acq(a) == 0);

  /* Model a sweep-time preservation/detach publication after the numeric
  ** cursor already passed this exact valid header. Before the fix, finish
  ** rejected LIVE forever while every subsequent reclaim started at EOF. */
  lj_gc_unlink_root_obj(g, obj2gco(t));
  assert(!gc_root_list_contains(g, obj2gco(t)));
  assert(lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_WHITE,
				  LJ_ARENA_SWEEP_LIVE));
  /* PREPSWEEP hands quarantine an exact CLOSED, publisher-free gate. */
  assert(lj_arena_remote_active_acq(a) == LJ_ARENA_REMOTE_CLOSED);
  g->gc2.cycle++;
  sweep = test_publish_sweep_phase(g);
  tg->alloc.prepare_epoch = g->gc2.cycle;
  gc2_sweep_bridge_ready_store_rlx(g, 0);
  gc2_sweep_root_done_rel(g, 1);
  assert(lj_gc2_handshake(g,
	LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_FLUSH_SSB) == 1);
  while (lj_gc2_test_ssb_drain(g) != 0)
    ;
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_thread_scan_needscan_pending_acq(g) == 0);
  /* This fixture manually publishes READY instead of driving the normal
  ** bridge helper. Retire the snapshot's completed mark round just as that
  ** helper does at its READY linearization point. */
  (void)gc2_marks_this_round_xchg_acqrel(g, 0);
  hs_epoch = gc2_hs_epoch_acq(g);
  assert(hs_epoch != 0);
  la_store64_rel(&a->hdr.retire_epoch, hs_epoch - 1u);
  gc2_sweep_grace_needed_rel(g, 0);
  lj_gc2_sweep_bridge_ready(g);
  assert(gc2_sweep_bridge_ready_acq(g));
  /* limit=1 makes the failed finish and exact rearm the sole reported unit.
  ** Without owner step accounting this call incorrectly declares no work. */
  assert(lj_gc2_test_sweep_owner_progress(g, tg, 1u) == 1u);
  /* CLOSED must precede the rejecting readiness scan and remain closed across
  ** retry; otherwise an ordinary producer can mutate after validation and
  ** still let the terminal close CAS succeed. */
  assert((lj_arena_remote_active_acq(a) & LJ_ARENA_REMOTE_CLOSED) != 0);
  assert(a->hdr.reclaim_cell == cell);  /* Exact actionable backedge. */

  assert(lj_gc2_test_sweep_owner_progress(g, tg, 1u) == 1u);
  assert(lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_WHITE);
  assert(gc_root_list_contains(g, obj2gco(t)));
  for (i = 0; i < 128u &&
	  tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] == a; i++)
    assert(lj_gc2_test_sweep_owner_progress(g, tg, 1u) == 1u);
  assert(i < 128u);
  assert(lj_arena_alloc_reclaimed_head(&tg->alloc,
	    LJ_ARENAK_TRAVERSABLE) == a);
  while (lj_gc2_test_ssb_drain(g) != 0)
    ;
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_thread_scan_needscan_pending_acq(g) == 0);
  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  idle = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(idle.state == LJ_GC2_ACT_IDLE);
  assert(idle.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(idle.generation == sweep.generation + 1u);
  assert(idle.mark_epoch == sweep.mark_epoch);
  assert(!lj_gc2_activation_reclaim_veto(g));

  /* Restore the other arenas prepared solely by this fixture. The completed
  ** target deliberately remains on the normal CLOSED reclaimed stack. */
  arena_fixture_needsweep(&tg->alloc, other_needsweep);
  lj_arena_alloc_restore_sweep_kind(&tg->alloc,
				    LJ_ARENAK_TRAVERSABLE);
  lua_close(L);
}

static void seed_traversable_needsweep(TGState *tg, uint32_t n)
{
  uint32_t i;
  for (i = 0; i < n; i++) {
    GCArena *a = lj_arena_map(&tg->prng, LJ_AF_TRAVERSABLE);
    assert(a != NULL);
    a->hdr.owner_tid = tg->alloc.owner_tid;
    a->hdr.flags |= LJ_AF_NEEDSWEEP;
    lj_arena_next_rel(a, tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE]);
    arena_fixture_needsweep(&tg->alloc, a);
  }
}

static void test_worker_owned_sweep_direct(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg, extra_tg;
  uint64_t worker_runs0, arenas0, idle0;
  uint32_t sweep_cycle;
  void *extra_plain, *extra_trav;
  GCArena *extra_plain_a, *extra_trav_a, *swept_a;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  lua_gc(L, LUA_GCCOLLECT, 0);

  lj_tg_init_thread(g, &extra_tg, NULL, 1);
  extra_tg.tid = tg->tid + 3000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  lj_native_enter(&extra_tg);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);

  extra_plain = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64, 0);
  extra_trav = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			      LJ_AF_TRAVERSABLE);
  assert(extra_plain != NULL);
  assert(extra_trav != NULL);
  extra_plain_a = lj_arena_of(extra_plain);
  extra_trav_a = lj_arena_of(extra_trav);

  g->gc2.cycle++;
  sweep_cycle = g->gc2.cycle;
  (void)test_publish_sweep_phase(g);
  gc2_sweep_bridge_ready_store_rlx(g, 0);
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN);
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN],
			     extra_plain_a));
  assert(arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     extra_trav_a));
  swept_a = extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE];
  assert(swept_a != NULL);
  assert(lj_gc2_sweep_tg_ready(&extra_tg));
  assert(lj_gc2_sweep_needs_prepare(g));
  assert(lj_gc2_sweep_pending(g));

  worker_runs0 = gc2_worker_runs_acq(g);
  arenas0 = gc2_sweep_owner_arenas_acq(g);
  assert(lj_gc2_worker_drain(g, 1) == 0);
  assert(gc2_worker_runs_acq(g) == worker_runs0);
  assert(gc2_sweep_owner_arenas_acq(g) == arenas0);
  lj_gc2_sweep_bridge_ready(g);

  /* Classification and epoch grace are distinct production worker quanta. */
  assert(lj_gc2_worker_drain(g, 1u) == 1u);
  assert(extra_tg.alloc.quarantine[LJ_ARENAK_TRAVERSABLE] == swept_a);
  assert(gc2_sweep_grace_needed_acq(g));
  assert(lj_gc2_worker_drain(g, 1u) == 1u);
  assert(!gc2_sweep_grace_needed_acq(g));

  /* An inert arena now finishes in one summarized reclaim visit. The internal
  ** finishedp edge stops the global TG walk, but the public worker must report
  ** its actual single unit rather than saturating this larger quantum. */
  assert(LJ_GC2_SWEEP_BATCH > 1u);
  assert(lj_gc2_worker_drain(g, LJ_GC2_SWEEP_BATCH) == 1u);
  assert(gc2_worker_runs_acq(g) > worker_runs0);
  assert(gc2_sweep_owner_arenas_acq(g) == arenas0 + 1u);
  assert(gc2_worker_active_acq(g) == 0);
  assert(!arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			      swept_a));
  assert(arena_list_contains(lj_arena_alloc_reclaimed_head(
		&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE), swept_a));
  assert((extra_plain_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((swept_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((swept_a->hdr.flags & LJ_AF_RECLAIMED) != 0);
  assert(swept_a->hdr.sweep_epoch == sweep_cycle);
  assert(!lj_gc2_sweep_pending(g));
  idle0 = gc2_worker_idle_declares_acq(g);
  assert(lj_gc2_worker_drain(g, 1) == 0);
  assert(gc2_worker_idle_declares_acq(g) == idle0 + 1u);
  assert(gc2_worker_active_acq(g) == 0);

  lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  lj_gc2_cycle_to_idle(g);
  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra_tg);
  lua_close(L);
}

static void test_minor_sweep_identity_direct(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg, extra_tg;
  uint64_t minor_arenas0;
  uint32_t sweep_cycle, i;
  void *dead, *live;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  lua_gc(L, LUA_GCCOLLECT, 0);

  lj_tg_init_thread(g, &extra_tg, NULL, 1);
  extra_tg.tid = tg->tid + 3500u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  lj_native_enter(&extra_tg);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);

  extra_tg.alloc.alloc_black = 0;
  dead = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			LJ_AF_TRAVERSABLE);
  extra_tg.alloc.alloc_black = 1;
  live = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			LJ_AF_TRAVERSABLE);
  extra_tg.alloc.alloc_black = 0;
  assert(dead != NULL);
  assert(live != NULL);
  assert(lj_arena_of(dead) == lj_arena_of(live));
  assert(ptr_state(dead) == 2);
  assert(ptr_state(live) == 3);

  g->gc2.cycle++;
  sweep_cycle = g->gc2.cycle;
  (void)test_publish_sweep_phase(g);
  gc2_sweep_bridge_ready_store_rlx(g, 0);
  la_store32_rel(&g->gc2.cycle_minor_requested, 1);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  la_store32_rel(&g->gc2.cycle_sweep_minor, 1);
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL);
  /* Opaque raw storage is intentionally retained unless its owner publishes
  ** physical destruction. Model that terminal publication explicitly; this
  ** fixture is testing minor bitmap identity, not GC-header classification. */
  assert(lj_arena_sweep_state_cas(lj_arena_of(dead),
				  lj_arena_cellof(dead),
				  LJ_ARENA_SWEEP_WHITE,
				  LJ_ARENA_SWEEP_FREEING));
  minor_arenas0 = gc2_minor_sweep_arenas_acq(g);
  assert(lj_gc2_test_sweep_owner_progress(g, &extra_tg, 1) == 0);
  assert(gc2_minor_sweep_arenas_acq(g) == minor_arenas0);
  lj_gc2_sweep_bridge_ready(g);
  for (i = 0; i < 256u &&
	      gc2_minor_sweep_arenas_acq(g) == minor_arenas0; i++)
    (void)lj_gc2_test_sweep_owner_progress(g, &extra_tg, 1);
  assert(i != 0 && i < 256u);
  assert(gc2_minor_sweep_arenas_acq(g) == minor_arenas0 + 1u);
  assert(ptr_state(dead) == 1);
  assert(ptr_state(live) == 3);
  assert(lj_arena_of(live)->hdr.sweep_epoch == sweep_cycle);

  lj_gc2_cycle_to_idle(g);
  la_store32_rel(&g->gc2.cycle_minor_requested, 0);
  la_store32_rel(&g->gc2.cycle_sweep_minor, 0);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 0);
  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra_tg);
  lua_close(L);
}

static void test_boundary_lazy_sweep(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCRef empty;
  MSize oldstepmul;
  uint64_t arenas0, delta;
  uint32_t oldcycle, i;
  const uint32_t seeded = LJ_GC2_SWEEP_BATCH + 3u;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  lua_gc(L, LUA_GCCOLLECT, 0);

  oldcycle = g->gc2.cycle;
  g->gc2.cycle = oldcycle + 1u;
  (void)test_publish_sweep_phase(g);
  tg->alloc.sweep_epoch = g->gc2.cycle;  /* Simulate prepared boundary. */
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);

  seed_traversable_needsweep(tg, seeded);
  assert(lj_gc2_sweep_pending(g));

  setgcrefnull(empty);
  setmref(g->gc.sweep, &empty);
  g->gc.state = GCSsweep;
  oldstepmul = g->gc.stepmul;
  g->gc.stepmul = 1;
  arenas0 = gc2_sweep_owner_arenas_acq(g);

  (void)lj_gc_step(L);
  delta = gc2_sweep_owner_arenas_acq(g) - arenas0;
  assert(delta > 0);
  assert(delta <= LJ_GC2_SWEEP_BATCH);
  assert(g->gc.state == GCSsweep);
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL ||
	 tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] != NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(lj_gc2_sweep_pending(g));

  for (i = 0; i < seeded + 4u && g->gc.state != GCSpause; i++)
    (void)lj_gc_step(L);
  assert(g->gc.state == GCSpause);
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(!lj_gc2_sweep_pending(g));
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.stepmul = oldstepmul;
  lua_close(L);
}

static void test_boundary_lazy_sweep_extra_tg(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg, extra_tg;
  GCRef empty;
  MSize oldstepmul;
  uint64_t arenas0, delta;
  uint32_t oldcycle, sweep_cycle, i;
  void *extra_plain, *extra_trav;
  GCArena *extra_plain_a, *extra_trav_a;
  const uint32_t seeded = LJ_GC2_SWEEP_BATCH + 2u;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  lua_gc(L, LUA_GCCOLLECT, 0);

  lj_tg_init_thread(g, &extra_tg, NULL, 1);
  extra_tg.tid = tg->tid + 2000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  lj_native_enter(&extra_tg);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);

  extra_plain = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64, 0);
  extra_trav = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			      LJ_AF_TRAVERSABLE);
  assert(extra_plain != NULL);
  assert(extra_trav != NULL);
  extra_plain_a = lj_arena_of(extra_plain);
  extra_trav_a = lj_arena_of(extra_trav);
  assert(extra_plain_a->hdr.owner_tid == extra_tg.alloc.owner_tid);
  assert(extra_trav_a->hdr.owner_tid == extra_tg.alloc.owner_tid);

  oldcycle = g->gc2.cycle;
  g->gc2.cycle = oldcycle + 1u;
  sweep_cycle = g->gc2.cycle;
  (void)test_publish_sweep_phase(g);
  assert(lj_gc2_handshake(g, LJ_GC2_HS_RESET_ALLOC) == 2);
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN],
			     extra_plain_a));
  assert(arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     extra_trav_a));
  assert((extra_plain_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((extra_trav_a->hdr.flags & LJ_AF_NEEDSWEEP) != 0);
  seed_traversable_needsweep(&extra_tg, seeded);
  assert(!lj_gc2_sweep_needs_prepare(g));
  assert(lj_gc2_sweep_pending(g));

  setgcrefnull(empty);
  setmref(g->gc.sweep, &empty);
  g->gc.state = GCSsweep;
  oldstepmul = g->gc.stepmul;
  g->gc.stepmul = 1;
  arenas0 = gc2_sweep_owner_arenas_acq(g);

  (void)lj_gc_step(L);
  delta = gc2_sweep_owner_arenas_acq(g) - arenas0;
  assert(delta > 0);
  assert(delta <= LJ_GC2_SWEEP_BATCH);
  assert(g->gc.state == GCSsweep);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL ||
	 extra_tg.alloc.quarantine[LJ_ARENAK_TRAVERSABLE] != NULL);
  assert(lj_gc2_sweep_pending(g));

  for (i = 0; i < seeded + 4u && g->gc.state != GCSpause; i++)
    (void)lj_gc_step(L);
  assert(g->gc.state == GCSpause);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(!lj_gc2_sweep_needs_prepare(g));
  assert(!lj_gc2_sweep_pending(g));
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN],
			     extra_plain_a));
  assert(arena_list_contains(lj_arena_alloc_reclaimed_head(
		&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE), extra_trav_a));
  assert((extra_plain_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((extra_trav_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert(extra_trav_a->hdr.sweep_epoch == sweep_cycle);
  assert(lj_tg_in_native_acq(&extra_tg) == 1);

  g->gc.stepmul = oldstepmul;
  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra_tg);
  lua_close(L);
}

static void test_sweep_to_idle_worker_active(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCRef empty;
  uint64_t sweep_to_idle0;
  uint32_t i;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  lua_gc(L, LUA_GCCOLLECT, 0);

  g->gc2.cycle++;
  (void)test_publish_sweep_phase(g);
  tg->alloc.sweep_epoch = g->gc2.cycle;
  setgcrefnull(empty);
  setmref(g->gc.sweep, &empty);
  g->gc.state = GCSsweep;
  sweep_to_idle0 = gc2_sweep_to_idle_acq(g);

  gc2_worker_active_rel(g, 1);
  (void)lj_gc_step(L);
  assert(g->gc.state == GCSsweep);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_SWEEP);
  assert(gc2_sweep_to_idle_acq(g) == sweep_to_idle0);

  gc2_worker_active_rel(g, 0);
  /* Reclaimed arenas from the setup collection now correctly participate in
  ** this synthetic next sweep. Let the bounded owner finish those batches;
  ** the property under test is that worker_active prevented the transition,
  ** not that an otherwise-ready transition always fits in one GC step. */
  for (i = 0; i < 1024u && g->gc.state != GCSpause; i++)
    (void)lj_gc_step(L);
  assert(g->gc.state == GCSpause);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_IDLE);
  assert(gc2_sweep_to_idle_acq(g) == sweep_to_idle0 + 1u);

  setmref(g->gc.sweep, &g->gc.root);
  lua_close(L);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  TValue *tv;
  GCtab *keep, *arrtab, *deadtab, *deadcolo, *deadsplit;
  GCfunc *fn, *deadchunk, *deadfn, *livefn, *deadcf, *finchunk, *finfn;
  GCfunc *bcfn, *hugefn, *uvfn;
  GCupval *deaduv;
  GCproto *deadpt, *deadfnpt;
  GCproto *bcpt, *hugept, *finpt;
  GCSize before_tab, deadtab_size, deadarr_size, deadnode_size;
  GCSize before_colo, deadcolo_size;
  GCSize before_split, deadsplit_size, splitarr_size, splitnode_size;
  GCSize before_fn, deadfn_size, before_drop, deadpt_size, deadchunk_size;
  GCSize before_raw, before_cf, deadcf_size;
  GCSize before_bc, bcfn_size, bcpt_size, before_huge, hugefn_size;
  GCSize hugept_size;
  GCSize before_fin, finpt_size, finchunk_size, finfn_size;
  GCSize before_uv, uvfn_size, uv_size;
  uint64_t sweep_owner_runs0, sweep_owner_arenas0, sweep_owner_live0;
  uint64_t huge_live_bytes;
  uint32_t sweep_epoch0;
  void *raw, *deadarr, *deadnode, *splitarr, *splitnode;
  LJHugeInfo hugehi;
  GCArena *fna, *arra;

  test_selected_lane_cas();
  test_packed_unmarked_classifier();
  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "keep = {}\n"
    "keep.f = function(x) return x + 1 end\n"
    "keep.dead = loadstring('return 42')\n"
    "keep.parent = loadstring('return function(x) return x + 7 end')\n"
    "keep.deadfn = keep.parent()\n"
    "keep.livefn = keep.parent()\n"
    "do\n"
    "  local x = 10\n"
    "  keep.uvdead = function() x = x + 1; return x end\n"
    "end\n"
    "keep.arr = {}\n"
    "for i = 1, 300 do keep.arr[i] = i end\n"
    "keep.deadtab = {}\n"
    "for i = 1, 300 do keep.deadtab[i] = i end\n"
    "for i = 1, 80 do keep.deadtab['k'..i] = i end\n"
    "keep.deadcolo = {10, 20, 30}\n"
    "keep.deadsplit = {1, 2, 3}\n"
    "for i = 4, 80 do keep.deadsplit[i] = i end\n"
    "collectgarbage('collect')\n") == LUA_OK);

  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);
  assert(tg->alloc.sweep_epoch != 0);
  test_packed_unmarked_outer_scan(g, tg);

  lua_getglobal(L, "keep");
  tv = L->top - 1;
  assert(tvistab(tv));
  keep = tabV(tv);
  assert((lj_arena_of(keep)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(keep) == 2);
  assert(lj_arena_of(keep)->hdr.sweep_epoch == tg->alloc.sweep_epoch);

  lua_getfield(L, -1, "f");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  fna = lj_arena_of(fn);
  assert((fna->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(fn) == 2);
  assert(fna->hdr.sweep_epoch == tg->alloc.sweep_epoch);
  L->top--;

  lua_getfield(L, -1, "dead");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  assert(isluafunc(fn));
  deadchunk = fn;
  deadchunk_size = sizeLfunc((MSize)lj_funcL_nupvalues(&deadchunk->l));
  deadpt = funcproto(fn);
  deadpt_size = deadpt->sizept;
  assert((lj_obj_gcflags(obj2gco(deadpt)) &
	  (LJ_GC_FIXED|LJ_GC_SFIXED)) == 0);
  assert(ptr_state(deadchunk) == 2);
  assert(ptr_state(deadpt) == 2);
  L->top--;

  lua_getfield(L, -1, "deadfn");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  deadfn = funcV(tv);
  assert(isluafunc(deadfn));
  deadfn_size = sizeLfunc((MSize)lj_funcL_nupvalues(&deadfn->l));
  deadfnpt = funcproto(deadfn);
  assert(ptr_state(deadfn) == 2);
  assert(lj_arena_root_state_acq(lj_arena_of(deadfn),
    lj_arena_cellof(deadfn)) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(deadfn),
    lj_arena_cellof(deadfn)) == LJ_ARENA_DTOR_LFUNC0);
  assert(ptr_state(deadfnpt) == 2);
  L->top--;

  lua_getfield(L, -1, "livefn");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  livefn = funcV(tv);
  assert(isluafunc(livefn));
  assert(funcproto(livefn) == deadfnpt);
  assert(ptr_state(livefn) == 2);
  L->top--;

  lua_getfield(L, -1, "uvdead");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  uvfn = funcV(tv);
  assert(isluafunc(uvfn));
  assert(lj_funcL_nupvalues(&uvfn->l) == 1);
  uvfn_size = sizeLfunc((MSize)lj_funcL_nupvalues(&uvfn->l));
  deaduv = func_uv_acq(&uvfn->l, 0);
  uv_size = sizeof(GCupval);
  assert(deaduv->closed);
  assert(uvval(deaduv) == &deaduv->tv);
  assert((lj_arena_of(deaduv)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(uvfn) == 2);
  assert(ptr_state(deaduv) == 2);
  assert(lj_arena_root_state_acq(lj_arena_of(uvfn),
    lj_arena_cellof(uvfn)) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(uvfn),
    lj_arena_cellof(uvfn)) == LJ_ARENA_DTOR_LFUNC1);
  assert(lj_arena_root_state_acq(lj_arena_of(deaduv),
    lj_arena_cellof(deaduv)) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(deaduv),
    lj_arena_cellof(deaduv)) == LJ_ARENA_DTOR_CLOSED_UV);
  L->top--;

  lua_getfield(L, -1, "arr");
  tv = L->top - 1;
  assert(tvistab(tv));
  arrtab = tabV(tv);
  assert(arrtab->asize > 0);
  assert(arrtab->colo <= 0);
  arra = lj_arena_of(lj_tab_array_hdrw(lj_tab_array_acq(arrtab)));
  assert((arra->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
  assert(arra->hdr.sweep_epoch == 0);
  assert(ptr_state(lj_tab_array_hdrw(lj_tab_array_acq(arrtab))) == 3);
  L->top--;

  lua_getfield(L, -1, "deadtab");
  tv = L->top - 1;
  assert(tvistab(tv));
  deadtab = tabV(tv);
  assert(deadtab->asize > 0);
  assert(deadtab->hmask > 0);
  assert(deadtab->colo <= 0);
  deadtab_size = sizeof(GCtab);
  deadarr = lj_tab_array_hdrw(lj_tab_array_acq(deadtab));
  deadarr_size = lj_tab_array_bytes(deadtab->acap);
  deadnode = lj_tab_node_hdrw(lj_tab_node_acq(deadtab));
  deadnode_size = lj_tab_node_bytes(deadtab->hmask);
  assert((lj_arena_of(deadtab)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert((lj_arena_of(deadarr)->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
  assert((lj_arena_of(deadnode)->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
  assert(ptr_state(deadtab) == 2);
  assert(ptr_state(deadarr) == 3);
  assert(ptr_state(deadnode) == 3);
  L->top--;

  sweep_owner_runs0 = gc2_sweep_owner_runs_acq(g);
  sweep_owner_arenas0 = gc2_sweep_owner_arenas_acq(g);
  sweep_owner_live0 = gc2_sweep_owner_live_cells_acq(g);
  sweep_epoch0 = tg->alloc.sweep_epoch;
  before_tab = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadtab");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_sweep_owner_runs_acq(g) > sweep_owner_runs0);
  assert(gc2_sweep_owner_arenas_acq(g) > sweep_owner_arenas0);
  assert(gc2_sweep_owner_live_cells_acq(g) >= sweep_owner_live0);
  assert(tg->alloc.sweep_epoch > sweep_epoch0);
  assert(g->gc.total <=
	 before_tab - deadtab_size - deadarr_size - deadnode_size);
  assert((ptr_state(deadtab) & 2u) == 0);
  assert((ptr_state(deadarr) & 2u) == 0);
  assert((ptr_state(deadnode) & 2u) == 0);

  lua_getfield(L, -1, "deadcolo");
  tv = L->top - 1;
  assert(tvistab(tv));
  deadcolo = tabV(tv);
  assert(deadcolo->asize > 0);
  assert(deadcolo->hmask == 0);
  assert(deadcolo->colo > 0);
  deadcolo_size = sizetabcolo((uint32_t)deadcolo->colo & 0x7f);
  assert((lj_arena_of(deadcolo)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(deadcolo) == 2);
  L->top--;

  before_colo = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadcolo");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_colo - deadcolo_size);
  assert((ptr_state(deadcolo) & 2u) == 0);

  lua_getfield(L, -1, "deadsplit");
  tv = L->top - 1;
  assert(tvistab(tv));
  deadsplit = tabV(tv);
  assert(deadsplit->asize > 0);
  assert(deadsplit->colo < 0);
  assert(deadsplit->asize > lj_tab_colo_size(deadsplit));
  assert(lj_tab_colo_size(deadsplit) != 0);
  deadsplit_size = sizetabcolo(lj_tab_colo_size(deadsplit));
  splitarr = lj_tab_array_hdrw(lj_tab_array_acq(deadsplit));
  splitarr_size = lj_tab_array_bytes(deadsplit->acap);
  splitnode_size = deadsplit->hmask > 0 ?
		   lj_tab_node_bytes(deadsplit->hmask) : 0;
  splitnode = deadsplit->hmask > 0 ?
	      (void *)lj_tab_node_hdrw(lj_tab_node_acq(deadsplit)) : NULL;
  assert((lj_arena_of(deadsplit)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert((lj_arena_of(splitarr)->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
  assert(ptr_state(deadsplit) == 2);
  assert(ptr_state(splitarr) == 3);
  if (splitnode) {
    assert((lj_arena_of(splitnode)->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
    assert(ptr_state(splitnode) == 3);
  }
  L->top--;

  before_split = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadsplit");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <=
	 before_split - deadsplit_size - splitarr_size - splitnode_size);
  assert((ptr_state(deadsplit) & 2u) == 0);
  assert((ptr_state(splitarr) & 2u) == 0);
  if (splitnode)
    assert((ptr_state(splitnode) & 2u) == 0);

  /* Fixed/SFIXED retention overrides arena destructor identity. Rootless
  ** typed bodies have no ownership-spine entry which could otherwise preserve
  ** this flag, so exercise the post-grace validator directly through a real
  ** collection before allowing the ordinary destructor on the next cycle. */
  lj_obj_addgcflags_atomic(obj2gco(deadfn), LJ_GC_FIXED|LJ_GC_SFIXED);
  lua_pushnil(L);
  lua_setfield(L, -2, "deadfn");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert((ptr_state(deadfn) & 2u) != 0);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(deadfn),
    lj_arena_cellof(deadfn)) == LJ_ARENA_DTOR_LFUNC0);
  assert((lj_obj_gcflags(obj2gco(deadfn)) &
	  (LJ_GC_FIXED|LJ_GC_SFIXED)) == (LJ_GC_FIXED|LJ_GC_SFIXED));
  lj_obj_cleargcflags_atomic(obj2gco(deadfn), LJ_GC_FIXED|LJ_GC_SFIXED);

  before_fn = g->gc.total;
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_fn - deadfn_size);
  assert((ptr_state(deadfn) & 2u) == 0);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(deadfn),
    lj_arena_cellof(deadfn)) == LJ_ARENA_DTOR_NONE);
  assert(ptr_state(deadfnpt) == 2);
  assert(ptr_state(livefn) == 2);

  /* A cdata sidecar disagreement must likewise pin a rootless typed object
  ** without selecting either destructor family. The independently described
  ** closure can still be reclaimed in the first cycle; clearing the injected
  ** disagreement admits the closed-upvalue destructor in the next one. */
  lj_arena_bm_set(lj_arena_of(deaduv)->cdata, lj_arena_cellof(deaduv));
  before_uv = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "uvdead");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_uv - uvfn_size);
  assert((ptr_state(uvfn) & 2u) == 0);
  assert((ptr_state(deaduv) & 2u) != 0);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(uvfn),
    lj_arena_cellof(uvfn)) == LJ_ARENA_DTOR_NONE);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(deaduv),
    lj_arena_cellof(deaduv)) == LJ_ARENA_DTOR_CLOSED_UV);
  assert(lj_arena_cdata_get(lj_arena_of(deaduv),
    lj_arena_cellof(deaduv)) == 1u);
  lj_arena_bm_clear(lj_arena_of(deaduv)->cdata, lj_arena_cellof(deaduv));

  before_uv = g->gc.total;
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_uv - uv_size);
  assert((ptr_state(deaduv) & 2u) == 0);
  assert(lj_arena_dtor_kind_acq(lj_arena_of(deaduv),
    lj_arena_cellof(deaduv)) == LJ_ARENA_DTOR_NONE);

  before_raw = g->gc.total;
  raw = lj_mem_newgco_raw(L, 64, LJ_AF_TRAVERSABLE);
  /* Raw allocator storage has malloc semantics and may come from a reclaimed
  ** arena, so establish the header value this deferral check expects. */
  memset(raw, 0, 64);
  assert(g->gc.total == before_raw + 64);
  assert(ptr_state(raw) == 2);
  assert(lj_gc2_markmem(g, raw) == 1);
  assert(ptr_state(raw) == 3);
  assert(lj_mem_freegco_defer(g, raw, 64) == 1);
  assert(g->gc.total == before_raw);
  assert(((GCobj *)raw)->gch.gct == 0);
  assert(ptr_state(raw) == 2);

  before_drop = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "dead");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_drop - deadpt_size - deadchunk_size);
  assert((ptr_state(deadchunk) & 2u) == 0);
  assert((ptr_state(deadpt) & 2u) == 0);
  assert(ptr_state(raw) == 1);

  lua_getfield(L, -1, "arr");
  lua_pushcclosure(L, noop_finalizer, 1);
  tv = L->top - 1;
  assert(tvisfunc(tv));
  deadcf = funcV(tv);
  assert(!isluafunc(deadcf));
  assert(lj_funcC_nupvalues(&deadcf->c) == 1);
  deadcf_size = sizeCfunc((MSize)lj_funcC_nupvalues(&deadcf->c));
  assert(tvistab(&deadcf->c.upvalue[0]));
  assert(tabV(&deadcf->c.upvalue[0]) == arrtab);
  assert((lj_arena_of(deadcf)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(deadcf) == 2);
  lua_setfield(L, -2, "deadcf");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(ptr_state(deadcf) == 2);

  before_cf = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadcf");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_cf - deadcf_size);
  assert((ptr_state(deadcf) & 2u) == 0);
  assert(ptr_state(arrtab) == 2);

  assert(luaL_dostring(L,
    "do\n"
    "  local f = assert(loadstring('return function(y) return y * 9 end'))()\n"
    "  keep.bcblob = string.dump(f)\n"
    "  keep.bcdead = assert(loadstring(keep.bcblob))\n"
    "end\n"
    "collectgarbage('collect')\n") == LUA_OK);
  lua_getfield(L, -1, "bcdead");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  bcfn = funcV(tv);
  assert(isluafunc(bcfn));
  bcfn_size = sizeLfunc((MSize)lj_funcL_nupvalues(&bcfn->l));
  bcpt = funcproto(bcfn);
  bcpt_size = bcpt->sizept;
  assert((lj_obj_gcflags(obj2gco(bcpt)) &
	  (LJ_GC_FIXED|LJ_GC_SFIXED)) == 0);
  assert((lj_arena_of(bcpt)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(bcfn) == 2);
  assert(ptr_state(bcpt) == 2);
  L->top--;

  before_bc = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "bcdead");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_bc - bcpt_size - bcfn_size);
  assert((ptr_state(bcfn) & 2u) == 0);
  assert((ptr_state(bcpt) & 2u) == 0);

  assert(luaL_dostring(L,
    "do\n"
    "  local t = {'return function() local x = 0\\n'}\n"
    "  for i = 1, 6000 do t[#t+1] = 'x = x + 1\\n' end\n"
    "  t[#t+1] = 'return x end'\n"
    "  keep.huge = assert(loadstring(table.concat(t)))()\n"
    "end\n"
    "collectgarbage('collect')\n") == LUA_OK);
  lua_getfield(L, -1, "huge");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  hugefn = funcV(tv);
  assert(isluafunc(hugefn));
  hugefn_size = sizeLfunc((MSize)lj_funcL_nupvalues(&hugefn->l));
  hugept = funcproto(hugefn);
  hugept_size = hugept->sizept;
  assert(hugept_size > LJ_HUGE_THRESHOLD);
  assert(lj_arena_ishuge(lj_arena_of(hugept)));
  assert(lj_arena_hugetab_lookup(&tg->huge, hugept, &hugehi) == 1);
  assert(hugehi.size == hugept_size);
  assert((hugehi.flags & LJ_HUGEF_TRAVERSABLE) != 0);
  /* A completed nongenerational major clears survivor marks for both small
  ** arenas and huge-table entries; liveness is republished next cycle. */
  assert((hugehi.flags & LJ_HUGEF_MARK) == 0);
  huge_live_bytes = la_load64_acq(&g->gc2.sweep_live_huge_bytes);
  assert(huge_live_bytes >= hugept_size);
  assert(la_load64_acq(&g->gc2.live_estimate) >= huge_live_bytes);
  assert(ptr_state(hugefn) == 2);
  L->top--;

  before_huge = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "huge");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_huge - hugept_size - hugefn_size);
  assert((ptr_state(hugefn) & 2u) == 0);
  assert(lj_arena_hugetab_lookup(&tg->huge, hugept, NULL) == 0);

  assert(luaL_dostring(L,
    "keep.deadfin = loadstring('return 43')\n"
    "keep.deadfinfn = keep.parent()\n") ==
	 LUA_OK);
  lua_getfield(L, -1, "deadfin");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  assert(isluafunc(fn));
  finchunk = fn;
  finchunk_size = sizeLfunc((MSize)lj_funcL_nupvalues(&finchunk->l));
  finpt = funcproto(fn);
  finpt_size = finpt->sizept;
  assert(ptr_state(finchunk) == 2);
  assert(ptr_state(finpt) == 2);
  L->top--;

  lua_getfield(L, -1, "deadfinfn");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  finfn = funcV(tv);
  assert(isluafunc(finfn));
  finfn_size = sizeLfunc((MSize)lj_funcL_nupvalues(&finfn->l));
  assert(funcproto(finfn) == deadfnpt);
  assert(ptr_state(finfn) == 2);
  L->top--;

  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, noop_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_setfield(L, -2, "ud");

  before_fin = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadfin");
  lua_pushnil(L);
  lua_setfield(L, -2, "deadfinfn");
  lua_pushnil(L);
  lua_setfield(L, -2, "ud");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_fin - finpt_size - finchunk_size - finfn_size);
  assert((ptr_state(finchunk) & 2u) == 0);
  assert((ptr_state(finfn) & 2u) == 0);
  assert((ptr_state(finpt) & 2u) == 0);

  lua_close(L);
  test_reclaim_noop_word_summary();
  test_prepare_collision_detaches_allocator();
  test_preserve_abort_waits_for_restore_publisher();
  test_quarantine_late_live_after_eof();
  test_sweep_to_idle_worker_active();
  test_worker_owned_sweep_direct();
  test_minor_sweep_identity_direct();
  test_boundary_lazy_sweep();
  test_boundary_lazy_sweep_extra_tg();
  test_typed_dtor_pregrace_exclusive();
  test_typed_dtor_gc2_token_veto();
  test_typed_dtor_gc2_desc_exact_veto();
  test_typed_dtor_gc2_desc_unrelated_progress();
  test_typed_dtor_gc2_desc_pinned_veto();
  test_typed_dtor_gc2_desc_malformed_veto();
  test_typed_dtor_dense_batch(5u);
  test_typed_dtor_dense_batch(6u);
  test_typed_dtor_no_adjacency_match();
  test_typed_dtor_batch_body_reject(TYPED_BATCH_REJECT_FIXED);
  test_typed_dtor_batch_body_reject(TYPED_BATCH_REJECT_HEADER);
  test_typed_dtor_batch_extent_reject();
  test_typed_dtor_batch_recovery_cancel();
  test_typed_dtor_denied_capability_retires(0);
  test_typed_dtor_denied_capability_retires(1);
  printf("t-arena-gcsweep OK: traversable runtime sweep bridge verified\n");
  return 0;
}
