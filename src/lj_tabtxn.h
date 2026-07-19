/*
** Prepared exact table-store transaction.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_TABTXN_H
#define _LJ_TABTXN_H

#include "lj_gc2.h"
#include "lj_tab.h"

/* Address-only keyed-slot resolution for prepared table publishers.  The
** held form is one bounded, non-allocating scan: the caller must already own
** exact leases for |t| and |key| and a GC2 SMR reader which retains both table
** vectors.  FOUND includes an in-range nil array cell and an existing hash key
** whose value is nil.  ABSENT means that no structural slot exists.  RETRY
** covers a changing/retiring generation, FORWARD/KEYLOCK/finalizer claims,
** malformed snapshots and invalid operands.  |addr| is set to zero before
** validation and on every non-FOUND return.  FOUND integerizes the slot while
** the caller's authority is still live; no TValue pointer crosses the API.
**
** resolve_rooted_try supplies those leases and the SMR reader from exact
** authoritative TValue roots.  It performs one bounded attempt, validates the
** exact lua_State owner before and after the scan, never waits or allocates,
** and releases all authority before returning.  The exact main-state/main-TG
** owner remains valid while lua_close runs user finalizers after cur_L is
** cleared; no secondary state receives that terminal exception.
**
** resolve_or_insert_rooted_l is the L-aware attach path.  It may wait, grow or
** allocate to ensure an absent key has a structural slot, but it discards the
** legacy setter's pointer result and derives |addr| only in a wholly fresh
** rooted resolve.  |tabrootp| and |keyrootp| are in/out because either root may
** live on a Lua stack which is rehomed across a wait or allocation.  Callers
** must use the returned/rebased pointers after every normal return, including
** RETRY.  Non-stack inputs must be fixed-address, owner-stable enumerated
** semantic root cells for the whole invocation; concurrently mutable external
** cells are not valid inputs to the allocation-capable form.  A STOPREQ or
** allocation error throws only after resolver authority has been closed. */
#define LJ_TAB_KEYED_SLOT_RETRY		(-1)
#define LJ_TAB_KEYED_SLOT_ABSENT		0
#define LJ_TAB_KEYED_SLOT_FOUND		1

LJ_FUNC int lj_tab_keyed_slot_resolve_held(GCtab *t, cTValue *key,
					    uintptr_t *addr);
LJ_FUNC int lj_tab_keyed_slot_resolve_rooted_try(lua_State *L,
						  cTValue *tabroot,
						  cTValue *keyroot,
						  uintptr_t *addr);
LJ_FUNC int lj_tab_keyed_slot_resolve_or_insert_rooted_l(
  lua_State *L, cTValue **tabrootp, cTValue **keyrootp, uintptr_t *addr);

/* Prepared exact keyed store for a caller-owned publication clock.  The
** caller resolves and integerizes |dst_addr| while the resolver still owns
** the slot/vector lifetime, before prepare and while allocation, resize
** assistance, waits and throws are still legal.  Parent, key and desired
** value must begin in live semantic roots; |dst_addr| itself is only an opaque
** candidate address and is never converted back to a pointer.  A caller must
** not retain a returned TValue pointer and integerize it after that resolver's
** authority has ended.
** A successful prepare retains the exact table vector under SMR, roots/leases
** the parent, key and desired value via the scalar store guard, and leases the
** captured expected value separately.
** The latter prevents GC-address ABA even when the old table edge is replaced.
**
** prepare_snapshot captures whatever ordinary Lua value currently occupies
** the exact key.  prepare_exact accepts only the supplied raw TValue, which
** must begin the call in a live semantic root.  Both retain a separate exact
** lease for a GC-valued expectation and expose its raw identity through the
** accessor while prepared (the accessor is not traversal authority for that
** object's children).  They return LJ_TAB_STORE_CAS_OK only when the
** transaction is ready; other LJ_TAB_STORE_CAS_* results own no retained
** authority and may be retried only after resolving a fresh |dst_addr|.
** The two validation readers intentionally do not overlap guard admission.
** Between them only the integer |dst_addr| survives.  Final current-slot
** validation derives fresh pointer provenance from the current table roots,
** compares integer addresses, and never evaluates or dereferences a pointer
** from a retired allocation.  The separately retained expected lease prevents
** a reclaimed GC value from passing the exact comparison through address
** reuse.
** Hidden table sentinels, finalizer claims, NaN/nil keys and GC tag/header
** mismatches are never transaction operands: prepare/commit reject them as a
** structural retry instead of publishing malformed Lua-visible state.
**
** After prepare returns OK, the owner may publish an odd external clock and
** call commit exactly once.  commit has deliberately no lua_State argument:
** it performs only bounded atomic/TLS validation of the exact owner, validates
** current-generation/key/expected identity, attempts one exact TValue CAS,
** and validates currentness once more.  Its boolean return is the semantic
** commit authority, while |status| independently reports OK, STALE, FORWARD
** or CHANGED.  In particular {true, STALE} is committed and must publish one
** new even clock generation; it must never be retried as an uncommitted store.
** Production commit has no allocation, wait, safepoint, throw, barrier, hook
** or semantic L-aware runtime callout.  LJ_TAB_TEST_HELPERS may install one
** bounded post-CAS hook which is compiled out of production.
**
** The same owner restores the external clock to even before finish/abort.
** finish is mandatory after a committed CAS; abort is valid only when commit
** did not commit (or was not called).  They perform the GC2 handoff and drop
** the descriptor, expected lease and SMR reader.  Cleanup requires the exact
** original universe, TG, physical actor, exact lua_State pointer and owner
** claim before it touches the TLS-accounted SMR reader.  A transaction is
** linear, owner-thread-only and must not be copied while prepared.  init is
** for a fresh or FINISHED object only; calling it while PREPARED would discard
** live authority.  Every failed or finished attempt must be initialized again
** before another prepare. */
typedef struct LJTabKeyedStoreTxn {
  LJGC2TableStoreGuard guard;
  LJGC2Lease expected_lease;
  lua_State *owner_L;
  GCtab *parent;
  uintptr_t dst_addr;
  TValue *dst;
  TValue key;
  TValue expected;
  TValue desired;
  uint8_t state;
  uint8_t smr_active;
  uint8_t guard_active;
  uint8_t expected_lease_active;
  uint8_t commit_attempted;
  uint8_t committed;
  uint8_t reserved[2];
} LJTabKeyedStoreTxn;

LJ_FUNC void lj_tab_keyed_store_txn_init(LJTabKeyedStoreTxn *txn);
LJ_FUNC int lj_tab_keyed_store_prepare_snapshot(lua_State *L,
						 LJTabKeyedStoreTxn *txn,
						 GCtab *parent, uintptr_t dst_addr,
						 cTValue *key, cTValue *desired);
LJ_FUNC int lj_tab_keyed_store_prepare_exact(lua_State *L,
					      LJTabKeyedStoreTxn *txn,
					      GCtab *parent, uintptr_t dst_addr,
					      cTValue *key, cTValue *expected,
					      cTValue *desired);
LJ_FUNC cTValue *lj_tab_keyed_store_expected(const LJTabKeyedStoreTxn *txn);
LJ_FUNC int lj_tab_keyed_store_commit(LJTabKeyedStoreTxn *txn, int *status);
LJ_FUNC int lj_tab_keyed_store_finish(lua_State *L,
				       LJTabKeyedStoreTxn *txn);
LJ_FUNC int lj_tab_keyed_store_abort(lua_State *L,
				      LJTabKeyedStoreTxn *txn);

#ifdef LJ_TAB_TEST_HELPERS
typedef void (*LJTabKeyedStoreTxnPostCasHook)(LJTabKeyedStoreTxn *txn);
LJ_FUNC void lj_tab_keyed_store_test_set_post_cas_hook(
  LJTabKeyedStoreTxnPostCasHook hook);
LJ_FUNC void lj_tab_keyed_slot_test_retry_stack_grow_once(void);
LJ_FUNC uint32_t lj_tab_keyed_slot_test_retry_stack_grow_hits(void);
#endif

#endif /* _LJ_TABTXN_H */
