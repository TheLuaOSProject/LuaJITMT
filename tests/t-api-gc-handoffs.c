/*
** Public API GC-valued handoff regression: generated strings, constructor
** environments, suspended targets, exact root-anchor bounds and OOM cleanup.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_func.h"
#include "lj_frame.h"
#include "lj_gc.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_udata.h"
#include "lj_vm.h"

#if !defined(LJ_TG_ROOT_TEST_HELPERS)
#error "t-api-gc-handoffs requires LJ_TG_ROOT_TEST_HELPERS"
#endif

#if !defined(LJ_API_ROOT_TEST_HELPERS)
#error "t-api-gc-handoffs requires LJ_API_ROOT_TEST_HELPERS"
#endif

typedef void (*LJApiNewMetatableHook)(lua_State *L, int stage, GCtab *regt,
				      GCstr *key, TValue *valueslot,
				      TValue *rootslot);
extern void lj_api_test_set_newmetatable_hook(LJApiNewMetatableHook hook);

enum RootExpect {
  ROOT_EXPECT_NONE,
  ROOT_EXPECT_STR,
  ROOT_EXPECT_TAB
};

static enum RootExpect root_expect;
static const char *root_expect_str;
static GCtab *root_expect_tab;
static uint32_t root_hook_hits;
static GCtab *race_mt;
static GCstr *race_hold_key;
static uint32_t race_anchor_base;
static uint32_t race_existing_hits;
static uint32_t race_competing_hits;

enum NewMetatableRaceMode {
  NEWMT_RACE_NONE,
  NEWMT_RACE_EXISTING,
  NEWMT_RACE_COMPETING
};

static enum NewMetatableRaceMode race_mode;

static void full_cycle(lua_State *L)
{
  lua_gc(L, LUA_GCRESTART, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
}

static void root_full_gc_hook(lua_State *L, TGState *tg, uint32_t idx,
			      TValue *slot)
{
  TValue before, after;
  UNUSED(tg);
  UNUSED(idx);
  lj_tv_load_acq(&before, slot);
  if (root_expect == ROOT_EXPECT_STR) {
    assert(tvisstr(&before));
    assert(strcmp(strdata(strV(&before)), root_expect_str) == 0);
  } else if (root_expect == ROOT_EXPECT_TAB) {
    assert(tvistab(&before));
    assert(tabV(&before) == root_expect_tab);
  } else {
    assert(0 && "unexpected API root hook");
  }
  root_hook_hits++;
  full_cycle(L);
  lj_tv_load_acq(&after, slot);
  assert(tv_rawload(&after) == tv_rawload(&before));
}

static void arm_str_hook(const char *str)
{
  root_expect = ROOT_EXPECT_STR;
  root_expect_str = str;
  root_expect_tab = NULL;
  lj_tg_root_test_set_push_hook(root_full_gc_hook);
}

static void arm_tab_hook(GCtab *tab)
{
  root_expect = ROOT_EXPECT_TAB;
  root_expect_str = NULL;
  root_expect_tab = tab;
  lj_tg_root_test_set_push_hook(root_full_gc_hook);
}

static int dummy_cfunc(lua_State *L)
{
  UNUSED(L);
  return 0;
}

typedef struct FailCtx {
  lua_State *target;
  int constructor;
} FailCtx;

static TValue *reserve_fail_cp(lua_State *L, lua_CFunction dummy, void *ud)
{
  FailCtx *ctx = (FailCtx *)ud;
  UNUSED(dummy);
  cframe_errfunc(L->cframe) = -1;
  if (ctx->constructor)
    lua_pushcclosure(ctx->target, dummy_cfunc, 0);
  else
    lua_pushliteral(ctx->target, "reserve-failure");
  return NULL;
}

static TValue *limit_push_cp(lua_State *L, lua_CFunction dummy, void *ud)
{
  TValue nilv;
  UNUSED(dummy);
  UNUSED(ud);
  cframe_errfunc(L->cframe) = -1;
  setnilV(&nilv);
  (void)lj_tg_root_anchor_push(L, L2TG(L), &nilv, NULL);
  return NULL;
}

static void test_reserve_oom_and_bound(lua_State *L, lua_State *co)
{
  TGState *tg = L2TG(L);
  TValue nilv;
  uint32_t idx[16], i, baseline = lj_tg_root_anchor_top_acq(tg);
  TGState *saved_hint = co->tg_hint;
  FailCtx ctx;
  int status;

  assert(lj_tg_root_anchor_next_acq(&tg->root_anchor) == NULL);
  setnilV(&nilv);
  for (i = 0; i < 16; i++) {
    assert(lj_tg_root_anchor_push(L, tg, &nilv, &idx[i]) != NULL);
    assert(idx[i] == baseline + i);
  }
  assert(lj_tg_root_anchor_top_acq(tg) == baseline + 16u);
  assert(lj_tg_root_anchor_next_acq(&tg->root_anchor) == NULL);

  /* A tryclaim preclaim must preserve an existing ownerless-state TG hint on
  ** both the string-reserve and constructor-env-reserve failures. */
  co->tg_hint = tg;
  ctx.target = co;
  ctx.constructor = 0;
  lj_tg_root_test_fail_reserve_after(1);
  status = lj_vm_cpcall(L, NULL, &ctx, reserve_fail_cp);
  assert(status == LUA_ERRMEM);
  assert(lj_state_owner_acq(co) == 0);
  assert(co->tg_hint == tg);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline + 16u);

  ctx.constructor = 1;
  lj_tg_root_test_fail_reserve_after(1);
  status = lj_vm_cpcall(L, NULL, &ctx, reserve_fail_cp);
  assert(status == LUA_ERRMEM);
  assert(lj_state_owner_acq(co) == 0);
  assert(co->tg_hint == tg);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline + 16u);
  co->tg_hint = saved_hint;

  for (i = 16; i-- > 0; )
    lj_tg_root_anchor_pop(tg, idx[i]);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);

  /* Exercise both producer gates at the exact remote-scanner boundary without
  ** allocating or traversing a million anchor objects. */
  lj_tg_root_anchor_top_rel(tg, LJ_ROOT_SCAN_LIMIT);
  assert(!lj_tg_root_anchor_reserve_nothrow(L, tg));
  status = lj_vm_cpcall(L, NULL, NULL, limit_push_cp);
  assert(status == LUA_ERRMEM);
  assert(lj_tg_root_anchor_top_acq(tg) == LJ_ROOT_SCAN_LIMIT);
  lj_tg_root_anchor_top_rel(tg, baseline);
}

static int mm_index(lua_State *L)
{
  assert(lua_gettop(L) == 2);
  assert(lua_type(L, 2) == LUA_TSTRING);
  full_cycle(L);
  lua_pushvalue(L, 2);
  return 1;
}

static int mm_newindex(lua_State *L)
{
  assert(lua_gettop(L) == 3);
  full_cycle(L);
  lua_rawset(L, 1);
  return 0;
}

static int mm_ping(lua_State *L)
{
  full_cycle(L);
  lua_pushliteral(L, "pong");
  return 1;
}

static void newmetatable_race_hook(lua_State *L, int stage, GCtab *regt,
				   GCstr *key, TValue *valueslot,
				   TValue *rootslot)
{
  TValue keytv, mtv, nilv, old;
  TGState *tg = L2TG(L);
  int rc;
  setstrV(L, &keytv, key);
  if (stage == 0) {
    TValue snap;
    TValue *slot;
    assert(race_mode == NEWMT_RACE_EXISTING);
    assert(valueslot == NULL && rootslot != NULL);
    assert(lj_tg_root_anchor_top_acq(tg) == race_anchor_base + 3u);
    assert(rootslot ==
	   lj_tg_root_anchor_slot_acq(tg, race_anchor_base + 2u));
    lj_tv_load_acq(&snap, rootslot);
    assert(tvistab(&snap) && tabV(&snap) == race_mt);

    /* The lookup helper has returned, but luaL_newmetatable has not yet
    ** acquired/grown the target stack. Remove the registry's sole edge and
    ** collect at this first caller-visible boundary. Only the release-published
    ** dedicated winner anchor can preserve the table. */
    slot = lj_tab_setstr(L, regt, key);
    setnilV(&nilv);
    rc = lj_tab_trystoretv_cas_keyed(L, regt, slot, &keytv, &nilv);
    assert(rc == LJ_TAB_STORE_CAS_OK);
    full_cycle(L);
    lj_tv_load_acq(&snap, rootslot);
    assert(tvistab(&snap) && tabV(&snap) == race_mt);
    assert(lj_tg_root_anchor_top_acq(tg) == race_anchor_base + 3u);
    race_existing_hits++;
  } else if (stage == 1) {
    assert(race_mode == NEWMT_RACE_COMPETING);
    assert(valueslot != NULL && rootslot != NULL);
    assert(lj_tg_root_anchor_top_acq(tg) == race_anchor_base + 4u);
    assert(rootslot ==
	   lj_tg_root_anchor_slot_acq(tg, race_anchor_base + 1u));
    settabV(L, &mtv, race_mt);
    rc = lj_tab_trysetnil_cas_keyed(L, regt, valueslot, &keytv, &mtv,
				    &old);
    assert(rc == LJ_TAB_STORE_CAS_OK);
    lj_gc_pubtab(L, regt);
    full_cycle(L);  /* regt, key and losing mt roots all remain published. */
  } else {
    TValue snap;
    TValue holdkeytv;
    TValue *holdslot;
    assert(race_mode == NEWMT_RACE_COMPETING);
    assert(stage == 2 && rootslot != NULL);
    assert(lj_tg_root_anchor_top_acq(tg) == race_anchor_base + 4u);
    assert(rootslot ==
	   lj_tg_root_anchor_slot_acq(tg, race_anchor_base + 2u));
    lj_tv_load_acq(&snap, rootslot);
    assert(tvistab(&snap) && tabV(&snap) == race_mt);
    setnilV(&nilv);
    rc = lj_tab_trystoretv_cas_keyed(L, regt, valueslot, &keytv, &nilv);
    assert(rc == LJ_TAB_STORE_CAS_OK);
    setstrV(L, &holdkeytv, race_hold_key);
    holdslot = lj_tab_setstr(L, regt, race_hold_key);
    rc = lj_tab_trystoretv_cas_keyed(L, regt, holdslot, &holdkeytv,
				     &nilv);
    assert(rc == LJ_TAB_STORE_CAS_OK);
    full_cycle(L);
    lj_tv_load_acq(&snap, rootslot);
    assert(tvistab(&snap) && tabV(&snap) == race_mt);
    assert(lj_tg_root_anchor_top_acq(tg) == race_anchor_base + 4u);
    race_competing_hits++;
  }
}

static void test_string_and_key_roots(lua_State *L, lua_State *co)
{
  TGState *tg = L2TG(L);
  uint32_t baseline = lj_tg_root_anchor_top_acq(tg);
  uint32_t hits = root_hook_hits;
  size_t len;
  const char *s;

  arm_str_hook("alpha");
  lua_pushliteral(co, "alpha");
  assert(strcmp(lua_tostring(co, -1), "alpha") == 0);
  lua_pop(co, 1);

  arm_str_hook("beta");
  lua_pushstring(co, "beta");
  assert(strcmp(lua_tostring(co, -1), "beta") == 0);
  lua_pop(co, 1);

  arm_str_hook("fmt-17");
  s = lua_pushfstring(co, "fmt-%d", 17);
  assert(strcmp(s, "fmt-17") == 0);
  lua_pop(co, 1);

  lua_pushnumber(co, 123.5);
  arm_str_hook("123.5");
  s = lua_tolstring(co, -1, &len);
  assert(len == 5 && strcmp(s, "123.5") == 0);
  lua_pop(co, 1);
  assert(root_hook_hits == hits + 4u);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);

  lua_newtable(L);
  lua_newtable(L);
  lua_pushcfunction(L, mm_index);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, mm_newindex);
  lua_setfield(L, -2, "__newindex");
  lua_setmetatable(L, -2);
  lua_getfield(L, -1, "missing-key");
  assert(strcmp(lua_tostring(L, -1), "missing-key") == 0);
  lua_pop(L, 1);
  lua_pushinteger(L, 21);
  lua_setfield(L, -2, "stored-key");
  lua_getfield(L, -1, "stored-key");
  assert(lua_tointeger(L, -1) == 21);
  lua_pop(L, 2);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);
}

static void test_meta_roots(lua_State *L)
{
  TGState *tg = L2TG(L);
  uint32_t baseline = lj_tg_root_anchor_top_acq(tg);
  uint32_t hits = root_hook_hits;
  uint32_t existing_hits = race_existing_hits;
  uint32_t competing_hits = race_competing_hits;
  void *ud;
  LJTabRoot race_root;

  arm_tab_hook(lj_registry_tab_acq(G(L)));
  assert(luaL_newmetatable(L, "api.gc.handoff.mt") == 1);
  lua_pushinteger(L, 44);
  lua_setfield(L, -2, "__value");
  lua_pushcfunction(L, mm_ping);
  lua_setfield(L, -2, "__ping");
  lua_pop(L, 1);
  assert(luaL_newmetatable(L, "api.gc.handoff.mt") == 0);
  lua_pop(L, 1);

  /* A pre-existing winner starts with the registry slot as its only semantic
  ** edge. Stage 0 runs immediately after the by-value lookup has transferred
  ** it into the helper's dedicated enumerated output root. */
  lua_newtable(L);
  race_mt = tabV(L->top-1);
  lua_pushinteger(L, 73);
  lua_setfield(L, -2, "__winner");
  lua_setfield(L, LUA_REGISTRYINDEX, "api.gc.handoff.existing");
  race_mode = NEWMT_RACE_EXISTING;
  race_anchor_base = lj_tg_root_anchor_top_acq(tg);
  lj_api_test_set_newmetatable_hook(newmetatable_race_hook);
  assert(luaL_newmetatable(L, "api.gc.handoff.existing") == 0);
  assert(race_existing_hits == existing_hits + 1u);
  assert(tvistab(L->top-1) && tabV(L->top-1) == race_mt);
  lua_getfield(L, -1, "__winner");
  assert(lua_tointeger(L, -1) == 73);
  lua_pop(L, 2);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);

  /* Inject a peer winner between construction and the nil-slot CAS, then
  ** delete both registry edges at stage 2, immediately after the rooted CAS
  ** helper has transferred the observed winner. The forced cycle proves the
  ** CAS-loser return does not rely on either registry snapshot. */
  race_mt = lj_tab_new_ah_rooted(L, 0, 1, &race_root);
  /* Give the peer table an ordinary root before entering the transaction, so
  ** no older constructor anchor sits below the API's reg/key/table anchors. */
  lj_state_checkstack(L, 1);
  settabV(L, L->top, race_mt);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  lj_tab_root_release(&race_root);
  lua_setfield(L, LUA_REGISTRYINDEX, "api.gc.handoff.race.hold");
  race_hold_key = lj_str_newz(L, "api.gc.handoff.race.hold");
  race_mode = NEWMT_RACE_COMPETING;
  race_anchor_base = lj_tg_root_anchor_top_acq(tg);
  lj_api_test_set_newmetatable_hook(newmetatable_race_hook);
  assert(luaL_newmetatable(L, "api.gc.handoff.race") == 0);
  assert(race_competing_hits == competing_hits + 1u);
  assert(tvistab(L->top-1) && tabV(L->top-1) == race_mt);
  lua_pop(L, 1);
  race_mode = NEWMT_RACE_NONE;
  full_cycle(L);

  ud = lua_newuserdata(L, 8);
  memset(ud, 0x5a, 8);
  luaL_getmetatable(L, "api.gc.handoff.mt");
  assert(lua_setmetatable(L, -2));

  arm_str_hook("__value");
  assert(luaL_getmetafield(L, -1, "__value") == 1);
  assert(lua_tointeger(L, -1) == 44);
  lua_pop(L, 1);

  arm_str_hook("__ping");
  assert(luaL_callmeta(L, -1, "__ping") == 1);
  assert(strcmp(lua_tostring(L, -1), "pong") == 0);
  lua_pop(L, 1);

  arm_str_hook("api.gc.handoff.mt");
  assert(luaL_testudata(L, -1, "api.gc.handoff.mt") == ud);
  lua_pop(L, 1);
  assert(root_hook_hits == hits + 4u);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);
}

static void test_constructor_env_roots(lua_State *L, lua_State *co)
{
  TGState *tg = L2TG(L);
  GCtab *env = lj_state_env_acq(co);
  uint32_t baseline = lj_tg_root_anchor_top_acq(tg);
  uint32_t hits = root_hook_hits;
  lua_State *child;
  GCfunc *fn;
  GCudata *ud;

  arm_tab_hook(env);
  lua_pushcclosure(co, dummy_cfunc, 0);
  fn = funcV(co->top-1);
  assert(lj_func_env_acq(fn) == env);
  lua_pop(co, 1);

  arm_tab_hook(env);
  child = lua_newthread(co);
  assert(lj_state_env_acq(child) == env);
  lua_pop(co, 1);

  arm_tab_hook(env);
  (void)lua_newuserdata(co, 16);
  ud = udataV(co->top-1);
  assert(lj_udata_env_acq(ud) == env);
  lua_pop(co, 1);

  assert(root_hook_hits == hits + 3u);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);
  assert(lj_state_owner_acq(co) == 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  TGState *tg;
  uint32_t baseline;
  assert(L != NULL);
  luaL_openlibs(L);
  tg = L2TG(L);
  assert(tg != NULL);
  baseline = lj_tg_root_anchor_top_acq(tg);

  co = lua_newthread(L);
  lua_pushvalue(L, -1);
  lua_setfield(L, LUA_REGISTRYINDEX, "api.gc.handoff.co");
  lua_pop(L, 1);
  assert(lj_state_owner_acq(co) == 0);

  test_reserve_oom_and_bound(L, co);
  test_string_and_key_roots(L, co);
  test_meta_roots(L);
  test_constructor_env_roots(L, co);
  full_cycle(L);
  assert(lj_tg_root_anchor_top_acq(tg) == baseline);

  lua_close(L);
  printf("t-api-gc-handoffs OK\n");
  return 0;
}
