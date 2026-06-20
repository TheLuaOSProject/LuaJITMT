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

static void assert_nilnode_clean(lua_State *L)
{
  Node *nilnode = &G(L)->nilnode;
  assert(lj_tab_node_hmask_acq(nilnode) == 0);
  assert(tvisnil(&nilnode->key));
  assert(tvisnil(&nilnode->val));
  assert(lj_tab_nextnode_acq(nilnode) == NULL);
}

static GCtab *new_empty_table(lua_State *L)
{
  GCtab *t;
  lua_newtable(L);
  t = tabV(L->top-1);
  assert(t->hmask == 0);
  assert(noderef(t->node) == &G(L)->nilnode);
  assert(lj_tab_node_hmask_acq(lj_tab_node_acq(t)) == 0);
#if LJ_GC64
  assert(getfreetop(t, noderef(t->node)) == &G(L)->nilnode);
#endif
  assert_nilnode_clean(L);
  return t;
}

static void check_first_string_key(lua_State *L)
{
  GCtab *t;
  lua_settop(L, 0);
  t = new_empty_table(L);
  lua_pushliteral(L, "alpha");
  lua_pushinteger(L, 42);
  lua_rawset(L, -3);
  assert(t->hmask != 0);
  assert(noderef(t->node) != &G(L)->nilnode);
  assert_nilnode_clean(L);
  lua_pushliteral(L, "alpha");
  lua_rawget(L, -2);
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 2);
}

static void check_first_integer_hash_key(lua_State *L)
{
  GCtab *t;
  lua_settop(L, 0);
  t = new_empty_table(L);
  lua_pushinteger(L, 1000);
  lua_pushliteral(L, "sparse");
  lua_rawset(L, -3);
  assert(t->hmask != 0);
  assert(noderef(t->node) != &G(L)->nilnode);
  assert_nilnode_clean(L);
  lua_pushinteger(L, 1000);
  lua_rawget(L, -2);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "sparse") == 0);
  lua_pop(L, 2);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  check_first_string_key(L);
  check_first_integer_hash_key(L);
  lua_close(L);
  printf("t-tab-emptyhash OK: nilnode remains read-only on first hash insert\n");
  return 0;
}
