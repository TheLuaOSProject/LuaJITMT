/*
** Exact TG-registry TLS binding ownership and ABA regression.
*/

#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_gc2.h"
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

typedef struct GC2TLSIsolationCtx {
  global_State *g;
  uint32_t *ready;
  uint32_t *go;
} GC2TLSIsolationCtx;

#if LJ_TARGET_WINDOWS && defined(LJ_THR_TLS_TEST_HELPERS)
#define LJ_TLS_INIT_RACE_THREADS 8
#define LJ_TLS_RAW_ABORT_ARG "--raw-tls-admission-abort"
#define LJ_TLS_INIT_RACE_ARG "--concurrent-tls-index-retry"
#define LJ_TLS_RAW_ABORT_EXIT ((DWORD)0x4c4a4142u)  /* "LJAB". */
#define LJ_TLS_RAW_RETURN_EXIT ((DWORD)0x4c4a5254u)  /* "LJRT". */

typedef struct AdmissionFailureThreadCtx {
  LJTGRegistryBorrow hold;
  LJThrTGResult result;
  TGState *observed;
} AdmissionFailureThreadCtx;

typedef struct InitRaceThreadCtx {
  uint32_t *ready;
  uint32_t *go;
  uint32_t *failures;
  uint32_t *successes;
  int result;
} InitRaceThreadCtx;

typedef struct GC2TLSFallbackCtx {
  global_State *g;
  LJThrGC2TLS *before;
  LJThrGC2TLS *after;
  int read_result;
  int init_result;
} GC2TLSFallbackCtx;
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

/* This fixture synthesizes registry bodies without a lua_State or lifecycle
** owner. Stamp that deliberately incomplete model with the physical actor
** whose TLS transaction is under test. Production LIVE actor-zero bodies are
** paired handoffs and must never be consumed by the generic binder. */
static void fixture_adopt_unpaired_tg(TGState *tg)
{
  uint32_t actor = lj_thr_actor_ensure();
  uint32_t owner = 0;
  assert(actor != 0);
  assert(lj_tg_actor_cas(tg, &owner, actor) || owner == actor);
}

static void fixture_release_unpaired_tg(TGState *tg)
{
  uint32_t actor = lj_thr_actor_current();
  uint32_t owner = actor;
  assert(actor != 0 && lj_tg_actor_cas(tg, &owner, 0));
}

static void *binding_thread(void *arg)
{
  BindingThreadCtx *ctx = (BindingThreadCtx *)arg;
  LJTGRegistryBorrow old;
  LJTGRegistryKey key;
  lj_tgregistry_borrow_init(&old);
  fixture_adopt_unpaired_tg(ctx->body);
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
  fixture_release_unpaired_tg(ctx->body);
  la_store32_rel(&ctx->done, 1);
  return ctx->body;
}

static void *gc2_tls_isolation_thread(void *arg)
{
  GC2TLSIsolationCtx *ctx = (GC2TLSIsolationCtx *)arg;
  assert(lj_thr_tg_tls_init());
  assert(lj_gc2_smr_read_try(ctx->g));
  assert(lj_gc2_smr_read_try(ctx->g));
  (void)la_add32_acqrel(ctx->ready, 1);
  while (la_load32_acq(ctx->go) == 0)
    (void)lj_thr_yield(NULL);
  lj_gc2_smr_read_leave(ctx->g);
  lj_gc2_smr_read_leave(ctx->g);
  return NULL;
}

#if LJ_TARGET_WINDOWS && defined(LJ_THR_TLS_TEST_HELPERS)
static void *gc2_tls_fallback_thread(void *arg)
{
  GC2TLSFallbackCtx *ctx = (GC2TLSFallbackCtx *)arg;
  ctx->before = lj_thr_gc2_tls_current();
  ctx->read_result = lj_gc2_smr_read_try(ctx->g);
  ctx->after = lj_thr_gc2_tls_current();
  if (ctx->read_result)
    lj_gc2_smr_read_leave(ctx->g);
  /* The armed allocation failure must still be pending: SMR lookup and its
  ** fully-counted fallback are forbidden from admitting this thread. */
  ctx->init_result = lj_thr_tg_tls_init();
  return NULL;
}

static void *admission_failure_thread(void *arg)
{
  AdmissionFailureThreadCtx *ctx = (AdmissionFailureThreadCtx *)arg;
  ctx->result = lj_thr_tg_install(&ctx->hold);
  ctx->observed = lj_thr_get_tg();
  return NULL;
}

static void raw_admission_abort_handler(int signo)
{
  ExitProcess(signo == SIGABRT ? LJ_TLS_RAW_ABORT_EXIT :
                                LJ_TLS_RAW_RETURN_EXIT);
}

static void raw_admission_abort_child(void)
{
  TGState raw;
  memset(&raw, 0, sizeof(raw));
  assert(signal(SIGABRT, raw_admission_abort_handler) != SIG_ERR);
  lj_thr_tls_test_fail_cell_alloc(1);
  lj_thr_set_tg(&raw);
  ExitProcess(LJ_TLS_RAW_RETURN_EXIT);  /* Silent failure reached this point. */
}

static void *init_race_thread(void *arg)
{
  InitRaceThreadCtx *ctx = (InitRaceThreadCtx *)arg;
  (void)la_add32_acqrel(ctx->ready, 1);
  while (la_load32_acq(ctx->go) == 0)
    (void)lj_thr_yield(NULL);
  if (lj_thr_tg_tls_init()) {
    ctx->result = 1;
    (void)la_add32_acqrel(ctx->successes, 1);
    return NULL;
  }
  (void)la_add32_acqrel(ctx->failures, 1);
  /* Do not let the failed caller perform the process-index retry first. A
  ** concurrent waiter must complete InitOnce and its own cell admission. */
  while (la_load32_acq(ctx->successes) == 0)
    (void)lj_thr_yield(NULL);
  ctx->result = lj_thr_tg_tls_init();
  if (ctx->result)
    (void)la_add32_acqrel(ctx->successes, 1);
  return NULL;
}

static int concurrent_index_retry_child(void)
{
  InitRaceThreadCtx ctx[LJ_TLS_INIT_RACE_THREADS];
  LJThr thr[LJ_TLS_INIT_RACE_THREADS];
  uint32_t ready = 0, go = 0, failures = 0, successes = 0;
  unsigned int i;
  memset(ctx, 0, sizeof(ctx));
  memset(thr, 0, sizeof(thr));
  lj_thr_tls_test_fail_index_alloc(1);
  for (i = 0; i < LJ_TLS_INIT_RACE_THREADS; i++) {
    ctx[i].ready = &ready;
    ctx[i].go = &go;
    ctx[i].failures = &failures;
    ctx[i].successes = &successes;
    assert(lj_thr_create(&thr[i], init_race_thread, &ctx[i]) == 0);
  }
  while (la_load32_acq(&ready) != LJ_TLS_INIT_RACE_THREADS)
    (void)lj_thr_yield(NULL);
  la_store32_rel(&go, 1);
  for (i = 0; i < LJ_TLS_INIT_RACE_THREADS; i++) {
    assert(lj_thr_join(&thr[i], NULL) == 0);
    assert(ctx[i].result);
  }
  assert(la_load32_acq(&failures) == 1u);
  assert(la_load32_acq(&successes) == LJ_TLS_INIT_RACE_THREADS);
  return 0;
}

static DWORD run_windows_child(const char *arg)
{
  char image[MAX_PATH];
  char command[MAX_PATH + 64];
  STARTUPINFOA startup;
  PROCESS_INFORMATION process;
  DWORD code = STILL_ACTIVE;
  DWORD len;
  int command_len;
  memset(&startup, 0, sizeof(startup));
  memset(&process, 0, sizeof(process));
  startup.cb = sizeof(startup);
  len = GetModuleFileNameA(NULL, image, (DWORD)sizeof(image));
  assert(len > 0 && len < (DWORD)sizeof(image));
  command_len = snprintf(command, sizeof(command), "\"%s\" %s", image, arg);
  assert(command_len > 0 && (size_t)command_len < sizeof(command));
  assert(CreateProcessA(image, command, NULL, NULL, FALSE, 0, NULL, NULL,
                        &startup, &process));
  assert(WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0);
  assert(GetExitCodeProcess(process.hProcess, &code));
  assert(CloseHandle(process.hThread));
  assert(CloseHandle(process.hProcess));
  return code;
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

int main(int argc, char **argv)
{
#if LJ_TARGET_WINDOWS && defined(LJ_THR_TLS_TEST_HELPERS)
  if (argc == 2 && strcmp(argv[1], LJ_TLS_RAW_ABORT_ARG) == 0) {
    raw_admission_abort_child();
    return 1;
  }
  if (argc == 2 && strcmp(argv[1], LJ_TLS_INIT_RACE_ARG) == 0)
    return concurrent_index_retry_child();
  assert(argc == 1);
  /* Each child starts with a fresh process index. The first proves that the
  ** void raw API fails closed on per-thread cell admission; the second proves
  ** a failed concurrent process-index initializer remains retryable. */
  assert(run_windows_child(LJ_TLS_RAW_ABORT_ARG) == LJ_TLS_RAW_ABORT_EXIT);
  assert(run_windows_child(LJ_TLS_INIT_RACE_ARG) == 0);
#else
  (void)argc;
  (void)argv;
#endif
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
  LJThr gc2_tls_thr[2] = {{0}, {0}};
  GC2TLSIsolationCtx gc2_tls_ctx[2];
  uint32_t gc2_tls_ready = 0, gc2_tls_go = 0;
  LJTGSlotSnap snap, pinned;
  la_u128 saved_token;
  void *aret = NULL, *bret = NULL;
  void *body = NULL;
  void *mis_storage = NULL, *mis_body;
#if LJ_TARGET_WINDOWS && defined(LJ_THR_TLS_TEST_HELPERS)
  AdmissionFailureThreadCtx alloc_fail, publish_fail;
  GC2TLSFallbackCtx fallback;
  LJThr alloc_fail_thr = {0}, publish_fail_thr = {0};
  LJThr fallback_thr = {0};
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
#if LJ_TARGET_WINDOWS && defined(LJ_THR_TLS_TEST_HELPERS)
  /* Preserve the direct-slot backend's documented TlsGetValue side effect. */
  SetLastError(ERROR_INVALID_DATA);
  assert(lj_thr_get_tg() == NULL);
  assert(GetLastError() == ERROR_SUCCESS);

  /* A raw foreign thread may take ordinary SMR reads before Lua admission.
  ** It must use the global count without allocating a cell; exclusive GC2
  ** ownership remains unavailable until admission. */
  memset(&fallback, 0, sizeof(fallback));
  fallback.g = g;
  lj_thr_tls_test_fail_cell_alloc(1);
  assert(lj_thr_create(&fallback_thr, gc2_tls_fallback_thread, &fallback) == 0);
  assert(lj_thr_join(&fallback_thr, NULL) == 0);
  assert(fallback.before == NULL && fallback.after == NULL);
  assert(fallback.read_result && !fallback.init_result);
  assert(gc2_smr_readers_acq(g) == 0);
#endif

  /* Two same-universe readers must each own an independent nesting record.
  ** A process-global capability would incorrectly collapse this count to one. */
  memset(gc2_tls_ctx, 0, sizeof(gc2_tls_ctx));
  gc2_tls_ctx[0].g = g;
  gc2_tls_ctx[1].g = g;
  gc2_tls_ctx[0].ready = gc2_tls_ctx[1].ready = &gc2_tls_ready;
  gc2_tls_ctx[0].go = gc2_tls_ctx[1].go = &gc2_tls_go;
  assert(lj_thr_create(&gc2_tls_thr[0], gc2_tls_isolation_thread,
                       &gc2_tls_ctx[0]) == 0);
  assert(lj_thr_create(&gc2_tls_thr[1], gc2_tls_isolation_thread,
                       &gc2_tls_ctx[1]) == 0);
  while (la_load32_acq(&gc2_tls_ready) != 2u)
    (void)lj_thr_yield(NULL);
  assert(gc2_smr_readers_acq(g) == 2u);
  la_store32_rel(&gc2_tls_go, 1);
  assert(lj_thr_join(&gc2_tls_thr[0], NULL) == 0);
  assert(lj_thr_join(&gc2_tls_thr[1], NULL) == 0);
  assert(gc2_smr_readers_acq(g) == 0);
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
  fixture_adopt_unpaired_tg(a);
  fixture_adopt_unpaired_tg(b);

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
  fixture_release_unpaired_tg(b);
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

  fixture_release_unpaired_tg(a);
  retire_body(&akey);
  reclaim_body(&akey, a);
  stale = akey;

  /* Reuse the same stable slot and body address under a new incarnation. */
  akey = publish_body(g, a, aslot, 0);
  fixture_adopt_unpaired_tg(a);
  assert(akey.incarnation != stale.incarnation && akey.slot == stale.slot);
  assert(lj_tgregistry_try_borrow(&akey, &ahold, &snap) == LJ_TGSLOT_OK);
  assert(lj_thr_tg_install(&ahold) == LJ_THR_TG_OK);
  assert(lj_thr_tg_clear(&stale, &old) == LJ_THR_TG_EXPECT_MISMATCH);
  assert(!old.active && lj_thr_get_tg() == a);
  assert(lj_thr_tg_clear(&akey, &old) == LJ_THR_TG_OK);
  assert(old.active && old.key.incarnation == akey.incarnation);
  release_borrow(&old);
  fixture_release_unpaired_tg(a);
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
  fixture_adopt_unpaired_tg(b);
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
  fixture_release_unpaired_tg(b);
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
