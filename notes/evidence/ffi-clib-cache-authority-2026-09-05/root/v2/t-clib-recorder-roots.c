/* Speculative namespace lookup must unwind owner anchors after refusal/OOM. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_dispatch.h"
#include "lj_tg.h"
#include "lj_tab.h"
#include "lj_err.h"
#include "lib/lua_fixture_helpers.h"

static GCtab *cache;
static uint32_t anchorbase;
static int armed, stage, failstage, injected, refused, successful;

extern TValue *__real_lj_tg_root_anchor_push(lua_State *, TGState *,
                                            cTValue *, uint32_t *);
extern int __real_lj_tab_gettv_rooted_hit_try(lua_State *, cTValue *,
                                            cTValue *, TValue *);

static int recording(lua_State *L)
{
  jit_State *J = G2J(G(L));
  return J && J->L == L &&
    (J->state == LJ_TRACE_RECORD || J->state == LJ_TRACE_RECORD_1ST);
}

TValue *__wrap_lj_tg_root_anchor_push(lua_State *L, TGState *tg,
                                     cTValue *tv, uint32_t *idx)
{
  if (armed && recording(L) &&
      (stage || (tvistab(tv) && tabV(tv) == cache))) {
    stage++;
    assert(tg == L2TG(L));
    assert(lj_tg_root_anchor_top_acq(tg) == anchorbase + stage - 1u);
    if (stage == failstage) {
      armed = 0;
      injected++;
      lj_err_mem(L);
    }
  }
  return __real_lj_tg_root_anchor_push(L, tg, tv, idx);
}

int __wrap_lj_tab_gettv_rooted_hit_try(lua_State *L, cTValue *parent,
                                     cTValue *key, TValue *out)
{
  if (armed && recording(L) && tvistab(parent) && tabV(parent) == cache) {
    TGState *tg = L2TG(L);
    uint64_t scratch1 = tg->tmptv.u64, scratch2 = tg->tmptv2.u64;
    int result;
    assert(stage == 3 && out == parent);
    assert(lj_tg_root_anchor_top_acq(tg) == anchorbase + 3);
    assert(parent == lj_tg_root_anchor_slot_acq(tg, anchorbase));
    assert(key == lj_tg_root_anchor_slot_acq(tg, anchorbase + 1));
    armed = 0;
    if (failstage == 0) {
      /* Contract-preserving refusal: all source/output cells stay unchanged. */
      refused++;
      return 0;
    }
    result = __real_lj_tab_gettv_rooted_hit_try(L, parent, key, out);
    assert(result == 1);
    assert(tg->tmptv.u64 == scratch1 && tg->tmptv2.u64 == scratch2);
    successful++;
    return result;
  }
  return __real_lj_tab_gettv_rooted_hit_try(L, parent, key, out);
}

int main(int argc, char **argv)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  TGState *tg = L2TG(L);
  jit_State *J = G2J(G(L));
  int status;
  assert(argc == 2);
  if (!strcmp(argv[1], "refuse")) failstage = 0;
  else if (!strcmp(argv[1], "hit")) failstage = 4;
  else {
    failstage = atoi(argv[1]);
    assert(failstage >= 1 && failstage <= 3);
  }
  ljt_lua_dostring(L,
    "local ffi=require('ffi');ffi.cdef('int abs(int);');lib=ffi.C;"
    "assert(lib.abs);cache=debug.getfenv(lib);index=debug.getmetatable(lib).__index;"
    "effects={0};function run(fn,p,n) local sum=0;for i=0,n do if i>0 then "
    "effects[1]=effects[1]+1;if fn(p,'abs') then sum=sum+1 end end end;return sum end;"
    "collectgarbage('collect');collectgarbage('collect');jit.flush();"
    "jit.opt.start('hotloop=1','hotexit=1000')");
  lua_getglobal(L, "cache");
  assert(lua_istable(L, -1));
  cache = tabV(L->top-1);  /* Retained by the ordinary global cache root. */
  lua_pop(L, 1);
  anchorbase = lj_tg_root_anchor_top_acq(tg);
  ljt_lua_loadstring(L, "return run(index,lib,80)");
  armed = 1;
  status = lua_pcall(L, 0, 1, 0);
  armed = 0;
  if (failstage >= 1 && failstage <= 3) {
    assert(injected == 1 && status == LUA_ERRMEM);
  } else {
    assert(status == 0 && lua_tointeger(L, -1) == 80);
    assert(failstage == 0 ? refused == 1 : successful == 1);
    lua_getglobal(L, "effects");
    lua_rawgeti(L, -1, 1);
    assert(lua_tointeger(L, -1) == 80);
  }
  assert(lj_tg_root_anchor_top_acq(tg) == anchorbase);
  assert(J->state == LJ_TRACE_IDLE && J->loop_cdata_fload == 0);
  printf("clib-recorder-roots mode=%s stage=%d status=%d injected=%d refused=%d hit=%d anchors=%u\n",
         argv[1], stage, status, injected, refused, successful,
         (unsigned)lj_tg_root_anchor_top_acq(tg));
  lua_settop(L, 0);
  ljt_lua_dostring(L,
    "jit.flush();effects[1]=0;local util=require('jit.util');local exits=0;"
    "local function onexit() exits=exits+1 end;jit.off(onexit);"
    "jit.attach(onexit,'texit');assert(run(index,lib,80)==80);"
    "assert(util.traceinfo(1) and exits>0 and effects[1]==80);jit.attach(onexit)");
  assert(lj_tg_root_anchor_top_acq(tg) == anchorbase);
  lua_close(L);
  return 0;
}
