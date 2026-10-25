/* Native release-profile timing for the production rooted scalar read. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_obj.h"
#include "lj_tab.h"

static uint64_t elapsed_ns(struct timespec start, struct timespec end)
{
  int64_t seconds = (int64_t)end.tv_sec - (int64_t)start.tv_sec;
  int64_t nanos = (int64_t)end.tv_nsec - (int64_t)start.tv_nsec;
  return (uint64_t)(seconds * INT64_C(1000000000) + nanos);
}

int main(void)
{
  enum { WARMUP = 10000, ITERATIONS = 1000000 };
  lua_State *L = luaL_newstate();
  TValue out;
  cTValue *table_root, *key_root;
  struct timespec start, end;
  uint32_t hits = 0, i;
  uint64_t ns;
  assert(L);
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF));
  lua_createtable(L, 1, 0);
  lua_pushnumber(L, 2.5);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, 1);
  table_root = L->top - 2;
  key_root = L->top - 1;
  setnilV(&out);
  for (i = 0; i < WARMUP; i++)
    assert(lj_tab_getscalar_rooted_try(L, table_root, key_root, &out));
  assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
  for (i = 0; i < ITERATIONS; i++) {
    if (lj_tab_getscalar_rooted_try(L, table_root, key_root, &out)) hits++;
  }
  assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
  ns = elapsed_ns(start, end);
  assert(hits == ITERATIONS && tvisnumber(&out) && numberVnum(&out) == 2.5);
  printf("release rooted scalar read: iterations=%u elapsed-ns=%llu ns/read=%.2f reads/s=%.0f\n",
    ITERATIONS, (unsigned long long)ns,
    (double)ns / ITERATIONS,
    (double)ITERATIONS * 1000000000.0 / (double)(ns ? ns : 1));
  lua_close(L);
  return 0;
}
