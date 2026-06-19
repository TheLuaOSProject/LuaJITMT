/*
** Focused x64 guard for TGETS over a forwarded hash slot.
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

static void store_forward(TValue *slot)
{
  TValue forward;
  setforwardV(&forward);
  tv_rawstore_rel(slot, tv_rawload(&forward));
}

static TValue *find_str_slot(Node *node, MSize hmask, const GCstr *key)
{
  Node *n = hashstr_node(node, hmask, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisstr(&nk) && strV(&nk) == key)
      return &n->val;
  } while ((n = lj_tab_nextnode_acq(n)));
  return NULL;
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
  GCstr *key;
  Node *oldnode, *newnode;
  MSize oldhmask, newhmask;
  TValue *oldslot;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L, "jit.off()\n") == 0);

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  key = lj_str_new(L, "tgets_forward_field",
		   sizeof("tgets_forward_field") - 1u);
  lj_tab_storeint(L, lj_tab_setstr(L, t, key), 4242);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldslot = find_str_slot(oldnode, oldhmask, key);
  assert(oldslot != NULL);

  lua_setglobal(L, "tgets_forward_t");
  load_lua(L,
    "local t = tgets_forward_t\n"
    "assert(t.tgets_forward_field == 4242, type(t.tgets_forward_field))\n");

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  assert(lj_tab_getstr(t, key) != NULL);

  store_forward(oldslot);
  la_store32_rel(&lj_tab_node_hdrw(oldnode)->flags, 0);
  lj_tab_hmask_rel(t, oldhmask);
  lj_tab_node_rel(t, oldnode);
  run_loaded(L);

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);

  lua_close(L);
  printf("t-x64-tgets-forward OK: TGETS resolves forwarded hash slots\n");
  return 0;
}
