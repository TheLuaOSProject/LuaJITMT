/*
** ARM64 C API generation/rooting regression for equality, comparison,
** metatable and metafield/callmeta paths.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_safepoint.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_thr.h"

#if !defined(LJ_API_ROOT_TEST_HELPERS)
#error "t-arm64-capi-meta-roots requires LJ_API_ROOT_TEST_HELPERS"
#endif
#if !defined(LJ_TG_ROOT_TEST_HELPERS)
#error "t-arm64-capi-meta-roots requires LJ_TG_ROOT_TEST_HELPERS"
#endif

typedef void (*LJApiRawMetatablePublishHook)(lua_State *L, TValue *result);
extern void lj_api_test_set_raw_mt_publish_hook(
  LJApiRawMetatablePublishHook hook);

static uint32_t eq_calls;
static uint32_t lt_calls;
static uint32_t raw_mt_hook_calls;
static int raw_mt_hook_object;
static lua_State *last_busy_state;
static lua_State *ownerless_error_target;
static lua_State *ownerless_success_target;
static TGState *ownerless_error_hint;
static LJStateOwner ownerless_error_owner;
static ptrdiff_t ownerless_error_topofs;
static void *ownerless_error_cframe;

enum {
  OWNERLESS_ERROR_COMPARE,
  OWNERLESS_ERROR_STOPREQ,
  OWNERLESS_ERROR_OOM,
  OWNERLESS_ERROR_METACALL
};
static int ownerless_error_mode;

static void full_cycle(lua_State *L)
{
  lua_gc(L, LUA_GCRESTART, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
}

static int mm_equal(lua_State *L)
{
  assert(lua_gettop(L) == 2);
  assert(!lua_rawequal(L, 1, 2));
  full_cycle(L);
  eq_calls++;
  lua_pushboolean(L, 1);
  return 1;
}

static int mm_less(lua_State *L)
{
  assert(lua_gettop(L) == 2);
  assert(!lua_rawequal(L, 1, 2));
  full_cycle(L);
  lt_calls++;
  lua_pushboolean(L, 1);
  return 1;
}

static int mm_probe(lua_State *L)
{
  assert(lua_gettop(L) == 1);
  full_cycle(L);
  lua_pushinteger(L, 77);
  return 1;
}

static int mm_error(lua_State *L)
{
  return luaL_error(L, "intentional equality error");
}

static void raw_mt_stop_hook(lua_State *L, TValue *result)
{
  assert(tvistab(result));
  lj_safepoint_checkstop(L, LJ_GC2_HS_STOPREQ);
  assert(0 && "STOPREQ hook returned");
}

static void push_meta(lua_State *L, lua_CFunction eq, lua_CFunction lt)
{
  lua_newtable(L);
  if (eq) {
    lua_pushcfunction(L, eq);
    lua_setfield(L, -2, "__eq");
  }
  if (lt) {
    lua_pushcfunction(L, lt);
    lua_setfield(L, -2, "__lt");
  }
}

static void raw_mt_publish_gc_hook(lua_State *L, TValue *result)
{
  ptrdiff_t resultofs = savestack(L, result);
  TValue before, after;
  lj_tv_load_acq(&before, result);
  assert(tvistab(&before));
  lua_pushnil(L);
  assert(lua_setmetatable(L, raw_mt_hook_object) == 1);
  full_cycle(L);
  result = restorestack(L, resultofs);
  lj_tv_load_acq(&after, result);
  assert(tvistab(&after));
  assert(tv_rawload(&after) == tv_rawload(&before));
  raw_mt_hook_calls++;
}

static void arm_raw_mt_gc_hook(int object)
{
  raw_mt_hook_object = object;
  lj_api_test_set_raw_mt_publish_hook(raw_mt_publish_gc_hook);
}

static void test_nil_invalid_and_negative_growth(lua_State *L)
{
  int start = lua_gettop(L);
  ptrdiff_t topofs;

  lua_pushnil(L);
  lua_pushnil(L);
  assert(lua_rawequal(L, -2, -1) == 1);
  assert(lua_equal(L, -2, -1) == 1);
  assert(lua_gettop(L) == start+2);
  assert(lua_rawequal(L, start+99, start+98) == 0);
  assert(lua_equal(L, start+99, start+98) == 0);
  assert(lua_lessthan(L, start+99, -1) == 0);
  assert(lua_gettop(L) == start+2);
  lua_settop(L, start);

  /* Leave exactly the boundary at which the pair API must grow. The two
  ** negative indices are resolved before that relocation and before either
  ** temporary root changes top. */
  while (mref(L->maxstack, TValue) - L->top > 4) {
    setnilV(L->top);
    lj_state_stack_pubtv(L, L, L->top);
    L->top++;
  }
  lua_newtable(L);
  lua_pushvalue(L, -1);
  topofs = savestack(L, L->top);
  assert(mref(L->maxstack, TValue) - L->top <= 2);
  assert(lua_rawequal(L, -2, -1) == 1);
  assert(lua_equal(L, -2, -1) == 1);
  assert(savestack(L, L->top) == topofs);
  lua_settop(L, start);
}

static int pseudo_pair_check(lua_State *L)
{
  int top = lua_gettop(L);
  uint32_t eq0 = eq_calls, lt0 = lt_calls;
  assert(lua_rawequal(L, LUA_GLOBALSINDEX, LUA_ENVIRONINDEX) == 0);
  assert(lua_equal(L, LUA_GLOBALSINDEX, LUA_ENVIRONINDEX) == 1);
  assert(lua_lessthan(L, LUA_GLOBALSINDEX, LUA_ENVIRONINDEX) == 1);
  assert(eq_calls == eq0+1u && lt_calls == lt0+1u);
  assert(lua_gettop(L) == top);
  return 0;
}

static int upvalue_pair_check(lua_State *L)
{
  int top = lua_gettop(L);
  assert(lua_rawequal(L, lua_upvalueindex(1), LUA_REGISTRYINDEX) == 1);
  assert(lua_equal(L, lua_upvalueindex(1), LUA_REGISTRYINDEX) == 1);
  assert(lua_gettop(L) == top);
  return 0;
}

static void test_pseudo_and_upvalue_pairs(lua_State *L)
{
  int mtref;
  lua_settop(L, 0);
  push_meta(L, mm_equal, mm_less);
  mtref = luaL_ref(L, LUA_REGISTRYINDEX);

  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_rawgeti(L, LUA_REGISTRYINDEX, mtref);
  assert(lua_setmetatable(L, -2) == 1);
  lua_pop(L, 1);

  lua_pushcfunction(L, pseudo_pair_check);
  lua_newtable(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, mtref);
  assert(lua_setmetatable(L, -2) == 1);
  assert(lua_setfenv(L, -2) == 1);
  assert(lua_pcall(L, 0, 0, 0) == LUA_OK);

  lua_pushvalue(L, LUA_REGISTRYINDEX);
  lua_pushcclosure(L, upvalue_pair_check, 1);
  assert(lua_pcall(L, 0, 0, 0) == LUA_OK);

  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_pushnil(L);
  assert(lua_setmetatable(L, -2) == 1);
  lua_pop(L, 1);
  luaL_unref(L, LUA_REGISTRYINDEX, mtref);
}

static void set_same_metatable(lua_State *L, int first, int second,
			       int mt)
{
  lua_pushvalue(L, mt);
  assert(lua_setmetatable(L, first) == 1);
  lua_pushvalue(L, mt);
  assert(lua_setmetatable(L, second) == 1);
}

static void test_table_userdata_meta(lua_State *L)
{
  uint32_t eq0 = eq_calls, lt0 = lt_calls;
  int top;
  lua_settop(L, 0);
  lua_newtable(L);                 /* 1: lhs */
  lua_newtable(L);                 /* 2: rhs */
  push_meta(L, mm_equal, mm_less); /* 3: mt */
  set_same_metatable(L, 1, 2, 3);
  top = lua_gettop(L);
  assert(lua_equal(L, 1, 2) == 1);
  assert(lua_lessthan(L, 1, 2) == 1);
  assert(lua_gettop(L) == top);
  assert(eq_calls == eq0+1u && lt_calls == lt0+1u);

  lua_settop(L, 0);
  (void)lua_newuserdata(L, 8);     /* 1: lhs */
  (void)lua_newuserdata(L, 8);     /* 2: rhs */
  push_meta(L, mm_equal, NULL);    /* 3: mt */
  set_same_metatable(L, 1, 2, 3);
  top = lua_gettop(L);
  assert(lua_equal(L, 1, 2) == 1);
  assert(lua_gettop(L) == top);
}

static void test_actual_nil_metatable(lua_State *L)
{
  lua_settop(L, 0);
  lua_pushnil(L);                  /* 1: actual nil */
  lua_newtable(L);
  lua_pushliteral(L, "nil-mt");
  lua_setfield(L, -2, "tag");
  assert(lua_setmetatable(L, 1) == 1);
  assert(lua_getmetatable(L, 1) == 1);
  lua_getfield(L, -1, "tag");
  assert(strcmp(lua_tostring(L, -1), "nil-mt") == 0);
  lua_pop(L, 2);
  assert(lua_getmetatable(L, 99) == 0);  /* None is not an actual nil. */
  lua_pushnil(L);
  assert(lua_setmetatable(L, 1) == 1);
  lua_settop(L, 0);
}

static void test_raw_metatable_and_metafield_gc(lua_State *L)
{
  uint32_t hooks = raw_mt_hook_calls;
  lua_settop(L, 0);

  lua_newtable(L);                 /* 1: object */
  lua_newtable(L);                 /* mt, consumed below */
  lua_pushliteral(L, "raw-mt");
  lua_setfield(L, -2, "tag");
  assert(lua_setmetatable(L, 1) == 1);
  arm_raw_mt_gc_hook(1);
  assert(lua_getmetatable(L, 1) == 1);
  assert(raw_mt_hook_calls == ++hooks);
  lua_getfield(L, -1, "tag");
  assert(strcmp(lua_tostring(L, -1), "raw-mt") == 0);
  lua_pop(L, 2);
  assert(lua_getmetatable(L, 1) == 0);

  lua_settop(L, 0);
  lua_newtable(L);                 /* 1: object */
  lua_newtable(L);
  lua_newtable(L);                 /* collectable metafield result */
  lua_pushliteral(L, "field-value");
  lua_setfield(L, -2, "tag");
  lua_setfield(L, -2, "__probe");
  assert(lua_setmetatable(L, 1) == 1);
  arm_raw_mt_gc_hook(1);
  assert(luaL_getmetafield(L, 1, "__probe") == 1);
  assert(raw_mt_hook_calls == ++hooks);
  lua_getfield(L, -1, "tag");
  assert(strcmp(lua_tostring(L, -1), "field-value") == 0);
  lua_pop(L, 2);

  lua_settop(L, 0);
  lua_newtable(L);                 /* 1: object */
  lua_newtable(L);
  lua_pushcfunction(L, mm_probe);
  lua_setfield(L, -2, "__probe");
  assert(lua_setmetatable(L, 1) == 1);
  arm_raw_mt_gc_hook(1);
  assert(luaL_callmeta(L, 1, "__probe") == 1);
  assert(raw_mt_hook_calls == ++hooks);
  assert(lua_tointeger(L, -1) == 77);
  lua_pop(L, 1);
}

static int invoke_equal_error(lua_State *L)
{
  (void)lua_equal(L, 1, 2);
  return 0;
}

static int invoke_compare_error(lua_State *L)
{
  (void)lua_lessthan(L, 1, 2);
  return 0;
}

static int invoke_metafield_stop(lua_State *L)
{
  lua_newtable(L);
  lua_newtable(L);
  lua_pushinteger(L, 1);
  lua_setfield(L, -2, "__probe");
  assert(lua_setmetatable(L, 1) == 1);
  lj_api_test_set_raw_mt_publish_hook(raw_mt_stop_hook);
  (void)luaL_getmetafield(L, 1, "__probe");
  assert(0 && "STOPREQ metafield lookup returned");
  return 0;
}

static void test_error_cleanup(lua_State *L)
{
  TGState *tg = L2TG(L);
  uint32_t roots = lj_tg_root_anchor_top_acq(tg);
  int top;
  lua_settop(L, 0);
  lua_pushcfunction(L, invoke_equal_error);
  lua_newtable(L);
  lua_newtable(L);
  push_meta(L, mm_error, NULL);
  set_same_metatable(L, 2, 3, 4);
  lua_remove(L, 4);
  top = lua_gettop(L)-3;
  assert(lua_pcall(L, 2, 0, 0) == LUA_ERRRUN);
  assert(strstr(lua_tostring(L, -1), "intentional equality error") != NULL);
  assert(lua_gettop(L) == top+1);
  lua_pop(L, 1);
  assert(lj_tg_root_anchor_top_acq(tg) == roots);

  lua_pushcfunction(L, invoke_compare_error);
  lua_newtable(L);
  lua_newtable(L);
  top = lua_gettop(L)-3;
  assert(lua_pcall(L, 2, 0, 0) == LUA_ERRRUN);
  assert(lua_gettop(L) == top+1);
  lua_pop(L, 1);
  assert(lj_tg_root_anchor_top_acq(tg) == roots);

  lua_pushcfunction(L, invoke_metafield_stop);
  top = lua_gettop(L)-1;
  assert(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  assert(lua_gettop(L) == top+1);
  lua_pop(L, 1);
  assert(lj_tg_root_anchor_top_acq(tg) == roots);
}

static int invoke_ownerless_success_curL(lua_State *L)
{
  lua_State *co = ownerless_success_target;
  TGState *tg = L2TG(L);
  assert(co != NULL && lj_tg_load_cur_L(tg) == L);
  assert(lua_equal(co, 1, 2) == 1);
  assert(lj_tg_load_cur_L(tg) == L);
  assert(lua_lessthan(co, 1, 2) == 1);
  assert(lj_tg_load_cur_L(tg) == L);
  assert(luaL_callmeta(co, 1, "__probe") == 1);
  assert(lj_tg_load_cur_L(tg) == L);
  assert(lua_tointeger(co, -1) == 88);
  lua_pop(co, 1);
  return 0;
}

static void test_ownerless_success(lua_State *L)
{
  lua_State *co;
  lua_settop(L, 0);
  co = lua_newthread(L);
  assert(luaL_loadstring(co,
    "local mt={"
    "__eq=function(a,b)return true end,"
    "__lt=function(a,b)return true end,"
    "__probe=function(a)return 88 end} "
    "return setmetatable({},mt),setmetatable({},mt)") == LUA_OK);
  lua_call(co, 0, 2);
  assert(lj_state_owner_acq(co) == 0);
  assert(lua_rawequal(co, 1, 2) == 0);
  assert(lua_equal(co, 1, 2) == 1);
  assert(lua_lessthan(co, 1, 2) == 1);
  assert(lua_getmetatable(co, 1) == 1);
  lua_pop(co, 1);
  assert(luaL_getmetafield(co, 1, "__probe") == 1);
  lua_pop(co, 1);
  assert(luaL_callmeta(co, 1, "__probe") == 1);
  assert(lua_tointeger(co, -1) == 88);
  lua_pop(co, 1);
  assert(lua_gettop(co) == 2);
  assert(lj_state_owner_acq(co) == 0);
  ownerless_success_target = co;
  lua_pushcfunction(L, invoke_ownerless_success_curL);
  assert(lua_pcall(L, 0, 0, 0) == LUA_OK);
  ownerless_success_target = NULL;
  lua_pop(L, 1);
}

static void snapshot_ownerless_target(lua_State *caller)
{
  lua_State *co = ownerless_error_target;
  TGState *tg = L2TG(caller);
  assert(co != NULL && lj_tg_load_cur_L(tg) == caller);
  ownerless_error_hint = co->tg_hint;
  ownerless_error_owner = lj_state_owner_word_acq(co);
  ownerless_error_topofs = savestack(co, co->top);
  ownerless_error_cframe = co->cframe;
  assert(lj_state_owner_tid(ownerless_error_owner) == 0);
}

static int invoke_ownerless_prepare_error(lua_State *L)
{
  TGState *tg = L2TG(L);
  snapshot_ownerless_target(L);
  if (ownerless_error_mode == OWNERLESS_ERROR_COMPARE) {
    (void)lua_lessthan(ownerless_error_target, 1, 2);
  } else if (ownerless_error_mode == OWNERLESS_ERROR_STOPREQ) {
    lj_api_test_set_raw_mt_publish_hook(raw_mt_stop_hook);
    (void)luaL_getmetafield(ownerless_error_target, 1, "__probe");
  } else if (ownerless_error_mode == OWNERLESS_ERROR_OOM) {
    TValue nilv;
    uint32_t pushed = 0;
    setnilV(&nilv);
    /* Reach the first not-yet-allocated anchor block. The protected API
    ** boundary starts after these ambient roots and must preserve that exact
    ** depth while catching the forced MetaChainRoots allocation failure. */
    while ((lj_tg_root_anchor_top_acq(tg) % TG_ROOT_ANCHOR_SLOTS) != 0 ||
           lj_tg_root_anchor_slot_acq(
	     tg, lj_tg_root_anchor_top_acq(tg)) != NULL) {
      assert(pushed++ < 512u);
      assert(lj_tg_root_anchor_push(L, tg, &nilv, NULL) != NULL);
    }
    lj_tg_root_test_fail_reserve_after(1);
    (void)lua_equal(ownerless_error_target, 1, 2);
  } else {
    (void)lua_equal(ownerless_error_target, 1, 2);
  }
  assert(0 && "ownerless preparation error returned");
  return 0;
}

static void assert_ownerless_target_restored(lua_State *L,
					      uint32_t roots)
{
  lua_State *co = ownerless_error_target;
  assert(lj_state_owner_word_acq(co) == ownerless_error_owner);
  assert(co->tg_hint == ownerless_error_hint);
  assert(savestack(co, co->top) == ownerless_error_topofs);
  assert(co->cframe == ownerless_error_cframe);
  assert(lj_tg_load_cur_L(L2TG(L)) == L);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == roots);
  assert(lua_rawequal(co, 1, 2) == 0);
  assert(lj_state_owner_word_acq(co) == ownerless_error_owner);
}

static void run_ownerless_error(lua_State *L, const char *chunk, int mode,
				const char *message)
{
  uint32_t roots = lj_tg_root_anchor_top_acq(L2TG(L));
  lua_settop(L, 0);
  ownerless_error_target = lua_newthread(L);
  assert(luaL_loadstring(ownerless_error_target, chunk) == LUA_OK);
  lua_call(ownerless_error_target, 0, mode == OWNERLESS_ERROR_STOPREQ ? 1 : 2);
  assert(lj_state_owner_acq(ownerless_error_target) == 0);
  ownerless_error_mode = mode;
  lua_pushcfunction(L, invoke_ownerless_prepare_error);
  assert(lua_pcall(L, 0, 0, 0) ==
	 (mode == OWNERLESS_ERROR_OOM ? LUA_ERRMEM : LUA_ERRRUN));
  assert(lua_tostring(L, -1) != NULL);
  assert(strstr(lua_tostring(L, -1), message) != NULL);
  assert_ownerless_target_restored(L, roots);
  lua_settop(L, 0);
  ownerless_error_target = NULL;
}

static void test_ownerless_error_unwind(lua_State *L)
{
  run_ownerless_error(L, "return {}, {}", OWNERLESS_ERROR_COMPARE,
		      "attempt to compare two table values");
  run_ownerless_error(L,
    "return setmetatable({}, {__probe={tag='rooted'}})",
    OWNERLESS_ERROR_STOPREQ, "thread interrupted: VM shutdown");
  run_ownerless_error(L, "return {}, {}", OWNERLESS_ERROR_OOM,
		      "not enough memory");
  run_ownerless_error(L,
    "local mt={__eq=function() error('intentional ownerless metamethod error') end}; "
    "return setmetatable({}, mt), setmetatable({}, mt)",
    OWNERLESS_ERROR_METACALL, "intentional ownerless metamethod error");
}

static uint32_t foreign_tid(lua_State *L)
{
  uint32_t tid = lj_thr_current_id(G(L)) + 1000u;
  return tid == 0 || tid == LJ_THREAD_GCSCAN ? 123u : tid;
}

static lua_State *busy_state(lua_State *L)
{
  lua_State *co = lua_newthread(L);
  uint32_t tid = lj_thr_current_id(G(L));
  assert(lj_state_claim(co, tid));
  lua_newtable(co);
  lua_newtable(co);
  lj_state_release(co, tid);
  lj_state_owner_rel(co, foreign_tid(L));
  last_busy_state = co;
  return co;
}

static int busy_rawequal(lua_State *L)
{
  lua_State *co = busy_state(L);
  (void)lua_rawequal(co, 1, 2);
  return 0;
}

static int busy_equal(lua_State *L)
{
  lua_State *co = busy_state(L);
  (void)lua_equal(co, 1, 2);
  return 0;
}

static int busy_less(lua_State *L)
{
  lua_State *co = busy_state(L);
  (void)lua_lessthan(co, 1, 2);
  return 0;
}

static int busy_getmetatable(lua_State *L)
{
  lua_State *co = busy_state(L);
  (void)lua_getmetatable(co, 1);
  return 0;
}

static int busy_callmeta(lua_State *L)
{
  lua_State *co = busy_state(L);
  (void)luaL_callmeta(co, 1, "__probe");
  return 0;
}

static int busy_getmetafield(lua_State *L)
{
  lua_State *co = busy_state(L);
  (void)luaL_getmetafield(co, 1, "__probe");
  return 0;
}

static void expect_busy(lua_State *L, lua_CFunction fn)
{
  const char *msg;
  lua_pushcfunction(L, fn);
  assert(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
  msg = lua_tostring(L, -1);
  assert(msg != NULL && strstr(msg, "thread busy") != NULL);
  assert(last_busy_state != NULL);
  lj_state_owner_rel(last_busy_state, 0);
  last_busy_state = NULL;
  lua_pop(L, 1);
}

static void test_busy(lua_State *L)
{
  lua_settop(L, 0);
  expect_busy(L, busy_rawequal);
  expect_busy(L, busy_equal);
  expect_busy(L, busy_less);
  expect_busy(L, busy_getmetatable);
  expect_busy(L, busy_getmetafield);
  expect_busy(L, busy_callmeta);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  TGState *tg;
  uint32_t roots;
  assert(L != NULL);
  luaL_openlibs(L);
  tg = L2TG(L);
  roots = lj_tg_root_anchor_top_acq(tg);

  test_nil_invalid_and_negative_growth(L);
  test_pseudo_and_upvalue_pairs(L);
  test_table_userdata_meta(L);
  test_actual_nil_metatable(L);
  test_raw_metatable_and_metafield_gc(L);
  test_error_cleanup(L);
  test_ownerless_success(L);
  test_ownerless_error_unwind(L);
  test_busy(L);

  lua_settop(L, 0);
  full_cycle(L);
  assert(lj_tg_root_anchor_top_acq(tg) == roots);
  lua_close(L);
  puts("arm64 C API meta roots: OK");
  return 0;
}
