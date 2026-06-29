/*
** Concurrent lua_setupvalue() publication for a shared C closure upvalue.
*/

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define READERS 3
#define WRITES 512
#define MIN_READS 256
#define CHECK_MASK 0x5a5a5a

typedef struct WriterCtx {
  lua_State *L;
  int status;
  volatile int *ready;
  volatile int *start;
  volatile int *done;
} WriterCtx;

typedef struct ReaderCtx {
  lua_State *L;
  int id;
  int status;
  long calls;
  volatile int *ready;
  volatile int *start;
  volatile int *done;
} ReaderCtx;

static int load_flag(volatile int *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_flag(volatile int *p, int v)
{
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static void add_flag(volatile int *p, int v)
{
  (void)__atomic_add_fetch(p, v, __ATOMIC_ACQ_REL);
}

static void push_payload(lua_State *L, int seq)
{
  char tag[64];

  snprintf(tag, sizeof(tag), "payload:%d", seq);

  lua_newtable(L);
  lua_pushinteger(L, seq);
  lua_setfield(L, -2, "seq");
  lua_pushinteger(L, seq ^ CHECK_MASK);
  lua_setfield(L, -2, "check");
  lua_pushstring(L, tag);
  lua_setfield(L, -2, "tag");

  lua_newtable(L);
  lua_pushinteger(L, seq);
  lua_setfield(L, -2, "seq");
  lua_pushinteger(L, seq ^ CHECK_MASK);
  lua_setfield(L, -2, "check");
  lua_setfield(L, -2, "nested");

  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "self");
}

static void assert_payload(lua_State *L, int idx)
{
  char want_tag[64];
  const char *tag;
  int seq, check, nested_seq, nested_check;

  if (idx < 0 && idx > LUA_REGISTRYINDEX)
    idx = lua_gettop(L) + idx + 1;
  assert(lua_istable(L, idx));

  lua_getfield(L, idx, "seq");
  assert(lua_isnumber(L, -1));
  seq = (int)lua_tointeger(L, -1);
  assert(seq >= 0 && seq <= WRITES);
  lua_pop(L, 1);

  lua_getfield(L, idx, "check");
  assert(lua_isnumber(L, -1));
  check = (int)lua_tointeger(L, -1);
  assert(check == (seq ^ CHECK_MASK));
  lua_pop(L, 1);

  snprintf(want_tag, sizeof(want_tag), "payload:%d", seq);
  lua_getfield(L, idx, "tag");
  tag = lua_tostring(L, -1);
  assert(tag != NULL && strcmp(tag, want_tag) == 0);
  lua_pop(L, 1);

  lua_getfield(L, idx, "nested");
  assert(lua_istable(L, -1));
  lua_getfield(L, -1, "seq");
  nested_seq = (int)lua_tointeger(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, -1, "check");
  nested_check = (int)lua_tointeger(L, -1);
  lua_pop(L, 1);
  assert(nested_seq == seq);
  assert(nested_check == (seq ^ CHECK_MASK));
  lua_pop(L, 1);

  lua_getfield(L, idx, "self");
  assert(lua_rawequal(L, -1, idx));
  lua_pop(L, 1);
}

static int upvalue_reader_c(lua_State *L)
{
  int uv = lua_upvalueindex(1);
  int seq;

  lua_pushvalue(L, uv);
  assert_payload(L, -1);
  lua_getfield(L, -1, "seq");
  seq = (int)lua_tointeger(L, -1);
  lua_pop(L, 2);
  lua_pushinteger(L, seq);
  return 1;
}

static void *writer_main(void *arg)
{
  WriterCtx *ctx = (WriterCtx *)arg;
  lua_State *L = ctx->L;
  int i;

  if (!luaMT_attach(L)) {
    ctx->status = 1;
    store_flag(ctx->done, 1);
    return NULL;
  }

  if (!lua_isfunction(L, 1) || !lua_checkstack(L, 8)) {
    ctx->status = 2;
    store_flag(ctx->done, 1);
    luaMT_detach(L);
    return NULL;
  }

  add_flag(ctx->ready, 1);
  while (!load_flag(ctx->start))
    sched_yield();

  for (i = 1; i <= WRITES; i++) {
    const char *name;

    push_payload(L, i);
    name = lua_setupvalue(L, 1, 1);
    if (name == NULL || lua_gettop(L) != 1) {
      ctx->status = 3;
      store_flag(ctx->done, 1);
      luaMT_detach(L);
      return NULL;
    }

    if ((i & 127) == 0) {
      lua_gc(L, LUA_GCSTEP, 32);
      sched_yield();
    }
  }

  ctx->status = 0;
  store_flag(ctx->done, 1);
  luaMT_detach(L);
  return NULL;
}

static void *reader_main(void *arg)
{
  ReaderCtx *ctx = (ReaderCtx *)arg;
  lua_State *L = ctx->L;
  long calls = 0;

  if (!luaMT_attach(L)) {
    ctx->status = 1;
    return NULL;
  }

  if (!lua_isfunction(L, 1) || !lua_checkstack(L, 8)) {
    ctx->status = 2;
    luaMT_detach(L);
    return NULL;
  }

  add_flag(ctx->ready, 1);
  while (!load_flag(ctx->start))
    sched_yield();

  while (!load_flag(ctx->done) || calls < MIN_READS) {
    int status, seq;

    lua_pushvalue(L, 1);
    status = lua_pcall(L, 0, 1, 0);
    if (status != LUA_OK) {
      fprintf(stderr, "reader %d: %s\n", ctx->id, lua_tostring(L, -1));
      ctx->status = 3;
      luaMT_detach(L);
      return NULL;
    }

    seq = (int)lua_tointeger(L, -1);
    if (seq < 0 || seq > WRITES) {
      ctx->status = 4;
      luaMT_detach(L);
      return NULL;
    }
    lua_pop(L, 1);
    calls++;

    if ((calls & 255) == 0) {
      if (lua_gettop(L) != 1) {
	ctx->status = 5;
	luaMT_detach(L);
	return NULL;
      }
      lua_gc(L, LUA_GCSTEP, 16);
      sched_yield();
    }
  }

  ctx->calls = calls;
  ctx->status = 0;
  luaMT_detach(L);
  return NULL;
}

static lua_State *new_child_with_closure(lua_State *L)
{
  lua_State *child = lua_newthread(L);
  lua_pushvalue(L, 1);
  lua_xmove(L, child, 1);
  return child;
}

static int call_shared_closure(lua_State *L)
{
  int status, seq;
  lua_pushvalue(L, 1);
  status = lua_pcall(L, 0, 1, 0);
  assert(status == LUA_OK);
  seq = (int)lua_tointeger(L, -1);
  lua_pop(L, 1);
  return seq;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  pthread_t writer_thread;
  pthread_t reader_threads[READERS];
  WriterCtx writer;
  ReaderCtx readers[READERS];
  volatile int ready = 0;
  volatile int start = 0;
  volatile int done = 0;
  int i;

  assert(L != NULL);
  luaL_openlibs(L);
  push_payload(L, 0);
  lua_pushcclosure(L, upvalue_reader_c, 1);
  assert(lua_gettop(L) == 1);

  memset(&writer, 0, sizeof(writer));
  writer.L = new_child_with_closure(L);
  writer.ready = &ready;
  writer.start = &start;
  writer.done = &done;
  assert(pthread_create(&writer_thread, NULL, writer_main, &writer) == 0);

  for (i = 0; i < READERS; i++) {
    memset(&readers[i], 0, sizeof(readers[i]));
    readers[i].L = new_child_with_closure(L);
    readers[i].id = i + 1;
    readers[i].ready = &ready;
    readers[i].start = &start;
    readers[i].done = &done;
    assert(pthread_create(&reader_threads[i], NULL, reader_main,
			  &readers[i]) == 0);
  }

  while (load_flag(&ready) != READERS + 1)
    sched_yield();
  store_flag(&start, 1);

  assert(pthread_join(writer_thread, NULL) == 0);
  assert(writer.status == 0);
  for (i = 0; i < READERS; i++) {
    assert(pthread_join(reader_threads[i], NULL) == 0);
    assert(readers[i].status == 0);
    assert(readers[i].calls >= MIN_READS);
  }

  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(call_shared_closure(L) == WRITES);

  lua_close(L);
  printf("t-cclosure-upvalue-race OK: shared C closure upvalue publication is coherent\n");
  return 0;
}
