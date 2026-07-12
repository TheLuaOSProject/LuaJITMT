/*
** Focused test for per-TG pending GC root publication.
*/

#include <assert.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_func.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_udata.h"

static int root_contains(global_State *g, GCobj *needle)
{
  GCobj *o;
  uint32_t n = 0;
  for (o = lj_gc_root_acq(g); o != NULL; o = lj_obj_gcw_acq(o)) {
    if (o == needle)
      return 1;
    assert(++n < LJ_GC2_ROOT_SCAN_LIMIT);
  }
  return 0;
}

static int pending_contains(TGState *tg, GCobj *needle)
{
  GCobj *o;
  uint32_t n = 0;
  for (o = lj_tg_gcroot_pending_acq(tg); o != NULL; o = lj_obj_gcw_acq(o)) {
    if (o == needle)
      return 1;
    assert(++n < LJ_GC2_ROOT_SCAN_LIMIT);
  }
  return 0;
}

static int pending_after_main_contains(TGState *tg, GCobj *needle)
{
  GCobj *o;
  uint32_t n = 0;
  for (o = lj_tg_gcroot_pending_after_main_acq(tg);
       o != NULL;
       o = lj_obj_gcw_acq(o)) {
    if (o == needle)
      return 1;
    assert(++n < LJ_GC2_ROOT_SCAN_LIMIT);
  }
  return 0;
}

static int after_main_contains(global_State *g, GCobj *needle)
{
  GCobj *main = obj2gco(mainthread_acq(g));
  GCobj *o;
  uint32_t n = 0;
  for (o = lj_obj_gcw_acq(main); o != NULL; o = lj_obj_gcw_acq(o)) {
    if (o == needle)
      return 1;
    assert(++n < LJ_GC2_ROOT_SCAN_LIMIT);
  }
  return 0;
}

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

static GCproto *load_legacy_child_proto(lua_State *L)
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

static GCupval *new_legacy_capture(lua_State *L, GCproto *pt, TValue *base)
{
  GCfunc *parent = top_lfunc(L);
  GCfunc *fn = lj_func_newL_gc_forjit(L, base, pt, &parent->l);
  assert(lj_funcL_nupvalues(&fn->l) == 1);
  return func_uv_acq(&fn->l, 0);
}

static void test_explicit_flush(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GC2StatsSnapshot before, after;
  GCtab *t, *t2;
  uint32_t flushed;
  assert(tg != NULL);
  (void)lj_gc_flush_root_pending(g);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(lj_gc_flush_root_pending(g) == 0);

  t = lj_tab_new(L, 0, 0);
  t2 = lj_tab_new(L, 0, 0);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(pending_contains(tg, obj2gco(t)));
  assert(pending_contains(tg, obj2gco(t2)));
  assert(!root_contains(g, obj2gco(t)));
  assert(!root_contains(g, obj2gco(t2)));

  lj_gcroot_pending_hint_rel(g, 0);
  lj_gc2_stats_snapshot(g, &before);
  flushed = lj_gc_flush_root_pending(g);
  assert(flushed >= 2u);
  lj_gc2_stats_snapshot(g, &after);
  assert(after.pending_root_flushes >= before.pending_root_flushes + 1u);
  assert(after.pending_root_flushed >= before.pending_root_flushed + flushed);
  assert(after.pending_root_flush_max >= flushed);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_contains(g, obj2gco(t)));
  assert(root_contains(g, obj2gco(t2)));
}

static void test_hint_only_on_empty_transition(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCtab *t, *t2;
  assert(tg != NULL);
  (void)lj_gc_flush_root_pending(g);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_gcroot_pending_hint_acq(g) == 0);

  t = lj_tab_new(L, 0, 0);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(pending_contains(tg, obj2gco(t)));

  /*
  ** A flusher can clear the global hint while a same-TG pending stack remains
  ** observable through the direct main/self checks. Appending to that non-empty
  ** stack must not republish the global hint on the allocation fast path.
  */
  lj_gcroot_pending_hint_rel(g, 0);
  t2 = lj_tab_new(L, 0, 0);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(pending_contains(tg, obj2gco(t)));
  assert(pending_contains(tg, obj2gco(t2)));

  assert(lj_gc_flush_root_pending(g) >= 2u);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_contains(g, obj2gco(t)));
  assert(root_contains(g, obj2gco(t2)));
}

static void test_pending_cycle_breaker(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCtab *t, *t2;
  assert(tg != NULL);
  (void)lj_gc_flush_root_pending(g);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_gcroot_pending_hint_acq(g) == 0);

  t = lj_tab_new(L, 0, 0);
  t2 = lj_tab_new(L, 0, 0);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(t2));
  assert(lj_obj_gcw_acq(obj2gco(t2)) == obj2gco(t));

  /*
  ** A malformed publisher-owned pending chain must not hang the collector. The
  ** flusher preserves the unique objects ahead of the cycle and splices a finite
  ** chain into the root spine.
  */
  lj_obj_setgcwrel(obj2gco(t), obj2gco(t2));
  assert(lj_gc_flush_root_pending(g) == 2u);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_contains(g, obj2gco(t)));
  assert(root_contains(g, obj2gco(t2)));
  assert(lj_obj_gcw_acq(obj2gco(t)) != obj2gco(t2));
}

static void test_after_main_flush(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  lua_State *L1, *L2;
  GCudata *ud;
  assert(tg != NULL);
  (void)lj_gc_flush_root_pending(g);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);

  L1 = lua_newthread(L);
  L2 = lua_newthread(L);
  assert(L1 != NULL && L2 != NULL);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(pending_after_main_contains(tg, obj2gco(L1)));
  assert(pending_after_main_contains(tg, obj2gco(L2)));
  assert(!after_main_contains(g, obj2gco(L1)));
  assert(!after_main_contains(g, obj2gco(L2)));

  lua_newuserdata(L, 16);
  ud = udataV(L->top - 1);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(pending_after_main_contains(tg, obj2gco(ud)));
  assert(!after_main_contains(g, obj2gco(ud)));

  assert(lj_gc_flush_root_pending(g) >= 3u);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(after_main_contains(g, obj2gco(L1)));
  assert(after_main_contains(g, obj2gco(L2)));
  assert(after_main_contains(g, obj2gco(ud)));
  lua_pop(L, 3);
}

static void test_after_main_cycle_breaker(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  lua_State *L1, *L2;
  assert(tg != NULL);
  (void)lj_gc_flush_root_pending(g);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);
  assert(lj_gcroot_pending_hint_acq(g) == 0);

  L1 = lua_newthread(L);
  L2 = lua_newthread(L);
  assert(L1 != NULL && L2 != NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == obj2gco(L2));
  assert(lj_obj_gcw_acq(obj2gco(L2)) == obj2gco(L1));
  assert(!after_main_contains(g, obj2gco(L1)));
  assert(!after_main_contains(g, obj2gco(L2)));

  /*
  ** Child states and userdata publish after mainthread to preserve LuaJIT's
  ** object-list layout. This queue has a different insertion anchor
  ** from the regular pending stack, so keep explicit malformed-cycle coverage.
  */
  lj_obj_setgcwrel(obj2gco(L1), obj2gco(L2));
  assert(lj_gc_flush_root_pending(g) == 2u);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(after_main_contains(g, obj2gco(L1)));
  assert(after_main_contains(g, obj2gco(L2)));
  assert(lj_obj_gcw_acq(obj2gco(L1)) != obj2gco(L2));
  lua_pop(L, 2);
}

static void test_fullgc_flush(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCtab *t;
  assert(tg != NULL);
  (void)lj_gc_flush_root_pending(g);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);

  lua_newtable(L);
  t = tabV(L->top - 1);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(pending_contains(tg, obj2gco(t)));
  assert(!root_contains(g, obj2gco(t)));

  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_contains(g, obj2gco(t)));

  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
}

static void test_tls_only_tg_flush(lua_State *L)
{
  global_State *g = G(L);
  TGState extra, *oldtg = lj_thr_get_tg();
  TGState *oldhint = L->tg_hint;
  GCtab *t;
  GCudata *ud;
  assert(oldtg != NULL);
  (void)lj_gc_flush_root_pending(g);

  lj_tg_init_thread(g, &extra, NULL, 0);
  lj_tg_tid_rel(&extra, lj_thr_newid());
  lj_thr_set_tg(&extra);
  L->tg_hint = &extra;

  t = lj_tab_new(L, 0, 0);
  ud = lj_udata_new(L, 16, NULL);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(pending_contains(&extra, obj2gco(t)));
  assert(pending_after_main_contains(&extra, obj2gco(ud)));
  assert(!root_contains(g, obj2gco(t)));
  assert(!after_main_contains(g, obj2gco(ud)));

  lj_gcroot_pending_hint_rel(g, 0);
  assert(lj_gc_flush_root_pending(g) >= 2u);
  assert(lj_tg_gcroot_pending_acq(&extra) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(&extra) == NULL);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_contains(g, obj2gco(t)));
  assert(after_main_contains(g, obj2gco(ud)));

  L->tg_hint = oldhint;
  lj_thr_set_tg(oldtg);
  lj_tg_fini_thread(g, &extra);
}

static void test_attach_flushes_pending(lua_State *L)
{
  global_State *g = G(L);
  TGState extra, *oldtg = lj_thr_get_tg();
  TGState *oldhint = L->tg_hint;
  GCtab *t;
  GCudata *ud;
  assert(oldtg != NULL);
  (void)lj_gc_flush_root_pending(g);

  lj_tg_init_thread(g, &extra, NULL, 0);
  lj_tg_tid_rel(&extra, lj_thr_newid());
  lj_thr_set_tg(&extra);
  L->tg_hint = &extra;

  t = lj_tab_new(L, 0, 0);
  ud = lj_udata_new(L, 16, NULL);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(pending_contains(&extra, obj2gco(t)));
  assert(pending_after_main_contains(&extra, obj2gco(ud)));
  assert(!root_contains(g, obj2gco(t)));
  assert(!after_main_contains(g, obj2gco(ud)));

  L->tg_hint = oldhint;
  lj_thr_set_tg(oldtg);

  lj_tg_attach(g, &extra);
  assert(lj_tg_gcroot_pending_acq(&extra) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(&extra) == NULL);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_contains(g, obj2gco(t)));
  assert(after_main_contains(g, obj2gco(ud)));

  lj_tg_detach(g, &extra);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra);
}

static void test_closed_upvalue_relink_pending(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCproto *child;
  TValue slots[256];
  TValue *slot;
  GCupval *uv;
  GCupval *same;
  uint32_t uvdesc;
  uint8_t deadwhite;
  assert(tg != NULL);
  (void)lj_gc_flush_root_pending(g);

  child = load_legacy_child_proto(L);
  (void)lj_gc_flush_root_pending(g);
  child->flags2 &= ~(uint32_t)PROTO2_CELLUV;
  proto_setlegacyuv(child);
  uvdesc = proto_uv(child)[0];
  assert((uvdesc & PROTO_UV_LOCAL) != 0);
  slot = &slots[uvdesc & 0xffu];

  setintV(slot, 17);
  uv = new_legacy_capture(L, child, slots);
  assert(!uv->closed);
  assert(uvval(uv) == slot);
  assert(!pending_contains(tg, obj2gco(uv)));
  assert(!root_contains(g, obj2gco(uv)));
  (void)lj_gc_flush_root_pending(g);

  /* Legacy lookup recolored this cell and legacy close used the same header
  ** color as a liveness verdict. GC2 owns both decisions, so an adversarial
  ** stale other-white value must be observationally irrelevant. */
  deadwhite = (uint8_t)(otherwhite(g) & LJ_GC_WHITES);
  lj_obj_masksetgcflags(obj2gco(uv), LJ_GC_COLORS, deadwhite);
  assert(isdead(g, obj2gco(uv)));
  same = new_legacy_capture(L, child, slots);
  assert(same == uv);
  assert((lj_obj_gcflags(obj2gco(uv)) & LJ_GC_COLORS) == deadwhite);

  lj_func_closeuv(L, slot);
  assert(uv->closed);
  assert(uvval(uv) == &uv->tv);
  assert((lj_obj_gcflags(obj2gco(uv)) & LJ_GC_COLORS) == deadwhite);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(pending_contains(tg, obj2gco(uv)));
  assert(!root_contains(g, obj2gco(uv)));

  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_contains(g, obj2gco(uv)));
  lua_settop(L, 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  test_explicit_flush(L);
  test_hint_only_on_empty_transition(L);
  test_pending_cycle_breaker(L);
  test_after_main_flush(L);
  test_after_main_cycle_breaker(L);
  test_fullgc_flush(L);
  test_tls_only_tg_flush(L);
  test_attach_flushes_pending(L);
  test_closed_upvalue_relink_pending(L);
  lua_close(L);
  return 0;
}
