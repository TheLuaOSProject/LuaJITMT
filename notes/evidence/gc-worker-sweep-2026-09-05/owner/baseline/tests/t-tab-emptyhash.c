/*
** Guard the M5 invariant that the shared nilnode is read-only fallback
** storage and never a table insertion destination.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_tab.h"

typedef struct NilNodeSnapshot {
  TValue key;
  TValue val;
  Node *next;
} NilNodeSnapshot;

static NilNodeSnapshot nilnode_snapshot(lua_State *L)
{
  Node *nilnode = &G(L)->nilnode;
  NilNodeSnapshot snap;
  lj_tv_load_acq(&snap.key, &nilnode->key);
  lj_tv_load_acq(&snap.val, &nilnode->val);
  snap.next = lj_tab_nextnode_acq(nilnode);
  return snap;
}

static void assert_nilnode_unchanged(lua_State *L,
				     const NilNodeSnapshot *snap)
{
  Node *nilnode = &G(L)->nilnode;
  TValue key, val;
  lj_tv_load_acq(&key, &nilnode->key);
  lj_tv_load_acq(&val, &nilnode->val);
  assert(memcmp(&key, &snap->key, sizeof(key)) == 0);
  assert(memcmp(&val, &snap->val, sizeof(val)) == 0);
  assert(lj_tab_nextnode_acq(nilnode) == snap->next);
}

static GCtab *new_empty_table(lua_State *L, const NilNodeSnapshot *snap)
{
  GCtab *t;
  Node *node;
  lua_newtable(L);
  t = tabV(L->top-1);
  node = lj_tab_node_acq(t);
  assert(lj_tab_hmask_acq(t) == 0);
#if LJ_GC64
  assert(node != &G(L)->nilnode ||
	 getfreetop(t, node) == &G(L)->nilnode);
#endif
  assert_nilnode_unchanged(L, snap);
  return t;
}

static void check_first_string_key(lua_State *L, const NilNodeSnapshot *snap)
{
  GCtab *t;
  lua_settop(L, 0);
  t = new_empty_table(L, snap);
  lua_pushliteral(L, "alpha");
  lua_pushinteger(L, 42);
  lua_rawset(L, -3);
  assert(lj_tab_hmask_acq(t) != 0);
  assert(lj_tab_node_acq(t) != &G(L)->nilnode);
  assert_nilnode_unchanged(L, snap);
  lua_pushliteral(L, "alpha");
  lua_rawget(L, -2);
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 2);
}

static void check_first_integer_hash_key(lua_State *L,
					 const NilNodeSnapshot *snap)
{
  GCtab *t;
  lua_settop(L, 0);
  t = new_empty_table(L, snap);
  lua_pushinteger(L, 1000);
  lua_pushliteral(L, "sparse");
  lua_rawset(L, -3);
  assert(lj_tab_hmask_acq(t) != 0);
  assert(lj_tab_node_acq(t) != &G(L)->nilnode);
  assert_nilnode_unchanged(L, snap);
  lua_pushinteger(L, 1000);
  lua_rawget(L, -2);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "sparse") == 0);
  lua_pop(L, 2);
}

static void check_first_integer_array_key(lua_State *L,
					  const NilNodeSnapshot *snap)
{
  GCtab *t;
  lua_settop(L, 0);
  t = new_empty_table(L, snap);
  lua_pushinteger(L, 1);
  lua_pushliteral(L, "dense");
  lua_rawset(L, -3);
  assert(lj_tab_hmask_acq(t) == 0);
  assert_nilnode_unchanged(L, snap);
  lua_pushinteger(L, 1);
  lua_rawget(L, -2);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "dense") == 0);
  lua_pop(L, 2);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  NilNodeSnapshot snap;
  assert(L != NULL);
  luaL_openlibs(L);
  snap = nilnode_snapshot(L);
  check_first_string_key(L, &snap);
  check_first_integer_hash_key(L, &snap);
  check_first_integer_array_key(L, &snap);
  lua_close(L);
  printf("t-tab-emptyhash OK: nilnode remains read-only on first hash insert\n");
  return 0;
}
