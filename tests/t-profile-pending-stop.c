/* A stopped profiler must discard blocked SIGPROF deliveries before restoring
** a disposition which could run an unrelated handler or terminate the process.
*/

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "luajit.h"

#include "lj_atomic.h"
#include "lj_profile.h"
#include "lj_thr.h"

#if LJ_PROFILE_SIGPROF && LJ_THR_TG_SIGNAL_CACHE

#define WAIT_ATTEMPTS 5000u

typedef struct PendingSignalCase {
  uint32_t blocked;
  uint32_t pending_before;
  uint32_t inspect_after_stop;
  uint32_t pending_after;
  uint32_t unblock;
  uint32_t fresh_sent;
  uint32_t error;
  int send_fresh;
} PendingSignalCase;

static uint32_t old_handler_calls;

static void pause_briefly(void)
{
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static int wait_flag(const uint32_t *flag)
{
  uint32_t i;
  for (i = 0; i < WAIT_ATTEMPTS; i++) {
    if (la_load32_acq(flag) != 0)
      return 1;
    pause_briefly();
  }
  return 0;
}

static void old_handler(int signo)
{
  (void)signo;
  la_add32_rlx(&old_handler_calls, 1);
}

static void profile_callback(void *data, lua_State *L, int samples,
                             int vmstate)
{
  (void)data;
  (void)L;
  (void)vmstate;
  assert(samples > 0);
}

static int signal_is_pending(void)
{
  sigset_t pending;
  assert(sigpending(&pending) == 0);
  return sigismember(&pending, SIGPROF) == 1;
}

static void *blocked_signal_peer(void *arg)
{
  PendingSignalCase *ctx = (PendingSignalCase *)arg;
  sigset_t set;
  sigset_t oldmask;
  int rc;

  sigemptyset(&set);
  sigaddset(&set, SIGPROF);
  rc = pthread_sigmask(SIG_BLOCK, &set, &oldmask);
  if (rc != 0) {
    la_store32_rel(&ctx->error, 1);
    return NULL;
  }
  la_store32_rel(&ctx->blocked, 1);
  if (raise(SIGPROF) != 0 || !signal_is_pending())
    la_store32_rel(&ctx->error, 1);
  la_store32_rel(&ctx->pending_before, 1);
  if (!wait_flag(&ctx->inspect_after_stop))
    la_store32_rel(&ctx->error, 1);
  if (signal_is_pending()) {
    struct sigaction ignore;
    la_store32_rel(&ctx->error, 1);
    /* Keep a failed default-disposition case from terminating the harness
    ** while still verifying the bad pending state above. */
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    (void)sigaction(SIGPROF, &ignore, NULL);
  }
  la_store32_rel(&ctx->pending_after, 1);
  if (!wait_flag(&ctx->unblock))
    la_store32_rel(&ctx->error, 1);
  if (ctx->send_fresh) {
    rc = pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    if (rc != 0)
      la_store32_rel(&ctx->error, 1);
    if (raise(SIGPROF) != 0)
      la_store32_rel(&ctx->error, 1);
    la_store32_rel(&ctx->fresh_sent, 1);
  }
  rc = pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
  if (rc != 0)
    la_store32_rel(&ctx->error, 1);
  return NULL;
}

static void run_case(lua_State *L, void (*handler)(int), int send_fresh,
                     uint32_t expected_handler_calls)
{
  struct sigaction action;
  struct sigaction current;
  PendingSignalCase ctx;
  pthread_t peer;
  memset(&ctx, 0, sizeof(ctx));
  ctx.send_fresh = send_fresh;
  memset(&action, 0, sizeof(action));
  action.sa_handler = handler;
  sigemptyset(&action.sa_mask);
  assert(sigaction(SIGPROF, &action, NULL) == 0);
  la_store32_rel(&old_handler_calls, 0);

  luaJIT_profile_start(L, "i1000000", profile_callback, NULL);
  assert(lj_profile_active(L));
  assert(pthread_create(&peer, NULL, blocked_signal_peer, &ctx) == 0);
  assert(wait_flag(&ctx.blocked));
  assert(wait_flag(&ctx.pending_before));
  assert(la_load32_acq(&ctx.error) == 0);

  luaJIT_profile_stop(L);
  assert(!lj_profile_active(L));
  assert(sigaction(SIGPROF, NULL, &current) == 0);
  assert(current.sa_handler == handler);
  la_store32_rel(&ctx.inspect_after_stop, 1);
  assert(wait_flag(&ctx.pending_after));
  assert(la_load32_acq(&ctx.error) == 0);

  la_store32_rel(&ctx.unblock, 1);
  assert(pthread_join(peer, NULL) == 0);
  assert(la_load32_acq(&ctx.error) == 0);
  if (send_fresh)
    assert(la_load32_acq(&ctx.fresh_sent) != 0);
  assert(la_load32_acq(&old_handler_calls) == expected_handler_calls);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  struct sigaction original;
  assert(L != NULL);
  assert(sigaction(SIGPROF, NULL, &original) == 0);

  /* A queued signal is discarded; only a fresh post-stop delivery reaches
  ** the custom disposition that was saved before profiling began. */
  run_case(L, old_handler, 1, 1);
  /* SIG_IGN is safe to exercise with a fresh signal after unblocking. */
  run_case(L, SIG_IGN, 1, 0);
  /* Do not send a fresh SIGPROF with the default disposition.  The pending
  ** signal is checked and cleared while still blocked before unblocking. */
  run_case(L, SIG_DFL, 0, 0);

  lua_close(L);
  assert(sigaction(SIGPROF, &original, NULL) == 0);
  puts("t-profile-pending-stop OK");
  return 0;
}

#else

int main(void)
{
  puts("t-profile-pending-stop skipped: SIGPROF unavailable");
  return 0;
}

#endif
