/*
** Legacy open captures retain owner semantics and veto secondary admission.
** Captures made during/after activation use the existing snapshot policy.
*/

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_func.h"
#include "lj_safepoint.h"
#include "lj_state.h"
#include "lj_thr.h"
#include "lj_tg.h"

#include "lib/test_sleep.h"

#if LJ_GC64
static const char legacy_probe_src[] =
  "return function()\n"
  "  local x = 0\n"
  "  return function() return x end\n"
  "end\n";

static GCproto *first_child_proto(GCproto *pt)
{
  MSize i;
  assert((pt->flags & PROTO_CHILD) != 0);
  for (i = 0; i < pt->sizekgc; i++) {
    GCobj *o = proto_kgc(pt, ~(ptrdiff_t)i);
    if (o->gch.gct == ~LJ_TPROTO)
      return gco2pt(o);
  }
  assert(0 && "missing child proto");
  return NULL;
}

static GCfunc *top_lfunc(lua_State *L)
{
  GCfunc *fn;
  assert(tvisfunc(L->top - 1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  return fn;
}

static GCproto *load_parent(lua_State *L)
{
  GCproto *child;
  assert(luaL_loadstring(L, legacy_probe_src) == LUA_OK);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  child = first_child_proto(funcproto(top_lfunc(L)));
  child->flags2 &= ~(uint32_t)PROTO2_CELLUV;
  proto_setlegacyuv(child);
  assert(proto_legacyuv(child));
  assert(!proto_celluv(child));
  return child;
}

static void assert_uv_int(GCupval *uv, int32_t expect)
{
  cTValue *tv = uvval(uv);
  assert(tvisnumber(tv));
  assert((int32_t)numberVnum(tv) == expect);
}

static GCupval *new_legacy_capture(lua_State *L, GCproto *pt, TValue *base)
{
  GCfunc *parent = top_lfunc(L);
  GCfunc *fn = lj_func_newL_gc_forjit(L, base, pt, &parent->l);
  assert(lj_funcL_nupvalues(&fn->l) == 1);
  return func_uv_acq(&fn->l, 0);
}

typedef struct AttachCtx {
  lua_State *L;
  int attached;
} AttachCtx;

static void *attach_worker(void *arg)
{
  AttachCtx *ctx = (AttachCtx *)arg;
  ctx->attached = lj_threading_attach(ctx->L);
  if (ctx->attached)
    lj_threading_detach(ctx->L, 1);
  return NULL;
}

static void check_attach(lua_State *L, lua_State *co, int expected)
{
  AttachCtx ctx;
  pthread_t worker;
  ctx.L = co;
  ctx.attached = -1;
  lj_native_enter(L2TG(L));
  assert(pthread_create(&worker, NULL, attach_worker, &ctx) == 0);
  assert(pthread_join(worker, NULL) == 0);
  (void)lj_native_leave(L);
  assert(ctx.attached == expected);
  assert(mt_live_acq(G(L)) == 0 && mt_entering_acq(G(L)) == 0);
}

static void check_activation(lua_State *L)
{
  global_State *g = G(L);
  GCproto *child = load_parent(L);
  GCfunc *parent = top_lfunc(L);
  GCfunc *fn;
  GCupval *uv;
  lua_State *co;
  TValue *base, *slot;
  ptrdiff_t slotofs;
  uint32_t desc = proto_uv(child)[0];
  int closure_ref;

  /* Use actual owner-stack storage so close and GC see the real lifetime. */
  assert(lua_checkstack(L, 260));
  base = L->top;
  slot = base + (desc & 0xffu);
  slotofs = savestack(L, slot);
  while (L->top < base + 256)
    setnilV(L->top++);
  setintV(slot, 41);
  fn = lj_func_newL_gc_forjit(L, base, child, &parent->l);
  uv = func_uv_acq(&fn->l, 0);
  assert(!uv->closed && uvval(uv) == slot);
  assert(la_load32_acq(&g->mt_openuv) == 2);
  setfuncV(L, L->top++, fn);
  closure_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  fn = lj_func_newL_gc_forjit(L, base, child, &parent->l);
  assert(func_uv_acq(&fn->l, 0) == uv);
  assert(la_load32_acq(&g->mt_openuv) == 2);  /* Count cells, not closures. */

  /* Even an unrelated entry function must not expose a hidden open capture
  ** through a table, environment, or later channel message. */
  assert(luaL_dostring(L,
    "local ok, err = pcall(require('threading').spawn, function() end)\n"
    "assert(not ok and err:find('open legacy upvalues', 1, true))\n") == 0);
  assert(mt_active_acq(g) == 0);
  assert(mt_live_acq(g) == 0 && mt_entering_acq(g) == 0);
  slot = restorestack(L, slotofs);
  assert(!uv->closed && uvval(uv) == slot);
  setintV(slot, 42);
  assert_uv_int(uv, 42);

  co = lua_newthread(L);
  check_attach(L, co, 0);
  slot = restorestack(L, slotofs);
  assert(mt_active_acq(g) == 0);
  assert(!uv->closed && uvval(uv) == slot);
  setintV(slot, 43);
  assert_uv_int(uv, 43);

  /* Closing preserves the same cell and value, then permits foreign attach
  ** and ordinary spawn. The shared closure survives a remote collection. */
  lj_func_closeuv(L, slot);
  assert(uv->closed && uvval(uv) == &uv->tv);
  assert(la_load32_acq(&g->mt_openuv) == 0);
  assert_uv_int(uv, 43);
  check_attach(L, co, 1);
  assert(mt_active_acq(g) == 1);
  assert(la_load32_acq(&g->mt_openuv) == 1);
  lua_rawgeti(L, LUA_REGISTRYINDEX, closure_ref);
  lua_setglobal(L, "legacy_capture");
  assert(luaL_dostring(L,
    "local t = require('threading').spawn(function()\n"
    "  collectgarbage('collect')\n"
    "  return legacy_capture()\n"
    "end)\n"
    "local ok, value = t:join()\n"
    "assert(ok and value == 43)\n") == 0);
  luaL_unref(L, LUA_REGISTRYINDEX, closure_ref);
}

typedef struct AdmissionRace {
  global_State *g;
  uint32_t start, done;
  int activated;
} AdmissionRace;

#define RACE_ROUNDS 2000u

static void *admission_worker(void *arg)
{
  AdmissionRace *race = (AdmissionRace *)arg;
  uint32_t i;
  for (i = 1; i <= RACE_ROUNDS; i++) {
    while (la_load32_acq(&race->start) != i)
      sleep_ns(1000);
    race->activated = mt_openuv_activate(race->g);
    la_store32_rel(&race->done, i);
  }
  return NULL;
}

static void check_admission_race(void)
{
  AdmissionRace race;
  pthread_t worker;
  uint32_t i;
  memset(&race, 0, sizeof(race));
  race.g = (global_State *)calloc(1, sizeof(global_State));
  assert(race.g != NULL);
  assert(pthread_create(&worker, NULL, admission_worker, &race) == 0);
  for (i = 1; i <= RACE_ROUNDS; i++) {
    int reserved;
    la_store32_rel(&race.g->mt_openuv, 0);  /* Independent synthetic universe. */
    la_store32_rel(&race.start, i);
    reserved = mt_openuv_reserve(race.g);
    while (la_load32_acq(&race.done) != i)
      sleep_ns(1000);
    assert(reserved != race.activated);
    if (reserved) {
      assert(!mt_openuv_activate(race.g));
      mt_openuv_release(race.g);
      assert(mt_openuv_activate(race.g));
    }
    assert(!mt_openuv_reserve(race.g));
  }
  assert(pthread_join(worker, NULL) == 0);
  la_store32_rel(&race.g->mt_openuv, 0xfffffffeu);
  assert(mt_openuv_reserve(race.g) == -1);
  assert(!mt_openuv_activate(race.g));
  assert(la_load32_acq(&race.g->mt_openuv) == 0xfffffffeu);
  free(race.g);
}
#endif

int main(void)
{
#if LJ_GC64
  lua_State *L = luaL_newstate();
  global_State *g;
  GCproto *child;
  TValue slots[256];
  TValue *slot;
  GCupval *uv;
  uint32_t uvdesc;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  assert(mt_active_acq(g) == 0);
  assert(mt_entering_acq(g) == 0);

  child = load_parent(L);
  assert(child->sizeuv == 1);
  uvdesc = proto_uv(child)[0];
  assert((uvdesc & PROTO_UV_LOCAL) != 0);
  slot = &slots[uvdesc & 0xffu];

  setintV(slot, 11);
  uv = new_legacy_capture(L, child, slots);
  assert(!uv->closed);
  assert(uvval(uv) == slot);
  setintV(slot, 22);
  assert_uv_int(uv, 22);
  lj_func_closeuv(L, slot);

  setintV(slot, 11);
  assert(mt_entering_add_rlx(g, 1) == 0);
  uv = new_legacy_capture(L, child, slots);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, INT_MAX);
  assert(uv->closed);
  assert(uvval(uv) == &uv->tv);
  assert_uv_int(uv, 11);
  setintV(slot, 22);
  assert_uv_int(uv, 11);

  check_activation(L);
  lua_close(L);
  check_admission_race();
#endif
  return 0;
}
