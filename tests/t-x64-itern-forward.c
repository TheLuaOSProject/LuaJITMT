/*
** Focused x64 guard for BC_ITERN over forwarded table slots.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

#include "lib/tab_forward_helpers.h"

static const char itern_forward_array_src[] =
  "local seen = false\n"
  "for k, v in pairs(itern_forward_array_t) do\n"
  "  if v == itern_forward_array_value then seen = true end\n"
  "end\n"
  "assert(seen, 'missing array successor value')\n";

static void exercise_array_forward(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  MSize oldasize, newasize, oldacap;
  int32_t target = 3;
  MSize i;

  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert((MSize)target < oldasize);
  for (i = 0; i < oldasize; i++) {
    int32_t v = (int32_t)i + 5100;
    tabfwd_set_int(L, t, (int32_t)i, v);
    lj_tab_storeint(L, &oldarray[i], v);
  }
  assert(tabfwd_get_i32(t, target) == target + 5100);
  lua_setglobal(L, "itern_forward_array_t");
  lua_pushinteger(L, target + 5100);
  lua_setglobal(L, "itern_forward_array_value");
  tabfwd_load_lua(L, itern_forward_array_src);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);

  tabfwd_store_forward(&oldarray[target]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  assert(tabfwd_get_i32(t, target) == target + 5100);
  tabfwd_run_loaded(L);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  lj_tab_asize_rel(t, 0);
  tabfwd_load_lua(L, itern_forward_array_src);
  tabfwd_run_loaded(L);
  lj_tab_asize_rel(t, newasize);
}

static void exercise_hash_forward(lua_State *L)
{
  GCtab *t;
  GCstr *key;
  Node *oldnode, *newnode;
  MSize oldhmask, newhmask;
  TValue *oldslot;

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  key = lj_str_new(L, "itern_forward_field",
		   sizeof("itern_forward_field") - 1u);
  lj_tab_storeint(L, lj_tab_setstr(L, t, key), 6262);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldslot = tabfwd_find_str_slot(oldnode, oldhmask, key);
  assert(oldslot != NULL);
  lua_setglobal(L, "itern_forward_hash_t");
  tabfwd_load_lua(L,
    "local seen = false\n"
    "for k, v in pairs(itern_forward_hash_t) do\n"
    "  if k == 'itern_forward_field' and v == 6262 then seen = true end\n"
    "end\n"
    "assert(seen, 'missing hash successor value')\n");

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);

  tabfwd_store_forward(oldslot);
  la_store32_rel(&lj_tab_node_hdrw(oldnode)->flags, 0);
  lj_tab_hmask_rel(t, oldhmask);
  lj_tab_node_rel(t, oldnode);
  tabfwd_run_loaded(L);

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L, "jit.off()\n") == 0);

  exercise_array_forward(L);
  exercise_hash_forward(L);

  lua_close(L);
  printf("t-x64-itern-forward OK: BC_ITERN resolves forwarded slots\n");
  return 0;
}
