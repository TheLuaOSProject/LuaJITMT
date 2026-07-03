/*
** Focused regression fixture for legacy-loaded local upvalue capture while
** mt_entering is nonzero.
*/

#include <assert.h>
#include <limits.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_func.h"

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
  assert(fn->l.nupvalues == 1);
  return func_uv_acq(&fn->l, 0);
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

  lua_close(L);
#endif
  return 0;
}
