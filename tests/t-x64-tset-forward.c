/*
** Focused x64 guard for TSET array fast paths over forwarded slots.
*/

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_tab.h"

#include "lib/tab_forward_helpers.h"

/* Built by the M5/M6 harness with LJ_TAB_TEST_HELPERS enabled. */

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
} TSetMForwardHookCtx;

static TSetMForwardHookCtx *tsetm_forward_hook_ctx;

static const char tsetb_forward_src[] =
  "local t = tset_forward_t\n"
  "t[3] = tset_forward_bvalue\n";

static const char tsetv_forward_src[] =
  "local t = tset_forward_t\n"
  "local kv = tset_forward_vkey\n"
  "t[kv] = tset_forward_vvalue\n";

static const char tsetr_forward_src[] =
  "local k = tset_forward_rkey\n"
  "assert(table.move(tset_forward_src_t, k, k, k, tset_forward_t) ==\n"
  "       tset_forward_t)\n";

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
    assert(lj_tab_asize_acq(t) >= (MSize)needed);
    assert(!lj_tab_array_is_retiring(t, ctx->oldarray));
    assert(lj_tab_array_nextgen_acq(ctx->oldarray) == NULL);
    assert(lj_tab_struct_owner_acq(t) == 0);

    for (i = 0; i < (MSize)TSETM_FORWARD_N; i++)
      tabfwd_store_forward(&ctx->oldarray[TSETM_FORWARD_START + i]);
    return;
  }
}

static void exercise_vm_tsetm_forward_retry(lua_State *L)
{
  TSetMForwardHookCtx ctx;
  GCtab *t;
  TValue *newarray;
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
  /*
  ** The hook borrows the current array before TSETM repairs its first marker.
  ** Keep one outer reader across the hook, repair resize and all retired-array
  ** assertions. Nested table helpers preserve this exact reader depth.
  */
  lj_tab_read_enter(L2TG(L));
  tabfwd_run_loaded(L);
  lua_sethook(L, NULL, 0, 0);
  tsetm_forward_hook_ctx = NULL;

  assert(ctx.done);
  lua_getglobal(L, "tsetm_forward_result");
  assert(lua_type(L, -1) == LUA_TTABLE);
  t = tabV(L->top-1);
  assert(t == ctx.t);
  newarray = lj_tab_array_acq(t);
  assert(newarray != ctx.oldarray);
  assert(lj_tab_array_is_retiring(t, ctx.oldarray));
  assert(lj_tab_array_nextgen_acq(ctx.oldarray) == newarray);

  for (i = 0; i < (MSize)TSETM_FORWARD_N; i++) {
    int32_t key = TSETM_FORWARD_START + (int32_t)i;
    tabfwd_assert_forward(&ctx.oldarray[key]);
    tabfwd_assert_i32(&newarray[key], want[i]);
    tabfwd_assert_i32(lj_tab_getint(t, key), want[i]);
  }

  lj_tab_read_leave(L2TG(L));
  lua_pop(L, 1);
  lua_pushnil(L);
  lua_setglobal(L, "tsetm_forward_result");
}

static void exercise_vm_tset_forward_opcode(lua_State *L, const char *src,
					     int32_t key, int32_t want)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  MSize oldasize;

  lua_settop(L, 0);
  tabfwd_load_lua(L, src);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  tabfwd_set_int(L, t, key, key + 6000);
  lua_setglobal(L, "tset_forward_t");

  /*
  ** Capture the generation only after all setup which may allocate. This
  ** reader starts while oldarray is still current, so it remains valid after
  ** the stable-FORWARD repair retires that generation.
  */
  lj_tab_read_enter(L2TG(L));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert((MSize)key < oldasize);
  assert(!lj_tab_array_is_retiring(t, oldarray));
  assert(lj_tab_array_nextgen_acq(oldarray) == NULL);
  assert(lj_tab_struct_owner_acq(t) == 0);
  tabfwd_store_forward(&oldarray[key]);

  tabfwd_run_loaded(L);

  newarray = lj_tab_array_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_is_retiring(t, oldarray));
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  tabfwd_assert_forward(&oldarray[key]);
  tabfwd_assert_i32(&newarray[key], want);
  assert(tabfwd_get_i32(t, key) == want);
  lj_tab_read_leave(L2TG(L));

  lua_pushnil(L);
  lua_setglobal(L, "tset_forward_t");
}

static void exercise_vm_tsetr_forward(lua_State *L, int32_t key,
				       int32_t want)
{
  GCtab *src, *t;
  TValue *oldarray, *newarray;
  MSize oldasize;

  lua_settop(L, 0);
  tabfwd_load_lua(L, tsetr_forward_src);

  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  src = tabV(L->top-1);
  assert(lj_tab_array_separated(src));
  tabfwd_set_int(L, src, key, want);
  lua_setglobal(L, "tset_forward_src_t");

  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  tabfwd_set_int(L, t, key, key + 6000);
  lua_setglobal(L, "tset_forward_t");

  /*
  ** table.move is generated library bytecode: its raw destination write is
  ** BC_TSETR. The source table remains ordinary, while only the destination
  ** slot is made a stable orphan marker.
  */
  lj_tab_read_enter(L2TG(L));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert((MSize)key < oldasize);
  assert(!lj_tab_array_is_retiring(t, oldarray));
  assert(lj_tab_array_nextgen_acq(oldarray) == NULL);
  assert(lj_tab_struct_owner_acq(t) == 0);
  tabfwd_store_forward(&oldarray[key]);

  tabfwd_run_loaded(L);

  newarray = lj_tab_array_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_is_retiring(t, oldarray));
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  tabfwd_assert_forward(&oldarray[key]);
  tabfwd_assert_i32(&newarray[key], want);
  assert(tabfwd_get_i32(t, key) == want);
  assert(tabfwd_get_i32(src, key) == want);
  lj_tab_read_leave(L2TG(L));

  lua_pushnil(L);
  lua_setglobal(L, "tset_forward_t");
  lua_pushnil(L);
  lua_setglobal(L, "tset_forward_src_t");
}

static void exercise_vm_tset_entering_helpers(lua_State *L)
{
  global_State *g = G(L);
  uint32_t array_calls0, hash_calls0;

  lua_settop(L, 0);
  lj_tab_test_reset_vm_array_store_calls();
  lj_tab_test_reset_vm_strhash_store_calls();
  array_calls0 = lj_tab_test_vm_array_store_calls();
  hash_calls0 = lj_tab_test_vm_strhash_store_calls();

  assert(mt_entering_add_rlx(g, 1) == 0);
  tabfwd_load_lua(L,
    "local t = { 0, 0, 0, stable = 0 }\n"
    "local k = 2\n"
    "t[1] = 1101\n"
    "t[k] = 2202\n"
    "for i = 3, 3 do t[i] = 3303 end\n"
    "t.stable = 4404\n"
    "local h = { nilslot = 1 }\n"
    "h.nilslot = nil\n"
    "h.nilslot = 5505\n"
    "assert(t[1] == 1101 and t[2] == 2202 and t[3] == 3303)\n"
    "assert(t.stable == 4404)\n"
    "assert(h.nilslot == 5505)\n");
  tabfwd_run_loaded(L);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, INT_MAX);

  assert(lj_tab_test_vm_array_store_calls() >= array_calls0 + 3u);
  assert(lj_tab_test_vm_strhash_store_calls() >= hash_calls0 + 2u);
}

static void exercise_vm_tsets_single_thread_direct(lua_State *L)
{
  uint32_t array_calls0, hash_calls0;

  lua_settop(L, 0);
  lj_tab_test_reset_vm_array_store_calls();
  lj_tab_test_reset_vm_strhash_store_calls();
  array_calls0 = lj_tab_test_vm_array_store_calls();
  hash_calls0 = lj_tab_test_vm_strhash_store_calls();

  tabfwd_load_lua(L,
    "local t = { fast = 1, nilslot = 2 }\n"
    "t.nilslot = nil\n"
    "t.fast = 4404\n"
    "t.nilslot = 5505\n"
    "assert(t.fast == 4404)\n"
    "assert(t.nilslot == 5505)\n");
  tabfwd_run_loaded(L);

  assert(lj_tab_test_vm_array_store_calls() == array_calls0);
  assert(lj_tab_test_vm_strhash_store_calls() == hash_calls0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  int32_t key_b = 3, key_v = 4, key_r = 5;
  int32_t val_b = 9103, val_v = 9104, val_r = 9105;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L, "jit.off()\n") == 0);

  lua_pushinteger(L, key_v);
  lua_setglobal(L, "tset_forward_vkey");
  lua_pushinteger(L, key_r);
  lua_setglobal(L, "tset_forward_rkey");
  lua_pushinteger(L, val_b);
  lua_setglobal(L, "tset_forward_bvalue");
  lua_pushinteger(L, val_v);
  lua_setglobal(L, "tset_forward_vvalue");

  exercise_vm_tset_forward_opcode(L, tsetb_forward_src, key_b, val_b);
  exercise_vm_tset_forward_opcode(L, tsetv_forward_src, key_v, val_v);
  exercise_vm_tsetr_forward(L, key_r, val_r);

  exercise_vm_tsets_single_thread_direct(L);
  exercise_vm_tset_entering_helpers(L);
  exercise_vm_tsetm_forward_retry(L);

  lua_close(L);
  printf("t-x64-tset-forward OK: TSET/TSETM fast paths reroute forwarded slots\n");
  return 0;
}
