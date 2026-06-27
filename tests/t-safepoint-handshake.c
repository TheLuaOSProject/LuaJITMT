/*
** Focused test for the C-level soft-handshake scaffold.
*/

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_dispatch.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"

typedef struct NativeStopReqCtx {
  global_State *g;
  TGState *tg;
  pthread_t thread;
  char path[PATH_MAX];
  int active;
  int open_fifo;
  int open_errno;
} NativeStopReqCtx;

static NativeStopReqCtx native_stopreq_ctx;

typedef struct PrintStopReqCtx {
  global_State *g;
  TGState *tg;
  pthread_t thread;
  int fd;
  int err;
  uint32_t done;
} PrintStopReqCtx;

typedef struct InputStopReqCtx {
  global_State *g;
  TGState *tg;
  pthread_t thread;
  int fd;
  int err;
} InputStopReqCtx;

static void publish_manual(global_State *g, TGState *tg, uint32_t actions)
{
  uint64_t epoch = la_load64_rlx(&g->gc2.hs_epoch) + 1u;
  la_store32_rel(&g->gc2.hs_actions, actions);
  la_store32_rel(&g->gc2.hs_pending, 1);  /* 05 section 5.4.2. */
  la_store64_rel(&g->gc2.hs_epoch, epoch);  /* 05 section 5.4.2. */
  la_store32_rel(&tg->reqmask, actions);  /* 05 section 5.4.2. */
  la_store32_rel(&tg->poll, 1);  /* 05 section 5.4.2 signal word. */
}

static int publish_alloc_white_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  publish_manual(g, tg, LJ_GC2_HS_ALLOC_WHITE);
  return 0;
}

static int publish_stopreq_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  publish_manual(g, tg, LJ_GC2_HS_STOPREQ);
  return 0;
}

static int mark_sticky_stopreq_c(lua_State *L)
{
  TGState *tg = G2TG(G(L));
  assert(tg != NULL);
  la_or8_rlx(&tg->tg_flags, TGF_STOPREQ);
  return 0;
}

static int assert_acked_alloc_white_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  assert(g->gc2.hs_pending == 0);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->hs_epoch_ack == g->gc2.hs_epoch);
  assert(tg->alloc.alloc_black == 0);
  return 0;
}

static int assert_acked_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  assert(g->gc2.hs_pending == 0);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->hs_epoch_ack == g->gc2.hs_epoch);
  return 0;
}

static int clear_stopreq_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  uint8_t flags;
  assert(tg != NULL);
  assert(g->gc2.hs_pending == 0);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->hs_epoch_ack == g->gc2.hs_epoch);
  flags = la_load8_acq(&tg->tg_flags);
  assert((flags & TGF_STOPREQ) != 0);
  la_store8_rel(&tg->tg_flags, (uint8_t)(flags & ~TGF_STOPREQ));
  return 0;
}

static int mkfifo_test_c(lua_State *L)
{
  const char *path = luaL_checkstring(L, 1);
  if (mkfifo(path, 0600) != 0)
    return luaL_error(L, "mkfifo failed: %s", strerror(errno));
  lua_pushboolean(L, 1);
  return 1;
}

static void *native_stopreq_thread(void *arg)
{
  NativeStopReqCtx *ctx = (NativeStopReqCtx *)arg;
  struct timespec delay;
  int i;
  delay.tv_sec = 0;
  delay.tv_nsec = 1000000;
  for (i = 0; i < 1000 &&
       la_load8_acq(&ctx->tg->in_native) == 0; i++)
    (void)nanosleep(&delay, NULL);
  publish_manual(ctx->g, ctx->tg, LJ_GC2_HS_STOPREQ);
  if (ctx->open_fifo) {
    int fd = open(ctx->path, O_WRONLY);
    if (fd == -1)
      ctx->open_errno = errno;
    else
      (void)close(fd);
  }
  return NULL;
}

static int start_native_stopreq(lua_State *L, const char *path)
{
  size_t len = path ? strlen(path) : 0;
  int err;
  if (native_stopreq_ctx.active)
    return luaL_error(L, "native STOPREQ helper already active");
  if (len >= sizeof(native_stopreq_ctx.path))
    return luaL_error(L, "FIFO path too long");
  native_stopreq_ctx.g = G(L);
  native_stopreq_ctx.tg = G2TG(native_stopreq_ctx.g);
  native_stopreq_ctx.open_fifo = path != NULL;
  native_stopreq_ctx.open_errno = 0;
  if (path != NULL)
    memcpy(native_stopreq_ctx.path, path, len + 1u);
  else
    native_stopreq_ctx.path[0] = '\0';
  err = pthread_create(&native_stopreq_ctx.thread, NULL,
		       native_stopreq_thread, &native_stopreq_ctx);
  if (err != 0)
    return luaL_error(L, "pthread_create failed: %s", strerror(err));
  native_stopreq_ctx.active = 1;
  return 0;
}

static int start_fifo_stopreq_c(lua_State *L)
{
  return start_native_stopreq(L, luaL_checkstring(L, 1));
}

static int start_native_stopreq_c(lua_State *L)
{
  return start_native_stopreq(L, NULL);
}

static int join_native_stopreq_c(lua_State *L)
{
  int err;
  if (!native_stopreq_ctx.active)
    return luaL_error(L, "native STOPREQ helper is not active");
  err = pthread_join(native_stopreq_ctx.thread, NULL);
  native_stopreq_ctx.active = 0;
  if (err != 0)
    return luaL_error(L, "pthread_join failed: %s", strerror(err));
  if (native_stopreq_ctx.open_errno != 0)
    return luaL_error(L, "FIFO writer open failed: %s",
		      strerror(native_stopreq_ctx.open_errno));
  return 0;
}

static int join_fifo_stopreq_c(lua_State *L)
{
  return join_native_stopreq_c(L);
}

static int assert_not_native_c(lua_State *L);

static void *print_stopreq_thread(void *arg)
{
  PrintStopReqCtx *ctx = (PrintStopReqCtx *)arg;
  struct timespec delay;
  char buf[4096];
  int i;
  delay.tv_sec = 0;
  delay.tv_nsec = 1000000;
  for (i = 0; i < 1000 &&
       la_load8_acq(&ctx->tg->in_native) == 0; i++)
    (void)nanosleep(&delay, NULL);
  publish_manual(ctx->g, ctx->tg, LJ_GC2_HS_STOPREQ);
  while (la_load32_acq(&ctx->done) == 0) {
    ssize_t n = read(ctx->fd, buf, sizeof(buf));
    if (n > 0)
      continue;
    if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      (void)nanosleep(&delay, NULL);
      continue;
    }
    if (n == 0) {
      (void)nanosleep(&delay, NULL);
      continue;
    }
    ctx->err = errno;
    break;
  }
  return NULL;
}

static void *input_stopreq_thread(void *arg)
{
  InputStopReqCtx *ctx = (InputStopReqCtx *)arg;
  struct timespec delay;
  const char msg[] = "cont\n";
  ssize_t n;
  int i;
  delay.tv_sec = 0;
  delay.tv_nsec = 1000000;
  for (i = 0; i < 1000 &&
       la_load8_acq(&ctx->tg->in_native) == 0; i++)
    (void)nanosleep(&delay, NULL);
  publish_manual(ctx->g, ctx->tg, LJ_GC2_HS_STOPREQ);
  n = write(ctx->fd, msg, sizeof(msg) - 1u);
  if (n != (ssize_t)(sizeof(msg) - 1u))
    ctx->err = n == -1 ? errno : EIO;
  return NULL;
}

static void fill_pipe_until_full(int fd)
{
  char buf[4096];
  int flags = fcntl(fd, F_GETFL, 0);
  assert(flags != -1);
  memset(buf, 'p', sizeof(buf));
  assert(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
  for (;;) {
    ssize_t n = write(fd, buf, sizeof(buf));
    if (n == -1) {
      assert(errno == EAGAIN || errno == EWOULDBLOCK);
      break;
    }
    assert(n > 0);
  }
  assert(fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0);
}

static void test_print_stopreq(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  PrintStopReqCtx ctx;
  int pipefd[2];
  int saved_stdout;
  int flags;
  int err;
  int status;
  assert(tg != NULL);
  assert(pipe(pipefd) == 0);
  fill_pipe_until_full(pipefd[1]);
  flags = fcntl(pipefd[0], F_GETFL, 0);
  assert(flags != -1);
  assert(fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) == 0);
  assert(fflush(stdout) == 0);
  saved_stdout = dup(STDOUT_FILENO);
  assert(saved_stdout != -1);
  assert(dup2(pipefd[1], STDOUT_FILENO) != -1);
  (void)setvbuf(stdout, NULL, _IONBF, 0);

  memset(&ctx, 0, sizeof(ctx));
  ctx.g = g;
  ctx.tg = tg;
  ctx.fd = pipefd[0];
  err = pthread_create(&ctx.thread, NULL, print_stopreq_thread, &ctx);
  assert(err == 0);
  status = luaL_dostring(L, "print(string.rep('x', 4 * 1024 * 1024))");
  la_store32_rel(&ctx.done, 1);
  err = pthread_join(ctx.thread, NULL);
  assert(err == 0);
  assert(dup2(saved_stdout, STDOUT_FILENO) != -1);
  close(saved_stdout);
  close(pipefd[0]);
  close(pipefd[1]);
  clearerr(stdout);
  assert(ctx.err == 0);
  assert(status != LUA_OK);
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  clear_stopreq_c(L);
  assert_not_native_c(L);
}

static void test_debug_debug_stopreq(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  InputStopReqCtx ctx;
  int pipefd[2];
  int saved_stdin;
  int err;
  int status;
  assert(tg != NULL);
  assert(pipe(pipefd) == 0);
  saved_stdin = dup(STDIN_FILENO);
  assert(saved_stdin != -1);
  assert(dup2(pipefd[0], STDIN_FILENO) != -1);

  memset(&ctx, 0, sizeof(ctx));
  ctx.g = g;
  ctx.tg = tg;
  ctx.fd = pipefd[1];
  err = pthread_create(&ctx.thread, NULL, input_stopreq_thread, &ctx);
  assert(err == 0);
  status = luaL_dostring(L, "debug.debug()");
  err = pthread_join(ctx.thread, NULL);
  assert(err == 0);
  assert(dup2(saved_stdin, STDIN_FILENO) != -1);
  close(saved_stdin);
  close(pipefd[0]);
  close(pipefd[1]);
  assert(ctx.err == 0);
  assert(status != LUA_OK);
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  clear_stopreq_c(L);
  assert_not_native_c(L);
}

static int assert_not_native_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  assert(tg->in_native == 0);
  return 0;
}

static int assert_no_stopreq_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  assert(g->gc2.hs_pending == 0);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert((la_load8_acq(&tg->tg_flags) & TGF_STOPREQ) == 0);
  return 0;
}

#if LJ_HASFFI
static global_State *ffi_stopreq_g;
static TGState *ffi_stopreq_tg;
typedef int (*ffi_stopreq_cb_t)(void);

static int ffi_publish_stopreq(void)
{
  assert(ffi_stopreq_g != NULL);
  assert(ffi_stopreq_tg != NULL);
  publish_manual(ffi_stopreq_g, ffi_stopreq_tg, LJ_GC2_HS_STOPREQ);
  return 123;
}

static int ffi_call_callback_stopreq(ffi_stopreq_cb_t cb)
{
  assert(ffi_stopreq_g != NULL);
  assert(ffi_stopreq_tg != NULL);
  publish_manual(ffi_stopreq_g, ffi_stopreq_tg, LJ_GC2_HS_STOPREQ);
  return cb();
}
#endif

static int arena_list_contains(GCArena *a, GCArena *needle)
{
  while (a) {
    if (a == needle)
      return 1;
    a = lj_arena_next_acq(a);
  }
  return 0;
}

static int tg_list_contains(TGState *tg, TGState *needle)
{
  while (tg) {
    if (tg == needle)
      return 1;
    tg = lj_tg_next_acq(tg);
  }
  return 0;
}

static void assert_attach_phase(lua_State *L, global_State *g, TGState *main_tg,
				uint32_t phase, uint32_t mark_active,
				uint32_t alloc_black)
{
  TGState phase_tg;
  uint32_t oldphase = la_load32_acq(&g->gc2.phase);

  lj_tg_init_thread(g, &phase_tg, NULL, 0);
  phase_tg.tid = main_tg->tid + 2000u + phase;
  phase_tg.alloc.owner_tid = phase_tg.tid;
  phase_tg.cur_L = L;
  la_store32_rel(&g->gc2.phase, phase);
  lj_tg_attach(g, &phase_tg);
  assert(g->gc2.n_threads == 2);
  assert(tg_list_contains(g->gc2.tg_list, &phase_tg));
  assert(phase_tg.mark_active == mark_active);
  assert(phase_tg.alloc.alloc_black == alloc_black);
  assert(phase_tg.hs_epoch_ack == g->gc2.hs_epoch);
  lj_tg_detach(g, &phase_tg);
  assert(g->gc2.n_threads == 1);
  assert(phase_tg.tg_flags & TGF_DEAD);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(!tg_list_contains(g->gc2.tg_list, &phase_tg));
  la_store32_rel(&g->gc2.phase, oldphase);
  lj_tg_fini_thread(g, &phase_tg);
}

static void test_multistate_public_api_gc(lua_State *L1)
{
  TGState *tg1 = G2TG(G(L1));
  lua_State *L2;
  global_State *g2;
  TGState *tg2;
  uint64_t epoch0;
  uint32_t actions;

  assert(lj_thr_get_tg() == tg1);
  L2 = luaL_newstate();
  assert(L2 != NULL);
  assert(lj_thr_get_tg() == tg1);

  g2 = G(L2);
  tg2 = G2TG(g2);
  assert(tg2 != NULL);
  assert(tg2 != tg1);
  assert(g2->gc2.tg_list == tg2);

  epoch0 = g2->gc2.hs_epoch;
  actions = LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK;
  assert(lj_gc2_handshake(g2, actions) == 1);
  assert(g2->gc2.hs_epoch == epoch0 + 1u);
  assert(g2->gc2.hs_pending == 0);
  assert(tg2->poll == 0);
  assert(tg2->reqmask == 0);
  assert(tg2->hs_epoch_ack == g2->gc2.hs_epoch);

  assert(lj_gc2_test_finalizer_try_enter(g2));
  assert(la_load32_acq(&g2->gc2.finalizer_owner_tid) ==
	 la_load32_acq(&tg2->tid));
  lj_gc2_test_finalizer_leave(g2);
  assert(la_load32_acq(&g2->gc2.finalizer_owner_tid) == 0);

  luaL_openlibs(L2);
  assert(luaL_dostring(L2,
    "local hold = {}\n"
    "for i=1,20000 do hold[i] = {i, tostring(i)} end\n"
    "collectgarbage('collect')\n") == LUA_OK);
  lua_gc(L2, LUA_GCCOLLECT, 0);
  assert(lj_thr_get_tg() == tg1);
  lua_close(L2);
  assert(lj_thr_get_tg() == tg1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  TGState extra_tg;
  TGState arena_tg;
  GCtab *root_tab, *native_tab;
  void *plain_reset, *trav_reset;
  void *transfer_small, *transfer_huge;
  size_t transfer_huge_size = LJ_HUGE_THRESHOLD + 8192u;
  GCArena *plain_reset_a, *trav_reset_a;
  LJHugeInfo hi;
  uint32_t i, ssb_published0, ssb_drained0;
  uint64_t ssb_items_published0, ssb_items_drained0;
  uint64_t epoch0;
  uint64_t ack_samples0, ack_sum0, ack_max0;
  uint32_t actions;
  ASMFunction saved_dispatch;

  assert(L != NULL);
  luaL_openlibs(L);
  lua_pushcfunction(L, publish_alloc_white_c);
  lua_setglobal(L, "publish_alloc_white");
  lua_pushcfunction(L, publish_stopreq_c);
  lua_setglobal(L, "publish_stopreq");
  lua_pushcfunction(L, mark_sticky_stopreq_c);
  lua_setglobal(L, "mark_sticky_stopreq");
  lua_pushcfunction(L, assert_acked_alloc_white_c);
  lua_setglobal(L, "assert_acked_alloc_white");
  lua_pushcfunction(L, assert_acked_c);
  lua_setglobal(L, "assert_acked");
  lua_pushcfunction(L, clear_stopreq_c);
  lua_setglobal(L, "clear_stopreq");
  lua_pushcfunction(L, assert_not_native_c);
  lua_setglobal(L, "assert_not_native");
  lua_pushcfunction(L, assert_no_stopreq_c);
  lua_setglobal(L, "assert_no_stopreq");
  lua_pushcfunction(L, mkfifo_test_c);
  lua_setglobal(L, "mkfifo_test");
  lua_pushcfunction(L, start_fifo_stopreq_c);
  lua_setglobal(L, "start_fifo_stopreq");
  lua_pushcfunction(L, join_fifo_stopreq_c);
  lua_setglobal(L, "join_fifo_stopreq");
  lua_pushcfunction(L, start_native_stopreq_c);
  lua_setglobal(L, "start_native_stopreq");
  lua_pushcfunction(L, join_native_stopreq_c);
  lua_setglobal(L, "join_native_stopreq");
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert(g->gc2.tg_list == tg);
  assert(g->gc2.n_threads == 1);
  assert(g->gc2.hs_epoch == 0);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.ssb_head == NULL);
  assert(gc2_ssb_published_acq(g) == 0);
  assert(gc2_ssb_items_published_acq(g) == 0);
  assert(lj_gc2_ssb_empty(g));
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->hs_epoch_ack == 0);
  assert(tg->ssb_active == &tg->ssb_node[0]);
  assert(tg->ssb_free == &tg->ssb_node[1]);
  assert(tg->ssb_base == tg->ssb_node[0].slot);
  assert(tg->ssb_next == tg->ssb_base);
  assert(tg->ssb_end == tg->ssb_base + TG_GC2_SSB_SLOTS);

  assert_attach_phase(L, g, tg, LJ_GC2_IDLE, 0, 0);
  assert_attach_phase(L, g, tg, LJ_GC2_MARK, 1, 1);
  assert_attach_phase(L, g, tg, LJ_GC2_WEAK, 1, 1);
  assert_attach_phase(L, g, tg, LJ_GC2_SWEEP, 0, 1);
  test_multistate_public_api_gc(L);

  epoch0 = g->gc2.hs_epoch;
  ack_samples0 = la_load64_acq(&g->gc2.hs_ack_latency_samples);
  ack_sum0 = la_load64_acq(&g->gc2.hs_ack_latency_sum_ns);
  ack_max0 = la_load64_acq(&g->gc2.hs_ack_latency_max_ns);
  actions = LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.hs_actions == actions);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->hs_epoch_ack == g->gc2.hs_epoch);
  assert(tg->mark_active == 1);
  assert(tg->alloc.alloc_black == 1);
  assert(la_load64_acq(&g->gc2.hs_ack_latency_samples) == ack_samples0);
  assert(la_load64_acq(&g->gc2.hs_ack_latency_sum_ns) == ack_sum0);
  assert(la_load64_acq(&g->gc2.hs_ack_latency_max_ns) == ack_max0);

  actions = LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_pending == 0);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);

  saved_dispatch = tg->dispatch[BC_RET];
  assert(saved_dispatch == G2GG(g)->dispatch[BC_RET]);
  tg->dispatch[BC_RET] = NULL;
  assert(tg->dispatch[BC_RET] != G2GG(g)->dispatch[BC_RET]);
  actions = LJ_GC2_HS_REDISPATCH;
  epoch0 = g->gc2.hs_epoch;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.hs_actions == actions);
  assert(tg->dispatch[BC_RET] == saved_dispatch);
  assert(tg->dispatch[BC_RET] == G2GG(g)->dispatch[BC_RET]);

  assert((tg->tg_flags & TGF_STOPREQ) == 0);
  actions = LJ_GC2_HS_STOPREQ;
  epoch0 = g->gc2.hs_epoch;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.hs_actions == actions);
  assert((tg->tg_flags & TGF_STOPREQ) != 0);
  tg->tg_flags &= (uint8_t)~TGF_STOPREQ;

#if LJ_HASJIT
  assert(luaL_dostring(L,
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local s = 0\n"
    "for i = 1, 200 do s = s + i end\n"
    "return s\n") == LUA_OK);
  lua_pop(L, 1);
  assert(traceref(G2J(g), 1) != NULL || G2J(g)->freetrace > 0);
  epoch0 = g->gc2.hs_epoch;
  actions = LJ_GC2_HS_FLUSHJ;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.hs_actions == actions);
  assert(G2J(g)->cur.traceno == 0);
  assert(G2J(g)->freetrace == 0);

  assert(lj_trace_state_load(G2J(g)) == LJ_TRACE_IDLE);
  lj_trace_state_store_active(G2J(g), LJ_TRACE_RECORD);
  assert(lj_trace_state_load(G2J(g)) == LJ_TRACE_RECORD);
  epoch0 = g->gc2.hs_epoch;
  actions = LJ_GC2_HS_EXIT_TRACES;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.hs_actions == actions);
  assert((lj_trace_state_load(G2J(g)) & LJ_TRACE_ACTIVE) == 0);
  lj_trace_state_store(G2J(g), LJ_TRACE_IDLE);
#endif

  lua_newtable(L);
  root_tab = tabV(L->top - 1);
  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(root_tab)) == 0);
  epoch0 = g->gc2.hs_epoch;
  assert(lj_gc2_handshake(g, LJ_GC2_HS_SCAN_ROOTS) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(lj_gc2_ismarked(g, obj2gco(root_tab)) == 1);
  assert(la_load64_acq(&g->gc2.marks_this_round) > 0);
  assert(!lj_gc2_ssb_empty(g));
  assert(lj_gc2_flush_ssb(g, tg) > 0);
  assert(lj_gc2_drain_ssb(g) > 0);
  assert(lj_gc2_ssb_empty(g));
  ssb_published0 = gc2_ssb_published_acq(g);
  ssb_drained0 = gc2_ssb_drained_acq(g);
  ssb_items_published0 = gc2_ssb_items_published_acq(g);
  ssb_items_drained0 = gc2_ssb_items_drained_acq(g);
  assert(lj_gc2_ssb_push(g, obj2gco(root_tab)) == 1);
  assert(lj_gc2_ssb_push(g, obj2gco(root_tab)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  assert(tg->ssb_next == tg->ssb_base + 2);
  epoch0 = g->gc2.hs_epoch;
  assert(lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(tg->ssb_next == tg->ssb_base);
  assert(g->gc2.ssb_head != NULL);
  assert(!lj_gc2_ssb_empty(g));
  assert(gc2_ssb_published_acq(g) == ssb_published0 + 1u);
  assert(gc2_ssb_items_published_acq(g) == ssb_items_published0 + 2u);
  assert(lj_gc2_drain_ssb(g) == 2);
  assert(g->gc2.ssb_head == NULL);
  assert(lj_gc2_ssb_empty(g));
  assert(gc2_ssb_drained_acq(g) == ssb_drained0 + 1u);
  assert(gc2_ssb_items_drained_acq(g) == ssb_items_drained0 + 2u);
  ssb_drained0 = gc2_ssb_drained_acq(g);
  ssb_items_drained0 = gc2_ssb_items_drained_acq(g);
  for (i = 0; i < TG_GC2_SSB_SLOTS; i++)
    assert(lj_gc2_ssb_push(g, obj2gco(root_tab)) == 1);
  assert(tg->ssb_next == tg->ssb_end);
  ssb_published0 = gc2_ssb_published_acq(g);
  assert(lj_gc2_ssb_push(g, obj2gco(root_tab)) == 1);
  assert(gc2_ssb_published_acq(g) == ssb_published0 + 1u);
  assert(tg->ssb_next == tg->ssb_base + 1);
  assert(lj_gc2_drain_ssb(g) == TG_GC2_SSB_SLOTS);
  assert(g->gc2.ssb_head == NULL);
  assert(gc2_ssb_drained_acq(g) == ssb_drained0 + 1u);
  assert(gc2_ssb_items_drained_acq(g) ==
	 ssb_items_drained0 + TG_GC2_SSB_SLOTS);
  ssb_drained0 = gc2_ssb_drained_acq(g);
  ssb_items_drained0 = gc2_ssb_items_drained_acq(g);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(tg->ssb_next == tg->ssb_base);
  assert(!lj_gc2_ssb_empty(g));
  assert(lj_gc2_drain_ssb(g) == 1);
  assert(lj_gc2_ssb_empty(g));
  assert(gc2_ssb_drained_acq(g) == ssb_drained0 + 1u);
  assert(gc2_ssb_items_drained_acq(g) == ssb_items_drained0 + 1u);
  lua_pop(L, 1);
  lj_gc2_legacy_cycle_end(g);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);

  lua_newtable(L);
  native_tab = tabV(L->top - 1);
  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(native_tab)) == 0);
  lj_native_enter(tg);
  assert(tg->in_native == 1);
  epoch0 = g->gc2.hs_epoch;
  assert(lj_gc2_handshake(g, LJ_GC2_HS_SCAN_ROOTS) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(tg->in_native == 1);
  assert(lj_gc2_ismarked(g, obj2gco(native_tab)) == 1);
  assert(lj_native_leave(L) == 0);
  assert(tg->in_native == 0);
  assert(lj_gc2_flush_ssb(g, tg) > 0);
  assert(lj_gc2_drain_ssb(g) > 0);
  assert(lj_gc2_ssb_empty(g));
  lua_pop(L, 1);
  lj_gc2_legacy_cycle_end(g);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);

  ack_samples0 = la_load64_acq(&g->gc2.hs_ack_latency_samples);
  ack_sum0 = la_load64_acq(&g->gc2.hs_ack_latency_sum_ns);
  ack_max0 = la_load64_acq(&g->gc2.hs_ack_latency_max_ns);
  publish_manual(g, tg, LJ_GC2_HS_ENABLE_BARRIER);
  assert(lj_safepoint_poll(L) == LJ_GC2_HS_ENABLE_BARRIER);
  assert(g->gc2.hs_pending == 0);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->mark_active == 1);
  assert(la_load64_acq(&g->gc2.hs_ack_latency_samples) == ack_samples0 + 1u);
  assert(la_load64_acq(&g->gc2.hs_ack_latency_sum_ns) >= ack_sum0);
  assert(la_load64_acq(&g->gc2.hs_ack_latency_max_ns) >= ack_max0);
  assert(lj_safepoint_poll(L) == 0);

  lj_native_enter(tg);
  assert(tg->in_native == 1);
  actions = LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_pending == 0);
  assert(tg->in_native == 1);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);
  assert(lj_native_leave(L) == 0);
  assert(tg->in_native == 0);

  lj_native_enter(tg);
  publish_manual(g, tg, LJ_GC2_HS_ALLOC_BLACK);
  assert(lj_native_leave(L) == LJ_GC2_HS_ALLOC_BLACK);
  assert(tg->in_native == 0);
  assert(g->gc2.hs_pending == 0);
  assert(tg->alloc.alloc_black == 1);

  assert(lj_gc2_handshake(g, 0) == 0);

  assert(luaL_dostring(L,
    "local p = os.tmpname()\n"
    "local q = p .. '.renamed'\n"
    "local f = assert(io.open(p, 'w'))\n"
    "f:write('x')\n"
    "f:close()\n"
    "publish_alloc_white()\n"
    "assert(os.rename(p, q))\n"
    "assert_acked_alloc_white()\n"
    "publish_alloc_white()\n"
    "assert(os.remove(q))\n"
    "assert_acked_alloc_white()\n"
    "publish_alloc_white()\n"
    "local r = os.tmpname()\n"
    "assert_acked_alloc_white()\n"
    "os.remove(r)\n") == LUA_OK);

  if (luaL_dostring(L,
    "local stopreq_case = 0\n"
    "local function expect_stopreq(fn)\n"
    "  stopreq_case = stopreq_case + 1\n"
    "  publish_stopreq()\n"
    "  local ok, err = pcall(fn)\n"
    "  assert(not ok, 'expected STOPREQ case ' .. stopreq_case)\n"
    "  assert(tostring(err):find('thread interrupted: VM shutdown', 1, true))\n"
    "  clear_stopreq()\n"
    "  assert_no_stopreq()\n"
    "end\n"
    "local function expect_sticky_ok(fn, cleanup)\n"
    "  stopreq_case = stopreq_case + 1\n"
    "  mark_sticky_stopreq()\n"
    "  local ok, res = pcall(fn)\n"
    "  assert(ok, 'unexpected sticky STOPREQ interruption case ' .. stopreq_case .. ': ' .. tostring(res))\n"
    "  clear_stopreq()\n"
    "  if cleanup then cleanup(res) end\n"
    "  assert_no_stopreq()\n"
    "end\n"
    "local function expect_fopen_stopreq(fn)\n"
    "  stopreq_case = stopreq_case + 1\n"
    "  local fifo = os.tmpname()\n"
    "  os.remove(fifo)\n"
    "  assert(mkfifo_test(fifo))\n"
    "  start_fifo_stopreq(fifo)\n"
    "  local ok, err = pcall(function() return fn(fifo) end)\n"
    "  join_fifo_stopreq()\n"
    "  assert(not ok, 'expected STOPREQ case ' .. stopreq_case)\n"
    "  assert(tostring(err):find('thread interrupted: VM shutdown', 1, true))\n"
    "  clear_stopreq()\n"
    "  assert_no_stopreq()\n"
    "  os.remove(fifo)\n"
    "end\n"
    "local function expect_native_stopreq(fn)\n"
    "  stopreq_case = stopreq_case + 1\n"
    "  start_native_stopreq()\n"
    "  local ok, err = pcall(fn)\n"
    "  join_native_stopreq()\n"
    "  assert(not ok, 'expected STOPREQ case ' .. stopreq_case)\n"
    "  assert(tostring(err):find('thread interrupted: VM shutdown', 1, true))\n"
    "  clear_stopreq()\n"
    "  assert_no_stopreq()\n"
    "end\n"
    "local function expect_loadlib_stopreq()\n"
    "  local so = os.getenv('LJ_LOADLIB_STOPREQ_SO')\n"
    "  if not so or so == '' then return end\n"
    "  expect_native_stopreq(function()\n"
    "    return package.loadlib(so, 'luaopen_lj_loadlib_stopreq')\n"
    "  end)\n"
    "  local ffi = require('ffi')\n"
    "  expect_native_stopreq(function() return ffi.load(so) end)\n"
    "end\n"
    "local p = os.tmpname()\n"
    "local q = p .. '.stopreq'\n"
    "local f = assert(io.open(p, 'w'))\n"
    "f:write('x')\n"
    "f:close()\n"
    "expect_stopreq(function() return os.rename(p, q) end)\n"
    "os.remove(p)\n"
    "f = assert(io.open(q, 'w'))\n"
    "f:write('y')\n"
    "f:close()\n"
    "expect_stopreq(function() return os.remove(q) end)\n"
    "os.remove(q)\n"
    "expect_stopreq(function() return os.execute(':') end)\n"
    "expect_stopreq(function() return os.tmpname() end)\n"
    "expect_stopreq(function() return io.tmpfile() end)\n"
    "expect_sticky_ok(function() return os.tmpname() end, function(name) os.remove(name) end)\n"
    "expect_sticky_ok(function() return io.tmpfile() end, function(file) file:close() end)\n"
    "p = os.tmpname()\n"
    "f = assert(io.open(p, 'w'))\n"
    "f:write('z')\n"
    "expect_stopreq(function() return f:flush() end)\n"
    "expect_stopreq(function() return f:seek('set', 0) end)\n"
    "f:close()\n"
    "os.remove(p)\n"
    "p = os.tmpname()\n"
    "f = assert(io.open(p, 'w'))\n"
    "f:write('12\\nabc\\nxyz')\n"
    "f:close()\n"
    "expect_fopen_stopreq(function(fifo) return io.open(fifo, 'r') end)\n"
    "expect_fopen_stopreq(function(fifo) return io.lines(fifo) end)\n"
    "expect_fopen_stopreq(function(fifo) return loadfile(fifo) end)\n"
    "expect_fopen_stopreq(function(fifo) return dofile(fifo) end)\n"
    "expect_fopen_stopreq(function(fifo)\n"
    "  return package.searchpath('lj_stopreq_probe', fifo)\n"
    "end)\n"
    "f = assert(io.open(p, 'r'))\n"
    "expect_stopreq(function() return f:read('*n') end)\n"
    "f:seek('set', 0)\n"
    "expect_stopreq(function() return f:read('*l') end)\n"
    "f:seek('set', 0)\n"
    "expect_stopreq(function() return f:read(1) end)\n"
    "f:seek('set', 0)\n"
    "expect_stopreq(function() return f:read('*a') end)\n"
    "f:seek('set', 0)\n"
    "expect_stopreq(function() return f:read(0) end)\n"
    "f:close()\n"
    "local pipe = assert(io.popen('sleep 0.2', 'r'))\n"
    "expect_native_stopreq(function() return pipe:close() end)\n"
    "local write_pipe = assert(io.popen(\"sh -c 'sleep 0.2; cat >/dev/null'\", 'w'))\n"
    "local big = string.rep('w', 4 * 1024 * 1024)\n"
    "expect_native_stopreq(function() return write_pipe:write(big) end)\n"
    "write_pipe:close()\n"
    "expect_loadlib_stopreq()\n"
    "os.remove(p)\n") != LUA_OK) {
    fprintf(stderr, "STOPREQ coverage chunk failed: %s\n",
	    lua_tostring(L, -1));
    assert(0);
  }

  test_print_stopreq(L);
  test_debug_debug_stopreq(L);

  assert(luaL_dostring(L,
    "local th = require('threading')\n"
    "local worker = th.spawn(function() return 'joined' end)\n"
    "while worker:running() do th.sleep(0.001) end\n"
    "publish_stopreq()\n"
    "local ok, err = pcall(function() return worker:join() end)\n"
    "assert(not ok)\n"
    "assert(tostring(err):find('thread interrupted: VM shutdown', 1, true), tostring(err))\n"
    "clear_stopreq()\n"
    "local jok, value = worker:join()\n"
    "assert(jok == true and value == 'joined')\n") == LUA_OK);

#if LJ_HASFFI
#if LJ_HASJIT
  assert(lj_trace_state_load(G2J(g)) == LJ_TRACE_IDLE);
  assert(jit_token_acq(g) == 0);
#endif
  ffi_stopreq_g = g;
  ffi_stopreq_tg = tg;
  lua_pushlightuserdata(L, (void *)ffi_publish_stopreq);
  lua_setglobal(L, "ffi_stopreq_ptr");
  lua_pushlightuserdata(L, (void *)ffi_call_callback_stopreq);
  lua_setglobal(L, "ffi_call_callback_stopreq_ptr");
  if (luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "int getpid(void);\n"
    "typedef int (*cmp_t)(const void *, const void *);\n"
    "typedef int (*stopreq_t)(void);\n"
    "typedef int (*cb_stopreq_t)(void);\n"
    "typedef int (*call_cb_stopreq_t)(cb_stopreq_t);\n"
    "void qsort(void *base, unsigned long nmemb, unsigned long size,\n"
    "           cmp_t compar);\n"
    "]]\n"
    "publish_alloc_white()\n"
    "ffi.C.getpid()\n"
    "assert_acked_alloc_white()\n"
    "local arr = ffi.new('int[2]', {2, 1})\n"
    "local cmp\n"
    "cmp = ffi.cast('cmp_t', function(a, b)\n"
    "  assert_not_native()\n"
    "  local ia = ffi.cast('const int *', a)[0]\n"
    "  local ib = ffi.cast('const int *', b)[0]\n"
    "  return ia - ib\n"
    "end)\n"
    "publish_alloc_white()\n"
    "ffi.C.qsort(arr, 2, ffi.sizeof('int'), cmp)\n"
    "assert_acked()\n"
    "cmp:free()\n"
    "assert(arr[0] == 1 and arr[1] == 2)\n"
    "local stopreq = ffi.cast('stopreq_t', ffi_stopreq_ptr)\n"
    "local ok, err = pcall(function() return stopreq() end)\n"
    "assert(not ok)\n"
    "assert(tostring(err):find('thread interrupted: VM shutdown', 1, true), tostring(err))\n"
    "clear_stopreq()\n"
    "local call_cb_stopreq = ffi.cast('call_cb_stopreq_t', ffi_call_callback_stopreq_ptr)\n"
    "local entered = false\n"
    "local cb_stopreq = ffi.cast('cb_stopreq_t', function()\n"
    "  entered = true\n"
    "  return 77\n"
    "end)\n"
    "ok, err = pcall(function() return call_cb_stopreq(cb_stopreq) end)\n"
    "assert(not ok)\n"
    "assert(not entered)\n"
    "assert(tostring(err):find('thread interrupted: VM shutdown', 1, true), tostring(err))\n"
    "clear_stopreq()\n"
    "cb_stopreq:free()\n") != LUA_OK) {
    fprintf(stderr, "FFI STOPREQ coverage chunk failed: %s "
	    "(flags=%u poll=%u req=%u pending=%u ack=%llu epoch=%llu)\n",
	    lua_tostring(L, -1), (unsigned)la_load8_acq(&tg->tg_flags),
	    (unsigned)la_load32_acq(&tg->poll),
	    (unsigned)la_load32_acq(&tg->reqmask),
	    (unsigned)la_load32_acq(&g->gc2.hs_pending),
	    (unsigned long long)la_load64_acq(&tg->hs_epoch_ack),
	    (unsigned long long)la_load64_acq(&g->gc2.hs_epoch));
    assert(0);
  }
#endif

  plain_reset = lj_arena_alloc(&tg->alloc, &tg->prng, 64, 0);
  trav_reset = lj_arena_alloc(&tg->alloc, &tg->prng, 64,
			      LJ_AF_TRAVERSABLE);
  assert(plain_reset != NULL);
  assert(trav_reset != NULL);
  plain_reset_a = lj_arena_of(plain_reset);
  trav_reset_a = lj_arena_of(trav_reset);
  actions = LJ_GC2_HS_RESET_ALLOC;
  epoch0 = g->gc2.hs_epoch;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.hs_actions == actions);
  assert(tg->alloc.bump[LJ_ARENAK_PLAIN].a != NULL);
  assert(tg->alloc.bump[LJ_ARENAK_TRAVERSABLE].a == NULL);
  assert(arena_list_contains(tg->alloc.owned[LJ_ARENAK_PLAIN],
			     plain_reset_a));
  assert(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(arena_list_contains(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     trav_reset_a));
  assert((plain_reset_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((trav_reset_a->hdr.flags & LJ_AF_NEEDSWEEP) != 0);
  lj_arena_alloc_restore_sweep_kind(&tg->alloc, LJ_ARENAK_TRAVERSABLE);
  lj_arena_alloc_restore_sweep_kind(&tg->alloc, LJ_ARENAK_PLAIN);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  lj_arena_free(&tg->alloc, plain_reset, 64);
  lj_arena_free(&tg->alloc, trav_reset, 64);

  lj_tg_init_thread(g, &extra_tg, NULL, 0);
  extra_tg.cur_L = L;
  lj_native_enter(&extra_tg);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);
  assert(tg_list_contains(g->gc2.tg_list, tg));
  assert(tg_list_contains(g->gc2.tg_list, &extra_tg));
  assert(!(extra_tg.tg_flags & TGF_DEAD));
  assert(extra_tg.hs_epoch_ack == g->gc2.hs_epoch);

  epoch0 = g->gc2.hs_epoch;
  ack_samples0 = la_load64_acq(&g->gc2.hs_ack_latency_samples);
  actions = LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK;
  assert(lj_gc2_handshake(g, actions) == 2);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(extra_tg.poll == 0);
  assert(extra_tg.reqmask == 0);
  assert(extra_tg.hs_epoch_ack == g->gc2.hs_epoch);
  assert(extra_tg.mark_active == 1);
  assert(extra_tg.alloc.alloc_black == 1);
  assert(extra_tg.in_native == 1);
  assert(la_load64_acq(&g->gc2.hs_ack_latency_samples) == ack_samples0);

  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(extra_tg.tg_flags & TGF_DEAD);
  assert(extra_tg.in_native == 0);
  epoch0 = g->gc2.hs_epoch;
  actions = LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(extra_tg.hs_epoch_ack == epoch0);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(!tg_list_contains(g->gc2.tg_list, &extra_tg));
  lj_tg_fini_thread(g, &extra_tg);

  lj_tg_init_thread(g, &arena_tg, NULL, 1);
  arena_tg.tid = tg->tid + 1000u;
  arena_tg.alloc.owner_tid = arena_tg.tid;
  transfer_small = lj_arena_allocf(&arena_tg.allocd, NULL, 0, 64);
  transfer_huge = lj_arena_allocf(&arena_tg.allocd, NULL, 0,
				  transfer_huge_size);
  assert(transfer_small != NULL);
  assert(transfer_huge != NULL);
  assert(lj_arena_of(transfer_small)->hdr.owner_tid == arena_tg.tid);
  assert(lj_arena_of(transfer_huge)->hdr.owner_tid == arena_tg.tid);
  assert(lj_arena_hugetab_lookup(&arena_tg.huge, transfer_huge, &hi) == 1);
  assert(hi.size == transfer_huge_size);
  lj_tg_attach(g, &arena_tg);
  assert(g->gc2.n_threads == 2);
  assert(tg_list_contains(g->gc2.tg_list, &arena_tg));
  lj_tg_detach(g, &arena_tg);
  assert(g->gc2.n_threads == 1);
  assert(arena_tg.tg_flags & TGF_DEAD);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(!tg_list_contains(g->gc2.tg_list, &arena_tg));
  assert((arena_tg.tg_flags & TGF_ARENA_INTERNAL) == 0);
  assert((arena_tg.tg_flags & TGF_HUGETAB) == 0);
  assert(arena_tg.huge.h == NULL);
  assert(lj_arena_of(transfer_small)->hdr.owner_tid == tg->alloc.owner_tid);
  assert(lj_arena_of(transfer_huge)->hdr.owner_tid == tg->alloc.owner_tid);
  assert(lj_arena_hugetab_lookup(&tg->huge, transfer_huge, &hi) == 1);
  assert(hi.size == transfer_huge_size);
  assert(lj_arena_allocf(&tg->allocd, transfer_small, 64, 0) == NULL);
  assert(lj_arena_allocf(&tg->allocd, transfer_huge, transfer_huge_size, 0) ==
	 NULL);
  lj_tg_fini_thread(g, &arena_tg);

  lua_close(L);

  printf("t-safepoint-handshake OK: C soft handshakes verified\n");
  return 0;
}
