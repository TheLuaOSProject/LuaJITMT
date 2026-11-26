/*
** Shared upvalue mutation while a peer is suspended inside the recorder.
*/

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_obj.h"
#include "lj_thr.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if LJ_HAS_X64_MT_JIT_HELPERS && LJ_HASJIT && defined(LJ_TRACE_TEST_HELPERS)
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int ready, start, paused, resume;
static lua_State *peer;
static GCproto *peer_pt;

static void park_recorder(jit_State *J, const BCIns *pc)
{
  if (J->pt != peer_pt || bc_op(*pc) != BC_UGET)
    return;
  assert(jit_owner_l_acq(J) == peer);
  assert(lj_jit_token_held(J));
  assert(pthread_mutex_lock(&mutex) == 0);
  if (!paused) {
    paused = 1;
    assert(pthread_cond_broadcast(&cond) == 0);
    while (!resume)
      assert(pthread_cond_wait(&cond, &mutex) == 0);
  }
  assert(pthread_mutex_unlock(&mutex) == 0);
}

static void *run_peer(void *unused)
{
  (void)unused;
  assert(lj_threading_attach(peer));
  assert(pthread_mutex_lock(&mutex) == 0);
  ready = 1;
  assert(pthread_cond_broadcast(&cond) == 0);
  while (!start)
    assert(pthread_cond_wait(&cond, &mutex) == 0);
  assert(pthread_mutex_unlock(&mutex) == 0);
  lua_pushinteger(peer, 100);
  ljt_lua_pcall(peer, 1, 1, "record peer loop");
  assert(lua_tointeger(peer, -1) == 5);
  lj_threading_detach(peer, 1);
  return NULL;
}

typedef struct NativeWitness {
  TGState *tg;
  TraceNo trace;
  uint32_t entered;
  uint32_t stop;
} NativeWitness;

static void *observe_native(void *arg)
{
  NativeWitness *witness = arg;
  while (!la_load32_acq(&witness->stop)) {
    if (lj_tg_vmstate_load_acq(witness->tg) == (int32_t)witness->trace) {
      la_store32_rel(&witness->entered, 1);
      break;
    }
    sched_yield();
  }
  return NULL;
}

static void check_native(lua_State *L, TraceNo tr, int step)
{
  NativeWitness witness = { L2TG(L), tr, 0, 0 };
  pthread_t observer;
  int n = 200000;
  assert(pthread_create(&observer, NULL, observe_native, &witness) == 0);
  for (int i=0; i<16 && !la_load32_acq(&witness.entered); i++) {
    lua_pushvalue(L, 1);
    lua_pushinteger(L, n);
    ljt_lua_pcall(L, 1, 1, "execute compiled upvalue load");
    assert(lua_tointeger(L, -1) == n * step);
    lua_pop(L, 1);
  }
  la_store32_rel(&witness.stop, 1);
  assert(pthread_join(observer, NULL) == 0);
  assert(la_load32_acq(&witness.entered));
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  const char *name;
  GCproto *pt;
  TraceNo tr;
  pthread_t thread;
  alarm(15);
  lua_gc(L, LUA_GCSTOP, 0);
  ljt_lua_dostring(L,
    "local value = 3\n"
    "local function sum(n)\n"
    "  local total = 0\n"
    "  for i=1,n do\n"
    "    total = total + value\n"
    "  end\n"
    "  return total\n"
    "end\n"
    "local function record(n)\n"
    "  local last\n"
    "  for i=1,n do last = value end\n"
    "  return last\n"
    "end\n"
    "jit.flush(); jit.opt.start('hotloop=1', 'hotexit=1000')\n"
    "return sum, record");
  pt = funcproto(funcV(L->top-2));
  peer_pt = funcproto(funcV(L->top-1));
  peer = lua_newthread(L);
  lua_pushvalue(L, 2);
  lua_xmove(L, peer, 1);
  lj_trace_test_set_recorded_hook(park_recorder);
  assert(pthread_create(&thread, NULL, run_peer, NULL) == 0);
  assert(pthread_mutex_lock(&mutex) == 0);
  while (!ready)
    assert(pthread_cond_wait(&cond, &mutex) == 0);
  assert(pthread_mutex_unlock(&mutex) == 0);

  for (int i=0; i<10; i++) {
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 80);
    ljt_lua_pcall(L, 1, 1, "warm shared upvalue load");
    assert(lua_tointeger(L, -1) == 240);
    lua_pop(L, 1);
  }
  tr = proto_trace_acq(pt);
  assert(tr != 0);
  check_native(L, tr, 3);

  assert(pthread_mutex_lock(&mutex) == 0);
  start = 1;
  assert(pthread_cond_broadcast(&cond) == 0);
  while (!paused)
    assert(pthread_cond_wait(&cond, &mutex) == 0);
  assert(pthread_mutex_unlock(&mutex) == 0);

  assert(lua_upvalueid(L, 1, 1) == lua_upvalueid(L, 2, 1));
  lua_pushnil(L);
  name = lua_setupvalue(L, 1, 1);
  assert(name && strcmp(name, "value") == 0);
  lua_pushvalue(L, 1);
  lua_pushinteger(L, 80);
  assert(lua_pcall(L, 1, 1, 0) == LUA_ERRRUN);
  lua_pop(L, 1);
  lua_pushinteger(L, 5);
  name = lua_setupvalue(L, 1, 1);
  assert(name && strcmp(name, "value") == 0);
  assert(proto_trace_acq(pt) == tr);
  check_native(L, tr, 5);

  assert(pthread_mutex_lock(&mutex) == 0);
  resume = 1;
  assert(pthread_cond_broadcast(&cond) == 0);
  assert(pthread_mutex_unlock(&mutex) == 0);
  assert(pthread_join(thread, NULL) == 0);
  lj_trace_test_set_recorded_hook(NULL);
  lua_close(L);
  alarm(0);
  puts("t-jit-upvalue-mutation-progress OK");
  return 0;
}
#else
int main(void)
{
  puts("t-jit-upvalue-mutation-progress SKIP: requires Linux x64 trace helpers");
  return 0;
}
#endif
