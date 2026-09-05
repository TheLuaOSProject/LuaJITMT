/* Close at the real native lookup's return boundary. No runtime gates are
** modified. The fixture's Lua locals and trace KGCs retain all selected inputs. */
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_clib.h"
#include "lj_tab.h"
#include "lj_tg.h"
static CLibrary *selected;
static GCtab *selected_env;
static GCstr *selected_key;
static global_State *selected_g;
static uint32_t armed;
static lua_State *selected_L;
static int hits, hit_status, native_witness;
extern int __real_lj_tab_gettv_rooted_hit_try(lua_State *L, cTValue *tabroot,
                                             cTValue *keyroot, TValue *outroot);
int __wrap_lj_tab_gettv_rooted_hit_try(lua_State *L, cTValue *tabroot,
                                      cTValue *keyroot, TValue *outroot)
{
  /* The successful aliased output replaces tabroot: match before calling. */
  int match = la_load32_acq(&armed) && L == selected_L && G(L) == selected_g && tvistab(tabroot) &&
    tabV(tabroot) == selected_env && tvisstr(keyroot) &&
    strV(keyroot) == selected_key && lj_tg_load_jit_base(L2TG(L)) != NULL;
  int status = __real_lj_tab_gettv_rooted_hit_try(L, tabroot, keyroot, outroot);
  if (match && status) {
    la_store32_rel(&armed, 0);
    hits++;
    hit_status = status;
    native_witness = lj_tg_load_jit_base(L2TG(L)) != NULL;
    assert(native_witness);
    lj_clib_unload(L, G(L), selected);
    assert(lj_clib_lifecycle_acq(selected) & LJ_CLIB_CLOSING);
  }
  return status;
}
static int arm_close(lua_State *L)
{
  assert(!la_load32_acq(&armed) && hits == 0);
  assert(tvisudata(L->base) && tvisstr(L->base+1));
  assert(lj_udata_udtype_acq(udataV(L->base)) == UDTYPE_FFI_CLIB);
  selected = (CLibrary *)uddata(udataV(L->base));
  selected_env = lj_clib_cache_env_acq(selected);
  selected_key = strV(L->base+1);
  selected_g = G(L);
  assert(selected_env != NULL && !(lj_clib_lifecycle_acq(selected) & LJ_CLIB_CLOSING));
  selected_L = L;
  la_store32_rel(&armed, 1);
  return 0;
}
static int witness(lua_State *L)
{
  lua_pushinteger(L, hits); lua_pushinteger(L, hit_status);
  lua_pushinteger(L, native_witness); return 3;
}
int main(int argc, char **argv)
{
  lua_State *L;
  int i, status;
  assert(argc == 6 || argc == 7);
  alarm(25);
  L = luaL_newstate(); assert(L); luaL_openlibs(L);
  lua_pushcfunction(L, arm_close); lua_setglobal(L, "arm_clib_close");
  lua_pushcfunction(L, witness); lua_setglobal(L, "close_witness");
  lua_newtable(L);
  for (i=1; i<argc; i++) {lua_pushstring(L, argv[i]);lua_rawseti(L,-2,i-1);}
  lua_pushliteral(L, "helper");lua_rawseti(L,-2,5);
  if (argc == 7) {lua_pushstring(L,argv[6]);lua_rawseti(L,-2,6);}
  lua_setglobal(L,"arg");
  status=luaL_loadfile(L,argv[1]);if (!status) status=lua_pcall(L,0,0,0);
  if (status) fprintf(stderr,"%s\n",lua_tostring(L,-1));
  assert(status==0);
  assert(hits==1 && hit_status==1 && native_witness==1 && !la_load32_acq(&armed));
  lua_close(L);
  return 0;
}
