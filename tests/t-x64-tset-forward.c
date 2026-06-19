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

static void store_forward(TValue *slot)
{
  TValue forward;
  setforwardV(&forward);
  tv_rawstore_rel(slot, tv_rawload(&forward));
}

static void assert_forward(cTValue *tv)
{
  TValue val;
  lj_tv_load_acq(&val, tv);
  assert(tvisforward(&val));
}

static void set_int(lua_State *L, GCtab *t, int32_t k, int32_t v)
{
  lj_tab_storeint(L, lj_tab_setint(L, t, k), v);
}

static int32_t get_i32(GCtab *t, int32_t k)
{
  cTValue *tv = lj_tab_getint(t, k);
  assert(tv != NULL);
  assert(tvisnumber(tv));
  return tvisint(tv) ? intV(tv) : (int32_t)numV(tv);
}

static void assert_i32(cTValue *tv, int32_t want)
{
  assert(tv != NULL);
  assert(tvisnumber(tv));
  assert((tvisint(tv) ? intV(tv) : (int32_t)numV(tv)) == want);
}

static void load_lua(lua_State *L, const char *src)
{
  int status = luaL_loadstring(L, src);
  if (status != 0) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "luaL_loadstring failed");
  }
  assert(status == 0);
}

static void run_loaded(lua_State *L)
{
  int status = lua_pcall(L, 0, 0, 0);
  if (status != 0) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "lua_pcall failed");
  }
  assert(status == 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  TValue *oldarray, *newarray;
  MSize oldasize, newasize, oldacap;
  int32_t key_b = 3, key_v = 4, key_r = 5;
  int32_t val_b = 9103, val_v = 9104, val_r = 9105;
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
  for (i = 0; i < oldasize; i++) {
    int32_t v = (int32_t)i + 6000;
    set_int(L, t, (int32_t)i, v);
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
  load_lua(L,
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
  assert(get_i32(t, key_b) == key_b + 6000);
  assert(get_i32(t, key_v) == key_v + 6000);
  assert(get_i32(t, key_r) == key_r + 6000);

  store_forward(&oldarray[key_b]);
  store_forward(&oldarray[key_v]);
  store_forward(&oldarray[key_r]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  run_loaded(L);

  assert_forward(&oldarray[key_b]);
  assert_forward(&oldarray[key_v]);
  assert_forward(&oldarray[key_r]);
  assert(get_i32(t, key_b) == val_b);
  assert(get_i32(t, key_v) == val_v);
  assert(get_i32(t, key_r) == val_r);
  assert_i32(&newarray[key_b], val_b);
  assert_i32(&newarray[key_v], val_v);
  assert_i32(&newarray[key_r], val_r);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);

  lua_close(L);
  printf("t-x64-tset-forward OK: TSET fast paths slow-path forwarded slots\n");
  return 0;
}
