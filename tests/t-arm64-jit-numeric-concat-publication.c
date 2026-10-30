#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_arena.h"
#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tg.h"
#include "lj_trace.h"

#if !defined(__APPLE__) || !(defined(__aarch64__) || defined(__arm64__)) || \
    !defined(LUAJIT_MT_ARM64_BOOTSTRAP) || \
    !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) || \
    !defined(LJ_TRACE_TEST_HELPERS) || !defined(LJ_STR_TEST_HELPERS)
#error "numeric concat publication requires native ARM64 test helpers"
#endif

extern uint32_t lj_trace_test_root_entry_successes(void);
extern uint32_t lj_trace_test_exit_calls(void);

static lua_State *test_L;
static unsigned watching, lease_publications, inactive_publications;
static unsigned expected_publications, expected_under_lease;
static uint32_t entries_before;
static char expected_text[64];
static size_t expected_len;

static void numeric_concat_publish(lua_State *L, GCstr *str)
{
  GCArena *arena;
  int has_base;
  if (!watching) return;
  assert(L == test_L);
  has_base = lj_tg_load_jit_base(L2TG(L)) != NULL;
  if (str->len == expected_len &&
      memcmp(strdata(str), expected_text, expected_len) == 0) {
    expected_publications++;
    if (has_base) expected_under_lease++;
  }
  if (!has_base) {
    inactive_publications++;
    return;
  }
  assert(lj_tg_jit_active_acq(L2TG(L)));
  assert(str->gct == (uint8_t)~LJ_TSTR);
  arena = lj_arena_of(str);
  assert(!lj_arena_ishuge(arena));
  assert(lj_arena_flags_acq(arena) & LJ_AF_TRAVERSABLE);
  assert(lj_arena_ready_get(arena, lj_arena_cellof(str)));
  lease_publications++;
}

static int native_begin_int(lua_State *L)
{
  int32_t value = (int32_t)luaL_checkinteger(L, 1);
  int len;
  assert(!watching);
  len = snprintf(expected_text, sizeof(expected_text), "%d", value);
  assert(len > 0 && (size_t)len < sizeof(expected_text));
  expected_len = (size_t)len;
  lease_publications = inactive_publications = expected_publications = 0;
  expected_under_lease = 0;
  entries_before = lj_trace_test_root_entry_successes();
  watching = 1;
  return 0;
}

static int native_begin_numeric(lua_State *L)
{
  static const char expected[] = "100020.125";
  (void)L;
  assert(!watching);
  memcpy(expected_text, expected, sizeof(expected));
  expected_len = sizeof(expected) - 1u;
  lease_publications = inactive_publications = expected_publications = 0;
  expected_under_lease = 0;
  entries_before = lj_trace_test_root_entry_successes();
  watching = 1;
  return 0;
}

static int native_begin_char(lua_State *L)
{
  unsigned value = (unsigned)luaL_checkinteger(L, 1);
  assert(value <= 255u && !watching);
  expected_text[0] = (char)value;
  expected_len = 1;
  lease_publications = inactive_publications = expected_publications = 0;
  expected_under_lease = 0;
  entries_before = lj_trace_test_root_entry_successes();
  watching = 1;
  return 0;
}

static int native_finish(lua_State *L)
{
  uint32_t entries_after;
  watching = 0;
  entries_after = lj_trace_test_root_entry_successes();
  fprintf(stderr, "fresh string publications under-jit-base=%u inactive=%u expected=%u expected-under-jit-base=%u\n",
          lease_publications, inactive_publications, expected_publications,
          expected_under_lease);
  fprintf(stderr, "fresh string root entries before=%u after=%u exits=%u\n",
          entries_before, entries_after, lj_trace_test_exit_calls());
  assert(entries_after > entries_before);
  assert(lj_trace_test_exit_calls() > 0);
  assert(lease_publications > 0 && expected_publications == 1 &&
         expected_under_lease == 1);
  lua_pushinteger(L, (lua_Integer)lease_publications);
  return 1;
}

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  assert(L && argc == 2);
  luaL_openlibs(L);
  test_L = L;
  lua_pushcfunction(L, native_begin_int); lua_setglobal(L, "native_begin_int");
  lua_pushcfunction(L, native_begin_numeric); lua_setglobal(L, "native_begin_numeric");
  lua_pushcfunction(L, native_begin_char); lua_setglobal(L, "native_begin_char");
  lua_pushcfunction(L, native_finish); lua_setglobal(L, "native_finish");
  lj_str_test_set_publish_hook(numeric_concat_publish);
  if (luaL_dofile(L, argv[1]) != LUA_OK) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    abort();
  }
  lj_str_test_set_publish_hook(NULL);
  assert(!watching && expected_publications == 1 && expected_under_lease == 1);
  printf("CONCAT fresh string published under TOSTR trace lease: matches=%u publications=%u\n",
         expected_publications, lease_publications);
  lua_close(L);
  return 0;
}
