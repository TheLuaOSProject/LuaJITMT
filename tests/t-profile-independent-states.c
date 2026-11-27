#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

typedef struct Samples {
  lua_State *expected;
  int count;
} Samples;

static int poke(lua_State *L)
{
  (void)L;
  assert(raise(SIGPROF) == 0);
  return 0;
}

static void sample(void *data, lua_State *L, int count, int vmstate)
{
  Samples *s = (Samples *)data;
  assert(L == s->expected && count > 0 && strchr("NICGJ", vmstate));
  s->count += count;
}

static lua_State *new_main(void)
{
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  if (getenv("LJ_PROFILE_TEST_JOFF"))
    assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF));
  lua_register(L, "poke", poke);
  return L;
}

static void drive(lua_State *L, int resume)
{
  static const char script[] =
    "for i = 1, 100 do\n"
    "  poke()\n"
    "  local sum = 0; for j = 1, 120 do sum = sum + j end\n"
    "  assert(sum == 7260)\n"
    "end";
  assert(luaL_loadstring(L, script) == 0);
  assert((resume ? lua_resume(L, 0) : lua_pcall(L, 0, 0, 0)) == 0);
}

static void start(lua_State *L, Samples *s)
{
  s->expected = L;
  s->count = 0;
  luaJIT_profile_start(L, "i1000000", sample, s);
}

static void check(lua_State *L, int resume)
{
  Samples s;
  start(L, &s);
  drive(L, resume);
  luaJIT_profile_stop(L);
  assert(s.count == 100);
}

static void *unrelated_thread(void *unused)
{
  lua_State *L;
  (void)unused;
  assert(raise(SIGPROF) == 0);
  L = new_main();
  drive(L, 0);
  lua_close(L);
  assert(raise(SIGPROF) == 0);
  return NULL;
}

int main(int argc, char **argv)
{
  struct sigaction ignore = {0}, old;
  lua_State *outer, *inner, *co, *survivor;
  Samples s;
  int isolate = argc == 1 || strcmp(argv[1], "stock") != 0;
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  assert(sigaction(SIGPROF, &ignore, &old) == 0);
  outer = new_main();
  inner = new_main();
  check(outer, 0);
  check(inner, 0);
  co = lua_newthread(inner);
  check(co, 1);
  check(outer, 0);
  start(inner, &s);
  if (isolate) {
    pthread_t thread;
    /* While the selected VM is idle, run a different VM on its pthread and
    ** send signals before, during and after a foreign pthread's attachment. */
    drive(outer, 0);
    assert(pthread_create(&thread, NULL, unrelated_thread, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
  }
  lua_close(outer);
  drive(inner, 0);
  luaJIT_profile_stop(inner);
  assert(s.count == 100);
  check(inner, 0);  /* Cold start after the initial TLS binding was closed. */
  survivor = new_main();
  start(inner, &s);
  drive(inner, 0);
  assert(s.count == 100);
  lua_close(inner);  /* Close stops this still-active profile. */
  check(survivor, 0);
  lua_close(survivor);
  assert(sigaction(SIGPROF, &old, NULL) == 0);
  puts("t-profile-independent-states OK");
  return 0;
}
