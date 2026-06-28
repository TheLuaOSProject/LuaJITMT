/*
** Linux perf-map writer native-state STOPREQ guard.
*/

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

typedef struct PerfStopCtx {
  global_State *g;
  TGState *tg;
  char path[128];
  uint32_t done;
  uint32_t saw_native;
  uint32_t handshook;
  uint32_t signaled;
  uint32_t stopreq_seen;
  int open_errno;
} PerfStopCtx;

static void *perf_reader_thread(void *arg)
{
  PerfStopCtx *ctx = (PerfStopCtx *)arg;
  int fd, i;
  for (i = 0; i < 5000; i++) {
    if (lj_tg_in_native_acq(ctx->tg)) {
      la_store32_rel(&ctx->saw_native, 1);
      break;
    }
    usleep(1000);
  }
  if (!la_load32_acq(&ctx->saw_native)) {
    ctx->open_errno = ETIMEDOUT;
    return NULL;
  }

  la_store32_rel(&ctx->signaled,
		 lj_safepoint_handshake(ctx->g, LJ_GC2_HS_STOPREQ));
  la_store32_rel(&ctx->stopreq_seen,
		 (la_load8_acq(&ctx->tg->tg_flags) & TGF_STOPREQ) != 0);
  la_store32_rel(&ctx->handshook, 1);

  fd = open(ctx->path, O_RDONLY);
  if (fd < 0) {
    ctx->open_errno = errno;
    return NULL;
  }
  while (!la_load32_acq(&ctx->done))
    usleep(1000);
  close(fd);
  return NULL;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  PerfStopCtx ctx;
  pthread_t reader;
  int rc;
  const char *err;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);

  memset(&ctx, 0, sizeof(ctx));
  ctx.g = g;
  ctx.tg = tg;
  snprintf(ctx.path, sizeof(ctx.path), "/tmp/perf-%d.map", (int)getpid());
  unlink(ctx.path);
  assert(mkfifo(ctx.path, 0600) == 0);
  assert(pthread_create(&reader, NULL, perf_reader_thread, &ctx) == 0);

  alarm(20);
  rc = luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 20 do assert(f(80) == 3240) end\n");
  la_store32_rel(&ctx.done, 1);
  assert(pthread_join(reader, NULL) == 0);
  alarm(0);

  assert(ctx.open_errno == 0);
  assert(la_load32_acq(&ctx.saw_native) != 0);
  assert(la_load32_acq(&ctx.handshook) != 0);
  assert(la_load32_acq(&ctx.signaled) != 0);
  assert(la_load32_acq(&ctx.stopreq_seen) != 0);
  assert(rc != LUA_OK);
  err = lua_tostring(L, -1);
  assert(err && strstr(err, "thread interrupted: VM shutdown"));
  lua_pop(L, 1);

  tg->tg_flags &= (uint8_t)~TGF_STOPREQ;
  lua_close(L);
  unlink(ctx.path);
  printf("t-jit-perftools-native OK: perf map writer acks STOPREQ as native\n");
  return 0;
}
