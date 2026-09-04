/*
** Arena heap bitmap scaffolding.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_arena_c
#define LUA_CORE

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_prng.h"
#include "lj_tg.h"
#include "lj_thr.h"

#if !defined(_WIN32)
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

#define LJ_ARENA_MMAP_PROBE_MAX		30
#define LJ_ARENA_MMAP_PROBE_LINEAR	5
#define LJ_ARENA_MMAP_LOWER		((uintptr_t)0x4000)
#define LJ_ARENA_ADDR_LIMIT		((uintptr_t)1 << 47)
#define LJ_HUGETAB_MAX_BITS		26
#define LJ_HUGETAB_SIZE_SHIFT		16
#define LJ_HUGETAB_SIZE_MASK \
  (UINT64_C(0xffffffff) << LJ_HUGETAB_SIZE_SHIFT)
#define LJ_HUGETAB_READER_SHIFT	48
#define LJ_HUGETAB_READER_ONE \
  (UINT64_C(1) << LJ_HUGETAB_READER_SHIFT)
#define LJ_HUGETAB_READER_MASK \
  (UINT64_C(0xffff) << LJ_HUGETAB_READER_SHIFT)
/* Reserve the all-ones reader encoding for a destructive admission close.
** Ordinary readers saturate one step earlier. This keeps the close in the
** same full-slot CAS word as reader admission instead of overloading BUSY,
** through which stable retire-ticket readers are intentionally admitted. */
#define LJ_HUGETAB_READER_MAX		LJ_ARENA_HUGE_READER_MAX
#define LJ_HUGETAB_READER_CLOSED	0xffffu
#define LJ_HUGETAB_ADMISSION_CLOSED \
  ((uint64_t)LJ_HUGETAB_READER_CLOSED << LJ_HUGETAB_READER_SHIFT)
#define LJ_HUGETAB_META_SHIFT		LJ_HUGETAB_SIZE_SHIFT
#define LJ_HUGETAB_META_MASK \
  (((uint64_t)1 << LJ_HUGETAB_META_SHIFT) - 1u)
#define LJ_HUGETAB_EMPTY		((uint64_t)0)
#define LJ_HUGETAB_TOMBSTONE		((uint64_t)1)

LJ_STATIC_ASSERT(LJ_HUGEF_MASK == LJ_HUGETAB_META_MASK);
LJ_STATIC_ASSERT(LJ_HUGETAB_ADMISSION_CLOSED == LJ_HUGETAB_READER_MASK);

#if defined(LJ_ARENA_TEST_HELPERS)
static void arena_test_plain_claim_pause_after_close(void);
static void arena_test_plain_admit_pause_after_enter(void);
static void arena_test_remote_publish_pause_after_queue(void);
static void arena_test_remote_drain_pause_after_clear(void);
static void arena_test_open_sealed_pause_before_cas(GCArena *a);
static int arena_test_gc2_sidecar_alloc_fails(void);
static void hugetab_test_realloc_pause_after_busy(void);
static void hugetab_test_admission_close_pause_after_snapshot(void);
static void hugetab_test_admission_close_pause_after_close(void);
#define arena_test_remote_fast_skip() \
  ((void)la_add64_rlx(&arena_test_remote_fast_skip_count, 1))
#define arena_test_remote_arena_probe() \
  ((void)la_add64_rlx(&arena_test_remote_arena_probe_count, 1))
#else
#define arena_test_plain_claim_pause_after_close() ((void)0)
#define arena_test_plain_admit_pause_after_enter() ((void)0)
#define arena_test_remote_publish_pause_after_queue() ((void)0)
#define arena_test_remote_drain_pause_after_clear() ((void)0)
#define arena_test_open_sealed_pause_before_cas(a) ((void)0)
#define arena_test_remote_fast_skip() ((void)0)
#define arena_test_remote_arena_probe() ((void)0)
#define arena_test_gc2_sidecar_alloc_fails() 0
#define hugetab_test_admission_close_pause_after_snapshot() ((void)0)
#define hugetab_test_admission_close_pause_after_close() ((void)0)
#endif

static LJ_AINLINE uint64_t arena_remote_count(uint64_t active)
{
  return active & LJ_ARENA_REMOTE_COUNT_MASK;
}

static LJ_AINLINE int arena_terminal_closed_acq(const GCArena *a)
{
  return a && la_load32_acq(&a->hdr.terminal_closed) != 0;
}

/* A destructive owner samples the admission generation only after acquiring
** the object's DESTRUCT lane. An OPEN local owner has no admission, while an
** intrusive remote-free publisher accounts for exactly its own admission.
** Readers entering afterward must observe DESTRUCT before touching bytes. */
static LJ_AINLINE int arena_mutation_open_quiet(const GCArena *a,
						 uint64_t own_count)
{
  /* No-both-miss litmus with rescue readers: reader admission RMW; SC fence;
  ** lifetime acquire. Writer lifetime CAS; SC fence; admission acquire. Thus
  ** either the writer counts a preexisting reader or the reader observes a
  ** non-readable lifetime state before body access, independent of acq/rel
  ** timing. */
  la_fence_seq();
  return a && lj_arena_remote_active_acq(a) == own_count;
}

/* Sweep owns a closed/sealed generation. State bits are expected, but a
** pending intent or admitted reader defeats destructive ownership. */
static LJ_AINLINE int arena_mutation_closed_quiet(const GCArena *a)
{
  uint64_t active;
  la_fence_seq();  /* Paired with rescue admission -> SC fence -> lifetime. */
  active = a ? lj_arena_remote_active_acq(a) : 0;
  return a && arena_remote_count(active) == 0 &&
    !(active & LJ_ARENA_REMOTE_PENDING);
}

static LJ_NORET void arena_remote_overflow(void)
{
  /* Every live admission requires a distinct executing context and nonzero
  ** address-space-backed state. Supported x64 processes cannot host 2^61 of
  ** them. Treat a violated platform invariant as fatal instead of dropping a
  ** lifetime intent or inventing an uncoordinated poison state. */
  abort();
}

/* Bind both pointers while the arena is private or owned by a quiescent
** transfer source. The descriptor is published first, so any observer which
** acquires progress_g also has a stable reclamation authority. Arena transfer
** is only valid within one global state; rebinding would make a dormant
** descriptor from the old global impossible to classify safely. */
static void arena_progress_bind_rel(GCArena *a, global_State *g)
{
  global_State *oldg;
  LJGC2TableDesc *desc, *olddesc;
  if (!a || !g)
    return;
  desc = &g->gc2.table_rescan_desc;
  oldg = (global_State *)lj_arena_progress_g_acq(a);
  olddesc = lj_arena_gc2_tabledesc_acq(a);
  if ((oldg && oldg != g) || (olddesc && olddesc != desc)) {
    lj_assertX(0, "arena table descriptor rebound across global states");
    abort();
  }
  if (!olddesc)
    lj_arena_gc2_tabledesc_rel(a, desc);
  if (!oldg)
    lj_arena_progress_g_rel(a, g);
}

static void arena_progress_wake(GCArena *a)
{
  global_State *g = a ? (global_State *)lj_arena_progress_g_acq(a) : NULL;
  if (g)
    lj_gc2_sweep_publish_wake(g);
}

void lj_arena_recovery_complete_wake(GCArena *a)
{
  arena_progress_wake(a);
}

static void arena_late_clear_committed_free(GCArena *a)
{
  uint32_t w;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t block = la_load64_acq(&a->block[w]);
    (void)la_and64_rlx(&a->late[w], block);
  }
}

static uint32_t arena_recovery_word_bits(uint64_t states)
{
  uint64_t occupied = (states | (states >> 1)) &
    UINT64_C(0x5555555555555555);
  uint32_t bits = 0;
  while (occupied) {
    uint32_t pair = (uint32_t)(lj_ffs64(occupied) >> 1);
    bits |= (uint32_t)1u << pair;
    occupied &= occupied - 1u;
  }
  return bits;
}

/* Convert two packed recovery words to the allocation-bit geometry used by
** block[]/mark[]. The zero fast path makes the normal no-recovery sweep pay
** only two acquire loads and two predictable branches per bitmap word. */
static uint64_t arena_recovery_block_bits(const GCArena *a, uint32_t w)
{
  uint64_t lo, hi;
  if (!a || w >= LJ_ARENA_WORDS)
    return 0;
  lo = la_load64_acq(&a->recovery[w << 1]);
  hi = la_load64_acq(&a->recovery[(w << 1) + 1u]);
  return (uint64_t)arena_recovery_word_bits(lo) |
    ((uint64_t)arena_recovery_word_bits(hi) << 32);
}

/* Convert the persistent root-membership plane to allocation-bit geometry.
** Any transient or committed root state pins the allocation start until its
** owner explicitly returns it to NONE. */
static uint64_t arena_root_block_bits(const GCArena *a, uint32_t w)
{
  uint64_t lo, hi;
  if (!a || w >= LJ_ARENA_WORDS)
    return 0;
  lo = la_load64_acq(&a->root[w << 1]);
  hi = la_load64_acq(&a->root[(w << 1) + 1u]);
  return (uint64_t)arena_recovery_word_bits(lo) |
    ((uint64_t)arena_recovery_word_bits(hi) << 32);
}

/* A nonzero destructor class is an authoritative typed allocation identity.
** Generic bitmap sweep cannot run its semantic destructor, so every such
** start remains pinned until GC2 explicitly classifies and destroys it. */
static uint64_t arena_dtor_block_bits(const GCArena *a, uint32_t w)
{
  uint32_t plane;
  uint64_t bits = 0;
  if (!a || w >= LJ_ARENA_WORDS)
    return 0;
  for (plane = 0; plane < LJ_ARENA_DTOR_PLANES; plane++)
    bits |= la_load64_acq(&a->dtor[plane][w]);
  return bits;
}

static int arena_dtor_empty(const GCArena *a)
{
  uint32_t plane, w;
  if (!a)
    return 1;
  for (plane = 0; plane < LJ_ARENA_DTOR_PLANES; plane++)
    for (w = 0; w < LJ_ARENA_WORDS; w++)
      if (la_load64_acq(&a->dtor[plane][w]) != 0)
	return 0;
  return 1;
}

/* Called only after lifetime FREE and/or block removal gives the structural
** owner exclusive reuse authority. Keeping this after the discovery clear
** ensures no reader can observe a still-discoverable typed body as raw. */
static void arena_dtor_clear_mask_rlx(GCArena *a, uint32_t w, uint64_t mask)
{
  uint32_t plane;
  for (plane = 0; plane < LJ_ARENA_DTOR_PLANES; plane++)
    (void)la_and64_rlx(&a->dtor[plane][w], ~mask);
}

/* Convert non-FREE allocation-lifetime lanes to block geometry. This is not
** semantic liveness: it only prevents a free-run scan from crossing a start
** whose bytes or publication metadata still have an owner. */
static uint64_t arena_lifetime_block_bits(const GCArena *a, uint32_t w)
{
  uint64_t bits = 0;
  uint32_t k;
  if (!a || w >= LJ_ARENA_WORDS)
    return 0;
  for (k = 0; k < 4u; k++) {
    uint64_t states = la_load64_acq(&a->lifetime[(w << 2) + k]);
    uint64_t occupied = (states | (states >> 1) | (states >> 2) |
			 (states >> 3)) & UINT64_C(0x1111111111111111);
    /* Reclaimed arenas are normally sparse or completely FREE. Convert only
    ** actual non-zero nibbles instead of testing all 16 lifetime lanes. */
    while (occupied) {
      uint32_t lane = (uint32_t)(lj_ffs64(occupied) >> 2);
      bits |= (uint64_t)1 << ((k << 4) + lane);
      occupied &= occupied - 1u;
    }
  }
  return bits;
}

static LJ_AINLINE int arena_lifetime_managed(const GCArena *a)
{
  return a && !lj_arena_ishuge(a) &&
    (lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) != 0;
}

/* Callers already retain the small mapping. A missing sidecar on a
** traversable arena violates its pre-publication storage invariant and is
** therefore a conservative owner, not an absent token. */
static LJ_AINLINE LJGC2TabStamp *arena_gc2_cell_stamp_acq(
  const GCArena *a, uint32_t cell)
{
  LJGC2TabStampArena *side;
  if (!arena_lifetime_managed(a) || cell < LJ_AFIRST_CELL ||
      cell >= LJ_ARENA_CELLS)
    return NULL;
  side = lj_arena_gc2_tabstamp_acq(a);
  return side ? &side->cell[cell] : NULL;
}

static LJ_AINLINE int arena_gc2_token_none_acq(const GCArena *a,
                                                uint32_t cell)
{
  return lj_arena_gc2_token_none_acq(a, cell);
}

/* Convert the persistent token sidecar into the same one-bit-per-cell shape
** as the structural bitmaps. This is deliberately a cold-path summary: it is
** used by sweep/rebuild/terminal certificates, while ordinary allocation
** checks only its exact candidate. A missing traversable sidecar fails closed
** for every allocation-capable cell in the word. */
static LJ_AINLINE uint64_t arena_gc2_token_block_bits(const GCArena *a,
					       uint32_t wi)
{
  LJGC2TabStampArena *side;
  uint64_t bits = 0;
  uint32_t lo, hi, cell;
  if (!arena_lifetime_managed(a) || wi >= LJ_ARENA_WORDS)
    return 0;
  lo = wi << 6;
  hi = lo + 64u;
  if (hi <= LJ_AFIRST_CELL)
    return 0;
  if (lo < LJ_AFIRST_CELL)
    lo = LJ_AFIRST_CELL;
  side = lj_arena_gc2_tabstamp_acq(a);
  if (!side) {
    uint32_t bit = lo & 63u;
    return bit == 0 ? ~(uint64_t)0 : (~(uint64_t)0 << bit);
  }
  for (cell = lo; cell < hi; cell++)
    if (lj_gc2_table_token_state(
	  la_load64_acq(&side->cell[cell].token.control)) !=
	LJ_GC2_TABLE_TOKEN_NONE)
      bits |= (uint64_t)1 << (cell & 63u);
  return bits;
}

/* One exact descriptor observation converted to allocation-bit geometry.
** IDLE and an ACTIVE table outside this word contribute no bit. PINNED,
** malformed, or a published-global/missing-descriptor mismatch are global
** no-reclaim authority and therefore conservatively fill the complete word. */
static LJ_AINLINE uint64_t arena_gc2_desc_block_bits(const GCArena *a,
					      uint32_t wi)
{
  LJGC2TableDesc *desc;
  LJGC2TableDescSnap snap;
  uint32_t cell;
  if (!arena_lifetime_managed(a) || wi >= LJ_ARENA_WORDS)
    return 0;
  desc = lj_arena_gc2_tabledesc_acq(a);
  if (!desc)
    return lj_arena_progress_g_acq(a) == NULL ? 0 : ~(uint64_t)0;
  snap = lj_gc2_tabledesc_snapshot(desc);
  if (snap.state == LJ_GC2_TABLEDESC_IDLE)
    return 0;
  if (snap.state != LJ_GC2_TABLEDESC_ACTIVE)
    return ~(uint64_t)0;
  if (lj_arena_of((const void *)snap.table) != a)
    return 0;
  cell = lj_arena_cellof((const void *)snap.table);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS)
    return ~(uint64_t)0;
  return (cell >> 6) == wi ? (uint64_t)1 << (cell & 63u) : 0;
}

/* The caller has changed FREE to a non-readable construction/mutation lane,
** after proving the old token NONE. Lawful token publishers must acquire the
** reciprocal allocation lifetime before refresh, so the token cannot change
** until this new incarnation is published. Reset only the scan proof: token
** generation is persistent side identity across cell reuse. */
static LJ_AINLINE int arena_gc2_prepare_incarnation(GCArena *a, uint32_t cell)
{
  LJGC2TabStamp *stamp;
  if (!arena_lifetime_managed(a))
    return 1;
  stamp = arena_gc2_cell_stamp_acq(a, cell);
  if (!stamp || lj_gc2_table_token_state(
	la_load64_acq(&stamp->token.control)) != LJ_GC2_TABLE_TOKEN_NONE)
    return 0;
  la_store64_rel(&stamp->state, 0);
  return 1;
}

LJ_STATIC_ASSERT(LJ_ARENA_SWEEP_CELLS_PER_WORD == 32u);
LJ_STATIC_ASSERT(LJ_ARENA_ROOT_CELLS_PER_WORD == 32u);
LJ_STATIC_ASSERT(LJ_ARENA_RECOVERY_CELLS_PER_WORD == 32u);
LJ_STATIC_ASSERT(LJ_ARENA_LIFETIME_CELLS_PER_WORD == 16u);

/* Dilate a portable 32-bit bitmap into the even bits of a 64-bit word. This
** is the scalar Morton-code expansion, not a BMI/PDEP dependency: bit n in x
** becomes bit 2*n in the result on every supported compiler and target. */
static LJ_AINLINE uint64_t arena_dilate32_even(uint32_t x)
{
  uint64_t v = (uint64_t)x;
  v = (v | (v << 16)) & UINT64_C(0x0000ffff0000ffff);
  v = (v | (v << 8)) & UINT64_C(0x00ff00ff00ff00ff);
  v = (v | (v << 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
  v = (v | (v << 2)) & UINT64_C(0x3333333333333333);
  v = (v | (v << 1)) & UINT64_C(0x5555555555555555);
  return v;
}

/* Prove the exact scalar outcome live=0/freeing=block for one bitmap word.
** Every root/recovery lane and every non-FREE lifetime nibble in the complete
** word must be absent. sweep[] must then contain FREEING (11) at precisely
** the block starts and WHITE (00) everywhere else. This deliberately accepts
** block-zero words only when both packed sweep words are exactly zero.
**
** A false negative merely selects the scalar path. Before the clean sealed
** commit this proof authorizes only a readiness-scan skip; any racing publisher
** dirties that generation. arena_quarantine_apply_bitmap() calls it again
** after the clean commit instead of transferring the earlier observation. */
static LJ_AINLINE int arena_quarantine_terminal_freeing_word(
  const GCArena *a, uint32_t w, uint64_t block, int terminal_managed)
{
  uint64_t even, expect_lo, expect_hi;
  uint32_t k;
  if (!a || w >= LJ_ARENA_WORDS || !terminal_managed)
    return 0;
  if (la_load64_acq(&a->root[w << 1]) != 0 ||
      la_load64_acq(&a->root[(w << 1) + 1u]) != 0 ||
      la_load64_acq(&a->recovery[w << 1]) != 0 ||
      la_load64_acq(&a->recovery[(w << 1) + 1u]) != 0)
    return 0;
  for (k = 0; k < 4u; k++)
    if (la_load64_acq(&a->lifetime[(w << 2) + k]) != 0)
      return 0;
  if ((arena_gc2_token_block_bits(a, w) |
       arena_gc2_desc_block_bits(a, w)) != 0)
    return 0;
  even = arena_dilate32_even((uint32_t)block);
  expect_lo = even | (even << 1);
  even = arena_dilate32_even((uint32_t)(block >> 32));
  expect_hi = even | (even << 1);
  return la_load64_acq(&a->sweep[w << 1]) == expect_lo &&
    la_load64_acq(&a->sweep[(w << 1) + 1u]) == expect_hi;
}

static int arena_root_empty(const GCArena *a)
{
  uint32_t w;
  if (!a)
    return 1;
  for (w = 0; w < LJ_ARENA_ROOT_WORDS; w++)
    if (la_load64_acq(&a->root[w]) != 0)
      return 0;
  return 1;
}

static LJ_AINLINE int arena_side_owners_none(const GCArena *a, uint32_t cell)
{
  return lj_arena_recovery_state_acq(a, cell) == LJ_ARENA_RECOVERY_IDLE &&
    lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE &&
    arena_gc2_token_none_acq(a, cell);
}

int lj_arena_lifetime_empty(const GCArena *a)
{
  uint32_t w;
  if (!a)
    return 1;
  for (w = 0; w < LJ_ARENA_LIFETIME_WORDS; w++)
    if (la_load64_acq(&a->lifetime[w]) != 0)
      return 0;
  return 1;
}

uint32_t lj_arena_lifetime_clear_terminal(GCArena *a, uint32_t cell)
{
  uint32_t state;
  if (!a || cell >= LJ_ARENA_CELLS)
    return LJ_ARENA_LIFETIME_FREE;
  state = lj_arena_lifetime_state_acq(a, cell);
  while (state != LJ_ARENA_LIFETIME_FREE) {
    /* A token may legitimately coexist with FREE, but this quiescent cleanup
    ** helper promises that no runtime actor can resume the allocation. Refuse
    ** to discard its locator while that promise is visibly false. */
    if (!lj_arena_gc2_reclaim_clear_acq(a, cell))
      return LJ_ARENA_LIFETIME_CLEAR_BLOCKED;
    if (lj_arena_lifetime_state_cas(a, cell, state,
					    LJ_ARENA_LIFETIME_FREE))
      return state;
    state = lj_arena_lifetime_state_acq(a, cell);
  }
  return LJ_ARENA_LIFETIME_FREE;
}

int lj_arena_root_construct_claim(GCArena *a, uint32_t cell)
{
  uint32_t life, root;
  if (!arena_lifetime_managed(a) || cell < LJ_AFIRST_CELL ||
      cell >= LJ_ARENA_CELLS)
    return 0;
  life = lj_arena_lifetime_state_acq(a, cell);
  root = lj_arena_root_state_acq(a, cell);
  if (life == LJ_ARENA_LIFETIME_CONSTRUCT &&
      root == LJ_ARENA_ROOT_LINKING)
    return 1;  /* Idempotent retry by the unique allocation owner. */
  if (life != LJ_ARENA_LIFETIME_FREE || root != LJ_ARENA_ROOT_NONE ||
      !arena_gc2_token_none_acq(a, cell) ||
      !lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_FREE,
					   LJ_ARENA_LIFETIME_CONSTRUCT))
    return 0;
  if (!lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_NONE,
				       LJ_ARENA_ROOT_LINKING)) {
    int rolled = lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_FREE);
    lj_assertX(rolled, "arena root-construction claim rollback lost");
    UNUSED(rolled);
    return 0;
  }
  if (!arena_gc2_prepare_incarnation(a, cell)) {
    int root_rolled = lj_arena_root_state_cas(a, cell,
	LJ_ARENA_ROOT_LINKING, LJ_ARENA_ROOT_NONE);
    int life_rolled = root_rolled && lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_FREE);
    /* FREE+token is the exact old incarnation: the token remains its physical
    ** reuse veto, while no allocation byte or structural bit was changed. */
    lj_assertX(root_rolled && life_rolled,
		 "arena root-construction GC2 rollback lost");
    UNUSED(root_rolled); UNUSED(life_rolled);
    return 0;
  }
  return 1;
}

int lj_arena_root_construct_commit(GCArena *a, uint32_t cell)
{
  uint32_t life, root;
  int committed;
  if (!arena_lifetime_managed(a) || cell < LJ_AFIRST_CELL ||
      cell >= LJ_ARENA_CELLS)
    return 0;
  life = lj_arena_lifetime_state_acq(a, cell);
  root = lj_arena_root_state_acq(a, cell);
  if (life == LJ_ARENA_LIFETIME_RECOVERY) {
    /* Only recovery may claim CONSTRUCT. Publish the already-visible edge
    ** without stealing its lane; recovery restores LIVE after observing that
    ** LINKING no longer owns constructor state. */
    committed = root == LJ_ARENA_ROOT_MEMBER ||
      (root == LJ_ARENA_ROOT_LINKING &&
       lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_LINKING,
				       LJ_ARENA_ROOT_MEMBER));
    if (!committed)
      return 0;
    /* Recovery may have sampled LINKING before the root CAS and restored its
    ** saved origin as CONSTRUCT afterward. Repair that crossover; if recovery
    ** reclaimed RECOVERY again, its post-restore root recheck performs the
    ** symmetric repair. */
    (void)lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_LIVE);
    return 1;
  }
  if (life == LJ_ARENA_LIFETIME_CONSTRUCT) {
    if (root != LJ_ARENA_ROOT_LINKING && root != LJ_ARENA_ROOT_MEMBER)
      return 0;
    if (!lj_arena_lifetime_state_cas(a, cell,
	  LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_LIVE))
      return 0;
    life = LJ_ARENA_LIFETIME_LIVE;
  }
  if (life != LJ_ARENA_LIFETIME_LIVE)
    return 0;  /* Recovery, mutation or free owns the lane; never wait. */
  root = lj_arena_root_state_acq(a, cell);
  if (root == LJ_ARENA_ROOT_MEMBER)
    return 1;
  return root == LJ_ARENA_ROOT_LINKING &&
    lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_LINKING,
				    LJ_ARENA_ROOT_MEMBER);
}

int lj_arena_root_construct_commit_pair(GCArena *a, uint32_t first,
					 uint32_t second)
{
  uint32_t lwi, rwi, lshift1, lshift2, rshift1, rshift2;
  uint64_t lmask1, lmask2, rmask1, rmask2, old;
  if (!arena_lifetime_managed(a) || first == second ||
      first < LJ_AFIRST_CELL || second < LJ_AFIRST_CELL ||
      first >= LJ_ARENA_CELLS || second >= LJ_ARENA_CELLS)
    goto fallback;
  lwi = first / LJ_ARENA_LIFETIME_CELLS_PER_WORD;
  rwi = first / LJ_ARENA_ROOT_CELLS_PER_WORD;
  if (lwi != second / LJ_ARENA_LIFETIME_CELLS_PER_WORD ||
      rwi != second / LJ_ARENA_ROOT_CELLS_PER_WORD)
    goto fallback;
  lshift1 = (first & (LJ_ARENA_LIFETIME_CELLS_PER_WORD-1u)) << 2;
  lshift2 = (second & (LJ_ARENA_LIFETIME_CELLS_PER_WORD-1u)) << 2;
  rshift1 = (first & (LJ_ARENA_ROOT_CELLS_PER_WORD-1u)) << 1;
  rshift2 = (second & (LJ_ARENA_ROOT_CELLS_PER_WORD-1u)) << 1;
  lmask1 = (uint64_t)0x0fu << lshift1;
  lmask2 = (uint64_t)0x0fu << lshift2;
  rmask1 = (uint64_t)0x03u << rshift1;
  rmask2 = (uint64_t)0x03u << rshift2;

  old = la_load64_acq(&a->lifetime[lwi]);
  for (;;) {
    uint64_t next;
    if (((old & lmask1) >> lshift1) != LJ_ARENA_LIFETIME_CONSTRUCT ||
	((old & lmask2) >> lshift2) != LJ_ARENA_LIFETIME_CONSTRUCT)
      goto fallback;
    next = (old & ~(lmask1 | lmask2)) |
	((uint64_t)LJ_ARENA_LIFETIME_LIVE << lshift1) |
	((uint64_t)LJ_ARENA_LIFETIME_LIVE << lshift2);
    if (la_cas64(&a->lifetime[lwi], &old, next, LA_ACQ_REL, LA_ACQ))
      break;
  }

  old = la_load64_acq(&a->root[rwi]);
  for (;;) {
    uint32_t root1 = (uint32_t)((old & rmask1) >> rshift1);
    uint32_t root2 = (uint32_t)((old & rmask2) >> rshift2);
    uint64_t next;
    if ((root1 != LJ_ARENA_ROOT_LINKING &&
	 root1 != LJ_ARENA_ROOT_MEMBER) ||
	(root2 != LJ_ARENA_ROOT_LINKING &&
	 root2 != LJ_ARENA_ROOT_MEMBER))
      goto fallback;
    if (root1 == LJ_ARENA_ROOT_MEMBER && root2 == LJ_ARENA_ROOT_MEMBER)
      return 1;
    next = (old & ~(rmask1 | rmask2)) |
	((uint64_t)LJ_ARENA_ROOT_MEMBER << rshift1) |
	((uint64_t)LJ_ARENA_ROOT_MEMBER << rshift2);
    if (la_cas64(&a->root[rwi], &old, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }

fallback:
  return lj_arena_root_construct_commit(a, first) &&
    lj_arena_root_construct_commit(a, second);
}

int lj_arena_root_construct_abandon(GCArena *a, uint32_t cell)
{
  uint32_t life, root;
  if (!arena_lifetime_managed(a) || cell < LJ_AFIRST_CELL ||
      cell >= LJ_ARENA_CELLS)
    return 0;
  life = lj_arena_lifetime_state_acq(a, cell);
  if (life != LJ_ARENA_LIFETIME_CONSTRUCT &&
      life != LJ_ARENA_LIFETIME_LIVE &&
      life != LJ_ARENA_LIFETIME_RECOVERY)
    return 0;
  root = lj_arena_root_state_acq(a, cell);
  if (root == LJ_ARENA_ROOT_LINKING &&
      !lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_LINKING,
			       LJ_ARENA_ROOT_NONE))
    return 0;
  if (lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE)
    return 0;
  if (life == LJ_ARENA_LIFETIME_RECOVERY)
  {
    /* Repair recovery's stale-LINKING CONSTRUCT restore if it crossed the root
    ** clear. A still-RECOVERY publisher performs the symmetric root recheck. */
    (void)lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_LIVE);
    return 1;
  }
  if (life == LJ_ARENA_LIFETIME_CONSTRUCT)
    return lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_LIVE);
  return 1;
}

/* Validate every descriptor-discovery bit before claiming lifetime. The bump
** owner is the sole structural writer and the span remains absent from block,
** so a successful FREE->CONSTRUCT claim makes this snapshot stable until the
** same owner publishes the immutable kind below. Pair starts normally share a
** 64-cell word; test their combined mask with one load per plane. */
static int arena_dtor_kind_preflight_unpublished(GCArena *a, uint32_t first,
						  uint32_t second, int pair)
{
  uint32_t fw, sw, plane;
  uint64_t fbit, sbit, mask;
  if (!arena_lifetime_managed(a) || first < LJ_AFIRST_CELL ||
      first >= LJ_ARENA_CELLS || (pair &&
       (second < LJ_AFIRST_CELL || second >= LJ_ARENA_CELLS ||
	second == first)))
    return 0;
  fw = first >> 6;
  sw = second >> 6;
  fbit = (uint64_t)1 << (first & 63u);
  sbit = (uint64_t)1 << (second & 63u);
  if (!pair || fw == sw) {
    mask = fbit | (pair ? sbit : 0);
    if (((la_load64_acq(&a->block[fw]) |
	  la_load64_acq(&a->ready[fw])) & mask) != 0)
      return 0;
    for (plane = 0; plane < LJ_ARENA_DTOR_PLANES; plane++)
      if ((la_load64_acq(&a->dtor[plane][fw]) & mask) != 0)
	return 0;
  } else {
    if (((la_load64_acq(&a->block[fw]) |
	  la_load64_acq(&a->ready[fw])) & fbit) != 0 ||
	((la_load64_acq(&a->block[sw]) |
	  la_load64_acq(&a->ready[sw])) & sbit) != 0)
      return 0;
    for (plane = 0; plane < LJ_ARENA_DTOR_PLANES; plane++)
      if ((la_load64_acq(&a->dtor[plane][fw]) & fbit) != 0 ||
	  (la_load64_acq(&a->dtor[plane][sw]) & sbit) != 0)
	return 0;
  }
  return 1;
}

/* Publish immutable destructor classes after the exact lifetime claim and
** before READY/block make either body decodable. The arena/bump owner is the
** sole structural writer, so plain word stores are sufficient; block's later
** release publication orders these writes for readers. */
static void arena_dtor_kind_publish_claimed(GCArena *a, uint32_t first,
	uint32_t second, int pair, uint32_t first_kind, uint32_t second_kind)
{
  uint32_t fw = first >> 6, sw = second >> 6, plane;
  uint64_t fbit = (uint64_t)1 << (first & 63u);
  uint64_t sbit = (uint64_t)1 << (second & 63u);
  lj_assertX(lj_arena_root_state_acq(a, first) == LJ_ARENA_ROOT_NONE &&
      lj_arena_lifetime_state_acq(a, first) ==
	LJ_ARENA_LIFETIME_CONSTRUCT,
      "arena typed first-start publication lost claim");
  lj_assertX(!pair ||
      (lj_arena_root_state_acq(a, second) == LJ_ARENA_ROOT_NONE &&
       lj_arena_lifetime_state_acq(a, second) ==
	 LJ_ARENA_LIFETIME_CONSTRUCT),
      "arena typed second-start publication lost claim");
  for (plane = 0; plane < LJ_ARENA_DTOR_PLANES; plane++) {
    uint32_t pbit = (uint32_t)1u << plane;
    uint64_t fadd = (first_kind & pbit) ? fbit : 0;
    uint64_t sadd = (pair && (second_kind & pbit)) ? sbit : 0;
    if (fw == sw) {
      uint64_t add = fadd | sadd;
      if (add != 0) {
	uint64_t old = la_load64_rlx(&a->dtor[plane][fw]);
	lj_assertX((old & (fbit | (pair ? sbit : 0))) == 0,
	    "arena typed same-word descriptor publication crossed authority");
	la_store64_rlx(&a->dtor[plane][fw], old | add);
      }
    } else {
      if (fadd != 0) {
	uint64_t old = la_load64_rlx(&a->dtor[plane][fw]);
	lj_assertX((old & fbit) == 0,
	    "arena typed first descriptor publication crossed authority");
	la_store64_rlx(&a->dtor[plane][fw], old | fadd);
      }
      if (sadd != 0) {
	uint64_t old = la_load64_rlx(&a->dtor[plane][sw]);
	lj_assertX((old & sbit) == 0,
	    "arena typed second descriptor publication crossed authority");
	la_store64_rlx(&a->dtor[plane][sw], old | sadd);
      }
    }
  }
  lj_assertX(lj_arena_dtor_kind_acq(a, first) == first_kind &&
      (!pair || lj_arena_dtor_kind_acq(a, second) == second_kind),
      "arena typed descriptor publication lost bits");
}

int lj_arena_dtor_construct_commit(GCArena *a, uint32_t cell)
{
  uint32_t life;
  if (!arena_lifetime_managed(a) || cell < LJ_AFIRST_CELL ||
      cell >= LJ_ARENA_CELLS ||
      lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE ||
      lj_arena_dtor_kind_acq(a, cell) == LJ_ARENA_DTOR_NONE ||
      !lj_arena_ready_get(a, cell) || !lj_arena_bm_get(a->block, cell))
    return 0;
  life = lj_arena_lifetime_state_acq(a, cell);
  for (;;) {
    if (life == LJ_ARENA_LIFETIME_LIVE)
      return 1;
    if (life == LJ_ARENA_LIFETIME_RECOVERY) {
      /* Only recovery can claim a published CONSTRUCT body. Its saved origin
      ** is CONSTRUCT and root NONE makes its mandatory restore target LIVE.
      ** The global recovery reservation already vetoes phase close before the
      ** durable per-cell identity appears, so the constructor never waits. */
      return 1;
    }
    if (life != LJ_ARENA_LIFETIME_CONSTRUCT)
      return 0;
    if (lj_arena_lifetime_state_cas(a, cell,
	  LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_LIVE))
      return 1;
    life = lj_arena_lifetime_state_acq(a, cell);
  }
}

int lj_arena_dtor_construct_commit_pair(GCArena *a, uint32_t first,
						 uint32_t second)
{
  uint32_t life1, life2;
  if (!a || first == second || first < LJ_AFIRST_CELL ||
      second < LJ_AFIRST_CELL || first >= LJ_ARENA_CELLS ||
      second >= LJ_ARENA_CELLS ||
      lj_arena_root_state_acq(a, first) != LJ_ARENA_ROOT_NONE ||
      lj_arena_root_state_acq(a, second) != LJ_ARENA_ROOT_NONE ||
      lj_arena_dtor_kind_acq(a, first) == LJ_ARENA_DTOR_NONE ||
      lj_arena_dtor_kind_acq(a, second) == LJ_ARENA_DTOR_NONE ||
      !lj_arena_ready_get(a, first) || !lj_arena_ready_get(a, second) ||
      !lj_arena_bm_get(a->block, first) ||
      !lj_arena_bm_get(a->block, second))
    return 0;
  life1 = lj_arena_lifetime_state_acq(a, first);
  life2 = lj_arena_lifetime_state_acq(a, second);
  if (life1 == LJ_ARENA_LIFETIME_CONSTRUCT &&
      life2 == LJ_ARENA_LIFETIME_CONSTRUCT &&
      first / LJ_ARENA_LIFETIME_CELLS_PER_WORD ==
	second / LJ_ARENA_LIFETIME_CELLS_PER_WORD &&
      lj_arena_lifetime_state_cas_pair(a, first, second,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_LIVE))
    return 1;
  /* Recovery may own either lane independently. The single-start helper
  ** accepts RECOVERY without waiting and commits each still-CONSTRUCT lane. */
  return lj_arena_dtor_construct_commit(a, first) &&
    lj_arena_dtor_construct_commit(a, second);
}

int lj_arena_recovery_empty(const GCArena *a)
{
  uint32_t w;
  if (!a)
    return 1;
  for (w = 0; w < LJ_ARENA_RECOVERY_WORDS; w++)
    if (la_load64_acq(&a->recovery[w]) != 0)
      return 0;
  return 1;
}

static int arena_remote_enter(GCArena *a)
{
  uint64_t active;
  if (!a)
    return 0;
  active = la_load64_acq(&a->hdr.remote_active);
  for (;;) {
    uint64_t expect = active;
    if (arena_terminal_closed_acq(a))
      return 0;
    if (active & LJ_ARENA_REMOTE_STATE_MASK)
      return 0;
    if (arena_remote_count(active) == LJ_ARENA_REMOTE_COUNT_MASK) {
      arena_remote_overflow();
    }
    if (la_cas64(&a->hdr.remote_active, &expect, active + 1u,
		 LA_ACQ_REL, LA_ACQ))
      return 1;
    active = expect;
  }
}

/* Admit a bit-only late publisher after terminal close or while SEALED. The
** admission CAS publishes PENDING before the late bit, so an owner can never
** clear/commit past an intent whose producer was preempted before the bit. */
static int arena_remote_late_enter(GCArena *a)
{
  uint64_t active;
  if (!a)
    return 0;
  active = la_load64_acq(&a->hdr.remote_active);
  for (;;) {
    uint64_t expect = active;
    uint64_t next;
    if (arena_terminal_closed_acq(a))
      return -1;
    if (!(active & (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED)))
      return 0;  /* Gate reopened: caller must use the ordinary route. */
    if (arena_remote_count(active) == LJ_ARENA_REMOTE_COUNT_MASK) {
      arena_remote_overflow();
    }
    next = (active + 1u) | LJ_ARENA_REMOTE_PENDING;
    if (la_cas64(&a->hdr.remote_active, &expect, next,
		 LA_ACQ_REL, LA_ACQ))
      return 1;
    active = expect;
  }
}

static void arena_publish_leave(GCArena *a)
{
  uint64_t old = la_sub64_acqrel(&a->hdr.remote_active, 1);
  lj_assertX(arena_remote_count(old) != 0,
	     "arena publisher leave without admission");
  if (arena_remote_count(old) == 1u) {
    uint32_t flags = lj_arena_flags_acq(a);
    uint64_t now = old - 1u;
    int wake = (now & LJ_ARENA_REMOTE_STATE_MASK) != 0 ||
	(flags & (LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|
		  LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) != 0;
    /* Adoption/restore publishes all bitmap and bin state before clearing the
    ** lifecycle flags and dropping SEALED to CLOSED. The last bit-only
    ** publisher can then complete the exact clean gate transition to OPEN. */
    if (!(flags & (LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|
		   LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) &&
	(now & LJ_ARENA_REMOTE_CLOSED) &&
	!(now & LJ_ARENA_REMOTE_SEALED)) {
      uint64_t expect = now;
      arena_late_clear_committed_free(a);
      (void)la_cas64(&a->hdr.remote_active, &expect, 0,
		     LA_REL, LA_ACQ);
    }
    /* OPEN 1->0 is the ordinary mark fast path and has no waiter. Lifecycle
    ** owners need the edge only after they publish a terminal gate/flag. */
    if (wake)
      arena_progress_wake(a);
  }
}

static void arena_remote_leave(GCArena *a)
{
  arena_publish_leave(a);
}

int lj_arena_remote_sweep_busy_acq(const GCArena *a)
{
  uint64_t active;
  if (!a)
    return 0;
  active = lj_arena_remote_active_acq(a);
  return arena_remote_count(active) != 0;
}

int lj_arena_reclaim_seal(GCArena *a)
{
  uint64_t active;
  if (!a)
    return 0;
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t state = active & LJ_ARENA_REMOTE_STATE_MASK;
    uint64_t expect = active;
    uint64_t next;
    if (arena_remote_count(active) != 0)
      return 0;
    if (state & LJ_ARENA_REMOTE_SEALED)
      return 0;
    if (state != 0 && state != LJ_ARENA_REMOTE_CLOSED &&
	state != (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_PENDING))
      return 0;
    next = (state & LJ_ARENA_REMOTE_PENDING) |
	   LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED;
    if (la_cas64(&a->hdr.remote_active, &expect, next,
		 LA_ACQ_REL, LA_ACQ))
      return 1;
    active = expect;
  }
}

/* Final clean-state arbitration. A bit-only publisher changes this exact word
** before publishing its intent, so the owner cannot enter mutation/commit
** ownership past a preempted producer. */
static int arena_reclaim_commit_sealed(GCArena *a)
{
  uint64_t expect = LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED;
  return a && la_cas64(&a->hdr.remote_active, &expect,
			LJ_ARENA_REMOTE_SEALED, LA_ACQ_REL, LA_ACQ);
}

int lj_arena_reclaim_clear_pending(GCArena *a)
{
  uint64_t active;
  const uint64_t clean = LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED;
  if (!a)
    return 0;
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t expect = active;
    if (active == clean)
      return 1;
    if (active != (clean|LJ_ARENA_REMOTE_PENDING))
      return 0;
    if (la_cas64(&a->hdr.remote_active, &expect, clean,
		 LA_ACQ_REL, LA_ACQ))
      return 1;
    active = expect;
  }
}

void lj_arena_reclaim_unseal(GCArena *a, int keep_pending)
{
  uint64_t active;
  if (!a)
    return;
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t expect = active;
    uint64_t next;
    lj_assertX((active & LJ_ARENA_REMOTE_SEALED) != 0,
	       "arena unseal without exclusive ownership");
    if (!(active & LJ_ARENA_REMOTE_SEALED))
      return;
    next = arena_remote_count(active) | LJ_ARENA_REMOTE_CLOSED |
	((keep_pending || arena_remote_count(active) != 0) &&
	 (active & LJ_ARENA_REMOTE_PENDING) ?
	 LJ_ARENA_REMOTE_PENDING : 0u);
    if (la_cas64(&a->hdr.remote_active, &expect, next,
		 LA_REL, LA_ACQ))
      return;
    active = expect;
  }
}

static int arena_remote_open_sealed(GCArena *a)
{
  uint64_t active;
  if (!a)
    return 0;
  arena_test_open_sealed_pause_before_cas(a);
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t expect = active;
    if (active != LJ_ARENA_REMOTE_SEALED)
      return 0;
    if (la_cas64(&a->hdr.remote_active, &expect, 0,
		 LA_REL, LA_ACQ))
      return 1;
    active = expect;
  }
}

/* Complete/abort a plain writer generation. CLOSED means every body/bin
** mutation is published. Counted late publishers may still finish their
** bitmap intent; the last leave opens the generation. A zero-count writer
** attempts that open itself, racing a new publisher in the same word. */
static void arena_plain_mutation_done(GCArena *a)
{
  uint64_t active;
  if (!a)
    return;
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t expect = active;
    uint64_t next;
    lj_assertX((active & LJ_ARENA_REMOTE_SEALED) != 0,
	       "plain arena writer completion without SEALED");
    if (!(active & LJ_ARENA_REMOTE_SEALED))
      return;
    next = (active & ~(uint64_t)LJ_ARENA_REMOTE_SEALED) |
	LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_PENDING;
    if (la_cas64(&a->hdr.remote_active, &expect, next,
		 LA_REL, LA_ACQ)) {
      if (arena_remote_count(next) == 0) {
	uint64_t open = next;
	arena_late_clear_committed_free(a);
	(void)la_cas64(&a->hdr.remote_active, &open, 0, LA_REL, LA_ACQ);
      }
      return;
    }
    active = expect;
  }
}

/* A writer which acquired C|S but lost the commit CAS must publish its own
** logical intent before the competing publisher can be the last leave/open.
** Replace the consumed own_count with one explicit admission while converting
** the generation to completed CLOSED. Return -1 tells the caller to publish
** its late bit and then call arena_remote_late_leave(). */
static void arena_plain_mutation_abort_admit(GCArena *a)
{
  uint64_t active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t expect = active;
    uint64_t next;
    lj_assertX((active & LJ_ARENA_REMOTE_SEALED) != 0,
	       "plain writer abort without SEALED");
    if (!(active & LJ_ARENA_REMOTE_SEALED))
      return;
    if (arena_remote_count(active) == LJ_ARENA_REMOTE_COUNT_MASK)
      arena_remote_overflow();
    next = ((active + 1u) & ~(uint64_t)LJ_ARENA_REMOTE_SEALED) |
	LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_PENDING;
    if (la_cas64(&a->hdr.remote_active, &expect, next,
		 LA_ACQ_REL, LA_ACQ))
      return;
    active = expect;
  }
}

/* Plain allocations have no per-start lifetime lane. First close+seal the
** exact admission generation, then commit it to the distinct SEALED|PENDING
** writer state. A late publisher admitted between those CASes defeats commit;
** the helper publishes CLOSED and returns -1 after consuming own_count.
** Return 1 for committed ownership, 0 when the initial CAS lost, and -1 for
** this safe post-acquisition abort. */
static int arena_plain_mutation_claim(GCArena *a, uint64_t own_count)
{
  uint64_t expect = own_count;
  uint64_t closed = LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED;
  if (!a || arena_lifetime_managed(a) ||
      !la_cas64(&a->hdr.remote_active, &expect, closed,
		LA_ACQ_REL, LA_ACQ))
    return 0;
  arena_test_plain_claim_pause_after_close();
  expect = closed;
  if (la_cas64(&a->hdr.remote_active, &expect,
	       LJ_ARENA_REMOTE_SEALED|LJ_ARENA_REMOTE_PENDING,
	       LA_ACQ_REL, LA_ACQ))
    return 1;
  arena_plain_mutation_abort_admit(a);
  return -1;
}

static LJ_AINLINE int arena_plain_mutation_held(const GCArena *a)
{
  return a && !arena_lifetime_managed(a) &&
    (lj_arena_remote_active_acq(a) & LJ_ARENA_REMOTE_STATE_MASK) ==
      (LJ_ARENA_REMOTE_SEALED|LJ_ARENA_REMOTE_PENDING);
}

static void arena_plain_mutation_release(GCArena *a)
{
  arena_plain_mutation_done(a);
}

int lj_arena_rescue_enter(GCArena *a)
{
  uint64_t active;
  if (!a)
    return 0;
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t expect = active;
    uint64_t next;
    int committed;
    if (arena_terminal_closed_acq(a))
      return LJ_ARENA_RESCUE_RETRY;
    /* A plain writer has no object-local descriptor to make later admissions
    ** reject. SEALED itself is therefore terminal for this nonblocking try. */
    if (!arena_lifetime_managed(a) &&
	(active & LJ_ARENA_REMOTE_STATE_MASK))
      return LJ_ARENA_RESCUE_RETRY;
    if (arena_remote_count(active) == LJ_ARENA_REMOTE_COUNT_MASK) {
      arena_remote_overflow();
    }
    committed = (active & LJ_ARENA_REMOTE_SEALED) &&
		!(active & LJ_ARENA_REMOTE_CLOSED);
    next = active + 1u;
    if (!committed &&
	(active & (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED)))
      next |= LJ_ARENA_REMOTE_PENDING;
    if (la_cas64(&a->hdr.remote_active, &expect, next,
		 LA_ACQ_REL, LA_ACQ))
      return committed ? LJ_ARENA_RESCUE_COMMITTED :
	((active & LJ_ARENA_REMOTE_SEALED) ?
	 LJ_ARENA_RESCUE_BIT_ONLY : LJ_ARENA_RESCUE_FULL);
    active = expect;
  }
}

void lj_arena_rescue_leave(GCArena *a)
{
  arena_publish_leave(a);
}

static void arena_remote_late_leave(GCArena *a)
{
  arena_publish_leave(a);
}

typedef struct LJHugeEnt {
  la_u128 slot;
} LJHugeEnt;

struct LJHugeTabHdr {
  uint32_t hbits;
  uint32_t mask;
  size_t mapsize;
  LJHugeEnt ent[1];
};

LJ_STATIC_ASSERT(sizeof(LJHugeEnt) == 16u);
LJ_STATIC_ASSERT(offsetof(LJHugeTabHdr, ent) == 16u);
LJ_STATIC_ASSERT((offsetof(LJHugeTabHdr, ent) & 15u) == 0);

/* INVALID/PINNED deliberately does not suppress allocator progress. The
** absorbing scanner veto remains fail-closed while heap mutations continue. */
static LJ_AINLINE LJGC2TableTopology *
hugetab_table_topology_acq(const HugeTab *ht)
{
  return ht ? (LJGC2TableTopology *)la_loadptr_acq(
    (void *const *)&ht->table_topology) : NULL;
}

static LJ_AINLINE void hugetab_table_topology_changed(HugeTab *ht)
{
  LJGC2TableTopology *topology = hugetab_table_topology_acq(ht);
  (void)lj_gc2_table_topology_changed(topology);
}

#if defined(LJ_ARENA_TEST_HELPERS)
static uint32_t hugetab_test_retire_pause;
static uint32_t hugetab_test_retire_paused;
static uint32_t hugetab_test_realloc_pause;
static uint32_t hugetab_test_realloc_paused;
static uint32_t hugetab_test_admission_close_pause;
static uint32_t hugetab_test_admission_close_paused;
static uint32_t arena_test_plain_late_pause_flag;
static uint32_t arena_test_plain_late_pause_seen;
static uint32_t arena_test_registry_pause_flag;
static uint32_t arena_test_registry_pause_seen;
static uint32_t arena_test_plain_claim_pause_flag;
static uint32_t arena_test_plain_claim_pause_seen;
static uint32_t arena_test_plain_admit_pause_flag;
static uint32_t arena_test_plain_admit_pause_seen;
static uint32_t arena_test_remote_publish_pause_flag;
static uint32_t arena_test_remote_publish_pause_seen;
static uint32_t arena_test_remote_drain_pause_flag;
static uint32_t arena_test_remote_drain_pause_seen;
static uint64_t arena_test_remote_fast_skip_count;
static uint64_t arena_test_remote_arena_probe_count;
static uint64_t arena_test_adopt_whole_count;
static uint32_t arena_test_gc2_sidecar_fail_alloc_flag;
static GCArena *arena_test_open_sealed_target;
static uint32_t arena_test_open_sealed_pause_flag;
static uint32_t arena_test_open_sealed_pause_seen;

void lj_arena_test_open_sealed_pause(GCArena *a, int enabled)
{
  if (enabled) {
    la_store32_rel(&arena_test_open_sealed_pause_seen, 0);
    la_storeptr_rel((void **)&arena_test_open_sealed_target, a);
  }
  la_store32_rel(&arena_test_open_sealed_pause_flag, enabled != 0);
}

uint32_t lj_arena_test_open_sealed_paused(void)
{
  return la_load32_acq(&arena_test_open_sealed_pause_seen);
}

static void arena_test_open_sealed_pause_before_cas(GCArena *a)
{
  if (la_load32_acq(&arena_test_open_sealed_pause_flag) != 0 &&
      la_loadptr_acq((void *const *)&arena_test_open_sealed_target) == a) {
    la_store32_rel(&arena_test_open_sealed_pause_seen, 1);
    while (la_load32_acq(&arena_test_open_sealed_pause_flag) != 0)
      la_cpu_pause();
    la_store32_rel(&arena_test_open_sealed_pause_seen, 0);
  }
}

void lj_arena_hugetab_test_admission_close_pause(int enabled)
{
  if (enabled)
    la_store32_rel(&hugetab_test_admission_close_paused, 0);
  la_store32_rel(&hugetab_test_admission_close_pause, (uint32_t)enabled);
}

uint32_t lj_arena_hugetab_test_admission_close_paused(void)
{
  return la_load32_acq(&hugetab_test_admission_close_paused);
}

static void hugetab_test_admission_close_pause_after_snapshot(void)
{
  if (la_load32_acq(&hugetab_test_admission_close_pause) == 1u) {
    la_store32_rel(&hugetab_test_admission_close_paused, 1);
    while (la_load32_acq(&hugetab_test_admission_close_pause) == 1u)
      la_cpu_pause();
    la_store32_rel(&hugetab_test_admission_close_paused, 0);
  }
}

static void hugetab_test_admission_close_pause_after_close(void)
{
  if (la_load32_acq(&hugetab_test_admission_close_pause) == 2u) {
    la_store32_rel(&hugetab_test_admission_close_paused, 2);
    while (la_load32_acq(&hugetab_test_admission_close_pause) == 2u)
      la_cpu_pause();
    la_store32_rel(&hugetab_test_admission_close_paused, 0);
  }
}

void lj_arena_hugetab_test_realloc_pause(int enabled)
{
  if (enabled)
    la_store32_rel(&hugetab_test_realloc_paused, 0);
  la_store32_rel(&hugetab_test_realloc_pause, enabled != 0);
}

uint32_t lj_arena_hugetab_test_realloc_paused(void)
{
  return la_load32_acq(&hugetab_test_realloc_paused);
}

static void hugetab_test_realloc_pause_after_busy(void)
{
  if (la_load32_acq(&hugetab_test_realloc_pause) != 0) {
    la_store32_rel(&hugetab_test_realloc_paused, 1);
    while (la_load32_acq(&hugetab_test_realloc_pause) != 0)
      la_cpu_pause();
    la_store32_rel(&hugetab_test_realloc_paused, 0);
  }
}

void lj_arena_hugetab_test_retire_pause(int enabled)
{
  if (enabled)
    la_store32_rel(&hugetab_test_retire_paused, 0);
  la_store32_rel(&hugetab_test_retire_pause, enabled != 0);
}

uint32_t lj_arena_hugetab_test_retire_paused(void)
{
  return la_load32_acq(&hugetab_test_retire_paused);
}

static void hugetab_test_retire_pause_after_busy(void)
{
  if (la_load32_acq(&hugetab_test_retire_pause) != 0) {
    la_store32_rel(&hugetab_test_retire_paused, 1);
    while (la_load32_acq(&hugetab_test_retire_pause) != 0)
      la_cpu_pause();
    la_store32_rel(&hugetab_test_retire_paused, 0);
  }
}

void lj_arena_test_plain_late_pause(int enabled)
{
  if (enabled)
    la_store32_rel(&arena_test_plain_late_pause_seen, 0);
  la_store32_rel(&arena_test_plain_late_pause_flag, enabled != 0);
}

uint32_t lj_arena_test_plain_late_paused(void)
{
  return la_load32_acq(&arena_test_plain_late_pause_seen);
}

static void arena_test_plain_late_pause_after_enter(void)
{
  if (la_load32_acq(&arena_test_plain_late_pause_flag) != 0) {
    la_store32_rel(&arena_test_plain_late_pause_seen, 1);
    while (la_load32_acq(&arena_test_plain_late_pause_flag) != 0)
      la_cpu_pause();
    la_store32_rel(&arena_test_plain_late_pause_seen, 0);
  }
}

void lj_arena_test_registry_pause(int enabled)
{
  if (enabled)
    la_store32_rel(&arena_test_registry_pause_seen, 0);
  la_store32_rel(&arena_test_registry_pause_flag, enabled != 0);
}

uint32_t lj_arena_test_registry_paused(void)
{
  return la_load32_acq(&arena_test_registry_pause_seen);
}

static void arena_test_registry_pause_after_reader(void)
{
  if (la_load32_acq(&arena_test_registry_pause_flag) != 0) {
    la_store32_rel(&arena_test_registry_pause_seen, 1);
    while (la_load32_acq(&arena_test_registry_pause_flag) != 0)
      la_cpu_pause();
    la_store32_rel(&arena_test_registry_pause_seen, 0);
  }
}

void lj_arena_test_plain_claim_pause(int enabled)
{
  if (enabled)
    la_store32_rel(&arena_test_plain_claim_pause_seen, 0);
  la_store32_rel(&arena_test_plain_claim_pause_flag, enabled != 0);
}

uint32_t lj_arena_test_plain_claim_paused(void)
{
  return la_load32_acq(&arena_test_plain_claim_pause_seen);
}

static void arena_test_plain_claim_pause_after_close(void)
{
  if (la_load32_acq(&arena_test_plain_claim_pause_flag) != 0) {
    la_store32_rel(&arena_test_plain_claim_pause_seen, 1);
    while (la_load32_acq(&arena_test_plain_claim_pause_flag) != 0)
      la_cpu_pause();
    la_store32_rel(&arena_test_plain_claim_pause_seen, 0);
  }
}

void lj_arena_test_plain_admit_pause(int enabled)
{
  if (enabled)
    la_store32_rel(&arena_test_plain_admit_pause_seen, 0);
  la_store32_rel(&arena_test_plain_admit_pause_flag, enabled != 0);
}

uint32_t lj_arena_test_plain_admit_paused(void)
{
  return la_load32_acq(&arena_test_plain_admit_pause_seen);
}

static void arena_test_plain_admit_pause_after_enter(void)
{
  if (la_load32_acq(&arena_test_plain_admit_pause_flag) != 0) {
    la_store32_rel(&arena_test_plain_admit_pause_seen, 1);
    while (la_load32_acq(&arena_test_plain_admit_pause_flag) != 0)
      la_cpu_pause();
    la_store32_rel(&arena_test_plain_admit_pause_seen, 0);
  }
}

void lj_arena_test_remote_publish_pause(int enabled)
{
  if (enabled)
    la_store32_rel(&arena_test_remote_publish_pause_seen, 0);
  la_store32_rel(&arena_test_remote_publish_pause_flag, enabled != 0);
}

uint32_t lj_arena_test_remote_publish_paused(void)
{
  return la_load32_acq(&arena_test_remote_publish_pause_seen);
}

static void arena_test_remote_publish_pause_after_queue(void)
{
  if (la_load32_acq(&arena_test_remote_publish_pause_flag) != 0) {
    la_store32_rel(&arena_test_remote_publish_pause_seen, 1);
    while (la_load32_acq(&arena_test_remote_publish_pause_flag) != 0)
      la_cpu_pause();
    la_store32_rel(&arena_test_remote_publish_pause_seen, 0);
  }
}

void lj_arena_test_remote_drain_pause(int enabled)
{
  if (enabled)
    la_store32_rel(&arena_test_remote_drain_pause_seen, 0);
  la_store32_rel(&arena_test_remote_drain_pause_flag, enabled != 0);
}

uint32_t lj_arena_test_remote_drain_paused(void)
{
  return la_load32_acq(&arena_test_remote_drain_pause_seen);
}

static void arena_test_remote_drain_pause_after_clear(void)
{
  if (la_load32_acq(&arena_test_remote_drain_pause_flag) != 0) {
    la_store32_rel(&arena_test_remote_drain_pause_seen, 1);
    while (la_load32_acq(&arena_test_remote_drain_pause_flag) != 0)
      la_cpu_pause();
    la_store32_rel(&arena_test_remote_drain_pause_seen, 0);
  }
}

void lj_arena_test_remote_stats_reset(void)
{
  la_store64_rel(&arena_test_remote_fast_skip_count, 0);
  la_store64_rel(&arena_test_remote_arena_probe_count, 0);
}

uint64_t lj_arena_test_remote_fast_skips(void)
{
  return la_load64_acq(&arena_test_remote_fast_skip_count);
}

uint64_t lj_arena_test_remote_arena_probes(void)
{
  return la_load64_acq(&arena_test_remote_arena_probe_count);
}

uint64_t lj_arena_test_adopt_whole_count(void)
{
  return la_load64_acq(&arena_test_adopt_whole_count);
}

void lj_arena_test_gc2_sidecar_fail_alloc(int enabled)
{
  la_store32_rel(&arena_test_gc2_sidecar_fail_alloc_flag, enabled != 0);
}

static int arena_test_gc2_sidecar_alloc_fails(void)
{
  return la_load32_acq(&arena_test_gc2_sidecar_fail_alloc_flag) != 0;
}
#else
#define hugetab_test_retire_pause_after_busy() ((void)0)
#define hugetab_test_realloc_pause_after_busy() ((void)0)
#define arena_test_plain_late_pause_after_enter() ((void)0)
#define arena_test_registry_pause_after_reader() ((void)0)
#endif

#if defined(LJ_ARENA_TEST_HELPERS) || defined(LJ_GC2_TEST_HELPERS)
static uint32_t arena_test_lifetime_pause_flag;
static uint32_t arena_test_lifetime_pause_seen;

void lj_arena_test_lifetime_pause(int enabled)
{
  if (enabled)
    la_store32_rel(&arena_test_lifetime_pause_seen, 0);
  la_store32_rel(&arena_test_lifetime_pause_flag, enabled != 0);
}

uint32_t lj_arena_test_lifetime_paused(void)
{
  return la_load32_acq(&arena_test_lifetime_pause_seen);
}

static void arena_test_lifetime_pause_after_claim(void)
{
  if (la_load32_acq(&arena_test_lifetime_pause_flag) != 0) {
    la_store32_rel(&arena_test_lifetime_pause_seen, 1);
    while (la_load32_acq(&arena_test_lifetime_pause_flag) != 0)
      la_cpu_pause();
    la_store32_rel(&arena_test_lifetime_pause_seen, 0);
  }
}
#else
#define arena_test_lifetime_pause_after_claim() ((void)0)
#endif
LJ_STATIC_ASSERT((LJ_AF_HUGE_MAGIC & LJ_AF_FLAG_MASK) == 0);

/* Destructive bitmap transforms precede the allocator/free-run helpers below,
** but share their exact lifetime-release operations. */
static int arena_destruct_commit_free(GCArena *a, uint32_t cell);
static int arena_destruct_restore_live(GCArena *a, uint32_t cell);

/* Apply the 04_allocator.md sweep identities over the arena bitmaps. */
void lj_arena_sweep_words(GCArena *a, int preserve_marks)
{
  uint32_t w;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t b = la_load64_acq(&a->block[w]);
    uint64_t m = la_load64_acq(&a->mark[w]);
    uint64_t r = arena_recovery_block_bits(a, w);
    uint64_t root = arena_root_block_bits(a, w);
    uint64_t dtor = arena_dtor_block_bits(a, w);
    uint64_t live = b & (m | r | root | dtor);
    if (arena_lifetime_managed(a)) {
      uint64_t candidates = b & ~(m | r | root | dtor);
      live = b;  /* Non-LIVE/transient lanes conservatively remain allocated. */
      while (candidates) {
        uint32_t j = lj_ffs64(candidates);
        uint32_t cell = (w << 6) + j;
        uint64_t bit = (uint64_t)1 << j;
        candidates &= candidates - 1u;
        if (!lj_arena_lifetime_state_cas(a, cell,
	      LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT))
	  continue;
	arena_test_lifetime_pause_after_claim();
        if (!arena_mutation_closed_quiet(a) ||
	    !lj_arena_bm_get(a->block, cell) ||
	    lj_arena_bm_get(a->mark, cell) ||
	    lj_arena_recovery_state_acq(a, cell) !=
	      LJ_ARENA_RECOVERY_IDLE ||
	    lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE) {
	  (void)arena_destruct_restore_live(a, cell);
	  continue;
	}
	/* Exact semantic rescue and physical free arbitrate in this lane. FREE
	** rejects later readers before block/READY/body mutation begins. */
	if (!arena_destruct_commit_free(a, cell))
	  continue;
	live &= ~bit;
      }
    }
    /* Keep cdata allocation coverage until its dead span is selected as a free
    ** run or reused. Dead starts are no longer in block, so stale coverage
    ** alone cannot admit a header; preserving it here keeps live interior
    ** coverage across the bitmap transform. Recovery identity is an
    ** allocation-free liveness veto even if a carried object's cycle mark was
    ** reset before its fallback traversal ran. */
    (void)la_and64_rlx(&a->ready[w], live);
    la_store64_rel(&a->block[w], live);
    la_store64_rel(&a->mark[w], preserve_marks ? (b | m) : (b ^ m));
    (void)la_and64_rlx(&a->late[w], live);
  }
}

void lj_arena_scan_free_runs(const GCArena *a, LJArenaRunCB cb, void *ud)
{
  int32_t run_start = -1;
  uint32_t i = LJ_AFIRST_CELL;
  uint32_t token_wi = UINT32_MAX;
  uint64_t gc2_bits = 0;
  while (i < LJ_ARENA_CELLS) {
    uint32_t wi = i >> 6;
    uint64_t starts;
    uint32_t st;
    if (wi != token_wi) {
      token_wi = wi;
      gc2_bits = arena_gc2_token_block_bits(a, wi) |
	 arena_gc2_desc_block_bits(a, wi);
    }
    starts = (la_load64_rlx(&a->block[i >> 6]) |
		       la_load64_rlx(&a->mark[i >> 6]) |
		       arena_recovery_block_bits(a, i >> 6) |
		       arena_root_block_bits(a, i >> 6) |
		       arena_dtor_block_bits(a, i >> 6) |
		       arena_lifetime_block_bits(a, i >> 6) |
		       gc2_bits) >> (i & 63);
    if (!starts) {
      i = (i | 63u) + 1u;
      continue;
    }
    i += (uint32_t)__builtin_ctzll(starts);
    if (i >= LJ_ARENA_CELLS)
      break;
    st = (!arena_side_owners_none(a, i) ||
	  lj_arena_lifetime_state_acq(a, i) != LJ_ARENA_LIFETIME_FREE) ?
	  3u : lj_arena_state(a, i);
    if (st == 1) {
      if (run_start < 0)
	run_start = (int32_t)i;
    } else if (run_start >= 0) {
      cb((uint32_t)run_start, i - (uint32_t)run_start, ud);
      run_start = -1;
    }
    i++;
  }
  if (run_start >= 0)
    cb((uint32_t)run_start, LJ_ARENA_CELLS - (uint32_t)run_start, ud);
}

static void arena_count_run(uint32_t start, uint32_t len, void *ud)
{
  uint32_t *count = (uint32_t *)ud;
  UNUSED(start);
  UNUSED(len);
  (*count)++;
}

uint32_t lj_arena_count_free_runs(const GCArena *a)
{
  uint32_t count = 0;
  lj_arena_scan_free_runs(a, arena_count_run, &count);
  return count;
}

static int arena_addr_ok(uintptr_t addr, size_t size)
{
  if (size > LJ_ARENA_ADDR_LIMIT - LJ_ARENA_MMAP_LOWER)
    return 0;
  return addr >= LJ_ARENA_MMAP_LOWER &&
	 addr <= LJ_ARENA_ADDR_LIMIT - size &&
	 checkptrGC((void *)addr) &&
	 checkptrGC((void *)(addr + size - 1u));
}

static uintptr_t arena_random_hint(PRNGState *rs, size_t span)
{
  uintptr_t slots = (LJ_ARENA_ADDR_LIMIT - span) >> LJ_ARENA_SHIFT;
  uintptr_t hint;
  if (!rs)
    return 0;
  hint = (uintptr_t)(lj_prng_u64(rs) % slots) << LJ_ARENA_SHIFT;
  if (hint < LJ_ARENA_MMAP_LOWER)
    hint += LJ_ARENA_SIZE;
  return hint;
}

#if defined(_WIN32)
static void *arena_map_aligned(PRNGState *rs, size_t keep)
{
  int olderr = errno;
  uintptr_t hint = 0;
  int retry;
  if (!arena_addr_ok(LJ_ARENA_MMAP_LOWER, keep))
    return NULL;
  for (retry = 0; retry < LJ_ARENA_MMAP_PROBE_MAX; retry++) {
    void *p = VirtualAlloc(hint ? (void *)hint : NULL, keep,
			   MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    uintptr_t addr = (uintptr_t)p;
    if (p) {
      if ((addr & LJ_ARENA_MASK) == 0 && arena_addr_ok(addr, keep)) {
	errno = olderr;
	return p;
      }
      VirtualFree(p, 0, MEM_RELEASE);
    }
    if (hint && retry < LJ_ARENA_MMAP_PROBE_LINEAR) {
      hint += 0x1000000u;
      if (!arena_addr_ok(hint, keep))
	hint = 0;
      continue;
    }
    hint = arena_random_hint(rs, keep);
  }
  errno = olderr;
  return NULL;
}

static void arena_unmap_aligned(void *p, size_t size)
{
  UNUSED(size);
  VirtualFree(p, 0, MEM_RELEASE);
}

static void *arena_os_map(size_t size)
{
  return VirtualAlloc(NULL, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
}

static void arena_os_unmap(void *p, size_t size)
{
  UNUSED(size);
  VirtualFree(p, 0, MEM_RELEASE);
}
#else
static void *arena_trim(void *base, size_t span, size_t keep)
{
  uintptr_t addr = (uintptr_t)base;
  uintptr_t aligned = (addr + LJ_ARENA_MASK) & ~(uintptr_t)LJ_ARENA_MASK;
  size_t lead = (size_t)(aligned - addr);
  size_t trail = span - lead - keep;
  if (lead && munmap(base, lead) != 0) {
    munmap(base, span);
    return NULL;
  }
  if (trail && munmap((void *)(aligned + keep), trail) != 0) {
    munmap((void *)aligned, keep + trail);
    return NULL;
  }
  return (void *)aligned;
}

static void *arena_map_aligned(PRNGState *rs, size_t keep)
{
  int olderr = errno;
  size_t span = keep + LJ_ARENA_SIZE;
  uintptr_t hint = 0;
  int retry;
  if (span < keep || !arena_addr_ok(LJ_ARENA_MMAP_LOWER, span))
    return NULL;
  for (retry = 0; retry < LJ_ARENA_MMAP_PROBE_MAX; retry++) {
    void *p = mmap(hint ? (void *)hint : NULL, span,
		   PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    uintptr_t addr = (uintptr_t)p;
    if (p != MAP_FAILED) {
      if (arena_addr_ok(addr, span)) {
	void *m = arena_trim(p, span, keep);
	if (m) {
	  errno = olderr;
	  return m;
	}
      } else {
	munmap(p, span);
      }
    } else if (errno == ENOMEM) {
      errno = olderr;
      return NULL;
    }
    if (hint && retry < LJ_ARENA_MMAP_PROBE_LINEAR) {
      hint += 0x1000000u;
      if (!arena_addr_ok(hint, span))
	hint = 0;
      continue;
    }
    hint = arena_random_hint(rs, span);
  }
  errno = olderr;
  return NULL;
}

static void arena_unmap_aligned(void *p, size_t size)
{
  munmap(p, size);
}

static void *arena_os_map(size_t size)
{
  void *p = mmap(NULL, size, PROT_READ|PROT_WRITE,
		 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}

static void arena_os_unmap(void *p, size_t size)
{
  munmap(p, size);
}
#endif

GCArena *lj_arena_map(PRNGState *rs, uint32_t flags)
{
  int olderr = errno;
  GCArena *a = (GCArena *)arena_map_aligned(rs, LJ_ARENA_SIZE);
  if (a) {
    LJGC2TabStampArena *side = NULL;
    memset(a, 0, sizeof(*a));
    a->hdr.flags = flags & LJ_AF_FLAG_MASK;
    if (flags & LJ_AF_TRAVERSABLE) {
      if (!arena_test_gc2_sidecar_alloc_fails())
	side = (LJGC2TabStampArena *)calloc(1, sizeof(*side));
      if (!side) {
	arena_unmap_aligned((void *)a, LJ_ARENA_SIZE);
	errno = olderr;
	return NULL;
      }
      /* The arena is still private; release publication pairs with stamp
      ** lookup and also makes the initialization rule explicit. */
      la_storeptr_rel((void **)&a->hdr.gc2_tabstamp, side);
    }
  }
  errno = olderr;
  return a;
}

#define LJ_ARENA_REMOTE_TERMINAL \
  (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED|LJ_ARENA_REMOTE_PENDING)

/* Close fresh admissions in a word which every rescue/publisher CAS also
** modifies. terminal_closed handles losing retries; the terminal state then
** makes even a stale pre-store observation fail its admission CAS. */
static int arena_unmap_claim(GCArena *a, uint64_t *restorep)
{
  uint64_t active;
  if (!a)
    return 0;
  la_store32_rel(&a->hdr.terminal_closed, 1);
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t expect = active;
    if (arena_remote_count(active) != 0 ||
	(active & LJ_ARENA_REMOTE_SEALED)) {
      la_store32_rel(&a->hdr.terminal_closed, 0);
      return 0;
    }
    if (la_cas64(&a->hdr.remote_active, &expect,
		 LJ_ARENA_REMOTE_TERMINAL, LA_ACQ_REL, LA_ACQ)) {
      if (restorep)
	*restorep = active;
      return 1;
    }
    active = expect;
  }
}

static void arena_unmap_abandon(GCArena *a, uint64_t restore)
{
  uint64_t expect = LJ_ARENA_REMOTE_TERMINAL;
  int restored = a && la_cas64(&a->hdr.remote_active, &expect, restore,
			       LA_REL, LA_ACQ);
  lj_assertX(restored, "arena terminal-unmap claim lost");
  if (a)
    la_store32_rel(&a->hdr.terminal_closed, 0);
  UNUSED(restored);
}

int lj_arena_gc2_tokens_empty_acq(const GCArena *a)
{
  LJGC2TabStampArena *side;
  uint32_t flags, cell;
  if (!a)
    return 0;
  flags = lj_arena_flags_acq(a);
  if ((flags & LJ_AF_HUGE_MAGIC) == LJ_AF_HUGE_MAGIC) {
    uint64_t control = la_load64_acq(&a->hdr.huge_tabstamp.token.control);
    return lj_gc2_table_token_state(control) == LJ_GC2_TABLE_TOKEN_NONE;
  }
  side = lj_arena_gc2_tabstamp_acq(a);
  if (!(flags & LJ_AF_TRAVERSABLE))
    return side == NULL;
  /* A traversable mapping without its eagerly published sidecar violates the
  ** mapping invariant. Fail closed rather than erase a possible token. */
  if (!side)
    return 0;
  for (cell = 0; cell < LJ_ARENA_CELLS; cell++) {
    uint64_t control = la_load64_acq(&side->cell[cell].token.control);
    if (lj_gc2_table_token_state(control) != LJ_GC2_TABLE_TOKEN_NONE)
      return 0;
  }
  return 1;
}

static LJ_AINLINE int arena_unmap_side_empty(const GCArena *a)
{
  return a && lj_arena_recovery_empty(a) && arena_root_empty(a) &&
    lj_arena_lifetime_empty(a) && arena_dtor_empty(a) &&
    lj_arena_gc2_tokens_empty_acq(a) &&
    lj_arena_gc2_desc_mapping_clear_acq(a);
}

static void arena_unmap_claimed(GCArena *a)
{
  free(lj_arena_gc2_tabstamp_acq(a));
  arena_unmap_aligned((void *)a, LJ_ARENA_SIZE);
}

void lj_arena_unmap(GCArena *a)
{
  int olderr = errno;
  if (a) {
    uint64_t restore;
    /* Runtime teardown must first drain or explicitly discard recovery work.
    ** Conservatively retain a violated mapping rather than erasing its only
    ** durable traversal identity. */
    if ((lj_arena_flags_acq(a) & LJ_AF_REGISTERED) ||
	!arena_unmap_claim(a, &restore)) {
      errno = olderr;
      return;
    }
    if (!arena_unmap_side_empty(a)) {
      arena_unmap_abandon(a, restore);
      errno = olderr;
      return;
    }
    arena_unmap_claimed(a);
  }
  errno = olderr;
}

size_t lj_arena_huge_mapsize(size_t size)
{
  size_t need = size + sizeof(GCAhdr);
  if (size <= LJ_HUGE_THRESHOLD ||
      need < size || need > ~(size_t)LJ_ARENA_MASK)
    return 0;
  return (need + LJ_ARENA_MASK) & ~(size_t)LJ_ARENA_MASK;
}

void *lj_arena_huge_map(PRNGState *rs, size_t size, uint32_t flags)
{
  size_t mapsize = lj_arena_huge_mapsize(size);
  GCArena *a;
  if (!mapsize)
    return NULL;
  a = (GCArena *)arena_map_aligned(rs, mapsize);
  if (!a)
    return NULL;
  memset(&a->hdr, 0, sizeof(a->hdr));
  a->hdr.flags = LJ_AF_HUGE_MAGIC | (flags & LJ_AF_FLAG_MASK);
  a->hdr.live_cells = (uint32_t)(mapsize >> LJ_CELL_SHIFT);
  return (void *)((char *)a + sizeof(GCAhdr));
}

void lj_arena_huge_unmap(void *p, size_t size)
{
  int olderr = errno;
  size_t mapsize = lj_arena_huge_mapsize(size);
  if (p && mapsize) {
    GCArena *a = lj_arena_of(p);
    /* Last-resort mapping certificate. Normal callers prove this before
    ** deleting the HugeTab locator; retain a violated mapping rather than
    ** erasing a live scan owner if a terminal caller regresses. */
    if (lj_arena_gc2_tokens_empty_acq(a) &&
	lj_arena_gc2_desc_mapping_clear_acq(a))
      arena_unmap_aligned((void *)a, mapsize);
  }
  errno = olderr;
}

void lj_arena_huge_unmap_claimed(void *p, size_t size)
{
  int olderr = errno;
  size_t mapsize = lj_arena_huge_mapsize(size);
  if (p && mapsize)
    arena_unmap_aligned((void *)lj_arena_of(p), mapsize);
  errno = olderr;
}

static size_t hugetab_mapsize(uint32_t hbits)
{
  size_t cap, hdr = offsetof(LJHugeTabHdr, ent);
  if (hbits > LJ_HUGETAB_MAX_BITS)
    return 0;
  cap = (size_t)1 << hbits;
  if (cap > (~(size_t)0 - hdr) / sizeof(LJHugeEnt))
    return 0;
  return hdr + cap * sizeof(LJHugeEnt);
}

static uint32_t hugetab_hash(uint64_t addr, uint32_t mask)
{
  uint64_t x = addr >> LJ_CELL_SHIFT;
  x ^= x >> 33;
  x *= U64x(ff51afd7,ed558ccd);
  x ^= x >> 33;
  x *= U64x(c4ceb9fe,1a85ec53);
  x ^= x >> 33;
  return (uint32_t)x & mask;
}

static int hugetab_pack(void *p, size_t size, uint32_t hflags,
			uint64_t *addr, uint64_t *meta)
{
  uintptr_t u = (uintptr_t)p;
  if (!p || u <= LJ_HUGETAB_TOMBSTONE || !lj_arena_huge_mapsize(size) ||
      (hflags & ~LJ_HUGEF_MASK) != 0 ||
      size > UINT32_MAX)
    return 0;
  *addr = (uint64_t)u;
  *meta = ((uint64_t)size << LJ_HUGETAB_SIZE_SHIFT) | (uint64_t)hflags;
  return 1;
}

static LJ_AINLINE size_t hugetab_size(uint64_t meta)
{
  return (size_t)((meta & LJ_HUGETAB_SIZE_MASK) >> LJ_HUGETAB_SIZE_SHIFT);
}

static LJ_AINLINE uint32_t hugetab_readers(uint64_t meta)
{
  return (uint32_t)((meta & LJ_HUGETAB_READER_MASK) >>
		    LJ_HUGETAB_READER_SHIFT);
}

static LJ_AINLINE int hugetab_admission_closed(uint64_t meta)
{
  return hugetab_readers(meta) == LJ_HUGETAB_READER_CLOSED;
}

static void hugetab_decode(uint64_t meta, LJHugeInfo *hi)
{
  if (hi) {
    hi->size = hugetab_size(meta);
    hi->flags = (uint32_t)(meta & LJ_HUGETAB_META_MASK);
    hi->readers = hugetab_readers(meta);
  }
}

static LJ_AINLINE uint32_t hugetab_recovery_state(uint64_t meta)
{
  return ((uint32_t)meta & LJ_HUGEF_RECOVERY_MASK) >>
    LJ_HUGEF_RECOVERY_SHIFT;
}

static LJ_AINLINE int hugetab_recovery_pending(uint64_t meta)
{
  return (meta & LJ_HUGEF_RECOVERY_MASK) != 0;
}

static LJ_AINLINE uint32_t hugetab_root_state(uint64_t meta)
{
  return ((uint32_t)meta & LJ_HUGEF_ROOT_MASK) >> LJ_HUGEF_ROOT_SHIFT;
}

static LJ_AINLINE int hugetab_root_pending(uint64_t meta)
{
  return (meta & LJ_HUGEF_ROOT_MASK) != 0;
}

/* The HugeTab slot retains mapping lifetime while this exact pointer-form
** certificate is sampled. Plain mappings have no table-scan authority;
** traversable mappings require both descriptor clearance and embedded token
** NONE before a transition can expose destruction, reuse, or locator loss. */
static LJ_AINLINE int hugetab_gc2_reclaim_clear_acq(const void *p,
						     uint64_t meta)
{
  GCArena *a;
  if (!p)
    return 0;
  /* Plain entries cannot publish a GC2 table traversal, and HugeTab accepts
  ** synthetic addresses in metadata-only users/tests. Do not derive and
  ** dereference a mapping header unless the slot itself says the allocation
  ** is traversable. */
  if (!(meta & LJ_HUGEF_TRAVERSABLE))
    return 1;
  a = lj_arena_of(p);
  /* The same HugeTab implementation is also the small-arena registry. Its
  ** key is the mapping base (cell zero), so it requires a mapping-wide
  ** certificate rather than the one-huge-body exact-pointer predicate. */
  if (p == (const void *)a)
    return lj_arena_gc2_tokens_empty_acq(a) &&
      lj_arena_gc2_desc_mapping_clear_acq(a);
  return lj_arena_gc2_reclaim_ptr_clear_acq(a, p);
}

enum {
  LJ_HUGETAB_FOLD_NONE = 0,
  LJ_HUGETAB_FOLD_BLOCKED = 1,
  LJ_HUGETAB_FOLD_SCHEDULED = 2,
  LJ_HUGETAB_FOLD_FREEING = 3
};

/* Complete an irrevocable logical free only after the final slot-local owner
** has left. The fresh sentinel is release-published before the full-slot CAS
** which exposes FREEING without DEFER_FREE. A failed CAS may leave an earlier
** sentinel, which is conservative and is repaired by any later live finish.
** The caller must already retain the mapping through a reader/root/recovery/
** BUSY claim; a stable HugeTab lookup alone is not a header lifetime lease. */
static uint64_t hugetab_fold_deferred_free(const void *p, uint64_t meta,
					    int *progressp)
{
  int eligible = (meta & LJ_HUGEF_DEFER_FREE) != 0 &&
    hugetab_readers(meta) == 0 && !hugetab_recovery_pending(meta) &&
    !hugetab_root_pending(meta) && !(meta & LJ_HUGEF_BUSY);
  int folded = eligible && hugetab_gc2_reclaim_clear_acq(p, meta);
  int scheduled = eligible && !folded && !(meta & LJ_HUGEF_SWEEP_OLD);
  if (folded) {
    la_store64_rel(&lj_arena_of(p)->hdr.retire_epoch, ~(uint64_t)0);
    meta = (meta & ~(uint64_t)(LJ_HUGEF_DEFER_FREE|LJ_HUGEF_MARK|
			       LJ_HUGEF_RETIRED|LJ_HUGEF_BUSY)) |
	   LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD;
  } else if (eligible) {
    /* No slot-local owner remains, but a physical table scan still names the
    ** mapping. Keep the logical free opaque and make it discoverable by the
    ** bounded huge sweep, which retries only this certificate while DEFER is
    ** present. The successful retry publishes the fresh-grace sentinel. */
    meta |= LJ_HUGEF_SWEEP_OLD;
  }
  if (progressp)
    *progressp = folded ? LJ_HUGETAB_FOLD_FREEING :
      (scheduled ? LJ_HUGETAB_FOLD_SCHEDULED :
       (eligible ? LJ_HUGETAB_FOLD_BLOCKED : LJ_HUGETAB_FOLD_NONE));
  return meta;
}

static int hugetab_search(LJHugeTabHdr *h, uint64_t addr,
			  LJHugeEnt **ep, uint64_t *metap)
{
  uint32_t cap = h->mask + 1u;
  uint32_t i = hugetab_hash(addr, h->mask);
  uint32_t n;
  for (n = 0; n < cap; n++, i = (i + 1u) & h->mask) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t ea = la_load64_acq(&e->slot.lo);  /* 04 §4.5.1 publish edge. */
    if (ea == LJ_HUGETAB_EMPTY)
      return 0;
    if (ea == addr) {
      uint64_t meta = la_load64_acq(&e->slot.hi);  /* 04 §4.5.1 metadata. */
      if (la_load64_acq(&e->slot.lo) == addr) {  /* Stable found snapshot. */
	if (ep)
	  *ep = e;
	if (metap)
	  *metap = meta;
	return 1;
      }
    }
  }
  return 0;
}

/* Metadata transitions which confer mapping/header ownership must also prove
** that the address half of the open-addressed slot is unchanged. A 64-bit
** metadata CAS after hugetab_search() can otherwise mutate a reused slot, or
** race a 128-bit delete which has already made the mapping unaddressable. */
static int hugetab_cas_meta(LJHugeEnt *e, uint64_t addr, uint64_t oldmeta,
			    uint64_t newmeta)
{
  la_u128 exp, des;
  exp.lo = addr;
  exp.hi = oldmeta;
  des.lo = addr;
  des.hi = newmeta;
  return la_cas128(&e->slot, &exp, des);
}

/* Acquire one internal counted mapping lease from an exact stable slot. No
** mapping byte is read before this full-slot CAS. A destructive transition
** consumes the lease by requiring reader count one in its own full-slot CAS;
** a nonterminal path releases it with the helper below. */
static int hugetab_cert_lease_acquire(LJHugeEnt *e, uint64_t addr,
				      uint64_t meta, uint64_t *leasedp)
{
  uint64_t leased;
  if (hugetab_readers(meta) != 0)
    return 0;
  leased = meta + LJ_HUGETAB_READER_ONE;
  if (!hugetab_cas_meta(e, addr, meta, leased))
    return -1;
  if (leasedp)
    *leasedp = leased;
  return 1;
}

static int hugetab_cert_lease_release(LJHugeEnt *e, uint64_t addr,
				      const void *p, LJHugeInfo *hi)
{
  for (;;) {
    uint64_t meta, next;
    int progress;
    if (la_load64_acq(&e->slot.lo) != addr)
      return 0;
    meta = la_load64_acq(&e->slot.hi);
    if (la_load64_acq(&e->slot.lo) != addr ||
	hugetab_readers(meta) == 0 || hugetab_admission_closed(meta))
      return 0;
    next = meta - LJ_HUGETAB_READER_ONE;
    next = hugetab_fold_deferred_free(p, next, &progress);
    if (!hugetab_cas_meta(e, addr, meta, next))
      continue;
    hugetab_decode(next, hi);
    if (progress != LJ_HUGETAB_FOLD_NONE)
      arena_progress_wake(lj_arena_of(p));
    return 1;
  }
}

/* Consume this caller's counted lease while durably publishing an irrevocable
** logical free. The actual slot remains reader-pinned while the held-only fold
** samples descriptor/token state; another reader/root/recovery owner simply
** carries DEFER_FREE onward to its own release. */
static int hugetab_cert_lease_defer(LJHugeEnt *e, uint64_t addr,
				    const void *p, LJHugeInfo *hi,
				    int *progressp)
{
  for (;;) {
    uint64_t meta, next;
    int progress;
    if (la_load64_acq(&e->slot.lo) != addr)
      return 0;
    meta = la_load64_acq(&e->slot.hi);
    if (la_load64_acq(&e->slot.lo) != addr ||
	hugetab_readers(meta) == 0 || hugetab_admission_closed(meta))
      return 0;
    next = (meta - LJ_HUGETAB_READER_ONE) | LJ_HUGEF_DEFER_FREE;
    next = hugetab_fold_deferred_free(p, next, &progress);
    if (!hugetab_cas_meta(e, addr, meta, next))
      continue;
    hugetab_decode(next, hi);
    if (progressp)
      *progressp = progress;
    if (progress != LJ_HUGETAB_FOLD_NONE)
      arena_progress_wake(lj_arena_of(p));
    return 1;
  }
}

/* Replace the caller's sole counted certificate lease with an exact reader
** admission close. A publisher which is still admitted makes the CAS lose;
** a publisher which completed a reader-count ABA before the CAS is observed
** by the token/descriptor validation that the destructive caller performs
** only after this close succeeds. No later publisher can enter until reopen.
**
** The all-ones reader encoding is never a counted lease. Every ordinary
** admission rejects it before addition, so a successful CAS consumes exactly
** this caller's count and cannot be mistaken for saturation. */
static int hugetab_cert_lease_close(LJHugeEnt *e, uint64_t addr,
				     uint64_t *closedp)
{
  for (;;) {
    uint64_t current, closed;
    if (la_load64_acq(&e->slot.lo) != addr)
      return 0;
    current = la_load64_acq(&e->slot.hi);
    if (la_load64_acq(&e->slot.lo) != addr ||
	hugetab_readers(current) != 1 || hugetab_admission_closed(current))
      return 0;
    closed = (current & ~(uint64_t)LJ_HUGETAB_READER_MASK) |
	LJ_HUGETAB_ADMISSION_CLOSED;
    hugetab_test_admission_close_pause_after_snapshot();
    if (hugetab_cas_meta(e, addr, current, closed)) {
      hugetab_test_admission_close_pause_after_close();
      if (closedp)
	*closedp = closed;
      return 1;
    }
  }
}

/* Roll a vetoed close back to an ordinary zero-reader slot while preserving
** every concurrent flag/state update. DEFER_FREE may have arrived while the
** close was held; fold it only after reopening and publish the normal wake. */
static int hugetab_admission_reopen(LJHugeEnt *e, uint64_t addr,
				    const void *p, LJHugeInfo *hi)
{
  for (;;) {
    uint64_t current, next;
    int progress;
    if (la_load64_acq(&e->slot.lo) != addr)
      return 0;
    current = la_load64_acq(&e->slot.hi);
    if (la_load64_acq(&e->slot.lo) != addr ||
	!hugetab_admission_closed(current))
      return 0;
    next = current & ~(uint64_t)LJ_HUGETAB_READER_MASK;
    next = hugetab_fold_deferred_free(p, next, &progress);
    if (!hugetab_cas_meta(e, addr, current, next))
      continue;
    hugetab_decode(next, hi);
    if (progress != LJ_HUGETAB_FOLD_NONE)
      arena_progress_wake(lj_arena_of(p));
    return 1;
  }
}

static int hugetab_has_lifetime_claim(LJHugeTabHdr *h)
{
  uint32_t i, cap;
  if (!h)
    return 0;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);
    /* Ordinary fini only releases an empty side table. Any live slot is an
    ** authoritative locator regardless of its transient metadata, and must
    ** be transferred/tombstoned by the terminal owner before table unmap. */
    if (addr > LJ_HUGETAB_TOMBSTONE)
      return 1;
  }
  return 0;
}

int lj_arena_hugetab_init(HugeTab *ht, uint32_t hbits)
{
  int olderr = errno;
  size_t mapsize = hugetab_mapsize(hbits);
  LJHugeTabHdr *h;
  if (!ht || ht->h || !mapsize)
    return 0;
  h = (LJHugeTabHdr *)arena_os_map(mapsize);
  if (!h)
    return 0;
  h->hbits = hbits;
  h->mask = (1u << hbits) - 1u;
  h->mapsize = mapsize;
  /* A recycled private wrapper must be rebound explicitly after init. */
  la_storeptr_rlx((void **)&ht->table_topology, NULL);
  ht->h = h;
  errno = olderr;
  return 1;
}

void lj_arena_hugetab_bind_table_topology(
  HugeTab *ht, LJGC2TableTopology *topology)
{
  LJGC2TableTopology *old;
  LJGC2TableTopologySnap snap;
  if (!ht || !ht->h || !topology)
    abort();
  snap = lj_gc2_table_topology_snapshot(topology);
  if (!snap.valid)
    abort();  /* Binding before global authority initialization is corruption. */
  old = (LJGC2TableTopology *)la_loadptr_acq(
    (void *const *)&ht->table_topology);
  if (old && old != topology)
    abort();  /* One published directory belongs to exactly one universe. */
  if (!old)
    la_storeptr_rel((void **)&ht->table_topology, topology);
}

int lj_arena_hugetab_fini_try(HugeTab *ht)
{
  int olderr = errno;
  if (ht && ht->h) {
    LJHugeTabHdr *h = ht->h;
    size_t mapsize = h->mapsize;
    if (hugetab_has_lifetime_claim(h)) {
      errno = olderr;
      return 0;  /* Retain the authoritative locator until recovery drains. */
    }
    ht->h = NULL;
    arena_os_unmap((void *)h, mapsize);
  }
  errno = olderr;
  return 1;
}

void lj_arena_hugetab_fini(HugeTab *ht)
{
  (void)lj_arena_hugetab_fini_try(ht);
}

int lj_arena_hugetab_fini_all_try(HugeTab *ht, uint32_t *unmappedp)
{
  int olderr = errno;
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap, unmapped = 0, blocked = 0;
  if (unmappedp)
    *unmappedp = 0;
  if (!h) {
    errno = olderr;
    return 1;
  }
  /* Terminal single-owner destruction. Ordinary fini deliberately releases
  ** only the side table because live mappings may already have transferred to
  ** another allocator. Here every slot is detached first and a mapping is
  ** unmapped exactly once. Dead-allocator transfer transactionally tombstones
  ** each source slot before changing the mapping header owner, so a later
  ** destination-capacity failure leaves every moved prefix destination-only
  ** and every unmoved suffix source-only. Thus this table is authoritative and
  ** no possibly stale mapping header need be sampled here. BUSY/FREEING/
  ** RETIRED are runtime arbitration states; after freeall and the terminal
  ** registry grace there is no actor left to complete them, so the full-slot
  ** CAS supersedes all of them. */
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    for (;;) {
      uint64_t addr = la_load64_acq(&e->slot.lo);
      uint64_t meta, leased, closed;
      int lease, removed = 0;
      la_u128 exp, des;
      if (addr <= LJ_HUGETAB_TOMBSTONE)
	break;
      meta = la_load64_acq(&e->slot.hi);
      if (la_load64_acq(&e->slot.lo) != addr)
	continue;
      if (hugetab_recovery_pending(meta) || hugetab_root_pending(meta) ||
	  hugetab_readers(meta) != 0) {
	blocked = 1;
	break;  /* A lifetime owner still names this exact mapping. */
      }
      lease = hugetab_cert_lease_acquire(e, addr, meta, &leased);
      if (lease < 0)
	continue;
      if (lease == 0) {
	blocked = 1;
	break;
      }
      if (!hugetab_cert_lease_close(e, addr, &closed)) {
	(void)hugetab_cert_lease_release(e, addr,
	  (const void *)(uintptr_t)addr, NULL);
	blocked = 1;
	break;
      }
      for (;;) {
	uint64_t current = la_load64_acq(&e->slot.hi);
	if (la_load64_acq(&e->slot.lo) != addr ||
	    !hugetab_admission_closed(current) ||
	    hugetab_recovery_pending(current) ||
	    hugetab_root_pending(current) ||
	    !hugetab_gc2_reclaim_clear_acq(
	      (const void *)(uintptr_t)addr, current)) {
	  (void)hugetab_admission_reopen(e, addr,
	    (const void *)(uintptr_t)addr, NULL);
	  blocked = 1;
	  break;
	}
	exp.lo = addr;
	exp.hi = current;
	des.lo = LJ_HUGETAB_TOMBSTONE;
	des.hi = 0;
	if (!la_cas128(&e->slot, &exp, des))
	  continue;
	closed = current;
	removed = 1;
	break;
      }
      if (!removed)
	break;
      hugetab_table_topology_changed(ht);
      {
	size_t size = hugetab_size(closed);
	lj_arena_huge_unmap_claimed((void *)(uintptr_t)addr, size);
	unmapped++;
      }
      break;
    }
  }
  /* Keep the table itself mapped while any recovery entry remains. A later
  ** terminal retry may clear/discard that state and finish the exact suffix;
  ** dropping the table now would erase the only locator for retained maps. */
  if (!blocked && !lj_arena_hugetab_fini_try(ht))
    blocked = 1;
  if (unmappedp)
    *unmappedp = unmapped;
  errno = olderr;
  return !blocked && (!ht || ht->h == NULL);
}

uint32_t lj_arena_hugetab_fini_all(HugeTab *ht)
{
  uint32_t unmapped = 0;
  (void)lj_arena_hugetab_fini_all_try(ht, &unmapped);
  return unmapped;
}

int lj_arena_hugetab_terminal_certificate_ready(HugeTab *ht)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap;
  if (!h)
    return 1;
  /* This is earlier than object freeall, so root/recovery/destructor owners
  ** may legitimately remain. The caller has joined every thread and worker;
  ** that outer quiescence retains each live slot while this read-only pass
  ** rejects only table-scan authority that freeall cannot safely overtake. */
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    for (;;) {
      uint64_t addr = la_load64_acq(&e->slot.lo);
      uint64_t meta;
      if (addr <= LJ_HUGETAB_TOMBSTONE)
	break;
      meta = la_load64_acq(&e->slot.hi);
      if (la_load64_acq(&e->slot.lo) != addr)
	continue;
      /* Root/recovery/destructor lanes are intentionally left for freeall,
      ** but an ordinary counted reader is external byte authority. Recovery
      ** plus DEFER/root is also not terminal-discardable without that owner. */
      if (hugetab_readers(meta) != 0 ||
	  (hugetab_recovery_pending(meta) &&
	   (meta & LJ_HUGEF_DEFER_FREE) && hugetab_root_pending(meta)))
	return 0;
      if (!hugetab_gc2_reclaim_clear_acq(
	    (const void *)(uintptr_t)addr, meta))
	return 0;
      break;
    }
  }
  return ht->h == h;
}

int lj_arena_hugetab_terminal_ready(HugeTab *ht)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap;
  if (!h)
    return 1;
  /* Joined-world preflight still takes the ordinary counted lease: this keeps
  ** the certificate proof local to the same protocol as destructive teardown
  ** and makes an accidental lawful publisher lose one exact slot CAS. The
  ** wrapper itself remains protected by the caller's joined-world contract. */
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    for (;;) {
      uint64_t addr = la_load64_acq(&e->slot.lo);
      uint64_t meta, leased;
      int lease, clear;
      if (addr <= LJ_HUGETAB_TOMBSTONE)
	break;
      meta = la_load64_acq(&e->slot.hi);
      if (la_load64_acq(&e->slot.lo) != addr)
	continue;
      if (hugetab_recovery_pending(meta) || hugetab_root_pending(meta) ||
	  hugetab_readers(meta) != 0)
	return 0;
      lease = hugetab_cert_lease_acquire(e, addr, meta, &leased);
      if (lease < 0)
	continue;
      if (lease == 0)
	return 0;
      clear = hugetab_gc2_reclaim_clear_acq(
	(const void *)(uintptr_t)addr, leased);
      if (!hugetab_cert_lease_release(e, addr,
	    (const void *)(uintptr_t)addr, NULL))
	return 0;
      if (!clear)
	return 0;
      break;
    }
  }
  return ht->h == h;
}

static int hugetab_forget_terminal(HugeTab *ht, const void *p,
				    LJHugeInfo *hi, int allow_root,
				    int alternate_locator)
{
  int olderr = errno;
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t leased, closed;
    int lease;
    la_u128 exp, des;
    if (!hugetab_search(h, addr, &e, &meta)) {
      errno = olderr;
      return 0;
    }
    if (hugetab_readers(meta) != 0 || hugetab_recovery_pending(meta) ||
	(!allow_root && hugetab_root_pending(meta))) {
      errno = olderr;
      return 0;
    }
    lease = hugetab_cert_lease_acquire(e, addr, meta, &leased);
    if (lease < 0)
      continue;
    if (lease == 0) {
      errno = olderr;
      return 0;
    }
    if (!hugetab_cert_lease_close(e, addr, &closed)) {
      (void)hugetab_cert_lease_release(e, addr, p, NULL);
      errno = olderr;
      return 0;
    }
    for (;;) {
      uint64_t current = la_load64_acq(&e->slot.hi);
      if (la_load64_acq(&e->slot.lo) != addr ||
	  !hugetab_admission_closed(current) ||
	  hugetab_recovery_pending(current) ||
	  (!allow_root && hugetab_root_pending(current)) ||
	  (!alternate_locator &&
	   !hugetab_gc2_reclaim_clear_acq(p, current))) {
	(void)hugetab_admission_reopen(e, addr, p, NULL);
	errno = olderr;
	return 0;
      }
      exp.lo = addr;
      exp.hi = current;
      des.lo = LJ_HUGETAB_TOMBSTONE;
      des.hi = 0;
      if (!la_cas128(&e->slot, &exp, des))
	continue;
      hugetab_table_topology_changed(ht);
      hugetab_decode(current & ~(uint64_t)LJ_HUGETAB_READER_MASK, hi);
      errno = olderr;
      return 1;
    }
  }
}

int lj_arena_hugetab_forget_terminal(HugeTab *ht, const void *p,
				      LJHugeInfo *hi)
{
  return hugetab_forget_terminal(ht, p, hi, 0, 0);
}

static int hugetab_insert(HugeTab *ht, void *p, size_t size,
			  uint32_t hflags, int allow_root)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  int root_construct = (hflags & LJ_AF_ROOT_CONSTRUCT) != 0;
  if (root_construct && (hflags & LJ_HUGEF_ROOT_MASK))
    return -1;
  hflags &= ~LJ_AF_ROOT_CONSTRUCT;
  if (root_construct)
    hflags |= (uint32_t)LJ_ARENA_ROOT_LINKING << LJ_HUGEF_ROOT_SHIFT;
  if (!h || (!allow_root && (hflags & LJ_HUGEF_ROOT_MASK)) ||
      !hugetab_pack(p, size, hflags, &addr, &meta))
    return -1;
  for (;;) {
    uint32_t cap = h->mask + 1u;
    uint32_t i = hugetab_hash(addr, h->mask);
    uint32_t n;
    LJHugeEnt *freeent = NULL;
    la_u128 freeval, des;
    for (n = 0; n < cap; n++, i = (i + 1u) & h->mask) {
      LJHugeEnt *e = &h->ent[i];
      uint64_t ea = la_load64_acq(&e->slot.lo);  /* 04 §4.5.1 slot state. */
      if (ea == addr)
	return 0;
      if (ea == LJ_HUGETAB_EMPTY || ea == LJ_HUGETAB_TOMBSTONE) {
	uint64_t emeta = la_load64_acq(&e->slot.hi);  /* 04 §4.5.1 CAS pair. */
	if (!freeent) {
	  freeent = e;
	  freeval.lo = ea;
	  freeval.hi = emeta;
	}
	if (ea == LJ_HUGETAB_EMPTY)
	  break;
      }
    }
    if (!freeent)
      return -1;
    /* A bound directory is part of the complete table-token universe. Its
    ** membership LP may expose only an identity whose token is NONE and whose
    ** exact descriptor is clear. This check is immediately before publish;
    ** construction is private, while transfer holds the source admission
    ** close, so no lawful publisher can create authority in the remaining
    ** interval. Unbound standalone/synthetic tables retain legacy behavior. */
    if ((meta & LJ_HUGEF_TRAVERSABLE) &&
	hugetab_table_topology_acq(ht) != NULL &&
	!hugetab_gc2_reclaim_clear_acq(p, meta))
      return -1;
    des.lo = addr;
    des.hi = meta;
    if (la_cas128(&freeent->slot, &freeval, des)) {  /* 04 §4.5.1 publish. */
      hugetab_table_topology_changed(ht);
      return 1;
    }
  }
}

int lj_arena_hugetab_insert(HugeTab *ht, void *p, size_t size,
			    uint32_t hflags)
{
  return hugetab_insert(ht, p, size, hflags,
			(hflags & LJ_AF_ROOT_CONSTRUCT) != 0);
}

int lj_arena_hugetab_lookup(HugeTab *ht, const void *p, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  if (hugetab_search(h, addr, NULL, &meta)) {
    if (hi)
      hugetab_decode(meta, hi);
    return 1;
  }
  return 0;
}

int lj_arena_hugetab_recovery_state_acq(HugeTab *ht, const void *p,
						 LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  if (!h || !p)
    return -1;
  addr = (uint64_t)(uintptr_t)p;
  if (!hugetab_search(h, addr, NULL, &meta))
    return -1;
  hugetab_decode(meta, hi);
  return (int)hugetab_recovery_state(meta);
}

int lj_arena_hugetab_recovery_state_cas(HugeTab *ht, const void *p,
						 uint32_t from, uint32_t to,
						 LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  LJHugeEnt *e;
  uint64_t addr, meta;
  if (!h || !p || from > LJ_ARENA_RECOVERY_REDIRTY ||
      to > LJ_ARENA_RECOVERY_REDIRTY)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta) ||
	hugetab_recovery_state(meta) != from)
      return 0;
    /* New fallback work may only name an initialized traversable mapping.
    ** FREEING owns terminal destruction, while non-sweep BUSY is realloc and
    ** has no GC traversal-discharge contract. Existing non-IDLE work already
    ** vetoes both claims and may always advance/clear its own state. */
    if (from == LJ_ARENA_RECOVERY_IDLE &&
	to != LJ_ARENA_RECOVERY_IDLE &&
	((meta & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) !=
	   (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY) ||
	 (meta & (LJ_HUGEF_FREEING|LJ_HUGEF_DEFER_FREE)) ||
	 ((meta & LJ_HUGEF_BUSY) && !(meta & LJ_HUGEF_SWEEP_OLD))))
      return 0;
    /* Only recovery_complete may clear this owner while preserving/folding a
    ** deferred free. With counted readers it may intentionally publish the
    ** IDLE|DEFER_FREE handoff consumed by the last reader. */
    if (to == LJ_ARENA_RECOVERY_IDLE &&
	(meta & LJ_HUGEF_DEFER_FREE))
      return 0;
    next = (meta & ~(uint64_t)LJ_HUGEF_RECOVERY_MASK) |
	((uint64_t)to << LJ_HUGEF_RECOVERY_SHIFT);
    if (to != LJ_ARENA_RECOVERY_IDLE)
      next = (next | LJ_HUGEF_MARK) & ~(uint64_t)LJ_HUGEF_RETIRED;
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      return 1;
    }
  }
}

int lj_arena_hugetab_root_state_acq(HugeTab *ht, const void *p,
					     LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  if (!h || !p)
    return -1;
  addr = (uint64_t)(uintptr_t)p;
  if (!hugetab_search(h, addr, NULL, &meta))
    return -1;
  hugetab_decode(meta, hi);
  return (int)hugetab_root_state(meta);
}

static int hugetab_root_complete(HugeTab *ht, const void *p,
				 uint32_t from, uint32_t to,
				 uint64_t retire_epoch, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  LJHugeEnt *e;
  uint64_t addr, meta;
  if (!h || !p || from > LJ_ARENA_ROOT_MEMBER ||
      to > LJ_ARENA_ROOT_MEMBER)
    return LJ_ARENA_HUGE_ROOT_COMPLETE_LOST;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta) ||
	 hugetab_root_state(meta) != from)
      return LJ_ARENA_HUGE_ROOT_COMPLETE_LOST;
    /* A fresh membership claim and a terminal/realloc ownership claim share
    ** this exact metadata word. Once BUSY/FREEING wins, no new intrusive root
    ** may begin naming bytes that operation can release. Existing root owners
    ** may still advance or clear their own state. */
    if (from == LJ_ARENA_ROOT_NONE && to != LJ_ARENA_ROOT_NONE &&
	(meta & (LJ_HUGEF_BUSY|LJ_HUGEF_FREEING|LJ_HUGEF_DEFER_FREE)))
      return LJ_ARENA_HUGE_ROOT_COMPLETE_LOST;
    if (from != LJ_ARENA_ROOT_NONE && to == LJ_ARENA_ROOT_NONE &&
	(meta & LJ_HUGEF_DEFER_FREE)) {
      int progress;
      /* Relinquish root ownership in either completion order. A remaining
      ** reader/recovery/BUSY owner keeps DEFER_FREE durable; its final CAS is
      ** then responsible for the same fold. */
      next = meta & ~(uint64_t)LJ_HUGEF_ROOT_MASK;
      next = hugetab_fold_deferred_free(p, next, &progress);
      if (progress == LJ_HUGETAB_FOLD_FREEING && retire_epoch)
	la_store64_rel(&lj_arena_of(p)->hdr.retire_epoch, retire_epoch);
      if (hugetab_cas_meta(e, addr, meta, next)) {
	hugetab_decode(next, hi);
	if (progress != LJ_HUGETAB_FOLD_NONE)
	  arena_progress_wake(lj_arena_of(p));
	return progress == LJ_HUGETAB_FOLD_FREEING ?
	       LJ_ARENA_HUGE_ROOT_COMPLETE_SWEEP :
	       LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE;
      }
      continue;
    }
    /* Never manufacture the orphaned NONE|DEFER_FREE combination, even for
    ** a malformed NONE->NONE request. */
    if (to == LJ_ARENA_ROOT_NONE && (meta & LJ_HUGEF_DEFER_FREE))
      return LJ_ARENA_HUGE_ROOT_COMPLETE_LOST;
    next = (meta & ~(uint64_t)LJ_HUGEF_ROOT_MASK) |
	((uint64_t)to << LJ_HUGEF_ROOT_SHIFT);
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      return LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE;
    }
  }
}

int lj_arena_hugetab_root_state_cas(HugeTab *ht, const void *p,
					     uint32_t from, uint32_t to,
					     LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  LJHugeEnt *e;
  uint64_t addr, meta;
  if (!h || !p || from > LJ_ARENA_ROOT_MEMBER ||
      to > LJ_ARENA_ROOT_MEMBER)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta) ||
	hugetab_root_state(meta) != from)
      return 0;
    if (from == LJ_ARENA_ROOT_NONE && to != LJ_ARENA_ROOT_NONE &&
	(meta & (LJ_HUGEF_BUSY|LJ_HUGEF_FREEING|LJ_HUGEF_DEFER_FREE)))
      return 0;
    /* The masked CAS API never changes unrelated bits. A logical-free fold
    ** requires root_complete so its caller observes SWEEP and publishes the
    ** corresponding wake/grace request. */
    if (to == LJ_ARENA_ROOT_NONE && (meta & LJ_HUGEF_DEFER_FREE))
      return 0;
    next = (meta & ~(uint64_t)LJ_HUGEF_ROOT_MASK) |
	((uint64_t)to << LJ_HUGEF_ROOT_SHIFT);
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      return 1;
    }
  }
}

int lj_arena_hugetab_root_complete(HugeTab *ht, const void *p,
					    uint32_t from, uint32_t to,
					    uint64_t retire_epoch,
					    LJHugeInfo *hi)
{
  return hugetab_root_complete(ht, p, from, to, retire_epoch, hi);
}

int lj_arena_hugetab_root_construct_commit(HugeTab *ht, const void *p,
						     LJHugeInfo *hi)
{
  return hugetab_root_complete(ht, p, LJ_ARENA_ROOT_LINKING,
				LJ_ARENA_ROOT_MEMBER, 0, hi);
}

int lj_arena_hugetab_root_construct_abandon(HugeTab *ht, const void *p,
						      uint64_t retire_epoch,
						      LJHugeInfo *hi)
{
  return hugetab_root_complete(ht, p, LJ_ARENA_ROOT_LINKING,
				LJ_ARENA_ROOT_NONE, retire_epoch, hi);
}

int lj_arena_hugetab_recovery_complete(HugeTab *ht, const void *p,
						LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  LJHugeEnt *e;
  uint64_t addr, meta;
  if (!h || !p)
    return LJ_ARENA_HUGE_RECOVERY_COMPLETE_LOST;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta) ||
	hugetab_recovery_state(meta) != LJ_ARENA_RECOVERY_CLAIMED)
      return LJ_ARENA_HUGE_RECOVERY_COMPLETE_LOST;
    if (!(meta & LJ_HUGEF_DEFER_FREE)) {
      next = meta & ~(uint64_t)LJ_HUGEF_RECOVERY_MASK;
      if (hugetab_cas_meta(e, addr, meta, next)) {
	hugetab_decode(next, hi);
	return LJ_ARENA_HUGE_RECOVERY_COMPLETE_LIVE;
      }
      continue;
    }
    if (hugetab_root_pending(meta)) {
      /* Root unlink owns the mapping. Preserve the logical free and return
      ** recovery ownership to the pending queue for a later retry. */
      next = (meta & ~(uint64_t)LJ_HUGEF_RECOVERY_MASK) |
	((uint64_t)LJ_ARENA_RECOVERY_PENDING <<
	 LJ_HUGEF_RECOVERY_SHIFT);
      if (hugetab_cas_meta(e, addr, meta, next)) {
	hugetab_decode(next, hi);
	return LJ_ARENA_HUGE_RECOVERY_COMPLETE_REQUEUED;
      }
      continue;
    }
    /* SWEEP_OLD|BUSY is a retirement owner publishing retire_obj/epoch.
    ** Requeue without changing the exact recovery count; a later claim can
    ** perform the terminal handoff after BUSY release. */
    if (meta & LJ_HUGEF_BUSY) {
      next = (meta & ~(uint64_t)LJ_HUGEF_RECOVERY_MASK) |
	((uint64_t)LJ_ARENA_RECOVERY_PENDING <<
	 LJ_HUGEF_RECOVERY_SHIFT);
      if (hugetab_cas_meta(e, addr, meta, next)) {
	hugetab_decode(next, hi);
	return LJ_ARENA_HUGE_RECOVERY_COMPLETE_REQUEUED;
      }
      continue;
    }
    {
      int progress;
      /* Clear this recovery owner in either reader-completion order. With an
      ** admitted reader, IDLE|DEFER_FREE is the intentional handoff state and
      ** the last reader performs the terminal CAS. */
      next = meta & ~(uint64_t)LJ_HUGEF_RECOVERY_MASK;
      next = hugetab_fold_deferred_free(p, next, &progress);
      if (hugetab_cas_meta(e, addr, meta, next)) {
	hugetab_decode(next, hi);
	if (progress != LJ_HUGETAB_FOLD_NONE)
	  arena_progress_wake(lj_arena_of(p));
	return progress == LJ_HUGETAB_FOLD_FREEING ?
	       LJ_ARENA_HUGE_RECOVERY_COMPLETE_SWEEP :
	       LJ_ARENA_HUGE_RECOVERY_COMPLETE_LIVE;
      }
    }
  }
}

int lj_arena_hugetab_recovery_discard_terminal(HugeTab *ht, const void *p,
							LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  LJHugeEnt *e;
  uint64_t addr, meta;
  if (!h || !p)
    return LJ_ARENA_HUGE_RECOVERY_TERMINAL_LOST;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    if (!hugetab_search(h, addr, &e, &meta) ||
	!hugetab_recovery_pending(meta))
      return LJ_ARENA_HUGE_RECOVERY_TERMINAL_LOST;
    if (hugetab_readers(meta) != 0 ||
	((meta & LJ_HUGEF_DEFER_FREE) && hugetab_root_pending(meta)))
      return LJ_ARENA_HUGE_RECOVERY_TERMINAL_LOST;
    if (meta & LJ_HUGEF_DEFER_FREE) {
      uint64_t leased, closed;
      int lease;
      la_u128 exp, des;
      lease = hugetab_cert_lease_acquire(e, addr, meta, &leased);
      if (lease < 0)
	continue;
      if (lease == 0)
	return LJ_ARENA_HUGE_RECOVERY_TERMINAL_LOST;
      if (!hugetab_cert_lease_close(e, addr, &closed)) {
	(void)hugetab_cert_lease_release(e, addr, p, NULL);
	return LJ_ARENA_HUGE_RECOVERY_TERMINAL_LOST;
      }
      for (;;) {
	uint64_t current = la_load64_acq(&e->slot.hi);
	if (la_load64_acq(&e->slot.lo) != addr ||
	    !hugetab_admission_closed(current) ||
	    !hugetab_recovery_pending(current) ||
	    !(current & LJ_HUGEF_DEFER_FREE) ||
	    hugetab_root_pending(current) ||
	    !hugetab_gc2_reclaim_clear_acq(p, current)) {
	  (void)hugetab_admission_reopen(e, addr, p, NULL);
	  return LJ_ARENA_HUGE_RECOVERY_TERMINAL_LOST;
	}
	exp.lo = addr;
	exp.hi = current;
	des.lo = LJ_HUGETAB_TOMBSTONE;
	des.hi = 0;
	if (!la_cas128(&e->slot, &exp, des))
	  continue;
	hugetab_table_topology_changed(ht);
	hugetab_decode(current & ~(uint64_t)LJ_HUGETAB_READER_MASK, hi);
	return LJ_ARENA_HUGE_RECOVERY_TERMINAL_UNMAP;
      }
    } else {
      uint64_t next = meta & ~(uint64_t)LJ_HUGEF_RECOVERY_MASK;
      if (hugetab_cas_meta(e, addr, meta, next)) {
	hugetab_decode(next, hi);
	return LJ_ARENA_HUGE_RECOVERY_TERMINAL_CLEARED;
      }
    }
  }
}

int lj_arena_hugetab_next(HugeTab *ht, uint32_t *cursor, void **pp,
				   LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap;
  if (pp)
    *pp = NULL;
  if (!h || !cursor)
    return 0;
  cap = h->mask + 1u;
  for (i = *cursor; i < cap;) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);
      la_u128 exp, des;
      if (la_load64_acq(&e->slot.lo) != addr)
	continue;
      exp.lo = addr;
      exp.hi = meta;
      des = exp;
      /* A no-op full-slot CAS proves that address, size and flags formed one
      ** stable entry snapshot. Semantic users still validate the returned
      ** flags; recovery iteration additionally relies on its non-IDLE state
      ** to exclude every ordinary deleter after this instant. */
      if (!la_cas128(&e->slot, &exp, des))
	continue;
      *cursor = ++i;
      if (pp)
	*pp = (void *)(uintptr_t)addr;
      hugetab_decode(meta, hi);
      return 1;
    }
    *cursor = ++i;
  }
  return 0;
}

uint32_t lj_arena_hugetab_slot_count(HugeTab *ht)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  return h ? h->mask + 1u : 0;
}

int lj_arena_hugetab_slot_snapshot_bounded(
  HugeTab *ht, uint32_t slot, void **pp, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  LJHugeEnt *e;
  uint64_t addr, meta;
  la_u128 exp, des;
  if (pp)
    *pp = NULL;
  if (hi) {
    hi->size = 0;
    hi->flags = 0;
    hi->readers = 0;
  }
  if (!h || slot > h->mask)
    return LJ_ARENA_HUGETAB_SLOT_EMPTY;
  e = &h->ent[slot];
  addr = la_load64_acq(&e->slot.lo);
  if (addr <= LJ_HUGETAB_TOMBSTONE)
    return LJ_ARENA_HUGETAB_SLOT_EMPTY;
  meta = la_load64_acq(&e->slot.hi);
  if (la_load64_acq(&e->slot.lo) != addr)
    return LJ_ARENA_HUGETAB_SLOT_BUSY;
  exp.lo = addr;
  exp.hi = meta;
  des = exp;
  if (!la_cas128(&e->slot, &exp, des))
    return LJ_ARENA_HUGETAB_SLOT_BUSY;
  if (pp)
    *pp = (void *)(uintptr_t)addr;
  hugetab_decode(meta, hi);
  return LJ_ARENA_HUGETAB_SLOT_PRESENT;
}

int lj_arena_hugetab_recovery_next(HugeTab *ht, uint32_t *cursor,
					    void **pp, LJHugeInfo *hi)
{
  void *p;
  LJHugeInfo snap = { 0, 0, 0 };
  if (pp)
    *pp = NULL;
  while (lj_arena_hugetab_next(ht, cursor, &p, &snap)) {
    if (lj_arena_huge_recovery_state(snap.flags) !=
	LJ_ARENA_RECOVERY_IDLE) {
      if (pp)
	*pp = p;
      if (hi)
	*hi = snap;
      return 1;
    }
  }
  return 0;
}

int lj_arena_hugetab_range_lookup(HugeTab *ht, const void *p, void **basep,
				  LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t target, candidate[3];
  uint32_t i, n = 1, cap;
  if (!h || !p)
    return 0;
  target = (uint64_t)(uintptr_t)p;
  candidate[0] = target;
  {
    uintptr_t span = (uintptr_t)p & ~(uintptr_t)LJ_ARENA_MASK;
    uint64_t base = (uint64_t)(span + sizeof(GCAhdr));
    if (base != target)
      candidate[n++] = base;
    if (span >= (uintptr_t)LJ_ARENA_SIZE) {
      base = (uint64_t)(span - (uintptr_t)LJ_ARENA_SIZE + sizeof(GCAhdr));
      if (base != target && (n == 1 || base != candidate[1]))
	candidate[n++] = base;
    }
  }
  for (i = 0; i < n; i++) {
    uint64_t meta;
    if (hugetab_search(h, candidate[i], NULL, &meta)) {
      size_t size = hugetab_size(meta);
      if (target >= candidate[i] && target - candidate[i] < (uint64_t)size) {
	if (basep)
	  *basep = (void *)(uintptr_t)candidate[i];
	hugetab_decode(meta, hi);
	return 1;
      }
    }
  }
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);  /* 04 §4.5.1 slot state. */
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);  /* 04 §4.5.1 metadata. */
      if (la_load64_acq(&e->slot.lo) == addr) {  /* Stable snapshot. */
	size_t size = hugetab_size(meta);
	if (target >= addr && target - addr < (uint64_t)size) {
	  if (basep)
	    *basep = (void *)(uintptr_t)addr;
	  hugetab_decode(meta, hi);
	  return 1;
	}
      }
    }
  }
  return 0;
}

/* GC object candidates are either exact allocation bases or interior-cdata
** headers within the first 64 KiB mapping span. Hash only those complete
** candidates; a stale word must never turn into a 2^16-slot table scan. */
int lj_arena_hugetab_cdata_range_lookup(HugeTab *ht, const void *p,
					 void **basep, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t target, candidate[3];
  uint32_t i, n = 1;
  if (basep)
    *basep = NULL;
  if (!h || !p)
    return 0;
  target = (uint64_t)(uintptr_t)p;
  candidate[0] = target;
  {
    uintptr_t span = (uintptr_t)p & ~(uintptr_t)LJ_ARENA_MASK;
    uint64_t base = (uint64_t)(span + sizeof(GCAhdr));
    if (base != target)
      candidate[n++] = base;
    if (span >= (uintptr_t)LJ_ARENA_SIZE) {
      base = (uint64_t)(span - (uintptr_t)LJ_ARENA_SIZE + sizeof(GCAhdr));
      if (base != target && (n == 1 || base != candidate[1]))
	candidate[n++] = base;
    }
  }
  for (i = 0; i < n; i++) {
    uint64_t meta;
    if (hugetab_search(h, candidate[i], NULL, &meta)) {
      size_t size = hugetab_size(meta);
      if ((target == candidate[i] &&
	   (meta & LJ_HUGEF_INTERIOR_CDATA)) ||
	  (target != candidate[i] &&
	   (meta & (LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA)) !=
	     (LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA)))
	continue;
      if (target >= candidate[i] && target - candidate[i] < (uint64_t)size) {
	if (basep)
	  *basep = (void *)(uintptr_t)candidate[i];
	hugetab_decode(meta, hi);
	return 1;
      }
    }
  }
  return 0;
}

int lj_arena_hugetab_mark(HugeTab *ht, const void *p, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, oldmeta;
  LJHugeEnt *e;
  if (!h || !p)
    return -1;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t newmeta;
    if (!hugetab_search(h, addr, &e, &oldmeta))
      return -1;
    if (oldmeta & (LJ_HUGEF_FREEING|LJ_HUGEF_DEFER_FREE))
      return -1;  /* Destructor ownership has crossed its grace LP. */
    /* A raw-memory root needs no payload read. Record its mark while the retire
    ** owner publishes the exact header ticket, then return without waiting for
    ** that owner. The retire CAS preserves MARK and suppresses RETIRED. Other
    ** BUSY states (notably realloc) have no such discharge contract. */
    if ((oldmeta & LJ_HUGEF_BUSY) && !(oldmeta & LJ_HUGEF_SWEEP_OLD))
      return -1;
    /* A destructive admission close is not a traversal owner. Atomically
    ** reopen it while publishing MARK, so its stale remover must lose and the
    ** metadata-only caller's liveness result remains durable. */
    newmeta = oldmeta;
    if (hugetab_admission_closed(oldmeta))
      newmeta &= ~(uint64_t)LJ_HUGETAB_READER_MASK;
    newmeta = (newmeta | LJ_HUGEF_MARK) & ~(uint64_t)LJ_HUGEF_RETIRED;
    if (hugetab_cas_meta(e, addr, oldmeta, newmeta)) {
      hugetab_decode(newmeta, hi);
      if (oldmeta & LJ_HUGEF_RETIRED)
	return 2;  /* Exact detached GC header must be reanchored after grace. */
      return (oldmeta & LJ_HUGEF_MARK) ? 0 : 1;
    }
  }
}

/* Atomically mark the entry which contains an interior address. This legacy
** metadata-only API returns a stable CAS snapshot, not a body lease; callers
** which dereference the base must use the counted mark-reader API below.
** MARK_INTENT records liveness behind a retire owner's BUSY claim and returns
** no base; the unique retire owner later discharges from retire_obj. */
static int hugetab_mark_range_entry(LJHugeEnt *e, uint64_t target,
				    void **basep, LJHugeInfo *hi)
{
  for (;;) {
    la_u128 exp, des;
    uint64_t addr = la_load64_acq(&e->slot.lo);
    uint64_t meta, next, size;
    int result;
    if (addr <= LJ_HUGETAB_TOMBSTONE)
      return -2;
    meta = la_load64_acq(&e->slot.hi);
    if (la_load64_acq(&e->slot.lo) != addr)
      continue;
    size = (uint64_t)hugetab_size(meta);
    if (target < addr || target - addr >= size)
      return -2;
    exp.lo = addr;
    exp.hi = meta;
    if (!(meta & LJ_HUGEF_TRAVERSABLE) || !(meta & LJ_HUGEF_READY) ||
	(meta & (LJ_HUGEF_FREEING|LJ_HUGEF_DEFER_FREE)) ||
	(target != addr &&
	 (meta & (LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA)) !=
	   (LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA))) {
      des = exp;  /* Prove exact rejection without changing the slot. */
      if (!la_cas128(&e->slot, &exp, des))
	continue;
      return -1;
    }
    if (hugetab_admission_closed(meta)) {
      /* Defeat the close before returning a metadata-only liveness result. */
      next = ((meta & ~(uint64_t)LJ_HUGETAB_READER_MASK) |
	      LJ_HUGEF_MARK) & ~(uint64_t)LJ_HUGEF_RETIRED;
      des.lo = addr;
      des.hi = next;
      if (!la_cas128(&e->slot, &exp, des))
	continue;
      if (basep)
	*basep = (void *)(uintptr_t)addr;
      hugetab_decode(next, hi);
      return (meta & LJ_HUGEF_RETIRED) ? 2 :
	((meta & LJ_HUGEF_MARK) ? 0 : 1);
    }
    if (meta & LJ_HUGEF_BUSY) {
      /* TICKET|MARK|BUSY is the body-stable live-reanchor claim and may use
      ** the ordinary admitted return below. Before TICKET, only metadata is
      ** stable: publish an opaque mark intent without reading the header or
      ** waiting for a paused retire owner. A TICKET without MARK is not a
      ** reachable protocol state.
      **
      ** SWEEP_OLD|BUSY without TICKET can also be a realloc claim acquired
      ** after prepare_sweep(). That owner has no traversal-discharge step, but
      ** none is needed: production GC allocations in traversable arenas are
      ** immutable/non-reallocable (lj_mem_realloc rejects them). Thus such a
      ** claim can name only raw storage hit by a conservative false candidate;
      ** the metadata MARK preserves its mapping and there is no object graph
      ** to walk. If this is the retire claim instead, retire() publishes the
      ** exact header ticket and its return-2 owner performs the one required
      ** traversal. These cases therefore safely share the opaque intent state
      ** without another metadata bit. */
      if (!(meta & LJ_HUGEF_SWEEP_OLD) ||
	  ((meta & LJ_HUGEF_TICKET) && !(meta & LJ_HUGEF_MARK))) {
	des = exp;
	if (!la_cas128(&e->slot, &exp, des))
	  continue;
	return -1;
      }
      if (!(meta & LJ_HUGEF_TICKET)) {
	next = (meta | LJ_HUGEF_MARK) & ~(uint64_t)LJ_HUGEF_RETIRED;
	des.lo = addr;
	des.hi = next;
	if (!la_cas128(&e->slot, &exp, des))
	  continue;
	hugetab_decode(next, hi);
	return LJ_ARENA_HUGE_MARK_INTENT;
      }
    }
    next = (meta | LJ_HUGEF_MARK) & ~(uint64_t)LJ_HUGEF_RETIRED;
    des.lo = addr;
    des.hi = next;
    if (!la_cas128(&e->slot, &exp, des))
      continue;
    if (basep)
      *basep = (void *)(uintptr_t)addr;
    hugetab_decode(next, hi);
    result = (meta & LJ_HUGEF_RETIRED) ? 2 :
	((meta & LJ_HUGEF_MARK) ? 0 : 1);
    return result;
  }
}

int lj_arena_hugetab_mark_range(HugeTab *ht, const void *p, void **basep,
				 LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t target, candidate[3];
  uint32_t i, n = 1, cap;
  if (basep)
    *basep = NULL;
  if (!h || !p)
    return -1;
  target = (uint64_t)(uintptr_t)p;
  candidate[0] = target;  /* Exact huge object/allocation base. */
  {
    uintptr_t span = (uintptr_t)p & ~(uintptr_t)LJ_ARENA_MASK;
    uint64_t base = (uint64_t)(span + sizeof(GCAhdr));
    if (base != target)
      candidate[n++] = base;
    if (span >= (uintptr_t)LJ_ARENA_SIZE) {
      base = (uint64_t)(span - (uintptr_t)LJ_ARENA_SIZE + sizeof(GCAhdr));
      if (base != target && (n == 1 || base != candidate[1]))
	candidate[n++] = base;
    }
  }
  /* Huge cdata headers are near their allocation base. Hash these exact
  ** candidates before the bounded full-table fallback used by generic range
  ** probes and adversarial tests. */
  for (i = 0; i < n; i++) {
    LJHugeEnt *e;
    uint64_t meta;
    int result;
    if (!hugetab_search(h, candidate[i], &e, &meta))
      continue;
    if ((target == candidate[i]) ==
	((meta & LJ_HUGEF_INTERIOR_CDATA) != 0))
      return -1;
    result = hugetab_mark_range_entry(e, target, basep, hi);
    if (result != -2)
      return result;
  }
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    int result = hugetab_mark_range_entry(&h->ent[i], target, basep, hi);
    if (result != -2)
      return result;
  }
  return -1;
}

int lj_arena_hugetab_mark_cdata_range(HugeTab *ht, const void *p,
				       void **basep, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t target, candidate[3];
  uint32_t i, n = 1;
  if (basep)
    *basep = NULL;
  if (!h || !p)
    return -1;
  target = (uint64_t)(uintptr_t)p;
  candidate[0] = target;
  {
    uintptr_t span = (uintptr_t)p & ~(uintptr_t)LJ_ARENA_MASK;
    uint64_t base = (uint64_t)(span + sizeof(GCAhdr));
    if (base != target)
      candidate[n++] = base;
    if (span >= (uintptr_t)LJ_ARENA_SIZE) {
      base = (uint64_t)(span - (uintptr_t)LJ_ARENA_SIZE + sizeof(GCAhdr));
      if (base != target && (n == 1 || base != candidate[1]))
	candidate[n++] = base;
    }
  }
  for (i = 0; i < n; i++) {
    LJHugeEnt *e;
    uint64_t meta;
    int result;
    if (!hugetab_search(h, candidate[i], &e, &meta))
      continue;
    if ((target == candidate[i]) ==
	((meta & LJ_HUGEF_INTERIOR_CDATA) != 0))
      return -1;
    result = hugetab_mark_range_entry(e, target, basep, hi);
    if (result != -2)
      return result;
  }
  return -1;
}

#define LJ_HUGE_READER_MARK		0x01u
#define LJ_HUGE_READER_TRAVERSABLE	0x02u
#define LJ_HUGE_READER_CDATA_SHAPE	0x04u
#define LJ_HUGE_READER_NOT_CONTAINING	(-3)

/* Validate and admit one exact slot. The successful CAS is simultaneously the
** range snapshot and the mapping-lifetime LP; no header byte participates. */
static int hugetab_reader_entry(LJHugeTabHdr *h, LJHugeEnt *e, uint64_t target,
				uint32_t mode, void **basep,
				LJHugeReader *reader, LJHugeInfo *hi)
{
  for (;;) {
    la_u128 exp, des;
    uint64_t addr = la_load64_acq(&e->slot.lo);
    uint64_t meta, next, size;
    int result;
    if (addr <= LJ_HUGETAB_TOMBSTONE)
      return LJ_HUGE_READER_NOT_CONTAINING;
    meta = la_load64_acq(&e->slot.hi);
    if (la_load64_acq(&e->slot.lo) != addr)
      continue;
    size = (uint64_t)hugetab_size(meta);
    if (target < addr || target - addr >= size)
      return LJ_HUGE_READER_NOT_CONTAINING;
    exp.lo = addr;
    exp.hi = meta;
    if (((mode & LJ_HUGE_READER_TRAVERSABLE) &&
	 ((meta & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) !=
	  (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY))) ||
	((mode & LJ_HUGE_READER_CDATA_SHAPE) &&
	 ((target == addr && (meta & LJ_HUGEF_INTERIOR_CDATA)) ||
	  (target != addr &&
	   (meta & (LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA)) !=
	    (LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA)))) ||
	(meta & (LJ_HUGEF_FREEING|LJ_HUGEF_DEFER_FREE))) {
      des = exp;
      if (!la_cas128(&e->slot, &exp, des))
	continue;
      return -1;
    }
    if (hugetab_admission_closed(meta)) {
      if (mode & LJ_HUGE_READER_MARK) {
	/* A close owner cannot discharge traversal. Reopen it, preserve MARK,
	** and report the same allocation-free retry used for reader saturation. */
	next = ((meta & ~(uint64_t)LJ_HUGETAB_READER_MASK) |
		LJ_HUGEF_MARK) & ~(uint64_t)LJ_HUGEF_RETIRED;
	des.lo = addr;
	des.hi = next;
	if (!la_cas128(&e->slot, &exp, des))
	  continue;
	hugetab_decode(next, hi);
	return LJ_ARENA_HUGE_MARK_SATURATED;
      }
      des = exp;
      if (!la_cas128(&e->slot, &exp, des))
	continue;
      return -1;
    }
    if (meta & LJ_HUGEF_BUSY) {
      int stable_ticket =
	(meta & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_TICKET|LJ_HUGEF_MARK)) ==
	(LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_TICKET|LJ_HUGEF_MARK);
      if ((mode & LJ_HUGE_READER_MARK) && (meta & LJ_HUGEF_SWEEP_OLD) &&
	  !(meta & LJ_HUGEF_TICKET)) {
	/* The retire owner has not published a readable exact header. Preserve
	** liveness, but deliberately do not increment the reader count. */
	next = (meta | LJ_HUGEF_MARK) & ~(uint64_t)LJ_HUGEF_RETIRED;
	des.lo = addr;
	des.hi = next;
	if (!la_cas128(&e->slot, &exp, des))
	  continue;
	hugetab_decode(next, hi);
	return LJ_ARENA_HUGE_MARK_INTENT;
      }
      if (!stable_ticket) {
	des = exp;
	if (!la_cas128(&e->slot, &exp, des))
	  continue;
	return -1;
      }
    }
    if (hugetab_readers(meta) == LJ_HUGETAB_READER_MAX) {
      if (mode & LJ_HUGE_READER_MARK) {
	/* Saturation may deny a body token, never semantic liveness. Publish the
	** exact mark transition in the full slot and require an explicit later
	** traversal/retry from the caller. */
	next = (meta | LJ_HUGEF_MARK) & ~(uint64_t)LJ_HUGEF_RETIRED;
	des.lo = addr;
	des.hi = next;
	if (!la_cas128(&e->slot, &exp, des))
	  continue;
	hugetab_decode(next, hi);
	return LJ_ARENA_HUGE_MARK_SATURATED;
      }
      return LJ_ARENA_HUGE_READER_OVERFLOW;
    }
    next = meta + LJ_HUGETAB_READER_ONE;
    if (mode & LJ_HUGE_READER_MARK)
      next = (next | LJ_HUGEF_MARK) & ~(uint64_t)LJ_HUGEF_RETIRED;
    des.lo = addr;
    des.hi = next;
    if (!la_cas128(&e->slot, &exp, des))
      continue;
    reader->h = h;
    reader->base = (void *)(uintptr_t)addr;
    reader->size = (uint32_t)hugetab_size(next);
    if (basep)
      *basep = reader->base;
    hugetab_decode(next, hi);
    if (!(mode & LJ_HUGE_READER_MARK))
      return LJ_ARENA_HUGE_READER_ACQUIRED;
    result = (meta & LJ_HUGEF_RETIRED) ? 2 :
	((meta & LJ_HUGEF_MARK) ? 0 : 1);
    return result;
  }
}

static int hugetab_reader_range_acquire(HugeTab *ht, const void *p,
					void **basep, LJHugeReader *reader,
					LJHugeInfo *hi, uint32_t mode,
					int bounded_cdata)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t target, candidate[3];
  uint32_t i, n = 1, cap;
  int missing = (mode & LJ_HUGE_READER_MARK) ? -1 :
		LJ_ARENA_HUGE_READER_MISSING;
  if (basep)
    *basep = NULL;
  if (!h || !p || !reader || reader->h || reader->base || reader->size)
    return missing;
  target = (uint64_t)(uintptr_t)p;
  candidate[0] = target;
  {
    uintptr_t span = (uintptr_t)p & ~(uintptr_t)LJ_ARENA_MASK;
    uint64_t base = (uint64_t)(span + sizeof(GCAhdr));
    if (base != target)
      candidate[n++] = base;
    if (span >= (uintptr_t)LJ_ARENA_SIZE) {
      base = (uint64_t)(span - (uintptr_t)LJ_ARENA_SIZE + sizeof(GCAhdr));
      if (base != target && (n == 1 || base != candidate[1]))
	candidate[n++] = base;
    }
  }
  for (i = 0; i < n; i++) {
    LJHugeEnt *e;
    uint64_t ignored;
    int result;
    if (!hugetab_search(h, candidate[i], &e, &ignored))
      continue;
    result = hugetab_reader_entry(h, e, target, mode, basep, reader, hi);
    if (result != LJ_HUGE_READER_NOT_CONTAINING)
      return result;
  }
  if (bounded_cdata)
    return missing;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    int result = hugetab_reader_entry(h, &h->ent[i], target, mode,
				      basep, reader, hi);
    if (result != LJ_HUGE_READER_NOT_CONTAINING)
      return result;
  }
  return missing;
}

int lj_arena_hugetab_reader_acquire(HugeTab *ht, const void *p,
				      LJHugeReader *reader, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  LJHugeEnt *e;
  uint64_t addr, ignored;
  int result;
  if (!h || !p || !reader || reader->h || reader->base || reader->size)
    return LJ_ARENA_HUGE_READER_MISSING;
  addr = (uint64_t)(uintptr_t)p;
  if (!hugetab_search(h, addr, &e, &ignored))
    return LJ_ARENA_HUGE_READER_MISSING;
  result = hugetab_reader_entry(h, e, addr, 0, NULL, reader, hi);
  return result < 0 && result != LJ_ARENA_HUGE_READER_OVERFLOW ?
	 LJ_ARENA_HUGE_READER_MISSING : result;
}

int lj_arena_hugetab_table_token_slot_lease_acquire_bounded(
  HugeTab *ht, uint32_t slot, void **pp, LJHugeTokenLease *lease,
  LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  LJHugeEnt *e;
  uint64_t addr, meta, next;
  la_u128 exp, des;
  if (pp)
    *pp = NULL;
  if (hi) {
    hi->size = 0;
    hi->flags = 0;
    hi->readers = 0;
  }
  if (!h || slot > h->mask || !lease || lease->h || lease->base ||
      lease->size || lease->body_authorized)
    return LJ_ARENA_HUGE_TOKEN_LEASE_MISSING;
  e = &h->ent[slot];
  addr = la_load64_acq(&e->slot.lo);
  if (addr <= LJ_HUGETAB_TOMBSTONE)
    return LJ_ARENA_HUGE_TOKEN_LEASE_MISSING;
  meta = la_load64_acq(&e->slot.hi);
  if (la_load64_acq(&e->slot.lo) != addr)
    return LJ_ARENA_HUGE_TOKEN_LEASE_BUSY;
  exp.lo = addr;
  exp.hi = meta;
  if (hugetab_admission_closed(meta) ||
      (meta & (LJ_HUGEF_FREEING|LJ_HUGEF_BUSY))) {
    des = exp;
    if (!la_cas128(&e->slot, &exp, des))
      return LJ_ARENA_HUGE_TOKEN_LEASE_BUSY;
    hugetab_decode(meta, hi);
    return (!hugetab_admission_closed(meta) &&
	    (meta & LJ_HUGEF_FREEING)) ?
      LJ_ARENA_HUGE_TOKEN_LEASE_FREEING :
      LJ_ARENA_HUGE_TOKEN_LEASE_BUSY;
  }
  if ((meta & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) !=
        (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY) ||
      (meta & (LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA))) {
    des = exp;
    if (!la_cas128(&e->slot, &exp, des))
      return LJ_ARENA_HUGE_TOKEN_LEASE_BUSY;
    return LJ_ARENA_HUGE_TOKEN_LEASE_MISSING;
  }
  if (hugetab_readers(meta) == LJ_HUGETAB_READER_MAX)
    return LJ_ARENA_HUGE_TOKEN_LEASE_OVERFLOW;
  next = meta + LJ_HUGETAB_READER_ONE;
  des.lo = addr;
  des.hi = next;
  if (!la_cas128(&e->slot, &exp, des))
    return LJ_ARENA_HUGE_TOKEN_LEASE_BUSY;
  lease->h = h;
  lease->base = (void *)(uintptr_t)addr;
  lease->size = (uint32_t)hugetab_size(next);
  lease->body_authorized = (next & LJ_HUGEF_DEFER_FREE) == 0;
  if (pp)
    *pp = lease->base;
  hugetab_decode(next, hi);
  return lease->body_authorized ? LJ_ARENA_HUGE_TOKEN_LEASE_LIVE :
    LJ_ARENA_HUGE_TOKEN_LEASE_DEFERRED;
}

int lj_arena_hugetab_table_token_lease_take_reader(
  LJHugeTokenLease *lease, LJHugeReader *reader)
{
  if (!lease || !reader || !lease->h || !lease->base || !lease->size ||
      !lease->body_authorized || reader->h || reader->base || reader->size)
    return 0;
  reader->h = lease->h;
  reader->base = lease->base;
  reader->size = lease->size;
  lease->h = NULL;
  lease->base = NULL;
  lease->size = 0;
  lease->body_authorized = 0;
  return 1;
}

LJGC2TabStamp *lj_arena_hugetab_table_token_lease_stamp_acq(
  const LJHugeTokenLease *lease)
{
  GCArena *a;
  if (!lease || !lease->h || !lease->base || !lease->size)
    return NULL;
  a = lj_arena_of(lease->base);
  if (lease->base != (void *)((char *)a + sizeof(GCAhdr)) ||
      (lj_arena_flags_acq(a) & LJ_AF_HUGE_MAGIC) != LJ_AF_HUGE_MAGIC)
    return NULL;
  return &a->hdr.huge_tabstamp;
}

int lj_arena_hugetab_sweep_reader_acquire(HugeTab *ht, const void *p,
					    LJHugeReader *reader,
					    LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  LJHugeEnt *e;
  uint64_t addr, meta, next;
  if (!h || !p || !reader || reader->h || reader->base || reader->size)
    return LJ_ARENA_HUGE_READER_MISSING;
  addr = (uint64_t)(uintptr_t)p;
  if (!hugetab_search(h, addr, &e, &meta))
    return LJ_ARENA_HUGE_READER_MISSING;
  /* Enumeration is only a hint. This exact CAS is the body/header admission
  ** LP and therefore closes a DEFER/BUSY publication race without reading a
  ** single mapping byte first. FREEING needs only the allocator header for its
  ** grace epoch; an ordinary live body additionally requires READY identity. */
  if (!(meta & LJ_HUGEF_SWEEP_OLD) || hugetab_admission_closed(meta) ||
      (meta & (LJ_HUGEF_DEFER_FREE|LJ_HUGEF_BUSY)) ||
      (!(meta & LJ_HUGEF_FREEING) &&
       (meta & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) !=
	 (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY))) {
    hugetab_decode(meta, hi);
    return LJ_ARENA_HUGE_READER_MISSING;
  }
  if (hugetab_readers(meta) == LJ_HUGETAB_READER_MAX) {
    hugetab_decode(meta, hi);
    return LJ_ARENA_HUGE_READER_OVERFLOW;
  }
  next = meta + LJ_HUGETAB_READER_ONE;
  if (!hugetab_cas_meta(e, addr, meta, next))
    return LJ_ARENA_HUGE_READER_MISSING;
  reader->h = h;
  reader->base = (void *)(uintptr_t)addr;
  reader->size = (uint32_t)hugetab_size(next);
  hugetab_decode(next, hi);
  return LJ_ARENA_HUGE_READER_ACQUIRED;
}

int lj_arena_hugetab_reader_range_acquire(HugeTab *ht, const void *p,
					    void **basep,
					    LJHugeReader *reader,
					    LJHugeInfo *hi)
{
  return hugetab_reader_range_acquire(ht, p, basep, reader, hi, 0, 0);
}

int lj_arena_hugetab_reader_cdata_range_acquire(HugeTab *ht, const void *p,
						  void **basep,
						  LJHugeReader *reader,
						  LJHugeInfo *hi)
{
  return hugetab_reader_range_acquire(ht, p, basep, reader, hi,
	LJ_HUGE_READER_TRAVERSABLE|LJ_HUGE_READER_CDATA_SHAPE, 1);
}

int lj_arena_hugetab_mark_reader_acquire(HugeTab *ht, const void *p,
					   LJHugeReader *reader,
					   LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  LJHugeEnt *e;
  uint64_t addr, ignored;
  int result;
  if (!h || !p || !reader || reader->h || reader->base || reader->size)
    return -1;
  addr = (uint64_t)(uintptr_t)p;
  if (!hugetab_search(h, addr, &e, &ignored))
    return -1;
  result = hugetab_reader_entry(h, e, addr, LJ_HUGE_READER_MARK,
				NULL, reader, hi);
  return result == LJ_HUGE_READER_NOT_CONTAINING ? -1 : result;
}

int lj_arena_hugetab_mark_range_reader_acquire(HugeTab *ht, const void *p,
						 void **basep,
						 LJHugeReader *reader,
						 LJHugeInfo *hi)
{
  return hugetab_reader_range_acquire(ht, p, basep, reader, hi,
	LJ_HUGE_READER_MARK|LJ_HUGE_READER_TRAVERSABLE, 0);
}

int lj_arena_hugetab_mark_cdata_range_reader_acquire(
  HugeTab *ht, const void *p, void **basep, LJHugeReader *reader,
  LJHugeInfo *hi)
{
  return hugetab_reader_range_acquire(ht, p, basep, reader, hi,
	LJ_HUGE_READER_MARK|LJ_HUGE_READER_TRAVERSABLE|
	LJ_HUGE_READER_CDATA_SHAPE, 1);
}

static int hugetab_counted_lease_release(LJHugeTabHdr *h, void *base,
					 LJHugeInfo *hi)
{
  LJHugeEnt *e;
  uint64_t addr, meta;
  if (!h || !base)
    return LJ_ARENA_HUGE_READER_RELEASE_LOST;
  addr = (uint64_t)(uintptr_t)base;
  for (;;) {
    uint64_t next;
    int progress;
    if (!hugetab_search(h, addr, &e, &meta) ||
	hugetab_readers(meta) == 0 || hugetab_admission_closed(meta))
      return LJ_ARENA_HUGE_READER_RELEASE_LOST;
    next = meta - LJ_HUGETAB_READER_ONE;
    next = hugetab_fold_deferred_free(base, next, &progress);
    if (!hugetab_cas_meta(e, addr, meta, next))
      continue;
    hugetab_decode(next, hi);
    if (progress != LJ_HUGETAB_FOLD_NONE)
      arena_progress_wake(lj_arena_of(base));
    if (progress == LJ_HUGETAB_FOLD_FREEING)
      return LJ_ARENA_HUGE_READER_HANDOFF;
    return LJ_ARENA_HUGE_READER_RELEASED;
  }
}

int lj_arena_hugetab_reader_release(LJHugeReader *reader, LJHugeInfo *hi)
{
  void *base;
  LJHugeTabHdr *h;
  if (!reader)
    return LJ_ARENA_HUGE_READER_RELEASE_LOST;
  h = reader->h;
  base = reader->base;
  reader->h = NULL;
  reader->base = NULL;
  reader->size = 0;
  return hugetab_counted_lease_release(h, base, hi);
}

int lj_arena_hugetab_table_token_lease_release(
  LJHugeTokenLease *lease, LJHugeInfo *hi)
{
  void *base;
  LJHugeTabHdr *h;
  if (!lease)
    return LJ_ARENA_HUGE_READER_RELEASE_LOST;
  h = lease->h;
  base = lease->base;
  lease->h = NULL;
  lease->base = NULL;
  lease->size = 0;
  lease->body_authorized = 0;
  return hugetab_counted_lease_release(h, base, hi);
}

int lj_arena_hugetab_reader_covers_range(const LJHugeReader *reader,
					   const void *p, size_t size)
{
  uintptr_t base, target;
  size_t offset;
  if (!reader || !reader->h || !reader->base || !p)
    return 0;
  base = (uintptr_t)reader->base;
  target = (uintptr_t)p;
  if (target < base)
    return 0;
  offset = (size_t)(target - base);
  return offset <= (size_t)reader->size &&
    size <= (size_t)reader->size - offset;
}

int lj_arena_hugetab_reader_covers(const LJHugeReader *reader, const void *p)
{
  return lj_arena_hugetab_reader_covers_range(reader, p, 1u);
}

int lj_arena_hugetab_publish_gco(HugeTab *ht, const void *p)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  LJHugeEnt *e;
  uint64_t addr, meta;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta) ||
	(meta & LJ_HUGEF_FREEING) || !(meta & LJ_HUGEF_TRAVERSABLE))
      return 0;
    if (meta & LJ_HUGEF_READY)
      return 1;
    next = meta | LJ_HUGEF_READY;
    if (hugetab_cas_meta(e, addr, meta, next))
      return 1;
  }
}

int lj_arena_hugetab_publish_cdata(HugeTab *ht, const void *p, int interior)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  LJHugeEnt *e;
  uint64_t addr, meta;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next, required;
    if (!hugetab_search(h, addr, &e, &meta) ||
	(meta & LJ_HUGEF_FREEING) || !(meta & LJ_HUGEF_TRAVERSABLE))
      return 0;
    required = LJ_HUGEF_CDATA | LJ_HUGEF_READY |
	(interior ? LJ_HUGEF_INTERIOR_CDATA : 0u);
    if ((meta & required) == required &&
	(interior || !(meta & LJ_HUGEF_INTERIOR_CDATA)))
      return 1;
    if ((meta & LJ_HUGEF_INTERIOR_CDATA) && !interior)
      return 0;  /* Allocation identity cannot change after publication. */
    next = meta | LJ_HUGEF_CDATA | LJ_HUGEF_READY |
	(interior ? LJ_HUGEF_INTERIOR_CDATA : 0u);
    if (hugetab_cas_meta(e, addr, meta, next))
      return 1;  /* Release-publishes the initialized base/header layout. */
  }
}

int lj_arena_hugetab_publish_interior_cdata(HugeTab *ht, const void *p)
{
  return lj_arena_hugetab_publish_cdata(ht, p, 1);
}

void lj_arena_hugetab_clear_marks(HugeTab *ht)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap;
  if (!h)
    return;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);
    while (addr > LJ_HUGETAB_TOMBSTONE &&
	   la_load64_acq(&e->slot.lo) == addr) {
      uint64_t meta = la_load64_acq(&e->slot.hi);
      if (!(meta & LJ_HUGEF_MARK) || hugetab_recovery_pending(meta) ||
	  hugetab_cas_meta(e, addr, meta,
			   meta & ~(uint64_t)LJ_HUGEF_MARK))
	break;
    }
  }
}

void lj_arena_hugetab_prepare_sweep(HugeTab *ht)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap;
  if (!h)
    return;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);
      while ((meta & LJ_HUGEF_TRAVERSABLE) != 0 &&
	     !(meta & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING|
		       LJ_HUGEF_TICKET|LJ_HUGEF_BUSY))) {
	uint64_t next = meta | LJ_HUGEF_SWEEP_OLD;
	if (hugetab_cas_meta(e, addr, meta, next))
	  break;
	if (la_load64_acq(&e->slot.lo) != addr)
	  break;
	meta = la_load64_acq(&e->slot.hi);
      }
    }
  }
}

void lj_arena_hugetab_abort_sweep(HugeTab *ht)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap;
  if (!h)
    return;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);
      while (la_load64_acq(&e->slot.lo) == addr &&
	     (meta & LJ_HUGEF_SWEEP_OLD) &&
	     !(meta & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING|
		       LJ_HUGEF_TICKET|LJ_HUGEF_BUSY|
		       LJ_HUGEF_DEFER_FREE))) {
	uint64_t next = (meta | LJ_HUGEF_MARK) &
			~(uint64_t)LJ_HUGEF_SWEEP_OLD;
	if (hugetab_cas_meta(e, addr, meta, next))
	  break;
	if (la_load64_acq(&e->slot.lo) != addr)
	  break;
	meta = la_load64_acq(&e->slot.hi);
      }
    }
  }
}

void lj_arena_hugetab_finish_sweep(HugeTab *ht, int preserve_marks)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap;
  if (!h)
    return;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    for (;;) {
      uint64_t addr = la_load64_acq(&e->slot.lo);
      uint64_t meta;
      int lease;
      const void *p;
      if (addr <= LJ_HUGETAB_TOMBSTONE)
	break;
      meta = la_load64_acq(&e->slot.hi);
      if (la_load64_acq(&e->slot.lo) != addr)
	continue;
      if (!(meta & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_MARK)) ||
	  (meta & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_MARK)) !=
	    (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_MARK) ||
	  hugetab_recovery_pending(meta) || hugetab_readers(meta) != 0 ||
	  (meta & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING|LJ_HUGEF_TICKET|
		   LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE)))
	break;
      lease = hugetab_cert_lease_acquire(e, addr, meta, NULL);
      if (lease < 0)
	continue;
      if (lease == 0)
	break;
      p = (const void *)(uintptr_t)addr;
      for (;;) {
	uint64_t current = la_load64_acq(&e->slot.hi);
	uint64_t next;
	GCArena *a;
	if (la_load64_acq(&e->slot.lo) != addr ||
	    hugetab_readers(current) == 0)
	  break;
	if ((current & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_MARK)) !=
	      (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_MARK) ||
	    hugetab_recovery_pending(current) ||
	    (current & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING|
		       LJ_HUGEF_TICKET|LJ_HUGEF_BUSY|
		       LJ_HUGEF_DEFER_FREE))) {
	  (void)hugetab_cert_lease_release(e, addr, p, NULL);
	  break;
	}
	a = lj_arena_of(p);
	if (la_loadptr_acq((void *const *)&a->hdr.retire_obj) != NULL) {
	  (void)hugetab_cert_lease_release(e, addr, p, NULL);
	  break;
	}
	next = current & ~(uint64_t)LJ_HUGEF_SWEEP_OLD;
	if (!preserve_marks)
	  next &= ~(uint64_t)LJ_HUGEF_MARK;
	if (!hugetab_cas_meta(e, addr, current, next))
	  continue;
	/* The reader remains in next until both final header stores publish. A
	** racing free can only add DEFER and is folded by the release below. */
	la_store64_rel(&a->hdr.retire_epoch, 0);
	la_storeptr_rel(&a->hdr.retire_obj, NULL);
	(void)hugetab_cert_lease_release(e, addr, p, NULL);
	break;
      }
      break;
    }
  }
}

int lj_arena_hugetab_sweep_next(HugeTab *ht, uint32_t *cursor,
				 void **pp, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint32_t i, cap;
  if (pp)
    *pp = NULL;
  if (!h || !cursor)
    return 0;
  cap = h->mask + 1u;
  for (i = *cursor; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);
    *cursor = i + 1u;
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);
      if (la_load64_acq(&e->slot.lo) == addr &&
	  (meta & LJ_HUGEF_SWEEP_OLD) &&
	  (meta & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_FREEING))) {
	if (pp)
	  *pp = (void *)(uintptr_t)addr;
	hugetab_decode(meta, hi);
	return 1;
      }
    }
  }
  return 0;
}

int lj_arena_hugetab_has_sweep_old(HugeTab *ht)
{
  uint32_t cursor = 0;
  void *p;
  return lj_arena_hugetab_sweep_next(ht, &cursor, &p, NULL);
}

int lj_arena_hugetab_retire(HugeTab *ht, const void *p, const void *obj,
			    uint64_t retire_epoch, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p || !obj)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  /* BUSY pins the mapping before either header field is touched. This matters
  ** even for a losing retirement attempt: an external free may otherwise win
  ** deletion between search and these stores, or its fresh-grace sentinel may
  ** be overwritten by a retire attempt which cannot publish TICKET. A return
  ** of 2 identifies the one owner whose final TICKET contains MARK; that owner
  ** must discharge a semantic traversal. MARK provenance is intentionally not
  ** encoded, so a pre-BUSY mark may cause a harmless duplicate traversal. */
  for (;;) {
    uint64_t busy, next;
    if (!hugetab_search(h, addr, &e, &meta))
      return 0;
    if (!(meta & LJ_HUGEF_SWEEP_OLD))
      return 0;
    if (meta & LJ_HUGEF_TICKET) {
      hugetab_decode(meta, hi);
      return 1;
    }
    if (hugetab_readers(meta) != 0 || (meta & LJ_HUGEF_BUSY))
      return 0;
    busy = meta | LJ_HUGEF_BUSY;
    if (!hugetab_cas_meta(e, addr, meta, busy))
      continue;
    hugetab_test_retire_pause_after_busy();
    {
      GCArena *a = lj_arena_of(p);
      la_storeptr_rel(&a->hdr.retire_obj, (void *)obj);
      /* FREEING already carries the external publisher's fresh-grace
      ** sentinel. Root detachment may still add its exact TICKET afterward,
      ** but must not weaken that later physical-free epoch. */
      if (!(busy & LJ_HUGEF_FREEING))
	la_store64_rel(&a->hdr.retire_epoch, retire_epoch);
    }
    /* MARK may be added while BUSY is held. Preserve it and publish TICKET
    ** only after the exact header fields are release-visible. */
    for (;;) {
      int progress;
      next = (busy | LJ_HUGEF_TICKET) & ~(uint64_t)LJ_HUGEF_BUSY;
	if (hugetab_recovery_pending(busy))
	  next = (next | LJ_HUGEF_MARK) & ~(uint64_t)LJ_HUGEF_RETIRED;
	else if (!(busy & (LJ_HUGEF_MARK|LJ_HUGEF_FREEING)))
	  next |= LJ_HUGEF_RETIRED;
      else
	next &= ~(uint64_t)LJ_HUGEF_RETIRED;
      next = hugetab_fold_deferred_free(p, next, &progress);
      if (hugetab_cas_meta(e, addr, busy, next)) {
	hugetab_decode(next, hi);
	if (progress != LJ_HUGETAB_FOLD_NONE)
	  arena_progress_wake(lj_arena_of(p));
	return (next & LJ_HUGEF_MARK) ? 2 : 1;
      }
      if (la_load64_acq(&e->slot.lo) != addr)
	return 0;
      busy = la_load64_acq(&e->slot.hi);
      if (!(busy & LJ_HUGEF_BUSY))
	return 0;
    }
  }
}

static int hugetab_destruct_abandon(LJHugeEnt *e, uint64_t addr,
				    uint64_t prior);

int lj_arena_hugetab_claim_freeing(HugeTab *ht, const void *p,
					    LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t leased;
    int lease;
    if (!hugetab_search(h, addr, &e, &meta))
      return 0;
    if (!(meta & LJ_HUGEF_RETIRED) || hugetab_readers(meta) != 0 ||
	hugetab_recovery_pending(meta) ||
	hugetab_root_pending(meta) ||
	(meta & (LJ_HUGEF_MARK|LJ_HUGEF_FREEING|LJ_HUGEF_BUSY|
		 LJ_HUGEF_DEFER_FREE)))
      return 0;
    lease = hugetab_cert_lease_acquire(e, addr, meta, &leased);
    if (lease < 0)
      continue;
    if (lease == 0)
      return 0;
    if (!hugetab_gc2_reclaim_clear_acq(p, leased)) {
      (void)hugetab_cert_lease_release(e, addr, p, NULL);
      return 0;
    }
    for (;;) {
      uint64_t current = la_load64_acq(&e->slot.hi);
      uint64_t prior, next;
      if (la_load64_acq(&e->slot.lo) != addr ||
	  hugetab_readers(current) == 0)
	return 0;
      if (hugetab_readers(current) != 1 ||
	  !(current & LJ_HUGEF_RETIRED) ||
	  hugetab_recovery_pending(current) ||
	  hugetab_root_pending(current) ||
	  (current & (LJ_HUGEF_MARK|LJ_HUGEF_FREEING|LJ_HUGEF_BUSY|
		      LJ_HUGEF_DEFER_FREE))) {
	(void)hugetab_cert_lease_release(e, addr, p, NULL);
	return 0;
      }
      prior = current - LJ_HUGETAB_READER_ONE;
      next = (prior & ~(uint64_t)LJ_HUGEF_RETIRED) |
	LJ_HUGEF_FREEING|LJ_HUGEF_BUSY;
      if (!hugetab_cas_meta(e, addr, current, next))
	continue;
      /* BUSY atomically replaces the counted lease. Recheck the independent
      ** descriptor/token word before returning semantic body ownership. */
      for (;;) {
	uint64_t owned = la_load64_acq(&e->slot.hi);
	uint64_t publish;
	if (la_load64_acq(&e->slot.lo) != addr ||
	    (owned & (LJ_HUGEF_FREEING|LJ_HUGEF_BUSY)) !=
	      (LJ_HUGEF_FREEING|LJ_HUGEF_BUSY))
	  return 0;
	if ((owned & (LJ_HUGEF_MARK|LJ_HUGEF_DEFER_FREE)) ||
	    hugetab_readers(owned) != 0 ||
	    hugetab_recovery_pending(owned) ||
	    hugetab_root_pending(owned) ||
	    !hugetab_gc2_reclaim_clear_acq(p, owned)) {
	  (void)hugetab_destruct_abandon(e, addr, prior);
	  return 0;
	}
	publish = owned & ~(uint64_t)LJ_HUGEF_BUSY;
	if (hugetab_cas_meta(e, addr, owned, publish)) {
	  hugetab_decode(publish, hi);
	  return 1;
	}
      }
    }
  }
}

int lj_arena_hugetab_claim_live_ticket(HugeTab *ht, const void *p,
					       LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta))
      return 0;
    if ((meta & (LJ_HUGEF_MARK|LJ_HUGEF_TICKET)) !=
	(LJ_HUGEF_MARK|LJ_HUGEF_TICKET) ||
	hugetab_readers(meta) != 0 ||
	(meta & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING|LJ_HUGEF_BUSY)))
      return 0;
    /* BUSY is a transient ownership claim. TICKET stays set until the exact
    ** header is linked and retire_obj has been cleared. */
    next = meta | LJ_HUGEF_BUSY;
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      return 1;
    }
  }
}

int lj_arena_hugetab_finish_live_ticket(HugeTab *ht, const void *p,
						LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    int progress;
    if (!hugetab_search(h, addr, &e, &meta))
      return 0;
    if ((meta & (LJ_HUGEF_MARK|LJ_HUGEF_TICKET|LJ_HUGEF_BUSY)) !=
	(LJ_HUGEF_MARK|LJ_HUGEF_TICKET|LJ_HUGEF_BUSY) ||
	(meta & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING)))
      return 0;
    next = meta & ~(uint64_t)(LJ_HUGEF_TICKET|LJ_HUGEF_BUSY);
    next = hugetab_fold_deferred_free(p, next, &progress);
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      if (progress != LJ_HUGETAB_FOLD_NONE)
	arena_progress_wake(lj_arena_of(p));
      return 1;
    }
  }
}

enum {
  LJ_HUGE_EXT_MISSING = 0,
  LJ_HUGE_EXT_CLAIMED = 1,
  LJ_HUGE_EXT_OWNED = 2,
  LJ_HUGE_EXT_CONTENDED = 3
};

/* Undo only this pre-destructor's opaque claim after a late descriptor veto.
** FREEING rejects new readers/roots/recovery, but an external logical free may
** still add DEFER_FREE while BUSY is held. Preserve that durable intent and
** every unrelated concurrent metadata change. MARK/RETIRED were cleared by
** the claim itself, so conservatively restore their pre-claim liveness state
** (with MARK dominant) before releasing or handing off the opaque claim. */
static int hugetab_destruct_abandon(LJHugeEnt *e, uint64_t addr,
				    uint64_t prior)
{
  for (;;) {
    uint64_t current = la_load64_acq(&e->slot.hi);
    uint64_t restore;
    int wake = 0;
    if (la_load64_acq(&e->slot.lo) != addr ||
	(current & (LJ_HUGEF_FREEING|LJ_HUGEF_BUSY)) !=
	  (LJ_HUGEF_FREEING|LJ_HUGEF_BUSY))
      return 0;
    restore = current & ~(uint64_t)(LJ_HUGEF_FREEING|LJ_HUGEF_BUSY);
    if ((current | prior) & LJ_HUGEF_MARK) {
      restore = (restore | LJ_HUGEF_MARK) & ~(uint64_t)LJ_HUGEF_RETIRED;
    } else if (prior & LJ_HUGEF_RETIRED) {
      restore |= LJ_HUGEF_RETIRED;
    }
    if (restore & LJ_HUGEF_DEFER_FREE) {
      uint64_t before = restore;
      int progress;
      restore = hugetab_fold_deferred_free(
	(const void *)(uintptr_t)addr, restore, &progress);
      wake = progress != LJ_HUGETAB_FOLD_NONE ||
	(!(before & LJ_HUGEF_SWEEP_OLD) && (restore & LJ_HUGEF_SWEEP_OLD));
    }
    if (hugetab_cas_meta(e, addr, current, restore)) {
      if (wake)
	arena_progress_wake(lj_arena_of((void *)(uintptr_t)addr));
      return 1;
    }
  }
}

int lj_arena_hugetab_destruct_acquire(HugeTab *ht, const void *p,
					       LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return LJ_ARENA_DESTRUCT_LOST;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t leased;
    int lease;
    if (!hugetab_search(h, addr, &e, &meta))
      return LJ_ARENA_DESTRUCT_LOST;
    if (meta & LJ_HUGEF_FREEING) {
      hugetab_decode(meta, hi);
      return LJ_ARENA_DESTRUCT_OWNED;
    }
    /* Unlike a raw external free, a pre-destructor attempt still owns live
    ** semantic work. Lose without publishing terminal intent so the caller
    ** can preserve/requeue the body and retry after this exact owner leaves. */
    if ((meta & LJ_HUGEF_DEFER_FREE) || hugetab_readers(meta) != 0 ||
	hugetab_recovery_pending(meta) || hugetab_root_pending(meta) ||
	(meta & LJ_HUGEF_BUSY)) {
	hugetab_decode(meta, hi);
	return LJ_ARENA_DESTRUCT_LOST;
    }
    lease = hugetab_cert_lease_acquire(e, addr, meta, &leased);
    if (lease < 0)
	continue;
    if (lease == 0)
	return LJ_ARENA_DESTRUCT_LOST;
    if (!hugetab_gc2_reclaim_clear_acq(p, leased)) {
	(void)hugetab_cert_lease_release(e, addr, p, NULL);
	return LJ_ARENA_DESTRUCT_LOST;
    }
    for (;;) {
	uint64_t current = la_load64_acq(&e->slot.hi);
	uint64_t prior, next;
	if (la_load64_acq(&e->slot.lo) != addr ||
	    hugetab_readers(current) == 0)
	  return LJ_ARENA_DESTRUCT_LOST;
	if (hugetab_readers(current) != 1 ||
	    hugetab_recovery_pending(current) ||
	    hugetab_root_pending(current) ||
	    (current & (LJ_HUGEF_DEFER_FREE|LJ_HUGEF_FREEING|
			LJ_HUGEF_BUSY))) {
	  (void)hugetab_cert_lease_release(e, addr, p, NULL);
	  return LJ_ARENA_DESTRUCT_LOST;
	}
	prior = current - LJ_HUGETAB_READER_ONE;
	next = (prior & ~(uint64_t)(LJ_HUGEF_MARK|LJ_HUGEF_RETIRED)) |
	  LJ_HUGEF_FREEING|LJ_HUGEF_BUSY;
	if (!hugetab_cas_meta(e, addr, current, next))
	  continue;
	if (next & LJ_HUGEF_SWEEP_OLD)
	  la_store64_rel(&lj_arena_of(p)->hdr.retire_epoch, ~(uint64_t)0);
	/* Lawful publishers participate in the counted-reader handshake and would
	** have defeated the sole-reader CAS above. This second sample is defensive
	** containment for joined-world raw fault injection; the successful first
	** sample plus destructive CAS is the production reclamation cutoff. */
	if (!hugetab_gc2_reclaim_clear_acq(p, next)) {
	  (void)hugetab_destruct_abandon(e, addr, prior);
	  return LJ_ARENA_DESTRUCT_LOST;
      }
      hugetab_decode(next, hi);
      return LJ_ARENA_DESTRUCT_ACQUIRED;
    }
  }
}

/* Atomically choose the external-free side of prepare-vs-free. If PREPARE has
** not published SWEEP_OLD, BUSY makes this caller the terminal table deleter.
** If PREPARE won, the same CAS pins the mapping until finish hands it to the
** sole sweep deleter. No header access precedes this ownership transition. */
static int hugetab_claim_external_free(HugeTab *ht, const void *p,
					int require_sweep, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return LJ_HUGE_EXT_MISSING;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t leased;
    int lease;
    if (!hugetab_search(h, addr, &e, &meta))
      return LJ_HUGE_EXT_MISSING;
    if (require_sweep && !(meta & LJ_HUGEF_SWEEP_OLD))
      return LJ_HUGE_EXT_MISSING;
    if (meta & LJ_HUGEF_DEFER_FREE) {
      hugetab_decode(meta, hi);
      return LJ_HUGE_EXT_CONTENDED;
    }
    if (hugetab_readers(meta) != 0 || hugetab_recovery_pending(meta) ||
	hugetab_root_pending(meta) || (meta & LJ_HUGEF_BUSY)) {
      /* Reader, recovery, root and every BUSY owner arbitrate in this exact
      ** metadata word. The external caller has irrevocably relinquished the
      ** allocation: publish intent without touching the mapping and let the
      ** final owner fold it into a fresh-grace sweep handoff. */
      uint64_t deferred = meta | LJ_HUGEF_DEFER_FREE;
	if (hugetab_cas_meta(e, addr, meta, deferred)) {
	hugetab_decode(deferred, hi);
	if (deferred & LJ_HUGEF_SWEEP_OLD)
	  arena_progress_wake(lj_arena_of(p));
	return LJ_HUGE_EXT_CONTENDED;
      }
      continue;
    }
    if (meta & LJ_HUGEF_FREEING) {
      hugetab_decode(meta, hi);
      return LJ_HUGE_EXT_OWNED;
    }
    lease = hugetab_cert_lease_acquire(e, addr, meta, &leased);
    if (lease < 0)
      continue;
    if (lease == 0)
      continue;
    if (!hugetab_gc2_reclaim_clear_acq(p, leased)) {
      (void)hugetab_cert_lease_defer(e, addr, p, hi, NULL);
      return LJ_HUGE_EXT_CONTENDED;
    }
    for (;;) {
      uint64_t current = la_load64_acq(&e->slot.hi);
      uint64_t prior, next;
      if (la_load64_acq(&e->slot.lo) != addr ||
	  hugetab_readers(current) == 0)
	return LJ_HUGE_EXT_CONTENDED;
      /* Any owner admitted after our counted pin wins body access. The free
      ** itself is already irrevocable, so consume only our count while
      ** publishing its durable handoff. */
      if (hugetab_readers(current) != 1 ||
	  hugetab_recovery_pending(current) ||
	  hugetab_root_pending(current) ||
	  (current & (LJ_HUGEF_DEFER_FREE|LJ_HUGEF_FREEING|
		      LJ_HUGEF_BUSY))) {
	(void)hugetab_cert_lease_defer(e, addr, p, hi, NULL);
	return LJ_HUGE_EXT_CONTENDED;
      }
      prior = current - LJ_HUGETAB_READER_ONE;
      next = (prior & ~(uint64_t)(LJ_HUGEF_MARK|LJ_HUGEF_RETIRED)) |
	LJ_HUGEF_FREEING|LJ_HUGEF_BUSY;
      if (!hugetab_cas_meta(e, addr, current, next))
	continue;
      if (next & LJ_HUGEF_SWEEP_OLD)
	la_store64_rel(&lj_arena_of(p)->hdr.retire_epoch, ~(uint64_t)0);
      if (!hugetab_gc2_reclaim_clear_acq(p, next)) {
	/* BUSY atomically replaced the lease. This defensive raw-fault sample can
	** still hand the logical free to sweep; lawful publishers would instead
	** have changed the reader count and defeated the claim CAS above. */
	for (;;) {
	  uint64_t owned = la_load64_acq(&e->slot.hi);
	  uint64_t deferred;
	  int progress;
	  if (la_load64_acq(&e->slot.lo) != addr ||
	      (owned & (LJ_HUGEF_FREEING|LJ_HUGEF_BUSY)) !=
		(LJ_HUGEF_FREEING|LJ_HUGEF_BUSY))
	    return LJ_HUGE_EXT_CONTENDED;
	  deferred = (owned & ~(uint64_t)LJ_HUGEF_BUSY) |
	    LJ_HUGEF_DEFER_FREE|LJ_HUGEF_SWEEP_OLD;
	  deferred = hugetab_fold_deferred_free(p, deferred, &progress);
	  if (!hugetab_cas_meta(e, addr, owned, deferred))
	    continue;
	  hugetab_decode(deferred, hi);
	  arena_progress_wake(lj_arena_of(p));
	  return LJ_HUGE_EXT_CONTENDED;
	}
      }
      hugetab_decode(next, hi);
      return LJ_HUGE_EXT_CLAIMED;
    }
  }
}

int lj_arena_hugetab_claim_external_free(HugeTab *ht, const void *p,
					   LJHugeInfo *hi)
{
  return hugetab_claim_external_free(ht, p, 0, hi) ==
	 LJ_HUGE_EXT_CLAIMED;
}

enum {
  LJ_HUGE_REALLOC_CLAIM_LOST = 0,
  LJ_HUGE_REALLOC_CLAIM_OWNER = 1
};

enum {
  LJ_HUGE_REALLOC_KEEP_LOST = 0,
  LJ_HUGE_REALLOC_KEEP_DONE = 1,
  LJ_HUGE_REALLOC_KEEP_MOVE = 2,
  LJ_HUGE_REALLOC_KEEP_PREEMPTED = 3
};

enum {
  LJ_HUGE_REALLOC_MOVE_LOST = 0,
  LJ_HUGE_REALLOC_MOVE_DIRECT = 1,
  LJ_HUGE_REALLOC_MOVE_DEFERRED = 2,
  LJ_HUGE_REALLOC_MOVE_PREEMPTED = 3
};

/* A realloc pin is nonterminal: it excludes prepare/free/header teardown while
** preserving the old allocation if replacement allocation fails. Counted raw
** readers may already exist; BUSY pins the mapping while the owner moves it
** and the eventual old-slot handoff retains those exact readers. Traversable
** allocations have address-bound GC identity and are never reallocable. */
static int hugetab_claim_realloc(HugeTab *ht, const void *p, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta) ||
	(meta & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_FREEING|
		 LJ_HUGEF_RETIRED|LJ_HUGEF_TICKET|LJ_HUGEF_DEFER_FREE|
		 LJ_HUGEF_RECOVERY_MASK|LJ_HUGEF_ROOT_MASK|
		 LJ_HUGEF_SWEEP_OLD)))
      return LJ_HUGE_REALLOC_CLAIM_LOST;
    /* Another resize owns this exact incarnation. A successful nonwaiting
    ** follower needs a generation/forwarding descriptor: allocating first has
    ** address ABA, while preempting first cannot preserve failed-realloc
    ** semantics. Reject without reading the mapping or reporting success. */
    if (meta & LJ_HUGEF_BUSY)
      return LJ_HUGE_REALLOC_CLAIM_LOST;
    next = meta | LJ_HUGEF_BUSY;
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      hugetab_test_realloc_pause_after_busy();
      return LJ_HUGE_REALLOC_CLAIM_OWNER;
    }
  }
}

static int hugetab_finish_realloc_keep(HugeTab *ht, const void *p,
					size_t nsize, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t packed_addr, next;
    uint32_t flags;
    if (!hugetab_search(h, addr, &e, &meta) ||
	!(meta & LJ_HUGEF_BUSY) || (meta & LJ_HUGEF_FREEING))
      return LJ_HUGE_REALLOC_KEEP_LOST;
    if (meta & LJ_HUGEF_DEFER_FREE) {
      int progress;
      next = meta & ~(uint64_t)LJ_HUGEF_BUSY;
      next = hugetab_fold_deferred_free(p, next, &progress);
      if (hugetab_cas_meta(e, addr, meta, next)) {
	hugetab_decode(next, hi);
	if (progress != LJ_HUGETAB_FOLD_NONE)
	  arena_progress_wake(lj_arena_of(p));
	return LJ_HUGE_REALLOC_KEEP_PREEMPTED;
      }
      continue;
    }
    if (hugetab_readers(meta) != 0 || hugetab_recovery_pending(meta) ||
	hugetab_root_pending(meta))
      return LJ_HUGE_REALLOC_KEEP_MOVE;
    flags = (uint32_t)meta & LJ_HUGEF_MASK;
    flags &= ~LJ_HUGEF_BUSY;
    if (!hugetab_pack((void *)(uintptr_t)addr, nsize, flags,
		      &packed_addr, &next) || packed_addr != addr)
      return LJ_HUGE_REALLOC_KEEP_LOST;
    /* hugetab_pack resets the bounded reader field; the exact checks above
    ** proved it zero and DEFER_FREE absent in the same slot snapshot. */
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      return LJ_HUGE_REALLOC_KEEP_DONE;
    }
  }
}

static int hugetab_release_realloc(HugeTab *ht, const void *p,
				    LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    int progress;
    if (!hugetab_search(h, addr, &e, &meta) ||
	!(meta & LJ_HUGEF_BUSY) || (meta & LJ_HUGEF_FREEING))
      return 0;
    next = meta & ~(uint64_t)LJ_HUGEF_BUSY;
    next = hugetab_fold_deferred_free(p, next, &progress);
    if (hugetab_cas_meta(e, addr, meta, next)) {
      hugetab_decode(next, hi);
      if (progress != LJ_HUGETAB_FOLD_NONE)
	arena_progress_wake(lj_arena_of(p));
      return 1;
    }
  }
}

static int hugetab_realloc_to_external_free(HugeTab *ht, const void *p,
					      LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    int progress;
    if (!hugetab_search(h, addr, &e, &meta) ||
	!(meta & LJ_HUGEF_BUSY) || (meta & LJ_HUGEF_FREEING))
      return LJ_HUGE_REALLOC_MOVE_LOST;
    if (meta & LJ_HUGEF_DEFER_FREE) {
      next = meta & ~(uint64_t)LJ_HUGEF_BUSY;
      next = hugetab_fold_deferred_free(p, next, &progress);
      if (hugetab_cas_meta(e, addr, meta, next)) {
	hugetab_decode(next, hi);
	if (progress != LJ_HUGETAB_FOLD_NONE)
	  arena_progress_wake(lj_arena_of(p));
	return LJ_HUGE_REALLOC_MOVE_PREEMPTED;
      }
      continue;
    }
    if (hugetab_readers(meta) != 0 || hugetab_recovery_pending(meta) ||
	hugetab_root_pending(meta)) {
      /* This owner won before the ordinary readers. Publish its logical move
      ** while retaining their old geometry and exact lifetime tokens. */
      next = (meta & ~(uint64_t)LJ_HUGEF_BUSY) | LJ_HUGEF_DEFER_FREE;
      next = hugetab_fold_deferred_free(p, next, &progress);
      if (hugetab_cas_meta(e, addr, meta, next)) {
	hugetab_decode(next, hi);
	if (progress != LJ_HUGETAB_FOLD_NONE)
	  arena_progress_wake(lj_arena_of(p));
	return LJ_HUGE_REALLOC_MOVE_DEFERRED;
      }
      continue;
    }
    next = (meta & ~(uint64_t)(LJ_HUGEF_MARK|LJ_HUGEF_RETIRED)) |
	   LJ_HUGEF_FREEING;  /* Retain BUSY continuously through the copy. */
    if (hugetab_cas_meta(e, addr, meta, next)) {
      if (next & LJ_HUGEF_SWEEP_OLD)
	la_store64_rel(&lj_arena_of(p)->hdr.retire_epoch, ~(uint64_t)0);
      hugetab_decode(next, hi);
      return LJ_HUGE_REALLOC_MOVE_DIRECT;
    }
  }
}

int lj_arena_hugetab_finish_external_free(HugeTab *ht, const void *p,
					    LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return LJ_ARENA_HUGE_FINISH_LOST;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    la_u128 exp, des;
    if (!hugetab_search(h, addr, &e, &meta) ||
	hugetab_readers(meta) != 0 || hugetab_recovery_pending(meta) ||
	hugetab_root_pending(meta) ||
	(meta & (LJ_HUGEF_FREEING|LJ_HUGEF_BUSY)) !=
	  (LJ_HUGEF_FREEING|LJ_HUGEF_BUSY))
      return LJ_ARENA_HUGE_FINISH_LOST;
    exp.lo = addr;
    exp.hi = meta;
    if (!hugetab_gc2_reclaim_clear_acq(p, meta)) {
      uint64_t next = (meta & ~(uint64_t)LJ_HUGEF_BUSY) |
	LJ_HUGEF_DEFER_FREE;
      /* The caller's logical free is irrevocable, but physical scan authority
      ** appeared after its BUSY claim. Relinquish opaque ownership and leave a
      ** durable helper-visible intent; exact token/descriptor completion will
      ** retry the common fold and wake sweep progress. */
      next = hugetab_fold_deferred_free(p, next, NULL);
      des.lo = addr;
      des.hi = next;
      if (la_cas128(&e->slot, &exp, des)) {
	hugetab_decode(next, hi);
	arena_progress_wake(lj_arena_of(p));
	return LJ_ARENA_HUGE_FINISH_DEFERRED;
      }
      continue;
    }
    if (meta & LJ_HUGEF_SWEEP_OLD) {
      uint64_t next = meta &
	~(uint64_t)(LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE);
      /* This is the final header store while BUSY excludes both reanchor and
      ** retirement publication. Its release edge precedes exposing FREEING
      ** to the sweep owner, which must complete a fresh grace before unmap. */
      la_store64_rel(&lj_arena_of(p)->hdr.retire_epoch, ~(uint64_t)0);
      des.lo = addr;
      des.hi = next;
      if (la_cas128(&e->slot, &exp, des)) {
	hugetab_decode(next, hi);
	return LJ_ARENA_HUGE_FINISH_DEFERRED;
      }
    } else {
      des.lo = LJ_HUGETAB_TOMBSTONE;
      des.hi = 0;
      if (la_cas128(&e->slot, &exp, des)) {
	hugetab_table_topology_changed(ht);
	hugetab_decode(meta, hi);
	return LJ_ARENA_HUGE_FINISH_UNMAP;
      }
    }
  }
}

int lj_arena_hugetab_defer_external_free(HugeTab *ht, const void *p,
					   LJHugeInfo *hi)
{
  LJHugeInfo snap;
  int claim = hugetab_claim_external_free(ht, p, 1, &snap);
  if (claim == LJ_HUGE_EXT_MISSING || claim == LJ_HUGE_EXT_CONTENDED)
    return 0;
  if (claim == LJ_HUGE_EXT_OWNED) {
    if (hi)
      *hi = snap;
    return 1;  /* A nonwaiting duplicate never touches the mapping header. */
  }
  if (lj_arena_hugetab_finish_external_free(ht, p, &snap) !=
      LJ_ARENA_HUGE_FINISH_DEFERRED)
    return 0;
  if (hi)
    *hi = snap;
  return 1;
}

int lj_arena_hugetab_retry_deferred(HugeTab *ht, const void *p,
				     LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    int lease;
    if (!hugetab_search(h, addr, &e, &meta) ||
	!(meta & LJ_HUGEF_DEFER_FREE))
      return 0;
    if (hugetab_readers(meta) != 0 || hugetab_recovery_pending(meta) ||
	hugetab_root_pending(meta) || (meta & LJ_HUGEF_BUSY))
      return 0;
    lease = hugetab_cert_lease_acquire(e, addr, meta, NULL);
    if (lease < 0)
      continue;
    if (lease == 0)
      return 0;
    /* The retry's counted reader is the mapping lease while the held-only
    ** fold samples the independent certificate. Its exact CAS consumes that
    ** count and either retains the durable marker or publishes FREEING. */
    for (;;) {
      uint64_t current = la_load64_acq(&e->slot.hi);
      uint64_t next;
      int progress;
      if (la_load64_acq(&e->slot.lo) != addr ||
	  hugetab_readers(current) == 0)
	return 0;
      next = current - LJ_HUGETAB_READER_ONE;
      next = hugetab_fold_deferred_free(p, next, &progress);
      if (!hugetab_cas_meta(e, addr, current, next))
	continue;
      hugetab_decode(next, hi);
      if (progress == LJ_HUGETAB_FOLD_SCHEDULED ||
	  progress == LJ_HUGETAB_FOLD_FREEING)
	arena_progress_wake(lj_arena_of(p));
      return progress == LJ_HUGETAB_FOLD_FREEING;
    }
  }
}

int lj_arena_hugetab_revert_retired(HugeTab *ht, const void *p)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr, meta;
  LJHugeEnt *e;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    uint64_t next;
    if (!hugetab_search(h, addr, &e, &meta))
      return 0;
    if (!(meta & LJ_HUGEF_FREEING) || hugetab_readers(meta) != 0 ||
	(meta & (LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE)))
      return 0;
    next = (meta & ~(uint64_t)LJ_HUGEF_FREEING) | LJ_HUGEF_RETIRED;
    if (hugetab_cas_meta(e, addr, meta, next))
      return 1;
  }
}

uint64_t lj_arena_hugetab_live_bytes(HugeTab *ht, uint32_t required_flags)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t bytes = 0;
  uint32_t i, cap;
  if (!h)
    return 0;
  required_flags &= LJ_HUGEF_MASK;
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);  /* 04 §4.5.1 slot state. */
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);  /* 04 §4.5.1 metadata. */
      if (la_load64_acq(&e->slot.lo) == addr) {  /* Stable snapshot. */
	uint32_t hflags = (uint32_t)(meta & LJ_HUGETAB_META_MASK);
	if ((hflags & required_flags) == required_flags) {
	  size_t size = hugetab_size(meta);
	  if (bytes > ~(uint64_t)0 - (uint64_t)size)
	    bytes = ~(uint64_t)0;
	  else
	    bytes += (uint64_t)size;
	}
      }
    }
  }
  return bytes;
}

int lj_arena_hugetab_transfer(HugeTab *dst, HugeTab *src, uint32_t owner_tid)
{
  LJHugeTabHdr *h = src ? src->h : NULL;
  LJGC2TableTopology *src_topology, *dst_topology;
  uint32_t i, cap;
  if (!h)
    return 1;
  if (dst == src)
    return 1;
  if (!dst || !dst->h)
    return 0;
  src_topology = hugetab_table_topology_acq(src);
  dst_topology = hugetab_table_topology_acq(dst);
  if ((src_topology || dst_topology) && src_topology != dst_topology)
    return 0;  /* Membership may move only within one enumerated universe. */
  cap = h->mask + 1u;
  for (i = 0; i < cap; i++) {
    LJHugeEnt *e = &h->ent[i];
    uint64_t addr = la_load64_acq(&e->slot.lo);
    if (addr > LJ_HUGETAB_TOMBSTONE) {
      uint64_t meta = la_load64_acq(&e->slot.hi);
      if (la_load64_acq(&e->slot.lo) == addr) {
	void *p = (void *)(uintptr_t)addr;
	size_t size;
	uint32_t hflags;
	uint64_t leased, closed;
	la_u128 exp, des;
	int inserted, lease;
	/* Recovery count/claim ownership is table-local. Without an explicit
	** MOVING descriptor, copying the state and tombstoning the source would
	** create two apparent owners for one eventual decrement. Retain the dead
	** source owner until recovery drains instead. */
	if (hugetab_readers(meta) != 0 || hugetab_recovery_pending(meta))
	  return 0;
	lease = hugetab_cert_lease_acquire(e, addr, meta, &leased);
	if (lease < 0) {
	  i--;
	  continue;
	}
	if (lease == 0)
	  return 0;
	if (!hugetab_cert_lease_close(e, addr, &closed)) {
	  (void)hugetab_cert_lease_release(e, addr, p, NULL);
	  return 0;
	}
	if (hugetab_recovery_pending(closed) ||
	    (closed & LJ_HUGEF_DEFER_FREE) ||
	    !hugetab_gc2_reclaim_clear_acq(p, closed)) {
	  (void)hugetab_admission_reopen(e, addr, p, NULL);
	  return 0;
	}
	size = hugetab_size(closed);
	hflags = (uint32_t)(closed & LJ_HUGETAB_META_MASK);
	inserted = hugetab_insert(dst, p, size, hflags, 1);
	if (inserted < 0) {
	  (void)hugetab_admission_reopen(e, addr, p, NULL);
	  return 0;
	}
	if (inserted == 0) {
	  LJHugeInfo existing;
	  if (lj_arena_hugetab_lookup(dst, p, &existing) != 1 ||
	      existing.size != size || existing.flags != hflags ||
	      existing.readers != 0) {
	    (void)hugetab_admission_reopen(e, addr, p, NULL);
	    return 0;
	  }
	}
	/* The source owner is dead and the surrounding TG writer gate has proved
	** quiescence. Make each entry transfer transactional even if an abandoned
	** BUSY/FREEING state would make the ordinary delete refuse: destination
	** insert/confirm, exact source tombstone, then and only then publish the new
	** header owner. A later capacity failure can never leave a stale source
	** duplicate pointing at a mapping which the destination may unmap. */
	exp.lo = addr;
	exp.hi = closed;
	des.lo = LJ_HUGETAB_TOMBSTONE;
	des.hi = 0;
	if (!hugetab_gc2_reclaim_clear_acq(p, closed) ||
	    !la_cas128(&e->slot, &exp, des)) {
	  if (inserted > 0 &&
	      !hugetab_forget_terminal(dst, p, NULL, 1, 1))
	    abort();  /* Never return while leaving a new duplicate behind. */
	  (void)hugetab_admission_reopen(e, addr, p, NULL);
	  return 0;
	}
	hugetab_table_topology_changed(src);
	lj_arena_owner_rel(lj_arena_of(p), owner_tid);
      }
    }
  }
  return 1;
}

int lj_arena_hugetab_delete(HugeTab *ht, const void *p, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = ht ? ht->h : NULL;
  uint64_t addr;
  if (!h || !p)
    return 0;
  addr = (uint64_t)(uintptr_t)p;
  for (;;) {
    LJHugeEnt *e;
    uint64_t meta, leased, closed;
    int lease;
    la_u128 exp, des;
    if (!hugetab_search(h, addr, &e, &meta))
      return 0;
    if (hugetab_readers(meta) != 0 ||
	(meta & (LJ_HUGEF_BUSY|LJ_HUGEF_RECOVERY_MASK|
		 LJ_HUGEF_ROOT_MASK|LJ_HUGEF_DEFER_FREE)))
      return 0;  /* Header publication/reanchor still owns the mapping. */
    lease = hugetab_cert_lease_acquire(e, addr, meta, &leased);
    if (lease < 0)
      continue;
    if (lease == 0)
      return 0;
    if (!hugetab_cert_lease_close(e, addr, &closed)) {
      (void)hugetab_cert_lease_release(e, addr, p, NULL);
      return 0;
    }
    /* The close consumed our sole certificate count before this validation.
    ** A completed acquire/release ABA is therefore covered by its durable
    ** token or still-ACTIVE exact descriptor, and no later publisher can enter
    ** between the successful validation and tombstone CAS. */
    for (;;) {
      uint64_t current = la_load64_acq(&e->slot.hi);
      if (la_load64_acq(&e->slot.lo) != addr ||
	  !hugetab_admission_closed(current)) {
	(void)hugetab_admission_reopen(e, addr, p, NULL);
	return 0;
      }
      if (
	  hugetab_recovery_pending(current) ||
	  hugetab_root_pending(current) ||
	  (current & (LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE)) ||
	  !hugetab_gc2_reclaim_clear_acq(p, current)) {
	(void)hugetab_admission_reopen(e, addr, p, NULL);
	return 0;
      }
      exp.lo = addr;
      exp.hi = current;
      des.lo = LJ_HUGETAB_TOMBSTONE;
      des.hi = 0;
      if (la_cas128(&e->slot, &exp, des)) {  /* 04 §4.5.1 delete LP. */
	hugetab_table_topology_changed(ht);
	hugetab_decode(current & ~(uint64_t)LJ_HUGETAB_READER_MASK, hi);
	return 1;
      }
    }
  }
}

static uint32_t arena_kind(uint32_t flags)
{
  return (flags & LJ_AF_TRAVERSABLE) ? LJ_ARENAK_TRAVERSABLE :
				       LJ_ARENAK_PLAIN;
}

static uint32_t arena_registry_hflags(uint32_t flags)
{
  uint32_t hflags = 0;
  if (flags & LJ_AF_TRAVERSABLE)
    hflags |= LJ_HUGEF_TRAVERSABLE;
  return hflags;
}

static void arena_registered_set(GCArena *a)
{
  uint32_t old;
  if (!a)
    return;
  old = lj_arena_flags_acq(a);
  while (!(old & LJ_AF_REGISTERED)) {
    uint32_t expect = old;
    if (la_cas32(&a->hdr.flags, &expect, old | LJ_AF_REGISTERED,
		 LA_ACQ_REL, LA_ACQ))
      return;
    old = expect;
  }
}

static void arena_registered_clear(GCArena *a)
{
  uint32_t old;
  if (!a)
    return;
  old = lj_arena_flags_acq(a);
  while (old & LJ_AF_REGISTERED) {
    uint32_t expect = old;
    if (la_cas32(&a->hdr.flags, &expect,
		 old & (uint32_t)~LJ_AF_REGISTERED, LA_ACQ_REL, LA_ACQ))
      return;
    old = expect;
  }
}

static uint32_t arena_bin(uint32_t ncells)
{
  return lj_arena_bin_from_ncells(ncells);
}

#define LJ_ARENA_BIN_WALK_LIMIT 8192u

/* Return 2 for an unmanaged plain arena, 1 for a held traversable DESTRUCT
** lane and 0 on a nonwaiting loss. No bitmap/header/body mutation is allowed
** before the post-claim ownership and reader-generation rechecks succeed. */
static int arena_destruct_claim_live(GCArena *a, uint32_t cell,
				     uint64_t own_open_count)
{
  if (!arena_lifetime_managed(a))
    return 2;
  if (!lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
					    LJ_ARENA_LIFETIME_DESTRUCT))
    return 0;
  arena_test_lifetime_pause_after_claim();
  if (!arena_mutation_open_quiet(a, own_open_count) ||
      !lj_arena_bm_get(a->block, cell) ||
      !arena_side_owners_none(a, cell) ||
      !lj_arena_gc2_desc_clear_acq(a, cell)) {
    (void)arena_destruct_restore_live(a, cell);
    return 0;
  }
  return 1;
}

static int arena_destruct_commit_free(GCArena *a, uint32_t cell)
{
  if (!arena_lifetime_managed(a))
    return 1;
  if (!lj_arena_gc2_reclaim_clear_acq(a, cell)) {
    /* GC2 scan ownership is a conservative live result. Restore the tentative
    ** lane here because several sweep callers historically relied on a failed
    ** FREE CAS meaning that a rescue actor had already performed restoration. */
    (void)lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_DESTRUCT,
					  LJ_ARENA_LIFETIME_LIVE);
    return 0;
  }
  return lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_DESTRUCT,
					      LJ_ARENA_LIFETIME_FREE);
}

static int arena_destruct_restore_live(GCArena *a, uint32_t cell)
{
  return !arena_lifetime_managed(a) ||
    lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_DESTRUCT,
					LJ_ARENA_LIFETIME_LIVE);
}

/* Non-destructive layout mutation (currently small in-place realloc). */
static int arena_mutation_claim_live(GCArena *a, uint32_t cell,
				      uint64_t own_open_count)
{
  if (!arena_lifetime_managed(a))
    return 2;
  if (!lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
					    LJ_ARENA_LIFETIME_MUTATING))
    return 0;
  if (!arena_mutation_open_quiet(a, own_open_count) ||
      !lj_arena_bm_get(a->block, cell) ||
      !arena_side_owners_none(a, cell) ||
      !lj_arena_gc2_desc_clear_acq(a, cell)) {
    (void)lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_MUTATING, LJ_ARENA_LIFETIME_LIVE);
    return 0;
  }
  return 1;
}

static void arena_mutation_restore_live(GCArena *a, uint32_t cell)
{
  if (arena_lifetime_managed(a)) {
    int ok = lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_MUTATING, LJ_ARENA_LIFETIME_LIVE);
    lj_assertX(ok, "arena non-destructive mutation restore lost");
    UNUSED(ok);
  }
}

static void arena_alloc_claim_rollback(GCArena *a, uint32_t cell,
					int root_construct)
{
  int ok;
  if (!arena_lifetime_managed(a))
    return;
  if (root_construct) {
    ok = lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_LINKING,
				 LJ_ARENA_ROOT_NONE);
    lj_assertX(ok, "arena root-construction allocation rollback lost root");
    if (!ok)
      return;  /* Fail closed: retained CONSTRUCT+root pins the span. */
    ok = lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_FREE);
  } else {
    ok = lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_MUTATING, LJ_ARENA_LIFETIME_FREE);
  }
  lj_assertX(ok, "arena allocation lifetime rollback lost");
}

static uint64_t arena_range_mask(uint32_t lo, uint32_t nbits)
{
  return nbits == 64u ? ~(uint64_t)0 :
    (((uint64_t)1 << nbits) - 1u) << lo;
}

/* Prove that every cell in a structural-reuse range is free of persistent
** ownership metadata. The packed-to-block converters are exact: any nonzero
** root/recovery/dtor state and every non-FREE lifetime nibble maps to its
** allocation cell. The surrounding owner-open/closed generation protocol is
** unchanged; this summary merely groups the former acquire observations and
** neither authorizes mutation nor replaces any later validation.
**
** READY is authoritative for arena_clear_extent_range(), where an old typed
** publication must veto boundary removal. arena_set_free_run() deliberately
** omits it: that path has already reached lifetime FREE with no destructor
** identity and defensively scrubs READY before its second, stricter interior
** validation through arena_clear_extent_range(). */
static LJ_AINLINE int arena_range_ownership_preflight(GCArena *a,
	uint32_t start, uint32_t len, int require_ready_clear,
	int abort_on_recovery)
{
  uint32_t pos, end;
  int lifetime_managed;
  if (start >= LJ_ARENA_CELLS || len > LJ_ARENA_CELLS - start)
    return 0;
  end = start + len;
  lifetime_managed = arena_lifetime_managed(a);
  for (pos = start; pos < end; ) {
    uint32_t wi = pos >> 6;
    uint32_t lo = pos & 63u;
    uint32_t take = end - pos;
    uint32_t room = 64u - lo;
    uint64_t mask, recovery, blockers;
    if (take > room)
      take = room;
    mask = arena_range_mask(lo, take);
    recovery = arena_recovery_block_bits(a, wi) & mask;
    blockers = arena_root_block_bits(a, wi) |
	       arena_dtor_block_bits(a, wi) |
	       arena_gc2_desc_block_bits(a, wi);
    if (lifetime_managed)
      blockers |= arena_lifetime_block_bits(a, wi);
    if (require_ready_clear)
      blockers |= la_load64_acq(&a->ready[wi]);
    blockers &= mask;
    if (recovery != 0 && abort_on_recovery) {
      uint64_t first_recovery = recovery & (0u - recovery);
      /* Preserve the old cell-order diagnostic policy on the cold corrupt
      ** path: an ordinary blocker in an earlier cell returns fail-closed;
      ** recovery at the same or an earlier cell remains a fatal invariant. */
      if ((blockers & (first_recovery - 1u)) == 0) {
	lj_assertX(0, "arena extent reuse crossed recovery ownership");
	abort();
      }
    }
    if (recovery != 0 || blockers != 0)
      return 0;
    if (lifetime_managed) {
      uint32_t cell;
      for (cell = pos; cell < pos + take; cell++)
	if (!arena_gc2_token_none_acq(a, cell))
	  return 0;
    }
    pos += take;
  }
  return 1;
}

static int arena_clear_extent_range(GCArena *a, uint32_t start, uint32_t len)
{
  uint32_t pos, end;
  /* Validate the complete range before changing either bitmap, so a failed
  ** side-plane check leaves every old boundary intact. */
  if (!arena_range_ownership_preflight(a, start, len, 1, 1))
    return 0;
  end = start + len;
  /* The complete preflight proves this is already opaque reusable storage:
  ** READY and dtor are zero, every lifetime lane is FREE, and no root/recovery
  ** owner exists. Remove old structural boundaries with release stores;
  ** mark[] admits concurrent marker ORs and therefore retains an atomic AND. */
  for (pos = start; pos < end; ) {
    uint32_t wi = pos >> 6;
    uint32_t lo = pos & 63u;
    uint32_t take = end - pos;
    uint32_t room = 64u - lo;
    uint64_t mask, word;
    if (take > room)
      take = room;
    mask = arena_range_mask(lo, take);
    word = la_load64_rlx(&a->block[wi]);
    la_store64_rel(&a->block[wi], word & ~mask);
    (void)la_and64_rlx(&a->mark[wi], ~mask);
    pos += take;
  }
  return 1;
}

/* Return 1 after committing the allocation, 0 for a candidate-local side
** descriptor veto, and -1 for a transient plain writer-generation veto.
** Neither failure result changes the requested allocation boundary. */
static int arena_set_alloc(GCArena *a, uint32_t cell, uint32_t ncells,
                           int black, int root_construct)
{
  uint32_t i;
  lj_assertX(ncells != 0 && cell >= LJ_AFIRST_CELL &&
	     ncells <= LJ_ARENA_CELLS - cell,
	     "arena allocation extent out of range");
  /* Plain writer completion may have published this free-run node while a
  ** counted late producer remains preempted. SEALED/PENDING is the committed
  ** generation veto: never consume or rewrite the node until it is opened. */
  if (!arena_lifetime_managed(a) &&
      (lj_arena_remote_active_acq(a) &
	(LJ_ARENA_REMOTE_SEALED|LJ_ARENA_REMOTE_PENDING)))
    return -1;  /* Transient plain-writer veto; try another arena. */
  if (root_construct && !arena_lifetime_managed(a))
    return 0;
  if (arena_lifetime_managed(a)) {
    if (!arena_gc2_token_none_acq(a, cell))
      return 0;
    int claimed = root_construct ?
      lj_arena_root_construct_claim(a, cell) :
      lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_FREE,
					  LJ_ARENA_LIFETIME_MUTATING);
    if (!claimed)
      return 0;
  }
  if (lj_arena_recovery_state_acq(a, cell) != LJ_ARENA_RECOVERY_IDLE ||
      !arena_gc2_token_none_acq(a, cell) ||
      (!root_construct &&
       lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE)) {
    arena_alloc_claim_rollback(a, cell, root_construct);
    return 0;
  }
  if (!root_construct && !arena_gc2_prepare_incarnation(a, cell)) {
    arena_alloc_claim_rollback(a, cell, 0);
    return 0;
  }
  /* Fail closed before mutating any old boundary. Only allocation starts may
  ** carry root state, but a complete preflight also makes corrupted/stale
  ** interior metadata unable to turn into reusable storage. */
  for (i = 1; i < ncells; i++)
    if (lj_arena_root_state_acq(a, cell + i) != LJ_ARENA_ROOT_NONE ||
	lj_arena_recovery_state_acq(a, cell + i) !=
	  LJ_ARENA_RECOVERY_IDLE ||
	(arena_lifetime_managed(a) &&
	 lj_arena_lifetime_state_acq(a, cell + i) !=
	   LJ_ARENA_LIFETIME_FREE)) {
      arena_alloc_claim_rollback(a, cell, root_construct);
      return 0;
    }
  /* Rebuilt free runs coalesce adjacent state-1 boundaries. Consuming such a
  ** run must erase every old interior boundary before publishing its new
  ** allocation start; otherwise a later rebuild can relink a suffix of this
  ** live allocation as reusable storage. */
  if (ncells > 1 &&
      !arena_clear_extent_range(a, cell + 1u, ncells - 1u)) {
    arena_alloc_claim_rollback(a, cell, root_construct);
    return 0;
  }
  /* Free-run publication clears typed coverage for the complete reusable
  ** span. Ordinary allocation must not pay side-plane RMWs. */
  if (LJ_UNLIKELY(lj_arena_cdata_get(a, cell) ||
		  lj_arena_ready_get(a, cell) ||
		  lj_arena_dtor_kind_acq(a, cell) != LJ_ARENA_DTOR_NONE)) {
    lj_assertX(0, "arena allocation reused published typed metadata");
    arena_alloc_claim_rollback(a, cell, root_construct);
    return 0;  /* Retain the span; never publish over stale type authority. */
  }
  if (black)
    lj_arena_bm_set(a->mark, cell);
  else
    lj_arena_bm_clear(a->mark, cell);
  /* Release-publish block[] only after the initial mark is durable. */
  lj_arena_block_set(a, cell);
  if (arena_lifetime_managed(a) && !root_construct) {
    int committed = lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_MUTATING, LJ_ARENA_LIFETIME_LIVE);
    lj_assertX(committed, "arena allocation lifetime commit lost");
    if (!committed)
      abort();
  }
  return 1;
}

static int arena_set_free_run(GCArena *a, uint32_t start, uint32_t len)
{
  uint32_t pos, end;
  /* This is the complete pre-mutation ownership proof. READY is intentionally
  ** scrubbed below; arena_clear_extent_range() then repeats the stricter
  ** ownership validation over every interior cell before boundary removal. */
  if (len == 0 ||
      !arena_range_ownership_preflight(a, start, len, 0, 0))
    return 0;
  pos = start;
  end = start + len;
  while (pos < end) {
    uint32_t wi = pos >> 6;
    uint32_t lo = pos & 63u;
    uint32_t n = end - pos;
    uint32_t take = n < 64u - lo ? n : 64u - lo;
    uint64_t mask = take == 64u ? ~(uint64_t)0 :
      (((uint64_t)1 << take) - 1u) << lo;
    (void)la_and64_rlx(&a->cdata[wi], ~mask);
    (void)la_and64_rlx(&a->ready[wi], ~mask);
    arena_dtor_clear_mask_rlx(a, wi, mask);  /* Defensive zero scrub. */
    pos += take;
  }
  lj_arena_block_clear(a, start);
  lj_arena_bm_set(a->mark, start);
  if (len > 1 && !arena_clear_extent_range(a, start + 1u, len - 1u))
    return 0;
  return 1;
}

#if defined(LJ_ARENA_TEST_HELPERS)
int lj_arena_test_set_free_run(GCArena *a, uint32_t start, uint32_t len)
{
  return arena_set_free_run(a, start, len);
}
#endif

/* These counters have the same single writer as their owner-local lists.
** Publication is one store, with no reader admission or retry protocol. The
** address-space bound in TGAlloc makes overflow impossible for valid lists. */
static LJ_AINLINE void arena_list_count_add(uint32_t *countp, uint32_t n)
{
  uint32_t count = la_load32_rlx(countp);
  if (LJ_UNLIKELY(n > ~(uint32_t)0 - count)) {
    lj_assertX(0, "allocator diagnostic arena count overflow");
    abort();
  }
  la_store32_rel(countp, count + n);
}

static LJ_AINLINE void arena_list_count_pop(uint32_t *countp, GCArena *next)
{
  uint32_t count;
  if (!next) {
    /* Also handles an existing list-repair branch which detached a bad tail. */
    la_store32_rel(countp, 0);
    return;
  }
  count = la_load32_rlx(countp);
  if (LJ_UNLIKELY(count < 2u)) {
    lj_assertX(0, "allocator diagnostic arena count underflow");
    abort();
  }
  la_store32_rel(countp, count - 1u);
}

static LJ_AINLINE void arena_binmask_publish(TGAlloc *alloc, uint32_t kind,
					    uint32_t mask)
{
  /* Bins remain owner-private. Only this scalar is observed remotely. */
  la_store32_rel(&alloc->binmask[kind], mask);
}

static int arena_link_run_head(TGAlloc *alloc, GCArena *a, uint32_t start,
			       uint32_t len)
{
  uint32_t k = arena_kind(a->hdr.flags);
  uint32_t b = arena_bin(len);
  LJArenaFreeRun *run = (LJArenaFreeRun *)lj_arena_cellptr(a, start);
  if (!arena_side_owners_none(a, start) ||
      !lj_arena_gc2_desc_clear_acq(a, start) ||
      (arena_lifetime_managed(a) &&
       lj_arena_lifetime_state_acq(a, start) != LJ_ARENA_LIFETIME_FREE))
    return 0;
  run->start = start;
  run->len = len;
  run->next = alloc->bins[k][b];
  /* FREE is the prior exact terminal LP. The node remains private until the
  ** final owner-local bin-head store below. */
  alloc->bins[k][b] = run;
  arena_binmask_publish(alloc, k, alloc->binmask[k] | ((uint32_t)1u << b));
  return 1;
}

static int arena_insert_run_head(TGAlloc *alloc, GCArena *a, uint32_t start,
                                 uint32_t len)
{
  if (!arena_set_free_run(a, start, len))
    return 0;
  return arena_link_run_head(alloc, a, start, len);
}

static LJ_AINLINE int arena_free_run_ptr_ok(const LJArenaFreeRun *run)
{
  uintptr_t addr = (uintptr_t)run;
  return checkptrGC(run) && addr >= (uintptr_t)LJ_ARENA_SIZE &&
	 (addr & (LJ_CELL_SIZE-1u)) == 0 &&
	 (addr & LJ_ARENA_MASK) >=
	 ((uintptr_t)LJ_AFIRST_CELL << LJ_CELL_SHIFT);
}

static LJ_AINLINE int arena_free_run_body_readable(const LJArenaFreeRun *run)
{
  GCArena *a = lj_arena_of(run);
  uint32_t cell = lj_arena_cellof(run);
  return !arena_lifetime_managed(a) ||
    (cell < LJ_ARENA_CELLS &&
     lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_FREE &&
     arena_side_owners_none(a, cell) &&
     lj_arena_gc2_desc_clear_acq(a, cell));
}

static LJ_AINLINE int arena_free_run_valid_knownptr(const LJArenaFreeRun *run,
						    uint32_t *lenp)
{
  GCArena *a;
  uint32_t start, len;
  a = lj_arena_of(run);
  if (!arena_free_run_body_readable(run))
    return 0;
  start = run->start;
  len = run->len;
  /*
  ** Free-run bin nodes live in the first cell of the free run they describe.
  ** Allocating from a bin can leave old payload bytes in cells that later sit
  ** behind a defensive bin pointer. Keep the exact-address duplicate scrub,
  ** but also drop any node whose bitmap state no longer says "free run start".
  */
  if (start < LJ_AFIRST_CELL || start >= LJ_ARENA_CELLS || len == 0 ||
      len > LJ_ARENA_CELLS - start ||
      lj_arena_cellptr(a, start) != (void *)run ||
      lj_arena_state(a, start) != 1 ||
      !arena_side_owners_none(a, start) ||
      (arena_lifetime_managed(a) &&
       lj_arena_lifetime_state_acq(a, start) != LJ_ARENA_LIFETIME_FREE))
    return 0;
  if (lenp) *lenp = len;
  return 1;
}

static int arena_insert_run(TGAlloc *alloc, GCArena *a, uint32_t start,
                            uint32_t len)
{
  uint32_t k = arena_kind(a->hdr.flags);
  uint32_t b = arena_bin(len);
  LJArenaFreeRun *run = (LJArenaFreeRun *)lj_arena_cellptr(a, start);
  LJArenaFreeRun **pp = &alloc->bins[k][b];
  uint32_t steps = 0;
  int scrub_head = 1;
  while (*pp) {
    LJArenaFreeRun *cur = *pp;
    LJArenaFreeRun *next;
    if (!arena_free_run_ptr_ok(cur) || !arena_free_run_body_readable(cur)) {
      *pp = NULL;
      break;
    }
    next = cur->next;
    if (cur == run) {
      *pp = next == cur ? NULL : next;
      continue;
    }
    if (next == cur || ++steps > LJ_ARENA_BIN_WALK_LIMIT) {
      *pp = NULL;
      break;
    }
    /*
    ** Scrub stale leading nodes before publishing a new run, but leave full
    ** per-node validation to arena_find_run(), which must validate before
    ** reuse anyway. This keeps insertion from turning long valid bins into a
    ** bitmap-walking hot path.
    */
    if (scrub_head && !arena_free_run_valid_knownptr(cur, NULL)) {
      *pp = next;
      continue;
    }
    scrub_head = 0;
    pp = &cur->next;
  }
  return arena_insert_run_head(alloc, a, start, len);
}

static LJ_AINLINE uint32_t arena_remote_meta(uint32_t start, uint32_t len)
{
  lj_assertX(start < (1u << 12) && len < (1u << 12),
	     "arena remote-free metadata overflow");
  return (start & 0xffu) | ((start & 0xf00u) << 8) | (len << 20);
}

static LJ_AINLINE uint32_t arena_remote_start(const LJArenaRemoteFree *node)
{
  uint32_t meta = la_load32_acq(&node->meta);
  return (meta & 0xffu) | ((meta >> 8) & 0xf00u);
}

static LJ_AINLINE uint32_t arena_remote_len(const LJArenaRemoteFree *node)
{
  return la_load32_acq(&node->meta) >> 20;
}

static LJ_AINLINE void arena_remote_set_meta(LJArenaRemoteFree *node,
					      uint32_t start, uint32_t len)
{
  node->reserved = 0;
  la_store32_rel(&node->meta, arena_remote_meta(start, len));
}

/* A terminal/grace-late free needs no intrusive size record. The allocation
** bitmap already identifies its exact start and extent, while this atomic bit
** pins that allocation until a later PREPSWEEP and grace consume it. */
static int arena_late_pin(GCArena *a, const void *p, size_t size)
{
  uint32_t start, ncells;
  if (!a || !p || size == 0)
    return -1;
  start = lj_arena_cellof(p);
  ncells = lj_arena_ncells(size);
  if (start < LJ_AFIRST_CELL || start >= LJ_ARENA_CELLS ||
      ncells > LJ_ARENA_CELLS - start ||
      lj_arena_cellptr(a, start) != p)
    return -1;
  /* Admission count precedes this atomic classification. An adoption cannot
  ** open/reuse the cell until leave; a terminal apply racing block 1->0 makes
  ** this at worst a duplicate bit which stable adoption removes. */
  if (!((la_load64_acq(&a->block[start >> 6]) >> (start & 63)) & 1u))
    return -1;
  /* Recovery pins the payload, but late[] is still required to remember a
  ** concurrent logical free after recovery eventually relinquishes it. */
  (void)la_bit_test_and_set64(&a->late[start >> 6], start & 63);
  arena_progress_wake(a);
  return 1;
}

/* Publish a plain late intent under its own admission regardless of whether
** the current generation is OPEN, tentative, committed or completed. This is
** the fallback for owner-local operations whose initial writer CAS lost; the
** bit is always visible before this admission can be the last leave/open. */
static int arena_plain_late_pin_admitted(GCArena *a, const void *p,
					  size_t size)
{
  uint64_t active;
  int published;
  if (!a || arena_lifetime_managed(a))
    return -1;
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t expect = active;
    uint64_t next;
    if (arena_terminal_closed_acq(a))
      return -1;
    if (arena_remote_count(active) == LJ_ARENA_REMOTE_COUNT_MASK)
      arena_remote_overflow();
    next = active + 1u;
    if (active & LJ_ARENA_REMOTE_STATE_MASK)
      next |= LJ_ARENA_REMOTE_PENDING;
    if (la_cas64(&a->hdr.remote_active, &expect, next,
		 LA_ACQ_REL, LA_ACQ)) {
      arena_test_plain_admit_pause_after_enter();
      break;
    }
    active = expect;
  }
  published = arena_late_pin(a, p, size);
  arena_remote_late_leave(a);
  return published;
}

/* Publish irrevocable external-free provenance with release ordering. Return
** 1 only to the first logical owner, 0 for a duplicate and -1 if stale. */
static int arena_late_claim_release(GCArena *a, const void *p, size_t size)
{
  uint32_t start, ncells;
  uint64_t bit, old;
  if (!a || !p || size == 0)
    return -1;
  start = lj_arena_cellof(p);
  ncells = lj_arena_ncells(size);
  if (start < LJ_AFIRST_CELL || ncells > LJ_ARENA_CELLS - start ||
      lj_arena_cellptr(a, start) != p ||
      !lj_arena_bm_get(a->block, start))
    return -1;
  bit = (uint64_t)1 << (start & 63);
  old = la_load64_acq(&a->late[start >> 6]);
  for (;;) {
    uint64_t expect = old;
    if (old & bit)
      return 0;
    if (la_cas64(&a->late[start >> 6], &expect, old | bit,
		 LA_REL, LA_ACQ)) {
      arena_progress_wake(a);
      return 1;
    }
    old = expect;
  }
}

static int arena_remote_late_publish(GCArena *a, void *p, size_t size)
{
  int entered;
  int published;
  if (!a)
    return -1;
  entered = arena_remote_late_enter(a);
  if (entered <= 0)
    return entered;  /* Zero means reopened/retry; negative means retain. */
  arena_test_plain_late_pause_after_enter();
  published = arena_lifetime_managed(a) ?
    arena_late_claim_release(a, p, size) : arena_late_pin(a, p, size);
  arena_remote_late_leave(a);
  return published;
}

int lj_arena_destruct_acquire(const void *p, size_t size)
{
  GCArena *a;
  uint32_t cell, ncells, life, oldstate, aflags;
  uint64_t active;
  if (!p || size == 0 || size > LJ_HUGE_THRESHOLD)
    return LJ_ARENA_DESTRUCT_LOST;
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  ncells = lj_arena_ncells(size);
  if (cell < LJ_AFIRST_CELL || ncells > LJ_ARENA_CELLS - cell ||
      lj_arena_cellptr(a, cell) != p ||
      !lj_arena_bm_get(a->block, cell))
    return LJ_ARENA_DESTRUCT_LOST;
  if (!arena_lifetime_managed(a)) {
    if (lj_arena_late_get(a, cell) || !arena_side_owners_none(a, cell))
      return LJ_ARENA_DESTRUCT_LOST;
    if (arena_plain_mutation_held(a) &&
	lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_FREEING)
      return LJ_ARENA_DESTRUCT_OWNED;
    {
      int gate = arena_plain_mutation_claim(a, 0);
      if (gate != 1) {
	if (gate < 0) {
	  (void)arena_late_pin(a, p, size);
	  arena_remote_late_leave(a);
	} else {
	  (void)arena_plain_late_pin_admitted(a, p, size);
	}
      return LJ_ARENA_DESTRUCT_LOST;
      }
    }
    aflags = lj_arena_flags_acq(a);
    if ((aflags & (LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|
		   LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) ||
	!lj_arena_bm_get(a->block, cell) || lj_arena_late_get(a, cell) ||
	!arena_side_owners_none(a, cell)) {
      (void)arena_late_pin(a, p, size);
      arena_plain_mutation_release(a);
      return LJ_ARENA_DESTRUCT_LOST;
    }
    oldstate = lj_arena_sweep_state_acq(a, cell);
    if (oldstate != LJ_ARENA_SWEEP_FREEING &&
	!lj_arena_sweep_state_cas(a, cell, oldstate,
				      LJ_ARENA_SWEEP_FREEING)) {
      (void)arena_late_pin(a, p, size);
      arena_plain_mutation_release(a);
      return LJ_ARENA_DESTRUCT_LOST;
    }
    /* Keep SEALED held across the semantic destructor. lj_arena_free consumes
    ** this exact FREEING token and reopens admissions only after bin commit. */
    return LJ_ARENA_DESTRUCT_ACQUIRED;
  }
  if (lj_arena_late_get(a, cell))
    return LJ_ARENA_DESTRUCT_LOST;  /* Intent alone is not terminal ownership. */
  life = lj_arena_lifetime_state_acq(a, cell);
  if (life == LJ_ARENA_LIFETIME_FREE)
    return lj_arena_sweep_state_acq(a, cell) ==
	LJ_ARENA_SWEEP_FREEING ? LJ_ARENA_DESTRUCT_OWNED :
	LJ_ARENA_DESTRUCT_LOST;
  if (life == LJ_ARENA_LIFETIME_DESTRUCT)
    return LJ_ARENA_DESTRUCT_LOST;  /* Tentative owner is not terminal. */
  if (life != LJ_ARENA_LIFETIME_LIVE ||
      !lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
					   LJ_ARENA_LIFETIME_DESTRUCT))
    return LJ_ARENA_DESTRUCT_LOST;
  arena_test_lifetime_pause_after_claim();
  active = lj_arena_remote_active_acq(a);
  if (((active & LJ_ARENA_REMOTE_STATE_MASK) ?
       !arena_mutation_closed_quiet(a) :
       !arena_mutation_open_quiet(a, 0)) ||
      !lj_arena_bm_get(a->block, cell) ||
      lj_arena_late_get(a, cell) ||
      lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE ||
      lj_arena_recovery_state_acq(a, cell) != LJ_ARENA_RECOVERY_IDLE) {
    (void)arena_destruct_restore_live(a, cell);
    return LJ_ARENA_DESTRUCT_LOST;
  }
  active = lj_arena_remote_active_acq(a);
  if (((active & LJ_ARENA_REMOTE_STATE_MASK) ?
       !arena_mutation_closed_quiet(a) :
       !arena_mutation_open_quiet(a, 0)) ||
      !arena_side_owners_none(a, cell) ||
      lj_arena_late_get(a, cell) ||
      !arena_destruct_commit_free(a, cell)) {
    (void)arena_destruct_restore_live(a, cell);  /* RESCUE may own it. */
    return LJ_ARENA_DESTRUCT_LOST;
  }
  /* FREE is the irreversible body-ownership LP. Publish terminal discovery
  ** afterward; a semantic reader already rejects FREE. This caller is the
  ** sole sweep-state classifier for its exact object. */
  oldstate = lj_arena_sweep_state_acq(a, cell);
  if (oldstate != LJ_ARENA_SWEEP_FREEING &&
      !lj_arena_sweep_state_cas(a, cell, oldstate,
					LJ_ARENA_SWEEP_FREEING))
    abort();
  if (oldstate == LJ_ARENA_SWEEP_RETIRED) {
    uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
    lj_assertX(old != 0, "arena destructor deferred underflow");
    UNUSED(old);
  }
  return LJ_ARENA_DESTRUCT_ACQUIRED;
}

int lj_arena_quarantine_owns_body(const void *p, size_t size)
{
  GCArena *a;
  uint32_t cell, ncells, flags, life;
  if (!p || size == 0 || size > LJ_HUGE_THRESHOLD)
    return 0;
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  ncells = lj_arena_ncells(size);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
      ncells > LJ_ARENA_CELLS - cell ||
      lj_arena_cellptr(a, cell) != p)
    return 0;
  if (arena_lifetime_managed(a) &&
      lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_FREE)
    return 1;  /* Terminal ownership precedes all post-destructor handoff. */
  if (!arena_side_owners_none(a, cell)) {
    (void)(arena_lifetime_managed(a) ?
	  arena_late_claim_release(a, p, size) :
	  arena_plain_late_pin_admitted(a, p, size));
    return 1;  /* Irrevocable external intent; pinned owner keeps bytes. */
  }
  flags = lj_arena_flags_acq(a);
  if (!arena_lifetime_managed(a)) {
    return (flags & (LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|
		     LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) != 0;
  }
  life = lj_arena_lifetime_state_acq(a, cell);
  if (life != LJ_ARENA_LIFETIME_LIVE) {
    (void)arena_late_claim_release(a, p, size);
    return 1;  /* Never steal CONSTRUCT/RECOVERY/DESTRUCT/RESCUE/MUTATING. */
  }
  if (!(flags & (LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|
		 LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)))
    return 0;
  /* A post-boundary caller has irrevocably relinquished ownership. Keep the
  ** readable body pinned; PREPSWEEP performs the terminal transition after
  ** recovery/root owners have left. Intent alone is never destructor OWNED. */
  (void)arena_late_claim_release(a, p, size);
  la_store64_rel(&a->hdr.retire_epoch, ~(uint64_t)0);
  return 1;
}

int lj_arena_remote_free_publish(TGAlloc *alloc, void *p, size_t size)
{
  GCArena *a;
  LJArenaRemoteFree *node, *head;
  uint32_t start, ncells, oldstate, flags;
  int claim, plain_held = 0;
  if (!alloc || !p || size == 0 || size > LJ_HUGE_THRESHOLD)
    return 0;
retry_open:
  a = lj_arena_of(p);
  if (lj_arena_alloc_free_noinsert_acq(alloc))
    return 1;
  if (!arena_remote_enter(a)) {
    int late = arena_remote_late_publish(a, p, size);
    if (late == 0)
      goto retry_open;
    return 1;
  }
  if (lj_arena_quarantine_owns_body(p, size)) {
    arena_remote_leave(a);
    return 1;
  }
  start = lj_arena_cellof(p);
  ncells = lj_arena_ncells(size);
  if (start < LJ_AFIRST_CELL || start + ncells > LJ_ARENA_CELLS ||
      !lj_arena_bm_get(a->block, start)) {
    arena_remote_leave(a);
    return 0;
  }
  if (!arena_side_owners_none(a, start)) {
    (void)(arena_lifetime_managed(a) ?
	  arena_late_claim_release(a, p, size) :
	  arena_plain_late_pin_admitted(a, p, size));
    arena_remote_leave(a);
    return 1;  /* Retain; never overwrite the pending object's header. */
  }
  if (arena_lifetime_managed(a)) {
    uint32_t life = lj_arena_lifetime_state_acq(a, start);
    if (life == LJ_ARENA_LIFETIME_FREE ||
	arena_late_claim_release(a, p, size) != 1) {
      arena_remote_leave(a);
      return 1;
    }
  }
  claim = arena_destruct_claim_live(a, start, 1);
  if (claim == 0) {
    arena_remote_leave(a);
    return 1;
  }
  if (claim == 2) {
    int gate = arena_plain_mutation_claim(a, 1);
    if (gate != 1) {
      (void)arena_late_pin(a, p, size);
      if (gate == 0)
	arena_remote_leave(a);  /* Initial CAS lost; our admission remains. */
      else
	arena_remote_late_leave(a);  /* Commit loss replaced the admission. */
      return 1;
    }
    plain_held = 1;  /* The exact 1->SEALED CAS consumed our admission. */
  }
  flags = lj_arena_flags_acq(a);
  if (flags & (LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|
	       LJ_AF_RECLAIMED)) {
    if (claim == 1)
      (void)arena_destruct_restore_live(a, start);
    else
      (void)arena_late_pin(a, p, size);
    if (plain_held)
      arena_plain_mutation_release(a);
    else
      arena_remote_leave(a);
    return 1;
  }
  oldstate = lj_arena_sweep_state_acq(a, start);
  if (claim == 1 &&
      (!arena_mutation_open_quiet(a, 1) ||
       !arena_side_owners_none(a, start) ||
       !arena_destruct_commit_free(a, start))) {
    (void)arena_destruct_restore_live(a, start);
    arena_remote_leave(a);
    return 1;
  }
  if (claim == 2 &&
      (!lj_arena_bm_get(a->block, start) ||
       !arena_side_owners_none(a, start))) {
    (void)arena_late_pin(a, p, size);
    arena_plain_mutation_release(a);
    return 1;
  }
  if (oldstate != LJ_ARENA_SWEEP_FREEING &&
      !lj_arena_sweep_state_cas(a, start, oldstate,
					LJ_ARENA_SWEEP_FREEING)) {
    if (claim == 1)
      abort();
    (void)arena_late_pin(a, p, size);
    arena_plain_mutation_release(a);
    return 1;
  }
  node = (LJArenaRemoteFree *)p;
  arena_remote_set_meta(node, start, ncells);
  head = (LJArenaRemoteFree *)la_loadptr_acq(
    (void *const *)&a->hdr.remote_free);
  do {
    la_storeptr_rlx((void **)&node->next, head);
  } while (!la_casptr((void **)&a->hdr.remote_free, (void **)&head, node,
		       LA_REL, LA_ACQ));
  /* FREE already excludes readers; queue publication transfers structural
  ** ownership to the drain, which clears late only after block/bin commit. */
  arena_test_remote_publish_pause_after_queue();
  if (plain_held)
    arena_plain_mutation_release(a);
  else
    arena_remote_leave(a);
  /* Publish the allocator-wide wake only after both the queue CAS and this
  ** producer's arena admission have been released. A drain which consumes the
  ** wake can therefore either take this record, or leave a later producer's
  ** independently published wake set. Publishing before remote_leave() would
  ** permit a consumer to clear the wake, reject the still-active arena and
  ** permanently strand the record. */
  lj_arena_remote_pending_rel(alloc, 1);
  return 1;
}

static uint32_t arena_remote_free_drain_one(TGAlloc *alloc, GCArena *a)
{
  LJArenaRemoteFree *node;
  uint32_t n = 0;
  uint32_t flags;
  int plain_held = 0;
  if (!alloc || !a)
    return 0;
  /*
  ** Do not close a plain arena generation merely to discover that its
  ** intrusive remote-free queue is empty. A publisher racing this acquire
  ** load either becomes visible to a later drain or is already covered by
  ** remote_active and defeats the ownership claim below. Remote frees are
  ** opportunistic allocator input; deferring a post-load publication does
  ** not make its storage reusable or drop its lifetime intent.
  */
  if (la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL)
    return 0;
  if (arena_remote_count(lj_arena_remote_active_acq(a)) != 0 ||
      ((flags = lj_arena_flags_acq(a)) &
       (LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|LJ_AF_PREPSWEEP))) {
    /* The global wake was already consumed, but an unrelated rescue/late
    ** publisher or sweep transition prevented this queue from being taken.
    ** Keep it scheduled: such an actor need not publish another queue wake
    ** when it leaves. */
    lj_arena_remote_pending_rel(alloc, 1);
    return 0;
  }
  if (!arena_lifetime_managed(a)) {
    int gate = arena_plain_mutation_claim(a, 0);
    if (gate != 1) {
      if (gate < 0)
	arena_remote_late_leave(a);
      lj_arena_remote_pending_rel(alloc, 1);
      return 0;
    }
    plain_held = 1;
    flags = lj_arena_flags_acq(a);
    if (flags & (LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|LJ_AF_PREPSWEEP)) {
      arena_plain_mutation_release(a);
      lj_arena_remote_pending_rel(alloc, 1);
      return 0;
    }
  }
  node = (LJArenaRemoteFree *)la_xchgptr_acqrel(
    (void **)&a->hdr.remote_free, NULL);
  while (node) {
    LJArenaRemoteFree *next = (LJArenaRemoteFree *)la_loadptr_acq(
      (void *const *)&node->next);
    uint32_t start = arena_remote_start(node);
    uint32_t len = arena_remote_len(node);
    int valid = lj_arena_of(node) == a && start >= LJ_AFIRST_CELL &&
	start < LJ_ARENA_CELLS && len != 0 &&
	len <= LJ_ARENA_CELLS - start &&
	lj_arena_cellptr(a, start) == (void *)node &&
	(!arena_lifetime_managed(a) ||
	 lj_arena_lifetime_state_acq(a, start) ==
	   LJ_ARENA_LIFETIME_FREE);
    if (valid && lj_arena_bm_get(a->block, start) &&
	arena_side_owners_none(a, start)) {
      int inserted;
      if (flags & LJ_AF_RECLAIMED)
	inserted = arena_set_free_run(a, start, len);
      else
	inserted = arena_insert_run(alloc, a, start, len);
      if (inserted) {
	(void)lj_arena_sweep_state_cas(a, start,
	  LJ_ARENA_SWEEP_FREEING, LJ_ARENA_SWEEP_WHITE);
	/* Block/bin discovery is published; consume every duplicate intent for
	** this old incarnation before its cell becomes reusable. */
	(void)la_and64_rlx(&a->late[start >> 6],
	  ~((uint64_t)1 << (start & 63)));
	n++;
      }
    } else if (valid && lj_arena_bm_get(a->block, start)) {
      (void)arena_late_pin(a, node, (size_t)len << LJ_CELL_SHIFT);
    }
    node = next;
  }
  if (plain_held)
    arena_plain_mutation_release(a);
  return n;
}

uint32_t lj_arena_remote_free_drain_sweep(TGAlloc *alloc, GCArena *a)
{
  LJArenaRemoteFree *node;
  uint32_t n = 0;
  if (!alloc || !a ||
      !(lj_arena_flags_acq(a) &
	(LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE)))
    return 0;
  node = (LJArenaRemoteFree *)la_xchgptr_acqrel(
    (void **)&a->hdr.remote_free, NULL);
  while (node) {
    LJArenaRemoteFree *next = (LJArenaRemoteFree *)la_loadptr_acq(
      (void *const *)&node->next);
    uint32_t start = arena_remote_start(node);
    uint32_t len = arena_remote_len(node);
    if (lj_arena_of(node) == a && start >= LJ_AFIRST_CELL &&
	start < LJ_ARENA_CELLS && len != 0 &&
	len <= LJ_ARENA_CELLS - start &&
	lj_arena_cellptr(a, start) == (void *)node &&
	lj_arena_bm_get(a->block, start) && arena_side_owners_none(a, start) &&
	(!arena_lifetime_managed(a) ||
	 lj_arena_lifetime_state_acq(a, start) ==
	   LJ_ARENA_LIFETIME_FREE)) {
      uint32_t state = lj_arena_sweep_state_acq(a, start);
      while (state != LJ_ARENA_SWEEP_FREEING) {
	if (lj_arena_sweep_state_cas(a, start, state,
					   LJ_ARENA_SWEEP_FREEING)) {
	  if (state == LJ_ARENA_SWEEP_RETIRED) {
	    uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	    lj_assertX(old != 0, "arena remote-free deferred underflow");
	    UNUSED(old);
	  }
	  break;
	}
	state = lj_arena_sweep_state_acq(a, start);
      }
      n++;
    } else if (lj_arena_of(node) == a && start >= LJ_AFIRST_CELL &&
	start < LJ_ARENA_CELLS && len != 0 &&
	len <= LJ_ARENA_CELLS - start &&
	lj_arena_cellptr(a, start) == (void *)node &&
	lj_arena_bm_get(a->block, start)) {
      (void)arena_late_pin(a, node, (size_t)len << LJ_CELL_SHIFT);
    }
    node = next;
  }
  return n;
}

static uint32_t arena_remote_free_drain_all(TGAlloc *alloc, int force)
{
  uint32_t k, n = 0;
  if (!alloc)
    return 0;
  /* Queue producers release their arena admission before release-publishing
  ** this advisory bit. The acquire half of this exchange therefore covers
  ** both publications. Clearing first is intentional: a producer after the
  ** clear leaves 1 for the next opportunistic drain, whether or not this scan
  ** happens to consume its queue record. A producer paused between its queue
  ** CAS and flag store likewise guarantees a subsequent drain. */
  if (lj_arena_remote_pending_xchg_acqrel(alloc, 0) == 0 && !force) {
    arena_test_remote_fast_skip();
    return 0;
  }
  arena_test_remote_drain_pause_after_clear();
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    GCArena *a;
    for (a = alloc->owned[k]; a != NULL; a = lj_arena_next_acq(a)) {
      arena_test_remote_arena_probe();
      n += arena_remote_free_drain_one(alloc, a);
    }
  }
  return n;
}

uint32_t lj_arena_remote_free_drain_force(TGAlloc *alloc)
{
  return arena_remote_free_drain_all(alloc, 1);
}

uint32_t lj_arena_remote_free_drain(TGAlloc *alloc)
{
  return arena_remote_free_drain_all(alloc, 0);
}

/* Retain bitmap-only discovery points when a free span cannot be written or
** linked into an owner-local bin. block=0,mark=1 is the canonical free-run
** boundary. Besides the span start, retain every descriptor boundary which
** can split a rebuild while it is still active. If a rebuild consumes the
** free prefix first, that descriptor's mark emerges as the next free boundary
** when its ownership clears. Atomic mark ORs are safe under a closed plain
** generation and never touch the protected body bytes. New descriptors cannot
** legitimately claim this detached block=0 span after candidate validation. */
static void arena_retain_free_boundaries(GCArena *a, uint32_t start,
					 uint32_t len)
{
  uint32_t pos, end;
  if (!a || len == 0 || start < LJ_AFIRST_CELL ||
      start >= LJ_ARENA_CELLS || len > LJ_ARENA_CELLS - start)
    return;
  lj_assertX(!lj_arena_bm_get(a->block, start),
	     "arena retained free boundary overlaps allocation start");
  if (lj_arena_bm_get(a->block, start))
    return;
  end = start + len;
  for (pos = start; pos < end; ) {
    uint32_t wi = pos >> 6;
    uint32_t lo = pos & 63u;
    uint32_t take = end - pos;
    uint32_t room = 64u - lo;
    uint64_t mask, boundaries;
    if (take > room)
      take = room;
    mask = arena_range_mask(lo, take);
    boundaries = la_load64_acq(&a->block[wi]) |
	la_load64_acq(&a->ready[wi]) |
	arena_recovery_block_bits(a, wi) |
	arena_root_block_bits(a, wi) |
	arena_dtor_block_bits(a, wi) |
	arena_lifetime_block_bits(a, wi) |
	arena_gc2_token_block_bits(a, wi);
    boundaries &= mask;
    if (pos == start)
      boundaries |= (uint64_t)1 << lo;
    if (boundaries)
      (void)la_or64_rlx(&a->mark[wi], boundaries);
    pos += take;
  }
}

static void arena_publish_bump_run(TGAlloc *alloc, uint32_t k)
{
  LJArenaBump *b;
  int published = 0;
  if (!alloc || k >= LJ_ARENA_NKINDS)
    return;
  b = &alloc->bump[k];
  if (!b->a || b->cell >= b->end)
    return;
  /*
  ** The active bump window is absent from the reusable free-run bins. Publish
  ** its unused tail before the window is replaced, otherwise lazy sweeping can
  ** strand one large free run per swept arena and force fresh arena mapping.
  ** A plain writer generation temporarily makes the tail body unwritable.
  ** Do not write a bin node through that gate. Instead retain only the atomic
  ** free-boundary sentinel, so a later rebuild can recover the all-zero tail
  ** without waiting for the writer or exposing its veto as allocation failure.
  */
  if (arena_lifetime_managed(b->a) ||
      !(lj_arena_remote_active_acq(b->a) &
	(LJ_ARENA_REMOTE_SEALED|LJ_ARENA_REMOTE_PENDING)))
    published = arena_insert_run_head(alloc, b->a, b->cell,
				      b->end - b->cell);
  if (!published)
    arena_retain_free_boundaries(b->a, b->cell, b->end - b->cell);
  b->a = NULL;
  b->cell = 0;
  b->end = 0;
}

static void arena_refresh_binmask(TGAlloc *alloc, uint32_t k, uint32_t b)
{
  if (alloc->bins[k][b])
    arena_binmask_publish(alloc, k, alloc->binmask[k] | ((uint32_t)1u << b));
  else
    arena_binmask_publish(alloc, k, alloc->binmask[k] & ~((uint32_t)1u << b));
}

static LJArenaFreeRun **arena_find_run(TGAlloc *alloc, uint32_t k,
				       uint32_t ncells, uint32_t *binp)
{
  uint32_t mask = alloc->binmask[k] & lj_arena_binmask_from_ncells(ncells);
  while (mask) {
    uint32_t b = lj_ffs(mask);
    LJArenaFreeRun **pp = &alloc->bins[k][b];
    uint32_t steps = 0;
    mask &= mask - 1u;
    while (*pp) {
      LJArenaFreeRun *run = *pp;
      LJArenaFreeRun *next;
      uint32_t len;
      if (++steps > LJ_ARENA_BIN_WALK_LIMIT) {
	alloc->bins[k][b] = NULL;
	arena_refresh_binmask(alloc, k, b);
	break;
      }
      /* A completed plain writer may have linked this run while a counted
      ** late publisher was preempted. SEALED/PENDING keeps that generation
      ** non-reusable until it is opened; preserve the bin head for a later
      ** allocation instead of scrubbing it as malformed. */
      if (!arena_free_run_ptr_ok(run)) {
	*pp = NULL;
	arena_refresh_binmask(alloc, k, b);
	break;
      }
      if (!arena_lifetime_managed(lj_arena_of(run)) &&
	  (lj_arena_remote_active_acq(lj_arena_of(run)) &
	   (LJ_ARENA_REMOTE_SEALED|LJ_ARENA_REMOTE_PENDING)))
	break;
      if (!arena_free_run_body_readable(run)) {
	*pp = NULL;
	arena_refresh_binmask(alloc, k, b);
	break;
      }
      next = run->next;
      if (next == run) {
	*pp = NULL;
	arena_refresh_binmask(alloc, k, b);
	break;
      }
      if (!arena_free_run_valid_knownptr(run, &len)) {
	*pp = next;
	arena_refresh_binmask(alloc, k, b);
	continue;
      }
      if (len >= ncells) {
	*binp = b;
	return pp;
      }
      pp = &run->next;
    }
    arena_refresh_binmask(alloc, k, b);
  }
  return NULL;
}

static void arena_clear_bins(TGAlloc *alloc, uint32_t k)
{
  memset(alloc->bins[k], 0, sizeof(alloc->bins[k]));
  arena_binmask_publish(alloc, k, 0);
}

static GCArena *arena_reclaimed_acq(const TGAlloc *alloc, uint32_t k)
{
  return (GCArena *)la_loadptr_acq((void *const *)&alloc->reclaimed[k]);
}

static int arena_reclaimed_cas(TGAlloc *alloc, uint32_t k,
				GCArena **oldp, GCArena *a)
{
  return la_casptr((void **)&alloc->reclaimed[k], (void **)oldp, a,
		   LA_ACQ_REL, LA_ACQ);
}

static void arena_sweep_state_reset(GCArena *a)
{
  uint32_t w;
  for (w = 0; w < LJ_ARENA_SWEEP_WORDS; w++)
    la_store64_rel(&a->sweep[w], 0);
}

static void arena_sweep_state_prepare(GCArena *a)
{
  uint32_t w;
  /* WHITE means "not detached/classified" during NEEDSWEEP. The mark bitmap
  ** still distinguishes live raw/fixed allocations. LIVE is reserved for an
  ** exact old GC header detached from the ownership spine (or rescued after
  ** retirement), so the post-grace pass can reanchor it exactly once. Keep an
  ** allocated FREEING start terminal throughout PREP: an intrusive remote-free
  ** record may already own and overwrite that body, and no committed reader may
  ** observe a transient WHITE gap before the stable queue drain. */
  for (w = 0; w < LJ_ARENA_SWEEP_WORDS; w++) {
    uint64_t old = la_load64_acq(&a->sweep[w]);
    uint64_t block = la_load64_acq(&a->block[w >> 1]);
    uint32_t starts = (uint32_t)(w & 1u ? block >> 32 : block);
    uint64_t next = 0;
    uint32_t j;
    for (j = 0; j < 32u; j++) {
      uint32_t cell = (w << 5) + j;
      if (cell >= LJ_AFIRST_CELL && (starts & ((uint32_t)1u << j)) &&
	  ((old >> (j << 1)) & 3u) == LJ_ARENA_SWEEP_FREEING)
	next |= (uint64_t)LJ_ARENA_SWEEP_FREEING << (j << 1);
    }
    la_store64_rel(&a->sweep[w], next);
  }
  a->hdr.reclaim_cell = LJ_AFIRST_CELL;
  a->hdr.reclaim_deferred = 0;
}

static void arena_prepare_bump_tail(GCArena *a)
{
  uint32_t cell, end, firstw, lastw, w;
  if (!a)
    return;
  cell = la_load32_acq(&a->hdr.prep_bump_cell);
  end = la_load32_acq(&a->hdr.prep_bump_end);
  if (cell >= LJ_AFIRST_CELL && cell < end && end <= LJ_ARENA_CELLS) {
    if (!arena_range_ownership_preflight(a, cell, end - cell, 0, 0)) {
      arena_retain_free_boundaries(a, cell, end - cell);
      goto done;
    }
    /* Committed PREP readers may mark another live cell in the same word.
    ** Structural publication must therefore update only the tail mask with
    ** atomic RMWs. Do this once per bitmap word rather than four locked RMWs
    ** per cell. The mark CAS keeps the leading free-run boundary continuously
    ** set and preserves unrelated concurrent marks outside the tail. */
    firstw = cell >> 6;
    lastw = (end - 1u) >> 6;
    for (w = firstw; w <= lastw; w++) {
      uint32_t lo = w == firstw ? (cell & 63u) : 0u;
      uint32_t hi = w == lastw ? ((end - 1u) & 63u) + 1u : 64u;
      uint64_t lomask = ~(uint64_t)0 << lo;
      uint64_t himask = hi == 64u ? ~(uint64_t)0 :
	(((uint64_t)1 << hi) - 1u);
      uint64_t mask = lomask & himask;
      uint64_t old, next;
      if (LJ_UNLIKELY(arena_dtor_block_bits(a, w) & mask)) {
	/* A bump tail has never contained a constructed object. Retain the
	** complete arena on impossible typed identity rather than erasing the
	** only authoritative destructor selector. */
	lj_assertX(0, "arena bump tail crossed typed allocation identity");
	abort();
      }
      (void)la_and64_rlx(&a->cdata[w], ~mask);
      (void)la_and64_rlx(&a->ready[w], ~mask);
      arena_dtor_clear_mask_rlx(a, w, mask);  /* Defensive zero scrub. */
      (void)la_and64_rlx(&a->block[w], ~mask);
      old = la_load64_rlx(&a->mark[w]);
      do {
	next = old & ~mask;
	if (w == firstw)
	  next |= (uint64_t)1 << (cell & 63u);
      } while (!la_cas64(&a->mark[w], &old, next, LA_RLX, LA_RLX));
    }
  }
done:
  la_store32_rel(&a->hdr.prep_bump_cell, 0);
  la_store32_rel(&a->hdr.prep_bump_end, 0);
}

/* Consume only pins which were visible before this PREPSWEEP generation.
** SEALED admission makes a concurrent publisher set PENDING first. A bit
** published after this exchange simply remains pinned for one extra cycle. */
static uint32_t arena_late_prepare_consume(GCArena *a)
{
  uint32_t w, n = 0;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t bits = la_load64_acq(&a->late[w]);
    while (bits) {
      uint32_t j = lj_ffs64(bits);
      uint32_t cell = (w << 6) + j;
      uint32_t state;
      bits &= bits - 1u;
      if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
	  !lj_arena_bm_get(a->block, cell)) {
	(void)la_and64_rlx(&a->late[w], ~((uint64_t)1 << j));
	continue;
      }
      if (!arena_side_owners_none(a, cell))
	continue;  /* Keep the pin until recovery/root ownership relinquishes. */
      if (arena_lifetime_managed(a)) {
	if (!lj_arena_lifetime_state_cas(a, cell,
	      LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT))
	  continue;
	if (!arena_mutation_closed_quiet(a) ||
	    !lj_arena_bm_get(a->block, cell) ||
	    lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE ||
	    lj_arena_recovery_state_acq(a, cell) !=
	      LJ_ARENA_RECOVERY_IDLE) {
	  (void)arena_destruct_restore_live(a, cell);
	  continue;
	}
	if (!arena_destruct_commit_free(a, cell))
	  continue;  /* RESCUE won; it owns restoration and semantic work. */
      }
      state = lj_arena_sweep_state_acq(a, cell);
      while (state != LJ_ARENA_SWEEP_FREEING) {
	if (lj_arena_sweep_state_cas(a, cell, state,
				       LJ_ARENA_SWEEP_FREEING)) {
	  if (state == LJ_ARENA_SWEEP_RETIRED) {
	    uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	    lj_assertX(old != 0, "arena late-prepare deferred underflow");
	    UNUSED(old);
	  }
	  n++;
	  break;
	}
	state = lj_arena_sweep_state_acq(a, cell);
      }
      /* Retain late until quarantine publishes block removal. FREE already
      ** rejects readers and exact old-body discovery consumes the intent. */
    }
  }
  return n;
}

/* A current-generation late pin is a durable logical-free intent. It may name
** an already-completed physical destructor or a free which lost MUTATING and
** therefore touched no bytes. Keep the allocation body opaque in either case,
** never reconstruct a header, and settle detached accounting before commit. */
static uint32_t arena_quarantine_settle_late(GCArena *a)
{
  uint32_t w, n = 0;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t bits = la_load64_acq(&a->late[w]) &
		    la_load64_acq(&a->block[w]);
    while (bits) {
      uint32_t j = lj_ffs64(bits);
      uint32_t cell = (w << 6) + j;
      uint32_t state = lj_arena_sweep_state_acq(a, cell);
      int held = 0;
      bits &= bits - 1u;
      if (!arena_side_owners_none(a, cell))
	continue;
      if (arena_lifetime_managed(a)) {
	if (!lj_arena_lifetime_state_cas(a, cell,
	      LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_MUTATING))
	  continue;
	if (!arena_mutation_closed_quiet(a) ||
	    lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE ||
	    lj_arena_recovery_state_acq(a, cell) !=
	      LJ_ARENA_RECOVERY_IDLE) {
	  arena_mutation_restore_live(a, cell);
	  continue;
	}
	held = 1;
      }
      while (state != LJ_ARENA_SWEEP_WHITE) {
	if (lj_arena_sweep_state_cas(a, cell, state,
					 LJ_ARENA_SWEEP_WHITE)) {
	  if (state == LJ_ARENA_SWEEP_RETIRED) {
	    uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	    lj_assertX(old != 0, "arena late-pin deferred underflow");
	    UNUSED(old);
	  }
	  n++;
	  break;
	}
	state = lj_arena_sweep_state_acq(a, cell);
      }
      if (held)
	arena_mutation_restore_live(a, cell);
    }
  }
  return n;
}

void lj_arena_alloc_set_registry(TGAlloc *alloc, HugeTab *tab)
{
  if (alloc)
    la_storeptr_rel((void **)&alloc->smalltab, tab);
}

HugeTab *lj_arena_alloc_registry_acq(const TGAlloc *alloc)
{
  return alloc ? (HugeTab *)la_loadptr_acq((void *const *)&alloc->smalltab) :
		 NULL;
}

int lj_arena_alloc_registry_lookup(const TGAlloc *alloc, const GCArena *a,
				   LJHugeInfo *hi)
{
  HugeTab *tab = lj_arena_alloc_registry_acq(alloc);
  return tab && a ? lj_arena_hugetab_lookup(tab, a, hi) : 0;
}

int lj_arena_hugetab_rescue_enter(HugeTab *registry, GCArena *a,
				    LJHugeInfo *hi)
{
  LJHugeReader reader = { NULL, NULL, 0 };
  int admission;
  if (!registry || !a ||
      lj_arena_hugetab_reader_acquire(registry, a, &reader, hi) !=
	LJ_ARENA_HUGE_READER_ACQUIRED)
    return LJ_ARENA_RESCUE_RETRY;
  arena_test_registry_pause_after_reader();
  admission = lj_arena_rescue_enter(a);
  (void)lj_arena_hugetab_reader_release(&reader, NULL);
  return admission;
}

static int arena_rescue_enter_bounded(GCArena *a)
{
  uint64_t active, expect, next;
  int committed;
  if (!a || arena_terminal_closed_acq(a))
    return LJ_ARENA_RESCUE_RETRY;
  active = lj_arena_remote_active_acq(a);
  if ((!arena_lifetime_managed(a) &&
       (active & LJ_ARENA_REMOTE_STATE_MASK)) ||
      arena_remote_count(active) == LJ_ARENA_REMOTE_COUNT_MASK)
    return LJ_ARENA_RESCUE_RETRY;
  committed = (active & LJ_ARENA_REMOTE_SEALED) &&
    !(active & LJ_ARENA_REMOTE_CLOSED);
  next = active + 1u;
  if (!committed &&
      (active & (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED)))
    next |= LJ_ARENA_REMOTE_PENDING;
  expect = active;
  if (!la_cas64(&a->hdr.remote_active, &expect, next,
		LA_ACQ_REL, LA_ACQ))
    return LJ_ARENA_RESCUE_RETRY;
  return committed ? LJ_ARENA_RESCUE_COMMITTED :
    ((active & LJ_ARENA_REMOTE_SEALED) ?
     LJ_ARENA_RESCUE_BIT_ONLY : LJ_ARENA_RESCUE_FULL);
}

int lj_arena_hugetab_rescue_slot_enter_bounded(
  HugeTab *registry, uint32_t slot, GCArena *a, LJHugeInfo *hi)
{
  LJHugeTabHdr *h = registry ? registry->h : NULL;
  LJHugeEnt *e;
  LJHugeReader reader = { NULL, NULL, 0 };
  uint64_t addr, meta, next;
  la_u128 exp, des;
  int admission;
  if (hi) {
    hi->size = 0;
    hi->flags = 0;
    hi->readers = 0;
  }
  if (!h || slot > h->mask || !a)
    return LJ_ARENA_RESCUE_RETRY;
  e = &h->ent[slot];
  addr = la_load64_acq(&e->slot.lo);
  if (addr != (uint64_t)(uintptr_t)a)
    return LJ_ARENA_RESCUE_RETRY;
  meta = la_load64_acq(&e->slot.hi);
  if (la_load64_acq(&e->slot.lo) != addr ||
      (meta & (LJ_HUGEF_FREEING|LJ_HUGEF_DEFER_FREE|LJ_HUGEF_BUSY)) ||
      hugetab_admission_closed(meta) ||
      hugetab_readers(meta) == LJ_HUGETAB_READER_MAX)
    return LJ_ARENA_RESCUE_RETRY;
  exp.lo = addr;
  exp.hi = meta;
  next = meta + LJ_HUGETAB_READER_ONE;
  des.lo = addr;
  des.hi = next;
  if (!la_cas128(&e->slot, &exp, des))
    return LJ_ARENA_RESCUE_RETRY;
  reader.h = h;
  reader.base = a;
  reader.size = (uint32_t)hugetab_size(next);
  hugetab_decode(next, hi);
  admission = arena_rescue_enter_bounded(a);
  (void)lj_arena_hugetab_reader_release(&reader, NULL);
  return admission;
}

static int arena_registry_insert_fresh(TGAlloc *alloc, GCArena *a,
				       uint32_t flags)
{
  HugeTab *tab = lj_arena_alloc_registry_acq(alloc);
  if (!tab)
    return 1;
  if (lj_arena_hugetab_insert(tab, a, LJ_ARENA_SIZE,
			      arena_registry_hflags(flags)) != 1)
    return 0;
  arena_registered_set(a);
  return 1;
}

static int arena_registry_insert_existing(TGAlloc *alloc, GCArena *a,
					  uint32_t flags)
{
  int ok;
  HugeTab *tab = lj_arena_alloc_registry_acq(alloc);
  if (!tab)
    return 1;
  ok = lj_arena_hugetab_insert(tab, a, LJ_ARENA_SIZE,
			       arena_registry_hflags(flags));
  if (ok >= 0)
    arena_registered_set(a);
  return ok >= 0;
}

static int arena_registry_delete(TGAlloc *alloc, GCArena *a)
{
  HugeTab *tab = lj_arena_alloc_registry_acq(alloc);
  if (!a)
    return 0;
  /* Private/bootstrap/test mappings may lawfully predate registry attachment.
  ** Conversely, a published REGISTERED bit requires the exact directory: a
  ** missing wrapper is never permission to erase a stale locator. */
  if (!(lj_arena_flags_acq(a) & LJ_AF_REGISTERED))
    return 1;
  if (!tab || lj_arena_hugetab_delete(tab, a, NULL) != 1)
    return 0;
  arena_registered_clear(a);  /* Only after the exact slot is unreachable. */
  return 1;
}

static GCArena *arena_unmap_list(TGAlloc *alloc, GCArena *a,
				uint32_t *retained_countp)
{
  GCArena *retained = NULL;
  uint32_t retained_count = 0;
  while (a) {
    GCArena *next = lj_arena_next_acq(a);
    uint64_t restore;
    int claimed = arena_unmap_claim(a, &restore);
    int unmapped = 0;
    if (claimed) {
      /* Allocator fini is the quiescent terminal owner. Live/constructor
      ** lifetime lanes no longer have a semantic consumer and may be discarded
      ** with the mapping; recovery/root work and exact table-rescan tokens
      ** remain external owners and must still drain explicitly. */
      if (lj_arena_recovery_empty(a) && arena_root_empty(a) &&
	  lj_arena_gc2_tokens_empty_acq(a) &&
	  lj_arena_gc2_desc_mapping_clear_acq(a) &&
	  arena_registry_delete(alloc, a)) {
	arena_unmap_claimed(a);
	unmapped = 1;
      } else {
	arena_unmap_abandon(a, restore);
      }
    }
    if (!unmapped) {
      lj_arena_next_rel(a, retained);
      retained = a;
      retained_count++;
    }
    a = next;
  }
  if (retained_countp)
    *retained_countp = retained_count;
  return retained;
}

static int arena_register_list(TGAlloc *alloc, GCArena *a)
{
  for (; a != NULL; a = lj_arena_next_acq(a))
    if (!arena_registry_insert_existing(alloc, a, a->hdr.flags))
      return 0;
  return 1;
}

int lj_arena_alloc_register_existing(TGAlloc *alloc)
{
  uint32_t k;
  if (!alloc || !lj_arena_alloc_registry_acq(alloc))
    return 1;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    if (!arena_register_list(alloc, alloc->owned[k]) ||
	!arena_register_list(alloc, alloc->needsweep[k]) ||
	!arena_register_list(alloc, alloc->quarantine[k]) ||
	!arena_register_list(alloc, arena_reclaimed_acq(alloc, k)))
      return 0;
  }
  return 1;
}

static uint32_t arena_count_live_cells(const GCArena *a)
{
  uint32_t i = LJ_AFIRST_CELL, n = 0;
  while (i < LJ_ARENA_CELLS) {
    uint32_t st = (!arena_side_owners_none(a, i) ||
	lj_arena_lifetime_state_acq(a, i) != LJ_ARENA_LIFETIME_FREE) ?
	3u : lj_arena_state(a, i);
    uint32_t j = i + 1u;
    if (st == 0) {
      i++;
      continue;
    }
    while (j < LJ_ARENA_CELLS && lj_arena_state(a, j) == 0 &&
	   arena_side_owners_none(a, j) &&
	   lj_arena_lifetime_state_acq(a, j) == LJ_ARENA_LIFETIME_FREE)
      j++;
    if (st & 2u)
      n += j - i;
    i = j;
  }
  return n;
}

typedef struct ArenaLargestRun {
  uint32_t start;
  uint32_t len;
} ArenaLargestRun;

static void arena_find_largest_run(uint32_t start, uint32_t len, void *ud)
{
  ArenaLargestRun *lr = (ArenaLargestRun *)ud;
  if (len > lr->len) {
    lr->start = start;
    lr->len = len;
  }
}

typedef struct ArenaRebuildRuns {
  TGAlloc *alloc;
  GCArena *a;
  ArenaLargestRun bump;
} ArenaRebuildRuns;

typedef struct ArenaRebuildFree {
  TGAlloc *alloc;
  GCArena *a;
  uint32_t limit;
} ArenaRebuildFree;

static void arena_rebuild_run(uint32_t start, uint32_t len, void *ud)
{
  ArenaRebuildRuns *rr = (ArenaRebuildRuns *)ud;
  if (rr->bump.len >= LJ_BUMP_MIN &&
      start == rr->bump.start && len == rr->bump.len)
    return;
  /* Rebuild publication is the point at which this span becomes selectable
  ** from an allocator bin. Scrub READY and complete cdata coverage for every
  ** rebuilt run, not only the largest run selected as the bump window. */
  if (arena_set_free_run(rr->a, start, len))
    arena_link_run_head(rr->alloc, rr->a, start, len);
}

static void arena_rebuild_free_run(uint32_t start, uint32_t len, void *ud)
{
  ArenaRebuildFree *rf = (ArenaRebuildFree *)ud;
  if (start >= rf->limit)
    return;
  if (len > rf->limit - start)
    len = rf->limit - start;
  if (arena_set_free_run(rf->a, start, len))
    arena_link_run_head(rf->alloc, rf->a, start, len);
}

static int arena_adopt_reclaimed_one(TGAlloc *alloc, uint32_t k)
{
  GCArena *a, *next, *old_owned;
  ArenaRebuildFree rf;
  TGAlloc staged;
  LJArenaFreeRun *old_bins[LJ_ALLOC_NBINS];
  uint32_t old_binmask, b;
  if (!alloc || k >= LJ_ARENA_NKINDS)
    return 0;
  a = arena_reclaimed_acq(alloc, k);
  for (;;) {
    if (!a)
      return 0;
    next = lj_arena_next_acq(a);
    if (arena_reclaimed_cas(alloc, k, &a, next))
      break;
  }
  la_store32_rel(&a->hdr.flags,
		 lj_arena_flags_acq(a) | LJ_AF_PREPSWEEP);
  if (!lj_arena_reclaim_seal(a))
    goto retry_reclaimed;
  /* Rebuild into private staging heads while CLOSED|SEALED. If a bit-only
  ** publisher dirties the gate, no reusable run has escaped into owner bins. */
  (void)arena_remote_free_drain_one(alloc, a);
  arena_late_clear_committed_free(a);
  if (!lj_arena_reclaim_clear_pending(a) ||
      !arena_reclaim_commit_sealed(a))
    goto retry_unseal;
  memset(&staged, 0, sizeof(staged));
  memcpy(old_bins, alloc->bins[k], sizeof(old_bins));
  old_binmask = alloc->binmask[k];
  old_owned = alloc->owned[k];
  rf.alloc = &staged;
  rf.a = a;
  rf.limit = LJ_ARENA_CELLS;
  /* A terminal-word quarantine may already have proved that no live cell
  ** remains. Treat that count only as a hint: the whole-payload free-run
  ** transform repeats the complete root/recovery/destructor/lifetime
  ** preflight while this exact SEALED generation is clean. A stale typed
  ** identity or any other owner rejects the transform and retains the
  ** unchanged per-run scanner as the fail-closed fallback. */
  if (arena_lifetime_managed(a) && a->hdr.live_cells == 0 &&
      arena_set_free_run(a, LJ_AFIRST_CELL,
			 LJ_ARENA_CELLS - LJ_AFIRST_CELL) &&
      arena_link_run_head(&staged, a, LJ_AFIRST_CELL,
			  LJ_ARENA_CELLS - LJ_AFIRST_CELL)) {
#if defined(LJ_ARENA_TEST_HELPERS)
    (void)la_add64_rlx(&arena_test_adopt_whole_count, 1);
#endif
  } else {
    lj_arena_scan_free_runs(a, arena_rebuild_free_run, &rf);
  }
  for (b = 0; b < LJ_ALLOC_NBINS; b++) {
    LJArenaFreeRun *head = staged.bins[k][b];
    if (head) {
      LJArenaFreeRun *tail = head;
      while (tail->next)
	tail = tail->next;
      tail->next = alloc->bins[k][b];
      alloc->bins[k][b] = head;
    }
  }
  arena_binmask_publish(alloc, k, alloc->binmask[k] | staged.binmask[k]);
  la_store32_rel(&a->hdr.flags,
		 lj_arena_flags_acq(a) & ~LJ_AF_RECLAIMED);
  lj_arena_next_rel(a, old_owned);
  alloc->owned[k] = a;
  if (arena_remote_open_sealed(a)) {
    la_store32_rel(&a->hdr.flags,
		   lj_arena_flags_acq(a) & ~LJ_AF_PREPSWEEP);
    arena_list_count_add(&alloc->owned_count[k], 1u);
    return 1;
  }
  /* A publisher won after the clean generation CAS. Roll back owner-visible
  ** staging without touching its bit intent, then retry from CLOSED later. */
  alloc->owned[k] = old_owned;
  memcpy(alloc->bins[k], old_bins, sizeof(old_bins));
  arena_binmask_publish(alloc, k, old_binmask);
  la_store32_rel(&a->hdr.flags,
		 lj_arena_flags_acq(a) | LJ_AF_RECLAIMED);

retry_unseal:
  lj_arena_reclaim_unseal(a, 1);
retry_reclaimed:
  {
    GCArena *head = arena_reclaimed_acq(alloc, k);
    do {
      lj_arena_next_rel(a, head);
    } while (!arena_reclaimed_cas(alloc, k, &head, a));
  }
  return 0;
}

/* Terminal PRE reconciliation after every producer has joined. PENDING can
** conservatively survive the final counted leave even though no publisher is
** left to materialize another side-plane intent. Only the exact completed
** CLOSED|PENDING generation is repairable: never open the gate, complete a
** SEALED writer, consume an admission, or infer anything from late/root/
** recovery/lifetime state here. */
int lj_arena_terminal_reconcile(GCArena *a)
{
  uint64_t active;
  if (!a || arena_terminal_closed_acq(a))
    return 0;
  active = lj_arena_remote_active_acq(a);
  for (;;) {
    uint64_t expect = active;
    if (active == 0 || active == LJ_ARENA_REMOTE_CLOSED)
      return 1;
    if (active !=
	(LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_PENDING) ||
	arena_terminal_closed_acq(a))
      return 0;
    if (la_cas64(&a->hdr.remote_active, &expect,
		 LJ_ARENA_REMOTE_CLOSED, LA_ACQ_REL, LA_ACQ))
      return 1;
    active = expect;
  }
}

static int arena_terminal_reconcile_list(GCArena *a)
{
  GCArena *slow = a, *fast = a;
  int ok = 1;
  while (a) {
    GCArena *next = lj_arena_next_acq(a);
    if (!lj_arena_terminal_reconcile(a))
      ok = 0;
    slow = slow ? lj_arena_next_acq(slow) : NULL;
    if (fast)
      fast = lj_arena_next_acq(fast);
    if (fast)
      fast = lj_arena_next_acq(fast);
    if (fast && fast == slow)
      return 0;
    a = next;
  }
  return ok;
}

int lj_arena_alloc_terminal_reconcile(TGAlloc *alloc)
{
  uint32_t k;
  int ok = 1;
  if (!alloc)
    return 0;
  /* These are read-only walks. In particular reclaimed[] is not exchanged or
  ** detached: a failed terminal attempt retains every arena for diagnosis or
  ** an idempotent retry after the outstanding owner has been reconciled. */
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    if (!arena_terminal_reconcile_list(alloc->owned[k]))
      ok = 0;
    if (!arena_terminal_reconcile_list(alloc->needsweep[k]))
      ok = 0;
    if (!arena_terminal_reconcile_list(alloc->quarantine[k]))
      ok = 0;
    if (!arena_terminal_reconcile_list(arena_reclaimed_acq(alloc, k)))
      ok = 0;
  }
  return ok;
}

static int arena_terminal_ready_list(GCArena *a, int all_owners)
{
  GCArena *slow = a, *fast = a;
  int ok = 1;
  while (a) {
    GCArena *next = lj_arena_next_acq(a);
    if (!lj_arena_terminal_reconcile(a) ||
	!lj_arena_gc2_tokens_empty_acq(a) ||
	!lj_arena_gc2_desc_mapping_clear_acq(a) ||
	(all_owners &&
	 (!lj_arena_recovery_empty(a) || !arena_root_empty(a))))
      ok = 0;
    slow = slow ? lj_arena_next_acq(slow) : NULL;
    if (fast)
      fast = lj_arena_next_acq(fast);
    if (fast)
      fast = lj_arena_next_acq(fast);
    if (fast && fast == slow)
      return 0;
    a = next;
  }
  return ok;
}

int lj_arena_alloc_terminal_ready(TGAlloc *alloc)
{
  uint32_t k;
  int ok = 1;
  if (!alloc)
    return 1;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    if (!arena_terminal_ready_list(alloc->owned[k], 1))
      ok = 0;
    if (!arena_terminal_ready_list(alloc->needsweep[k], 1))
      ok = 0;
    if (!arena_terminal_ready_list(alloc->quarantine[k], 1))
      ok = 0;
    if (!arena_terminal_ready_list(arena_reclaimed_acq(alloc, k), 1))
      ok = 0;
  }
  return ok;
}

int lj_arena_alloc_terminal_certificate_ready(TGAlloc *alloc)
{
  uint32_t k;
  int ok = 1;
  if (!alloc)
    return 1;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    if (!arena_terminal_ready_list(alloc->owned[k], 0))
      ok = 0;
    if (!arena_terminal_ready_list(alloc->needsweep[k], 0))
      ok = 0;
    if (!arena_terminal_ready_list(alloc->quarantine[k], 0))
      ok = 0;
    if (!arena_terminal_ready_list(arena_reclaimed_acq(alloc, k), 0))
      ok = 0;
  }
  return ok;
}

void lj_arena_alloc_init(TGAlloc *alloc)
{
  /* Initialization/reset is private or follows joined terminal destruction.
  ** No snapshot reader may race the whole-allocator memset. */
  memset(alloc, 0, sizeof(*alloc));
}

int lj_arena_alloc_fini_try(TGAlloc *alloc)
{
  uint32_t k;
  int retained = 0;
  if (!alloc)
    return 1;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    uint32_t retained_count;
    alloc->owned[k] = arena_unmap_list(alloc, alloc->owned[k],
				     &retained_count);
    la_store32_rel(&alloc->owned_count[k], retained_count);
    alloc->needsweep[k] = arena_unmap_list(alloc, alloc->needsweep[k],
					 &retained_count);
    la_store32_rel(&alloc->needsweep_count[k], retained_count);
    alloc->quarantine[k] = arena_unmap_list(alloc, alloc->quarantine[k], NULL);
    la_storeptr_rel((void **)&alloc->reclaimed[k],
	(void *)arena_unmap_list(alloc, arena_reclaimed_acq(alloc, k), NULL));
    retained |= alloc->owned[k] != NULL || alloc->needsweep[k] != NULL ||
	alloc->quarantine[k] != NULL || arena_reclaimed_acq(alloc, k) != NULL;
    memset(&alloc->bump[k], 0, sizeof(alloc->bump[k]));
    memset(alloc->bins[k], 0, sizeof(alloc->bins[k]));
    arena_binmask_publish(alloc, k, 0);
  }
  if (!retained) {
    lj_arena_alloc_init(alloc);
    return 1;
  }
  return 0;
}

void lj_arena_alloc_fini(TGAlloc *alloc)
{
  (void)lj_arena_alloc_fini_try(alloc);
}

static void arena_clear_marks_list(GCArena *a)
{
  for (; a != NULL; a = lj_arena_next_acq(a)) {
    uint32_t w;
    for (w = 0; w < LJ_ARENA_WORDS; w++) {
      uint64_t block = la_load64_acq(&a->block[w]);
      uint64_t recovery = arena_recovery_block_bits(a, w);
      (void)la_and64_rlx(&a->mark[w], ~(block & ~recovery));
    }
  }
}

void lj_arena_alloc_clear_marks(TGAlloc *alloc)
{
  uint32_t k;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    arena_clear_marks_list(alloc->owned[k]);
    arena_clear_marks_list(alloc->needsweep[k]);
    /* A quarantine belongs to the still-open sweep cycle. Its LIVE state and
    ** mark bit are a late-publication rescue proof and must survive until that
    ** arena either finishes or the cycle is explicitly restored. */
  }
}

void lj_arena_alloc_rebuild_free_kind(TGAlloc *alloc, uint32_t k)
{
  if (k < LJ_ARENA_NKINDS) {
    GCArena *a;
    arena_clear_bins(alloc, k);
    for (a = alloc->owned[k]; a != NULL; a = lj_arena_next_acq(a)) {
      ArenaRebuildFree rf;
      rf.alloc = alloc;
      rf.a = a;
      rf.limit = alloc->bump[k].a == a ? alloc->bump[k].cell :
					 LJ_ARENA_CELLS;
      lj_arena_scan_free_runs(a, arena_rebuild_free_run, &rf);
    }
  }
}

void lj_arena_alloc_rebuild_free(TGAlloc *alloc)
{
  uint32_t k;
  for (k = 0; k < LJ_ARENA_NKINDS; k++)
    lj_arena_alloc_rebuild_free_kind(alloc, k);
}

int lj_arena_alloc_prepare_sweep_kind(TGAlloc *alloc, uint32_t k)
{
  GCArena *a, *reclaimed, *work, *prepared = NULL;
  uint32_t prepared_count = 0;
  int complete = 1;
  if (k >= LJ_ARENA_NKINDS)
    return 0;
  /* A completed sweep leaves arenas on the CLOSED reclaimed stack until the
  ** owner needs allocation space. A later collection must not depend on such
  ** an allocation. First detach every owner allocation cursor and bin at this
  ** owner safepoint. A gate collision can then leave an arena in PREPSWEEP
  ** without returning it to allocator-visible state; the mutator may allocate
  ** a fresh arena while the last publisher wakes a later RESET_ALLOC retry.
  */
  if (alloc->bump[k].a) {
    if (alloc->bump[k].cell < alloc->bump[k].end) {
      la_store32_rel(&alloc->bump[k].a->hdr.prep_bump_cell,
		     alloc->bump[k].cell);
      la_store32_rel(&alloc->bump[k].a->hdr.prep_bump_end,
		     alloc->bump[k].end);
    } else {
      la_store32_rel(&alloc->bump[k].a->hdr.prep_bump_cell, 0);
      la_store32_rel(&alloc->bump[k].a->hdr.prep_bump_end, 0);
    }
  }
  a = alloc->owned[k];
  alloc->owned[k] = NULL;
  la_store32_rel(&alloc->owned_count[k], 0);
  alloc->bump[k].a = NULL;
  alloc->bump[k].cell = 0;
  alloc->bump[k].end = 0;
  arena_clear_bins(alloc, k);
  reclaimed = (GCArena *)la_xchgptr_acqrel(
    (void **)&alloc->reclaimed[k], NULL);
  work = alloc->needsweep[k];  /* PREPSWEEP entries from an earlier retry. */
  alloc->needsweep[k] = NULL;
  la_store32_rel(&alloc->needsweep_count[k], 0);

  /* Publish every newly detached source as PREPSWEEP before it becomes part of
  ** the retry list. Remote physical frees then use nonintrusive late pins even
  ** if their OPEN admission raced this owner-side detachment. */
  while (a || reclaimed) {
    GCArena *next;
    if (!a) {
      a = reclaimed;
      reclaimed = NULL;
    }
    next = lj_arena_next_acq(a);
    if (next == a ||
	(next && (lj_arena_flags_acq(next) & LJ_AF_NEEDSWEEP)))
      next = NULL;
    la_store32_rel(&a->hdr.flags,
		   (lj_arena_flags_acq(a) &
		    ~(LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) |
		   LJ_AF_PREPSWEEP);
    lj_arena_next_rel(a, work);
    work = a;
    a = next;
  }

  while (work) {
    GCArena *next = lj_arena_next_acq(work);
    uint32_t flags = lj_arena_flags_acq(work);
    if (next == work)
      next = NULL;
    /* A prior retry may already have completed this arena. */
    if ((flags & (LJ_AF_NEEDSWEEP|LJ_AF_PREPSWEEP)) ==
	LJ_AF_NEEDSWEEP)
      goto prepared_one;
    la_store32_rel(&work->hdr.flags,
		   (flags &
		    ~(LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) |
		   LJ_AF_PREPSWEEP);
    if (!lj_arena_reclaim_seal(work)) {
      complete = 0;
      goto prepared_one;
    }
    /* SEALED still admits counted intent producers. PREPSWEEP is visible
    ** before the exact zero-count generation LP, so a post-LP reader returns
    ** DEAD without touching header bytes while sweep/late state is rebuilt. */
    if (!lj_arena_reclaim_clear_pending(work) ||
	!arena_reclaim_commit_sealed(work)) {
      lj_arena_reclaim_unseal(work, 1);
      complete = 0;
      goto prepared_one;
    }
    arena_prepare_bump_tail(work);
    arena_sweep_state_prepare(work);
    /* Ordinary OPEN records are stable under SEALED. Grace-late frees are
    ** bit-only and become FREEING only in this later generation, before its
    ** grace. A publication racing this exchange remains pinned for one extra
    ** generation and sets PENDING before the bit. */
    (void)lj_arena_remote_free_drain_sweep(alloc, work);
    (void)arena_late_prepare_consume(work);
    lj_arena_reclaim_unseal(work, 1);
    la_store32_rel(&work->hdr.flags,
		   (lj_arena_flags_acq(work) &
		    ~(LJ_AF_PREPSWEEP|LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) |
		   LJ_AF_NEEDSWEEP);

prepared_one:
    lj_arena_next_rel(work, prepared);
    prepared = work;
    prepared_count++;
    work = next;
  }
  alloc->needsweep[k] = prepared;
  la_store32_rel(&alloc->needsweep_count[k], prepared_count);
  return complete;
}

GCArena *lj_arena_alloc_quarantine_one(TGAlloc *alloc, uint32_t kind,
					       uint64_t retire_epoch)
{
  GCArena *a, *next;
  if (!alloc || kind >= LJ_ARENA_NKINDS)
    return NULL;
  a = alloc->needsweep[kind];
  if (!a)
    return NULL;
  next = lj_arena_next_acq(a);
  if (next == a || (next && !(lj_arena_flags_acq(next) & LJ_AF_NEEDSWEEP)))
    next = NULL;
  alloc->needsweep[kind] = next;
  arena_list_count_pop(&alloc->needsweep_count[kind], next);
  a->hdr.retire_epoch = retire_epoch;
  a->hdr.reclaim_cell = LJ_AFIRST_CELL;
  la_store32_rel(&a->hdr.flags,
		 (lj_arena_flags_acq(a) &
		  ~(LJ_AF_NEEDSWEEP|LJ_AF_RECLAIMED|LJ_AF_PREPSWEEP)) |
		 LJ_AF_QUARANTINE);
  lj_arena_next_rel(a, alloc->quarantine[kind]);
  alloc->quarantine[kind] = a;
  return a;
}

GCArena *lj_arena_alloc_quarantine_head(const TGAlloc *alloc, uint32_t kind)
{
  return alloc && kind < LJ_ARENA_NKINDS ? alloc->quarantine[kind] : NULL;
}

GCArena *lj_arena_alloc_reclaimed_head(const TGAlloc *alloc, uint32_t kind)
{
  return alloc && kind < LJ_ARENA_NKINDS ?
    arena_reclaimed_acq(alloc, kind) : NULL;
}

static int arena_quarantine_bitmap_ready(GCArena *a, uint32_t *retry_cell)
{
  uint32_t w;
  int terminal_managed = arena_lifetime_managed(a);
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t b = la_load64_acq(&a->block[w]);
    uint64_t late, marks;
    uint32_t j;
    /* This preliminary proof only skips scalar observations. It grants no
    ** commit authority: a publisher racing it dirties the sealed generation,
    ** and the post-commit apply independently proves the word again. */
    if (b == 0 || arena_quarantine_terminal_freeing_word(
		   a, w, b, terminal_managed))
      continue;
    late = la_load64_acq(&a->late[w]);
    marks = la_load64_acq(&a->mark[w]);
    for (j = 0; j < 64u; j++) {
      uint32_t state;
      if (!(b & ((uint64_t)1 << j)))
	continue;
      if (!lj_arena_gc2_reclaim_clear_acq(a, (w << 6) + j)) {
	if (retry_cell)
	  *retry_cell = (w << 6) + j;
	return 0;
      }
      if (lj_arena_recovery_state_acq(a, (w << 6) + j) !=
	  LJ_ARENA_RECOVERY_IDLE) {
	if (retry_cell)
	  *retry_cell = (w << 6) + j;
	return 0;  /* Recovery is authoritative actionable work. */
      }
      if (late & ((uint64_t)1 << j))
	continue;
      state = lj_arena_sweep_state_acq(a, (w << 6) + j);
      if (state == LJ_ARENA_SWEEP_LIVE ||
	  state == LJ_ARENA_SWEEP_RETIRED) {
	if (retry_cell)
	  *retry_cell = (w << 6) + j;
	return 0;  /* First actionable detached root or pending destructor. */
      }
      if (state == LJ_ARENA_SWEEP_WHITE &&
	  !(marks & ((uint64_t)1 << j))) {
	/* The ownership-spine pass has completed, so a remaining WHITE start is
	** raw/opaque storage. Resolve it here while SEALED instead of returning
	** an owner-only reason with no publisher wake. */
	(void)la_bit_test_and_set64(&a->mark[w], j);
      }
    }
  }
  return 1;
}

static int arena_quarantine_apply_bitmap(GCArena *a, int preserve_marks)
{
  uint32_t w;
  int terminal_managed = arena_lifetime_managed(a);
  int all_terminal = terminal_managed;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t b = la_load64_acq(&a->block[w]);
    uint64_t m, late, recovery, root;
    uint64_t live = 0, freeing = 0;
    uint32_t j;
    /* The clean sealed commit immediately precedes this call. Re-prove every
    ** packed plane rather than inheriting bitmap_ready's pre-commit sample.
    ** On success the scalar loop's exact result is live=0/freeing=b. Preserve
    ** its write order so READY/type discovery cannot outlive block removal. */
    if (arena_quarantine_terminal_freeing_word(
	  a, w, b, terminal_managed)) {
      m = la_load64_acq(&a->mark[w]);
      /* Preserve the scalar path's acquire edge from release-published late
      ** provenance before any structural metadata is removed. The value is
      ** irrelevant for terminal FREE/FREEING, but its ordering is not. */
      (void)la_load64_acq(&a->late[w]);
      (void)la_and64_rlx(&a->ready[w], 0);
      la_store64_rel(&a->block[w], 0);
      arena_dtor_clear_mask_rlx(a, w, b);
      la_store64_rel(&a->mark[w], ((~b) & m) | b);
      (void)la_and64_rlx(&a->late[w], 0);
      continue;
    }
    all_terminal = 0;
    m = la_load64_acq(&a->mark[w]);
    late = la_load64_acq(&a->late[w]);
    recovery = arena_recovery_block_bits(a, w);
    root = arena_root_block_bits(a, w);
    for (j = 0; j < 64u; j++) {
      uint32_t cell = (w << 6) + j;
      uint32_t state;
      if (!(b & ((uint64_t)1 << j)))
	continue;
      if (!lj_arena_gc2_reclaim_clear_acq(a, cell)) {
	live |= (uint64_t)1 << j;
	continue;
      }
      if ((recovery | root) & ((uint64_t)1 << j)) {
	live |= (uint64_t)1 << j;
	continue;
      }
      state = lj_arena_sweep_state_acq(a, cell);
      if (state == LJ_ARENA_SWEEP_WHITE) {
	live |= (uint64_t)1 << j;
      } else if (arena_lifetime_managed(a)) {
	uint32_t life = lj_arena_lifetime_state_acq(a, cell);
	uint64_t bit = (uint64_t)1 << j;
	int own = 0;
	if (life == LJ_ARENA_LIFETIME_LIVE) {
	  if (!lj_arena_lifetime_state_cas(a, cell,
		LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT)) {
	    live |= bit;
	    continue;
	  }
	  own = 1;
	  if (!arena_mutation_closed_quiet(a) ||
	      !lj_arena_bm_get(a->block, cell) ||
	      lj_arena_recovery_state_acq(a, cell) !=
		LJ_ARENA_RECOVERY_IDLE ||
	      lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE ||
	      lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_WHITE) {
	    (void)arena_destruct_restore_live(a, cell);
	    live |= bit;
	    continue;
	  }
	  life = LJ_ARENA_LIFETIME_DESTRUCT;
	}
	if (life == LJ_ARENA_LIFETIME_DESTRUCT && (own || (late & bit))) {
	  /* A quarantine owner may adopt another tentative DESTRUCT only for an
	  ** irrevocable external intent. Repeat the SC admission proof and exact
	  ** side-owner checks itself; the other claimant's earlier sample is not
	  ** transferable. */
	  if (!arena_mutation_closed_quiet(a) ||
	      !lj_arena_bm_get(a->block, cell) ||
	      lj_arena_recovery_state_acq(a, cell) !=
		LJ_ARENA_RECOVERY_IDLE ||
	      lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE ||
	      lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_WHITE) {
	    if (own)
	      (void)arena_destruct_restore_live(a, cell);
	    live |= bit;
	    continue;
	  }
	  if (arena_destruct_commit_free(a, cell)) {
	    uint32_t terminal_state = lj_arena_sweep_state_acq(a, cell);
	    /* This claimant crossed the irreversible lifetime LP and is now the
	    ** unique sweep-state classifier. Any unexpected CAS loss is fail-stop. */
	    if (terminal_state != LJ_ARENA_SWEEP_FREEING &&
		!lj_arena_sweep_state_cas(a, cell, terminal_state,
					  LJ_ARENA_SWEEP_FREEING))
	      abort();
	    if (terminal_state == LJ_ARENA_SWEEP_RETIRED) {
	      uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	      lj_assertX(old != 0, "arena quarantine deferred underflow");
	      UNUSED(old);
	    }
	    state = LJ_ARENA_SWEEP_FREEING;
	    life = LJ_ARENA_LIFETIME_FREE;
	  }
	}
	if (life == LJ_ARENA_LIFETIME_FREE &&
	    state == LJ_ARENA_SWEEP_FREEING &&
	    lj_arena_gc2_reclaim_clear_acq(a, cell)) {
	  freeing |= bit;
	} else {
	  live |= bit;  /* Constructor/link/recovery owner: retain without wait. */
	}
      } else {
	uint64_t bit = (uint64_t)1 << j;
	freeing |= bit;
      }
    }
    /* As in lj_arena_sweep_words(), keep coverage until free-run selection.
    ** block/mark boundaries prevent a dead start from being admitted. */
    (void)la_and64_rlx(&a->ready[w], live);
    la_store64_rel(&a->block[w], live);
    /* Clear only identities whose old discovery boundary was removed by this
    ** exact apply. A block-zero kind is malformed or still constructor-owned;
    ** either way it remains authoritative and must pin reuse fail-closed. */
    arena_dtor_clear_mask_rlx(a, w, b & ~live);
    la_store64_rel(&a->mark[w], ((~b) & m) | freeing |
		   (preserve_marks ? live : (uint64_t)0));
    /* Consume exactly the starts whose block publication removed them. This
    ** also catches a duplicate late intent racing our earlier snapshot, and
    ** leaves every retained incarnation pinned. */
    (void)la_and64_rlx(&a->late[w], live);
  }
  return all_terminal;
}

#if defined(LJ_ARENA_TEST_HELPERS)
int lj_arena_test_terminal_freeing_word(const GCArena *a, uint32_t word)
{
  return a && word < LJ_ARENA_WORDS &&
    arena_quarantine_terminal_freeing_word(
      a, word, la_load64_acq(&a->block[word]), arena_lifetime_managed(a));
}

int lj_arena_test_quarantine_apply_bitmap(GCArena *a, int preserve_marks)
{
  return a ? arena_quarantine_apply_bitmap(a, preserve_marks) : 0;
}
#endif

int lj_arena_alloc_quarantine_finish(TGAlloc *alloc, uint32_t kind,
				      GCArena *a, uint32_t sweep_epoch,
				      int preserve_marks, uint32_t *reasonp)
{
  GCArena *head, *next, *reclaimed;
  uint32_t retry_cell = LJ_ARENA_CELLS;
  uint32_t reason = LJ_ARENA_FINISH_NONE;
  int all_terminal;
  if (reasonp)
    *reasonp = LJ_ARENA_FINISH_NONE;
  if (!alloc || kind >= LJ_ARENA_NKINDS || !a)
    return 0;
  head = alloc->quarantine[kind];
  if (head != a)
    return 0;
  la_store32_rel(&a->hdr.flags,
		 (lj_arena_flags_acq(a) & ~LJ_AF_RECLAIMED) |
		 LJ_AF_QUARANTINE|LJ_AF_PREPSWEEP);
  if ((lj_arena_remote_active_acq(a) &
       (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED)) !=
      (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED)) {
    reason = LJ_ARENA_FINISH_PUBLISHER;
    goto blocked;
  }
  (void)arena_quarantine_settle_late(a);
  if (la_loadptr_acq((void *const *)&a->hdr.remote_free) != NULL) {
    reason = LJ_ARENA_FINISH_PUBLISHER;
    goto blocked;
  }
  if (la_load64_acq(&a->hdr.retire_epoch) == ~(uint64_t)0) {
    reason = LJ_ARENA_FINISH_EPOCH;
    goto blocked;
  }
  if (lj_arena_gcprep_pending_acq(a) != 0) {
    /* lifetime FREE makes the Lua header semantically unreadable, but a
    ** terminal-preparation queue still owns its bytes and side allocations.
    ** Do not interpret FREE+FREEING as synchronous destructor completion. */
    reason = LJ_ARENA_FINISH_ACTIONABLE;
    goto blocked;
  }
  if (lj_arena_reclaim_deferred_acq(a) != 0) {
    reason = LJ_ARENA_FINISH_ACTIONABLE;
    goto blocked;
  }
  if (!arena_quarantine_bitmap_ready(a, &retry_cell)) {
    reason = retry_cell < LJ_ARENA_CELLS ?
      LJ_ARENA_FINISH_ACTIONABLE : LJ_ARENA_FINISH_UNCLASSIFIED;
    goto blocked;
  }
  /* Clear only an intent whose producer has left, then switch generations by
  ** exact clean CAS. Any rescue admitted after validation dirties the word and
  ** defeats this LP. After it, rescue loses without touching mark/state. */
  if (!lj_arena_reclaim_clear_pending(a) ||
      !arena_reclaim_commit_sealed(a)) {
    reason = LJ_ARENA_FINISH_PUBLISHER;
    goto blocked;
  }
  all_terminal = arena_quarantine_apply_bitmap(a, preserve_marks);
  /* Publish every terminal block decision before WHITE is reused as the
  ** post-commit sidecar value. Committed readers sample state then block, so
  ** a dead cell can never combine reset-WHITE with its old block bit. */
  arena_sweep_state_reset(a);
  next = lj_arena_next_acq(a);
  if (next == a || (next && !(lj_arena_flags_acq(next) & LJ_AF_QUARANTINE)))
    next = NULL;
  alloc->quarantine[kind] = next;
  lj_arena_next_rel(a, NULL);
  /* Every independently proven terminal word published block=0 above. Only a
  ** scalar fallback can retain a live start, so avoid a redundant arena scan
  ** exactly when the complete word certificate proves the count is zero. */
  a->hdr.live_cells = all_terminal ? 0 : arena_count_live_cells(a);
  a->hdr.sweep_epoch = sweep_epoch;
  a->hdr.retire_epoch = 0;
  a->hdr.reclaim_cell = LJ_AFIRST_CELL;
  a->hdr.reclaim_deferred = 0;
  lj_assertX(lj_arena_gcprep_pending_acq(a) == 0,
	     "arena committed with incomplete terminal preparation");
  la_store32_rel(&a->hdr.flags,
		 (lj_arena_flags_acq(a) &
		  ~(LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|LJ_AF_PREPSWEEP)) |
		 LJ_AF_RECLAIMED);
  reclaimed = arena_reclaimed_acq(alloc, kind);
  do {
    lj_arena_next_rel(a, reclaimed);
  } while (!arena_reclaimed_cas(alloc, kind, &reclaimed, a));
  lj_arena_reclaim_unseal(a, 1);  /* Queue-only until stable adoption. */
  if (reasonp)
    *reasonp = LJ_ARENA_FINISH_COMMITTED;
  return 1;

blocked:
  if (retry_cell < a->hdr.reclaim_cell)
    a->hdr.reclaim_cell = retry_cell;
  la_store32_rel(&a->hdr.flags,
		 lj_arena_flags_acq(a) & ~LJ_AF_PREPSWEEP);
  if (reasonp)
    *reasonp = reason;
  return 0;
}

void lj_arena_alloc_prepare_sweep(TGAlloc *alloc)
{
  uint32_t k;
  for (k = 0; k < LJ_ARENA_NKINDS; k++)
    lj_arena_alloc_prepare_sweep_kind(alloc, k);
}

int lj_arena_alloc_restore_sweep_kind(TGAlloc *alloc, uint32_t k)
{
  GCArena *a;
  if (k >= LJ_ARENA_NKINDS)
    return 0;
  while ((a = alloc->needsweep[k]) != NULL) {
    GCArena *next = lj_arena_next_acq(a);
    GCArena *old_owned;
    TGAlloc staged;
    LJArenaFreeRun *old_bins[LJ_ALLOC_NBINS];
    uint32_t old_binmask, old_flags, b;
    ArenaRebuildFree rf;
    uint32_t i;
    if (next == a ||
	(next && !(lj_arena_flags_acq(next) &
		  (LJ_AF_NEEDSWEEP|LJ_AF_PREPSWEEP))))
      next = NULL;
    la_store32_rel(&a->hdr.flags,
		   lj_arena_flags_acq(a) | LJ_AF_PREPSWEEP);
    if (!lj_arena_reclaim_seal(a))
      return 0;
    (void)lj_arena_remote_free_drain_sweep(alloc, a);
    /* The legal abort path precedes root detachment, so PREPSWEEP may have
    ** produced only WHITE and destructor-complete FREEING starts. Refuse an
    ** unexpected actionable generation before taking the irreversible LP. */
    for (i = LJ_AFIRST_CELL; i < LJ_ARENA_CELLS; i++) {
      uint32_t state;
      if (!lj_arena_bm_get(a->block, i))
	continue;
      state = lj_arena_sweep_state_acq(a, i);
      if (arena_lifetime_managed(a) &&
	  state == LJ_ARENA_SWEEP_FREEING &&
	  lj_arena_lifetime_state_acq(a, i) == LJ_ARENA_LIFETIME_FREE) {
	lj_arena_reclaim_unseal(a, 1);
	return 0;  /* Irreversible terminal LP cannot be restored to readable. */
      }
      if (state == LJ_ARENA_SWEEP_LIVE ||
	  state == LJ_ARENA_SWEEP_RETIRED) {
	lj_arena_reclaim_unseal(a, 1);
	return 0;
      }
    }
    /* Exact C|S->S precedes every bitmap/sidecar mutation. A producer admitted
    ** before this point dirties PENDING and defeats the CAS; a later producer
    ** is counted in the committed generation and defeats exact OPEN. */
    if (!lj_arena_reclaim_clear_pending(a) ||
	!arena_reclaim_commit_sealed(a)) {
      lj_arena_reclaim_unseal(a, 1);
      return 0;
    }
    arena_prepare_bump_tail(a);
    /* PREPSWEEP already consumed late[] before changing those starts to
    ** FREEING, so absence of a late bit is not provenance. Re-pin every
    ** FREEING allocation and retain its block bit through a fresh full grace.
    ** This conservatively delays ordinary remote frees by one cycle too. */
    for (i = LJ_AFIRST_CELL; i < LJ_ARENA_CELLS; i++) {
      if (lj_arena_bm_get(a->block, i) &&
	  lj_arena_sweep_state_acq(a, i) == LJ_ARENA_SWEEP_FREEING) {
	int held = 0;
	if (arena_lifetime_managed(a)) {
	  uint32_t life = lj_arena_lifetime_state_acq(a, i);
	  if (life == LJ_ARENA_LIFETIME_LIVE) {
	    if (!lj_arena_lifetime_state_cas(a, i,
		  LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_MUTATING))
	      continue;
	    if (!arena_mutation_closed_quiet(a)) {
	      arena_mutation_restore_live(a, i);
	      continue;
	    }
	    held = 1;
	  } else {
	    continue;
	  }
	}
	(void)la_bit_test_and_set64(&a->late[i >> 6], i & 63);
	(void)lj_arena_sweep_state_cas(a, i, LJ_ARENA_SWEEP_FREEING,
					 LJ_ARENA_SWEEP_WHITE);
	if (held)
	  arena_mutation_restore_live(a, i);
      }
    }
    arena_sweep_state_reset(a);
    a->hdr.reclaim_cell = LJ_AFIRST_CELL;
    a->hdr.reclaim_deferred = 0;
    arena_late_clear_committed_free(a);
    memset(&staged, 0, sizeof(staged));
    rf.alloc = &staged;
    rf.a = a;
    rf.limit = LJ_ARENA_CELLS;
    lj_arena_scan_free_runs(a, arena_rebuild_free_run, &rf);
    memcpy(old_bins, alloc->bins[k], sizeof(old_bins));
    old_binmask = alloc->binmask[k];
    old_owned = alloc->owned[k];
    old_flags = lj_arena_flags_acq(a);
    for (b = 0; b < LJ_ALLOC_NBINS; b++) {
      LJArenaFreeRun *head = staged.bins[k][b];
      if (head) {
	LJArenaFreeRun *tail = head;
	while (tail->next)
	  tail = tail->next;
	tail->next = alloc->bins[k][b];
	alloc->bins[k][b] = head;
      }
    }
    arena_binmask_publish(alloc, k, alloc->binmask[k] | staged.binmask[k]);
    la_store32_rel(&a->hdr.flags,
		   lj_arena_flags_acq(a) & ~LJ_AF_NEEDSWEEP);
    lj_arena_next_rel(a, old_owned);
    alloc->owned[k] = a;
    if (!arena_remote_open_sealed(a)) {
      alloc->owned[k] = old_owned;
      memcpy(alloc->bins[k], old_bins, sizeof(old_bins));
      arena_binmask_publish(alloc, k, old_binmask);
      la_store32_rel(&a->hdr.flags, old_flags);
      lj_arena_next_rel(a, next);
      lj_arena_reclaim_unseal(a, 1);
      return 0;
    }
    la_store32_rel(&a->hdr.flags,
		   lj_arena_flags_acq(a) & ~LJ_AF_PREPSWEEP);
    arena_list_count_add(&alloc->owned_count[k], 1u);
    alloc->needsweep[k] = next;
    arena_list_count_pop(&alloc->needsweep_count[k], next);
  }
  return 1;
}

void lj_arena_alloc_sweep_kind(TGAlloc *alloc, uint32_t kind,
				       uint32_t epoch, int preserve_marks)
{
  while (lj_arena_sweep_one(alloc, kind, epoch, preserve_marks) != NULL)
    ;
}

static void arena_unlink_owned_duplicate(TGAlloc *alloc, uint32_t kind,
					 GCArena *target)
{
  GCArena *prev = NULL, *a;
  uint32_t prefix = 0;
  if (!alloc || kind >= LJ_ARENA_NKINDS || !target)
    return;
  for (a = alloc->owned[kind]; a != NULL;) {
    GCArena *next = lj_arena_next_acq(a);
    if (a == target) {
      if (next == a || (next && (next->hdr.flags & LJ_AF_NEEDSWEEP)))
	next = NULL;
      if (prev)
	lj_arena_next_rel(prev, next);
      else
	alloc->owned[kind] = next;
      if (!next)
	la_store32_rel(&alloc->owned_count[kind], prefix);
      else
	arena_list_count_pop(&alloc->owned_count[kind], next);
      return;
    }
    if (next == a)
      return;
    prev = a;
    prefix++;
    a = next;
  }
}

GCArena *lj_arena_sweep_one(TGAlloc *alloc, uint32_t kind, uint32_t epoch,
			    int preserve_marks)
{
  GCArena *a;
  ArenaLargestRun lr = { 0, 0 };
  ArenaRebuildRuns rr;
  if (kind >= LJ_ARENA_NKINDS)
    return NULL;
  a = alloc->needsweep[kind];
  if (!a)
    return NULL;
  arena_unlink_owned_duplicate(alloc, kind, a);
  {
    GCArena *next = lj_arena_next_acq(a);
    if (next == a || (next && !(next->hdr.flags & LJ_AF_NEEDSWEEP)))
      next = NULL;
    alloc->needsweep[kind] = next;
    arena_list_count_pop(&alloc->needsweep_count[kind], next);
  }
  lj_arena_next_rel(a, NULL);
  lj_arena_sweep_words(a, preserve_marks);
  lj_arena_scan_free_runs(a, arena_find_largest_run, &lr);
  if (lr.len >= LJ_BUMP_MIN) {
    if (!arena_set_free_run(a, lr.start, lr.len)) {
      lr.len = 0;  /* A root claim appeared: retain, never reuse the span. */
    }
  }
  rr.alloc = alloc;
  rr.a = a;
  rr.bump = lr;
  lj_arena_scan_free_runs(a, arena_rebuild_run, &rr);
  if (lr.len >= LJ_BUMP_MIN) {
    arena_publish_bump_run(alloc, kind);
    alloc->bump[kind].a = a;
    alloc->bump[kind].cell = lr.start;
    alloc->bump[kind].end = lr.start + lr.len;
  }
  a->hdr.live_cells = arena_count_live_cells(a);
  a->hdr.sweep_epoch = epoch;
  a->hdr.flags &= ~LJ_AF_NEEDSWEEP;
  lj_arena_next_rel(a, alloc->owned[kind]);
  alloc->owned[kind] = a;
  arena_list_count_add(&alloc->owned_count[kind], 1u);
  return a;
}

static uint32_t arena_transfer_list(GCArena **dstp, GCArena *a,
				    uint32_t owner_tid, global_State *progress_g)
{
  uint32_t n = 0;
  while (a) {
    GCArena *next = lj_arena_next_acq(a);
    lj_arena_owner_rel(a, owner_tid);
    arena_progress_bind_rel(a, progress_g);
    lj_arena_next_rel(a, *dstp);
    *dstp = a;
    a = next;
    n++;
  }
  return n;
}

uint32_t lj_arena_alloc_transfer(TGAlloc *dst, TGAlloc *src)
{
  uint32_t k, n = 0;
  uint32_t owner_tid;
  uint32_t remote_carry;
  TGState *owner_tg;
  global_State *progress_g;
  if (!dst || !src || dst == src)
    return 0;
  /* The source owner is dead/quiescent. Its arena-local Treiber queues remain
  ** routable across owner_tid changes, but draining now avoids carrying dead
  ** payload records through list rebuild. */
  (void)lj_arena_remote_free_drain_force(src);
  /* A conservative rearm can remain when an arena is already in an exact
  ** sweep generation. Carry it to the destination instead of stranding an
  ** advisory wake in the dead source allocator; sweep itself remains
  ** hint-independent. The caller's dead/quiescent-source precondition excludes
  ** a producer appearing after this exchange. */
  remote_carry = lj_arena_remote_pending_xchg_acqrel(src, 0);
  owner_tid = lj_arena_alloc_owner_acq(dst);
  owner_tg = (TGState *)lj_arena_alloc_owner_tg_acq(dst);
  progress_g = owner_tg ? owner_tg->gl : NULL;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    LJArenaBump *b = &src->bump[k];
    uint32_t moved;
    if (b->a && b->cell < b->end)
      (void)arena_set_free_run(b->a, b->cell, b->end - b->cell);
    src->bump[k].a = NULL;
    src->bump[k].cell = 0;
    src->bump[k].end = 0;
    arena_clear_bins(src, k);
    moved = arena_transfer_list(&dst->owned[k], src->owned[k], owner_tid,
				progress_g);
    arena_list_count_add(&dst->owned_count[k], moved);
    n += moved;
    src->owned[k] = NULL;
    la_store32_rel(&src->owned_count[k], 0);
    moved = arena_transfer_list(&dst->needsweep[k], src->needsweep[k],
				owner_tid, progress_g);
    arena_list_count_add(&dst->needsweep_count[k], moved);
    n += moved;
    src->needsweep[k] = NULL;
    la_store32_rel(&src->needsweep_count[k], 0);
    n += arena_transfer_list(&dst->quarantine[k], src->quarantine[k],
			     owner_tid, progress_g);
    src->quarantine[k] = NULL;
    {
      GCArena *a = (GCArena *)la_xchgptr_acqrel(
	(void **)&src->reclaimed[k], NULL);
      while (a) {
	GCArena *next = lj_arena_next_acq(a);
	GCArena *head = arena_reclaimed_acq(dst, k);
	lj_arena_owner_rel(a, owner_tid);
	arena_progress_bind_rel(a, progress_g);
	do {
	  lj_arena_next_rel(a, head);
	} while (!arena_reclaimed_cas(dst, k, &head, a));
	a = next;
	n++;
      }
    }
    lj_arena_alloc_rebuild_free_kind(dst, k);
  }
  lj_arena_alloc_set_registry(src, NULL);
  lj_arena_alloc_owner_rel(src, 0);
  lj_arena_alloc_owner_tg_rel(src, NULL);
  lj_arena_alloc_black_rel(src, 0);
  if (remote_carry)
    lj_arena_remote_pending_rel(dst, 1);
  return n;
}

static GCArena *arena_alloc_fresh(TGAlloc *alloc, PRNGState *rs,
				  uint32_t flags)
{
  uint32_t k = arena_kind(flags);
  GCArena *a = lj_arena_map(rs, flags);
  if (!a)
    return NULL;
  lj_arena_owner_rel(a, lj_arena_alloc_owner_acq(alloc));
  {
    TGState *owner_tg = (TGState *)lj_arena_alloc_owner_tg_acq(alloc);
    arena_progress_bind_rel(a, owner_tg ? owner_tg->gl : NULL);
  }
  if (!arena_registry_insert_fresh(alloc, a, flags)) {
    lj_arena_unmap(a);
    return NULL;
  }
  lj_arena_next_rel(a, alloc->owned[k]);
  alloc->owned[k] = a;
  arena_list_count_add(&alloc->owned_count[k], 1u);
  alloc->bump[k].a = a;
  alloc->bump[k].cell = LJ_AFIRST_CELL;
  alloc->bump[k].end = LJ_ARENA_CELLS;
  return a;
}

static int arena_reserve_lifetime(GCArena *a, uint32_t cell, uint32_t flags)
{
  int ok;
  if (!(flags & LJ_AF_TRAVERSABLE))
    return (flags & (LJ_AF_ROOT_CONSTRUCT|LJ_AF_DTOR_CONSTRUCT)) == 0;
  if (flags & LJ_AF_ROOT_CONSTRUCT)
    return lj_arena_root_construct_claim(a, cell);
  if (flags & LJ_AF_DTOR_CONSTRUCT) {
    if (lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE)
      return 0;
    if (!arena_gc2_token_none_acq(a, cell) ||
	!lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_FREE,
					 LJ_ARENA_LIFETIME_CONSTRUCT))
      return 0;
    if (!arena_gc2_prepare_incarnation(a, cell)) {
      ok = lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_FREE);
      lj_assertX(ok, "arena dtor bump GC2 rollback lost");
      UNUSED(ok);
      return 0;
    }
    return 1;
  }
  if (!arena_gc2_token_none_acq(a, cell) ||
      !lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_FREE,
					   LJ_ARENA_LIFETIME_MUTATING))
    return 0;
  if (!arena_gc2_prepare_incarnation(a, cell)) {
    ok = lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_MUTATING, LJ_ARENA_LIFETIME_FREE);
    lj_assertX(ok, "arena bump GC2 rollback lost");
    UNUSED(ok);
    return 0;
  }
  ok = lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_MUTATING, LJ_ARENA_LIFETIME_LIVE);
  lj_assertX(ok, "arena bump reservation lifetime publication failed");
  return ok;
}

static int arena_reserve_lifetime_pair(GCArena *a, uint32_t first,
					uint32_t second, uint32_t flags)
{
  int rootok, lifeok;
  int root_construct = (flags & LJ_AF_ROOT_CONSTRUCT) != 0;
  int dtor_construct = (flags & LJ_AF_DTOR_CONSTRUCT) != 0;
  if (!(flags & LJ_AF_TRAVERSABLE) ||
      root_construct == dtor_construct ||
      first == second || second >= LJ_ARENA_CELLS)
    return 0;
  if (dtor_construct) {
    if (lj_arena_root_state_acq(a, first) != LJ_ARENA_ROOT_NONE ||
        lj_arena_root_state_acq(a, second) != LJ_ARENA_ROOT_NONE ||
	!arena_gc2_token_none_acq(a, first) ||
	!arena_gc2_token_none_acq(a, second))
      return 0;
    if (first / LJ_ARENA_LIFETIME_CELLS_PER_WORD ==
	second / LJ_ARENA_LIFETIME_CELLS_PER_WORD) {
      lifeok = lj_arena_lifetime_state_cas_pair(a, first, second,
	LJ_ARENA_LIFETIME_FREE, LJ_ARENA_LIFETIME_CONSTRUCT);
      if (!lifeok)
	return 0;
      if (arena_gc2_prepare_incarnation(a, first) &&
	  arena_gc2_prepare_incarnation(a, second))
	return 1;
      lifeok = lj_arena_lifetime_state_cas_pair(a, first, second,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_FREE);
      lj_assertX(lifeok, "arena pair dtor GC2 rollback lost");
      UNUSED(lifeok);
      return 0;
    }
    if (!lj_arena_lifetime_state_cas(a, first,
	  LJ_ARENA_LIFETIME_FREE, LJ_ARENA_LIFETIME_CONSTRUCT))
      return 0;
    if (!lj_arena_lifetime_state_cas(a, second,
	  LJ_ARENA_LIFETIME_FREE, LJ_ARENA_LIFETIME_CONSTRUCT)) {
      lifeok = lj_arena_lifetime_state_cas(a, first,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_FREE);
      lj_assertX(lifeok, "arena split typed construction rollback lost");
      UNUSED(lifeok);
      return 0;
    }
    if (arena_gc2_prepare_incarnation(a, first) &&
	arena_gc2_prepare_incarnation(a, second))
      return 1;
    rootok = lj_arena_lifetime_state_cas(a, second,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_FREE);
    lifeok = rootok && lj_arena_lifetime_state_cas(a, first,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_FREE);
    lj_assertX(rootok && lifeok, "arena split dtor GC2 rollback lost");
    UNUSED(rootok); UNUSED(lifeok);
    return 0;
  }
  if (first / LJ_ARENA_LIFETIME_CELLS_PER_WORD ==
      second / LJ_ARENA_LIFETIME_CELLS_PER_WORD &&
      first / LJ_ARENA_ROOT_CELLS_PER_WORD ==
      second / LJ_ARENA_ROOT_CELLS_PER_WORD) {
    if (!arena_gc2_token_none_acq(a, first) ||
	!arena_gc2_token_none_acq(a, second))
      return 0;
    lifeok = lj_arena_lifetime_state_cas_pair(a, first, second,
	LJ_ARENA_LIFETIME_FREE, LJ_ARENA_LIFETIME_CONSTRUCT);
    if (!lifeok)
      return 0;
    rootok = lj_arena_root_state_cas_pair(a, first, second,
	LJ_ARENA_ROOT_NONE, LJ_ARENA_ROOT_LINKING);
    if (rootok && arena_gc2_prepare_incarnation(a, first) &&
	arena_gc2_prepare_incarnation(a, second))
      return 1;
    if (rootok) {
      rootok = lj_arena_root_state_cas_pair(a, first, second,
	LJ_ARENA_ROOT_LINKING, LJ_ARENA_ROOT_NONE);
      lj_assertX(rootok, "arena pair root GC2 rollback lost");
      if (!rootok)
	return 0;
    }
    lifeok = lj_arena_lifetime_state_cas_pair(a, first, second,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_FREE);
    lj_assertX(lifeok, "arena pair construction claim rollback lost");
    UNUSED(lifeok);
    return 0;
  }
  if (!lj_arena_root_construct_claim(a, first))
    return 0;
  if (lj_arena_root_construct_claim(a, second))
    return 1;
  rootok = lj_arena_root_construct_abandon(a, first);
  lifeok = rootok && lj_arena_lifetime_state_cas(a, first,
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_FREE);
  lj_assertX(rootok && lifeok,
	     "arena split pair construction claim rollback lost");
  UNUSED(rootok); UNUSED(lifeok);
  return 0;
}

static int arena_reserve_lifetime_kind(GCArena *a, uint32_t first,
	uint32_t second, uint32_t flags, uint32_t first_kind,
	uint32_t second_kind)
{
  int pair = second != first;
  if ((flags & LJ_AF_DTOR_CONSTRUCT) &&
      !arena_dtor_kind_preflight_unpublished(a, first, second, pair))
    return 0;  /* Stale type authority pins the unpublished span. */
  if (!(pair ? arena_reserve_lifetime_pair(a, first, second, flags) :
	arena_reserve_lifetime(a, first, flags)))
    return 0;
  if (!(flags & LJ_AF_DTOR_CONSTRUCT))
    return first_kind == LJ_ARENA_DTOR_NONE &&
      second_kind == LJ_ARENA_DTOR_NONE;
  arena_dtor_kind_publish_claimed(a, first, second, pair,
	first_kind, second_kind);
  return 1;
}

static int arena_reserve_bump_impl(TGAlloc *alloc, PRNGState *rs,
	uint32_t flags, uint32_t ncells, uint32_t second_offset,
	uint32_t first_kind, uint32_t second_kind,
	GCArena **ap, uint32_t *cellp)
{
  uint32_t k = arena_kind(flags);
  LJArenaBump *b;
  uint32_t cell;
  int root_construct = (flags & LJ_AF_ROOT_CONSTRUCT) != 0;
  int dtor_construct = (flags & LJ_AF_DTOR_CONSTRUCT) != 0;
  if (!alloc || !ap || !cellp || ncells == 0 ||
      ncells > LJ_ARENA_CELLS - LJ_AFIRST_CELL ||
      (second_offset != 0 &&
	(second_offset >= ncells || (!root_construct && !dtor_construct))) ||
      ((root_construct || dtor_construct) &&
	!(flags & LJ_AF_TRAVERSABLE)) ||
      (root_construct && dtor_construct) ||
      (dtor_construct &&
       (first_kind == LJ_ARENA_DTOR_NONE ||
	first_kind > LJ_ARENA_DTOR_MAX ||
	(second_offset != 0 &&
	 (second_kind == LJ_ARENA_DTOR_NONE ||
	  second_kind > LJ_ARENA_DTOR_MAX)))) ||
      (!dtor_construct && (first_kind != LJ_ARENA_DTOR_NONE ||
			   second_kind != LJ_ARENA_DTOR_NONE)))
    return 0;
  b = &alloc->bump[k];
  if (!b->a || b->cell + ncells > b->end) {
    uint32_t bin = 0;
    LJArenaFreeRun **pp;
    arena_publish_bump_run(alloc, k);
    pp = arena_find_run(alloc, k, ncells, &bin);
    if ((!pp || !*pp) && arena_adopt_reclaimed_one(alloc, k))
      pp = arena_find_run(alloc, k, ncells, &bin);
    if (!pp || !*pp) {
      (void)lj_arena_remote_free_drain(alloc);
      pp = arena_find_run(alloc, k, ncells, &bin);
    }
    if (pp && *pp) {
      LJArenaFreeRun *run = *pp;
      GCArena *a = lj_arena_of(run);
      uint32_t start = run->start;
      uint32_t len = run->len;
      *pp = run->next;
      arena_refresh_binmask(alloc, k, bin);
      /* Rebuild may have coalesced several adjacent state-1 boundaries into
      ** this private bump window. Erase the complete old boundary map now;
      ** specialized C/VM/JIT bump publishers subsequently install only their
      ** real object starts. */
      if (!arena_clear_extent_range(a, start, len)) {
	/* Keep a vetoed detached run private. Re-inserting metadata which failed
	** its ownership precondition would make the same unsafe span reusable. */
	b->a = a;
	b->cell = start;
	b->end = start + len;
	return 0;
      }
      b->a = NULL;
      b->cell = 0;
      b->end = 0;
      /*
      ** Specialized bump callers do not need generic free-run reuse order.
      ** Reserve the first cells for the caller and keep the remaining run as
      ** the unpublished bump window, matching sweep's largest-run protocol.
      */
      if (len > ncells) {
	b->a = a;
	b->cell = start + ncells;
	b->end = start + len;
      }
      if (!arena_reserve_lifetime_kind(a, start,
	    second_offset ? start + second_offset : start, flags,
	    first_kind, second_kind)) {
	/* No bytes or block bit have been published. Retain the detached span as
	** a private bump window so a transient descriptor conflict cannot make it
	** allocator-visible. */
	b->a = a;
	b->cell = start;
	b->end = start + len;
	return 0;
      }
      *ap = a;
      *cellp = start;
      return 1;
    }
    if (!arena_alloc_fresh(alloc, rs, flags))
      return 0;
  }
  cell = b->cell;
  b->cell = cell + ncells;
  if (!arena_reserve_lifetime_kind(b->a, cell,
	second_offset ? cell + second_offset : cell, flags,
	first_kind, second_kind)) {
    b->cell = cell;
    return 0;
  }
  *ap = b->a;
  *cellp = cell;
  return 1;
}

int lj_arena_reserve_bump(TGAlloc *alloc, PRNGState *rs, uint32_t flags,
			  uint32_t ncells, GCArena **ap, uint32_t *cellp)
{
  return arena_reserve_bump_impl(alloc, rs, flags, ncells, 0,
	LJ_ARENA_DTOR_NONE, LJ_ARENA_DTOR_NONE, ap, cellp);
}

int lj_arena_reserve_bump_pair(TGAlloc *alloc, PRNGState *rs, uint32_t flags,
	uint32_t ncells, uint32_t second_offset,
	GCArena **ap, uint32_t *cellp)
{
  return arena_reserve_bump_impl(alloc, rs, flags, ncells, second_offset,
	LJ_ARENA_DTOR_NONE, LJ_ARENA_DTOR_NONE, ap, cellp);
}

int lj_arena_reserve_bump_dtor(TGAlloc *alloc, PRNGState *rs,
	uint32_t flags, uint32_t ncells, uint32_t dtor_kind,
	GCArena **ap, uint32_t *cellp)
{
  return arena_reserve_bump_impl(alloc, rs,
	flags | LJ_AF_DTOR_CONSTRUCT, ncells, 0,
	dtor_kind, LJ_ARENA_DTOR_NONE, ap, cellp);
}

int lj_arena_reserve_bump_dtor_pair(TGAlloc *alloc, PRNGState *rs,
	uint32_t flags, uint32_t ncells, uint32_t second_offset,
	uint32_t first_kind, uint32_t second_kind,
	GCArena **ap, uint32_t *cellp)
{
  return arena_reserve_bump_impl(alloc, rs,
	flags | LJ_AF_DTOR_CONSTRUCT, ncells, second_offset,
	first_kind, second_kind, ap, cellp);
}

void *lj_arena_alloc(TGAlloc *alloc, PRNGState *rs, size_t size,
		     uint32_t flags)
{
  uint32_t k = arena_kind(flags);
  LJArenaBump *b = &alloc->bump[k];
  uint32_t ncells, cell;
  int adopted = 0, drained = 0;
  if (size == 0)
    return NULL;
  if ((flags & LJ_AF_ROOT_CONSTRUCT) &&
      !(flags & LJ_AF_TRAVERSABLE))
    return NULL;
  if (size > LJ_HUGE_THRESHOLD) {
    void *p;
    /* Huge root construction requires a HugeTab full-slot publication. The
    ** allocd path below owns that locator; the direct mapping API does not. */
    if (flags & LJ_AF_ROOT_CONSTRUCT)
      return NULL;
    p = lj_arena_huge_map(rs, size, flags);
    if (p) {
      GCArena *a = lj_arena_of(p);
      TGState *owner_tg =
	(TGState *)lj_arena_alloc_owner_tg_acq(alloc);
      lj_arena_owner_rel(a, lj_arena_alloc_owner_acq(alloc));
      arena_progress_bind_rel(a, owner_tg ? owner_tg->gl : NULL);
    }
    return p;
  }
  ncells = lj_arena_ncells(size);
  if (ncells > LJ_ARENA_CELLS - LJ_AFIRST_CELL)
    return NULL;
  {
    uint32_t bin = 0;
    LJArenaFreeRun **pp;
    if (b->a && b->cell + ncells <= b->end) {
      /* The active bump window is owner-private and absent from every bin.
      ** Consume it before consulting published free runs. Besides improving
      ** locality, this prevents allocation from repeatedly removing a large
      ** run and fully scrubbing its shrinking suffix while an older bump
      ** window remains usable. Bins are revisited as soon as this bounded
      ** window is exhausted. */
      int result;
      cell = b->cell;
      b->cell += ncells;
      result = arena_set_alloc(b->a, cell, ncells,
	lj_arena_alloc_black_acq(alloc),
	(flags & LJ_AF_ROOT_CONSTRUCT) != 0);
      if (result > 0)
	return lj_arena_cellptr(b->a, cell);
      b->cell = cell;
      /* A candidate-local descriptor conflict and a closed plain generation
      ** are both transient allocation vetoes, not resource exhaustion. Drop
      ** this private cursor (publishing it only when still safe) and continue
      ** with independent bins or a fresh arena in this same call. */
    }
    /* Preserve the ordinary exhausted/undersized bump publication path. The
    ** helper safely discards instead when this same arena is transiently
    ** closed, so no caller ever writes a free-run node through that gate. */
    arena_publish_bump_run(alloc, k);
    for (;;) {
      pp = arena_find_run(alloc, k, ncells, &bin);
      if (!pp || !*pp) {
	if (!adopted) {
	  adopted = 1;
	  if (arena_adopt_reclaimed_one(alloc, k))
	    continue;
	}
	if (!drained) {
	  drained = 1;
	  (void)lj_arena_remote_free_drain(alloc);
	  continue;
	}
	if (!arena_alloc_fresh(alloc, rs, flags))
	  return NULL;
	goto bump_alloc;
      }
      {
	LJArenaFreeRun *run = *pp;
	GCArena *a = lj_arena_of(run);
	uint32_t start = run->start;
	uint32_t len = run->len;
	int result;
	*pp = run->next;
	arena_refresh_binmask(alloc, k, bin);
	result = arena_set_alloc(a, start, ncells,
		lj_arena_alloc_black_acq(alloc),
		(flags & LJ_AF_ROOT_CONSTRUCT) != 0);
	if (result <= 0) {
	  /* The candidate changed after validation. Do not touch a possibly
	  ** closed or descriptor-owned run body to relink it; later sweep rebuilds
	  ** this detached boundary. Keep looking rather than report false OOM. */
	  continue;
	}
	if (len > ncells) {
	  /* Clear each rebuilt boundary once, then amortize the tail as a private
	  ** bump window instead of repeatedly scrubbing a shrinking free run. */
	  if (arena_clear_extent_range(a, start + ncells, len - ncells)) {
	    b->a = a;
	    b->cell = start + ncells;
	    b->end = start + len;
	  } else {
	    arena_retain_free_boundaries(a, start + ncells, len - ncells);
	  }
	  /* arena_set_alloc() already committed the requested prefix. If tail
	  ** preparation loses a side-owner race, retain bitmap rediscovery points
	  ** for that unpublished suffix and return the valid allocation. */
	}
	return lj_arena_cellptr(a, start);
      }
    }
  }
bump_alloc:
  cell = b->cell;
  b->cell += ncells;
  if (arena_set_alloc(b->a, cell, ncells,
		      lj_arena_alloc_black_acq(alloc),
		      (flags & LJ_AF_ROOT_CONSTRUCT) != 0) <= 0) {
    b->cell = cell;
    return NULL;
  }
  return lj_arena_cellptr(b->a, cell);
}

void lj_arena_free(TGAlloc *alloc, void *p, size_t size)
{
  GCArena *a;
  uint32_t start, ncells, aflags, oldstate = LJ_ARENA_SWEEP_WHITE;
  int claim, plain_held = 0;
  if (!p || size == 0)
    return;
  /* lua_Alloc's old size is the lifetime-safe class discriminator. Never read
  ** an arena header merely to decide whether a possibly stale huge address is
  ** mapped. Direct huge allocations have no side table and retain the normal
  ** single-owner free contract; arena_allocf uses terminal table ownership. */
  if (size > LJ_HUGE_THRESHOLD) {
    lj_arena_huge_unmap(p, size);
    return;
  }
  a = lj_arena_of(p);
  if (lj_arena_alloc_free_noinsert_acq(alloc))
    return;
  start = lj_arena_cellof(p);
  ncells = lj_arena_ncells(size);
  if (start < LJ_AFIRST_CELL || start + ncells > LJ_ARENA_CELLS ||
      lj_arena_cellptr(a, start) != p ||
      !lj_arena_bm_get(a->block, start))
    return;
  if (!arena_side_owners_none(a, start)) {
    (void)(arena_lifetime_managed(a) ?
	  arena_late_claim_release(a, p, size) :
	  arena_plain_late_pin_admitted(a, p, size));
    return;  /* Lifetime metadata retains bytes; late[] remembers the free. */
  }
  if (arena_lifetime_managed(a)) {
    uint32_t life = lj_arena_lifetime_state_acq(a, start);
    if (life == LJ_ARENA_LIFETIME_FREE ||
	arena_late_claim_release(a, p, size) != 1)
      return;
  }
  claim = arena_destruct_claim_live(a, start, 0);
  if (claim == 0)
    return;
  aflags = lj_arena_flags_acq(a);
  if (claim == 1 &&
      (aflags & (LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|
		 LJ_AF_RECLAIMED))) {
    (void)arena_destruct_restore_live(a, start);
    return;
  }
  if (claim == 2) {
    int gate;
    /* destruct_acquire deliberately leaves SEALED+FREEING held across the
    ** semantic destructor. Ordinary free acquires the same token from OPEN. */
    if (arena_plain_mutation_held(a) &&
	lj_arena_sweep_state_acq(a, start) == LJ_ARENA_SWEEP_FREEING) {
      plain_held = 1;
    } else {
      gate = arena_plain_mutation_claim(a, 0);
      if (gate == 1) {
	plain_held = 1;
      } else {
	if (gate < 0) {
	  (void)arena_late_pin(a, p, size);
	  arena_remote_late_leave(a);
	} else {
	  (void)arena_plain_late_pin_admitted(a, p, size);
	}
	return;
      }
    }
    aflags = lj_arena_flags_acq(a);
    if ((aflags & (LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|
		   LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) ||
	!lj_arena_bm_get(a->block, start) ||
	!arena_side_owners_none(a, start)) {
      (void)arena_late_pin(a, p, size);
      arena_plain_mutation_release(a);
      return;
    }
    oldstate = lj_arena_sweep_state_acq(a, start);
    if (oldstate != LJ_ARENA_SWEEP_FREEING &&
	!lj_arena_sweep_state_cas(a, start, oldstate,
				      LJ_ARENA_SWEEP_FREEING)) {
      (void)arena_late_pin(a, p, size);
      arena_plain_mutation_release(a);
      return;
    }
  }
  if (claim == 1) {
    oldstate = lj_arena_sweep_state_acq(a, start);
    if (!arena_mutation_open_quiet(a, 0) ||
	!arena_side_owners_none(a, start) ||
	!arena_destruct_commit_free(a, start)) {
	(void)arena_destruct_restore_live(a, start);
      return;
    }
    if (oldstate != LJ_ARENA_SWEEP_FREEING &&
	!lj_arena_sweep_state_cas(a, start, oldstate,
					LJ_ARENA_SWEEP_FREEING))
      abort();
  }
  if (!arena_insert_run(alloc, a, start, ncells)) {
    if (claim == 1 || plain_held) {
      /* A descriptor may become globally PINNED after the exact FREE LP, or a
      ** defensive free-run validation may otherwise refuse body publication.
      ** FREE cannot be restored, but block+FREEING+late is already the normal
      ** fail-closed quarantine representation. Preserve it and let a later
      ** sweep retry instead of treating a legitimate authority crossover as
      ** corruption. */
      int pinned = arena_late_pin(a, p, size);
      lj_assertX(pinned == 1,
	"arena terminal free-run refusal lost late quarantine");
      UNUSED(pinned);
    }
  } else if (claim == 1 || plain_held) {
    (void)lj_arena_sweep_state_cas(a, start, LJ_ARENA_SWEEP_FREEING,
					   LJ_ARENA_SWEEP_WHITE);
    (void)la_and64_rlx(&a->late[start >> 6],
	~((uint64_t)1 << (start & 63)));
  }
  if (plain_held)
    arena_plain_mutation_release(a);
}

int lj_arena_free_deferred(TGAlloc *alloc, void *p, size_t size)
{
  GCArena *a;
  uint32_t start, ncells, flags;
  int published;
  if (!alloc)
    return 0;
  if (!p || size == 0)
    return 0;
  if (size > LJ_HUGE_THRESHOLD)
    return 0;
retry_open:
  a = lj_arena_of(p);
  if (lj_arena_quarantine_owns_body(p, size))
    return 1;
  start = lj_arena_cellof(p);
  ncells = lj_arena_ncells(size);
  if (start < LJ_AFIRST_CELL || start + ncells > LJ_ARENA_CELLS)
    return 0;
  if (!arena_side_owners_none(a, start)) {
    (void)(arena_lifetime_managed(a) ?
	  arena_late_claim_release(a, p, size) :
	  arena_plain_late_pin_admitted(a, p, size));
    return 1;
  }
  if (!arena_remote_enter(a)) {
    int late = arena_remote_late_publish(a, p, size);
    if (late == 0)
      goto retry_open;
    return 1;  /* Published, duplicate, or conservatively retained. */
  }
  if (!arena_side_owners_none(a, start)) {
    (void)(arena_lifetime_managed(a) ?
	  arena_late_claim_release(a, p, size) : arena_late_pin(a, p, size));
    arena_remote_leave(a);
    return 1;
  }
  flags = lj_arena_flags_acq(a);
  if (flags & (LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|
	       LJ_AF_RECLAIMED)) {
    arena_remote_leave(a);
    return lj_arena_quarantine_owns_body(p, size);
  }
  if (!lj_arena_bm_get(a->block, start)) {
    arena_remote_leave(a);
    return 1;  /* Exact body was already committed free. */
  }
  /* A completed GC destructor must not make its body reusable before a later
  ** sweep/grace. Retain the allocation bit, clear its liveness mark, and pin
  ** the exact start without overwriting the still-SMR-visible header. The next
  ** PREPSWEEP converts the bit to FREEING before arming a fresh grace. */
  (void)la_and64_rlx(&a->mark[start >> 6],
		     ~((uint64_t)1 << (start & 63)));
  published = arena_lifetime_managed(a) ?
    arena_late_claim_release(a, p, size) : arena_late_pin(a, p, size);
  arena_remote_leave(a);
  return published != 0;
}

void *lj_arena_realloc(TGAlloc *alloc, PRNGState *rs, void *p,
		       size_t osize, size_t nsize, uint32_t flags)
{
  void *np;
  int oldhuge;
  if (!p)
    return lj_arena_alloc(alloc, rs, nsize, flags);
  oldhuge = osize > LJ_HUGE_THRESHOLD;
  if (!oldhuge) {
    GCArena *a = lj_arena_of(p);
    uint32_t cell = lj_arena_cellof(p);
    int managed = arena_lifetime_managed(a);
    uint32_t life = managed ? lj_arena_lifetime_state_acq(a, cell) :
	LJ_ARENA_LIFETIME_LIVE;
    if (!arena_side_owners_none(a, cell) ||
	(managed && life != LJ_ARENA_LIFETIME_LIVE)) {
      if (nsize == 0 && managed && life == LJ_ARENA_LIFETIME_FREE)
	return NULL;  /* A pre-destructor terminal owner already has the body. */
      if (nsize == 0)
	(void)(managed ? arena_late_claim_release(a, p, osize) :
			 arena_plain_late_pin_admitted(a, p, osize));
      return nsize == 0 ? p : NULL;  /* Preserve; caller may retry later. */
    }
  }
  if (nsize == 0) {
    lj_arena_free(alloc, p, osize);
    return NULL;
  }
  /* osize, not an allocation-header probe, is valid after a competing huge
  ** table owner has unmapped p. The direct API still assumes the caller owns
  ** p while copying; arena_allocf adds a BUSY pin for shared huge mappings. */
  if (oldhuge && nsize > LJ_HUGE_THRESHOLD &&
      lj_arena_huge_mapsize(osize) == lj_arena_huge_mapsize(nsize))
    return p;
  if (oldhuge || nsize > LJ_HUGE_THRESHOLD) {
    size_t csize = osize < nsize ? osize : nsize;
    np = lj_arena_alloc(alloc, rs, nsize, flags);
    if (!np)
      return NULL;
    memcpy(np, p, csize);
    lj_arena_free(alloc, p, osize);
    return np;
  }
  if (nsize <= osize) {
    uint32_t ocells = lj_arena_ncells(osize);
    uint32_t ncells = lj_arena_ncells(nsize);
    if (ncells < ocells) {
      GCArena *a = lj_arena_of(p);
      uint32_t cell = lj_arena_cellof(p);
      int claim = arena_mutation_claim_live(a, cell, 0);
      int plain_held = 0;
      if (claim == 0)
	return NULL;  /* Original allocation remains unchanged and retryable. */
      if (claim == 2) {
	uint32_t aflags;
	int gate = arena_plain_mutation_claim(a, 0);
	if (gate != 1) {
	  if (gate < 0)
	    arena_remote_late_leave(a);
	  goto move_realloc;
	}
	plain_held = 1;
	aflags = lj_arena_flags_acq(a);
	if ((aflags & (LJ_AF_PREPSWEEP|LJ_AF_NEEDSWEEP|
		       LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) ||
	    !lj_arena_bm_get(a->block, cell) || lj_arena_late_get(a, cell) ||
	    !arena_side_owners_none(a, cell)) {
	  arena_plain_mutation_release(a);
	  goto move_realloc;
	}
      }
      if (!arena_insert_run(alloc, a, cell + ncells, ocells - ncells)) {
	if (claim == 1) {
	  arena_mutation_restore_live(a, cell);
	  /* A traversable suffix can remain pinned by an interior root/recovery
	  ** owner. Keep the original extent retryable: moving and then freeing the
	  ** old full extent would encounter the same durable veto after terminal
	  ** ownership and turn this accepted fail-closed state into an abort. */
	  return NULL;
	}
	if (plain_held)
	  arena_plain_mutation_release(a);
	goto move_realloc;
      }
      if (claim == 1)
	arena_mutation_restore_live(a, cell);
      if (plain_held)
	arena_plain_mutation_release(a);
    }
    return p;
  }
move_realloc:
  np = lj_arena_alloc(alloc, rs, nsize, flags);
  if (!np)
    return NULL;
  memcpy(np, p, osize < nsize ? osize : nsize);
  lj_arena_free(alloc, p, osize);
  return np;
}

void lj_arena_allocd_init(LJArenaAllocD *ad, TGAlloc *alloc, PRNGState *rs,
			  uint32_t flags)
{
  ad->alloc = alloc;
  ad->prng = rs;
  ad->huge = NULL;
  ad->flags = flags;
}

void lj_arena_allocd_sethugetab(LJArenaAllocD *ad, HugeTab *ht)
{
  ad->huge = ht;
}

static uint32_t arena_allocf_hflags(LJArenaAllocD *ad, uint32_t flags)
{
  uint32_t hflags = 0;
  if (flags & LJ_AF_TRAVERSABLE)
    hflags |= LJ_HUGEF_TRAVERSABLE;
  if (lj_arena_alloc_black_acq(ad->alloc))
    hflags |= LJ_HUGEF_MARK;
  if (flags & LJ_AF_ROOT_CONSTRUCT)
    hflags |= LJ_AF_ROOT_CONSTRUCT;
  return hflags;
}

static void *arena_allocf_new(LJArenaAllocD *ad, size_t size, uint32_t flags)
{
  void *p;
  if ((flags & LJ_AF_ROOT_CONSTRUCT) &&
      !(flags & LJ_AF_TRAVERSABLE))
    return NULL;
  if (!ad->huge || size <= LJ_HUGE_THRESHOLD)
    return lj_arena_alloc(ad->alloc, ad->prng, size, flags);
  p = lj_arena_huge_map(ad->prng, size, flags);
  if (p) {
    GCArena *a = lj_arena_of(p);
    TGState *owner_tg =
      (TGState *)lj_arena_alloc_owner_tg_acq(ad->alloc);
    lj_arena_owner_rel(a, lj_arena_alloc_owner_acq(ad->alloc));
    /* A final counted reader may turn a deferred plain free into actionable
    ** FREEING|SWEEP_OLD work. Publish the process owner before the HugeTab
    ** entry so that handoff can wake an already-active sweep. */
    arena_progress_bind_rel(a, owner_tg ? owner_tg->gl : NULL);
  }
  if (p && lj_arena_hugetab_insert(ad->huge, p, size,
				   arena_allocf_hflags(ad, flags)) != 1) {
    lj_arena_huge_unmap(p, size);
    return NULL;
  }
  return p;
}

static void arena_allocf_free(LJArenaAllocD *ad, void *ptr, size_t osize)
{
  if (!ptr || osize == 0)
    return;
  if (osize > LJ_HUGE_THRESHOLD) {
    /* The table claim is both duplicate suppression and the linearization
    ** point against prepare_sweep(). A missing entry is stale/already owned;
    ** in either case no mapping header or payload may be touched. */
    if (ad->huge) {
      int finish;
      LJHugeInfo hi;
      if (!lj_arena_hugetab_claim_external_free(ad->huge, ptr, &hi))
	return;
      finish = lj_arena_hugetab_finish_external_free(ad->huge, ptr, &hi);
      lj_assertX(finish != LJ_ARENA_HUGE_FINISH_LOST,
		 "huge external-free ownership lost");
      if (finish == LJ_ARENA_HUGE_FINISH_UNMAP)
	lj_arena_huge_unmap_claimed(ptr, hi.size);
      return;
    }
    lj_arena_free(ad->alloc, ptr, osize);
    return;
  }
  {
    void *owner_tg = lj_arena_alloc_owner_tg_acq(ad->alloc);
    if (lj_arena_quarantine_owns_body(ptr, osize))
      return;  /* Quarantine exclusively converts the body bitmap to free. */
    if (owner_tg != NULL && (void *)lj_thr_get_tg() != owner_tg) {
      int published = lj_arena_remote_free_publish(ad->alloc, ptr, osize);
      /* A valid owner-routed arena body always has one of two destinations:
      ** the per-arena queue or sweep ownership. Never fall through to another
      ** TG's owner-local bins. In release builds an impossible validation
      ** failure conservatively retains the body instead of corrupting them. */
      lj_assertX(published, "arena remote-free publication failed");
      UNUSED(published);
      return;
    }
  }
  lj_arena_free(ad->alloc, ptr, osize);
}

static void *arena_allocf_realloc_huge(LJArenaAllocD *ad, void *ptr,
					 size_t nsize)
{
  LJHugeInfo hi;
  size_t csize;
  void *np;
  int claim, finish;
  /* Claim before allocation: this both rejects a stale table address before
  ** the OS can reuse it for np and pins the old payload throughout allocation
  ** and copy. A failed replacement releases the nonterminal pin unchanged. */
  claim = hugetab_claim_realloc(ad->huge, ptr, &hi);
  if (claim == LJ_HUGE_REALLOC_CLAIM_LOST)
    return NULL;
  if (hi.readers == 0 && nsize > LJ_HUGE_THRESHOLD &&
      lj_arena_huge_mapsize(hi.size) == lj_arena_huge_mapsize(nsize)) {
    int kept;
    /* Same mapping extent: update authoritative logical size and retain the
    ** stock O(1) realloc fast path without a free/sweep observation window. */
    kept = hugetab_finish_realloc_keep(ad->huge, ptr, nsize, &hi);
    if (kept == LJ_HUGE_REALLOC_KEEP_DONE)
      return ptr;
    if (kept == LJ_HUGE_REALLOC_KEEP_PREEMPTED)
      return NULL;
    if (kept == LJ_HUGE_REALLOC_KEEP_LOST) {
      (void)hugetab_release_realloc(ad->huge, ptr, NULL);
      return NULL;
    }
    /* A reader admitted before BUSY owns the old logical size. Fall through
    ** to an allocate/copy/deferred-old release rather than rewriting it. */
  }
  np = arena_allocf_new(ad, nsize, ad->flags);
  if (!np) {
    int released = hugetab_release_realloc(ad->huge, ptr, &hi);
    lj_assertX(released, "huge realloc pin lost on allocation failure");
    UNUSED(released);
    return NULL;
  }
  csize = hi.size < nsize ? hi.size : nsize;
  memcpy(np, ptr, csize);
  finish = hugetab_realloc_to_external_free(ad->huge, ptr, &hi);
  if (finish == LJ_HUGE_REALLOC_MOVE_PREEMPTED) {
    arena_allocf_free(ad, np, nsize);
    return NULL;
  }
  if (finish == LJ_HUGE_REALLOC_MOVE_DEFERRED)
    return np;
  if (finish != LJ_HUGE_REALLOC_MOVE_DIRECT) {
    (void)hugetab_release_realloc(ad->huge, ptr, NULL);
    arena_allocf_free(ad, np, nsize);
    return NULL;
  }
  finish = lj_arena_hugetab_finish_external_free(ad->huge, ptr, &hi);
  lj_assertX(finish != LJ_ARENA_HUGE_FINISH_LOST,
	     "huge realloc ownership lost");
  if (finish == LJ_ARENA_HUGE_FINISH_UNMAP) {
    lj_arena_huge_unmap_claimed(ptr, hi.size);
  } else if (finish == LJ_ARENA_HUGE_FINISH_LOST) {
    arena_allocf_free(ad, np, nsize);
    return NULL;
  }
  return np;
}

void *lj_arena_allocd_alloc(LJArenaAllocD *ad, size_t size, uint32_t flags)
{
  if (!ad || !ad->alloc || !ad->prng)
    return NULL;
  return arena_allocf_new(ad, size, flags);
}

static int arena_publish_start_bit(uint64_t *word, uint64_t bit)
{
  uint64_t old = la_load64_acq(word);
  for (;;) {
    uint64_t expect = old;
    if (old & bit)
      return 1;
    if (la_cas64(word, &expect, old | bit, LA_REL, LA_RLX))
      return 1;
    old = expect;
  }
}

int lj_arena_publish_gco_at(void *p)
{
  GCArena *a;
  uint32_t cell, life;
  uint64_t bit;
  if (!p)
    return 0;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a))
    return 0;
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
      lj_arena_cellptr(a, cell) != p)
    return 0;
  /* READY is irrevocable for this allocation incarnation. Recovery can move
  ** CONSTRUCT to RECOVERY only after observing READY, so a repeated owner-side
  ** publication must accept that already-complete release edge. */
  if (lj_arena_ready_get(a, cell))
    return 1;
  life = lj_arena_lifetime_state_acq(a, cell);
  if (!lj_arena_bm_get(a->block, cell) ||
      !(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) ||
      (life != LJ_ARENA_LIFETIME_LIVE &&
	   life != LJ_ARENA_LIFETIME_CONSTRUCT &&
	   life != LJ_ARENA_LIFETIME_RESCUE))
    return 0;
  bit = (uint64_t)1 << (cell & 63);
  return arena_publish_start_bit(&a->ready[cell >> 6], bit);
}

int lj_arena_allocd_publish_gco(LJArenaAllocD *ad, void *p)
{
  GCArena *a;
  if (!ad || !p)
    return 0;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a))
    return ad->huge && lj_arena_hugetab_publish_gco(ad->huge, p);
  return lj_arena_publish_gco_at(p);
}

int lj_arena_allocd_publish_cdata(LJArenaAllocD *ad, void *p, size_t size,
					   int interior)
{
  if (!ad || !p || size == 0)
    return 0;
  if (size > LJ_HUGE_THRESHOLD)
    return ad->huge &&
      lj_arena_hugetab_publish_cdata(ad->huge, p, interior);
  {
    GCArena *a = lj_arena_of(p);
    uint32_t cell = lj_arena_cellof(p), ncells = lj_arena_ncells(size);
    uint32_t pos = cell, end = cell + ncells;
    uint32_t life = lj_arena_lifetime_state_acq(a, cell);
    uint64_t bit;
    if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS || ncells == 0 ||
	end < cell || end > LJ_ARENA_CELLS ||
	!lj_arena_bm_get(a->block, cell) ||
	!(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) ||
	(life != LJ_ARENA_LIFETIME_LIVE &&
	 life != LJ_ARENA_LIFETIME_CONSTRUCT &&
	 life != LJ_ARENA_LIFETIME_RESCUE))
      return 0;
    while (pos < end) {
      uint32_t wi = pos >> 6;
      uint32_t lo = pos & 63u;
      uint32_t n = end - pos;
      uint32_t take = n < 64u - lo ? n : 64u - lo;
      uint64_t mask = take == 64u ? ~(uint64_t)0 :
	(((uint64_t)1 << take) - 1u) << lo;
      (void)la_or64_rlx(&a->cdata[wi], mask);
      pos += take;
    }
    bit = (uint64_t)1 << (cell & 63);
    /* READY is the final release publication. Its acquire observation orders
    ** the complete allocation-coverage plane and initialized descriptor. */
    return arena_publish_start_bit(&a->ready[cell >> 6], bit);
  }
}

int lj_arena_allocd_publish_interior_cdata(LJArenaAllocD *ad, void *p,
					    size_t size)
{
  return lj_arena_allocd_publish_cdata(ad, p, size, 1);
}

void *lj_arena_allocf(void *ud, void *ptr, size_t osize, size_t nsize)
{
  LJArenaAllocD *ad = (LJArenaAllocD *)ud;
  int oldhuge;
  if (!ad || !ad->alloc || !ad->prng)
    return NULL;
  if (!ptr)
    return arena_allocf_new(ad, nsize, ad->flags);
  if (nsize == 0) {
    arena_allocf_free(ad, ptr, osize);
    return NULL;
  }
  if (osize == 0)
    return NULL;
  oldhuge = osize > LJ_HUGE_THRESHOLD;
  if (ad->huge && oldhuge)
    return arena_allocf_realloc_huge(ad, ptr, nsize);
  if (ad->huge && nsize > LJ_HUGE_THRESHOLD) {
    size_t csize;
    void *np;
    csize = osize < nsize ? osize : nsize;
    np = arena_allocf_new(ad, nsize, ad->flags);
    if (!np)
      return NULL;
    memcpy(np, ptr, csize);
    arena_allocf_free(ad, ptr, osize);
    return np;
  }
  return lj_arena_realloc(ad->alloc, ad->prng, ptr, osize, nsize, ad->flags);
}
