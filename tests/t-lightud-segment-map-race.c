/* Concurrent public-API regression for the lock-free lightuserdata map. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_udata.h"

#if !LJ_64 || !defined(LJ_UDATA_TEST_HELPERS)
#error "lightuserdata segment-map fixture requires 64-bit userdata test helpers"
#endif

static uint32_t lightud_rendezvous_up = UINT32_MAX;
static uint32_t lightud_rendezvous_first_seg = UINT32_MAX;
static uint32_t lightud_rendezvous_expected;
static uint32_t lightud_rendezvous_arrived;
static uint32_t lightud_rendezvous_release;
static uint32_t lightud_rendezvous_timed_out;
static uint32_t lightud_rendezvous_bad_seg;

static void *ptr_for(uint32_t group, uint64_t low);

static uint64_t monotonic_ms(void)
{
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void lightud_pre_cas_rendezvous(lua_State *L, uint32_t up, uint32_t seg)
{
  uint64_t deadline;
  uint32_t first;
  UNUSED(L);
  if (up != la_load32_acq(&lightud_rendezvous_up))
    return;
  first = UINT32_MAX;
  if (!la_cas32(&lightud_rendezvous_first_seg, &first, seg, LA_ACQ_REL,
                LA_ACQ) &&
      first != seg)
    la_store32_rel(&lightud_rendezvous_bad_seg, 1);
  (void)la_add32_acqrel(&lightud_rendezvous_arrived, 1);
  deadline = monotonic_ms() + 10000u;
  while (!la_load32_acq(&lightud_rendezvous_release)) {
    if (monotonic_ms() >= deadline) {
      la_store32_rel(&lightud_rendezvous_timed_out, 1);
      la_store32_rel(&lightud_rendezvous_release, 1);
      break;
    }
    (void)sched_yield();
  }
}

static int l_lightud_rendezvous_arm(lua_State *L)
{
  uint32_t group = (uint32_t)lua_tointeger(L, 1);
  uint32_t expected = (uint32_t)lua_tointeger(L, 2);
  uint32_t up = lightudup((uint64_t)ptr_for(group, 0x1777));
  assert(expected > 0 && expected <= 4);
  la_store32_rel(&lightud_rendezvous_release, 0);
  la_store32_rel(&lightud_rendezvous_timed_out, 0);
  la_store32_rel(&lightud_rendezvous_arrived, 0);
  la_store32_rel(&lightud_rendezvous_expected, expected);
  la_store32_rel(&lightud_rendezvous_first_seg, UINT32_MAX);
  la_store32_rel(&lightud_rendezvous_bad_seg, 0);
  la_store32_rel(&lightud_rendezvous_up, up);
  return 0;
}

static int l_lightud_rendezvous_wait(lua_State *L)
{
  uint32_t expected = la_load32_acq(&lightud_rendezvous_expected);
  uint64_t deadline = monotonic_ms() + (uint32_t)lua_tointeger(L, 1);
  while (la_load32_acq(&lightud_rendezvous_arrived) < expected &&
         monotonic_ms() < deadline) {
    struct timespec pause = {0, 1000000};
    (void)nanosleep(&pause, NULL);
  }
  lua_pushinteger(L, la_load32_acq(&lightud_rendezvous_arrived));
  lua_pushboolean(L, la_load32_acq(&lightud_rendezvous_timed_out) != 0 ||
                         la_load32_acq(&lightud_rendezvous_arrived) < expected);
  lua_pushboolean(L, la_load32_acq(&lightud_rendezvous_bad_seg) != 0);
  return 3;
}

static int l_lightud_rendezvous_release(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&lightud_rendezvous_up, UINT32_MAX);
  la_store32_rel(&lightud_rendezvous_release, 1);
  return 0;
}

static void *ptr_for(uint32_t group, uint64_t low)
{
  assert(group < (1u << (64 - LJ_LIGHTUD_BITS_LO)));
  assert(low < ((uint64_t)1 << LJ_LIGHTUD_BITS_LO));
  return (void *)(uintptr_t)(((uint64_t)group << LJ_LIGHTUD_BITS_LO) | low);
}
static int l_lightud_roundtrip(lua_State *L)
{
  uint32_t group = (uint32_t)lua_tointeger(L, 1);
  uint64_t low = (uint64_t)lua_tointeger(L, 2);
  void *p = ptr_for(group, low);
  lua_pushlightuserdata(L, p);
  if (lua_touserdata(L, -1) != p)
    return luaL_error(L, "lightuserdata roundtrip mismatch");
  return 1;
}
static int l_lightud_matches(lua_State *L)
{
  uint32_t group = (uint32_t)lua_tointeger(L, 2);
  uint64_t low = (uint64_t)lua_tointeger(L, 3);
  void *want = ptr_for(group, low);
  lua_pushboolean(L, lua_type(L, 1) == LUA_TLIGHTUSERDATA &&
                         lua_touserdata(L, 1) == want);
  return 1;
}
static int l_push_group(lua_State *L)
{
  uint32_t group = (uint32_t)lua_tointeger(L, 1);
  lua_pushlightuserdata(L, ptr_for(group, 0x1234));
  return 1;
}
static void install(lua_State *L)
{
  lj_udata_test_set_lightud_intern_hook(lightud_pre_cas_rendezvous);
  lua_pushcfunction(L, l_lightud_rendezvous_arm);
  lua_setglobal(L, "lightud_rendezvous_arm");
  lua_pushcfunction(L, l_lightud_rendezvous_wait);
  lua_setglobal(L, "lightud_rendezvous_wait");
  lua_pushcfunction(L, l_lightud_rendezvous_release);
  lua_setglobal(L, "lightud_rendezvous_release");
  lua_pushcfunction(L, l_lightud_roundtrip);
  lua_setglobal(L, "lightud_roundtrip");
  lua_pushcfunction(L, l_lightud_matches);
  lua_setglobal(L, "lightud_matches");
}
static void test_full_prefix_domain_and_capacity(void)
{
  lua_State *L = luaL_newstate();
  uintptr_t pmax = (uintptr_t)UINT64_C(0xffffff8000001234);
  uint32_t upmax = lightudup(pmax);
  uint32_t i;
  assert(L);
  /* `lightudup` has seven fixed zero bits, so UINT32_MAX cannot be a real
  ** prefix, including for the highest possible 64-bit pointer prefix.
  ** All synthetic addresses in this fixture are opaque, never dereferenced. */
  assert(LJ_LIGHTUD_BITS_LO == 39 && (upmax & 0x7fu) == 0);
  assert(upmax == (UINT32_MAX & ~0x7fu));
  assert(upmax != UINT32_MAX);
  lua_pushlightuserdata(L, NULL);
  assert(tvislightud(L->top - 1) && lua_touserdata(L, -1) == NULL);
  lua_pop(L, 1);
  lua_pushlightuserdata(L, (void *)pmax);
  assert(lua_touserdata(L, -1) == (void *)pmax);
  lua_pop(L, 1);
  /* Prefixes 0--253 (including NULL's prefix) plus pmax fill 255 slots. */
  for (i = 0; i < 254; i++) {
    uintptr_t p = (uintptr_t)(((uint64_t)i << LJ_LIGHTUD_BITS_LO) | 0x1234);
    lua_pushlightuserdata(L, (void *)p);
    assert(tvislightud(L->top - 1) && lua_touserdata(L, -1) == (void *)p);
    lua_pop(L, 1);
  }
  lua_pushcfunction(L, l_push_group);
  lua_pushinteger(L, 254);
  assert(lua_pcall(L, 1, 1, 0) == LUA_ERRRUN);
  lua_pop(L, 1);
  /* A failed new-prefix insertion must leave every old token usable. */
  lua_pushlightuserdata(L, NULL);
  assert(lua_touserdata(L, -1) == NULL);
  lua_pop(L, 1);
  lua_pushlightuserdata(L, (void *)pmax);
  assert(lua_touserdata(L, -1) == (void *)pmax);
  lua_pop(L, 1);
  for (i = 0; i < 254; i++) {
    uintptr_t p = (uintptr_t)(((uint64_t)i << LJ_LIGHTUD_BITS_LO) | 0x1234);
    lua_pushlightuserdata(L, (void *)p);
    assert(lua_touserdata(L, -1) == (void *)p);
    lua_pop(L, 1);
  }
  lua_close(L);
}
int main(int argc, char **argv)
{
  lua_State *L;
  assert(argc == 2);
  test_full_prefix_domain_and_capacity();
  L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  install(L);
  if (luaL_dofile(L, argv[1])) {
    fprintf(stderr, "lightuserdata map Lua fixture: %s\n", lua_tostring(L, -1));
    return 2;
  }
  lj_udata_test_set_lightud_intern_hook(NULL);
  lua_close(L);
  puts("t-lightud-segment-map-race OK");
  return 0;
}
