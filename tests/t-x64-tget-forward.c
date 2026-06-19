/*
** Focused x64 guard for TGET array fast paths over forwarded slots.
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
  int32_t target = 3;
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
  assert((MSize)target < oldasize);
  for (i = 0; i < oldasize; i++) {
    int32_t v = (int32_t)i + 3000;
    set_int(L, t, (int32_t)i, v);
    lj_tab_storeint(L, &oldarray[i], v);
  }
  assert(get_i32(t, target) == target + 3000);
  lua_setglobal(L, "tget_forward_t");
  lua_pushinteger(L, target);
  lua_setglobal(L, "tget_forward_key");
  lua_pushinteger(L, target + 3000);
  lua_setglobal(L, "tget_forward_value");
  load_lua(L,
    "local t = tget_forward_t\n"
    "local k = tget_forward_key\n"
    "local want = tget_forward_value\n"
    "local function getv(a, key) return a[key] end\n"
    "assert(t[3] == want, type(t[3]))\n"
    "assert(t[k] == want, type(t[k]))\n"
    "assert(getv(t, k) == want, type(getv(t, k)))\n");

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  assert(get_i32(t, target) == target + 3000);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);

  store_forward(&oldarray[target]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  assert(get_i32(t, target) == target + 3000);
  run_loaded(L);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);

  lua_close(L);
  printf("t-x64-tget-forward OK: TGET fast paths resolve forwarded array slots\n");
  return 0;
}
