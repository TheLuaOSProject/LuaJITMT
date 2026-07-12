/*
** OS-thread substrate for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_thr_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"

#include <errno.h>
#include <stdlib.h>
#if LJ_TARGET_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif

#define LJ_THR_TG_EXACT_TAG ((uintptr_t)1u)
#define LJ_THR_TG_TAG_MASK LJ_THR_TG_EXACT_TAG
typedef char lj_thr_tg_tag_requires_alignment[
  __alignof__(TGState) >= 2 ? 1 : -1];
typedef char lj_thr_tg_tag_requires_pointer_width[
  sizeof(uintptr_t) == sizeof(void *) ? 1 : -1];

#if LJ_TARGET_WINDOWS
/* The process TLS slot holds a stable per-thread cell. The cell's tagged word
** is the complete hot binding: low bit zero is a raw compatibility TG pointer;
** low bit one means TLS owns one exact registry lease for the masked body.
** This indirection makes every mutation after first admission an atomic cell
** store instead of another fallible TlsSetValue call. */
typedef struct LJThrTGCell {
  uintptr_t tagged_word;
} LJThrTGCell;

static uint32_t lj_tls_tg_key = TLS_OUT_OF_INDEXES;
static INIT_ONCE lj_tls_tg_once = INIT_ONCE_STATIC_INIT;
#if defined(LJ_THR_TLS_TEST_HELPERS)
static uint32_t lj_tls_test_fail_index_alloc_after;
static uint32_t lj_tls_test_fail_cell_alloc_after;
static uint32_t lj_tls_test_fail_cell_publish_after;

void lj_thr_tls_test_fail_index_alloc(uint32_t nth)
{
  la_store32_rel(&lj_tls_test_fail_index_alloc_after, nth);
}

void lj_thr_tls_test_fail_cell_alloc(uint32_t nth)
{
  la_store32_rel(&lj_tls_test_fail_cell_alloc_after, nth);
}

void lj_thr_tls_test_fail_cell_publish(uint32_t nth)
{
  la_store32_rel(&lj_tls_test_fail_cell_publish_after, nth);
}

static int lj_thr_tls_test_fail_now(uint32_t *count)
{
  uint32_t current = la_load32_acq(count);
  while (current != 0) {
    uint32_t next = current - 1u;
    if (la_cas32(count, &current, next, LA_ACQ_REL, LA_ACQ))
      return next == 0;
  }
  return 0;
}
#endif

static DWORD lj_thr_tls_alloc_index(void)
{
#if defined(LJ_THR_TLS_TEST_HELPERS)
  if (lj_thr_tls_test_fail_now(&lj_tls_test_fail_index_alloc_after)) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return TLS_OUT_OF_INDEXES;
  }
#endif
  return TlsAlloc();
}

static LJThrTGCell *lj_thr_tls_alloc_cell(void)
{
#if defined(LJ_THR_TLS_TEST_HELPERS)
  if (lj_thr_tls_test_fail_now(&lj_tls_test_fail_cell_alloc_after)) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
  }
#endif
  return (LJThrTGCell *)malloc(sizeof(LJThrTGCell));
}

static BOOL lj_thr_tls_publish_cell(DWORD key, LJThrTGCell *cell)
{
#if defined(LJ_THR_TLS_TEST_HELPERS)
  if (lj_thr_tls_test_fail_now(&lj_tls_test_fail_cell_publish_after)) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
  }
#endif
  return TlsSetValue(key, cell);
}
#else
static LJ_TLS uintptr_t lj_tls_tg_word;
#endif
static uint32_t lj_thr_next_tid;

uint32_t lj_thr_newid(void)
{
  return lj_thr_id_alloc(&lj_thr_next_tid);  /* 09 section 9.2. */
}

#if LJ_TARGET_WINDOWS
static BOOL CALLBACK lj_thr_tls_init(PINIT_ONCE once, PVOID param,
				     PVOID *ctx)
{
  DWORD key;
  UNUSED(once);
  UNUSED(param);
  UNUSED(ctx);
  key = lj_thr_tls_alloc_index();
  if (key == TLS_OUT_OF_INDEXES) {
    la_store32_rel(&lj_tls_tg_key, TLS_OUT_OF_INDEXES);
    return FALSE;
  }
  la_store32_rel(&lj_tls_tg_key, (uint32_t)key);
  return TRUE;
}

int lj_thr_tg_tls_init(void)
{
  LJThrTGCell *cell;
  DWORD key;
  if (!InitOnceExecuteOnce(&lj_tls_tg_once, lj_thr_tls_init, NULL, NULL))
    return 0;
  key = (DWORD)la_load32_acq(&lj_tls_tg_key);
  if (key == TLS_OUT_OF_INDEXES)
    return 0;
  cell = (LJThrTGCell *)TlsGetValue(key);
  if (cell)
    return 1;
  cell = lj_thr_tls_alloc_cell();
  if (!cell)
    return 0;
  la_storeuptr_rel(&cell->tagged_word, 0);
  if (!lj_thr_tls_publish_cell(key, cell)) {
    free(cell);
    return 0;
  }
  return 1;
}

static DWORD lj_thr_tls_key(void)
{
  return (DWORD)la_load32_acq(&lj_tls_tg_key);
}

static uintptr_t lj_thr_tls_word_get(void)
{
  DWORD key = lj_thr_tls_key();
  LJThrTGCell *cell = key != TLS_OUT_OF_INDEXES ?
    (LJThrTGCell *)TlsGetValue(key) : NULL;
  return cell ? la_loaduptr_acq(&cell->tagged_word) : 0;
}

static void lj_thr_tls_word_set(uintptr_t word)
{
  DWORD key = lj_thr_tls_key();
  LJThrTGCell *cell = key != TLS_OUT_OF_INDEXES ?
    (LJThrTGCell *)TlsGetValue(key) : NULL;
  if (!cell)
    abort();  /* Install admitted this thread, or an exact view proved it. */
  la_storeuptr_rel(&cell->tagged_word, word);
}

static DWORD WINAPI lj_thr_windows_main(void *arg)
{
  LJThr *thr = (LJThr *)arg;
  thr->ret = thr->func(thr->arg);
  return 0;
}
#else
int lj_thr_tg_tls_init(void)
{
  return 1;
}

static uintptr_t lj_thr_tls_word_get(void)
{
  return la_loaduptr_acq(&lj_tls_tg_word);
}

static void lj_thr_tls_word_set(uintptr_t word)
{
  la_storeuptr_rel(&lj_tls_tg_word, word);
}
#endif

#if defined(LJ_THR_TLS_TEST_HELPERS)
void lj_thr_tls_test_set_word(uintptr_t word)
{
  if (!lj_thr_tg_tls_init())
    abort();
  lj_thr_tls_word_set(word);
}
#endif

static TGState *lj_thr_tls_get(void)
{
  return (TGState *)(lj_thr_tls_word_get() & ~LJ_THR_TG_TAG_MASK);
}

int lj_thr_create(LJThr *thr, LJThrFunc func, void *arg)
{
  if (!thr || !func)
    return EINVAL;
  if (thr->tid == 0)
    thr->tid = lj_thr_newid();
  if (thr->tid == 0 || thr->tid == LJ_THREAD_GCSCAN) {
    thr->tid = 0;
    return EAGAIN;
  }
#if LJ_TARGET_WINDOWS
  thr->func = func;
  thr->arg = arg;
  thr->ret = NULL;
  thr->handle = CreateThread(NULL, 0, lj_thr_windows_main, thr, 0,
			     &thr->sysid);  /* 09 section 9.3. */
  if (thr->handle == NULL) {
    thr->tid = 0;
    return EAGAIN;
  }
  return 0;
#else
  {
    int rc;
    rc = pthread_create(&thr->handle, NULL, func, arg);  /* 09 section 9.3. */
    if (rc != 0)
      thr->tid = 0;
    return rc;
  }
#endif
}

int lj_thr_join(LJThr *thr, void **ret)
{
  if (!thr)
    return EINVAL;
#if LJ_TARGET_WINDOWS
  if (WaitForSingleObject(thr->handle, INFINITE) != WAIT_OBJECT_0)
    return EINVAL;
  if (ret)
    *ret = thr->ret;
  CloseHandle(thr->handle);
  thr->handle = NULL;
  return 0;
#else
  return pthread_join(thr->handle, ret);  /* 09 section 9.4 substrate. */
#endif
}

uint32_t lj_thr_id(const LJThr *thr)
{
  return thr ? thr->tid : 0;
}

uint32_t lj_thr_current_id(global_State *g)
{
  TGState *tg = lj_thr_get_tg_fallback(g);
  return tg ? lj_tg_tid_acq(tg) : 0;
}

uint64_t lj_thr_now_ns(void)
{
#if LJ_TARGET_WINDOWS
  LARGE_INTEGER freq, ctr;
  if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&ctr) ||
      freq.QuadPart <= 0)
    return 0;
  return (uint64_t)ctr.QuadPart / (uint64_t)freq.QuadPart * 1000000000ull +
    (uint64_t)ctr.QuadPart % (uint64_t)freq.QuadPart * 1000000000ull /
    (uint64_t)freq.QuadPart;
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

typedef enum LJThrTGMode {
  LJ_THR_TG_MODE_EMPTY,
  LJ_THR_TG_MODE_RAW,
  LJ_THR_TG_MODE_KEYED,
  LJ_THR_TG_MODE_CORRUPT
} LJThrTGMode;

typedef struct LJThrTGView {
  TGState *body;
  LJTGRegistryKey key;
} LJThrTGView;

static int lj_thr_tg_borrow_empty(const LJTGRegistryBorrow *hold)
{
  return hold && !hold->active && hold->body == NULL &&
    hold->key.slot == NULL &&
    hold->key.incarnation == LJ_TGSLOT_INCARNATION_NONE;
}

static LJThrTGMode lj_thr_tg_view(LJThrTGView *view)
{
  uintptr_t word = lj_thr_tls_word_get();
  if (!view)
    return LJ_THR_TG_MODE_CORRUPT;
  view->body = NULL;
  view->key.slot = NULL;
  view->key.incarnation = LJ_TGSLOT_INCARNATION_NONE;
  if (word == 0)
    return LJ_THR_TG_MODE_EMPTY;
  view->body = (TGState *)(word & ~LJ_THR_TG_TAG_MASK);
  if ((word & LJ_THR_TG_EXACT_TAG) == 0)
    return LJ_THR_TG_MODE_RAW;
  if (!view->body ||
      (uintptr_t)view->body % (uintptr_t)__alignof__(TGState) != 0)
    return LJ_THR_TG_MODE_CORRUPT;
  /* The tag is ownership evidence for one ordinary lease. That lease keeps
  ** this body and its immutable embedded key alive while we reconstruct it. */
  view->key = view->body->registry_key;
  return lj_tgregistry_key_valid(&view->key) ? LJ_THR_TG_MODE_KEYED :
                                               LJ_THR_TG_MODE_CORRUPT;
}

static LJThrTGResult
lj_thr_tg_validate_view(const LJThrTGView *view, int new_target)
{
  LJTGRegistryBodySnap body;
  LJTGRegistrySlot *slot, *slow, *fast;
  LJTGSlotSnap snap;
  LJTGSlotResult status;
  TGState *tg;
  global_State *g;
  if (!view || !view->body ||
      (uintptr_t)view->body % (uintptr_t)__alignof__(TGState) != 0 ||
      !lj_tgregistry_key_valid(&view->key))
    return LJ_THR_TG_INVALID;
  snap.incarnation = LJ_TGSLOT_INCARNATION_NONE;
  snap.lease_count = 0;
  snap.state = LJ_TGSLOT_EMPTY;
  status = lj_tgregistry_key_snapshot(&view->key, &snap);
  if (!((status == LJ_TGSLOT_OK &&
         (snap.state == LJ_TGSLOT_ATTACHING ||
          snap.state == LJ_TGSLOT_LIVE ||
          snap.state == LJ_TGSLOT_DETACHING ||
          snap.state == LJ_TGSLOT_RETIRED)) ||
        (!new_target && status == LJ_TGSLOT_PINNED_RESULT &&
         snap.state == LJ_TGSLOT_PINNED)) ||
      snap.lease_count < 2u)
    return LJ_THR_TG_CORRUPT;
  if (new_target && snap.state != LJ_TGSLOT_ATTACHING &&
      snap.state != LJ_TGSLOT_LIVE)
    return LJ_THR_TG_CORRUPT;
  body = lj_tgregistry_slot_body_snapshot(view->key.slot);
  if (!lj_tgregistry_body_snap_is(&body, view->body,
                                  view->key.incarnation))
    return LJ_THR_TG_CORRUPT;
  tg = view->body;
  if (!lj_tgregistry_key_equal(&tg->registry_key, &view->key) || !tg->gl)
    return LJ_THR_TG_CORRUPT;
  /* The caller retains the universe lifetime while this dereferences tg->gl.
  ** Floyd detection rejects a corrupt cycle without imposing a size limit. */
  g = tg->gl;
  slot = gc2_tg_registry_head_acq(g);
  slow = fast = slot;
  while (slot) {
    if (slot == view->key.slot)
      return LJ_THR_TG_OK;
    slot = lj_tgregistry_slot_next_all(slot);
    slow = lj_tgregistry_slot_next_all(slow);
    fast = lj_tgregistry_slot_next_all(fast);
    if (fast)
      fast = lj_tgregistry_slot_next_all(fast);
    if (slow && slow == fast)
      return LJ_THR_TG_CORRUPT;
  }
  return LJ_THR_TG_CORRUPT;
}

static void lj_thr_tg_move_out(LJTGRegistryBorrow *hold,
                               const LJThrTGView *view)
{
  hold->key = view->key;
  hold->body = view->body;
  hold->active = 1;
}

LJThrTGResult lj_thr_tg_install(LJTGRegistryBorrow *new_hold)
{
  LJThrTGView current, incoming;
  LJThrTGMode mode;
  LJThrTGResult result;
  uintptr_t word;
  if (!new_hold || !new_hold->active || !new_hold->body ||
      !lj_tgregistry_key_valid(&new_hold->key) ||
      ((uintptr_t)new_hold->body & LJ_THR_TG_TAG_MASK) != 0)
    return LJ_THR_TG_INVALID;
  if (!lj_thr_tg_tls_init())
    return LJ_THR_TG_TLS_FAILURE;
  mode = lj_thr_tg_view(&current);
  if (mode == LJ_THR_TG_MODE_CORRUPT)
    return LJ_THR_TG_CORRUPT;
  if (mode != LJ_THR_TG_MODE_EMPTY)
    return LJ_THR_TG_EXPECT_MISMATCH;
  incoming.body = (TGState *)new_hold->body;
  incoming.key = new_hold->key;
  result = lj_thr_tg_validate_view(&incoming, 1);
  if (result != LJ_THR_TG_OK)
    return result;
  word = (uintptr_t)incoming.body | LJ_THR_TG_EXACT_TAG;
  lj_thr_tls_word_set(word);
  lj_tgregistry_borrow_init(new_hold);
  return LJ_THR_TG_OK;
}

LJThrTGResult lj_thr_tg_swap(const LJTGRegistryKey *expected_old,
                             LJTGRegistryBorrow *new_hold,
                             LJTGRegistryBorrow *old_hold)
{
  LJThrTGView current, incoming;
  LJThrTGMode mode;
  LJThrTGResult result;
  uintptr_t word;
  if (!lj_tgregistry_key_valid(expected_old) || !new_hold ||
      !new_hold->active || !new_hold->body || old_hold == new_hold ||
      !lj_thr_tg_borrow_empty(old_hold) ||
      ((uintptr_t)new_hold->body & LJ_THR_TG_TAG_MASK) != 0)
    return LJ_THR_TG_INVALID;
  mode = lj_thr_tg_view(&current);
  if (mode == LJ_THR_TG_MODE_CORRUPT)
    return LJ_THR_TG_CORRUPT;
  if (mode != LJ_THR_TG_MODE_KEYED ||
      !lj_tgregistry_key_equal(&current.key, expected_old))
    return LJ_THR_TG_EXPECT_MISMATCH;
  result = lj_thr_tg_validate_view(&current, 0);
  if (result != LJ_THR_TG_OK)
    return result;
  incoming.body = (TGState *)new_hold->body;
  incoming.key = new_hold->key;
  result = lj_thr_tg_validate_view(&incoming, 1);
  if (result != LJ_THR_TG_OK)
    return result;
  word = (uintptr_t)incoming.body | LJ_THR_TG_EXACT_TAG;
  lj_thr_tls_word_set(word);
  /* Both fungible token counts protect their exact bodies at this LP. */
  lj_thr_tg_move_out(old_hold, &current);
  lj_tgregistry_borrow_init(new_hold);
  return LJ_THR_TG_OK;
}

LJThrTGResult lj_thr_tg_clear(const LJTGRegistryKey *expected_old,
                              LJTGRegistryBorrow *old_hold)
{
  LJThrTGView current;
  LJThrTGMode mode;
  LJThrTGResult result;
  if (!lj_tgregistry_key_valid(expected_old) ||
      !lj_thr_tg_borrow_empty(old_hold))
    return LJ_THR_TG_INVALID;
  mode = lj_thr_tg_view(&current);
  if (mode == LJ_THR_TG_MODE_CORRUPT)
    return LJ_THR_TG_CORRUPT;
  if (mode != LJ_THR_TG_MODE_KEYED ||
      !lj_tgregistry_key_equal(&current.key, expected_old))
    return LJ_THR_TG_EXPECT_MISMATCH;
  result = lj_thr_tg_validate_view(&current, 0);
  if (result != LJ_THR_TG_OK)
    return result;
  lj_thr_tls_word_set(0);
  /* The lease count remains owned by this operation until materialized. */
  lj_thr_tg_move_out(old_hold, &current);
  return LJ_THR_TG_OK;
}

int lj_thr_tg_current_key(LJTGRegistryKey *key)
{
  LJThrTGView current;
  LJThrTGMode mode;
  LJThrTGResult result;
  if (!key)
    return LJ_THR_TG_INVALID;
  key->slot = NULL;
  key->incarnation = LJ_TGSLOT_INCARNATION_NONE;
  mode = lj_thr_tg_view(&current);
  if (mode == LJ_THR_TG_MODE_EMPTY || mode == LJ_THR_TG_MODE_RAW)
    return LJ_THR_TG_EXPECT_MISMATCH;
  if (mode == LJ_THR_TG_MODE_CORRUPT)
    return LJ_THR_TG_CORRUPT;
  result = lj_thr_tg_validate_view(&current, 0);
  if (result != LJ_THR_TG_OK)
    return result;
  *key = current.key;
  return LJ_THR_TG_OK;
}

static void lj_thr_tls_set_raw(TGState *tg)
{
  uintptr_t word;
  if (tg && ((uintptr_t)tg & LJ_THR_TG_TAG_MASK) != 0)
    abort();
  if (!lj_thr_tg_tls_init())
    abort();  /* Void raw callers cannot safely report per-thread admission. */
  word = lj_thr_tls_word_get();
  if ((word & LJ_THR_TG_EXACT_TAG) != 0)
    abort();  /* A void raw call cannot move the exact lease represented here. */
  lj_thr_tls_word_set((uintptr_t)tg);
}

void lj_thr_set_tg(TGState *tg)
{
  lj_thr_tls_set_raw(tg);  /* Raw compatibility until lifecycle migration. */
}

TGState *lj_thr_get_tg(void)
{
  return lj_thr_tls_get();
}

TGState *lj_thr_get_tg_fallback(global_State *g)
{
  TGState *tg = lj_thr_tls_get();
  if (!g)
    return tg;
  return tg && tg->gl == g ? tg : g->main_tg;
}

static void state_gcscan_wait_no_l(void)
{
  (void)lj_thr_retry_yield(NULL);
}

int lj_state_claim(lua_State *L, uint32_t tid)
{
  uint32_t owner;
  if (!L || tid == 0 || tid == LJ_THREAD_GCSCAN)
    return 0;
  for (;;) {
    owner = lj_state_owner_acq(L);
    if (owner == tid)
      return 1;
    if (owner == 0) {
      uint32_t expect = 0;
      if (lj_state_owner_cas(L, &expect, tid))
	return 1;
      continue;
    }
    if (owner == LJ_THREAD_GCSCAN) {
      state_gcscan_wait_no_l();
      continue;
    }
    return 0;
  }
}

int lj_state_tryclaim(lua_State *L, uint32_t tid, LJStateClaim *claim)
{
  uint32_t owner;
  if (claim) {
    claim->L = NULL;
    claim->tg_hint = NULL;
    claim->tid = 0;
    claim->release = 0;
  }
  if (!L || tid == 0 || tid == LJ_THREAD_GCSCAN)
    return 0;
  for (;;) {
    owner = lj_state_owner_acq(L);
    if (owner == tid) {
      if (claim) {
	claim->L = L;
	claim->tid = tid;
      }
      return 1;
    }
    if (owner == 0) {
      uint32_t expect = 0;
      if (lj_state_owner_cas(L, &expect, tid)) {
	if (claim) {
	  claim->L = L;
	  claim->tg_hint = NULL;
	  claim->tid = tid;
	  claim->release = 1;
	}
	return 1;
      }
      continue;
    }
    if (owner == LJ_THREAD_GCSCAN) {
      state_gcscan_wait_no_l();
      continue;
    }
    return 0;
  }
}

int lj_state_resumeclaim(lua_State *L, uint32_t tid, LJStateClaim *claim)
{
  if (!lj_state_tryclaim(L, tid, claim))
    return 0;
  /*
  ** Suspended coroutines are TG-neutral. A resume claim makes the coroutine
  ** temporarily run on the resumer's TG, then restores the previous hint
  ** before publishing the stack as unowned. The previous hint is usually NULL,
  ** but VM-event/callback states may already be attached while ownerless.
  */
  if (claim && claim->release) {
    claim->tg_hint = L->tg_hint;
    L->tg_hint = lj_thr_get_tg_fallback(G(L));
  }
  return 1;
}

int lj_state_gcscan_claim(lua_State *L, LJStateClaim *claim)
{
  uint32_t owner;
  if (claim) {
    claim->L = NULL;
    claim->tg_hint = NULL;
    claim->tid = 0;
    claim->release = 0;
  }
  if (!L)
    return 0;
  for (;;) {
    owner = lj_state_owner_acq(L);
    if (owner == 0) {
      uint32_t expect = 0;
      if (lj_state_owner_cas(L, &expect, LJ_THREAD_GCSCAN)) {
	if (claim) {
	  claim->L = L;
	  claim->tg_hint = NULL;
	  claim->tid = LJ_THREAD_GCSCAN;
	  claim->release = 1;
	}
	return 1;
      }
      continue;
    }
    if (owner == LJ_THREAD_GCSCAN) {
      /*
      ** The scan sentinel intentionally carries no owner id. A GC scanner can
      ** rediscover the same lua_State through another grey edge while it owns
      ** that sentinel, so waiting here can self-deadlock. Report the state as
      ** busy; GC2 will requeue it or let the owning thread satisfy NEEDSCAN.
      */
      return 0;  /* 05 section 5.7.2: scan claim handoff. */
    }
    return 0;
  }
}

static void state_stack_dirty(lua_State *L, uint32_t tid)
{
  TGState *tg;
  if (!L || tid == 0 || tid == LJ_THREAD_GCSCAN)
    return;
  tg = lj_tg_find_owner(G(L), tid);
  if (tg)
    lj_tg_stack_dirty_epoch_add_rlx(tg, 1);
}

void lj_state_dropclaim(LJStateClaim *claim)
{
  if (claim && claim->release) {
    lj_state_release(claim->L, claim->tid);
    claim->release = 0;
  }
}

void lj_state_resume_release(lua_State *L, uint32_t tid)
{
  if (L && tid != 0) {
    L->tg_hint = NULL;
    lj_state_release(L, tid);
  }
}

void lj_state_dropresumeclaim(LJStateClaim *claim)
{
  if (claim && claim->release) {
    claim->L->tg_hint = claim->tg_hint;
    lj_state_release(claim->L, claim->tid);
    claim->release = 0;
  }
}

uint32_t lj_state_owner_wait(lua_State *L, lua_State *target, uint32_t owner,
			     int64_t ns)
{
  TGState *tg = L ? L2TG(L) : lj_thr_tls_get();
  uint32_t actions = 0;
  if (!target || owner == 0)
    return 0;
  /*
  ** State-owner waits are native waits, not VM stalls. Mark the TG native so
  ** safepoint/STOPREQ handshakes can observe it while the futex wait blocks.
  */
  if (tg)
    lj_native_enter(tg);
  lj_state_owner_futex_wait(target, owner, ns);
  if (L) {
    actions = lj_native_leave(L);
  } else if (tg) {
    actions = lj_native_leave_tg(tg);
  }
  return actions;
}

void lj_state_release(lua_State *L, uint32_t tid)
{
  if (L && tid != 0) {
    uint32_t owner = lj_state_owner_acq(L);
    lj_assertX(owner == tid, "lua_State owner mismatch");
    UNUSED(owner);
    state_stack_dirty(L, tid);
    lj_state_owner_rel(L, 0);
    lj_state_owner_futex_wake(L, 0x7fffffff);
  }
}

uint32_t lj_thr_cpucount(void)
{
#if LJ_TARGET_WINDOWS
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors ? (uint32_t)si.dwNumberOfProcessors : 1u;
#else
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (uint32_t)n : 1u;
#endif
}

void lj_thr_fence(void)
{
  la_fence_seq();  /* 09 section 9.1 threading.fence memory edge. */
}

uint32_t lj_thr_yield(lua_State *L)
{
  TGState *tg = L ? L2TG(L) : lj_thr_tls_get();
  uint32_t actions = 0;
  if (tg)
    lj_native_enter(tg);
#if LJ_TARGET_WINDOWS
  if (!SwitchToThread())
    Sleep(0);
#else
  (void)sched_yield();
#endif
  if (L) {
    actions = lj_native_leave(L);
  } else if (tg) {
    actions = lj_native_leave_tg(tg);
  }
  return actions;
}

uint32_t lj_thr_retry_yield(lua_State *L)
{
  uint32_t i;
  for (i = 0; i < 64; i++)
    la_cpu_pause();
  return lj_thr_yield(L);
}

uint32_t lj_thr_sleep_ns(lua_State *L, int64_t ns)
{
  TGState *tg = L ? L2TG(L) : lj_thr_tls_get();
  uint32_t actions = 0;
  if (tg)
    lj_native_enter(tg);  /* 09 section 9.1 sleep is a native region. */
  if (ns > 0) {
#if LJ_TARGET_WINDOWS
    uint64_t ms = ((uint64_t)ns + 999999u) / 1000000u;
    if (ms == 0)
      ms = 1;
    Sleep(ms >= INFINITE ? INFINITE - 1u : (DWORD)ms);
#else
    struct timespec req;
    req.tv_sec = (time_t)(ns / 1000000000ll);
    req.tv_nsec = (long)(ns % 1000000000ll);
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
      ;
#endif
  }
  if (L) {
    actions = lj_native_leave(L);
  } else if (tg) {
    actions = lj_native_leave_tg(tg);  /* TG-private owner poll, no Lua stack. */
  }
  return actions;
}
