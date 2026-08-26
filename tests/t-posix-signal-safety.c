/*
** POSIX exact-TG signal cache and SIGPROF timer lifecycle regression.
*/

#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

#include "lj_obj.h"
#include "lj_profile.h"
#include "lj_safepoint.h"
#include "lj_tab.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_vm.h"

#if defined(LJ_PROFILE_DSO_LOADER)

#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#if LJ_TARGET_LINUX
#include <sys/syscall.h>
#endif

typedef lua_State *(*loader_newstate_f)(void);
typedef void (*loader_close_f)(lua_State *L);
typedef void (*loader_profile_start_f)(lua_State *L, const char *mode,
                                       luaJIT_profile_callback cb, void *data);
typedef void (*loader_profile_stop_f)(lua_State *L);
typedef int (*loader_profile_active_f)(lua_State *L);
typedef void (*loader_void_u32_f)(uint32_t value);
typedef void (*loader_void_f)(void);
typedef uint32_t (*loader_u32_f)(void);

static void loader_callback(void *data, lua_State *L, int samples, int vmstate)
{
  (void)data;
  (void)L;
  (void)samples;
  (void)vmstate;
}

static void *loader_symbol(void *handle, const char *name)
{
  void *symbol = dlsym(handle, name);
  assert(symbol != NULL);
  return symbol;
}

static int loader_noload(const char *path)
{
#ifdef RTLD_NOLOAD
  void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
  if (handle) {
    assert(dlclose(handle) == 0);
    return 1;
  }
#else
  (void)path;
#endif
  return 0;
}

static int profile_dso_loader_main(const char *path, const char *mode)
{
  struct sigaction sa, oldsa;
  void *handle;
  lua_State *L;
  loader_newstate_f newstate;
  loader_close_f close_state;
  loader_profile_start_f profile_start;
  loader_profile_stop_f profile_stop;
  loader_profile_active_f profile_active;
  loader_void_f reset;
  loader_void_u32_f fail_pin, fail_match, fail_setitimer;
  loader_u32_f pinned, installed;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  assert(sigemptyset(&sa.sa_mask) == 0);
  assert(sigaction(SIGPROF, &sa, &oldsa) == 0);
  handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  assert(handle != NULL);
  newstate = (loader_newstate_f)loader_symbol(handle, "luaL_newstate");
  close_state = (loader_close_f)loader_symbol(handle, "lua_close");
  profile_start = (loader_profile_start_f)
    loader_symbol(handle, "luaJIT_profile_start");
  profile_stop = (loader_profile_stop_f)
    loader_symbol(handle, "luaJIT_profile_stop");
  profile_active = (loader_profile_active_f)
    loader_symbol(handle, "luaJIT_profile_timer_test_active");
  reset = (loader_void_f)
    loader_symbol(handle, "luaJIT_profile_timer_test_reset");
  fail_pin = (loader_void_u32_f)
    loader_symbol(handle, "luaJIT_profile_timer_test_fail_image_pin");
  fail_match = (loader_void_u32_f)
    loader_symbol(handle, "luaJIT_profile_timer_test_fail_image_match");
  fail_setitimer = (loader_void_u32_f)
    loader_symbol(handle, "luaJIT_profile_timer_test_fail_setitimer");
  pinned = (loader_u32_f)
    loader_symbol(handle, "luaJIT_profile_timer_test_image_pinned");
  installed = (loader_u32_f)
    loader_symbol(handle, "luaJIT_profile_timer_test_handler_installed");
  L = newstate();
  assert(L != NULL);
  reset();
  if (strcmp(mode, "pin-failure") == 0 ||
      strcmp(mode, "pin-mismatch") == 0) {
    pid_t child;
    int status;
    if (strcmp(mode, "pin-failure") == 0)
      fail_pin(1);
    else
      fail_match(1);
    profile_start(L, "i1000000", loader_callback, NULL);
    assert(!profile_active(L) && !pinned() && !installed());
    close_state(L);
    assert(dlclose(handle) == 0);
#if !LJ_TARGET_OSX
    assert(!loader_noload(path));
#else
    /* Darwin never unloads an image containing compiler TLS, even without a
    ** retained dlopen reference. The logical !pinned/!installed checks above
    ** and the post-close fork remain the available negative proof there. */
#endif
    child = fork();
    assert(child >= 0);
    if (child == 0)
      _exit(0);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  } else {
    profile_start(L, "i1000000", loader_callback, NULL);
    assert(profile_active(L) && pinned());
    if (strcmp(mode, "stop-failure") == 0) {
      reset();
      fail_setitimer(1);
    }
    profile_stop(L);
    assert(!profile_active(L));
    if (strcmp(mode, "stop-failure") == 0)
      assert(installed());
    close_state(L);
    assert(dlclose(handle) == 0);
    assert(loader_noload(path));
    assert(raise(SIGPROF) == 0);
  }
  assert(sigaction(SIGPROF, &oldsa, NULL) == 0);
  return 0;
}

#else

#if LJ_THR_TG_SIGNAL_CACHE && LJ_PROFILE_SIGPROF && \
    defined(LJ_THR_SIGNAL_TEST_HELPERS) && \
    defined(LJ_PROFILE_TIMER_TEST_HELPERS)

#include <sys/wait.h>
#include <unistd.h>

static uintptr_t sampled_signal_body;
static uint32_t profile_callback_count;
static uint32_t profile_callback_samples;

static void sample_handler(int signo)
{
  int saved_errno = errno;
  (void)signo;
  /* On Darwin this is positive current-host coverage for the Apple ABI's
  ** implementation-specific pthread_self() signal behavior. POSIX itself does
  ** not make that portability guarantee. */
  la_storeuptr_rel(&sampled_signal_body,
                   (uintptr_t)lj_thr_get_tg_signal());
  errno = saved_errno;
}

static void sample_signal(TGState *expected)
{
  la_storeuptr_rel(&sampled_signal_body, ~(uintptr_t)0);
  assert(raise(SIGPROF) == 0);
  assert((TGState *)la_loaduptr_acq(&sampled_signal_body) == expected);
}

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

/* Signal-cache fixtures use synthetic TGs without a paired lua_State. Give
** each such body the exact actor whose TLS/signal transaction is exercised;
** a production registry-LIVE actor zero denotes a paired handoff and generic
** binding must reject it. */
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

static LJTGRegistryKey publish_body(global_State *g, TGState *tg,
                                    LJTGRegistrySlot *slot, int adopt_current)
{
  LJTGRegistryKey key;
  LJTGSlotSnap snap;
  LJTGRegistrySlot *head;
  assert(lj_tgregistry_slot_init_unpublished(slot, 0, NULL));
  assert(lj_tgregistry_try_claim(slot, &key, &snap) == LJ_TGSLOT_OK);
  memset(tg, 0, sizeof(*tg));
  tg->gl = g;
  tg->registry_key = key;
  assert(lj_tgregistry_try_publish_body(&key, tg, &snap) == LJ_TGSLOT_OK);
  head = gc2_tg_registry_head_acq(g);
  slot->next_all = head;
  assert(gc2_tg_registry_head_cas(g, &head, slot));
  (void)gc2_tg_registry_nodes_add(g, 1);
  assert(lj_tgregistry_try_publish(&key, &snap) == LJ_TGSLOT_OK);
  if (adopt_current)
    fixture_adopt_unpaired_tg(tg);
  assert(lease_count(&key, LJ_TGSLOT_LIVE) == 1u);
  return key;
}

static void retire_reclaim(const LJTGRegistryKey *key, void *expected)
{
  LJTGSlotSnap snap;
  void *body = NULL;
  assert(lj_tgregistry_try_detach(key, &snap) == LJ_TGSLOT_OK);
  assert(lj_tgregistry_try_retire(key, &snap) == LJ_TGSLOT_OK);
  assert(lj_tgregistry_try_reclaim(key, &body, &snap) == LJ_TGSLOT_OK);
  assert(body == expected);
  assert(lj_tgregistry_try_clear(key, &snap) == LJ_TGSLOT_OK);
  assert(lease_count(key, LJ_TGSLOT_EMPTY) == 0u);
}

typedef struct ExitBindingCtx {
  LJTGRegistryBorrow hold;
  TGState *body;
} ExitBindingCtx;

static void *exit_with_binding(void *arg)
{
  ExitBindingCtx *ctx = (ExitBindingCtx *)arg;
  fixture_adopt_unpaired_tg(ctx->body);
  assert(lj_thr_tg_install(&ctx->hold) == LJ_THR_TG_OK);
  assert(!ctx->hold.active);
  assert(lj_thr_get_tg_signal() == ctx->body);
  return NULL;  /* The pthread-key destructor clears handler visibility. */
}

static void test_signal_cache(void)
{
  struct sigaction sa, oldsa;
  global_State *g = (global_State *)calloc(1, sizeof(*g));
  TGState *a = (TGState *)calloc(1, sizeof(*a));
  TGState *b = (TGState *)calloc(1, sizeof(*b));
  LJTGRegistrySlot *aslot = (LJTGRegistrySlot *)malloc(sizeof(*aslot));
  LJTGRegistrySlot *bslot = (LJTGRegistrySlot *)malloc(sizeof(*bslot));
  LJTGRegistryKey akey, bkey;
  LJTGRegistryBorrow ahold, bhold, old, leaked;
  LJTGSlotSnap snap;
  ExitBindingCtx exit_ctx;
  LJThr exit_thr = {0};
  pid_t child;
  int status;

  assert(g && a && b && aslot && bslot);
  /* This fixture is linked into the non-unloadable main image. Production
  ** profile start performs its permanent DSO pin before this activation. */
#if LJ_TARGET_LINUX
  errno = EDOM;
  lj_thr_tg_signal_test_fail_fork_page(1);
  assert(!lj_thr_tg_signal_activate());
  assert(errno == EDOM && lj_thr_get_tg_signal() == NULL);
#endif
  assert(lj_thr_tg_signal_activate());
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sample_handler;
  sigemptyset(&sa.sa_mask);
  assert(sigaction(SIGPROF, &sa, &oldsa) == 0);
  lj_thr_set_tg(NULL);
  akey = publish_body(g, a, aslot, 1);
  bkey = publish_body(g, b, bslot, 1);
  lj_tgregistry_borrow_init(&ahold);
  lj_tgregistry_borrow_init(&bhold);
  lj_tgregistry_borrow_init(&old);
  assert(lj_tgregistry_try_borrow(&akey, &ahold, &snap) == LJ_TGSLOT_OK);

  /* Every cold admission failure is retryable, consumes no handle, changes
  ** neither TLS nor the mirror, and preserves the caller's errno. */
  errno = EDOM;
  lj_thr_tg_signal_test_fail_key_create(1);
  assert(lj_thr_tg_install(&ahold) == LJ_THR_TG_TLS_FAILURE);
  assert(errno == EDOM && ahold.active && lj_thr_get_tg() == NULL);
  sample_signal(NULL);
  errno = ERANGE;
  lj_thr_tg_signal_test_fail_cell_alloc(1);
  assert(lj_thr_tg_install(&ahold) == LJ_THR_TG_TLS_FAILURE);
  assert(errno == ERANGE && ahold.active && lj_thr_get_tg() == NULL);
  errno = E2BIG;
  lj_thr_tg_signal_test_fail_cell_publish(1);
  assert(lj_thr_tg_install(&ahold) == LJ_THR_TG_TLS_FAILURE);
  assert(errno == E2BIG && ahold.active && lj_thr_get_tg() == NULL);

  assert(lj_thr_tg_install(&ahold) == LJ_THR_TG_OK);
  assert(!ahold.active && lj_thr_get_tg() == a);
  sample_signal(a);
  assert(lj_tgregistry_try_borrow(&bkey, &bhold, &snap) == LJ_TGSLOT_OK);
  assert(lj_thr_tg_swap(&akey, &bhold, &old) == LJ_THR_TG_OK);
  assert(!bhold.active && old.active && old.body == a);
  sample_signal(b);
  release_borrow(&old);

#if LJ_TARGET_LINUX
  /* Direct SYS_fork bypasses every pthread_atfork callback. The kernel-wiped
  ** page remains DIRTY across an uninterrupted nested raw-fork chain, so no
  ** inherited cell is reachable even before a cold PID/generation repair. */
  child = (pid_t)syscall(SYS_fork);
  assert(child >= 0);
  if (child == 0) {
    pid_t grandchild = (pid_t)syscall(SYS_fork);
    assert(grandchild >= 0);
    if (grandchild == 0) {
      lj_thr_tg_signal_test_force_process((uintptr_t)getpid());
      assert(lj_thr_get_tg_signal() == NULL);
      lj_tgregistry_borrow_init(&ahold);
      lj_tgregistry_borrow_init(&old);
      assert(lj_tgregistry_try_borrow(&akey, &ahold, &snap) == LJ_TGSLOT_OK);
      assert(lj_thr_tg_swap(&bkey, &ahold, &old) == LJ_THR_TG_OK);
      assert(lj_thr_get_tg_signal() == a);
      release_borrow(&old);
      assert(lj_thr_tg_clear(&akey, &old) == LJ_THR_TG_OK);
      release_borrow(&old);
      _exit(0);
    }
    assert(waitpid(grandchild, &status, 0) == grandchild);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  sample_signal(b);
#endif

  /* A generation change rejects a stale cell even under deliberately reused
  ** PID/thread bits. Saturation poisons the child copy instead of wrapping. */
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    uint64_t generation = lj_thr_tg_signal_test_generation();
    assert(lj_thr_get_tg_signal() == NULL);
    lj_thr_tg_signal_test_advance_same_process();
    assert(lj_thr_tg_signal_test_generation() == generation + 1u);
    assert(lj_thr_tg_signal_test_process() == (uintptr_t)getpid());
    assert(lj_thr_get_tg_signal() == NULL);
    lj_thr_tg_signal_test_force_generation(UINT64_MAX);
    lj_thr_tg_signal_test_advance_same_process();
    assert(lj_thr_tg_signal_test_generation() == UINT64_MAX);
    assert(lj_thr_tg_signal_test_poisoned());
    assert(lj_thr_get_tg_signal() == NULL);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  /* A raw/missed fork is modeled by a mismatched cached PID. Cold admission
  ** repairs it, advances exactly once, and resets inherited BUILDING states. */
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    uint64_t generation = lj_thr_tg_signal_test_generation();
    lj_thr_tg_signal_test_force_process((uintptr_t)getpid() + 1u);
    lj_thr_tg_signal_test_force_building();
    assert(lj_thr_get_tg_signal() == NULL);
    assert(lj_tgregistry_try_borrow(&akey, &ahold, &snap) == LJ_TGSLOT_OK);
    assert(lj_thr_tg_swap(&bkey, &ahold, &old) == LJ_THR_TG_OK);
    assert(lj_thr_tg_signal_test_generation() == generation + 1u);
    assert(lj_thr_tg_signal_test_process() == (uintptr_t)getpid());
    sample_signal(a);
    release_borrow(&old);
    assert(lj_thr_tg_clear(&akey, &old) == LJ_THR_TG_OK);
    release_borrow(&old);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  /* A fork while cold builders are marked in progress cannot leave the child
  ** spinning for vanished threads. The child callback invalidates both words. */
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    uint64_t generation = lj_thr_tg_signal_test_generation();
    pid_t grandchild;
    lj_thr_tg_signal_test_force_building();
    grandchild = fork();
    assert(grandchild >= 0);
    if (grandchild == 0) {
      assert(lj_thr_tg_signal_test_generation() == generation + 1u);
      assert(lj_thr_tg_signal_test_key_state() == 0u);
      assert(lj_thr_get_tg_signal() == NULL);
      _exit(0);
    }
    assert(waitpid(grandchild, &status, 0) == grandchild);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  /* The child copy cannot match parent cells even if pthread_self is reused.
  ** Its inherited exact TLS handle can still be cleared and released. */
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    assert(lj_thr_get_tg() == b);
    sample_signal(NULL);
    assert(lj_thr_tg_clear(&bkey, &old) == LJ_THR_TG_OK);
    sample_signal(NULL);
    release_borrow(&old);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(lj_thr_get_tg() == b);
  sample_signal(b);
  assert(lj_thr_tg_clear(&bkey, &old) == LJ_THR_TG_OK);
  sample_signal(NULL);
  release_borrow(&old);
  retire_reclaim(&bkey, b);

  /* Raw compatibility remains invisible to exact consumers, while the
  ** profiler-only transitional getter can keep the stock API delivering
  ** samples until all production bindings carry exact leases. */
  lj_thr_set_tg(a);
  assert(lj_thr_get_tg() == a);
  assert(lj_thr_get_tg_profile_signal() == a);
  sample_signal(NULL);
  lj_thr_set_tg(NULL);

  /* A clean pthread exit clears the mirror before the exact lease can ever be
  ** recovered. This fixture reconstructs the deliberately leaked fungible
  ** count afterward solely to leave the model reclaimable. */
  memset(&exit_ctx, 0, sizeof(exit_ctx));
  exit_ctx.body = a;
  fixture_release_unpaired_tg(a);
  assert(lj_tgregistry_try_borrow(&akey, &exit_ctx.hold, &snap) ==
         LJ_TGSLOT_OK);
  lj_thr_tg_signal_test_reset_destructors();
  assert(lj_thr_create(&exit_thr, exit_with_binding, &exit_ctx) == 0);
  assert(lj_thr_join(&exit_thr, NULL) == 0);
  assert(lj_thr_tg_signal_test_destructors() == 1u);
  assert(lj_thr_tg_signal_test_last_destructor_word() ==
         ((uintptr_t)a | (uintptr_t)1u));
  leaked.key = akey;
  leaked.body = a;
  leaked.active = 1;
  release_borrow(&leaked);
  retire_reclaim(&akey, a);

  assert(sigaction(SIGPROF, &oldsa, NULL) == 0);
  free(bslot);
  free(aslot);
  free(b);
  free(a);
  free(g);
}

static void profile_callback(void *data, lua_State *L, int samples,
                             int vmstate)
{
  (void)data;
  (void)L;
  (void)vmstate;
  (void)la_add32_acqrel(&profile_callback_count, 1);
  (void)la_add32_acqrel(&profile_callback_samples, (uint32_t)samples);
}

static void profile_start_long(lua_State *L)
{
  luaJIT_profile_start(L, "i1000000", profile_callback, NULL);
}

#if LJ_HASJIT
#define KEY_PROFILE_THREAD (U64x(81000000,00000000)|'t')
#define KEY_PROFILE_FUNC   (U64x(81000000,00000000)|'f')

static int profile_start_long_cpcall(lua_State *L)
{
  profile_start_long(L);
  return 0;
}

static int profile_stop_cpcall(lua_State *L)
{
  luaJIT_profile_stop(L);
  return 0;
}

static void profile_registry_assert_clear(lua_State *L)
{
  TValue key;
  cTValue *tv;
  key.u64 = KEY_PROFILE_THREAD;
  tv = lj_tab_get(L, lj_registry_tab_acq(G(L)), &key);
  assert(tv == NULL || tvisnil(tv));
  key.u64 = KEY_PROFILE_FUNC;
  tv = lj_tab_get(L, lj_registry_tab_acq(G(L)), &key);
  assert(tv == NULL || tvisnil(tv));
}
#endif

static void assert_inert_profile_handler(void)
{
  la_storeuptr_rel(&sampled_signal_body, ~(uintptr_t)0);
  assert(raise(SIGPROF) == 0);
  assert(la_loaduptr_acq(&sampled_signal_body) == ~(uintptr_t)0);
}

typedef struct ProfileSignalCtx {
  LJTGRegistryBorrow hold;
  LJTGRegistryKey key;
  TGState *body;
} ProfileSignalCtx;

static void *raise_profile_signal(void *arg)
{
  ProfileSignalCtx *ctx = (ProfileSignalCtx *)arg;
  LJTGRegistryBorrow old;
  lj_tgregistry_borrow_init(&old);
  fixture_adopt_unpaired_tg(ctx->body);
  assert(lj_thr_tg_install(&ctx->hold) == LJ_THR_TG_OK);
  assert(raise(SIGPROF) == 0);
  assert(lj_thr_tg_clear(&ctx->key, &old) == LJ_THR_TG_OK);
  release_borrow(&old);
  fixture_release_unpaired_tg(ctx->body);
  return NULL;
}

static void *raise_profile_signal_after_before_arm(void *arg)
{
  ProfileSignalCtx *ctx = (ProfileSignalCtx *)arg;
  LJTGRegistryBorrow old;
  lj_tgregistry_borrow_init(&old);
  fixture_adopt_unpaired_tg(ctx->body);
  assert(lj_thr_tg_install(&ctx->hold) == LJ_THR_TG_OK);
  while (lj_profile_timer_test_before_arm_entered() == 0)
    (void)sched_yield();
  assert(raise(SIGPROF) == 0);
  assert(lj_thr_tg_clear(&ctx->key, &old) == LJ_THR_TG_OK);
  release_borrow(&old);
  fixture_release_unpaired_tg(ctx->body);
  return NULL;
}

static void *release_paused_start(void *arg)
{
  (void)arg;
  while (lj_profile_timer_test_signal_entered() == 0)
    (void)sched_yield();
  lj_profile_timer_test_pause_before_arm(0);
  while (lj_profile_timer_test_drain_waits() == 0)
    (void)sched_yield();
  lj_profile_timer_test_pause_signal(0);
  return NULL;
}

static void *release_paused_signal(void *arg)
{
  (void)arg;
  while (lj_profile_timer_test_signal_entered() == 0)
    (void)sched_yield();
  while (lj_profile_timer_test_drain_waits() == 0)
    (void)sched_yield();
  lj_profile_timer_test_pause_signal(0);
  return NULL;
}

static void test_profile_timer(void)
{
  struct sigaction sa, oldsa;
  lua_State *L;
  TGState *tg;
  TGState signal_tg;
  LJTGRegistrySlot *signal_slot;
  LJTGRegistryKey key;
  LJTGRegistryKey signal_key;
  LJTGRegistryBorrow exact, old;
  LJTGSlotSnap snap;
  ProfileSignalCtx signal_ctx;
  pthread_t signal_thread, release_thread;
  pid_t child;
  int status;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sample_handler;
  sigemptyset(&sa.sa_mask);
  assert(sigaction(SIGPROF, &sa, &oldsa) == 0);
  L = luaL_newstate();
  assert(L);
  signal_slot = (LJTGRegistrySlot *)malloc(sizeof(*signal_slot));
  assert(signal_slot != NULL);  /* Registry teardown owns the linked slot. */
  tg = L2TG(L);
  assert(tg != NULL);
  key = tg->registry_key;
  lj_tgregistry_borrow_init(&exact);
  lj_tgregistry_borrow_init(&old);
  lj_thr_set_tg(NULL);
  assert(lj_tgregistry_try_borrow(&key, &exact, &snap) == LJ_TGSLOT_OK);
  assert(lj_thr_tg_install(&exact) == LJ_THR_TG_OK);
  signal_key = publish_body(G(L), &signal_tg, signal_slot, 0);
#if LJ_HASJIT
  luaL_openlibs(L);
#endif

#if LJ_HASJIT
  /* Never arm while an active GC hook prevents the mandatory pre-policy trace
  ** flush. The recording flag is rolled back together with STARTING. */
  lj_profile_timer_test_reset();
  (void)hookmask_update(G(L), 0, HOOK_GC);
  profile_start_long(L);
  assert(!lj_profile_active(L));
  assert(!lj_profile_poll_required(G(L)));
  assert(lj_profile_timer_test_sigaction_calls() == 0u);
  (void)hookmask_update(G(L), HOOK_GC, 0);

  /* The Lua wrapper roots its hidden callback state before entering the low-
  ** level lifecycle. It must clear both roots if that lifecycle throws. */
  lj_profile_timer_test_reset();
  lj_profile_timer_test_fail_trace_flush(1);
  assert(luaL_dostring(L,
    "local profile = require('jit.profile')\n"
    "local ok = pcall(profile.start, 'i1000000', function() end)\n"
    "assert(not ok, 'injected profile start unexpectedly succeeded')\n") == 0);
  profile_registry_assert_clear(L);

  /* A protected retirement failure must first roll STARTING and its JIT poll
  ** policy back to IDLE, leaving a clean retry and no kernel-visible handler. */
  lj_profile_timer_test_reset();
  lj_profile_timer_test_fail_trace_flush(1);
  assert(lua_cpcall(L, profile_start_long_cpcall, NULL) != 0);
  lua_settop(L, 0);
  assert(!lj_profile_active(L));
  assert(!lj_profile_poll_required(G(L)));
  assert(lj_profile_timer_test_sigaction_calls() == 0u);
#endif

  /* A loadable image that cannot establish its permanent reference must fail
  ** before the first sigaction. The main-executable retry then pins logically
  ** without requiring dlopen on a static embedding. */
  lj_profile_timer_test_reset();
  lj_profile_timer_test_fail_image_pin(1);
  profile_start_long(L);
  assert(!lj_profile_active(L));
  assert(!lj_profile_timer_test_image_pinned());
  assert(lj_profile_timer_test_sigaction_calls() == 0u);
  assert(lj_profile_timer_test_setitimer_calls() == 0u);

  /* Handler installation failure never arms the timer. */
  lj_profile_timer_test_reset();
  lj_profile_timer_test_fail_sigaction(1);
  profile_start_long(L);
  assert(!lj_profile_active(L));
  assert(lj_profile_timer_test_sigaction_calls() == 1u);
  assert(lj_profile_timer_test_setitimer_calls() == 0u);
  assert(!lj_profile_timer_test_handler_installed());
  assert(lj_profile_timer_test_image_pinned());

#if LJ_HASJIT
  /* The matching stop edge must finish timer, callback, buffer, state, and JIT
  ** policy cleanup before preserving the retirement error. A subsequent full
  ** start/stop proves neither STOPPING nor a stale poll policy survived. */
  lj_profile_timer_test_reset();
  profile_start_long(L);
  assert(lj_profile_active(L));
  lj_profile_timer_test_fail_trace_flush(1);
  assert(lua_cpcall(L, profile_stop_cpcall, NULL) != 0);
  lua_settop(L, 0);
  assert(!lj_profile_active(L));
  assert(!lj_profile_poll_required(G(L)));
  profile_start_long(L);
  assert(lj_profile_active(L));
  luaJIT_profile_stop(L);
  assert(!lj_profile_active(L));
#endif

  /* Profile start must be owned by the TG named by L, not merely any
  ** process-global state. A missing current binding fails before sigaction and
  ** leaves its exact lease available for an ordinary reinstall/retry. */
  assert(lj_thr_tg_clear(&key, &old) == LJ_THR_TG_OK);
  lj_profile_timer_test_reset();
  profile_start_long(L);
  assert(!lj_profile_active(L));
  assert(lj_profile_timer_test_sigaction_calls() == 0u);
  assert(lj_thr_tg_install(&old) == LJ_THR_TG_OK);

  /* A child process has no signal cell in its new process incarnation. A
  ** transient admission failure must fail before sigaction, and the next
  ** start must republish the inherited exact TLS lease and recover. */
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    assert(lj_thr_get_tg_signal() == NULL);
    lj_profile_timer_test_reset();
    lj_thr_tg_signal_test_fail_cell_alloc(1);
    profile_start_long(L);
    assert(!lj_profile_active(L));
    assert(lj_profile_timer_test_sigaction_calls() == 0u);
    assert(lj_profile_timer_test_setitimer_calls() == 0u);
    lj_profile_timer_test_reset();
    profile_start_long(L);
    assert(lj_profile_active(L));
    assert(lj_thr_get_tg_signal() == tg);
    luaJIT_profile_stop(L);
    assert(!lj_profile_active(L));
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  /* Arm failure restores a newly installed handler before IDLE rollback. */
  lj_profile_timer_test_reset();
  lj_profile_timer_test_fail_setitimer(1);
  profile_start_long(L);
  assert(!lj_profile_active(L));
  assert(lj_profile_timer_test_sigaction_calls() == 2u);
  assert(lj_profile_timer_test_setitimer_calls() == 1u);
  assert(!lj_profile_timer_test_handler_installed());

#if LJ_HASJIT
  /* If timer arm fails and the rollback retirement also throws, resource and
  ** lifecycle cleanup still completes before the injected error is preserved. */
  lj_profile_timer_test_reset();
  lj_profile_timer_test_fail_setitimer(1);
  lj_profile_timer_test_fail_trace_flush(2);
  assert(lua_cpcall(L, profile_start_long_cpcall, NULL) != 0);
  lua_settop(L, 0);
  assert(!lj_profile_active(L));
  assert(!lj_profile_poll_required(G(L)));
  assert(lj_profile_timer_test_sigaction_calls() == 2u);
  assert(lj_profile_timer_test_setitimer_calls() == 1u);
  assert(!lj_profile_timer_test_handler_installed());
#endif

  /* A signal which enters during STARTING is drained before failed-start data
  ** rollback and IDLE publication. The global state is not published yet. */
  lj_profile_timer_test_reset();
  lj_profile_timer_test_fail_setitimer(1);
  lj_profile_timer_test_pause_before_arm(1);
  lj_profile_timer_test_pause_signal(1);
  memset(&signal_ctx, 0, sizeof(signal_ctx));
  signal_ctx.key = signal_key;
  signal_ctx.body = &signal_tg;
  assert(lj_tgregistry_try_borrow(&signal_key, &signal_ctx.hold, &snap) ==
         LJ_TGSLOT_OK);
  assert(pthread_create(&signal_thread, NULL,
                        raise_profile_signal_after_before_arm,
                        &signal_ctx) == 0);
  assert(pthread_create(&release_thread, NULL, release_paused_start, NULL) ==
         0);
  profile_start_long(L);
  assert(!lj_profile_active(L));
  assert(pthread_join(signal_thread, NULL) == 0);
  assert(pthread_join(release_thread, NULL) == 0);
  assert(lj_profile_timer_test_drain_waits() != 0);
  assert(!lj_profile_timer_test_handler_installed());

  /* If rollback restoration fails, our handler remains installed but inert;
  ** a later start reuses it and a successful stop restores the true old one. */
  lj_profile_timer_test_reset();
  lj_profile_timer_test_fail_setitimer(1);
  lj_profile_timer_test_fail_sigaction(2);
  profile_start_long(L);
  assert(!lj_profile_active(L));
  assert(lj_profile_timer_test_handler_installed());
  assert_inert_profile_handler();
  lj_profile_timer_test_reset();
  profile_start_long(L);
  assert(lj_profile_active(L));
  assert(lj_profile_timer_test_sigaction_calls() == 0u);
  luaJIT_profile_stop(L);
  assert(!lj_profile_active(L));
  assert(!lj_profile_timer_test_handler_installed());

  /* The handler publishes samples/request only. Owner polling installs the
  ** profile dispatch overlay in normal context and the callback consumes it. */
  la_store32_rel(&profile_callback_count, 0);
  la_store32_rel(&profile_callback_samples, 0);
  lj_profile_timer_test_reset();
  profile_start_long(L);
  assert(lj_profile_active(L));
  assert(!(lj_tg_hookmask_load(tg) & HOOK_PROFILE));
  assert(!lj_tg_profile_request_acq(tg));
  assert(raise(SIGPROF) == 0);
  assert(lj_tg_profile_request_acq(tg));
  assert(!(lj_tg_hookmask_load(tg) & HOOK_PROFILE));
  assert(lj_safepoint_poll(L) == 0);
  assert(!lj_tg_profile_request_acq(tg));
  assert(lj_tg_hookmask_load(tg) & HOOK_PROFILE);
  assert(tg->dispatch[0] == lj_vm_profhook);
  lj_profile_interpreter(L);
  assert(la_load32_acq(&profile_callback_count) == 1u);
  assert(la_load32_acq(&profile_callback_samples) >= 1u);
  luaJIT_profile_stop(L);
  assert(!lj_profile_active(L));

  /* A failed disarm must not restore the old action while a timer may remain
  ** armed. Teardown is still safe because our handler sees IDLE/NULL. */
  lj_profile_timer_test_reset();
  profile_start_long(L);
  assert(lj_profile_active(L));
  lj_profile_timer_test_reset();
  lj_profile_timer_test_fail_setitimer(1);
  luaJIT_profile_stop(L);
  assert(!lj_profile_active(L));
  assert(lj_profile_timer_test_setitimer_calls() == 1u);
  assert(lj_profile_timer_test_sigaction_calls() == 0u);
  assert(lj_profile_timer_test_handler_installed());
  assert_inert_profile_handler();
  lj_profile_timer_test_reset();
  profile_start_long(L);
  assert(lj_profile_active(L));
  luaJIT_profile_stop(L);
  assert(!lj_profile_active(L));
  assert(!lj_profile_timer_test_handler_installed());

  /* A failed restore after a successful disarm has the same recoverable inert
  ** state and does not overwrite oldsa on the next start. */
  lj_profile_timer_test_reset();
  profile_start_long(L);
  assert(lj_profile_active(L));
  lj_profile_timer_test_reset();
  lj_profile_timer_test_fail_sigaction(1);
  luaJIT_profile_stop(L);
  assert(!lj_profile_active(L));
  assert(lj_profile_timer_test_handler_installed());
  assert_inert_profile_handler();
  lj_profile_timer_test_reset();
  profile_start_long(L);
  assert(lj_profile_active(L));
  luaJIT_profile_stop(L);
  assert(!lj_profile_active(L));
  assert(!lj_profile_timer_test_handler_installed());

  /* Hold a handler after its entry count but before ACTIVE validation. STOPPING
  ** must observe and drain it before returning and permitting VM teardown. */
  lj_profile_timer_test_reset();
  profile_start_long(L);
  assert(lj_profile_active(L));
  lj_profile_timer_test_pause_signal(1);
  memset(&signal_ctx, 0, sizeof(signal_ctx));
  signal_ctx.key = signal_key;
  signal_ctx.body = &signal_tg;
  assert(lj_tgregistry_try_borrow(&signal_key, &signal_ctx.hold, &snap) ==
         LJ_TGSLOT_OK);
  assert(pthread_create(&signal_thread, NULL, raise_profile_signal,
                        &signal_ctx) == 0);
  while (lj_profile_timer_test_signal_entered() == 0)
    (void)sched_yield();
  assert(pthread_create(&release_thread, NULL, release_paused_signal, NULL) ==
         0);
  luaJIT_profile_stop(L);
  assert(pthread_join(signal_thread, NULL) == 0);
  assert(pthread_join(release_thread, NULL) == 0);
  assert(lj_profile_timer_test_drain_waits() != 0);
  assert(!lj_profile_active(L));

  /* The profile atfork callback clears an inherited in-flight count and any
  ** copied cold BUILDING marker. Keep the artificial parent state in a child
  ** copy so the real fixture remains usable. */
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    pid_t grandchild;
    lj_profile_timer_test_force_signal_handlers(7);
    lj_profile_timer_test_force_atfork_building();
    grandchild = fork();
    assert(grandchild >= 0);
    if (grandchild == 0) {
      assert(lj_profile_timer_test_signal_handlers() == 0u);
      _exit(0);
    }
    assert(waitpid(grandchild, &status, 0) == grandchild);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

#if LJ_TARGET_LINUX
  lj_profile_timer_test_reset();
  profile_start_long(L);
  assert(lj_profile_active(L));
  child = (pid_t)syscall(SYS_fork);
  assert(child >= 0);
  if (child == 0) {
    /* No atfork callback ran. Snapshot repair must consume WIPEONFORK and
    ** discard an inherited/phantom handler count before the stop drain. */
    lj_profile_timer_test_force_signal_handlers(7);
    lj_profile_timer_test_force_process((uintptr_t)getpid());
    lj_thr_tg_signal_test_force_process((uintptr_t)getpid());
    assert(lj_profile_stop_hs(L) == 0);
    assert(lj_profile_timer_test_signal_handlers() == 0u);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  luaJIT_profile_stop(L);
  assert(!lj_profile_active(L));
#endif

  retire_reclaim(&signal_key, &signal_tg);
  assert(lj_thr_tg_clear(&key, &old) == LJ_THR_TG_OK);
  release_borrow(&old);
  lj_thr_set_tg(tg);
  lua_close(L);
  assert(sigaction(SIGPROF, &oldsa, NULL) == 0);
}

#endif

#endif /* !LJ_PROFILE_DSO_LOADER */

int main(int argc, char **argv)
{
#if defined(LJ_PROFILE_DSO_LOADER)
  assert(argc == 3);
  return profile_dso_loader_main(argv[1], argv[2]);
#else
  (void)argc;
  (void)argv;
#if LJ_THR_TG_SIGNAL_CACHE && LJ_PROFILE_SIGPROF && \
    defined(LJ_THR_SIGNAL_TEST_HELPERS) && \
    defined(LJ_PROFILE_TIMER_TEST_HELPERS)
  test_signal_cache();
  test_profile_timer();
  puts("t-posix-signal-safety OK: exact cache and timer lifecycle verified");
#else
  puts("t-posix-signal-safety skipped: unsupported target/profile");
#endif
  return 0;
#endif
}
