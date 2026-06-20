/*
** Focused x64 guard for TSET array fast paths over forwarded slots.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_tab.h"

#include "lib/tab_forward_helpers.h"

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

  lua_close(L);
  printf("t-x64-tset-forward OK: TSET fast paths reroute forwarded slots\n");
  return 0;
}
