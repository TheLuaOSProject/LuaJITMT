/*
** Focused test for GC2 SSB-to-grey traversal.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_tg.h"

static void flush_and_drain(global_State *g, TGState *tg)
{
  (void)lj_gc2_flush_ssb(g, tg);
  (void)lj_gc2_drain_ssb(g);
  assert(lj_gc2_ssb_empty(g));
}

static void test_strong_table(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child;
  uint64_t grey_pushed0, grey_drained0;

  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "child");

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  grey_pushed0 = g->gc2.grey_pushed;
  grey_drained0 = g->gc2.grey_drained;
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(parent->asize > 0);
  assert(parent->hmask > 0);
  assert(lj_gc2_ismarkedmem(g, tvref(parent->array)) == 1);
  assert(lj_gc2_ismarkedmem(g, noderef(parent->node)) == 1);
  assert(g->gc2.grey_pushed == grey_pushed0 + 2u);
  assert(g->gc2.grey_drained == grey_drained0 + 2u);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
}

static void test_c_value_barrier(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *child;

  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
}

static void test_c_table_rescan_barrier(lua_State *L, global_State *g,
					TGState *tg)
{
  GCtab *parent, *child;

  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  assert(parent->asize > 0);
  settabV(L, arrayslot(parent, 0), child);
  lj_gc_anybarriert(L, parent);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
}

static void test_vm_upvalue_barrier(lua_State *L, global_State *g, TGState *tg)
{
  GCfunc *fn;
  GCupval *uv;
  GCtab *old, *child;

  assert(luaL_dostring(L,
    "local x = {}\n"
    "return function(v) x = v end, x\n") == LUA_OK);
  fn = funcV(L->top - 2);
  old = tabV(L->top - 1);
  assert(isluafunc(fn));
  assert(fn->l.nupvalues == 1);
  uv = gco2uv(gcref(fn->l.uvptr[0]));
  assert(uv->closed);
  assert(uvval(uv) == &uv->tv);
  assert(tabV(uvval(uv)) == old);

  lua_newtable(L);
  child = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(uv)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(uv)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(old)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lua_pushvalue(L, -3);
  lua_pushvalue(L, -2);
  lua_call(L, 1, 0);
  assert(tabV(uvval(uv)) == child);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  flush_and_drain(g, tg);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 3);
}

static void make_weak_table(lua_State *L, const char *mode,
			    GCtab **weak, GCtab **key, GCtab **val)
{
  lua_newtable(L);
  *weak = tabV(L->top - 1);
  lua_newtable(L);
  *key = tabV(L->top - 1);
  lua_newtable(L);
  *val = tabV(L->top - 1);
  lua_pushvalue(L, -2);
  lua_pushvalue(L, -2);
  lua_settable(L, -5);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushstring(L, mode);
  lua_settable(L, -3);
  lua_setmetatable(L, -4);
}

static void test_weak_tables(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *weakv, *keyv, *valv;
  GCtab *weakk, *keyk, *valk;
  GCtab *weakkv, *keykv, *valkv;

  make_weak_table(L, "v", &weakv, &keyv, &valv);
  make_weak_table(L, "k", &weakk, &keyk, &valk);
  make_weak_table(L, "kv", &weakkv, &keykv, &valkv);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weakv)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(weakk)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(weakkv)) == 1);
  flush_and_drain(g, tg);

  assert(lj_gc2_ismarked(g, obj2gco(keyv)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(valv)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(keyk)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(valk)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(keykv)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(valkv)) == 0);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 9);
}

static void test_closure(lua_State *L, global_State *g, TGState *tg)
{
  GCfunc *fn;
  GCtab *up;

  assert(luaL_dostring(L,
    "local x = {}\n"
    "return function() return x end, x\n") == LUA_OK);
  fn = funcV(L->top - 2);
  up = tabV(L->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(up)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(fn)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(funcproto(fn))) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(up)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
}

static void test_thread(lua_State *L, global_State *g, TGState *tg)
{
  lua_State *th;
  GCtab *stack_tab;

  th = lua_newthread(L);
  assert(th != NULL);
  lua_newtable(th);
  stack_tab = tabV(th->top - 1);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(th)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarkedmem(g, tvref(th->stack)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(stack_tab)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);
}

static void test_userdata(lua_State *L, global_State *g)
{
  GCtab *env, *mt;
  GCudata *ud;

  lua_newtable(L);
  env = tabV(L->top - 1);
  lua_newuserdata(L, 1);
  ud = udataV(L->top - 1);
  lua_pushvalue(L, -2);
  lua_setfenv(L, -2);
  lua_newtable(L);
  mt = tabV(L->top - 1);
  lua_setmetatable(L, -2);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(mt)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(ud)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  assert(lj_gc2_drain_ssb(g) == 0);
  assert(lj_gc2_ssb_empty(g));
  assert(lj_gc2_ismarked(g, obj2gco(env)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(mt)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 2);
}

static void test_leaf_ssb(lua_State *L, global_State *g, TGState *tg)
{
  GCstr *s;

  lua_pushliteral(L, "gc2 leaf ssb");
  s = strV(L->top - 1);
  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ssb_push(g, obj2gco(s)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(s)) == 1);
  lj_gc2_legacy_cycle_end(g);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);

  test_strong_table(L, g, tg);
  test_c_value_barrier(L, g, tg);
  test_c_table_rescan_barrier(L, g, tg);
  test_vm_upvalue_barrier(L, g, tg);
  test_weak_tables(L, g, tg);
  test_closure(L, g, tg);
  test_thread(L, g, tg);
  test_userdata(L, g);
  test_leaf_ssb(L, g, tg);

  lua_close(L);
  printf("t-gc2-traverse OK: SSB grey traversal verified\n");
  return 0;
}
