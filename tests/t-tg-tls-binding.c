/*
** Exact TG-registry TLS binding ownership and ABA regression.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <signal.h>
#endif

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_thr.h"
#include "lj_tg.h"

typedef struct BindingThreadCtx {
  LJTGRegistryKey key;
  LJTGRegistryBorrow hold;
  TGState *body;
  uint32_t ready;
  uint32_t go;
  uint32_t done;
} BindingThreadCtx;

#if LJ_TARGET_WINDOWS && defined(LJ_THR_TLS_TEST_HELPERS)
typedef struct AdmissionFailureThreadCtx {
  LJTGRegistryBorrow hold;
  LJThrTGResult result;
  TGState *observed;
} AdmissionFailureThreadCtx;
#endif

#if !LJ_TARGET_WINDOWS
static uintptr_t sampled_tls_body;

static void sample_handler(int signo)
{
  (void)signo;
  la_storeuptr_rlx(&sampled_tls_body, (uintptr_t)lj_thr_get_tg());
}

static void sample_tls(TGState *expected)
{
  la_storeuptr_rlx(&sampled_tls_body, ~(uintptr_t)0);
  assert(raise(SIGPROF) == 0);
  assert((TGState *)la_loaduptr_rlx(&sampled_tls_body) == expected);
}
#else
#define sample_tls(expected) ((void)(expected))
#endif

static uint64_t lease_count(const LJTGRegistryKey *key, uint8_t state)
{
  LJTGSlotSnap snap;
  assert(lj_tgregistry_key_snapshot(key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == state);
  return snap.lease_count;
}

static void release_borrow(LJTGRegistryBorrow *hold)
{
  assert(hold->active);
  assert(lj_tgregistry_release_to_completion(hold, NULL) == LJ_TGSLOT_OK);
  assert(!hold->active);
}

static void *binding_thread(void *arg)
{
  BindingThreadCtx *ctx = (BindingThreadCtx *)arg;
  LJTGRegistryBorrow old;
  LJTGRegistryKey key;
  lj_tgregistry_borrow_init(&old);
  assert(lj_thr_tg_install(&ctx->hold) == LJ_THR_TG_OK);
  assert(!ctx->hold.active && lj_thr_get_tg() == ctx->body);
  assert(lj_thr_tg_current_key(&key) == LJ_THR_TG_OK);
  assert(lj_tgregistry_key_equal(&key, &ctx->key));
  la_store32_rel(&ctx->ready, 1);
  while (la_load32_acq(&ctx->go) == 0)
    (void)lj_thr_yield(NULL);
  assert(lj_thr_get_tg() == ctx->body);
  assert(lj_thr_tg_clear(&ctx->key, &old) == LJ_THR_TG_OK);
  assert(lj_thr_get_tg() == NULL && old.active && old.body == ctx->body);
  release_borrow(&old);
  la_store32_rel(&ctx->done, 1);
  return ctx->body;
}

#if LJ_TARGET_WINDOWS && defined(LJ_THR_TLS_TEST_HELPERS)
static void *admission_failure_thread(void *arg)
{
  AdmissionFailureThreadCtx *ctx = (AdmissionFailureThreadCtx *)arg;
  ctx->result = lj_thr_tg_install(&ctx->hold);
  ctx->observed = lj_thr_get_tg();
  return NULL;
}
#endif

static LJTGRegistryKey publish_body(global_State *g, TGState *tg,
                                    LJTGRegistrySlot *slot, int link)
{
  LJTGRegistryKey key;
  LJTGSlotSnap snap;
  LJTGRegistrySlot *head;
  if (link)
    assert(lj_tgregistry_slot_init_unpublished(slot, 0, NULL));
  assert(lj_tgregistry_try_claim(slot, &key, &snap) == LJ_TGSLOT_OK);
  memset(tg, 0, sizeof(*tg));
  tg->gl = g;
  tg->registry_key = key;
  assert(lj_tgregistry_try_publish_body(&key, tg, &snap) == LJ_TGSLOT_OK);
  if (link) {
    head = gc2_tg_registry_head_acq(g);
    slot->next_all = head;
    assert(gc2_tg_registry_head_cas(g, &head, slot));
    (void)gc2_tg_registry_nodes_add(g, 1);
  }
  assert(lj_tgregistry_try_publish(&key, &snap) == LJ_TGSLOT_OK);
  assert(lease_count(&key, LJ_TGSLOT_LIVE) == 1u);
  return key;
}

static LJTGRegistryKey publish_opaque_body(void *body,
                                           LJTGRegistrySlot *slot)
{
  LJTGRegistryKey key;
  LJTGSlotSnap snap;
  assert(body && slot);
  assert(lj_tgregistry_try_claim(slot, &key, &snap) == LJ_TGSLOT_OK);
  assert(lj_tgregistry_try_publish_body(&key, body, &snap) == LJ_TGSLOT_OK);
  assert(lj_tgregistry_try_publish(&key, &snap) == LJ_TGSLOT_OK);
  assert(lease_count(&key, LJ_TGSLOT_LIVE) == 1u);
  return key;
}

static void retire_body(const LJTGRegistryKey *key)
{
  LJTGSlotSnap snap;
  assert(lj_tgregistry_try_detach(key, &snap) == LJ_TGSLOT_OK);
  assert(lj_tgregistry_try_retire(key, &snap) == LJ_TGSLOT_OK);
}

static void reclaim_body(const LJTGRegistryKey *key, void *expected_body)
{
  LJTGSlotSnap snap;
  void *body = NULL;
  assert(lj_tgregistry_try_reclaim(key, &body, &snap) == LJ_TGSLOT_OK);
  assert(body == expected_body);
  assert(lj_tgregistry_try_clear(key, &snap) == LJ_TGSLOT_OK);
  assert(lease_count(key, LJ_TGSLOT_EMPTY) == 0u);
}

int main(void)
{
  global_State *g = (global_State *)calloc(1, sizeof(*g));
  global_State *wrong_g = (global_State *)calloc(1, sizeof(*wrong_g));
  TGState *a = (TGState *)calloc(1, sizeof(*a));
  TGState *b = (TGState *)calloc(1, sizeof(*b));
  LJTGRegistrySlot *aslot = (LJTGRegistrySlot *)malloc(sizeof(*aslot));
  LJTGRegistrySlot *bslot = (LJTGRegistrySlot *)malloc(sizeof(*bslot));
  LJTGRegistryKey akey, bkey, miskey, stale, key;
  LJTGRegistryBorrow ahold, bhold, old;
  BindingThreadCtx actx, bctx;
  LJThr athr = {0}, bthr = {0};
  LJTGSlotSnap snap, pinned;
  la_u128 saved_token;
  void *aret = NULL, *bret = NULL;
  void *body = NULL;
  void *mis_storage = NULL, *mis_body;
#if LJ_TARGET_WINDOWS && defined(LJ_THR_TLS_TEST_HELPERS)
  AdmissionFailureThreadCtx alloc_fail, publish_fail;
  LJThr alloc_fail_thr = {0}, publish_fail_thr = {0};
  void *failure_ret = (void *)(uintptr_t)1u;
#endif

  assert(g && wrong_g && a && b && aslot && bslot);
#if LJ_TARGET_WINDOWS && defined(LJ_THR_TLS_TEST_HELPERS)
  /* All three first-admission failures report NULL before universe
  ** publication. Index failure leaves InitOnce retryable; unpublished cells
  ** are freed, and the hot getter neither initializes nor allocates. */
  lj_thr_tls_test_fail_index_alloc(1);
  assert(luaL_newstate() == NULL);
  assert(lj_thr_get_tg() == NULL);
  lj_thr_tls_test_fail_cell_alloc(1);
  assert(luaL_newstate() == NULL);
  assert(lj_thr_get_tg() == NULL);
  lj_thr_tls_test_fail_cell_publish(1);
  assert(luaL_newstate() == NULL);
  assert(lj_thr_get_tg() == NULL);
#endif
  assert(lj_thr_tg_tls_init());
  lj_thr_set_tg(NULL);
#if !LJ_TARGET_WINDOWS
  assert(signal(SIGPROF, sample_handler) != SIG_ERR);
#endif
  akey = publish_body(g, a, aslot, 1);
  bkey = publish_body(g, b, bslot, 1);

#if LJ_TARGET_WINDOWS && defined(LJ_THR_TLS_TEST_HELPERS)
  /* A new OS thread has no cell even though this thread and the process index
  ** are initialized. Cell allocation failure changes neither its hot binding
  ** nor the incoming exact handle. */
  memset(&alloc_fail, 0, sizeof(alloc_fail));
  lj_tgregistry_borrow_init(&alloc_fail.hold);
  assert(lj_tgregistry_try_borrow(&akey, &alloc_fail.hold, &snap) ==
         LJ_TGSLOT_OK);
  lj_thr_tls_test_fail_cell_alloc(1);
  assert(lj_thr_create(&alloc_fail_thr, admission_failure_thread,
                       &alloc_fail) == 0);
  assert(lj_thr_join(&alloc_fail_thr, &failure_ret) == 0);
  assert(failure_ret == NULL && alloc_fail.result == LJ_THR_TG_TLS_FAILURE);
  assert(alloc_fail.hold.active && alloc_fail.observed == NULL);
  release_borrow(&alloc_fail.hold);
#endif

  /* Distinct OS threads own distinct tagged bindings and token counts. */
  memset(&actx, 0, sizeof(actx));
  memset(&bctx, 0, sizeof(bctx));
  actx.key = akey;
  actx.body = a;
  bctx.key = bkey;
  bctx.body = b;
  lj_tgregistry_borrow_init(&actx.hold);
  lj_tgregistry_borrow_init(&bctx.hold);
  assert(lj_tgregistry_try_borrow(&akey, &actx.hold, &snap) ==
         LJ_TGSLOT_OK);
  assert(lj_tgregistry_try_borrow(&bkey, &bctx.hold, &snap) ==
         LJ_TGSLOT_OK);
  assert(lj_thr_create(&athr, binding_thread, &actx) == 0);
  assert(lj_thr_create(&bthr, binding_thread, &bctx) == 0);
  while (!la_load32_acq(&actx.ready) || !la_load32_acq(&bctx.ready))
    (void)lj_thr_yield(NULL);
  assert(lease_count(&akey, LJ_TGSLOT_LIVE) == 2u);
  assert(lease_count(&bkey, LJ_TGSLOT_LIVE) == 2u);
  la_store32_rel(&actx.go, 1);
  la_store32_rel(&bctx.go, 1);
  assert(lj_thr_join(&athr, &aret) == 0);
  assert(lj_thr_join(&bthr, &bret) == 0);
  assert(aret == a && bret == b && actx.done && bctx.done);
  assert(lease_count(&akey, LJ_TGSLOT_LIVE) == 1u);
  assert(lease_count(&bkey, LJ_TGSLOT_LIVE) == 1u);

  lj_tgregistry_borrow_init(&ahold);
  lj_tgregistry_borrow_init(&bhold);
  lj_tgregistry_borrow_init(&old);
  assert(lj_tgregistry_try_borrow(&akey, &ahold, &snap) == LJ_TGSLOT_OK);
  assert(lease_count(&akey, LJ_TGSLOT_LIVE) == 2u);

#if defined(LJ_THR_TLS_TEST_HELPERS)
  /* The reserved tag-only word is neither empty nor a recoverable binding. */
  lj_thr_tls_test_set_word((uintptr_t)1u);
  assert(lj_thr_get_tg() == NULL);
  assert(lj_thr_tg_current_key(&key) == LJ_THR_TG_CORRUPT);
  assert(lj_thr_tg_install(&ahold) == LJ_THR_TG_CORRUPT);
  assert(lj_thr_tg_clear(&akey, &old) == LJ_THR_TG_CORRUPT);
  assert(ahold.active && !old.active);
  lj_thr_tls_test_set_word(0);
#endif

  /* Raw mode is source-compatible but cannot be mistaken for a keyed lease. */
  lj_thr_set_tg(a);
  assert(lj_thr_get_tg() == a);
  assert(lj_thr_tg_current_key(&key) == LJ_THR_TG_EXPECT_MISMATCH);
  assert(lj_thr_tg_install(&ahold) == LJ_THR_TG_EXPECT_MISMATCH);
  assert(ahold.active);
  lj_thr_set_tg(NULL);

  /* Reverse TG/key association is mandatory and failure consumes nothing. */
  a->registry_key = bkey;
  assert(lj_thr_tg_install(&ahold) == LJ_THR_TG_CORRUPT);
  assert(ahold.active && lj_thr_get_tg() == NULL);
  a->registry_key = akey;
  a->gl = wrong_g;
  assert(lj_thr_tg_install(&ahold) == LJ_THR_TG_CORRUPT);
  assert(ahold.active && lj_thr_get_tg() == NULL);
  a->gl = g;

#if LJ_TARGET_WINDOWS && defined(LJ_THR_TLS_TEST_HELPERS)
  /* Keep a first-cell publication failure armed across this initialized
  ** thread's install, swap, and clear. A later fresh thread must consume it. */
  lj_thr_tls_test_fail_cell_publish(1);
#endif
  assert(lj_thr_tg_install(&ahold) == LJ_THR_TG_OK);
  assert(!ahold.active && lj_thr_get_tg() == a);
  sample_tls(a);
  assert(lj_thr_tg_current_key(&key) == LJ_THR_TG_OK);
  assert(lj_tgregistry_key_equal(&key, &akey));
  /* Malformed token components must fail closed without reading an
  ** uninitialized failed-snapshot output. Restore the exact test token before
  ** any lifecycle or release operation. */
  saved_token = akey.slot->token.value;
  akey.slot->token.value.hi = lj_tgslot_pack_hi(0, LJ_TGSLOT_LIVE);
  assert(lj_thr_tg_current_key(&key) == LJ_THR_TG_CORRUPT);
  akey.slot->token.value = saved_token;
  assert(lease_count(&akey, LJ_TGSLOT_LIVE) == 2u);
  assert(lj_thr_tg_clear(&bkey, &old) == LJ_THR_TG_EXPECT_MISMATCH);
  assert(!old.active && lj_thr_get_tg() == a);

  assert(lj_tgregistry_try_borrow(&bkey, &bhold, &snap) == LJ_TGSLOT_OK);
  assert(lj_thr_tg_swap(&akey, &bhold, &old) == LJ_THR_TG_OK);
  assert(!bhold.active && old.active && old.body == a);
  assert(lj_thr_get_tg() == b);
  sample_tls(b);
  assert(lease_count(&akey, LJ_TGSLOT_LIVE) == 2u);
  assert(lease_count(&bkey, LJ_TGSLOT_LIVE) == 2u);
  release_borrow(&old);
  assert(lease_count(&akey, LJ_TGSLOT_LIVE) == 1u);

  /* Clear publishes hot NULL before returning the still-held local lease. */
  retire_body(&bkey);
  assert(lj_tgregistry_try_reclaim(&bkey, &body, &snap) == LJ_TGSLOT_BUSY);
  assert(lj_thr_tg_clear(&bkey, &old) == LJ_THR_TG_OK);
  assert(lj_thr_get_tg() == NULL && old.active && old.body == b);
  sample_tls(NULL);
  assert(lease_count(&bkey, LJ_TGSLOT_RETIRED) == 2u);
  assert(lj_tgregistry_try_reclaim(&bkey, &body, &snap) == LJ_TGSLOT_BUSY);
  release_borrow(&old);
  reclaim_body(&bkey, b);

#if LJ_TARGET_WINDOWS && defined(LJ_THR_TLS_TEST_HELPERS)
  /* Existing-cell mutations above did not call TlsSetValue: the armed hook is
  ** still pending and fails this fresh thread's first cell publication. */
  memset(&publish_fail, 0, sizeof(publish_fail));
  lj_tgregistry_borrow_init(&publish_fail.hold);
  assert(lj_tgregistry_try_borrow(&akey, &publish_fail.hold, &snap) ==
         LJ_TGSLOT_OK);
  failure_ret = (void *)(uintptr_t)1u;
  assert(lj_thr_create(&publish_fail_thr, admission_failure_thread,
                       &publish_fail) == 0);
  assert(lj_thr_join(&publish_fail_thr, &failure_ret) == 0);
  assert(failure_ret == NULL && publish_fail.result == LJ_THR_TG_TLS_FAILURE);
  assert(publish_fail.hold.active && publish_fail.observed == NULL);
  release_borrow(&publish_fail.hold);
#endif

  retire_body(&akey);
  reclaim_body(&akey, a);
  stale = akey;

  /* Reuse the same stable slot and body address under a new incarnation. */
  akey = publish_body(g, a, aslot, 0);
  assert(akey.incarnation != stale.incarnation && akey.slot == stale.slot);
  assert(lj_tgregistry_try_borrow(&akey, &ahold, &snap) == LJ_TGSLOT_OK);
  assert(lj_thr_tg_install(&ahold) == LJ_THR_TG_OK);
  assert(lj_thr_tg_clear(&stale, &old) == LJ_THR_TG_EXPECT_MISMATCH);
  assert(!old.active && lj_thr_get_tg() == a);
  assert(lj_thr_tg_clear(&akey, &old) == LJ_THR_TG_OK);
  assert(old.active && old.key.incarnation == akey.incarnation);
  release_borrow(&old);
  retire_body(&akey);
  reclaim_body(&akey, a);

  /* An even but under-aligned opaque registry body is rejected before any
  ** TGState dereference, then remains ordinarily releasable/reclaimable. */
  assert(__alignof__(TGState) > 2u);
  mis_storage = malloc(sizeof(TGState) + (size_t)__alignof__(TGState));
  assert(mis_storage);
  mis_body = (void *)((char *)mis_storage + 2);
  assert(((uintptr_t)mis_body & 1u) == 0 &&
         (uintptr_t)mis_body % (uintptr_t)__alignof__(TGState) != 0);
  miskey = publish_opaque_body(mis_body, bslot);
  assert(lj_tgregistry_try_borrow(&miskey, &bhold, &snap) == LJ_TGSLOT_OK);
  assert(lj_thr_tg_install(&bhold) == LJ_THR_TG_INVALID);
  assert(bhold.active && lj_thr_get_tg() == NULL);
  release_borrow(&bhold);
  retire_body(&miskey);
  reclaim_body(&miskey, mis_body);
  free(mis_storage);

  /* PINNED is absorbing for reclamation, but a valid current binding can be
  ** cleared and return its exact count without leaving TLS stuck. */
  bkey = publish_body(g, b, bslot, 0);
  assert(lj_tgregistry_try_borrow(&bkey, &bhold, &snap) == LJ_TGSLOT_OK);
  assert(lj_thr_tg_install(&bhold) == LJ_THR_TG_OK);
  assert(lj_tgregistry_key_snapshot(&bkey, &snap) == LJ_TGSLOT_OK);
  assert(lj_tgslot_try_pin(&bkey.slot->token, &snap, &pinned) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(pinned.state == LJ_TGSLOT_PINNED && pinned.lease_count == 2u);
  assert(lj_thr_tg_current_key(&key) == LJ_THR_TG_OK);
  assert(lj_tgregistry_key_equal(&key, &bkey));
  assert(lj_thr_tg_clear(&bkey, &old) == LJ_THR_TG_OK);
  assert(old.active && old.body == b && lj_thr_get_tg() == NULL);
  release_borrow(&old);
  assert(lj_tgregistry_key_snapshot(&bkey, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(snap.state == LJ_TGSLOT_PINNED && snap.lease_count == 1u);
  body = (void *)(uintptr_t)1u;
  assert(lj_tgregistry_try_reclaim(&bkey, &body, &snap) ==
         LJ_TGSLOT_PINNED_RESULT);
  assert(body == NULL);

  free(bslot);
  free(aslot);
  free(b);
  free(a);
  free(wrong_g);
  free(g);
  puts("t-tg-tls-binding OK: exact TLS handle moves and ABA safety verified");
  return 0;
}
