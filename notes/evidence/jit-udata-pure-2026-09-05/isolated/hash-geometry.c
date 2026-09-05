/* Owner-only diagnostic between warmup and timing. No mutation/allocations
** occur until all raw geometry has been converted to scalars. */
#include <stdint.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
static int measure(lua_State *L)
{
  GCudata *ud;
  GCtab *mt, *index = NULL;
  GCstr *key;
  Node *n, *node;
  MSize mask, i;
  unsigned depth = 0, slot = 0, bucket;
  uintptr_t node_address = 0, table_address = 0;
  TValue k, v;
  if (!tvisudata(L->base) || !tvisstr(L->base+1))
    return luaL_error(L, "userdata and string required");
  ud = udataV(L->base); key = strV(L->base+1);
  mt = lj_udata_metatable_acq(ud);
  if (mt == NULL) return luaL_error(L, "metatable required");
  node = lj_tab_node_acq(mt); mask = lj_tab_node_hmask_acq(node);
  for (i=0; i<=mask; i++) {
    lj_tv_load_acq(&k, &node[i].key);
    if (tvisstr(&k) && strV(&k)->len == 7 &&
        !memcmp(strdata(strV(&k)), "__index", 7)) {
      lj_tv_load_acq(&v, &node[i].val);
      if (tvistab(&v)) index = tabV(&v);
      break;
    }
  }
  if (index == NULL) return luaL_error(L, "table-valued index required");
  node = lj_tab_node_acq(index); mask = lj_tab_node_hmask_acq(node);
  bucket = key->sid & mask;
  n = &node[bucket];
  while (n && depth <= mask+1) {
    depth++;
    lj_tv_load_acq(&k, &n->key);
    if (tvisstr(&k) && strV(&k) == key) {slot=(unsigned)(n-node);break;}
    n = nextnode(n);
  }
  if (!n || depth > mask+1) return luaL_error(L, "key not found in index chain");
  node_address = (uintptr_t)node; table_address = (uintptr_t)index;
  lua_pushinteger(L, depth); lua_pushinteger(L, bucket);
  lua_pushinteger(L, slot); lua_pushinteger(L, mask);
  lua_pushnumber(L, (lua_Number)node_address);
  lua_pushnumber(L, (lua_Number)table_address);
  return 6;
}
LUA_API int luaopen_hash_geometry(lua_State *L)
{
  lua_pushcfunction(L, measure);
  return 1;
}
