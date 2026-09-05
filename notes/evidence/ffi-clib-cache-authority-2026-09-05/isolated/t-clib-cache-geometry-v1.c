/* Observe table metadata between owner-thread mutations. The worker never
** receives this namespace. The node address is compared, never dereferenced. */
#include <assert.h>
#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_clib.h"
static int geometry(lua_State *L)
{
  GCudata *ud;
  CLibrary *cl;
  GCtab *env;
  MSize hmask;
  Node *node;
  luaL_checktype(L, 1, LUA_TUSERDATA);
  ud = udataV(L->base);
  assert(lj_udata_udtype_acq(ud) == UDTYPE_FFI_CLIB);
  cl = (CLibrary *)uddata(ud);
  env = lj_clib_cache_env_acq(cl);
  assert(env);
  hmask = lj_tab_hmask_acq(env);
  node = lj_tab_node_acq(env);
  lua_pushinteger(L, hmask);
  lua_pushlightuserdata(L, node);
  return 2;
}
LUA_API int luaopen_clib_cache_geometry(lua_State *L)
{
  lua_pushcfunction(L, geometry);
  return 1;
}
