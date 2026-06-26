/*
** Focused x64 guard for TSET array fast paths over forwarded slots.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_tab.h"

#include "lib/tab_forward_helpers.h"

enum {
  TSETM_FORWARD_START = 21,
  TSETM_FORWARD_N = 3,
  TSETM_FORWARD_VAL0 = 9201,
  TSETM_FORWARD_VAL1 = 9202,
  TSETM_FORWARD_VAL2 = 9203
};

typedef struct TSetMForwardHookCtx {
  int done;
  GCtab *t;
  TValue *oldarray;
  TValue *newarray;
  MSize oldasize;
  MSize newasize;
  MSize oldacap;
} TSetMForwardHookCtx;

static TSetMForwardHookCtx *tsetm_forward_hook_ctx;

static int tsetm_forward_local_i32(lua_State *L, lua_Debug *ar, int slot,
				   int32_t want)
{
  const char *name = lua_getlocal(L, ar, slot);
  int ok;
  if (name == NULL)
    return 0;
  ok = lua_isnumber(L, -1) && (int32_t)lua_tointeger(L, -1) == want;
  lua_pop(L, 1);
  return ok;
}

static GCtab *tsetm_forward_temp_table(lua_State *L, lua_Debug *ar, int slot)
{
  const char *name = lua_getlocal(L, ar, slot);
  GCtab *t = NULL;
  if (name != NULL) {
    if (strcmp(name, "(*temporary)") == 0 && lua_type(L, -1) == LUA_TTABLE)
      t = tabV(L->top-1);
    lua_pop(L, 1);
  }
  return t;
}

static void tsetm_forward_hook(lua_State *L, lua_Debug *ar)
{
  TSetMForwardHookCtx *ctx = tsetm_forward_hook_ctx;
  int slot;
  if (ctx == NULL || ctx->done)
    return;
  for (slot = 1; slot <= 16; slot++) {
    GCtab *t = tsetm_forward_temp_table(L, ar, slot);
    uint32_t needed = (uint32_t)(TSETM_FORWARD_START + TSETM_FORWARD_N);
    MSize i;
    if (t == NULL)
      continue;
    if (!tsetm_forward_local_i32(L, ar, slot + 1, TSETM_FORWARD_VAL0) ||
	!tsetm_forward_local_i32(L, ar, slot + 2, TSETM_FORWARD_VAL1) ||
	!tsetm_forward_local_i32(L, ar, slot + 3, TSETM_FORWARD_VAL2))
      continue;

    ctx->done = 1;
    ctx->t = t;
    if (lj_tab_asize_acq(t) < (MSize)needed)
      lj_tab_resize(L, t, needed, 0);
    assert(lj_tab_array_separated(t));
    ctx->oldarray = lj_tab_array_acq(t);
    ctx->oldasize = lj_tab_asize_acq(t);
    ctx->oldacap = t->acap;
    assert(ctx->oldasize >= (MSize)needed);

    lj_tab_resize(L, t, (uint32_t)ctx->oldasize + 8u, 0);
    ctx->newarray = lj_tab_array_acq(t);
    ctx->newasize = lj_tab_asize_acq(t);
    assert(ctx->newarray != ctx->oldarray);
    assert(lj_tab_array_nextgen_acq(ctx->oldarray) == ctx->newarray);

    for (i = 0; i < (MSize)TSETM_FORWARD_N; i++)
      tabfwd_store_forward(&ctx->oldarray[TSETM_FORWARD_START + i]);
    la_store32_rel(&lj_tab_array_hdrw(ctx->oldarray)->acap,
		   lj_tab_array_hdr_pack_acap(ctx->oldacap, 0));
    lj_tab_asize_rel(t, ctx->oldasize);
    lj_tab_array_rel(t, ctx->oldarray);
    return;
  }
}

static void exercise_vm_tsetm_forward_retry(lua_State *L)
{
  TSetMForwardHookCtx ctx;
  GCtab *t;
  MSize i;
  const int32_t want[TSETM_FORWARD_N] = {
    TSETM_FORWARD_VAL0, TSETM_FORWARD_VAL1, TSETM_FORWARD_VAL2
  };

  memset(&ctx, 0, sizeof(ctx));
  lua_settop(L, 0);
  tsetm_forward_hook_ctx = &ctx;
  lua_sethook(L, tsetm_forward_hook, LUA_MASKCOUNT, 1);
  tabfwd_load_lua(L,
    "local function vals()\n"
    "  return 9201, 9202, 9203\n"
    "end\n"
    "tsetm_forward_result = {\n"
    "  1, 2, 3, 4, 5,\n"
    "  6, 7, 8, 9, 10,\n"
    "  11, 12, 13, 14, 15,\n"
    "  16, 17, 18, 19, 20,\n"
    "  vals()\n"
    "}\n");
  tabfwd_run_loaded(L);
  lua_sethook(L, NULL, 0, 0);
  tsetm_forward_hook_ctx = NULL;

  assert(ctx.done);
  lua_getglobal(L, "tsetm_forward_result");
  assert(lua_type(L, -1) == LUA_TTABLE);
  t = tabV(L->top-1);
  assert(t == ctx.t);
  assert(lj_tab_array_acq(t) == ctx.oldarray);

  for (i = 0; i < (MSize)TSETM_FORWARD_N; i++) {
    int32_t key = TSETM_FORWARD_START + (int32_t)i;
    tabfwd_assert_forward(&ctx.oldarray[key]);
    tabfwd_assert_i32(&ctx.newarray[key], want[i]);
    tabfwd_assert_i32(lj_tab_getint(t, key), want[i]);
  }

  lj_tab_array_rel(t, ctx.newarray);
  lj_tab_asize_rel(t, ctx.newasize);
  lj_tab_array_hdr_flags_or_rel(ctx.oldarray, TABARRAY_FLAG_RETIRING);
  lua_pop(L, 1);
  lua_pushnil(L);
  lua_setglobal(L, "tsetm_forward_result");
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  TValue *oldarray, *newarray;
  MSize oldasize, newasize, oldacap;
  int32_t key_b = 3, key_v = 4, key_r = 5;
  int32_t key_helper = 6;
  int32_t val_b = 9103, val_v = 9104, val_r = 9105;
  int32_t val_helper = 9106;
  MSize i;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L, "jit.off()\n") == 0);

  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert((MSize)key_r < oldasize);
  assert((MSize)key_helper < oldasize);
  for (i = 0; i < oldasize; i++) {
    int32_t v = (int32_t)i + 6000;
    tabfwd_set_int(L, t, (int32_t)i, v);
    lj_tab_storeint(L, &oldarray[i], v);
  }
  lua_setglobal(L, "tset_forward_t");
  lua_pushinteger(L, key_v);
  lua_setglobal(L, "tset_forward_vkey");
  lua_pushinteger(L, key_r);
  lua_setglobal(L, "tset_forward_rkey");
  lua_pushinteger(L, val_b);
  lua_setglobal(L, "tset_forward_bvalue");
  lua_pushinteger(L, val_v);
  lua_setglobal(L, "tset_forward_vvalue");
  lua_pushinteger(L, val_r);
  lua_setglobal(L, "tset_forward_rvalue");
  tabfwd_load_lua(L,
    "local t = tset_forward_t\n"
    "local kv = tset_forward_vkey\n"
    "local kr = tset_forward_rkey\n"
    "t[3] = tset_forward_bvalue\n"
    "t[kv] = tset_forward_vvalue\n"
    "for i = kr, kr do t[i] = tset_forward_rvalue end\n");

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert(tabfwd_get_i32(t, key_b) == key_b + 6000);
  assert(tabfwd_get_i32(t, key_v) == key_v + 6000);
  assert(tabfwd_get_i32(t, key_r) == key_r + 6000);

  tabfwd_store_forward(&oldarray[key_b]);
  tabfwd_store_forward(&oldarray[key_v]);
  tabfwd_store_forward(&oldarray[key_r]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  tabfwd_run_loaded(L);

  tabfwd_assert_forward(&oldarray[key_b]);
  tabfwd_assert_forward(&oldarray[key_v]);
  tabfwd_assert_forward(&oldarray[key_r]);
  assert(tabfwd_get_i32(t, key_b) == val_b);
  assert(tabfwd_get_i32(t, key_v) == val_v);
  assert(tabfwd_get_i32(t, key_r) == val_r);
  tabfwd_assert_i32(&newarray[key_b], val_b);
  tabfwd_assert_i32(&newarray[key_v], val_v);
  tabfwd_assert_i32(&newarray[key_r], val_r);

  {
    TValue src;
    TValue *stored;
    setintV(&src, val_helper);
    lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
    stored = lj_tab_storetv_forvm_array(L, t, &oldarray[key_helper], &src,
					(MSize)key_helper);
    assert(stored == &newarray[key_helper]);
    tabfwd_assert_i32(&oldarray[key_helper], key_helper + 6000);
    tabfwd_assert_i32(&newarray[key_helper], val_helper);
  }

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);

  exercise_vm_tsetm_forward_retry(L);

  lua_close(L);
  printf("t-x64-tset-forward OK: TSET/TSETM fast paths reroute forwarded slots\n");
  return 0;
}
